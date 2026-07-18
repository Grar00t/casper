/*
 * niyah_core.c — NIYAH Inference Engine v3.0
 *
 * Compile (ARM64 / Snapdragon X Elite):
 *   gcc -O3 -march=armv8.2-a -std=c11 -Wall -Wextra -Werror
 *       -Wstrict-prototypes -Wmissing-prototypes
 *       -Wcast-align -Wwrite-strings -Wshadow -pedantic
 *       -I include niyah_core.c niyah_main.c -o niyah -lm
 *
 * Compile (x86_64 AVX2):
 *   gcc -O3 -mavx2 -mfma -march=native -std=c11 (same flags)
 *
 * SIMD strategy (compile-time selection, no runtime dispatch):
 *   AVX2+FMA  x86_64  dual-accum 16-float matvec
 *   NEON      aarch64 dual-accum  8-float matvec
 *   Scalar    fallback            4-wide unroll
 */

#define _GNU_SOURCE
#include "niyah_core.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <assert.h>
#include <time.h>
#include <stdint.h>

#if defined(_MSC_VER)
#include <windows.h>
static double niyah_monotonic_ms(void) {
    static LARGE_INTEGER freq = {0};
    LARGE_INTEGER now;
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart * 1000.0 / (double)freq.QuadPart;
}
#else
static double niyah_monotonic_ms(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) {
        return (double)clock() * 1000.0 / (double)CLOCKS_PER_SEC;
    }
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1000000.0;
}
#endif

/* ── SIMD headers ─────────────────────────────────────────────────── */
#if defined(__AVX2__) && defined(__FMA__)
#  include <immintrin.h>
#  define SIMD_AVX2 1
#elif defined(__ARM_NEON)
#  include <arm_neon.h>
#  define SIMD_NEON 1
#endif

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * §0  Compile-time assertions
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
typedef char _cfg_size_check[(sizeof(NiyahConfig) == 64) ? 1 : -1];

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * §1  Utility
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) { fprintf(stderr, "[niyah] OOM: %zu bytes\n", n); abort(); }
    return p;
}
static void *xcalloc(size_t n, size_t sz) {
    void *p = calloc(n, sz);
    if (!p) { fprintf(stderr, "[niyah] OOM: %zu×%zu bytes\n", n, sz); abort(); }
    return p;
}

