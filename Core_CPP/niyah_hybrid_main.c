/*
 * niyah_hybrid_main.c — NIYAH Hybrid Neuro-Symbolic CLI
 *
 * Separate binary from the smoke test. Provides:
 *   --model model.bin   Load a trained model
 *   --rules rules.nrule Load symbolic verification rules
 *   --interactive       Interactive prompt loop
 *   --smoke             Run all smoke tests (neural + symbolic + rules)
 *   --verify-proof f    Verify a .proof file (Task 5)
 *
 * Build:
 *   gcc -O2 -std=c11 -Wall -Wextra -Werror -Wstrict-prototypes -Wcast-align \
 *       niyah_core.c hybrid_reasoner.c constraint_solver.c rule_parser.c \
 *       proof_generator.c khz_q_svd.c niyah_hybrid_main.c ../tokenizer.c \
 *       -o niyah_hybrid -lm
 */

#include "niyah_core.h"
#include "rule_parser.h"
#include "proof_generator.h"
#include "khz_q_svd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* tokenizer.c API */
void     tokenizer_init(void);
uint32_t tokenizer_encode(const char *text, uint32_t *tokens, uint32_t max_len);
char    *tokenizer_decode(const uint32_t *tokens, uint32_t n);
void     tokenizer_free(void);

/* Symbolic smoke tests */
int niyah_sym_smoke(void);
int niyah_csp_smoke(void);
int niyah_rule_smoke(void);
int niyah_proof_smoke(void);

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * §1  Hybrid generation
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

/*
 * Autoregressive generation: run forward pass for each token,
 * sample, collect output tokens. Stop at EOS (token 1) or ctx_len.
 */
static uint32_t generate_tokens(NiyahModel *m, const uint32_t *prompt_tokens,
                                uint32_t prompt_len, uint32_t *out_tokens,
                                uint32_t max_out, NiyahSampler *sampler)
{
    uint32_t ctx = m->cfg.ctx_len;
    uint32_t pos = 0;
    uint32_t n_out = 0;

    /* Feed prompt tokens */
    for (uint32_t i = 0; i < prompt_len && pos < ctx; i++, pos++) {
        uint32_t tok = prompt_tokens[i] % m->cfg.vocab_size;
        niyah_forward(m, tok, pos);
    }

    /* Generate new tokens */
    const float *logits = NULL;
    uint32_t last_tok = prompt_tokens[prompt_len > 0 ? prompt_len - 1 : 0] % m->cfg.vocab_size;

    for (uint32_t i = 0; i < max_out && pos < ctx; i++, pos++) {
        logits = niyah_forward(m, last_tok, pos);
        uint32_t tok = niyah_sample(logits, m->cfg.vocab_size, sampler);

        if (tok == 1) break; /* EOS */
        out_tokens[n_out++] = tok;
        last_tok = tok;
    }

    return n_out;
}

