#include "niyah_train_full.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *train_calloc(size_t n, size_t size)
{
    if (size != 0u && n > SIZE_MAX / size) return NULL;
    return calloc(n, size);
}

static float train_dot(const float *a, const float *b, uint32_t n)
{
    float sum = 0.0f;
    uint32_t i;
    for (i = 0u; i < n; ++i) sum += a[i] * b[i];
    return sum;
}

static void train_matvec(float *out, const float *matrix, const float *in,
                         uint32_t rows, uint32_t cols)
{
    uint32_t r;
    for (r = 0u; r < rows; ++r) {
        const float *row = matrix + (size_t)r * cols;
        out[r] = train_dot(row, in, cols);
    }
}

static void train_matvec_t_add(float *out, const float *matrix, const float *grad_out,
                               uint32_t rows, uint32_t cols)
{
    uint32_t r;
    for (r = 0u; r < rows; ++r) {
        const float g = grad_out[r];
        const float *row = matrix + (size_t)r * cols;
        uint32_t c;
        for (c = 0u; c < cols; ++c) out[c] += row[c] * g;
    }
}

static void train_outer_add(float *grad_matrix, const float *grad_out, const float *in,
                            uint32_t rows, uint32_t cols)
{
    uint32_t r;
    for (r = 0u; r < rows; ++r) {
        const float g = grad_out[r];
        float *row = grad_matrix + (size_t)r * cols;
        uint32_t c;
        for (c = 0u; c < cols; ++c) row[c] += g * in[c];
    }
}

static void train_rmsnorm(float *out, const float *x, const float *weight,
                          uint32_t n, float eps)
{
    float ss = 0.0f;
    float scale;
    uint32_t i;
    for (i = 0u; i < n; ++i) ss += x[i] * x[i];
    scale = 1.0f / sqrtf(ss / (float)n + eps);
    for (i = 0u; i < n; ++i) out[i] = x[i] * weight[i] * scale;
}

static void train_rmsnorm_backward(float *grad_x, float *grad_weight,
                                   const float *x, const float *weight,
                                   const float *grad_out, uint32_t n, float eps)
{
    float ss = 0.0f;
    float weighted_dot = 0.0f;
    float scale;
    float common;
    uint32_t i;

    for (i = 0u; i < n; ++i) ss += x[i] * x[i];
    scale = 1.0f / sqrtf(ss / (float)n + eps);
    for (i = 0u; i < n; ++i) {
        weighted_dot += grad_out[i] * weight[i] * x[i];
        grad_weight[i] += grad_out[i] * x[i] * scale;
    }
    common = (scale * scale * scale) * weighted_dot / (float)n;
    for (i = 0u; i < n; ++i) {
        grad_x[i] += grad_out[i] * weight[i] * scale - x[i] * common;
    }
}

static void train_rope(float *x, uint32_t pos, uint32_t head_dim, float theta)
{
    uint32_t i;
    for (i = 0u; i + 1u < head_dim; i += 2u) {
        const float angle = (float)pos / powf(theta, (float)i / (float)head_dim);
        const float c = cosf(angle);
        const float s = sinf(angle);
        const float x0 = x[i];
        const float x1 = x[i + 1u];
        x[i] = x0 * c - x1 * s;
        x[i + 1u] = x0 * s + x1 * c;
    }
}

static void train_rope_backward(float *grad, uint32_t pos, uint32_t head_dim, float theta)
{
    uint32_t i;
    for (i = 0u; i + 1u < head_dim; i += 2u) {
        const float angle = (float)pos / powf(theta, (float)i / (float)head_dim);
        const float c = cosf(angle);
        const float s = sinf(angle);
        const float g0 = grad[i];
        const float g1 = grad[i + 1u];
        grad[i] = g0 * c + g1 * s;
        grad[i + 1u] = -g0 * s + g1 * c;
    }
}

static float train_silu(float x)
{
    return x / (1.0f + expf(-x));
}

static float train_silu_grad(float x)
{
    const float sig = 1.0f / (1.0f + expf(-x));
    return sig * (1.0f + x * (1.0f - sig));
}

