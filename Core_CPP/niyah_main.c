/*
 * niyah_main.c — NIYAH engine self-check driver.
 *
 * Exit code: 0 when every check passes, 1 otherwise.
 */
#include "niyah_core.h"
#include "niyah_train_full.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int check(int ok, const char *label)
{
    if (!ok) (void)fprintf(stderr, "[niyah] FAIL %s\n", label);
    return ok ? 0 : 1;
}

static int any_float_changed(const float *before, const float *after, size_t n)
{
    size_t i;
    for (i = 0u; i < n; ++i) {
        if (before[i] != after[i]) return 1;
    }
    return 0;
}

int main(void)
{
    int failed = 0;

    failed += check(sizeof(NiyahConfig) == 64u, "sizeof(NiyahConfig) == 64");
    failed += check(niyah_alloc(NULL) == NULL, "alloc rejects a NULL config");
    failed += check(niyah_param_count(NULL) == 0u, "param_count(NULL) == 0");

    {
        const NiyahConfig cfg = {
            .magic = NIYAH_MAGIC, .version = NIYAH_VER,
            .embed_dim = 32u, .n_heads = 4u, .n_kv_heads = 2u,
            .n_layers = 2u, .ffn_mult = 2u, .vocab_size = 64u,
            .ctx_len = 8u, .rope_theta = 10000.0f, .rms_eps = 1e-5f,
            .flags = 0u
        };
        NiyahConfig bad = cfg;
        NiyahModel *m;

        bad.n_kv_heads = cfg.n_heads + 1u;
        failed += check(niyah_alloc(&bad) == NULL, "alloc rejects kv_heads > heads");
        bad = cfg;
        bad.embed_dim = 30u;
        failed += check(niyah_alloc(&bad) == NULL, "alloc rejects embed_dim % n_heads");
        bad = cfg;
        bad.ctx_len = NIYAH_MAX_CTX + 1u;
        failed += check(niyah_alloc(&bad) == NULL, "alloc rejects ctx_len > NIYAH_MAX_CTX");
        bad = cfg;
        bad.vocab_size = 0u;
        failed += check(niyah_alloc(&bad) == NULL, "alloc rejects vocab_size 0");

        m = niyah_alloc(&cfg);
        if (!m) {
            (void)fputs("[niyah] FAIL alloc rejected a valid config\n", stderr);
            return 1;
        }

        failed += check(niyah_param_count(m) > 0u, "param_count > 0");
        failed += check(m->head_dim == cfg.embed_dim / cfg.n_heads, "head_dim");
        failed += check(m->kv_dim == cfg.n_kv_heads * m->head_dim, "kv_dim");
        failed += check(m->ffn_dim == cfg.embed_dim * cfg.ffn_mult, "ffn_dim");

        {
            float *w = (float *)m->_pool;
            size_t nw = niyah_param_count(m);
            size_t i;
            uint32_t l;
            for (i = 0u; i < nw; ++i) w[i] = ((float)(i % 37u) - 18.0f) * 0.005f;
            for (l = 0u; l < cfg.n_layers; ++l) {
                uint32_t j;
                for (j = 0u; j < cfg.embed_dim; ++j) {
                    m->layers[l].rms_att[j] = 1.0f;
                    m->layers[l].rms_ffn[j] = 1.0f;
                }
            }
            for (i = 0u; i < cfg.embed_dim; ++i) m->rms_final[i] = 1.0f;
        }

        failed += check(niyah_forward(NULL, 0u, 0u) == NULL, "forward rejects a NULL model");
        failed += check(niyah_forward(m, cfg.vocab_size, 0u) == NULL,
                        "forward rejects token >= vocab_size");
        failed += check(niyah_forward(m, 0u, cfg.ctx_len) == NULL,
                        "forward rejects pos >= ctx_len");

        {
            const float *logits = niyah_forward(m, 1u, 0u);
            failed += check(logits != NULL, "forward returns logits");
            if (logits) {
                int finite = 1;
                uint32_t i;
                for (i = 0u; i < cfg.vocab_size; ++i) {
                    if (!isfinite(logits[i])) {
                        finite = 0;
                        break;
                    }
                }
                failed += check(finite, "every logit is finite");
            }
        }

        {
            float probe[4] = { 0.5f, 2.5f, -1.0f, 1.0f };
            NiyahSampler greedy = { .temperature = 0.0f, .top_p = 1.0f, .seed = 1u };
            NiyahSampler warm = { .temperature = 0.8f, .top_p = 0.9f, .seed = 42u };
            failed += check(niyah_sample(probe, 4u, &greedy) == 1u,
                            "temperature 0 is argmax");
            failed += check(niyah_sample(probe, 4u, &warm) < 4u,
                            "sample stays in range");
            failed += check(niyah_sample(probe, 0u, &warm) == 0u,
                            "vocab_size 0 is rejected");
            failed += check(niyah_sample(NULL, 4u, &warm) == 0u,
                            "NULL logits are rejected");
        }

        {
            NiyahAdam *opt = niyah_adam_alloc(m);
            failed += check(opt != NULL, "adam alloc");
            if (opt) {
                const uint32_t toks[4] = { 1u, 2u, 3u, 4u };
                float loss = niyah_train_step(m, opt, toks, 4u);
                failed += check(isfinite(loss) && loss >= 0.0f,
                                "legacy output-head train_step loss is finite");
                failed += check(opt->step == 1u, "legacy adam step counter advances");
                failed += check(niyah_train_step(m, opt, toks, 1u) == 0.0f,
                                "legacy train_step rejects n < 2");
                niyah_adam_free(opt);
            }
        }

        {
            const char *path = "niyah_selfcheck.bin";
            if (check(niyah_save(m, path) == 0, "save writes the model") == 0) {
                NiyahModel *loaded = NULL;
                failed += check(niyah_load(&loaded, path) == 0 && loaded != NULL,
                                "load reads it back");
                if (loaded) {
                    size_t nw = niyah_param_count(m);
                    const float *a = (const float *)m->_pool;
                    const float *b = (const float *)loaded->_pool;
                    int same = 1;
                    size_t i;
                    failed += check(loaded->cfg.embed_dim == cfg.embed_dim &&
                                    loaded->cfg.n_layers == cfg.n_layers &&
                                    loaded->cfg.vocab_size == cfg.vocab_size,
                                    "round trip preserves the config");
                    for (i = 0u; i < nw; ++i) {
                        if (a[i] != b[i]) {
                            same = 0;
                            break;
                        }
                    }
                    failed += check(same, "round trip preserves the weights");
                    niyah_free(loaded);
                }
                (void)remove(path);
            } else {
                failed += 1;
            }
        }

        niyah_free(m);
    }

    /*
     * Full-parameter training regression. The model starts from deterministic
     * non-zero weights, repeatedly sees one tiny sequence, must reduce loss,
     * and must change a backbone attention matrix rather than only lm_head.
     */
    {
        const NiyahConfig train_cfg = {
            .magic = NIYAH_MAGIC, .version = NIYAH_VER,
            .embed_dim = 16u, .n_heads = 4u, .n_kv_heads = 2u,
            .n_layers = 1u, .ffn_mult = 2u, .vocab_size = 16u,
            .ctx_len = 8u, .rope_theta = 10000.0f, .rms_eps = 1e-5f,
            .flags = 0u
        };
        const uint32_t seq[6] = { 1u, 2u, 1u, 2u, 1u, 2u };
        const uint32_t invalid_seq[2] = { 1u, 16u };
        NiyahModel *tm = niyah_alloc(&train_cfg);
        NiyahAdam *to = NULL;
        float *wq_before = NULL;

        failed += check(tm != NULL, "full trainer model alloc");
        if (tm) {
            const size_t wq_count = (size_t)train_cfg.embed_dim * train_cfg.embed_dim;
            float first_loss = NAN;
            float last_loss = NAN;
            int iteration;

            niyah_init_weights(tm, UINT64_C(0x434153504552));
            to = niyah_adam_alloc(tm);
            failed += check(to != NULL, "full trainer adam alloc");
            wq_before = malloc(wq_count * sizeof(float));
            failed += check(wq_before != NULL, "full trainer snapshot alloc");

            if (to && wq_before) {
                memcpy(wq_before, tm->layers[0].wq, wq_count * sizeof(float));
                to->lr = 0.01f;
                to->wd = 0.0f;

                first_loss = niyah_full_train_step(tm, to, seq, 6u);
                for (iteration = 0; iteration < 79; ++iteration) {
                    last_loss = niyah_full_train_step(tm, to, seq, 6u);
                    if (!isfinite(last_loss)) break;
                }

                failed += check(isfinite(first_loss), "full trainer initial loss finite");
                failed += check(isfinite(last_loss), "full trainer final loss finite");
                failed += check(isfinite(first_loss) && isfinite(last_loss)
                                && last_loss < first_loss,
                                "full trainer reduces overfit loss");
                failed += check(any_float_changed(wq_before, tm->layers[0].wq, wq_count),
                                "full trainer updates attention backbone");
                failed += check(to->step == 80u,
                                "full trainer adam step count");
                failed += check(isnan(niyah_full_train_step(tm, to, invalid_seq, 2u)),
                                "full trainer rejects token >= vocab_size");
            }

            free(wq_before);
            niyah_adam_free(to);
            niyah_free(tm);
        }
    }

    if (failed == 0) {
        (void)printf("NIYAH SELF-CHECK PASS  simd=%s\n", niyah_simd_name());
        return 0;
    }

    (void)printf("NIYAH SELF-CHECK FAIL  %d checks\n", failed);
    return 1;
}
