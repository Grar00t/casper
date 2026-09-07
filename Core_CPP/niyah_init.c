/* Deterministic weight initialization for Casper/NIYAH. */
#include "niyah_core.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>

static uint64_t splitmix64(uint64_t *state)
{
    uint64_t z;
    *state += UINT64_C(0x9E3779B97F4A7C15);
    z = *state;
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

static float signed_unit(uint64_t *state)
{
    /* 24 random bits fit exactly in IEEE-754 float mantissa. */
    const uint32_t bits = (uint32_t)(splitmix64(state) >> 40);
    const float u = (float)bits * (1.0f / 16777216.0f);
    return 2.0f * u - 1.0f;
}

static void init_matrix(float *dst, size_t rows, size_t cols, uint64_t *state)
{
    size_t i;
    const float denom = (float)(rows + cols);
    const float limit = denom > 0.0f ? sqrtf(6.0f / denom) : 0.0f;
    for (i = 0u; i < rows * cols; ++i) dst[i] = signed_unit(state) * limit;
}

void niyah_init_weights(NiyahModel *m, uint64_t seed)
{
    uint32_t l;
    uint64_t state;
    size_t i;

    if (!m || !m->_pool) return;
    state = seed != 0u ? seed : UINT64_C(0x4E49594148434153);

    for (l = 0u; l < m->cfg.n_layers; ++l) {
        NiyahLayer *w = &m->layers[l];
        const size_t d = m->cfg.embed_dim;
        const size_t kd = m->kv_dim;
        const size_t f = m->ffn_dim;

        init_matrix(w->wq, d, d, &state);
        init_matrix(w->wk, kd, d, &state);
        init_matrix(w->wv, kd, d, &state);
        init_matrix(w->wo, d, d, &state);
        init_matrix(w->w_gate, f, d, &state);
        init_matrix(w->w_up, f, d, &state);
        init_matrix(w->w_down, d, f, &state);
        for (i = 0u; i < d; ++i) {
            w->rms_att[i] = 1.0f;
            w->rms_ffn[i] = 1.0f;
        }
    }

    init_matrix(m->token_embed, m->cfg.vocab_size, m->cfg.embed_dim, &state);
    for (i = 0u; i < m->cfg.embed_dim; ++i) m->rms_final[i] = 1.0f;
    init_matrix(m->lm_head, m->cfg.vocab_size, m->cfg.embed_dim, &state);
}