static uint64_t train_splitmix64(uint64_t *state)
{
    uint64_t z;
    *state += UINT64_C(0x9E3779B97F4A7C15);
    z = *state;
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

static float train_random_signed(uint64_t *state)
{
    const uint64_t bits = train_splitmix64(state);
    const uint32_t mantissa = (uint32_t)((bits >> 40) & UINT64_C(0xFFFFFF));
    return ((float)mantissa / 8388608.0f) - 1.0f;
}

static void train_init_matrix(float *matrix, uint32_t rows, uint32_t cols, uint64_t *state)
{
    const float scale = sqrtf(6.0f / ((float)rows + (float)cols));
    size_t count = (size_t)rows * cols;
    size_t i;
    for (i = 0u; i < count; ++i) matrix[i] = train_random_signed(state) * scale;
}

void niyah_init_weights(NiyahModel *m, uint64_t seed)
{
    uint64_t state = seed;
    uint32_t l;
    const uint32_t d = m ? m->cfg.embed_dim : 0u;

    if (!m || !m->_pool) return;

    memset(m->_pool, 0, niyah_param_count(m) * sizeof(float));

    for (l = 0u; l < m->cfg.n_layers; ++l) {
        NiyahLayer *layer = &m->layers[l];
        uint32_t i;
        train_init_matrix(layer->wq, d, d, &state);
        train_init_matrix(layer->wk, m->kv_dim, d, &state);
        train_init_matrix(layer->wv, m->kv_dim, d, &state);
        train_init_matrix(layer->wo, d, d, &state);
        train_init_matrix(layer->w_gate, m->ffn_dim, d, &state);
        train_init_matrix(layer->w_up, m->ffn_dim, d, &state);
        train_init_matrix(layer->w_down, d, m->ffn_dim, &state);
        for (i = 0u; i < d; ++i) {
            layer->rms_att[i] = 1.0f;
            layer->rms_ffn[i] = 1.0f;
        }
    }

    {
        const float embed_scale = 1.0f / sqrtf((float)d);
        size_t count = (size_t)m->cfg.vocab_size * d;
        size_t i;
        for (i = 0u; i < count; ++i) {
            m->token_embed[i] = train_random_signed(&state) * embed_scale;
        }
        for (i = 0u; i < d; ++i) m->rms_final[i] = 1.0f;
        train_init_matrix(m->lm_head, m->cfg.vocab_size, d, &state);
    }

    {
        const size_t kv_each = (size_t)m->cfg.n_layers * m->cfg.n_kv_heads
                             * m->cfg.ctx_len * m->head_dim;
        memset(m->kv_k, 0, kv_each * sizeof(float));
        memset(m->kv_v, 0, kv_each * sizeof(float));
    }
}

static float *train_grad_ptr(float *grad, float *base, float *weight)
{
    return grad + (size_t)(weight - base);
}

float niyah_full_train_step(NiyahModel *m, NiyahAdam *opt,
                            const uint32_t *tokens, uint32_t n)
{
    uint32_t t;
    uint32_t l;
    const uint32_t steps = (n > 0u) ? n - 1u : 0u;
    uint32_t d;
    uint32_t kd;
    uint32_t f;
    uint32_t nh;
    uint32_t nkv;
    uint32_t hd;
    uint32_t ctx;
    uint32_t vocab;
    size_t nw;
    float *base;
    float *grad = NULL;
    float *x_in = NULL;
    float *att_norm = NULL;
    float *q = NULL;
    float *k = NULL;
    float *v = NULL;
    float *att_ctx = NULL;
    float *x1 = NULL;
    float *ffn_norm = NULL;
    float *gate = NULL;
    float *up = NULL;
    float *sw = NULL;
    float *probs = NULL;
    float *xcur = NULL;
    float *final_x = NULL;
    float *final_norm = NULL;
    float *logits = NULL;
    float *dlogits = NULL;
    float *dx = NULL;
    float *d_x1 = NULL;
    float *d_xin = NULL;
    float *d_ctx = NULL;
    float *d_q = NULL;
    float *d_k = NULL;
    float *d_v = NULL;
    float *d_a = NULL;
    float *d_b = NULL;
    float *d_gate = NULL;
    float *d_up = NULL;
    float *d_sw = NULL;
    float *tmp_d = NULL;
    float *dp = NULL;
    float loss = 0.0f;
    float inv_steps;
    int failed = 0;

    if (!m || !opt || !tokens) return NAN;
    if (n < 2u) return 0.0f;
    if (steps > m->cfg.ctx_len) return NAN;
    if (opt->n_weights != niyah_param_count(m)) return NAN;
    for (t = 0u; t < n; ++t) {
        if (tokens[t] >= m->cfg.vocab_size) return NAN;
    }

    d = m->cfg.embed_dim;
    kd = m->kv_dim;
    f = m->ffn_dim;
    nh = m->cfg.n_heads;
    nkv = m->cfg.n_kv_heads;
    hd = m->head_dim;
    ctx = m->cfg.ctx_len;
    vocab = m->cfg.vocab_size;
    nw = niyah_param_count(m);
    base = (float *)m->_pool;
    inv_steps = 1.0f / (float)steps;

#define ALLOC_FLOATS(name, count) \
    do { \
        name = train_calloc((count), sizeof(float)); \
        if (!(name)) failed = 1; \
    } while (0)

    ALLOC_FLOATS(grad, nw);
    ALLOC_FLOATS(x_in, (size_t)m->cfg.n_layers * d);
    ALLOC_FLOATS(att_norm, (size_t)m->cfg.n_layers * d);
    ALLOC_FLOATS(q, (size_t)m->cfg.n_layers * d);
    ALLOC_FLOATS(k, (size_t)m->cfg.n_layers * kd);
    ALLOC_FLOATS(v, (size_t)m->cfg.n_layers * kd);
    ALLOC_FLOATS(att_ctx, (size_t)m->cfg.n_layers * d);
    ALLOC_FLOATS(x1, (size_t)m->cfg.n_layers * d);
    ALLOC_FLOATS(ffn_norm, (size_t)m->cfg.n_layers * d);
    ALLOC_FLOATS(gate, (size_t)m->cfg.n_layers * f);
    ALLOC_FLOATS(up, (size_t)m->cfg.n_layers * f);
    ALLOC_FLOATS(sw, (size_t)m->cfg.n_layers * f);
    ALLOC_FLOATS(probs, (size_t)m->cfg.n_layers * nh * ctx);
    ALLOC_FLOATS(xcur, d);
    ALLOC_FLOATS(final_x, d);
    ALLOC_FLOATS(final_norm, d);
    ALLOC_FLOATS(logits, vocab);
    ALLOC_FLOATS(dlogits, vocab);
    ALLOC_FLOATS(dx, d);
    ALLOC_FLOATS(d_x1, d);
    ALLOC_FLOATS(d_xin, d);
    ALLOC_FLOATS(d_ctx, d);
    ALLOC_FLOATS(d_q, d);
    ALLOC_FLOATS(d_k, kd);
    ALLOC_FLOATS(d_v, kd);
    ALLOC_FLOATS(d_a, d);
    ALLOC_FLOATS(d_b, d);
    ALLOC_FLOATS(d_gate, f);
    ALLOC_FLOATS(d_up, f);
    ALLOC_FLOATS(d_sw, f);
    ALLOC_FLOATS(tmp_d, d);
    ALLOC_FLOATS(dp, ctx);

#undef ALLOC_FLOATS

    if (failed) goto cleanup;

    {
        const size_t kv_each = (size_t)m->cfg.n_layers * nkv * ctx * hd;
        memset(m->kv_k, 0, kv_each * sizeof(float));
        memset(m->kv_v, 0, kv_each * sizeof(float));
    }

    for (t = 0u; t < steps; ++t) {
        const uint32_t token = tokens[t];
        const uint32_t target = tokens[t + 1u];
        const float inv_sqrt_hd = 1.0f / sqrtf((float)hd);
        float max_logit;
        float exp_sum = 0.0f;
        float logsumexp;
        uint32_t i;

        memcpy(xcur, m->token_embed + (size_t)token * d, d * sizeof(float));
        memset(probs, 0, (size_t)m->cfg.n_layers * nh * ctx * sizeof(float));

        for (l = 0u; l < m->cfg.n_layers; ++l) {
            NiyahLayer *layer = &m->layers[l];
            float *lx = x_in + (size_t)l * d;
            float *la = att_norm + (size_t)l * d;
            float *lq = q + (size_t)l * d;
            float *lk = k + (size_t)l * kd;
            float *lv = v + (size_t)l * kd;
            float *lc = att_ctx + (size_t)l * d;
            float *lx1 = x1 + (size_t)l * d;
            float *lb = ffn_norm + (size_t)l * d;
            float *lg = gate + (size_t)l * f;
            float *lu = up + (size_t)l * f;
            float *lsw = sw + (size_t)l * f;
            float *layer_probs = probs + (size_t)l * nh * ctx;
            const size_t layer_stride = (size_t)nkv * ctx * hd;
            float *kc = m->kv_k + (size_t)l * layer_stride;
            float *vc = m->kv_v + (size_t)l * layer_stride;
            uint32_t h;

            memcpy(lx, xcur, d * sizeof(float));
            train_rmsnorm(la, lx, layer->rms_att, d, m->cfg.rms_eps);
            train_matvec(lq, layer->wq, la, d, d);
            train_matvec(lk, layer->wk, la, kd, d);
            train_matvec(lv, layer->wv, la, kd, d);

            for (h = 0u; h < nh; ++h) train_rope(lq + h * hd, t, hd, m->cfg.rope_theta);
            for (h = 0u; h < nkv; ++h) train_rope(lk + h * hd, t, hd, m->cfg.rope_theta);

            for (h = 0u; h < nkv; ++h) {
                memcpy(kc + (size_t)h * ctx * hd + (size_t)t * hd,
                       lk + h * hd, hd * sizeof(float));
                memcpy(vc + (size_t)h * ctx * hd + (size_t)t * hd,
                       lv + h * hd, hd * sizeof(float));
            }

            memset(lc, 0, d * sizeof(float));
            for (h = 0u; h < nh; ++h) {
                const uint32_t kvh = (h * nkv) / nh;
                const float *qh = lq + h * hd;
                float *ph = layer_probs + (size_t)h * ctx;
                float max_score = -INFINITY;
                float den = 0.0f;
                uint32_t s;

                for (s = 0u; s <= t; ++s) {
                    const float *ks = kc + (size_t)kvh * ctx * hd + (size_t)s * hd;
                    const float score = train_dot(qh, ks, hd) * inv_sqrt_hd;
                    ph[s] = score;
                    if (score > max_score) max_score = score;
                }
                for (s = 0u; s <= t; ++s) {
                    ph[s] = expf(ph[s] - max_score);
                    den += ph[s];
                }
                if (!(den > 0.0f) || !isfinite(den)) {
                    failed = 1;
                    goto cleanup;
                }
                for (s = 0u; s <= t; ++s) {
                    const float p = ph[s] / den;
                    const float *vs = vc + (size_t)kvh * ctx * hd + (size_t)s * hd;
                    uint32_t j;
                    ph[s] = p;
                    for (j = 0u; j < hd; ++j) lc[h * hd + j] += p * vs[j];
                }
            }

            train_matvec(tmp_d, layer->wo, lc, d, d);
            for (i = 0u; i < d; ++i) lx1[i] = lx[i] + tmp_d[i];

            train_rmsnorm(lb, lx1, layer->rms_ffn, d, m->cfg.rms_eps);
            train_matvec(lg, layer->w_gate, lb, f, d);
            train_matvec(lu, layer->w_up, lb, f, d);
            for (i = 0u; i < f; ++i) lsw[i] = train_silu(lg[i]) * lu[i];
            train_matvec(tmp_d, layer->w_down, lsw, d, f);
            for (i = 0u; i < d; ++i) xcur[i] = lx1[i] + tmp_d[i];
        }

        memcpy(final_x, xcur, d * sizeof(float));
        train_rmsnorm(final_norm, final_x, m->rms_final, d, m->cfg.rms_eps);
        train_matvec(logits, m->lm_head, final_norm, vocab, d);

        max_logit = logits[0];
        for (i = 1u; i < vocab; ++i) if (logits[i] > max_logit) max_logit = logits[i];
        for (i = 0u; i < vocab; ++i) exp_sum += expf(logits[i] - max_logit);
        if (!(exp_sum > 0.0f) || !isfinite(exp_sum)) {
            failed = 1;
            goto cleanup;
        }
        logsumexp = logf(exp_sum) + max_logit;
        loss += (logsumexp - logits[target]) * inv_steps;

        for (i = 0u; i < vocab; ++i) {
            dlogits[i] = expf(logits[i] - logsumexp) * inv_steps;
        }
        dlogits[target] -= inv_steps;

        {
            float *grad_lm = train_grad_ptr(grad, base, m->lm_head);
            float *grad_final_rms = train_grad_ptr(grad, base, m->rms_final);
            memset(dx, 0, d * sizeof(float));
            train_outer_add(grad_lm, dlogits, final_norm, vocab, d);
            train_matvec_t_add(dx, m->lm_head, dlogits, vocab, d);
            memset(tmp_d, 0, d * sizeof(float));
            train_rmsnorm_backward(tmp_d, grad_final_rms, final_x, m->rms_final,
                                   dx, d, m->cfg.rms_eps);
            memcpy(dx, tmp_d, d * sizeof(float));
        }

        for (l = m->cfg.n_layers; l-- > 0u;) {
            NiyahLayer *layer = &m->layers[l];
            const float *lx = x_in + (size_t)l * d;
            const float *la = att_norm + (size_t)l * d;
            const float *lq = q + (size_t)l * d;
            const float *lc = att_ctx + (size_t)l * d;
            const float *lx1 = x1 + (size_t)l * d;
            const float *lb = ffn_norm + (size_t)l * d;
            const float *lg = gate + (size_t)l * f;
            const float *lu = up + (size_t)l * f;
            const float *lsw = sw + (size_t)l * f;
            const float *layer_probs = probs + (size_t)l * nh * ctx;
            const size_t layer_stride = (size_t)nkv * ctx * hd;
            const float *kc = m->kv_k + (size_t)l * layer_stride;
            const float *vc = m->kv_v + (size_t)l * layer_stride;
            float *g_wq = train_grad_ptr(grad, base, layer->wq);
            float *g_wk = train_grad_ptr(grad, base, layer->wk);
            float *g_wv = train_grad_ptr(grad, base, layer->wv);
            float *g_wo = train_grad_ptr(grad, base, layer->wo);
            float *g_gate = train_grad_ptr(grad, base, layer->w_gate);
            float *g_up = train_grad_ptr(grad, base, layer->w_up);
            float *g_down = train_grad_ptr(grad, base, layer->w_down);
            float *g_rms_att = train_grad_ptr(grad, base, layer->rms_att);
            float *g_rms_ffn = train_grad_ptr(grad, base, layer->rms_ffn);
            uint32_t h;

            memcpy(d_x1, dx, d * sizeof(float));
            train_outer_add(g_down, dx, lsw, d, f);
            memset(d_sw, 0, f * sizeof(float));
            train_matvec_t_add(d_sw, layer->w_down, dx, d, f);

            for (i = 0u; i < f; ++i) {
                d_gate[i] = d_sw[i] * lu[i] * train_silu_grad(lg[i]);
                d_up[i] = d_sw[i] * train_silu(lg[i]);
            }
            train_outer_add(g_gate, d_gate, lb, f, d);
            train_outer_add(g_up, d_up, lb, f, d);
            memset(d_b, 0, d * sizeof(float));
            train_matvec_t_add(d_b, layer->w_gate, d_gate, f, d);
            train_matvec_t_add(d_b, layer->w_up, d_up, f, d);
            memset(tmp_d, 0, d * sizeof(float));
            train_rmsnorm_backward(tmp_d, g_rms_ffn, lx1, layer->rms_ffn,
                                   d_b, d, m->cfg.rms_eps);
            for (i = 0u; i < d; ++i) d_x1[i] += tmp_d[i];

            memcpy(d_xin, d_x1, d * sizeof(float));
            train_outer_add(g_wo, d_x1, lc, d, d);
            memset(d_ctx, 0, d * sizeof(float));
            train_matvec_t_add(d_ctx, layer->wo, d_x1, d, d);

            memset(d_q, 0, d * sizeof(float));
            memset(d_k, 0, kd * sizeof(float));
            memset(d_v, 0, kd * sizeof(float));

            for (h = 0u; h < nh; ++h) {
                const uint32_t kvh = (h * nkv) / nh;
                const float *qh = lq + h * hd;
                const float *ph = layer_probs + (size_t)h * ctx;
                const float *dch = d_ctx + h * hd;
                float weighted = 0.0f;
                uint32_t s;

                for (s = 0u; s <= t; ++s) {
                    const float *vs = vc + (size_t)kvh * ctx * hd + (size_t)s * hd;
                    dp[s] = train_dot(dch, vs, hd);
                    weighted += ph[s] * dp[s];
                }
                for (s = 0u; s <= t; ++s) {
                    const float ds = ph[s] * (dp[s] - weighted) * inv_sqrt_hd;
                    const float *ks = kc + (size_t)kvh * ctx * hd + (size_t)s * hd;
                    uint32_t j;
                    for (j = 0u; j < hd; ++j) d_q[h * hd + j] += ds * ks[j];
                    if (s == t) {
                        for (j = 0u; j < hd; ++j) d_k[kvh * hd + j] += ds * qh[j];
                    }
                }
                for (i = 0u; i < hd; ++i) {
                    d_v[kvh * hd + i] += ph[t] * dch[i];
                }
            }

            for (h = 0u; h < nh; ++h) {
                train_rope_backward(d_q + h * hd, t, hd, m->cfg.rope_theta);
            }
            for (h = 0u; h < nkv; ++h) {
                train_rope_backward(d_k + h * hd, t, hd, m->cfg.rope_theta);
            }

            train_outer_add(g_wq, d_q, la, d, d);
            train_outer_add(g_wk, d_k, la, kd, d);
            train_outer_add(g_wv, d_v, la, kd, d);
            memset(d_a, 0, d * sizeof(float));
            train_matvec_t_add(d_a, layer->wq, d_q, d, d);
            train_matvec_t_add(d_a, layer->wk, d_k, kd, d);
            train_matvec_t_add(d_a, layer->wv, d_v, kd, d);
            memset(tmp_d, 0, d * sizeof(float));
            train_rmsnorm_backward(tmp_d, g_rms_att, lx, layer->rms_att,
                                   d_a, d, m->cfg.rms_eps);
            for (i = 0u; i < d; ++i) d_xin[i] += tmp_d[i];
            memcpy(dx, d_xin, d * sizeof(float));
        }

        {
            float *g_embed = train_grad_ptr(grad, base, m->token_embed)
                           + (size_t)token * d;
            for (i = 0u; i < d; ++i) g_embed[i] += dx[i];
        }
    }

    if (!isfinite(loss)) {
        failed = 1;
        goto cleanup;
    }

    {
        double sumsq = 0.0;
        float clip = 1.0f;
        float bc1;
        float bc2;
        size_t i;

        for (i = 0u; i < nw; ++i) sumsq += (double)grad[i] * (double)grad[i];
        if (!isfinite(sumsq)) {
            failed = 1;
            goto cleanup;
        }
        if (sumsq > 1.0) clip = (float)(1.0 / sqrt(sumsq));

        ++opt->step;
        bc1 = 1.0f - powf(opt->beta1, (float)opt->step);
        bc2 = 1.0f - powf(opt->beta2, (float)opt->step);
        if (!(bc1 > 0.0f) || !(bc2 > 0.0f)) {
            failed = 1;
            goto cleanup;
        }

        for (i = 0u; i < nw; ++i) {
            const float g = grad[i] * clip + opt->wd * base[i];
            float mh;
            float vh;
            opt->m[i] = opt->beta1 * opt->m[i] + (1.0f - opt->beta1) * g;
            opt->v[i] = opt->beta2 * opt->v[i] + (1.0f - opt->beta2) * g * g;
            mh = opt->m[i] / bc1;
            vh = opt->v[i] / bc2;
            if (!isfinite(mh) || !isfinite(vh)) {
                failed = 1;
                goto cleanup;
            }
            base[i] -= opt->lr * mh / (sqrtf(vh) + opt->eps);
        }
    }

cleanup:
    free(grad);
    free(x_in);
    free(att_norm);
    free(q);
    free(k);
    free(v);
    free(att_ctx);
    free(x1);
    free(ffn_norm);
    free(gate);
    free(up);
    free(sw);
    free(probs);
    free(xcur);
    free(final_x);
    free(final_norm);
    free(logits);
    free(dlogits);
    free(dx);
    free(d_x1);
    free(d_xin);
    free(d_ctx);
    free(d_q);
    free(d_k);
    free(d_v);
    free(d_a);
    free(d_b);
    free(d_gate);
    free(d_up);
    free(d_sw);
    free(tmp_d);
    free(dp);

    return failed ? NAN : loss;
}