char *niyah_hybrid_generate(NiyahModel *m, const char *prompt,
                            const NiyahHybridOpts *opts,
                            NiyahSampler *sampler,
                            uint8_t proof_out[32])
{
    tokenizer_init();

    /* Encode prompt */
    uint32_t prompt_tokens[512];
    uint32_t prompt_len = tokenizer_encode(prompt, prompt_tokens, 512);

    /* Clamp to vocab */
    for (uint32_t i = 0; i < prompt_len; i++)
        prompt_tokens[i] = prompt_tokens[i] % m->cfg.vocab_size;

    uint32_t max_retries = (opts && opts->max_retries > 0) ? opts->max_retries : 3;
    NiyahRuleKB *rules = (opts) ? (NiyahRuleKB *)opts->rules : NULL;

    uint32_t out_tokens[512];
    char *result = NULL;

    for (uint32_t attempt = 0; attempt <= max_retries; attempt++) {
        /* Adjust seed for retries */
        if (attempt > 0)
            sampler->seed += 12345ULL * attempt;

        uint32_t n_out = generate_tokens(m, prompt_tokens, prompt_len,
                                         out_tokens, 512, sampler);

        /* Decode to text */
        char *text = tokenizer_decode(out_tokens, n_out);
        if (!text) { text = malloc(1); text[0] = '\0'; }

        /* ── KHZ_Q Ethical Prism: SVD Verify step ─────────────────────────
         * Applied BEFORE rule_parser — mathematical coherence gate.
         * Flow: Decode → [KHZ_Q SVD Verify] → [Rule Verify] → Re-sample
         *
         * target_energy = 0.85f  (85% Fitrah-alignment threshold)
         * If energy < 85% OR penalty_nasl >= 1.0 → Re-sample.
         * ---------------------------------------------------------------- */
        KHZQ_Result khz = khz_q_verify_output(text, 0.85f);
        if (!khz.is_coherent) {
            fprintf(stderr,
                    "[KHZ_Q] Reject attempt %u/%u — "
                    "energy=%.3f penalty_nasl=%.3f chi_e=%d\n",
                    attempt + 1, max_retries + 1,
                    khz.energy_preserved, khz.penalty_nasl, khz.chi_e);
            free(text);
            continue; /* trigger re-sample */
        }
        fprintf(stderr,
                "[KHZ_Q] Coherent attempt %u — "
                "energy=%.3f penalty_nasl=%.3f chi_e=%d\n",
                attempt + 1,
                khz.energy_preserved, khz.penalty_nasl, khz.chi_e);
        /* ── end KHZ_Q ─────────────────────────────────────────────────── */

        /* If no rules, accept immediately */
        if (!rules) {
            result = text;
            break;
        }

        /* Check against symbolic rules */
        const char *violation = niyah_rule_check(rules, prompt, text);
        if (!violation) {
            result = text;
            break;
        }

        /* Rule violated */
        fprintf(stderr, "[NIYAH] Rule violation (attempt %u/%u): %s\n",
                attempt + 1, max_retries + 1, violation);

        if (attempt == max_retries) {
            /* Use replacement text if available, otherwise REJECTED */
            free(text);
            if (strcmp(violation, "REJECTED") == 0) {
                result = malloc(64);
                snprintf(result, 64, "[Output rejected by rules]");
            } else {
                result = malloc(strlen(violation) + 1);
                strcpy(result, violation);
            }
        } else {
            free(text);
        }
    }

    /* Fallback: if every attempt was rejected by KHZ_Q or rules,
     * return a safe placeholder rather than NULL so callers always
     * receive a valid (malloc'd) string. */
    if (!result) {
        result = malloc(32);
        if (result)
            snprintf(result, 32, "[Output rejected]");
    }

    /* Generate proof hash if requested */
    if (proof_out && opts && opts->generate_proof && result) {
        niyah_proof_generate(prompt, result, NULL, proof_out);
    }

    tokenizer_free();
    return result;
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * §2  Smoke test — all subsystems
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

static int run_all_smoke(void) {
    int total_fail = 0;

    /* Neural engine */
    total_fail += niyah_smoke();

    /* Symbolic reasoner */
    total_fail += niyah_sym_smoke();

    /* Constraint solver */
    total_fail += niyah_csp_smoke();

    /* Rule parser */
    total_fail += niyah_rule_smoke();

    /* Proof generator */
    total_fail += niyah_proof_smoke();

    /* KHZ_Q SVD Ethical Prism smoke test */
    {
        int pass = 0, fail = 0;

        fprintf(stderr, "\n+--------------------------------------+\n");
        fprintf(stderr, "|   KHZ_Q SVD Ethical Prism Test       |\n");
        fprintf(stderr, "+--------------------------------------+\n");

        #define KHZQ_PASS(cond, label) do { \
            if (cond) { pass++; fprintf(stderr, "  [PASS] %s\n", label); } \
            else      { fail++; fprintf(stderr, "  [FAIL] %s\n", label); } \
        } while(0)

        /* T1: coherent text */
        {
            KHZQ_Result r = khz_q_verify_output(
                "bismillah bismillah bismillah bismillah", 0.85f);
            KHZQ_PASS(r.energy_preserved >= 0.85f, "coherent text energy >= 85%");
            fprintf(stderr, "       energy=%.4f penalty=%.4f chi_e=%d\n",
                    r.energy_preserved, r.penalty_nasl, r.chi_e);
        }

        /* T2: empty text → degenerate → rejected */
        {
            KHZQ_Result r = khz_q_verify_output("", 0.85f);
            KHZQ_PASS(!r.is_coherent, "empty text rejected");
        }

        /* T3: target_energy clamping */
        {
            KHZQ_Result r = khz_q_verify_output("test", 1.5f);
            KHZQ_PASS(r.chi_e > 0 && r.chi_e <= KHZ_MAX_N,
                      "chi_e in valid range after clamp");
        }

        /* T4: Arabic UTF-8 */
        {
            KHZQ_Result r = khz_q_verify_output(
                "\xd8\xa8\xd8\xb3\xd9\x85 \xd8\xa7\xd9\x84\xd9\x84\xd9\x87",
                0.80f);
            KHZQ_PASS(r.chi_e >= 1, "Arabic UTF-8 processes without crash");
        }

        /* T5: high-entropy text gets higher penalty than low-entropy */
        {
            KHZQ_Result r_lo = khz_q_verify_output(
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 0.85f);
            KHZQ_Result r_hi = khz_q_verify_output(
                "a1!b2@c3#d4$e5%f6^g7&h8*i9(j0)k_l", 0.85f);
            KHZQ_PASS(r_hi.penalty_nasl >= r_lo.penalty_nasl,
                      "entropy increases Penalty_Nasl");
            fprintf(stderr, "       low=%.4f high=%.4f\n",
                    r_lo.penalty_nasl, r_hi.penalty_nasl);
        }

        fprintf(stderr, "\n  Results: %d passed, %d failed\n\n", pass, fail);
        total_fail += fail;

        #undef KHZQ_PASS
    }

    /* Hybrid integration test */
    {
        int pass = 0, fail = 0;

        fprintf(stderr, "\n+--------------------------------------+\n");
        fprintf(stderr, "|  NIYAH Hybrid Integration Smoke Test |\n");
        fprintf(stderr, "+--------------------------------------+\n");

        #define HYB_PASS(cond, label) do { \
            if (cond) { pass++; fprintf(stderr, "  [PASS] %s\n", label); } \
            else      { fail++; fprintf(stderr, "  [FAIL] %s\n", label); } \
        } while(0)

        /* Create a small model */
        NiyahConfig cfg = {
            .magic = NIYAH_MAGIC, .version = NIYAH_VER,
            .embed_dim = 64, .n_heads = 4, .n_kv_heads = 4,
            .n_layers = 2, .ffn_mult = 4, .vocab_size = 256,
            .ctx_len = 32, .rope_theta = 10000.f, .rms_eps = 1e-5f,
        };
        NiyahModel *m = niyah_alloc(&cfg);
        HYB_PASS(m != NULL, "alloc model for hybrid test");

        /* Init with deterministic weights */
        float *wp = (float *)m->_pool;
        size_t nw = niyah_param_count(m);
        for (size_t i = 0; i < nw; i++) wp[i] = ((float)(i % 37) - 18.f) * 0.005f;
        for (uint32_t l = 0; l < cfg.n_layers; l++) {
            for (uint32_t j = 0; j < cfg.embed_dim; j++) {
                m->layers[l].rms_att[j] = 1.f;
                m->layers[l].rms_ffn[j] = 1.f;
            }
        }
        for (uint32_t j = 0; j < cfg.embed_dim; j++) m->rms_final[j] = 1.f;

        /* Test 1: Pure generation (no rules) */
        {
            NiyahSampler s = { .temperature = 0.8f, .top_p = 0.9f, .seed = 42 };
            NiyahHybridOpts opts = { .rules = NULL, .max_retries = 0,
                                     .generate_proof = false };
            char *out = niyah_hybrid_generate(m, "hello", &opts, &s, NULL);
            HYB_PASS(out != NULL, "pure neural generation returns non-null");
            if (out) {
                fprintf(stderr, "  output: \"%s\"\n", out);
                free(out);
            }
        }

        /* Test 2: Generation with rejection rule */
        {
            const char *rule_src =
                "rule: \"IF output CONTAINS 'vaccine causes' "
                "THEN output = REJECTED\"\n";
            NiyahRuleKB *kb = niyah_rule_parse(rule_src);
            HYB_PASS(kb != NULL, "parse rejection rule");

            NiyahSampler s = { .temperature = 0.5f, .top_p = 0.9f, .seed = 100 };
            NiyahHybridOpts opts = { .rules = kb, .max_retries = 2,
                                     .generate_proof = false };
            char *out = niyah_hybrid_generate(m, "test", &opts, &s, NULL);
            HYB_PASS(out != NULL, "generation with rules returns non-null");
            if (out) free(out);
            niyah_rule_free(kb);
        }

        /* Test 3: Tokenizer encode/decode round-trip */
        {
            tokenizer_init();
            uint32_t tokens[256];
            uint32_t n = tokenizer_encode("hello world", tokens, 256);
            char *decoded = tokenizer_decode(tokens, n);
            HYB_PASS(decoded != NULL, "tokenizer decode returns non-null");
            if (decoded) {
                fprintf(stderr, "  decode: \"%s\"\n", decoded);
                free(decoded);
            }
            tokenizer_free();
        }

        niyah_free(m);

        fprintf(stderr, "\n  Results: %d passed, %d failed\n\n", pass, fail);
        total_fail += fail;

        #undef HYB_PASS
    }

    fprintf(stderr, "\n========================================\n");
    if (total_fail == 0)
        fprintf(stderr, "ALL SMOKE TESTS PASSED\n");
    else
        fprintf(stderr, "TOTAL FAILURES: %d\n", total_fail);
    fprintf(stderr, "========================================\n\n");

    return total_fail;
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * §3  Interactive mode
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

