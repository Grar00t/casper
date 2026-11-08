/* GGUF decoder for LLaMA/Qwen3-style dense decoder-only models.
 * Read-only mmap tensors; one preallocated runtime pool (KV + scratch + logits).
 * Supported GGML tensors: F32, F16, Q4_0, Q8_0, Q4_K, Q5_K, Q6_K, IQ4_XS.
 * Rejects unsupported operations/quantizations instead of silent fallbacks.
 * Included from niyah_core.c to share its tested RMSNorm/RoPE helpers.
 */
#include "niyah_gguf_map.h"

#define GG_MAX_LAYERS 128u
#define GG_MAX_HEADS 128u
#define GG_MAX_EMBD 8192u
#define GG_MAX_FFN 65536u
#define GG_MAX_VOCAB 262144u
#define GG_MAX_CTX NIYAH_MAX_CTX

typedef struct {
    const uint8_t *bytes;
    uint64_t offset;
    uint64_t dims[2];
    uint32_t n_dims;
    uint32_t type;
    size_t row_bytes;
} GGWeight;

typedef struct {
    GGWeight q, k, v, o, gate, up, down, att_norm, ffn_norm, q_norm, k_norm;
    GGWeight post_att_norm, post_ff_norm;
} GGBlock;

struct NiyahGGUFRunner {
    NiyahGGUFMap file;
    GGBlock blocks[GG_MAX_LAYERS];
    GGWeight embed, output, final_norm;
    char arch[24];
    uint32_t n_layers, d, nh, nkv, hd, rope_dim, kd, qd, ff, vocab, ctx, pos;
    float theta, eps, final_softcap, local_theta;
    uint32_t sliding_window;
    size_t tensor_data_base, tensor_data_bytes;
    int gemma3;
    void *_pool;
    size_t _pool_bytes;
    float *cache_k, *cache_v;
    float *x, *norm, *q, *k, *v, *att, *acc, *gate, *up, *scores, *logits, *quant_act;
};

typedef struct {
    const uint8_t *ptr;
    uint64_t len, at;
} GGReader;

