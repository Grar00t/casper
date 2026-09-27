/*
 * niyah_hybrid_main.c — NIYAH Hybrid Neuro-Symbolic CLI
 */

#include "niyah_core.h"
#include "rule_parser.h"
#include "proof_generator.h"
#include "khz_q_svd.h"
#include "casper_rag.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <time.h>

void tokenizer_init(void);
uint32_t tokenizer_encode(const char *text, uint32_t *tokens, uint32_t max_len);
char *tokenizer_decode(const uint32_t *tokens, uint32_t n);
void tokenizer_free(void);

int niyah_sym_smoke(void);
int niyah_csp_smoke(void);
int niyah_rule_smoke(void);
int niyah_proof_smoke(void);

static uint32_t clamp_token(uint32_t token, uint32_t vocab) {
    return vocab ? token % vocab : 0u;
}

static uint32_t generate_tokens(NiyahModel *m, const uint32_t *prompt_tokens,
                                uint32_t prompt_len, uint32_t *out_tokens,
                                uint32_t max_out, NiyahSampler *sampler)
{
    if (!m || !prompt_tokens || !out_tokens || !sampler || m->cfg.vocab_size == 0u) return 0u;
    uint32_t ctx = m->cfg.ctx_len;
    if (ctx == 0u || prompt_len == 0u || prompt_len > ctx) return 0u;

    uint32_t pos = 0u;
    uint32_t n_out = 0u;
    for (uint32_t i = 0u; i < prompt_len && pos < ctx; ++i, ++pos)
        (void)niyah_forward(m, clamp_token(prompt_tokens[i], m->cfg.vocab_size), pos);

    uint32_t last_tok = clamp_token(prompt_tokens[prompt_len - 1u], m->cfg.vocab_size);
    for (uint32_t i = 0u; i < max_out && pos < ctx; ++i, ++pos) {
        float *logits = niyah_forward(m, last_tok, pos);
        if (!logits) break;
        uint32_t tok = clamp_token(niyah_sample(logits, m->cfg.vocab_size, sampler), m->cfg.vocab_size);
        if (tok == 1u) break;
        if (n_out >= max_out) break;
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
    if (!m || !prompt || !sampler || m->cfg.vocab_size == 0u || m->cfg.ctx_len == 0u) return NULL;

    tokenizer_init();
    uint32_t prompt_tokens[512];
    uint32_t prompt_len = tokenizer_encode(prompt, prompt_tokens, 512u);
    if (prompt_len == 0u || prompt_len > m->cfg.ctx_len) {
        tokenizer_free();
        return NULL;
    }

    for (uint32_t i = 0u; i < prompt_len; ++i)
        prompt_tokens[i] = clamp_token(prompt_tokens[i], m->cfg.vocab_size);

    uint32_t max_retries = (opts && opts->max_retries > 0u) ? opts->max_retries : 3u;
    NiyahRuleKB *rules = opts ? (NiyahRuleKB *)opts->rules : NULL;
    bool generate_proof = opts ? opts->generate_proof : false;
    uint32_t out_tokens[512];
    char *result = NULL;

    for (uint32_t attempt = 0u; attempt <= max_retries; ++attempt) {
        if (attempt > 0u) sampler->seed += UINT64_C(12345) * attempt;
        uint32_t n_out = generate_tokens(m, prompt_tokens, prompt_len, out_tokens, 512u, sampler);
        char *text = tokenizer_decode(out_tokens, n_out);
        if (!text) continue;

        KHZQ_Result khz = khz_q_verify_output(text, 0.85f);
        if (!khz.is_coherent) { free(text); continue; }

        if (!rules) { result = text; break; }
        const char *violation = niyah_rule_check(rules, prompt, text);
        if (!violation) { result = text; break; }

        if (attempt == max_retries) {
            if (strcmp(violation, "REJECTED") == 0) {
                free(text);
                result = (char *)malloc(64u);
                if (result) (void)snprintf(result, 64u, "[Output rejected by rules]");
            } else {
                size_t len = strlen(violation) + 1u;
                char *replacement = (char *)malloc(len);
                if (replacement) memcpy(replacement, violation, len);
                free(text);
                result = replacement;
            }
            break;
        }
        free(text);
    }

    if (!result) {
        result = (char *)malloc(32u);
        if (result) (void)snprintf(result, 32u, "[Output rejected]");
    }

    if (proof_out) memset(proof_out, 0, 32u);
    /*
     * A loaded NiyahRuleKB does not retain its source file path. Do not emit a
     * proof that silently omits active rules. The --audit-stdin path below has
     * the rule-file path and therefore can bind the exact rule bytes.
     */
    if (proof_out && generate_proof && result && !rules)
        niyah_proof_generate(prompt, result, NULL, proof_out);

    tokenizer_free();
    return result;
}

static int json_hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool json_put_utf8(uint32_t cp, char *out, size_t cap, size_t *used) {
    size_t n = *used;
    if (cp <= 0x7Fu) {
        if (n + 1u >= cap) return false;
        out[n++] = (char)cp;
    } else if (cp <= 0x7FFu) {
        if (n + 2u >= cap) return false;
        out[n++] = (char)(0xC0u | (cp >> 6));
        out[n++] = (char)(0x80u | (cp & 0x3Fu));
    } else {
        if (n + 3u >= cap) return false;
        out[n++] = (char)(0xE0u | (cp >> 12));
        out[n++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[n++] = (char)(0x80u | (cp & 0x3Fu));
    }
    *used = n;
    return true;
}

static const char *json_find_value(const char *json, const char *key) {
    char needle[96];
    int nw;
    const char *p;
    if (!json || !key) return NULL;
    nw = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (nw <= 0 || (size_t)nw >= sizeof(needle)) return NULL;
    p = strstr(json, needle);
    if (!p) return NULL;
    p += (size_t)nw;
    while (*p && isspace((unsigned char)*p)) ++p;
    if (*p != ':') return NULL;
    ++p;
    while (*p && isspace((unsigned char)*p)) ++p;
    return p;
}

static bool json_get_string(const char *json, const char *key, char *out, size_t cap) {
    const char *p = json_find_value(json, key);
    size_t used = 0u;
    if (!p || !out || cap == 0u || *p != '"') return false;
    ++p;
    while (*p && *p != '"') {
        unsigned char c = (unsigned char)*p++;
        if (c == '\\') {
            uint32_t cp;
            int h0, h1, h2, h3;
            c = (unsigned char)*p++;
            if (!c) return false;
            if (c == 'n') c = '\n';
            else if (c == 'r') c = '\r';
            else if (c == 't') c = '\t';
            else if (c == 'b') c = '\b';
            else if (c == 'f') c = '\f';
            else if (c == '"' || c == '\\' || c == '/') { /* literal */ }
            else if (c == 'u') {
                h0 = json_hexval(p[0]); h1 = json_hexval(p[1]);
                h2 = json_hexval(p[2]); h3 = json_hexval(p[3]);
                if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0) return false;
                cp = (uint32_t)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
                p += 4;
                if (cp >= 0xD800u && cp <= 0xDFFFu) return false;
                if (!json_put_utf8(cp, out, cap, &used)) return false;
                continue;
            } else return false;
        }
        if (used + 1u >= cap) return false;
        out[used++] = (char)c;
    }
    if (*p != '"') return false;
    out[used] = '\0';
    return true;
}

static void json_write_string(FILE *fp, const char *s) {
    const unsigned char *p = (const unsigned char *)(s ? s : "");
    fputc('"', fp);
    while (*p) {
        unsigned char c = *p++;
        if (c == '"') fputs("\\\"", fp);
        else if (c == '\\') fputs("\\\\", fp);
        else if (c == '\n') fputs("\\n", fp);
        else if (c == '\r') fputs("\\r", fp);
        else if (c == '\t') fputs("\\t", fp);
        else if (c < 0x20u) fprintf(fp, "\\u%04x", (unsigned)c);
        else fputc((int)c, fp);
    }
    fputc('"', fp);
}

static int run_audit_stdin(void) {
    char payload[32768];
    char prompt[2049];
    char text[4097];
    char rules_path[4096];
    size_t nread = fread(payload, 1u, sizeof(payload) - 1u, stdin);
    clock_t started = clock();
    NiyahRuleKB *rules = NULL;
    const char *violation = NULL;
    KHZQ_Result khz;
    uint8_t proof[32];
    char proof_hex[65];
    bool verified;
    double elapsed_ms;

    if (ferror(stdin) || nread == sizeof(payload) - 1u) {
        fputs("{\"verified\":false,\"error\":\"stdin payload too large or unreadable\"}\n", stdout);
        return 0;
    }
    payload[nread] = '\0';
    rules_path[0] = '\0';

    if (!json_get_string(payload, "prompt", prompt, sizeof(prompt)) ||
        !json_get_string(payload, "text", text, sizeof(text))) {
        fputs("{\"verified\":false,\"error\":\"invalid audit JSON\"}\n", stdout);
        return 0;
    }
    (void)json_get_string(payload, "rules", rules_path, sizeof(rules_path));

    if (rules_path[0]) {
        rules = niyah_rule_load(rules_path);
        if (!rules) {
            fputs("{\"verified\":false,\"error\":\"rule file load failed\",\"rule_violation\":\"RULE_LOAD_ERROR\"}\n", stdout);
            return 0;
        }
    }

    khz = khz_q_verify_output(text, 0.85f);
    if (rules) violation = niyah_rule_check(rules, prompt, text);
    verified = khz.is_coherent && violation == NULL;

    niyah_proof_generate(prompt, text, rules_path[0] ? rules_path : NULL, proof);
    niyah_hash_to_hex(proof, proof_hex);
    elapsed_ms = ((double)(clock() - started) * 1000.0) / (double)CLOCKS_PER_SEC;

    fputs("{\"verified\":", stdout);
    fputs(verified ? "true" : "false", stdout);
    fputs(",\"chain_hash\":", stdout); json_write_string(stdout, proof_hex);
    fprintf(stdout, ",\"confidence\":%.6f,\"confidence_kind\":\"khz_energy_not_truth\",\"elapsed_ms\":%.3f,\"khz_energy\":%.6f,\"rule_violation\":",
            verified ? (double)khz.energy_preserved : 0.0, elapsed_ms, (double)khz.energy_preserved);
    if (violation) json_write_string(stdout, violation); else fputs("null", stdout);
    fputs("}\n", stdout);

    if (rules) niyah_rule_free(rules);
    return 0;
}

static int run_all_smoke(void) {
    int total_fail = 0;
    /* niyah_smoke() was merged into Niyah.Engine / NiyahKernel and is defined
     * nowhere here; the core self-check is build/niyah. */
    total_fail += niyah_sym_smoke();
    total_fail += niyah_csp_smoke();
    total_fail += niyah_rule_smoke();
    total_fail += niyah_proof_smoke();
    {
        int pass = 0, fail = 0;
#define KHZQ_PASS(cond, label) do { if (cond) { ++pass; } else { ++fail; (void)fprintf(stderr, "[FAIL] %s\n", label); } } while (0)
        KHZQ_Result r1 = khz_q_verify_output("bismillah bismillah bismillah bismillah", 0.85f);
        KHZQ_PASS(r1.energy_preserved >= 0.85f, "coherent text energy");
        KHZQ_Result r2 = khz_q_verify_output("", 0.85f);
        KHZQ_PASS(!r2.is_coherent, "empty text rejected");
        KHZQ_Result r3 = khz_q_verify_output("test", 1.5f);
        KHZQ_PASS(r3.chi_e > 0 && r3.chi_e <= KHZ_MAX_N, "chi_e range");
        KHZQ_Result r4 = khz_q_verify_output("\xd8\xa8\xd8\xb3\xd9\x85 \xd8\xa7\xd9\x84\xd9\x84\xd9\x87", 0.80f);
        KHZQ_PASS(r4.chi_e >= 1, "Arabic UTF-8");
        total_fail += fail;
#undef KHZQ_PASS
        (void)pass;
    }
    {
        char p[64], t[64], r[64];
        const char *sample = "{\"prompt\":\"hello\\nworld\",\"text\":\"answer\\\"ok\",\"rules\":\"rules.nrule\"}";
        if (!json_get_string(sample, "prompt", p, sizeof(p)) || strcmp(p, "hello\nworld") != 0) ++total_fail;
        if (!json_get_string(sample, "text", t, sizeof(t)) || strcmp(t, "answer\"ok") != 0) ++total_fail;
        if (!json_get_string(sample, "rules", r, sizeof(r)) || strcmp(r, "rules.nrule") != 0) ++total_fail;
    }
    {
        NiyahConfig cfg = {.magic=NIYAH_MAGIC,.version=NIYAH_VER,.embed_dim=64,.n_heads=4,.n_kv_heads=4,.n_layers=2,.ffn_mult=4,.vocab_size=256,.ctx_len=32,.rope_theta=10000.f,.rms_eps=1e-5f};
        NiyahModel *m = niyah_alloc(&cfg);
        if (!m) ++total_fail;
        else {
            float *wp = (float *)m->_pool;
            size_t nw = niyah_param_count(m);
            for (size_t i=0;i<nw;++i) wp[i]=((float)(i%37)-18.f)*0.005f;
            for (uint32_t l=0;l<cfg.n_layers;++l) for (uint32_t j=0;j<cfg.embed_dim;++j) { m->layers[l].rms_att[j]=1.f; m->layers[l].rms_ffn[j]=1.f; }
            for (uint32_t j=0;j<cfg.embed_dim;++j) m->rms_final[j]=1.f;
            NiyahSampler s={.temperature=0.8f,.top_p=0.9f,.seed=42};
            NiyahHybridOpts opts={.rules=NULL,.max_retries=0,.generate_proof=false};
            char *out=niyah_hybrid_generate(m,"hello",&opts,&s,NULL); if(!out) ++total_fail; free(out);
            niyah_free(m);
        }
    }
    return total_fail;
}

static void rag_loop(RagBackend backend, NiyahRuleKB *rules) {
    char line[2048];
    while (1) {
        (void)printf("[RAG] > "); (void)fflush(stdout);
        if (!fgets(line,sizeof(line),stdin)) break;
        size_t len=strlen(line); while(len && (line[len-1]=='\n'||line[len-1]=='\r')) line[--len]='\0';
        if (!len) continue;
        if (!strcmp(line,"quit") || !strcmp(line,"exit")) break;
        RagCtx *ctx=casper_rag_query(line,backend,NULL);
        if (!ctx) { (void)printf("[RAG] query failed\n"); continue; }
        (void)printf("sources=%d confidence=%.3f elapsed=%u\n",ctx->n_results,(double)ctx->confidence,ctx->elapsed_ms);
        for(int i=0;i<ctx->n_results;++i) (void)printf("[%d] %s\n    %s\n",i+1,ctx->results[i].title,ctx->results[i].url);
        if(rules && ctx->context[0]) { const char *v=niyah_rule_check(rules,line,ctx->context); if(v) (void)printf("rule=%s\n",v); }
        casper_rag_free(ctx);
    }
}

static void interactive_loop(NiyahModel *m, NiyahRuleKB *rules) {
    NiyahSampler sampler={.temperature=0.8f,.top_p=0.9f,.seed=12345};
    NiyahHybridOpts opts={.rules=rules,.max_retries=3,.generate_proof=false};
    char line[4096];
    while(1){
        (void)printf("> "); (void)fflush(stdout);
        if(!fgets(line,sizeof(line),stdin))break;
        size_t len=strlen(line); while(len&&(line[len-1]=='\n'||line[len-1]=='\r'))line[--len]='\0';
        if(!len) continue;
        if(!strcmp(line,"quit")||!strcmp(line,"exit")) break;
        char *response=niyah_hybrid_generate(m,line,&opts,&sampler,NULL);
        if(response){(void)printf("%s\n",response);free(response);} else (void)printf("[no response]\n");
        sampler.seed+=7919u;
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { (void)fprintf(stderr,"usage: %s --smoke | --audit-stdin | --rag | --model <file> | --interactive\n",argv[0]); return 3; }
    if (!strcmp(argv[1],"--smoke")) return run_all_smoke();
    if (!strcmp(argv[1],"--audit-stdin")) return run_audit_stdin();
    if (!strcmp(argv[1],"--rag")) { rag_loop(RAG_BACKEND_DDG,NULL); return 0; }
    if (!strcmp(argv[1],"--model")) {
        if (argc < 3) return 3;
        NiyahModel *m=NULL;
        int rc=niyah_load(&m,argv[2]);
        if(rc!=0 || !m) return 1;
        interactive_loop(m,NULL);
        niyah_free(m); return 0;
    }
    if (!strcmp(argv[1],"--interactive")) {
        NiyahConfig cfg={.magic=NIYAH_MAGIC,.version=NIYAH_VER,.embed_dim=128,.n_heads=8,.n_kv_heads=8,.n_layers=4,.ffn_mult=4,.vocab_size=8192,.ctx_len=64,.rope_theta=10000.f,.rms_eps=1e-5f};
        NiyahModel *m=niyah_alloc(&cfg); if(!m)return 1;
        interactive_loop(m,NULL); niyah_free(m); return 0;
    }
    return 3;
}
