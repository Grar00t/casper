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
#include <time.h>

#define AUDIT_INPUT_MAX      (64u * 1024u)
#define AUDIT_RULE_FILE_MAX  (1024u * 1024u)

void tokenizer_init(void);
uint32_t tokenizer_encode(const char *text, uint32_t *tokens, uint32_t max_len);
char *tokenizer_decode(const uint32_t *tokens, uint32_t n);
void tokenizer_free(void);

int niyah_sym_smoke(void);
int niyah_csp_smoke(void);
int niyah_rule_smoke(void);
int niyah_proof_smoke(void);

static void json_print_string(FILE *fp, const char *s)
{
    const unsigned char *p = (const unsigned char *)(s ? s : "");
    fputc('"', fp);
    while (*p) {
        switch (*p) {
        case '"': fputs("\\\"", fp); break;
        case '\\': fputs("\\\\", fp); break;
        case '\b': fputs("\\b", fp); break;
        case '\f': fputs("\\f", fp); break;
        case '\n': fputs("\\n", fp); break;
        case '\r': fputs("\\r", fp); break;
        case '\t': fputs("\\t", fp); break;
        default:
            if (*p < 0x20u) fprintf(fp, "\\u%04x", (unsigned)*p);
            else fputc(*p, fp);
            break;
        }
        ++p;
    }
    fputc('"', fp);
}

static char *read_stream_all(FILE *fp, size_t max_bytes)
{
    size_t cap = 4096u, len = 0u;
    char *buf;
    if (!fp || max_bytes == 0u) return NULL;
    if (cap > max_bytes + 1u) cap = max_bytes + 1u;
    buf = (char *)malloc(cap);
    if (!buf) return NULL;
    while (!feof(fp)) {
        size_t room;
        size_t got;
        if (len >= max_bytes) { free(buf); return NULL; }
        if (len + 1u >= cap) {
            size_t next = cap * 2u;
            char *grown;
            if (next > max_bytes + 1u) next = max_bytes + 1u;
            if (next <= cap) { free(buf); return NULL; }
            grown = (char *)realloc(buf, next);
            if (!grown) { free(buf); return NULL; }
            buf = grown;
            cap = next;
        }
        room = cap - len - 1u;
        got = fread(buf + len, 1u, room, fp);
        len += got;
        if (ferror(fp)) { free(buf); return NULL; }
        if (got == 0u && !feof(fp)) { free(buf); return NULL; }
    }
    buf[len] = '\0';
    return buf;
}

static char *read_text_file_bounded(const char *path, size_t max_bytes)
{
    FILE *fp;
    char *text;
    if (!path || !path[0]) return NULL;
    fp = fopen(path, "rb");
    if (!fp) return NULL;
    text = read_stream_all(fp, max_bytes);
    if (fclose(fp) != 0) { free(text); return NULL; }
    return text;
}

static int json_hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int json_u16(const char *p, uint32_t *out)
{
    uint32_t v = 0u;
    int i;
    if (!p || !out) return 0;
    for (i = 0; i < 4; ++i) {
        int h = json_hex_value(p[i]);
        if (h < 0) return 0;
        v = (v << 4) | (uint32_t)h;
    }
    *out = v;
    return 1;
}

static size_t utf8_write(char *out, uint32_t cp)
{
    if (cp <= 0x7Fu) { out[0] = (char)cp; return 1u; }
    if (cp <= 0x7FFu) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2u;
    }
    if (cp <= 0xFFFFu) {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3u;
    }
    if (cp <= 0x10FFFFu) {
        out[0] = (char)(0xF0u | (cp >> 18));
        out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
        out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[3] = (char)(0x80u | (cp & 0x3Fu));
        return 4u;
    }
    return 0u;
}

static char *json_extract_string(const char *json, const char *key)
{
    char needle[96];
    const char *p;
    char *out;
    size_t cap;
    size_t n = 0u;
    int nn;
    if (!json || !key) return NULL;
    nn = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (nn < 0 || (size_t)nn >= sizeof(needle)) return NULL;
    p = strstr(json, needle);
    if (!p) return NULL;
    p += (size_t)nn;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    if (*p++ != ':') return NULL;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    if (*p++ != '"') return NULL;
    cap = strlen(p) + 1u;
    out = (char *)malloc(cap);
    if (!out) return NULL;
    while (*p && *p != '"') {
        if (*p != '\\') {
            out[n++] = *p++;
            continue;
        }
        ++p;
        if (!*p) { free(out); return NULL; }
        switch (*p) {
        case '"': out[n++] = '"'; ++p; break;
        case '\\': out[n++] = '\\'; ++p; break;
        case '/': out[n++] = '/'; ++p; break;
        case 'b': out[n++] = '\b'; ++p; break;
        case 'f': out[n++] = '\f'; ++p; break;
        case 'n': out[n++] = '\n'; ++p; break;
        case 'r': out[n++] = '\r'; ++p; break;
        case 't': out[n++] = '\t'; ++p; break;
        case 'u': {
            uint32_t cp;
            size_t wrote;
            ++p;
            if (!json_u16(p, &cp)) { free(out); return NULL; }
            p += 4;
            if (cp >= 0xD800u && cp <= 0xDBFFu && p[0] == '\\' && p[1] == 'u') {
                uint32_t low;
                if (!json_u16(p + 2, &low) || low < 0xDC00u || low > 0xDFFFu) { free(out); return NULL; }
                cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
                p += 6;
            } else if (cp >= 0xD800u && cp <= 0xDFFFu) {
                free(out);
                return NULL;
            }
            wrote = utf8_write(out + n, cp);
            if (wrote == 0u) { free(out); return NULL; }
            n += wrote;
            break;
        }
        default: free(out); return NULL;
        }
    }
    if (*p != '"') { free(out); return NULL; }
    out[n] = '\0';
    return out;
}

