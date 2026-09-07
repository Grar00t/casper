/* NIYAH engine self-check driver. */
#include "niyah_core.h"

#include <math.h>
#include <stdio.h>

static int check(int ok, const char *label) {
    if (!ok) (void)fprintf(stderr, "[niyah] FAIL %s\n", label);
    return ok ? 0 : 1;
}

int main(void) {
    int failed = 0;

    failed += check(sizeof(NiyahConfig) == 64u, "sizeof(NiyahConfig) == 64");
    failed += check(niyah_alloc(NULL) == NULL, "alloc rejects a NULL config");
    failed += check(niyah_param_count(NULL) == 0u, "param_count(NULL) == 0");

    const NiyahConfig cfg = {
        .magic = NIYAH_MAGIC, .version = NIYAH_VER,
        .embed_dim = 32u, .n_heads = 4u, .n_kv_heads = 2u,
        .n_layers = 2u, .ffn_mult = 2u, .vocab_size = 64u,
        .ctx_len = 8u, .rope_theta = 10000.0f, .rms_eps = 1e-5f,
        .flags = 0u
    };

    NiyahConfig bad = cfg;
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

    NiyahModel *m = niyah_alloc(&cfg);
    NiyahModel *m2 = niyah_alloc(&cfg);
    if (!m || !m2) {
        (void)fputs("[niyah] FAIL alloc rejected a valid config\n", stderr);
        niyah_free(m);
        niyah_free(m2);
        return 1;
    }

    failed += check(niyah_param_count(m) > 0u, "param_count > 0");
    failed += check(m->head_dim == cfg.embed_dim / cfg.n_heads, "head_dim");
    failed += check(m->kv_dim == cfg.n_kv_heads * m->head_dim, "kv_dim");
    failed += check(m->ffn_dim == cfg.embed_dim * cfg.ffn_mult, "ffn_dim");

    niyah_init_weights(m, UINT64_C(1234567));
    niyah_init_weights(m2, UINT64_C(1234567));
    {
        const float *a = (const float *)m->_pool;
        const float *b = (const float *)m2->_pool;
        size_t nw = niyah_param_count(m);
        int same = 1;
        int nonzero = 0;
        for (size_t i = 0u; i < nw; ++i) {
            if (a[i] != b[i]) same = 0;
            if (a[i] != 0.0f) nonzero = 1;
        }
        failed += check(same, "weight initialization is deterministic");
        failed += check(nonzero, "weight initialization is nonzero");
    }

    failed += check(niyah_forward(NULL, 0u, 0u) == NULL, "forward rejects a NULL model");
    failed += check(niyah_forward(m, cfg.vocab_size, 0u) == NULL, "forward rejects token >= vocab_size");
    failed += check(niyah_forward(m, 0u, cfg.ctx_len) == NULL, "forward rejects pos >= ctx_len");

    const float *logits = niyah_forward(m, 1u, 0u);
    failed += check(logits != NULL, "forward returns logits");
    if (logits) {
        int finite = 1;
        for (uint32_t i = 0u; i < cfg.vocab_size; ++i) {
            if (!isfinite(logits[i])) {
                finite = 0;
                break;
            }
        }
        failed += check(finite, "every logit is finite");
    }

    {
        float probe[4] = {0.5f, 2.5f, -1.0f, 1.0f};
        NiyahSampler greedy = {.temperature = 0.0f, .top_p = 1.0f, .seed = 1u};
        NiyahSampler warm = {.temperature = 0.8f, .top_p = 0.9f, .seed = 42u};
        failed += check(niyah_sample(probe, 4u, &greedy) == 1u, "temperature 0 is argmax");
        failed += check(niyah_sample(probe, 4u, &warm) < 4u, "sample stays in range");
        failed += check(niyah_sample(probe, 0u, &warm) == 0u, "vocab_size 0 is rejected");
        failed += check(niyah_sample(NULL, 4u, &warm) == 0u, "NULL logits are rejected");
    }

    {
        NiyahAdam *opt = niyah_adam_alloc(m);
        failed += check(opt != NULL, "adam alloc");
        if (opt) {
            /* token 20 is deliberately >= ctx_len (8) but < vocab_size (64).
             * Token ids and sequence positions are different domains. */
            const uint32_t toks[4] = {1u, 20u, 3u, 4u};
            float before = m->lm_head[0];
            float loss = niyah_train_step(m, opt, toks, 4u);
            failed += check(isfinite(loss) && loss >= 0.0f, "train_step accepts valid token ids above ctx_len");
            failed += check(opt->step == 1u, "adam step counter advances");
            failed += check(m->lm_head[0] != before, "head-only train step updates lm_head");
            failed += check(niyah_train_step(m, opt, toks, 1u) == 0.0f, "train_step rejects n < 2");
            {
                const uint32_t invalid[2] = {1u, cfg.vocab_size};
                failed += check(isnan(niyah_train_step(m, opt, invalid, 2u)), "train_step rejects token >= vocab_size");
            }
            {
                const uint32_t too_long[10] = {0u};
                failed += check(isnan(niyah_train_step(m, opt, too_long, 10u)), "train_step rejects sequence beyond context");
            }
            niyah_adam_free(opt);
        }
    }

    {
        const char *path = "niyah_selfcheck.bin";
        size_t nw = niyah_param_count(m);
        if (check(niyah_save(m, path) == 0, "save writes the model") == 0) {
            NiyahModel *loaded = NULL;
            failed += check(niyah_load(&loaded, path) == 0 && loaded != NULL, "load reads it back");
            if (loaded) {
                failed += check(loaded->cfg.embed_dim == cfg.embed_dim &&
                                loaded->cfg.n_layers == cfg.n_layers &&
                                loaded->cfg.vocab_size == cfg.vocab_size,
                                "round trip preserves the config");
                {
                    const float *a = (const float *)m->_pool;
                    const float *b = (const float *)loaded->_pool;
                    int same = 1;
                    for (size_t i = 0u; i < nw; ++i) {
                        if (a[i] != b[i]) {
                            same = 0;
                            break;
                        }
                    }
                    failed += check(same, "round trip preserves the weights");
                }
                niyah_free(loaded);
            }
            (void)remove(path);
        } else {
            failed += 1;
        }
    }

    niyah_free(m2);
    niyah_free(m);

    if (failed == 0) {
        (void)printf("NIYAH SELF-CHECK PASS  simd=%s\n", niyah_simd_name());
        return 0;
    }
    (void)printf("NIYAH SELF-CHECK FAIL  %d checks\n", failed);
    return 1;
}