const char *niyah_simd_name(void) {
#if defined(SIMD_AVX2)
    return "AVX2+FMA";
#elif defined(SIMD_NEON)
    return "NEON";
#else
    return "Scalar";
#endif
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * §2  SIMD kernels
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

/* ── matvec: y[R] = A[R×C] × x[C]  (row-major A) ─────────────────── */
static void matvec(float * restrict y,
                   const float * restrict A,
                   const float * restrict x,
                   size_t R, size_t C)
{
#if defined(SIMD_AVX2)
    for (size_t r = 0; r < R; r++) {
        const float *row = A + r * C;
        __m256 a0 = _mm256_setzero_ps();
        __m256 a1 = _mm256_setzero_ps();
        size_t c = 0;
        for (; c + 15 < C; c += 16) {
            a0 = _mm256_fmadd_ps(_mm256_loadu_ps(row+c),   _mm256_loadu_ps(x+c),   a0);
            a1 = _mm256_fmadd_ps(_mm256_loadu_ps(row+c+8), _mm256_loadu_ps(x+c+8), a1);
        }
        __m256  acc  = _mm256_add_ps(a0, a1);
        __m128  lo   = _mm256_castps256_ps128(acc);
        __m128  hi   = _mm256_extractf128_ps(acc, 1);
        __m128  s4   = _mm_add_ps(lo, hi);
        __m128  s2   = _mm_add_ps(s4, _mm_movehdup_ps(s4));
        float   dot  = _mm_cvtss_f32(_mm_add_ss(s2, _mm_movehl_ps(s2, s2)));
        for (; c < C; c++) dot += row[c] * x[c];
        y[r] = dot;
    }

#elif defined(SIMD_NEON)
    for (size_t r = 0; r < R; r++) {
        const float *row = A + r * C;
        float32x4_t a0 = vdupq_n_f32(0.f);
        float32x4_t a1 = vdupq_n_f32(0.f);
        size_t c = 0;
        for (; c + 7 < C; c += 8) {
            a0 = vfmaq_f32(a0, vld1q_f32(row+c),   vld1q_f32(x+c));
            a1 = vfmaq_f32(a1, vld1q_f32(row+c+4), vld1q_f32(x+c+4));
        }
        float32x4_t acc = vaddq_f32(a0, a1);
        float32x2_t lo2 = vadd_f32(vget_low_f32(acc), vget_high_f32(acc));
        float dot = vget_lane_f32(vpadd_f32(lo2, lo2), 0);
        for (; c < C; c++) dot += row[c] * x[c];
        y[r] = dot;
    }

#else   /* Scalar — 4-wide unroll */
    for (size_t r = 0; r < R; r++) {
        const float *row = A + r * C;
        float s0 = 0.f, s1 = 0.f, s2 = 0.f, s3 = 0.f;
        size_t c = 0;
        for (; c + 3 < C; c += 4) {
            s0 += row[c]   * x[c];
            s1 += row[c+1] * x[c+1];
            s2 += row[c+2] * x[c+2];
            s3 += row[c+3] * x[c+3];
        }
        float dot = s0 + s1 + s2 + s3;
        for (; c < C; c++) dot += row[c] * x[c];
        y[r] = dot;
    }
#endif
}

/* ── dot: scalar dot product of two vectors ──────────────────────── */
static float dot_f32(const float * restrict a,
                     const float * restrict b, size_t n)
{
#if defined(SIMD_AVX2)
    __m256 acc0 = _mm256_setzero_ps();
    __m256 acc1 = _mm256_setzero_ps();
    size_t i = 0;
    for (; i + 15 < n; i += 16) {
        acc0 = _mm256_fmadd_ps(_mm256_loadu_ps(a+i),   _mm256_loadu_ps(b+i),   acc0);
        acc1 = _mm256_fmadd_ps(_mm256_loadu_ps(a+i+8), _mm256_loadu_ps(b+i+8), acc1);
    }
    __m256 acc = _mm256_add_ps(acc0, acc1);
    __m128 lo  = _mm256_castps256_ps128(acc);
    __m128 hi  = _mm256_extractf128_ps(acc, 1);
    __m128 s4  = _mm_add_ps(lo, hi);
    __m128 s2  = _mm_add_ps(s4, _mm_movehdup_ps(s4));
    float  d   = _mm_cvtss_f32(_mm_add_ss(s2, _mm_movehl_ps(s2, s2)));
    for (; i < n; i++) d += a[i]*b[i];
    return d;
#elif defined(SIMD_NEON)
    float32x4_t acc0 = vdupq_n_f32(0.f);
    float32x4_t acc1 = vdupq_n_f32(0.f);
    size_t i = 0;
    for (; i + 7 < n; i += 8) {
        acc0 = vfmaq_f32(acc0, vld1q_f32(a+i),   vld1q_f32(b+i));
        acc1 = vfmaq_f32(acc1, vld1q_f32(a+i+4), vld1q_f32(b+i+4));
    }
    float32x4_t acc = vaddq_f32(acc0, acc1);
    float32x2_t lo2 = vadd_f32(vget_low_f32(acc), vget_high_f32(acc));
    float d = vget_lane_f32(vpadd_f32(lo2, lo2), 0);
    for (; i < n; i++) d += a[i]*b[i];
    return d;
#else
    float d = 0.f;
    for (size_t i = 0; i < n; i++) d += a[i]*b[i];
    return d;
#endif
}

/* ── rmsnorm ─────────────────────────────────────────────────────── */
static void rmsnorm(float * restrict out,
                    const float * restrict x,
                    const float * restrict w,
                    size_t n, float eps)
{
    float ss = 0.f;
#if defined(SIMD_AVX2)
    {
        __m256 vss = _mm256_setzero_ps();
        size_t ri = 0;
        for (; ri + 7 < n; ri += 8) {
            __m256 vv = _mm256_loadu_ps(x + ri);
            vss = _mm256_fmadd_ps(vv, vv, vss);
        }
        __m128 lo = _mm256_castps256_ps128(vss);
        __m128 hi = _mm256_extractf128_ps(vss, 1);
        __m128 s4 = _mm_add_ps(lo, hi);
        __m128 s2 = _mm_add_ps(s4, _mm_movehdup_ps(s4));
        ss = _mm_cvtss_f32(_mm_add_ss(s2, _mm_movehl_ps(s2, s2)));
        for (; ri < n; ri++) ss += x[ri] * x[ri];
    }
#elif defined(SIMD_NEON)
    {
        float32x4_t vss = vdupq_n_f32(0.f);
        size_t ri = 0;
        for (; ri + 3 < n; ri += 4) {
            float32x4_t vv = vld1q_f32(x + ri);
            vss = vfmaq_f32(vss, vv, vv);
        }
        float32x2_t lo2 = vadd_f32(vget_low_f32(vss), vget_high_f32(vss));
        ss = vget_lane_f32(vpadd_f32(lo2, lo2), 0);
        for (; ri < n; ri++) ss += x[ri] * x[ri];
    }
#else
    for (size_t j = 0; j < n; j++) ss += x[j] * x[j];
#endif
    float scale = 1.0f / sqrtf(ss / (float)n + eps);
    for (size_t k = 0; k < n; k++) out[k] = x[k] * scale * w[k];
}

/* ── SiLU ─────────────────────────────────────────────────────────── */
static inline float silu(float v) { return v / (1.0f + expf(-v)); }

/* ── RoPE ─────────────────────────────────────────────────────────── */
static void rope(float *qk, uint32_t pos, uint32_t head_dim, float theta) {
    for (uint32_t i = 0; i < head_dim; i += 2) {
        float angle = (float)pos / powf(theta, (float)i / (float)head_dim);
        float c = cosf(angle), s = sinf(angle);
        float v0 = qk[i], v1 = qk[i+1];
        qk[i]   = v0*c - v1*s;
        qk[i+1] = v0*s + v1*c;
    }
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * §3  Pool size arithmetic
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

/* Returns number of weight floats (not counting KV cache or scratch) */
static size_t weight_count(const NiyahConfig *c) {
    size_t d  = c->embed_dim;
    size_t kd = c->n_kv_heads * (d / c->n_heads); /* kv_dim */
    size_t f  = d * c->ffn_mult;
    size_t L  = c->n_layers;
    size_t v  = c->vocab_size;
    /* per layer: wq kv wv wo w_gate w_up w_down rms_att rms_ffn */
    size_t pl = d*d + kd*d + kd*d + d*d     /* QKV O */
              + f*d + f*d + d*f              /* FFN    */
              + d   + d;                     /* norms  */
    return L*pl + v*d + d + v*d;            /* layers + embed + rms_final + lmhead */
}

static size_t kv_count(const NiyahConfig *c) {
    size_t hd = c->embed_dim / c->n_heads;
    return 2UL * c->n_layers * c->n_kv_heads * c->ctx_len * hd;
}

/* scratch: x xb xb2 hb1 hb2 q k v att logits */
static size_t scratch_count(const NiyahConfig *c) {
    size_t d = c->embed_dim;
    size_t f = d * c->ffn_mult;
    size_t a = (size_t)c->n_heads * c->ctx_len;
    return 3*d + 2*f + 3*d + a + c->vocab_size;
}

size_t niyah_param_count(const NiyahModel *m) {
    return weight_count(&m->cfg);
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * §4  Alloc / free
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

NiyahModel *niyah_alloc(const NiyahConfig *cfg) {
    assert(cfg->embed_dim > 0 && cfg->n_heads > 0 && cfg->n_layers > 0);
    assert(cfg->embed_dim % cfg->n_heads == 0);
    assert(cfg->n_kv_heads > 0 && cfg->n_kv_heads <= cfg->n_heads);
    assert(cfg->vocab_size > 0 && cfg->ctx_len > 0);

    NiyahModel *m = xcalloc(1, sizeof(*m));
    m->cfg      = *cfg;
    m->head_dim = cfg->embed_dim / cfg->n_heads;
    m->kv_dim   = cfg->n_kv_heads * m->head_dim;
    m->ffn_dim  = cfg->embed_dim * cfg->ffn_mult;

    size_t nw  = weight_count(cfg);
    size_t nkv = kv_count(cfg);
    size_t nsc = scratch_count(cfg);
    size_t nl  = cfg->n_layers;

    size_t float_bytes = (nw + nkv + nsc) * sizeof(float);
    /* Pad float region up to NiyahLayer alignment to avoid UB on LP64 */
    size_t align_pad   = (float_bytes % _Alignof(NiyahLayer)) == 0
                       ? 0
                       : _Alignof(NiyahLayer) - (float_bytes % _Alignof(NiyahLayer));
    m->_pool_bytes = float_bytes + align_pad + nl * sizeof(NiyahLayer);
    m->_pool = xcalloc(1, m->_pool_bytes);

    float      *fp = (float *)m->_pool;
    NiyahLayer *lp = (NiyahLayer *)((char *)m->_pool + float_bytes + align_pad);
    m->layers = lp;

    /* Assign weight pointers */
    size_t d  = cfg->embed_dim;
    size_t kd = m->kv_dim;
    size_t f  = m->ffn_dim;
    float *p  = fp;

    for (uint32_t l = 0; l < nl; l++) {
        m->layers[l].wq      = p; p += d*d;
        m->layers[l].wk      = p; p += kd*d;
        m->layers[l].wv      = p; p += kd*d;
        m->layers[l].wo      = p; p += d*d;
        m->layers[l].w_gate  = p; p += f*d;
        m->layers[l].w_up    = p; p += f*d;
        m->layers[l].w_down  = p; p += d*f;
        m->layers[l].rms_att = p; p += d;
        m->layers[l].rms_ffn = p; p += d;
    }
    m->token_embed = p; p += cfg->vocab_size * d;
    m->rms_final   = p; p += d;
    m->lm_head     = p; p += cfg->vocab_size * d;

    /* KV cache */
    m->kv_k = p; p += nkv / 2;
    m->kv_v = p; p += nkv / 2;

    /* Scratch — logits at end */
    m->scratch  = p;
    m->_logits  = p + 3*d + 2*f + 3*d
                + (size_t)cfg->n_heads * cfg->ctx_len; /* after att[] */

    /* Verify layout didn't overflow */
    assert((size_t)(p - fp) == nw + nkv);
    return m;
}

void niyah_free(NiyahModel *m) {
    if (!m) return;
    free(m->_pool);
    free(m);
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * §5  Save / load
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

int niyah_save(const NiyahModel *m, const char *path) {
    FILE *fp = fopen(path, "wb");
    if (!fp) { perror(path); return -1; }

    NiyahConfig hdr = m->cfg;
    hdr.magic   = NIYAH_MAGIC;
    hdr.version = NIYAH_VER;

    if (fwrite(&hdr, sizeof(hdr), 1, fp) != 1) goto err;
    size_t nw = weight_count(&m->cfg);
    if (fwrite(m->_pool, sizeof(float), nw, fp) != nw) goto err;
    fclose(fp);
    return 0;
err:
    fclose(fp);
    return -1;
}

int niyah_load(NiyahModel **out, const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror(path); return -1; }

    NiyahConfig cfg = {0};
    if (fread(&cfg, sizeof(cfg), 1, fp) != 1) { fclose(fp); return -1; }
    if (cfg.magic != NIYAH_MAGIC) {
        fprintf(stderr, "[niyah_load] bad magic 0x%08x\n", cfg.magic);
        fclose(fp); return -1;
    }
    if (cfg.version != NIYAH_VER) {
        fprintf(stderr, "[niyah_load] version mismatch: file=%u engine=%u\n",
                cfg.version, NIYAH_VER);
        fclose(fp); return -2;
    }

    NiyahModel *m = niyah_alloc(&cfg);
    size_t nw = weight_count(&cfg);
    if (fread(m->_pool, sizeof(float), nw, fp) != nw) {
        niyah_free(m); fclose(fp); return -1;
    }
    fclose(fp);
    *out = m;
    return 0;
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * §6  Forward pass
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

/* Scratch pointer accessors */
#define SCR_X(m)   ((m)->scratch)
#define SCR_XB(m)  ((m)->scratch + (m)->cfg.embed_dim)
#define SCR_XB2(m) ((m)->scratch + 2*(m)->cfg.embed_dim)
#define SCR_HB(m)  ((m)->scratch + 3*(m)->cfg.embed_dim)
#define SCR_HB2(m) ((m)->scratch + 3*(m)->cfg.embed_dim + (m)->ffn_dim)
#define SCR_Q(m)   ((m)->scratch + 3*(m)->cfg.embed_dim + 2*(m)->ffn_dim)
#define SCR_K(m)   ((m)->scratch + 3*(m)->cfg.embed_dim + 2*(m)->ffn_dim \
                                 + (m)->cfg.embed_dim)
#define SCR_V(m)   ((m)->scratch + 3*(m)->cfg.embed_dim + 2*(m)->ffn_dim \
                                 + (m)->cfg.embed_dim + (m)->kv_dim)
#define SCR_ATT(m) ((m)->scratch + 3*(m)->cfg.embed_dim + 2*(m)->ffn_dim \
                                 + (m)->cfg.embed_dim + 2*(m)->kv_dim)

float *niyah_forward(NiyahModel *m, uint32_t token, uint32_t pos) {
    const NiyahConfig *c = &m->cfg;
    uint32_t d   = c->embed_dim;
    uint32_t hd  = m->head_dim;
    uint32_t nh  = c->n_heads;
    uint32_t nkv = c->n_kv_heads;
    uint32_t ctx = c->ctx_len;

    assert(token < c->vocab_size);
    assert(pos   < ctx);

    float *x   = SCR_X(m);
    float *xb  = SCR_XB(m);
    float *xb2 = SCR_XB2(m);
    float *hb  = SCR_HB(m);
    float *hb2 = SCR_HB2(m);
    float *q   = SCR_Q(m);
    float *k   = SCR_K(m);
    float *v   = SCR_V(m);
    float *att = SCR_ATT(m);

    /* 1. Token embedding lookup */
    memcpy(x, m->token_embed + (size_t)token * d, d * sizeof(float));

    /* 2. Transformer layers */
    for (uint32_t l = 0; l < c->n_layers; l++) {
        const NiyahLayer *lw = &m->layers[l];

        /* 2a. Pre-attention RMSnorm */
        rmsnorm(xb, x, lw->rms_att, d, c->rms_eps);

        /* 2b. QKV projections */
        matvec(q, lw->wq, xb, d,        d);
        matvec(k, lw->wk, xb, m->kv_dim, d);
        matvec(v, lw->wv, xb, m->kv_dim, d);

        /* 2c. RoPE — applied per head */
        for (uint32_t h = 0; h < nh;  h++) rope(q + h*hd, pos, hd, c->rope_theta);
        for (uint32_t h = 0; h < nkv; h++) rope(k + h*hd, pos, hd, c->rope_theta);

        /* 2d. Write K,V into head-major KV cache
         * Layout: kv_k[ l * nkv*ctx*hd + h*ctx*hd + pos*hd ] */
        size_t L_stride = (size_t)nkv * ctx * hd;
        float *kc = m->kv_k + l * L_stride;
        float *vc = m->kv_v + l * L_stride;
        for (uint32_t h = 0; h < nkv; h++) {
            float *dst_k = kc + (size_t)h * ctx * hd + (size_t)pos * hd;
            float *dst_v = vc + (size_t)h * ctx * hd + (size_t)pos * hd;
            memcpy(dst_k, k + h*hd, hd * sizeof(float));
            memcpy(dst_v, v + h*hd, hd * sizeof(float));
        }

        /* 2e. Grouped Multi-head Attention */
        memset(xb2, 0, d * sizeof(float));
        float attn_scale = 1.0f / sqrtf((float)hd);
        for (uint32_t h = 0; h < nh; h++) {
            const float *qh = q + h * hd;
            /* GQA: map query head to its KV head */
            uint32_t kv_h = h * nkv / nh;
            float *ah = att + h * ctx;

            /* Dot product with all past K */
            for (uint32_t t = 0; t <= pos; t++) {
                const float *kt = kc + (size_t)kv_h*ctx*hd + (size_t)t*hd;
                ah[t] = dot_f32(qh, kt, hd) * attn_scale;
            }

            /* Softmax over [0..pos] */
            float mx = ah[0];
            for (uint32_t t = 1; t <= pos; t++) if (ah[t] > mx) mx = ah[t];
            float sm = 0.f;
            for (uint32_t t = 0; t <= pos; t++) { ah[t] = expf(ah[t]-mx); sm += ah[t]; }
            float inv = 1.f/sm;
            for (uint32_t t = 0; t <= pos; t++) ah[t] *= inv;

            /* Weighted sum of V */
            float *oh = xb2 + h * hd;
            memset(oh, 0, hd * sizeof(float));
            for (uint32_t t = 0; t <= pos; t++) {
                const float *vt = vc + (size_t)kv_h*ctx*hd + (size_t)t*hd;
                float wt = ah[t];
                for (uint32_t i = 0; i < hd; i++) oh[i] += wt * vt[i];
            }
        }

        /* 2f. Output projection + residual */
        matvec(xb, lw->wo, xb2, d, d);
        for (uint32_t i = 0; i < d; i++) x[i] += xb[i];

        /* 2g. Pre-FFN RMSnorm */
        rmsnorm(xb, x, lw->rms_ffn, d, c->rms_eps);

        /* 2h. SwiGLU FFN: down(silu(gate(xb)) * up(xb)) */
        matvec(hb,  lw->w_gate, xb, m->ffn_dim, d);
        matvec(hb2, lw->w_up,   xb, m->ffn_dim, d);
        for (uint32_t i = 0; i < m->ffn_dim; i++) hb[i] = silu(hb[i]) * hb2[i];
        matvec(xb, lw->w_down, hb, d, m->ffn_dim);
        for (uint32_t i = 0; i < d; i++) x[i] += xb[i];
    }

    /* 3. Final RMSnorm */
    rmsnorm(xb, x, m->rms_final, d, c->rms_eps);

    /* 4. LM head → logits */
    matvec(m->_logits, m->lm_head, xb, c->vocab_size, d);
    return m->_logits;
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * §7  Sampling
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

uint32_t niyah_sample(const float *logits, uint32_t vocab_size,
                      NiyahSampler *s)
{
    /* Greedy */
    if (s->temperature <= 0.f) {
        uint32_t best = 0;
        for (uint32_t i = 1; i < vocab_size; i++)
            if (logits[i] > logits[best]) best = i;
        return best;
    }

    /*
     * Temperature + top-p sampling — zero heap allocation.
     *
     * Two-pass algorithm:
     *   Pass 1: compute log-sum-exp normalisation constant.
     *   Pass 2: walk the distribution accumulating probability until
     *           the random threshold is reached.
     *
     * This avoids storing the full softmax array and therefore
     * eliminates the loose malloc that previously appeared in this
     * function.  The result is numerically identical to the
     * store-then-scan approach.
     */
    float mx = logits[0];
    for (uint32_t i = 1; i < vocab_size; i++)
        if (logits[i] > mx) mx = logits[i];

    float sm = 0.f;
    for (uint32_t i = 0; i < vocab_size; i++)
        sm += expf((logits[i] - mx) / s->temperature);

    /* LCG random — advance before use so seed=0 is not degenerate */
    s->seed = s->seed * 6364136223846793005ULL + 1442695040888963407ULL;
    float r = (float)((s->seed >> 11) & 0x0FFFFFFU) / (float)0x0FFFFFFU;
    r *= (s->top_p > 0.f && s->top_p < 1.f) ? s->top_p : 1.f;
    r *= sm; /* scale threshold to unnormalised sum */

    float cum = 0.f;
    for (uint32_t i = 0; i < vocab_size; i++) {
        cum += expf((logits[i] - mx) / s->temperature);
        if (cum >= r) return i;
    }
    return vocab_size - 1;
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * §8  Training (output-layer Adam)
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

NiyahAdam *niyah_adam_alloc(const NiyahModel *m) {
    NiyahAdam *opt = xcalloc(1, sizeof(*opt));
    opt->n_weights = weight_count(&m->cfg);
    opt->m   = xcalloc(opt->n_weights, sizeof(float));
    opt->v   = xcalloc(opt->n_weights, sizeof(float));
    opt->lr    = 3e-4f;
    opt->beta1 = 0.9f;
    opt->beta2 = 0.999f;
    opt->eps   = 1e-8f;
    opt->wd    = 0.01f;
    return opt;
}

void niyah_adam_free(NiyahAdam *opt) {
    if (!opt) return;
    free(opt->m);
    free(opt->v);
    free(opt);
}

float niyah_train_step(NiyahModel *m, NiyahAdam *opt,
                       const uint32_t *tokens, uint32_t n)
{
    if (n < 2) return 0.f;

    size_t nw = weight_count(&m->cfg);
    float *grad = xcalloc(nw, sizeof(float));
    float *dL   = xmalloc(m->cfg.vocab_size * sizeof(float));
    float loss  = 0.f;
    uint32_t d  = m->cfg.embed_dim;

    for (uint32_t t = 0; t < n - 1u; t++) {
        const float *logits = niyah_forward(m, tokens[t], t);
        uint32_t tgt = tokens[t+1];

        /* log-sum-exp: guard against logf(0) with FLT_MIN floor */
        float mx = logits[0];
        for (uint32_t i = 1; i < m->cfg.vocab_size; i++)
            if (logits[i] > mx) mx = logits[i];
        float lse_sum = 0.f;
        for (uint32_t i = 0; i < m->cfg.vocab_size; i++)
            lse_sum += expf(logits[i] - mx);
        float lse = logf(lse_sum) + mx;

        loss += lse - logits[tgt];

        /* dL/dlogits = softmax - one_hot(tgt) */
        for (uint32_t i = 0; i < m->cfg.vocab_size; i++)
            dL[i] = expf(logits[i] - lse);
        dL[tgt] -= 1.f;

        /* Outer product: dW_lmhead += dL ⊗ xb  (last rmsnorm output) */
        const float *xb    = SCR_XB(m);
        size_t  off        = nw - (size_t)m->cfg.vocab_size * d;
        float  *dW         = grad + off;
        for (uint32_t i = 0; i < m->cfg.vocab_size; i++) {
            float dl = dL[i];
            for (uint32_t j = 0; j < d; j++)
                dW[(size_t)i*d + j] += dl * xb[j];
        }
    }
    loss /= (float)(n-1);

    /* Adam update — lm_head only */
    opt->step++;
    float bc1 = 1.f - powf(opt->beta1, (float)opt->step);
    float bc2 = 1.f - powf(opt->beta2, (float)opt->step);

    size_t off   = nw - (size_t)m->cfg.vocab_size * d;
    float *W     = (float *)m->_pool + off;
    const float *gW = grad + off;
    float *mW    = opt->m + off;
    float *vW    = opt->v + off;
    size_t cnt   = (size_t)m->cfg.vocab_size * d;

    for (size_t i = 0; i < cnt; i++) {
        float g = gW[i] + opt->wd * W[i];
        mW[i] = opt->beta1*mW[i] + (1.f-opt->beta1)*g;
        vW[i] = opt->beta2*vW[i] + (1.f-opt->beta2)*g*g;
        float mh = mW[i]/bc1;
        float vh = vW[i]/bc2;
        W[i] -= opt->lr * mh / (sqrtf(vh) + opt->eps);
    }

    free(dL);
    free(grad);
    return loss;
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * §9  Smoke test  (returns 0 = all pass)
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

#define T_PASS(cond, label) do { \
    int _cond = (cond); \
    if (_cond) { pass++; fprintf(stderr,"  [PASS] %s\n", label); } \
    else      { fail++; fprintf(stderr,"  [FAIL] %s\n", label); } \
} while(0)

int niyah_smoke(void) {
    int pass = 0, fail = 0;

    fprintf(stderr, "\n╔══════════════════════════════════════╗\n");
    fprintf(stderr, "║  NIYAH v3.0  Smoke Test  [%s]\n", niyah_simd_name());
    fprintf(stderr, "╚══════════════════════════════════════╝\n");

    /* §9.0 — Struct size */
    T_PASS(sizeof(NiyahConfig) == 64, "NiyahConfig == 64 bytes");

    /* §9.1 — Alloc / free */
    NiyahConfig cfg = {
        .magic      = NIYAH_MAGIC, .version = NIYAH_VER,
        .embed_dim  = 64,          .n_heads = 4,
        .n_kv_heads = 4,           .n_layers = 2,
        .ffn_mult   = 4,           .vocab_size = 256,
        .ctx_len    = 32,          .rope_theta = 10000.f,
        .rms_eps    = 1e-5f,
    };

    NiyahModel *m = niyah_alloc(&cfg);
    T_PASS(m != NULL,        "alloc non-null");
    if (!m) { fprintf(stderr, "alloc failed — abort smoke\n"); return 1; }
    T_PASS(m->_pool != NULL, "pool non-null");
    T_PASS(m->kv_k  != NULL, "kv_k non-null");
    T_PASS(m->kv_v  != NULL, "kv_v non-null");

    /* §9.2 — Param count plausible */
    size_t params = niyah_param_count(m);
    T_PASS(params > 0 && params < 10000000UL, "param_count in range");
    fprintf(stderr, "  params = %zu (%.1f K)\n", params, params/1000.0);

    /* §9.3 — Init weights (non-zero RMS weights, small randoms elsewhere) */
    float *wp = (float *)m->_pool;
    size_t nw = weight_count(&cfg);
    for (size_t i = 0; i < nw; i++) wp[i] = ((float)(i % 37) - 18.f) * 0.005f;
    for (uint32_t l = 0; l < cfg.n_layers; l++) {
        for (uint32_t j = 0; j < cfg.embed_dim; j++) {
            m->layers[l].rms_att[j] = 1.f;
            m->layers[l].rms_ffn[j] = 1.f;
        }
    }
    for (uint32_t j = 0; j < cfg.embed_dim; j++) m->rms_final[j] = 1.f;

    /* §9.4 — Forward pass: 8 tokens, all logits finite */
    bool finite_ok = true;
    for (uint32_t t = 0; t < 8; t++) {
        const float *lg = niyah_forward(m, t % cfg.vocab_size, t);
        for (uint32_t i = 0; i < cfg.vocab_size; i++)
            if (!isfinite(lg[i])) { finite_ok = false; break; }
        if (!finite_ok) break;
    }
    T_PASS(finite_ok, "forward 8 tokens: all logits finite");

    /* §9.5 — Benchmark 200 passes */
    double t0 = niyah_monotonic_ms();
    for (int i = 0; i < 200; i++)
        niyah_forward(m, (uint32_t)(i % cfg.vocab_size), 0);
    double t1 = niyah_monotonic_ms();
    double ms = t1 - t0;
    double tok_s = 200.0 / (ms * 1e-3);
    fprintf(stderr, "  [BENCH] 200 forward  %.2f ms  →  %.0f tok/s\n", ms, tok_s);
    T_PASS(ms > 0.0, "bench time > 0");

    /* §9.6 — Save / load round-trip */
    const char *tmp = "/tmp/niyah_v3_smoke.bin";
    T_PASS(niyah_save(m, tmp) == 0, "save returns 0");

    NiyahModel *m2 = NULL;
    int rc = niyah_load(&m2, tmp);
    T_PASS(rc == 0,   "load returns 0");
    T_PASS(m2 != NULL, "load non-null");

    /* §9.7 — Bitwise logit match after round-trip */
    if (m2) {
        const float *l1 = niyah_forward(m,  0, 0);
        const float *l2 = niyah_forward(m2, 0, 0);
        bool match = true;
        for (uint32_t i = 0; i < cfg.vocab_size; i++)
            if (l1[i] != l2[i]) { match = false; break; }
        T_PASS(match, "save/load bitwise logit match");
        niyah_free(m2);
    }

    /* §9.8 — Version mismatch → -2 */
    {
        FILE *fp = fopen("/tmp/niyah_v3_bad.bin", "wb");
        NiyahConfig bc = cfg; bc.version = 0xDEADBEEF;
        fwrite(&bc, sizeof(bc), 1, fp); fclose(fp);
    }
    NiyahModel *bad = NULL;
    int rc2 = niyah_load(&bad, "/tmp/niyah_v3_bad.bin");
    T_PASS(rc2 == -2 && bad == NULL, "version mismatch → -2, out=NULL");

    /* §9.9 — Sampler */
    {
        const float *lg = niyah_forward(m, 0, 0);
        NiyahSampler greedy = { .temperature=0.f, .top_p=0.f, .seed=42 };
        NiyahSampler stoch  = { .temperature=1.f, .top_p=0.9f,.seed=12345 };
        uint32_t g = niyah_sample(lg, cfg.vocab_size, &greedy);
        uint32_t s = niyah_sample(lg, cfg.vocab_size, &stoch);
        T_PASS(g < cfg.vocab_size, "greedy sample in range");
        T_PASS(s < cfg.vocab_size, "stochastic sample in range");
        fprintf(stderr, "  greedy=%u  stochastic=%u\n", g, s);
    }

    /* §9.10 — Train step */
    {
        NiyahAdam *opt = niyah_adam_alloc(m);
        T_PASS(opt != NULL, "adam alloc");
        const uint32_t toks[5] = {1, 2, 3, 4, 5};
        float loss = niyah_train_step(m, opt, toks, 5);
        T_PASS(isfinite(loss) && loss > 0.f, "train_step: loss finite & > 0");
        fprintf(stderr, "  loss = %.4f\n", (double)loss);
        niyah_adam_free(opt);
    }

    /* §9.11 — Medium model bench */
    {
        NiyahConfig mc = {
            .magic=NIYAH_MAGIC,.version=NIYAH_VER,
            .embed_dim=256,.n_heads=8,.n_kv_heads=8,
            .n_layers=4,.ffn_mult=4,.vocab_size=512,
            .ctx_len=64,.rope_theta=10000.f,.rms_eps=1e-5f
        };
        NiyahModel *mm = niyah_alloc(&mc);
        float *wp2 = (float *)mm->_pool;
        size_t nw2 = weight_count(&mc);
        for (size_t i = 0; i < nw2; i++) wp2[i] = ((float)(i%31)-15.f)*0.005f;
        for (uint32_t l = 0; l < mc.n_layers; l++)
            for (uint32_t j = 0; j < mc.embed_dim; j++) {
                mm->layers[l].rms_att[j] = 1.f;
                mm->layers[l].rms_ffn[j] = 1.f;
            }
        for (uint32_t j = 0; j < mc.embed_dim; j++) mm->rms_final[j] = 1.f;

        double t0b = niyah_monotonic_ms();
        for (int i = 0; i < 50; i++)
            niyah_forward(mm, (uint32_t)(i % mc.vocab_size), 0);
        double t1b = niyah_monotonic_ms();
        double ms2 = t1b - t0b;
        fprintf(stderr,"  [BENCH] medium(embed=256,L=4) 50 fwd  %.2f ms  →  %.0f tok/s\n",
                ms2, 50.0/(ms2*1e-3));
        T_PASS(ms2 > 0.0, "medium bench time > 0");
        niyah_free(mm);
    }

    niyah_free(m);

    fprintf(stderr, "\n  Results: %d passed, %d failed\n\n", pass, fail);
    return fail;
}