static uint32_t elapsed_clock_ms(clock_t start)
{
    clock_t delta = clock() - start;
    if (delta <= 0) return 0u;
    return (uint32_t)(((double)delta * 1000.0) / (double)CLOCKS_PER_SEC);
}

static int audit_stdin(void)
{
    clock_t start = clock();
    char *payload = read_stream_all(stdin, AUDIT_INPUT_MAX);
    char *prompt = NULL;
    char *text = NULL;
    char *rules_path = NULL;
    char *rules_text = NULL;
    NiyahRuleKB *kb = NULL;
    const char *violation = NULL;
    KHZQ_Result khz;
    uint8_t proof[32], rules_hash[32];
    char proof_hex[65], rules_hex[65];
    bool rules_bound = false;
    bool verified = false;
    const char *error = NULL;

    if (!payload) error = "invalid or oversized stdin payload";
    if (!error) prompt = json_extract_string(payload, "prompt");
    if (!error) text = json_extract_string(payload, "text");
    if (!error) rules_path = json_extract_string(payload, "rules");
    if (!error && (!prompt || !text || !rules_path)) error = "payload must contain string fields prompt, text, and rules";

    if (!error && rules_path[0]) {
        rules_bound = true;
        rules_text = read_text_file_bounded(rules_path, AUDIT_RULE_FILE_MAX);
        if (!rules_text) error = "rules file unreadable or too large";
        if (!error) {
            kb = niyah_rule_parse(rules_text);
            if (!kb) error = "rules file could not be parsed";
        }
    }

    memset(&khz, 0, sizeof(khz));
    if (!error) {
        khz = khz_q_verify_output(text, 0.85f);
        if (kb) violation = niyah_rule_check(kb, prompt, text);
        niyah_proof_generate(prompt, text, rules_text, proof);
        if (rules_text) niyah_sha256((const uint8_t *)rules_text, strlen(rules_text), rules_hash);
        else niyah_sha256((const uint8_t *)"", 0u, rules_hash);
        niyah_hash_to_hex(proof, proof_hex);
        niyah_hash_to_hex(rules_hash, rules_hex);
        verified = khz.is_coherent && violation == NULL;
    } else {
        memset(proof_hex, '0', 64u); proof_hex[64] = '\0';
        memset(rules_hex, '0', 64u); rules_hex[64] = '\0';
    }

    printf("{");
    printf("\"verified\":%s", verified ? "true" : "false");
    printf(",\"verification_scope\":\"khz_q_heuristic+text_rules+receipt_integrity\"");
    printf(",\"factual_truth_verified\":false");
    printf(",\"chain_hash\":\"%s\"", proof_hex);
    printf(",\"proof_kind\":\"NIYAH-PROOF-V2\"");
    printf(",\"rules_bound\":%s", rules_bound ? "true" : "false");
    printf(",\"rules_hash\":\"%s\"", rules_hex);
    printf(",\"confidence\":%.6f", verified ? (double)khz.energy_preserved : 0.0);
    printf(",\"khz_energy\":%.6f", (double)khz.energy_preserved);
    printf(",\"khz_penalty\":%.6f", (double)khz.penalty_nasl);
    printf(",\"rule_violation\":");
    if (violation) json_print_string(stdout, violation); else fputs("null", stdout);
    printf(",\"error\":");
    if (error) json_print_string(stdout, error); else fputs("null", stdout);
    printf(",\"elapsed_ms\":%u}\n", elapsed_clock_ms(start));

    if (kb) niyah_rule_free(kb);
    free(rules_text);
    free(rules_path);
    free(text);
    free(prompt);
    free(payload);
    return 0;
}

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
        char *text_out = tokenizer_decode(out_tokens, n_out);
        if (!text_out) continue;

        KHZQ_Result khz = khz_q_verify_output(text_out, 0.85f);
        if (!khz.is_coherent) { free(text_out); continue; }

        if (!rules) { result = text_out; break; }
        const char *rule_result = niyah_rule_check(rules, prompt, text_out);
        if (!rule_result) { result = text_out; break; }

        if (attempt == max_retries) {
            if (strcmp(rule_result, "REJECTED") == 0) {
                free(text_out);
                result = (char *)malloc(64u);
                if (result) (void)snprintf(result, 64u, "[Output rejected by rules]");
            } else {
                size_t len = strlen(rule_result) + 1u;
                char *replacement = (char *)malloc(len);
                if (replacement) memcpy(replacement, rule_result, len);
                free(text_out);
                result = replacement;
            }
            break;
        }
        free(text_out);
    }

    if (!result) {
        result = (char *)malloc(32u);
        if (result) (void)snprintf(result, 32u, "[Output rejected]");
    }

    if (proof_out) memset(proof_out, 0, 32u);
    /* Fail closed: a parsed rule KB has no byte-level provenance in this API.
     * Do not emit an unbound receipt when rules are active. The CLI/audit path
     * binds the exact rule-file bytes and should be used for rules-bound proof. */
    if (proof_out && generate_proof && result && !rules)
        niyah_proof_generate(prompt, result, NULL, proof_out);

    tokenizer_free();
    return result;
}

static int run_all_smoke(void) {
    int total_fail = 0;
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
    if (!strcmp(argv[1],"--audit-stdin")) return audit_stdin();
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