static int gg_take(GGReader *r, uint64_t len, const uint8_t **out) {
    if (r->at > r->len || len > r->len - r->at) return 0;
    if (out) *out = r->ptr + (size_t)r->at;
    r->at += len;
    return 1;
}
static uint32_t gg_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t gg_u64(const uint8_t *p) {
    return (uint64_t)gg_u32(p) | ((uint64_t)gg_u32(p + 4u) << 32);
}
static int gg_read32(GGReader *r, uint32_t *v) {
    const uint8_t *p;
    if (!gg_take(r, 4u, &p)) return 0;
    *v = gg_u32(p); return 1;
}
static int gg_read64(GGReader *r, uint64_t *v) {
    const uint8_t *p;
    if (!gg_take(r, 8u, &p)) return 0;
    *v = gg_u64(p); return 1;
}
static int gg_string(GGReader *r, char *buf, size_t cap) {
    uint64_t n;
    const uint8_t *p;
    if (!gg_read64(r, &n) || !gg_take(r, n, &p)) return 0;
    if (buf && cap) {
        size_t count = n < (uint64_t)(cap - 1u) ? (size_t)n : cap - 1u;
        memcpy(buf, p, count);
        buf[count] = '\0';
    }
    return 1;
}
static int gg_skip_value(GGReader *r, uint32_t type, uint32_t depth) {
    uint64_t n, i;
    uint32_t et;
    size_t bytes = 0u;
    if (depth > 3u) return 0;
    switch (type) {
    case 0: case 1: case 7: bytes = 1u; break;
    case 2: case 3: bytes = 2u; break;
    case 4: case 5: case 6: bytes = 4u; break;
    case 10: case 11: case 12: bytes = 8u; break;
    case 8: return gg_string(r, NULL, 0u);
    case 9:
        if (!gg_read32(r, &et) || !gg_read64(r, &n) ||
            n > r->len - r->at) return 0;
        for (i = 0u; i < n; ++i)
            if (!gg_skip_value(r, et, depth + 1u)) return 0;
        return 1;
    default: return 0;
    }
    return gg_take(r, (uint64_t)bytes, NULL);
}
static int gg_number(const GGReader *r, uint32_t type, double *num) {
    const uint8_t *p = r->ptr + (size_t)r->at;
    uint32_t bits32;
    uint64_t bits64;
    float fl;
    double db;
    size_t bytes = 0u;
    switch (type) {
    case 0: case 1: case 7: bytes = 1u; break;
    case 2: case 3: bytes = 2u; break;
    case 4: case 5: case 6: bytes = 4u; break;
    case 10: case 11: case 12: bytes = 8u; break;
    default: return 0;
    }
    if ((uint64_t)bytes > r->len - r->at) return 0;
    if (type == 6u) {
        bits32 = gg_u32(p);
        memcpy(&fl, &bits32, sizeof fl);
        *num = (double)fl;
    } else if (type == 12u) {
        bits64 = gg_u64(p);
        memcpy(&db, &bits64, sizeof db);
        *num = db;
    } else if (type == 4u || type == 5u) {
        *num = (double)gg_u32(p);
    } else if (type == 10u || type == 11u) {
        *num = (double)gg_u64(p);
    } else if (type == 2u || type == 3u) {
        *num = (double)((unsigned)p[0] | ((unsigned)p[1] << 8));
    } else {
        *num = (double)p[0];
    }
    return 1;
}
static int gg_key_arch(const char *key, const char *arch, const char *suffix) {
    size_t n = strlen(arch);
    return strncmp(key, arch, n) == 0 && key[n] == '.' &&
           strcmp(key + n + 1u, suffix) == 0;
}
static int gg_meta(NiyahGGUFRunner *m, GGReader *r, uint32_t *alignment,
                   uint32_t *meta_hd, uint32_t *meta_ff,
                   uint32_t *meta_ctx) {
    uint64_t i;
    for (i = 0; i < m->file.metadata_count; ++i) {
        char key[160];
        uint32_t type;
        double num = 0.0;
        if (!gg_string(r, key, sizeof(key)) || !gg_read32(r, &type)) return 0;
        if (strcmp(key, "general.architecture") == 0 && type == 8u) {
            if (!gg_string(r, m->arch, sizeof(m->arch))) return 0;
            continue;
        }
        if (strstr(key, ".rope.scaling.") != NULL) return 0;
        if (gg_number(r, type, &num) && isfinite(num) && num >= 0.0) {
            if (strcmp(key, "general.alignment") == 0 && num <= 4096.0) *alignment = (uint32_t)num;
            if (gg_key_arch(key, "llama", "block_count") || gg_key_arch(key, "qwen3", "block_count") ||
                gg_key_arch(key, "gemma3", "block_count"))
                m->n_layers = (uint32_t)num;
            if (gg_key_arch(key, "llama", "embedding_length") || gg_key_arch(key, "qwen3", "embedding_length") ||
                gg_key_arch(key, "gemma3", "embedding_length"))
                m->d = (uint32_t)num;
            if (gg_key_arch(key, "llama", "attention.head_count") || gg_key_arch(key, "qwen3", "attention.head_count") ||
                gg_key_arch(key, "gemma3", "attention.head_count"))
                m->nh = (uint32_t)num;
            if (gg_key_arch(key, "llama", "attention.head_count_kv") || gg_key_arch(key, "qwen3", "attention.head_count_kv") ||
                gg_key_arch(key, "gemma3", "attention.head_count_kv"))
                m->nkv = (uint32_t)num;
            if (gg_key_arch(key, "llama", "feed_forward_length") || gg_key_arch(key, "qwen3", "feed_forward_length") ||
                gg_key_arch(key, "gemma3", "feed_forward_length"))
                *meta_ff = (uint32_t)num;
            if (gg_key_arch(key, "llama", "context_length") || gg_key_arch(key, "qwen3", "context_length") ||
                gg_key_arch(key, "gemma3", "context_length"))
                *meta_ctx = (uint32_t)num;
            if (gg_key_arch(key, "llama", "attention.key_length") || gg_key_arch(key, "qwen3", "attention.key_length") ||
                gg_key_arch(key, "gemma3", "attention.key_length"))
                *meta_hd = (uint32_t)num;
            if (gg_key_arch(key, "llama", "rope.dimension_count") || gg_key_arch(key, "qwen3", "rope.dimension_count") ||
                gg_key_arch(key, "gemma3", "rope.dimension_count"))
                *meta_hd = (uint32_t)num;
            if (gg_key_arch(key, "llama", "rope.freq_base") || gg_key_arch(key, "qwen3", "rope.freq_base") ||
                gg_key_arch(key, "gemma3", "rope.freq_base"))
                m->theta = (float)num;
            if (gg_key_arch(key, "llama", "attention.layer_norm_rms_epsilon") ||
                gg_key_arch(key, "qwen3", "attention.layer_norm_rms_epsilon") ||
                gg_key_arch(key, "gemma3", "attention.layer_norm_rms_epsilon"))
                m->eps = (float)num;
            if (gg_key_arch(key, "gemma3", "attention.sliding_window") && num <= GG_MAX_CTX)
                m->sliding_window = (uint32_t)num;
            if (gg_key_arch(key, "gemma3", "final_logit_softcapping"))
                m->final_softcap = (float)num;
            if (gg_key_arch(key, "gemma3", "rope.freq_base_swa"))
                m->local_theta = (float)num;
        }
        if (!gg_skip_value(r, type, 0u)) return 0;
    }
    return 1;
}
static GGWeight *gg_slot(NiyahGGUFRunner *m, const char *name) {
    const char *suffix;
    unsigned long i;
    char *end;
    GGBlock *b;
    if (strcmp(name, "token_embd.weight") == 0) return &m->embed;
    if (strcmp(name, "output.weight") == 0) return &m->output;
    if (strcmp(name, "output_norm.weight") == 0) return &m->final_norm;
    if (strncmp(name, "blk.", 4u) != 0) return NULL;
    i = strtoul(name + 4u, &end, 10);
    if (end == name + 4u || *end != '.' || i >= m->n_layers) return NULL;
    suffix = end + 1u;
    b = &m->blocks[i];
    if (!strcmp(suffix, "attn_q.weight")) return &b->q;
    if (!strcmp(suffix, "attn_k.weight")) return &b->k;
    if (!strcmp(suffix, "attn_v.weight")) return &b->v;
    if (!strcmp(suffix, "attn_output.weight")) return &b->o;
    if (!strcmp(suffix, "ffn_gate.weight")) return &b->gate;
    if (!strcmp(suffix, "ffn_up.weight")) return &b->up;
    if (!strcmp(suffix, "ffn_down.weight")) return &b->down;
    if (!strcmp(suffix, "attn_norm.weight")) return &b->att_norm;
    if (!strcmp(suffix, "ffn_norm.weight")) return &b->ffn_norm;
    if (!strcmp(suffix, "attn_q_norm.weight")) return &b->q_norm;
    if (!strcmp(suffix, "attn_k_norm.weight")) return &b->k_norm;
    if (!strcmp(suffix, "post_attention_norm.weight")) return &b->post_att_norm;
    if (!strcmp(suffix, "post_ffw_norm.weight")) return &b->post_ff_norm;
    return NULL;
}
static int gg_type_layout(uint32_t type, size_t *block, size_t *bytes) {
    switch (type) {
    case 0u: *block = 1u; *bytes = 4u; return 1;
    case 1u: *block = 1u; *bytes = 2u; return 1;
    case 2u: *block = 32u; *bytes = 18u; return 1;
    case 6u: *block = 32u; *bytes = 22u; return 1; /* Q5_0 */
    case 3u: *block = 32u; *bytes = 20u; return 1;
    case 8u: *block = 32u; *bytes = 34u; return 1;
    case 12u: *block = 256u; *bytes = 144u; return 1;
    case 13u: *block = 256u; *bytes = 176u; return 1;
    case 14u: *block = 256u; *bytes = 210u; return 1;
    case 23u: *block = 256u; *bytes = 136u; return 1; /* IQ4_XS */
    default: return 0;
    }
}
static int gg_validate_weight(NiyahGGUFRunner *m, GGWeight *w, uint64_t base) {
    size_t block, bytes, row, total;
    const uint8_t *p;
    if (!w->n_dims || !gg_type_layout(w->type, &block, &bytes) ||
        w->dims[0] == 0u || w->dims[0] > SIZE_MAX / bytes ||
        w->dims[0] % block != 0u ||
        w->dims[1] == 0u || w->dims[1] > SIZE_MAX) return 0;
    row = (size_t)(w->dims[0] / block) * bytes;
    if (row == 0u || (size_t)w->dims[1] > SIZE_MAX / row) return 0;
    total = row * (size_t)w->dims[1];
    if (w->offset > UINT64_MAX - base) return 0;
    p = niyah_gguf_map_range(&m->file, base + w->offset, (uint64_t)total);
    if (!p) return 0;
    w->bytes = p;
    w->row_bytes = row;
    return 1;
}
static int gg_shape(const GGWeight *w, uint32_t cols, uint32_t rows) {
    return w->bytes && w->dims[0] == (uint64_t)cols &&
           w->dims[1] == (uint64_t)rows &&
           w->n_dims == (rows == 1u ? 1u : 2u);
}
static float gg_f16(uint16_t h) {
    uint32_t sign = ((uint32_t)h & 0x8000u) << 16;
    uint32_t exp = ((uint32_t)h >> 10) & 31u;
    uint32_t mant = (uint32_t)h & 1023u;
    uint32_t bits;
    float out;
    if (exp == 0u) {
        if (!mant) bits = sign;
        else {
            uint32_t shift = 0u;
            while ((mant & 1024u) == 0u) { mant <<= 1; ++shift; }
            mant &= 1023u;
            bits = sign | ((113u - shift) << 23) | (mant << 13);
        }
    } else if (exp == 31u) bits = sign | 0x7f800000u | (mant << 13);
    else bits = sign | ((exp + 112u) << 23) | (mant << 13);
    memcpy(&out, &bits, sizeof(out));
    return out;
}
static void gg_scale_k4(const uint8_t scales[12], size_t group,
                        int *scale, int *minimum) {
    if (group < 4u) {
        *scale = scales[group] & 63;
        *minimum = scales[group + 4u] & 63;
    } else {
        *scale = (scales[group + 4u] & 15) | ((scales[group - 4u] >> 6) << 4);
        *minimum = (scales[group + 4u] >> 4) | ((scales[group] >> 6) << 4);
    }
}
static float gg_value(const GGWeight *w, size_t row, size_t col) {
    const uint8_t *p = w->bytes + row * w->row_bytes;
    uint32_t bits;
    float value;
    size_t k, local;
    if (w->type == 0u) {
        bits = gg_u32(p + col * 4u);
        memcpy(&value, &bits, sizeof value);
        return value;
    }
    if (w->type == 1u)
        return gg_f16((uint16_t)((uint16_t)p[2u * col] | ((uint16_t)p[2u * col + 1u] << 8)));
    if (w->type == 2u) {
        k = col / 32u; local = col % 32u; p += k * 18u;
        return gg_f16((uint16_t)(p[0] | ((uint16_t)p[1] << 8))) *
               (float)((int)((local < 16u ? p[2u + local] : p[2u + local - 16u] >> 4) &
               (local < 16u ? 15u : 15u)) - 8);
    }
    /* GGML Q4_1: 16 packed bytes, each nibble dequantized as d*q+m. */
    if (w->type == 3u) {
        k = col / 32u; local = col % 32u; p += k * 20u;
        {
            float d = gg_f16((uint16_t)(p[0] | ((uint16_t)p[1] << 8)));
            float m = gg_f16((uint16_t)(p[2] | ((uint16_t)p[3] << 8)));
            unsigned q = local < 16u ? (p[4u + local] & 15u) :
                                        (p[4u + local - 16u] >> 4);
            return d * (float)q + m;
        }
    }
    if (w->type == 6u) { /* Q5_0: 16 low nibbles, 32 high bits. */
        uint32_t upper, q;
        k = col / 32u; local = col % 32u; p += k * 22u;
        upper = gg_u32(p + 2u);
        q = ((uint32_t)p[6u + local % 16u] >> (local >= 16u ? 4u : 0u)) & 15u;
        q |= ((upper >> local) & 1u) << 4u;
        return gg_f16((uint16_t)(p[0] | ((uint16_t)p[1] << 8))) *
               (float)((int)q - 16);
    }
    if (w->type == 8u) {
        k = col / 32u; local = col % 32u; p += k * 34u;
        return gg_f16((uint16_t)(p[0] | ((uint16_t)p[1] << 8))) *
               (float)(int8_t)p[2u + local];
    }
    k = col / 256u; local = col % 256u;
    if (w->type == 23u) { /* IQ4_XS, GGML QK_K=256 */
        static const int8_t values[16] = {
            -127, -104, -83, -65, -49, -35, -22, -10,
            1, 13, 25, 38, 53, 69, 89, 113
        };
        size_t group = local / 32u, j = local % 16u;
        uint16_t high;
        int ls, index;
        p += k * 136u;
        high = (uint16_t)(p[2] | ((uint16_t)p[3] << 8));
        ls = (int)((p[4u + group / 2u] >> (4u * (group & 1u))) & 15u) |
             (int)(((high >> (2u * group)) & 3u) << 4);
        index = (int)((local & 16u) ? (p[8u + group * 16u + j] >> 4) :
                      (p[8u + group * 16u + j] & 15u));
        return gg_f16((uint16_t)(p[0] | ((uint16_t)p[1] << 8))) *
               (float)(ls - 32) * (float)values[index];
    }
    if (w->type == 12u || w->type == 13u) {
        const uint8_t *scales, *qs, *qh = NULL;
        float d, dmin;
        size_t group = local / 32u, idx = (local / 64u) * 32u + local % 32u;
        int sc, mn, q;
        p += k * (w->type == 12u ? 144u : 176u);
        d = gg_f16((uint16_t)(p[0] | ((uint16_t)p[1] << 8)));
        dmin = gg_f16((uint16_t)(p[2] | ((uint16_t)p[3] << 8)));
        scales = p + 4u;
        if (w->type == 13u) { qh = p + 16u; qs = p + 48u; }
        else qs = p + 16u;
        gg_scale_k4(scales, group, &sc, &mn);
        q = (int)((group & 1u) ? (qs[idx] >> 4) : (qs[idx] & 15u));
        if (qh) q += (int)(((qh[local % 32u] >> group) & 1u) << 4);
        return d * (float)sc * (float)q - dmin * (float)mn;
    }
    /* Q6_K: two groups of 128, 4 interleaved subgroups of 32 each. */
    if (w->type == 14u) {
        const uint8_t *ql, *qh;
        const int8_t *scales;
        size_t half = local / 128u, within = local % 128u;
        size_t sub = within / 32u, j = within % 32u;
        int q;
        float d;
        p += k * 210u;
        ql = p; qh = p + 128u; scales = (const int8_t *)(p + 192u);
        d = gg_f16((uint16_t)(p[208] | ((uint16_t)p[209] << 8)));
        if (sub == 0u) q = (ql[64u * half + j] & 15u) |
                           ((qh[32u * half + j] & 3u) << 4);
        else if (sub == 1u) q = (ql[64u * half + 32u + j] & 15u) |
                                (((qh[32u * half + j] >> 2) & 3u) << 4);
        else if (sub == 2u) q = (ql[64u * half + j] >> 4) |
                                (((qh[32u * half + j] >> 4) & 3u) << 4);
        else q = (ql[64u * half + 32u + j] >> 4) |
                 (((qh[32u * half + j] >> 6) & 3u) << 4);
        return d * (float)scales[8u * half + 2u * sub + j / 16u] * (float)(q - 32);
    }
    return NAN;
}
/* Qwen3 uses split-half (NeoX) rotary pairs; LLaMA uses adjacent pairs. */
static void gg_rope_neox(float *v, uint32_t pos, uint32_t dim, float theta) {
    for (uint32_t i = 0u; i < dim / 2u; ++i) {
        float angle = (float)pos / powf(theta, (float)(2u * i) / (float)dim);
        float cs = cosf(angle), sn = sinf(angle);
        float a = v[i], b = v[i + dim / 2u];
        v[i] = a * cs - b * sn;
        v[i + dim / 2u] = a * sn + b * cs;
    }
}
/* CPU GGML quantized matmul first quantizes the input activations.
 * Q4_0/Q4_1/Q5_0/Q8_0 use Q8_0 activations (FP16 scale, 32 values),
 * K-quants and IQ4_XS use Q8_K (FP32 scale, 256 values).
 * Staging is caller-owned _pool storage; no heap allocation in hot loops.
 */
