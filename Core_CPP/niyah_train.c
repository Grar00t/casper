#include "niyah_core.h"
#include "tokenizer.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static FILE *open_training_data_path(const char *path) {
    if (path && path[0]) {
        FILE *f = fopen(path, "r");
        if (f) return f;
    }
    const char *candidates[] = {
        "Data_Training/sovereign_knowledge.txt",
        "sovereign_knowledge_data.txt",
        "sovereign_knowledge.txt"
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        FILE *f = fopen(candidates[i], "r");
        if (f) return f;
    }
    return NULL;
}

static int parse_int(const char *s, int fallback) {
    if (!s || !s[0]) return fallback;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || v <= 0 || v > INT32_MAX) return fallback;
    return (int)v;
}

static float parse_float(const char *s, float fallback) {
    if (!s || !s[0]) return fallback;
    char *end = NULL;
    float v = strtof(s, &end);
    if (end == s || *end != '\0' || !isfinite(v) || v <= 0.0f) return fallback;
    return v;
}

static float cosine_lr(float base, float min_lr, uint32_t step, uint32_t total, uint32_t warmup) {
    if (step < warmup) {
        float t = (float)step / (float)(warmup ? warmup : 1u);
        return min_lr + (base - min_lr) * t;
    }
    uint32_t denom = total > warmup ? total - warmup : 1u;
    float p = (float)(step - warmup) / (float)denom;
    if (p > 1.0f) p = 1.0f;
    return min_lr + (base - min_lr) * 0.5f * (1.0f + cosf(3.14159265f * p));
}

int main(int argc, char **argv) {
    tokenizer_init();
    const uint32_t live_vocab = tokenizer_vocab_size();
    if (live_vocab == 0u || live_vocab > NIYAH_MAX_VOCAB) {
        fputs("[NIYAH] invalid tokenizer vocabulary\n", stderr);
        tokenizer_free();
        return 1;
    }

    NiyahConfig cfg = {
        .magic = NIYAH_MAGIC,
        .version = NIYAH_VER,
        .vocab_size = live_vocab,
        .ctx_len = 256,
        .embed_dim = 128,
        .n_layers = 4,
        .n_heads = 8,
        .n_kv_heads = 4,
        .ffn_mult = 4,
        .rope_theta = 10000.0f,
        .rms_eps = 1e-5f,
        .flags = 0
    };

    const char *data_path = argc > 1 ? argv[1] : NULL;
    int epochs = argc > 2 ? parse_int(argv[2], 5) : 5;
    float base_lr = argc > 3 ? parse_float(argv[3], 3e-4f) : 3e-4f;
    float min_lr = argc > 4 ? parse_float(argv[4], 3e-5f) : 3e-5f;
    if (min_lr > base_lr) min_lr = base_lr * 0.1f;

    NiyahModel *model = niyah_alloc(&cfg);
    if (!model) {
        fputs("[NIYAH] alloc failed\n", stderr);
        tokenizer_free();
        return 1;
    }
    niyah_init_weights(model, UINT64_C(0x4341535045521337));

    NiyahAdam *opt = niyah_adam_alloc(model);
    if (!opt) {
        fputs("[NIYAH] adam alloc failed\n", stderr);
        niyah_free(model);
        tokenizer_free();
        return 1;
    }
    opt->lr = base_lr;
    opt->beta1 = 0.9f;
    opt->beta2 = 0.999f;
    opt->eps = 1e-8f;
    opt->wd = 0.01f;

    FILE *data = open_training_data_path(data_path);
    if (!data) {
        fputs("[NIYAH] no data file found\n", stderr);
        niyah_adam_free(opt);
        niyah_free(model);
        tokenizer_free();
        return 1;
    }

    fputs("[NIYAH] compatibility trainer: lm_head only; use tools/train_casper.py for full-model training\n", stderr);

    char line[4096];
    uint32_t total_lines = 0u;
    while (fgets(line, sizeof(line), data)) {
        if (strlen(line) > 2u) ++total_lines;
    }
    rewind(data);
    if (total_lines == 0u) {
        fputs("[NIYAH] no usable lines\n", stderr);
        fclose(data);
        tokenizer_free();
        niyah_adam_free(opt);
        niyah_free(model);
        return 1;
    }

    uint64_t step_budget = (uint64_t)total_lines * (uint64_t)epochs;
    if (step_budget == 0u || step_budget > UINT32_MAX) {
        fputs("[NIYAH] invalid training step budget\n", stderr);
        fclose(data);
        tokenizer_free();
        niyah_adam_free(opt);
        niyah_free(model);
        return 1;
    }
    uint32_t total_steps = (uint32_t)step_budget;
    uint32_t warmup_steps = total_steps / 20u;
    if (warmup_steps < 100u && total_steps > 100u) warmup_steps = 100u;
    if (warmup_steps >= total_steps && total_steps > 1u) warmup_steps = total_steps - 1u;

    float ema = 0.0f;
    float best_ema = INFINITY;
    int bad_windows = 0;
    uint32_t global_step = 0u;
    clock_t t0 = clock();
    int rc = 0;

    for (int ep = 0; ep < epochs; ++ep) {
        rewind(data);
        float loss_sum = 0.0f;
        uint32_t steps = 0u;

        while (fgets(line, sizeof(line), data)) {
            uint32_t tokens[1024];
            uint32_t n = tokenizer_encode(line, tokens, cfg.ctx_len + 1u);
            if (n < 2u) continue;

            opt->lr = cosine_lr(base_lr, min_lr, global_step, total_steps, warmup_steps);
            float loss = niyah_train_step(model, opt, tokens, n);
            if (!isfinite(loss)) {
                fputs("[NIYAH] non-finite loss\n", stderr);
                rc = 1;
                goto cleanup;
            }

            loss_sum += loss;
            ++steps;
            ++global_step;
            ema = (ema <= 0.0f) ? loss : 0.995f * ema + 0.005f * loss;

            if (steps % 500u == 0u) {
                printf("ep%d step%u loss=%.4f ema=%.4f lr=%.2e\n",
                       ep + 1, steps, (double)(loss_sum / (float)steps),
                       (double)ema, (double)opt->lr);
                fflush(stdout);
            }

            if (steps % 2000u == 0u) {
                if (ema < best_ema - 1e-3f) {
                    best_ema = ema;
                    bad_windows = 0;
                } else if (++bad_windows >= 6) {
                    goto cleanup;
                }
            }
        }
    }

cleanup:
    fclose(data);
    tokenizer_free();
    if (rc == 0 && niyah_save(model, "niyah_trained.bin") != 0) {
        fputs("[NIYAH] model save failed\n", stderr);
        rc = 1;
    }
    printf("training_elapsed_s=%.3f\n", (double)(clock() - t0) / (double)CLOCKS_PER_SEC);
    niyah_adam_free(opt);
    niyah_free(model);
    return rc;
}