static void interactive_loop(NiyahModel *m, NiyahRuleKB *rules) {
    printf("\n=== NIYAH Hybrid Interactive Mode ===\n");
    printf("  SIMD: %s  |  Params: %zu\n", niyah_simd_name(), niyah_param_count(m));
    if (rules)
        printf("  Rules: %u loaded\n", rules->count);
    else
        printf("  Rules: none (pure neural)\n");
    printf("  KHZ_Q Ethical Prism: ACTIVE (target_energy=0.85)\n");
    printf("  Type a prompt and press Enter. Type 'quit' to exit.\n\n");

    NiyahSampler sampler = { .temperature = 0.8f, .top_p = 0.9f, .seed = 12345 };
    NiyahHybridOpts opts = {
        .rules = rules,
        .max_retries = 3,
        .generate_proof = false
    };

    char line[4096];
    while (1) {
        printf("> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;

        /* Trim newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        if (len == 0) continue;
        if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) break;

        char *response = niyah_hybrid_generate(m, line, &opts, &sampler, NULL);
        if (response) {
            printf("\n[NIYAH] %s\n\n", response);
            free(response);
        } else {
            printf("\n[NIYAH] (no response generated)\n\n");
        }

        /* Advance seed for variety */
        sampler.seed += 7919;
    }

    printf("\nGoodbye.\n");
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * §4  Main entry point
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

static void usage(const char *prog) {
    fprintf(stderr, "NIYAH Hybrid Neuro-Symbolic Engine\n\n");
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s --smoke\n", prog);
    fprintf(stderr, "  %s --model model.bin [--rules rules.nrule] --interactive\n", prog);
    fprintf(stderr, "  %s --verify-proof response.proof\n\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --smoke           Run all smoke tests\n");
    fprintf(stderr, "  --model PATH      Load model from .bin file\n");
    fprintf(stderr, "  --rules PATH      Load verification rules from .nrule file\n");
    fprintf(stderr, "  --interactive     Start interactive prompt loop\n");
    fprintf(stderr, "  --verify-proof P  Verify a .proof file\n");
}

int main(int argc, char **argv) {
    const char *model_path = NULL;
    const char *rules_path = NULL;
    bool do_smoke = false;
    bool do_interactive = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--smoke") == 0) {
            do_smoke = true;
        } else if (strcmp(argv[i], "--model") == 0 && i+1 < argc) {
            model_path = argv[++i];
        } else if (strcmp(argv[i], "--rules") == 0 && i+1 < argc) {
            rules_path = argv[++i];
        } else if (strcmp(argv[i], "--interactive") == 0) {
            do_interactive = true;
        } else if (strcmp(argv[i], "--verify-proof") == 0 && i+1 < argc) {
            const char *proof_path = argv[++i];
            const char *vp = (i+1 < argc) ? argv[++i] : "";
            const char *vo = (i+1 < argc) ? argv[++i] : "";
            const char *vr = (i+1 < argc) ? argv[++i] : NULL;
            bool ok = niyah_proof_verify(proof_path, vp, vo, vr);
            printf("Proof verification: %s\n", ok ? "VALID" : "INVALID");
            return ok ? 0 : 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (do_smoke) {
        int failed = run_all_smoke();
        if (failed == 0) {
            printf("ALL SMOKE PASS — 0 failed\n");
            return 0;
        }
        printf("SMOKE FAIL — %d failed\n", failed);
        return 1;
    }

    if (do_interactive) {
        NiyahModel *m = NULL;

        if (model_path) {
            int rc = niyah_load(&m, model_path);
            if (rc != 0 || !m) {
                fprintf(stderr, "[NIYAH] Failed to load model: %s (rc=%d)\n",
                        model_path, rc);
                return 1;
            }
            printf("[NIYAH] Loaded model: %s (%zu params)\n",
                   model_path, niyah_param_count(m));
        } else {
            fprintf(stderr, "[NIYAH] No --model specified, creating default small model\n");
            NiyahConfig cfg = {
                .magic = NIYAH_MAGIC, .version = NIYAH_VER,
                .embed_dim = 64, .n_heads = 4, .n_kv_heads = 4,
                .n_layers = 2, .ffn_mult = 4, .vocab_size = 256,
                .ctx_len = 32, .rope_theta = 10000.f, .rms_eps = 1e-5f,
            };
            m = niyah_alloc(&cfg);
            float *wp = (float *)m->_pool;
            size_t nw = niyah_param_count(m);
            for (size_t i = 0; i < nw; i++) wp[i] = ((float)(i%37)-18.f)*0.005f;
            for (uint32_t l = 0; l < cfg.n_layers; l++)
                for (uint32_t j = 0; j < cfg.embed_dim; j++) {
                    m->layers[l].rms_att[j] = 1.f;
                    m->layers[l].rms_ffn[j] = 1.f;
                }
            for (uint32_t j = 0; j < cfg.embed_dim; j++) m->rms_final[j] = 1.f;
        }

        NiyahRuleKB *rules = NULL;
        if (rules_path) {
            rules = niyah_rule_load(rules_path);
            if (!rules) {
                fprintf(stderr, "[NIYAH] Failed to load rules: %s\n", rules_path);
                niyah_free(m);
                return 1;
            }
            printf("[NIYAH] Loaded %u rules from: %s\n", rules->count, rules_path);
        }

        interactive_loop(m, rules);

        if (rules) niyah_rule_free(rules);
        niyah_free(m);
        return 0;
    }

    usage(argv[0]);
    return 1;
}