static uint16_t gg_to_f16(float x) {
    uint32_t u, sign, mant, rounded;
    int exp;
    memcpy(&u, &x, sizeof u);
    sign = (u >> 16u) & 0x8000u;
    mant = u & 0x007fffffu;
    exp = (int)((u >> 23u) & 255u) - 112;
    if (exp >= 31) return (uint16_t)(sign | 0x7c00u);
    if (exp <= 0) {
        uint32_t shift;
        if (exp < -10) return (uint16_t)sign;
        mant |= 0x00800000u;
        shift = (uint32_t)(14 - exp);
        rounded = mant + ((UINT32_C(1) << (shift - 1u)) - 1u) +
                  ((mant >> shift) & 1u);
        return (uint16_t)(sign | (rounded >> shift));
    }
    rounded = mant + 0x00000fffu + ((mant >> 13u) & 1u);
    if (rounded & 0x00800000u) { rounded = 0u; ++exp; }
    if (exp >= 31) return (uint16_t)(sign | 0x7c00u);
    return (uint16_t)(sign | (uint32_t)(exp << 10u) | (rounded >> 13u));
}
static float gg_round_to_even(float x) {
    int whole = (int)x;
    const float fraction = fabsf(x - (float)whole);
    if (fraction > 0.5f || (fraction == 0.5f && (whole & 1) != 0))
        whole += x < 0.0f ? -1 : 1;
    return (float)whole;
}
static void gg_quant_act(float *out, const float *in, size_t n, uint32_t weight_type) {
    const size_t width = (weight_type == 2u || weight_type == 3u ||
                          weight_type == 6u || weight_type == 8u) ? 32u : 256u;
    for (size_t base = 0u; base < n; base += width) {
        float max_abs = 0.0f, max_value = 0.0f, scale, inv;
        for (size_t j = 0u; j < width; ++j) {
            const float a = fabsf(in[base + j]);
            if (a > max_abs) { max_abs = a; max_value = in[base + j]; }
        }
        if (width == 32u) {
            scale = gg_f16(gg_to_f16(max_abs / 127.0f));
            inv = max_abs > 0.0f ? 127.0f / max_abs : 0.0f;
        } else {
            inv = max_abs > 0.0f ? -127.0f / max_value : 0.0f;
            scale = inv != 0.0f ? 1.0f / inv : 0.0f;
        }
        out[n + base / width] = scale;
        for (size_t j = 0u; j < width; ++j) {
            const float scaled = in[base + j] * inv;
            float q = gg_round_to_even(scaled);
            if (q > 127.0f) q = 127.0f;
            if (q < -127.0f) q = -127.0f;
            out[base + j] = scale * q;
        }
    }
}
static void gg_mv(float *out, const GGWeight *w, const float *x, float *scratch) {
    size_t rows = (size_t)w->dims[1], cols = (size_t)w->dims[0];
    if (w->type != 0u && w->type != 1u) {
        gg_quant_act(scratch, x, cols, w->type);
        x = scratch;
    }
    if (w->type == 8u) {
        for (size_t i = 0u; i < rows; ++i) {
            const uint8_t *wr = w->bytes + i * w->row_bytes;
            float lanes[8] = {0.0f, 0.0f, 0.0f, 0.0f,
                              0.0f, 0.0f, 0.0f, 0.0f};
            for (size_t b = 0u; b < cols / 32u; ++b) {
                const uint8_t *p = wr + b * 34u;
                const float act_scale = scratch[cols + b];
                const float scale = gg_f16((uint16_t)(p[0] | ((uint16_t)p[1] << 8))) * act_scale;
                int partial[8] = {0, 0, 0, 0, 0, 0, 0, 0};
                for (size_t j = 0u; j < 32u; ++j) {
                    const int q = act_scale != 0.0f
                                      ? (int)roundf(x[b * 32u + j] / act_scale)
                                      : 0;
                    partial[j / 4u] += (int)(int8_t)p[2u + j] * q;
                }
                for (size_t lane = 0u; lane < 8u; ++lane)
                    lanes[lane] = fmaf(scale, (float)partial[lane], lanes[lane]);
            }
            {
                const float a0 = lanes[0] + lanes[4];
                const float a1 = lanes[1] + lanes[5];
                const float a2 = lanes[2] + lanes[6];
                const float a3 = lanes[3] + lanes[7];
                out[i] = (a0 + a2) + (a1 + a3);
            }
        }
        return;
    }
    if (w->type == 6u || w->type == 12u || w->type == 14u) {
        const size_t block = (w->type == 6u) ? 32u : 256u;
        const size_t block_bytes = (w->type == 6u) ? 22u : (w->type == 12u ? 144u : 210u);
        for (size_t i = 0u; i < rows; ++i) {
            const uint8_t *wr = w->bytes + i * w->row_bytes;
            float sum = 0.0f;
            float sum_min = 0.0f;
            float lanes[8] = {0.0f, 0.0f, 0.0f, 0.0f,
                              0.0f, 0.0f, 0.0f, 0.0f};
            for (size_t b = 0u; b < cols / block; ++b) {
                const uint8_t *p = wr + b * block_bytes;
                const float sx = scratch[cols + b];
                if (sx == 0.0f) continue;
                if (w->type == 6u) {
                    int partial[8] = {0, 0, 0, 0, 0, 0, 0, 0};
                    const uint32_t upper = gg_u32(p + 2u);
                    for (size_t j = 0u; j < 32u; ++j) {
                        unsigned q = ((unsigned)p[6u + j % 16u] >>
                                      (j >= 16u ? 4u : 0u)) & 15u;
                        q |= ((upper >> j) & 1u) << 4u;
                        partial[j / 4u] +=
                            ((int)q - 16) * (int)roundf(x[b * block + j] / sx);
                    }
                    const float dw = gg_f16((uint16_t)(p[0] | ((uint16_t)p[1] << 8)));
                    const float scale = dw * sx;
                    for (size_t lane = 0u; lane < 8u; ++lane)
                        lanes[lane] = fmaf(scale, (float)partial[lane], lanes[lane]);
                } else if (w->type == 12u) {
                    int8_t q8[256];
                    int32_t scaled = 0, mins = 0;
                    int32_t sums32[8] = {0, 0, 0, 0, 0, 0, 0, 0};
                    for (size_t g = 0u; g < 8u; ++g) {
                        int scale, minimum, dot = 0;
                        gg_scale_k4(p + 4u, g, &scale, &minimum);
                        for (size_t j = 0u; j < 32u; ++j) {
                            const size_t local = g * 32u + j;
                            q8[local] = (int8_t)roundf(x[b * block + local] / sx);
                            sums32[g] += q8[local];
                            const size_t idx = (local / 64u) * 32u + local % 32u;
                            const int q = (int)((g & 1u) ? (p[16u + idx] >> 4u)
                                                           : (p[16u + idx] & 15u));
                            dot += q * q8[local];
                        }
                        scaled += scale * dot;
                        mins += minimum * sums32[g];
                    }
                    {
                        const float dw = gg_f16((uint16_t)(p[0] | ((uint16_t)p[1] << 8)));
                        const float dmin = gg_f16((uint16_t)(p[2] | ((uint16_t)p[3] << 8)));
                        const float d = dw * sx, dm = dmin * sx;
                        sum = fmaf((float)scaled, d, sum);
                        sum_min = fmaf((float)mins, dm, sum_min);
                    }
                } else {
                    int8_t q8[256];
                    int32_t partial[8] = {0, 0, 0, 0, 0, 0, 0, 0};
                    int32_t sums16[16] = {0};
                    const int8_t *scales = (const int8_t *)(p + 192u);
                    const float d = gg_f16((uint16_t)(p[208] | ((uint16_t)p[209] << 8)));
                    for (size_t pair = 0u; pair < 8u; ++pair) {
                        for (size_t g = pair * 2u; g < pair * 2u + 2u; ++g) {
                            for (size_t j = 0u; j < 16u; ++j) {
                                const size_t local = g * 16u + j;
                                q8[local] = (int8_t)roundf(x[b * block + local] / sx);
                                sums16[g] += q8[local];
                            }
                        }
                        for (size_t lane = 0u; lane < 8u; ++lane) {
                            const size_t g = pair * 2u + lane / 4u;
                            for (size_t j = 0u; j < 4u; ++j) {
                                const size_t local = g * 16u + (lane % 4u) * 4u + j;
                                const size_t half = local / 128u;
                                const size_t sub = (local % 128u) / 32u;
                                const size_t k = local % 32u;
                                const size_t lo = half * 64u, hi = half * 32u;
                                unsigned q;
                                if (sub == 0u) q = (p[lo + k] & 15u) |
                                    ((p[128u + hi + k] & 3u) << 4u);
                                else if (sub == 1u) q = (p[lo + 32u + k] & 15u) |
                                    (((p[128u + hi + k] >> 2u) & 3u) << 4u);
                                else if (sub == 2u) q = (p[lo + k] >> 4u) |
                                    (((p[128u + hi + k] >> 4u) & 3u) << 4u);
                                else q = (p[lo + 32u + k] >> 4u) |
                                    (((p[128u + hi + k] >> 6u) & 3u) << 4u);
                                partial[lane] += (int)scales[g] * (int)q * q8[local];
                            }
                        }
                    }
                    for (size_t lane = 0u; lane < 8u; ++lane)
                        partial[lane] -= 32 * ((int)scales[2u * lane] * sums16[2u * lane] +
                                               (int)scales[2u * lane + 1u] * sums16[2u * lane + 1u]);
                    {
                        const float scale = sx * d;
                        for (size_t lane = 0u; lane < 8u; ++lane)
                            lanes[lane] = fmaf(scale, (float)partial[lane], lanes[lane]);
                    }
                }
            }
            if (w->type == 6u || w->type == 14u) {
                const float a0 = lanes[0] + lanes[4];
                const float a1 = lanes[1] + lanes[5];
                const float a2 = lanes[2] + lanes[6];
                const float a3 = lanes[3] + lanes[7];
                out[i] = (a0 + a2) + (a1 + a3);
            } else if (w->type == 12u) out[i] = sum - sum_min;
            else out[i] = sum;
        }
        return;
    }
    for (size_t i = 0u; i < rows; ++i) {
        double acc = 0.0;
        for (size_t j = 0u; j < cols; ++j)
            acc += (double)gg_value(w, i, j) * (double)x[j];
        out[i] = (float)acc;
    }
}
static float gg_gelu(float x) {
    if (x <= -10.0f) return 0.0f;
    if (x >= 10.0f) return x;
    x = gg_f16(gg_to_f16(x));
    {
        float y = 0.5f * x * (1.0f + tanhf(0.7978845608028654f * x *
                                          (1.0f + 0.044715f * x * x)));
        return gg_f16(gg_to_f16(y));
    }
}
static void gg_norm(float *out, const float *x, const GGWeight *w,
                    uint32_t n, float eps) {
    double ss = 0.0;
    for (uint32_t i = 0u; i < n; ++i) ss += (double)(x[i] * x[i]);
    {
        float scale = 1.0f / sqrtf((float)(ss / n) + eps);
        for (uint32_t i = 0u; i < n; ++i)
            out[i] = x[i] * scale * gg_value(w, 0u, i);
    }
}
static int gg_parse_tensors(NiyahGGUFRunner *m, GGReader *r, uint32_t alignment) {
    uint64_t i, base;
    for (i = 0; i < m->file.tensor_count; ++i) {
        char name[160];
        uint32_t nd, type;
        uint64_t dims[2] = {1u, 1u}, offset;
        GGWeight *slot;
        if (!gg_string(r, name, sizeof(name)) || !gg_read32(r, &nd) ||
            nd == 0u || nd > 4u) return 0;
        for (uint32_t j = 0u; j < nd; ++j) {
            uint64_t d;
            if (!gg_read64(r, &d) || d == 0u) return 0;
            if (j < 2u) dims[j] = d;
            else if (d != 1u) return 0;
        }
        if (!gg_read32(r, &type) || !gg_read64(r, &offset)) return 0;
        if (strstr(name, ".bias") != NULL) return 0;
        slot = gg_slot(m, name);
        if (slot) {
            if (slot->n_dims || nd > 2u) return 0;
            slot->n_dims = nd;
            slot->dims[0] = dims[0]; slot->dims[1] = dims[1];
            slot->type = type; slot->offset = offset;
        }
    }
    if (!alignment || (alignment & (alignment - 1u)) != 0u) return 0;
    if (r->at > UINT64_MAX - (uint64_t)(alignment - 1u)) return 0;
    base = (r->at + (uint64_t)(alignment - 1u)) & ~(uint64_t)(alignment - 1u);
    if (base > m->file.file_size) return 0;
    /* The original mapping is bounded to SIZE_MAX by niyah_gguf_map_open. */
    m->tensor_data_base = (size_t)base;
    m->tensor_data_bytes = (size_t)(m->file.file_size - base);
    if (!gg_validate_weight(m, &m->embed, base) ||
        !gg_validate_weight(m, &m->final_norm, base)) return 0;
    if (m->output.n_dims) {
        if (!gg_validate_weight(m, &m->output, base)) return 0;
    } else m->output = m->embed;
    if (!gg_shape(&m->embed, m->d, m->vocab) ||
        !gg_shape(&m->output, m->d, m->vocab) ||
        !gg_shape(&m->final_norm, m->d, 1u)) return 0;
    for (uint32_t l = 0u; l < m->n_layers; ++l) {
        GGBlock *b = &m->blocks[l];
        GGWeight *weights[] = {&b->q,&b->k,&b->v,&b->o,&b->gate,&b->up,
                               &b->down,&b->att_norm,&b->ffn_norm};
        for (size_t j = 0u; j < sizeof(weights) / sizeof(weights[0]); ++j)
            if (!gg_validate_weight(m, weights[j], base)) return 0;
        if (!gg_shape(&b->q, m->d, m->qd) ||
            !gg_shape(&b->k, m->d, m->kd) ||
            !gg_shape(&b->v, m->d, m->kd) ||
            !gg_shape(&b->o, m->qd, m->d) ||
            !gg_shape(&b->gate, m->d, m->ff) ||
            !gg_shape(&b->up, m->d, m->ff) ||
            !gg_shape(&b->down, m->ff, m->d) ||
            !gg_shape(&b->att_norm, m->d, 1u) ||
            !gg_shape(&b->ffn_norm, m->d, 1u)) return 0;
        if (b->q_norm.n_dims || b->k_norm.n_dims) {
            if (!b->q_norm.n_dims || !b->k_norm.n_dims ||
                !gg_validate_weight(m, &b->q_norm, base) ||
                !gg_validate_weight(m, &b->k_norm, base) ||
                !gg_shape(&b->q_norm, m->hd, 1u) ||
                !gg_shape(&b->k_norm, m->hd, 1u)) return 0;
        } else if (strcmp(m->arch, "qwen3") == 0 || m->gemma3) return 0;
        if (m->gemma3) {
            if (!gg_validate_weight(m, &b->post_att_norm, base) ||
                !gg_validate_weight(m, &b->post_ff_norm, base) ||
                !gg_shape(&b->post_att_norm, m->d, 1u) ||
                !gg_shape(&b->post_ff_norm, m->d, 1u)) return 0;
        } else if (b->post_att_norm.n_dims || b->post_ff_norm.n_dims) return 0;
    }
    return 1;
}
static int gg_add(size_t *n, size_t amount) {
    if (amount > SIZE_MAX - *n) return 0;
    *n += amount; return 1;
}
/* Rebase every validated tensor from the temporary read-only mapping to _pool. */
static void gg_rebase_one(GGWeight *w, const uint8_t *old_base, uint8_t *new_base) {
    if (w->bytes) w->bytes = new_base + (size_t)(w->bytes - old_base);
}
static void gg_rebase_all(NiyahGGUFRunner *m, const uint8_t *old_base,
                          uint8_t *new_base) {
    GGWeight *globals[] = {&m->embed, &m->output, &m->final_norm};
    for (size_t j = 0u; j < sizeof(globals) / sizeof(globals[0]); ++j)
        gg_rebase_one(globals[j], old_base, new_base);
    for (uint32_t i = 0u; i < m->n_layers; ++i) {
        GGBlock *b = &m->blocks[i];
        GGWeight *fields[] = {&b->q, &b->k, &b->v, &b->o,
            &b->gate, &b->up, &b->down, &b->att_norm, &b->ffn_norm,
            &b->q_norm, &b->k_norm, &b->post_att_norm, &b->post_ff_norm};
        for (size_t j = 0u; j < sizeof(fields) / sizeof(fields[0]); ++j)
            gg_rebase_one(fields[j], old_base, new_base);
    }
}
static int gg_allocate_runtime(NiyahGGUFRunner *m) {
    size_t kv_per_layer, count = 0u, runtime, runtime_offset, pool_bytes, act_cap;
    const uint8_t *old_base;
    uint8_t *pool;
    float *p, *runtime_begin;
    if ((size_t)m->nkv > SIZE_MAX / m->ctx ||
        (size_t)m->nkv * m->ctx > SIZE_MAX / m->hd) return 0;
    kv_per_layer = (size_t)m->nkv * m->ctx * m->hd;
    if (kv_per_layer > SIZE_MAX / m->n_layers) return 0;
    runtime = kv_per_layer * m->n_layers;
    act_cap = m->d;
    if (act_cap < m->qd) act_cap = m->qd;
    if (act_cap < m->ff) act_cap = m->ff;
    if (runtime > SIZE_MAX / 2u || !gg_add(&count, 2u * runtime) ||
        !gg_add(&count, 3u * m->d) ||
        !gg_add(&count, m->qd) ||
        !gg_add(&count, 2u * m->kd) ||
        !gg_add(&count, m->qd) ||
        !gg_add(&count, 2u * m->ff) ||
        !gg_add(&count, m->ctx) ||
        !gg_add(&count, act_cap) ||
        !gg_add(&count, act_cap / 32u + 1u) ||
        !gg_add(&count, m->vocab) ||
        count > SIZE_MAX / sizeof(float)) return 0;
    /* One contiguous allocation contains original GGUF bytes, KV cache,
       scratch vectors and logits. Mapping exists only while loading. */
    if (!m->file.bytes || !m->tensor_data_bytes) return 0;
    runtime_offset = m->tensor_data_bytes;
    if (runtime_offset % _Alignof(float) != 0u &&
        !gg_add(&runtime_offset, _Alignof(float) - runtime_offset % _Alignof(float))) return 0;
    pool_bytes = runtime_offset;
    if (count > (SIZE_MAX - pool_bytes) / sizeof(float)) return 0;
    pool_bytes += count * sizeof(float);
    pool = (uint8_t *)malloc(pool_bytes);
    if (!pool) return 0;
    m->_pool = pool;
    m->_pool_bytes = pool_bytes;
    old_base = m->file.bytes + m->tensor_data_base;
    memcpy(pool, old_base, m->tensor_data_bytes);
    memset(pool + runtime_offset, 0, count * sizeof(float));
    runtime_begin = (float *)(void *)(pool + runtime_offset);
    p = runtime_begin;
    m->cache_k = p; p += runtime;
    m->cache_v = p; p += runtime;
    m->x = p; p += m->d;
    m->norm = p; p += m->d;
    m->acc = p; p += m->d;
    m->q = p; p += m->qd;
    m->k = p; p += m->kd;
    m->v = p; p += m->kd;
    m->att = p; p += m->qd;
    m->gate = p; p += m->ff;
    m->up = p; p += m->ff;
    m->scores = p; p += m->ctx;
    m->quant_act = p; p += act_cap + act_cap / 32u + 1u;
    m->logits = p; p += m->vocab;
    assert((size_t)(p - runtime_begin) == count);
    gg_rebase_all(m, old_base, pool);
    niyah_gguf_map_close(&m->file);
    m->pos = UINT32_MAX;
    return 1;
}
void niyah_gguf_runner_close(NiyahGGUFRunner *m) {
    if (!m) return;
    free(m->_pool);
    niyah_gguf_map_close(&m->file);
    free(m);
}
int niyah_gguf_runner_open(NiyahGGUFRunner **out, const char *path,
                           uint32_t ctx_override) {
    NiyahGGUFRunner *m;
    GGReader r;
    uint32_t alignment = 32u, meta_hd = 0u, meta_ff = 0u, meta_ctx = 0u;
    uint32_t ctx;
    if (!out || !path) return -1;
    *out = NULL;
    m = (NiyahGGUFRunner *)calloc(1u, sizeof(*m));
    if (!m) return -2;
    m->theta = 10000.0f; m->local_theta = 10000.0f; m->eps = 1e-5f;
    if (niyah_gguf_map_open(&m->file, path) != 0) goto error;
    r.ptr = m->file.bytes; r.len = m->file.file_size; r.at = 24u;
    if (!gg_meta(m, &r, &alignment, &meta_hd, &meta_ff, &meta_ctx)) goto error;
    if (strcmp(m->arch, "llama") != 0 && strcmp(m->arch, "qwen3") != 0 &&
        strcmp(m->arch, "gemma3") != 0) goto error;
    m->gemma3 = strcmp(m->arch, "gemma3") == 0;
    if (!m->n_layers || m->n_layers > GG_MAX_LAYERS ||
        !m->d || m->d > GG_MAX_EMBD ||
        !m->nh || m->nh > GG_MAX_HEADS ||
        !m->nkv || m->nkv > m->nh ||
        m->nh % m->nkv != 0u ||
        !meta_ff || meta_ff > GG_MAX_FFN ||
        meta_hd > 256u || (meta_hd & 1u) ||
        !meta_ctx || !isfinite(m->theta) || m->theta <= 1.0f ||
        !isfinite(m->eps) || m->eps <= 0.0f) goto error;
    /* The GGUF metadata's rope.dimension_count is the rotary width.
     * Key/query projection width is read from tensor shapes below. */
    m->ff = meta_ff;
    if (!m->blocks[0].q.n_dims) {
        /* Projection dimensions will be inferred from the first tensor pass. */
    }
    {
        GGReader scan = r;
        uint64_t i;
        for (i = 0u; i < m->file.tensor_count; ++i) {
            char name[160];
            uint32_t nd, type;
            uint64_t dims[4] = {1u,1u,1u,1u}, offset;
            if (!gg_string(&scan, name, sizeof name) || !gg_read32(&scan, &nd) ||
                nd == 0u || nd > 4u) goto error;
            for (uint32_t j = 0u; j < nd; ++j)
                if (!gg_read64(&scan, &dims[j])) goto error;
            if (!gg_read32(&scan, &type) || !gg_read64(&scan, &offset)) goto error;
            (void)type; (void)offset;
            if (!strcmp(name, "token_embd.weight") && nd == 2u) {
                if (dims[0] != m->d || dims[1] > GG_MAX_VOCAB) goto error;
                m->vocab = (uint32_t)dims[1];
            }
            if (!strcmp(name, "blk.0.attn_q.weight") && nd == 2u) {
                if (dims[0] != m->d || dims[1] > UINT32_MAX ||
                    dims[1] % m->nh) goto error;
                m->hd = (uint32_t)(dims[1] / m->nh);
            }
        }
    }
    if (!m->vocab || !m->hd || m->hd > 256u || (m->hd & 1u) ||
        meta_hd > m->hd ||
        m->nh > UINT32_MAX / m->hd ||
        m->nkv > UINT32_MAX / m->hd) goto error;
    m->rope_dim = meta_hd ? meta_hd : m->hd;
    m->qd = m->nh * m->hd; m->kd = m->nkv * m->hd;
    ctx = ctx_override ? ctx_override : (meta_ctx < 512u ? meta_ctx : 512u);
    if (!ctx || ctx > meta_ctx || ctx > GG_MAX_CTX) goto error;
    m->ctx = ctx;
    if (!gg_parse_tensors(m, &r, alignment)) goto error;
    if (!gg_allocate_runtime(m)) goto error;
    *out = m;
    return 0;
error:
    niyah_gguf_runner_close(m);
    return -3;
}
uint32_t niyah_gguf_runner_vocab_size(const NiyahGGUFRunner *m) {
    return m ? m->vocab : 0u;
}
void niyah_gguf_runner_reset(NiyahGGUFRunner *m) {
    size_t elems;
    if (!m || !m->_pool) return;
    elems = (size_t)m->n_layers * m->nkv * m->ctx * m->hd;
    memset(m->cache_k, 0, 2u * elems * sizeof(float));
    m->pos = UINT32_MAX;
}
float *niyah_gguf_runner_step(NiyahGGUFRunner *m, uint32_t token, uint32_t pos) {
    if (!m || !m->_pool || token >= m->vocab || pos >= m->ctx ||
        (pos == 0u ? m->pos != UINT32_MAX : m->pos != pos - 1u))
        return NULL;
    for (uint32_t i = 0u; i < m->d; ++i)
        m->x[i] = gg_value(&m->embed, token, i) *
                  (m->gemma3 ? sqrtf((float)m->d) : 1.0f);
    for (uint32_t l = 0u; l < m->n_layers; ++l) {
        GGBlock *w = &m->blocks[l];
        size_t stride = (size_t)m->nkv * m->ctx * m->hd;
        float *ck = m->cache_k + (size_t)l * stride;
        float *cv = m->cache_v + (size_t)l * stride;
        gg_norm(m->norm, m->x, &w->att_norm, m->d, m->eps);
        gg_mv(m->q, &w->q, m->norm, m->quant_act);
        gg_mv(m->k, &w->k, m->norm, m->quant_act);
        gg_mv(m->v, &w->v, m->norm, m->quant_act);
        if (w->q_norm.bytes) {
            for (uint32_t h = 0u; h < m->nh; ++h)
                gg_norm(m->q + (size_t)h * m->hd,
                        m->q + (size_t)h * m->hd,
                        &w->q_norm, m->hd, m->eps);
            for (uint32_t h = 0u; h < m->nkv; ++h)
                gg_norm(m->k + (size_t)h * m->hd,
                        m->k + (size_t)h * m->hd,
                        &w->k_norm, m->hd, m->eps);
        }
        for (uint32_t h = 0u; h < m->nh; ++h) {
            float *head = m->q + (size_t)h * m->hd;
            if (strcmp(m->arch, "qwen3") == 0 || m->gemma3)
                gg_rope_neox(head, pos, m->rope_dim,
                    m->gemma3 && m->sliding_window && (l + 1u) % 6u != 0u
                    ? m->local_theta : m->theta);
            else rope(head, pos, m->rope_dim, m->theta);
        }
        for (uint32_t h = 0u; h < m->nkv; ++h) {
            float *head = m->k + (size_t)h * m->hd;
            if (strcmp(m->arch, "qwen3") == 0 || m->gemma3)
                gg_rope_neox(head, pos, m->rope_dim,
                    m->gemma3 && m->sliding_window && (l + 1u) % 6u != 0u
                    ? m->local_theta : m->theta);
            else rope(head, pos, m->rope_dim, m->theta);
        }
        for (uint32_t h = 0u; h < m->nkv; ++h) {
            size_t at = (size_t)h * m->ctx * m->hd + (size_t)pos * m->hd;
            memcpy(ck + at, m->k + (size_t)h * m->hd, m->hd * sizeof(float));
            for (uint32_t j = 0u; j < m->hd; ++j) {
                cv[at + j] = gg_f16(gg_to_f16(m->v[(size_t)h * m->hd + j]));
            }
        }
        for (uint32_t h = 0u; h < m->nh; ++h) {
            uint32_t kh = h / (m->nh / m->nkv);
            float maxs = -INFINITY, sum = 0.0f;
            float *q = m->q + (size_t)h * m->hd;
            float *y = m->att + (size_t)h * m->hd;
            const float inv = 1.0f / sqrtf((float)m->hd);
            uint32_t first = (m->gemma3 && m->sliding_window &&
                (l + 1u) % 6u != 0u && pos >= m->sliding_window)
                ? pos + 1u - m->sliding_window : 0u;
            for (uint32_t t = first; t <= pos; ++t) {
                const float *key = ck + (size_t)kh * m->ctx * m->hd + (size_t)t * m->hd;
                float score = dot_f32(q, key, m->hd) * inv;
                if (!isfinite(score)) return NULL;
                m->scores[t] = score;
                if (score > maxs) maxs = score;
            }
            for (uint32_t t = first; t <= pos; ++t) {
                m->scores[t] = expf(m->scores[t] - maxs);
                sum += m->scores[t];
            }
            if (!(sum > 0.0f) || !isfinite(sum)) return NULL;
            memset(y, 0, m->hd * sizeof(float));
            for (uint32_t t = first; t <= pos; ++t) {
                const float *value = cv + (size_t)kh * m->ctx * m->hd + (size_t)t * m->hd;
                axpy_f32(y, value, m->scores[t] / sum, m->hd);
            }
        }
        gg_mv(m->acc, &w->o, m->att, m->quant_act);
        if (m->gemma3) gg_norm(m->acc, m->acc, &w->post_att_norm, m->d, m->eps);
        for (uint32_t i = 0u; i < m->d; ++i) m->x[i] += m->acc[i];
        gg_norm(m->norm, m->x, &w->ffn_norm, m->d, m->eps);
        gg_mv(m->gate, &w->gate, m->norm, m->quant_act);
        gg_mv(m->up, &w->up, m->norm, m->quant_act);
        for (uint32_t i = 0u; i < m->ff; ++i)
            m->gate[i] = (m->gemma3 ? gg_gelu(m->gate[i]) : silu(m->gate[i])) * m->up[i];
        gg_mv(m->acc, &w->down, m->gate, m->quant_act);
        if (m->gemma3) gg_norm(m->acc, m->acc, &w->post_ff_norm, m->d, m->eps);
        for (uint32_t i = 0u; i < m->d; ++i) m->x[i] += m->acc[i];
    }
    gg_norm(m->norm, m->x, &m->final_norm, m->d, m->eps);
    gg_mv(m->logits, &m->output, m->norm, m->quant_act);
    if (m->final_softcap > 0.0f)
        for (uint32_t i = 0u; i < m->vocab; ++i)
            m->logits[i] = m->final_softcap * tanhf(m->logits[i] / m->final_softcap);
    for (uint32_t i = 0u; i < m->vocab; ++i)
        if (!isfinite(m->logits[i])) return NULL;
    m->pos = pos;
    return m->logits;
}

/* Independent exact-value fixtures for every supported quantized tensor type.
 * Invoked by --smoke; these are decoder unit tests, not real-model accuracy.
 */
int niyah_gguf_quant_smoke(void) {
    uint8_t b[210];
    GGWeight w = {0};
    int fail = 0;
    w.bytes = b; w.dims[1] = 1u;
    if (fabsf(gg_f16(0x3c00u) - 1.0f) > 1e-7f ||
        fabsf(gg_f16(0x0001u) - 5.96046448e-8f) > 1e-11f) ++fail;
    {
        float v[4] = {1.0f, 0.0f, 0.0f, 1.0f};
        gg_rope_neox(v, 1u, 4u, 10000.0f);
        if (fabsf(v[0]-cosf(1.0f))>1e-6f ||
            fabsf(v[2]-sinf(1.0f))>1e-6f ||
            fabsf(v[1]+sinf(0.01f))>1e-6f ||
            fabsf(v[3]-cosf(0.01f))>1e-6f) ++fail;
    }
    memset(b, 0, sizeof b);
    w.type=2u; w.dims[0]=32u; w.row_bytes=18u;
    b[0]=0x00u; b[1]=0x38u; b[2]=0xb3u;
    if (fabsf(gg_value(&w,0u,0u)+2.5f)>1e-6f ||
        fabsf(gg_value(&w,0u,16u)-1.5f)>1e-6f) ++fail;
    memset(b, 0, sizeof b);
    w.type=3u; w.row_bytes=20u;
    b[0]=0x00u; b[1]=0x38u; b[2]=0x00u; b[3]=0x3cu; b[4]=0xb3u;
    if (fabsf(gg_value(&w,0u,0u)-2.5f)>1e-6f ||
        fabsf(gg_value(&w,0u,16u)-6.5f)>1e-6f) ++fail;
    memset(b, 0, sizeof b);
    w.type=8u; w.row_bytes=34u;
    b[0]=0x00u; b[1]=0x38u; b[2]=(uint8_t)-10; b[33]=7u;
    if (fabsf(gg_value(&w,0u,0u)+5.0f)>1e-6f ||
        fabsf(gg_value(&w,0u,31u)-3.5f)>1e-6f) ++fail;
    memset(b, 0, sizeof b);
    w.type=12u; w.dims[0]=256u; w.row_bytes=144u;
    b[0]=0x00u; b[1]=0x3cu; b[2]=0x00u; b[3]=0x38u;
    b[4]=2u; b[5]=4u; b[8]=1u; b[9]=3u;
    b[16]=0xa3u;
    if (fabsf(gg_value(&w,0u,0u)-5.5f)>1e-6f ||
        fabsf(gg_value(&w,0u,32u)-38.5f)>1e-6f) ++fail;
    memset(b, 0, sizeof b);
    w.type=13u; w.row_bytes=176u;
    b[0]=0x00u; b[1]=0x3cu; b[2]=0x00u; b[3]=0x38u;
    b[4]=2u; b[8]=1u; b[16]=1u; b[48]=3u;
    if (fabsf(gg_value(&w,0u,0u)-37.5f)>1e-6f) ++fail;
    memset(b, 0, sizeof b);
    w.type=14u; w.row_bytes=210u;
    b[0]=0x12u; b[32]=0x05u; b[128]=0xe4u;
    b[192]=2u; b[194]=3u; b[196]=4u; b[198]=5u;
    b[208]=0x00u; b[209]=0x38u;
    if (fabsf(gg_value(&w,0u,0u)+30.0f)>1e-6f ||
        fabsf(gg_value(&w,0u,32u)+16.5f)>1e-6f ||
        fabsf(gg_value(&w,0u,64u)-2.0f)>1e-6f ||
        fabsf(gg_value(&w,0u,96u)-40.0f)>1e-6f) ++fail;
    (void)printf("GGUF_QUANT_DECODERS=%s\n",fail?"FAIL":"PASS");
    return fail;
}
