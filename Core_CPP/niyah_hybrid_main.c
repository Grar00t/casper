/* niyah_hybrid_main.c — NIYAH C11 CLI and integration bridge. */
#include "niyah_core.h"
#include "rule_parser.h"
#include "proof_generator.h"
#include "khz_q_svd.h"
#include "casper_rag.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
                                uint32_t max_out, NiyahSampler *sampler) {
    uint32_t ctx, pos, n_out, last_tok, i;
    if (!m || !prompt_tokens || !out_tokens || !sampler || m->cfg.vocab_size == 0u) return 0u;
    ctx = m->cfg.ctx_len;
    if (ctx == 0u || prompt_len == 0u || prompt_len > ctx) return 0u;

    pos = 0u;
    n_out = 0u;
    for (i=0u;i<prompt_len && pos<ctx;++i,++pos)
        (void)niyah_forward(m, clamp_token(prompt_tokens[i], m->cfg.vocab_size), pos);

    last_tok = clamp_token(prompt_tokens[prompt_len-1u], m->cfg.vocab_size);
    for (i=0u;i<max_out && pos<ctx;++i,++pos) {
        float *logits = niyah_forward(m, last_tok, pos);
        uint32_t tok;
        if (!logits) break;
        tok = clamp_token(niyah_sample(logits, m->cfg.vocab_size, sampler), m->cfg.vocab_size);
        if (tok == 1u) break;
        out_tokens[n_out++] = tok;
        last_tok = tok;
    }
    return n_out;
}

char *niyah_hybrid_generate(NiyahModel *m, const char *prompt,
                            const NiyahHybridOpts *opts,
                            NiyahSampler *sampler,
                            uint8_t proof_out[32]) {
    uint32_t prompt_tokens[512];
    uint32_t out_tokens[512];
    uint32_t prompt_len, max_retries, attempt, i;
    NiyahRuleKB *rules;
    bool generate_proof;
    char *result = NULL;

    if (!m || !prompt || !sampler || m->cfg.vocab_size == 0u || m->cfg.ctx_len == 0u) return NULL;
    tokenizer_init();
    prompt_len = tokenizer_encode(prompt, prompt_tokens, 512u);
    if (prompt_len == 0u || prompt_len > m->cfg.ctx_len) {
        tokenizer_free();
        return NULL;
    }
    for (i=0u;i<prompt_len;++i) prompt_tokens[i] = clamp_token(prompt_tokens[i], m->cfg.vocab_size);

    max_retries = (opts && opts->max_retries > 0u) ? opts->max_retries : 3u;
    rules = opts ? (NiyahRuleKB *)opts->rules : NULL;
    generate_proof = opts ? opts->generate_proof : false;

    for (attempt=0u;attempt<=max_retries;++attempt) {
        uint32_t n_out;
        char *text;
        const char *violation;
        if (attempt > 0u) sampler->seed += UINT64_C(12345) * attempt;
        n_out = generate_tokens(m, prompt_tokens, prompt_len, out_tokens, 512u, sampler);
        text = tokenizer_decode(out_tokens, n_out);
        if (!text) continue;

        if (!rules) { result = text; break; }
        violation = niyah_rule_check(rules, prompt, text);
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
    if (proof_out && generate_proof && result)
        (void)niyah_proof_generate(prompt, result, NULL, proof_out);

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
        KHZQ_Result empty = khz_q_verify_output("", 0.85f);
        KHZQ_Result text = khz_q_verify_output("bismillah bismillah bismillah bismillah", 0.85f);
        if (empty.energy_preserved != 0.0f) ++total_fail;
        if (text.chi_e < 1 || text.chi_e > KHZ_MAX_N) ++total_fail;
    }

    {
        NiyahConfig cfg = {
            .magic=NIYAH_MAGIC,.version=NIYAH_VER,.embed_dim=64,.n_heads=4,.n_kv_heads=4,
            .n_layers=2,.ffn_mult=4,.vocab_size=256,.ctx_len=32,.rope_theta=10000.f,.rms_eps=1e-5f
        };
        NiyahModel *m = niyah_alloc(&cfg);
        if (!m) ++total_fail;
        else {
            float *wp = (float *)m->_pool;
            size_t nw = niyah_param_count(m);
            uint32_t l, j;
            size_t i;
            NiyahSampler s = {.temperature=0.8f,.top_p=0.9f,.seed=42};
            NiyahHybridOpts opts = {.rules=NULL,.max_retries=0,.generate_proof=false};
            char *out;
            for (i=0u;i<nw;++i) wp[i]=((float)(i%37u)-18.f)*0.005f;
            for (l=0u;l<cfg.n_layers;++l)
                for (j=0u;j<cfg.embed_dim;++j) {
                    m->layers[l].rms_att[j]=1.f;
                    m->layers[l].rms_ffn[j]=1.f;
                }
            for (j=0u;j<cfg.embed_dim;++j) m->rms_final[j]=1.f;
            out=niyah_hybrid_generate(m,"hello",&opts,&s,NULL);
            if(!out) ++total_fail;
            free(out);
            niyah_free(m);
        }
    }
    return total_fail;
}

static void rag_loop(RagBackend backend, NiyahRuleKB *rules) {
    char line[2048];
    while (1) {
        size_t len;
        RagCtx *ctx;
        int i;
        (void)printf("[RAG] > ");
        (void)fflush(stdout);
        if (!fgets(line,sizeof(line),stdin)) break;
        len=strlen(line);
        while(len && (line[len-1]=='\n'||line[len-1]=='\r')) line[--len]='\0';
        if (!len) continue;
        if (!strcmp(line,"quit") || !strcmp(line,"exit")) break;
        ctx=casper_rag_query(line,backend,NULL);
        if (!ctx) { (void)printf("[RAG] query failed\n"); continue; }
        (void)printf("sources=%d confidence=%.3f elapsed=%u\n",ctx->n_results,(double)ctx->confidence,ctx->elapsed_ms);
        for(i=0;i<ctx->n_results;++i)
            (void)printf("[%d] %s\n    %s\n",i+1,ctx->results[i].title,ctx->results[i].url);
        if(rules && ctx->context[0]) {
            const char *v=niyah_rule_check(rules,line,ctx->context);
            if(v) (void)printf("rule=%s\n",v);
        }
        casper_rag_free(ctx);
    }
}

static void interactive_loop(NiyahModel *m, NiyahRuleKB *rules) {
    NiyahSampler sampler={.temperature=0.8f,.top_p=0.9f,.seed=12345};
    NiyahHybridOpts opts={.rules=rules,.max_retries=3,.generate_proof=false};
    char line[4096];
    while(1) {
        size_t len;
        char *response;
        (void)printf("> ");
        (void)fflush(stdout);
        if(!fgets(line,sizeof(line),stdin)) break;
        len=strlen(line);
        while(len&&(line[len-1]=='\n'||line[len-1]=='\r')) line[--len]='\0';
        if(!len) continue;
        if(!strcmp(line,"quit")||!strcmp(line,"exit")) break;
        response=niyah_hybrid_generate(m,line,&opts,&sampler,NULL);
        if(response){(void)printf("%s\n",response);free(response);}
        else (void)printf("[no response]\n");
        sampler.seed+=7919u;
    }
}

static char *read_stdin_all(size_t max_bytes) {
    size_t cap = 4096u, len = 0u;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;
    while (!feof(stdin)) {
        size_t want, n;
        if (len >= max_bytes) { free(buf); return NULL; }
        if (cap - len < 2048u) {
            size_t next = cap * 2u;
            char *p;
            if (next > max_bytes + 1u) next = max_bytes + 1u;
            p = (char *)realloc(buf, next);
            if (!p) { free(buf); return NULL; }
            buf = p; cap = next;
        }
        want = cap - len - 1u;
        n = fread(buf + len, 1u, want, stdin);
        len += n;
        if (ferror(stdin)) { free(buf); return NULL; }
        if (n == 0u) break;
    }
    buf[len] = '\0';
    return buf;
}

static int json_hex(char c) {
    if (c>='0'&&c<='9') return c-'0';
    if (c>='a'&&c<='f') return c-'a'+10;
    if (c>='A'&&c<='F') return c-'A'+10;
    return -1;
}

static int append_utf8(char *out, size_t max, size_t *used, unsigned cp) {
    size_t n = *used;
    if (cp <= 0x7fu) {
        if (n + 1u >= max) return -1;
        out[n++] = (char)cp;
    } else if (cp <= 0x7ffu) {
        if (n + 2u >= max) return -1;
        out[n++] = (char)(0xc0u | (cp >> 6));
        out[n++] = (char)(0x80u | (cp & 0x3fu));
    } else {
        if (n + 3u >= max) return -1;
        out[n++] = (char)(0xe0u | (cp >> 12));
        out[n++] = (char)(0x80u | ((cp >> 6) & 0x3fu));
        out[n++] = (char)(0x80u | (cp & 0x3fu));
    }
    *used = n;
    return 0;
}

static int json_get_string(const char *json, const char *key, char *out, size_t max) {
    char pattern[64];
    const char *p;
    size_t used = 0u;
    int pn;
    if (!json || !key || !out || max == 0u) return -1;
    pn = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (pn < 0 || (size_t)pn >= sizeof(pattern)) return -1;
    p = strstr(json, pattern);
    if (!p) return -1;
    p += strlen(pattern);
    while (*p==' '||*p=='\t'||*p=='\r'||*p=='\n') ++p;
    if (*p++ != ':') return -1;
    while (*p==' '||*p=='\t'||*p=='\r'||*p=='\n') ++p;
    if (*p++ != '"') return -1;
    while (*p && *p != '"') {
        unsigned char c = (unsigned char)*p++;
        if (c == '\\') {
            c = (unsigned char)*p++;
            switch (c) {
                case '"': c='"'; break;
                case '\\': c='\\'; break;
                case '/': c='/'; break;
                case 'b': c='\b'; break;
                case 'f': c='\f'; break;
                case 'n': c='\n'; break;
                case 'r': c='\r'; break;
                case 't': c='\t'; break;
                case 'u': {
                    int h0=json_hex(p[0]),h1=json_hex(p[1]),h2=json_hex(p[2]),h3=json_hex(p[3]);
                    unsigned cp;
                    if(h0<0||h1<0||h2<0||h3<0) return -1;
                    cp=(unsigned)((h0<<12)|(h1<<8)|(h2<<4)|h3);
                    p += 4;
                    if (append_utf8(out,max,&used,cp)!=0) return -1;
                    continue;
                }
                default: return -1;
            }
        }
        if (used + 1u >= max) return -1;
        out[used++] = (char)c;
    }
    if (*p != '"') return -1;
    out[used] = '\0';
    return 0;
}

static void json_print_string(const char *s) {
    const unsigned char *p;
    (void)putchar('"');
    for (p=(const unsigned char *)(s?s:""); *p; ++p) {
        switch (*p) {
            case '"': (void)fputs("\\\"",stdout); break;
            case '\\': (void)fputs("\\\\",stdout); break;
            case '\n': (void)fputs("\\n",stdout); break;
            case '\r': (void)fputs("\\r",stdout); break;
            case '\t': (void)fputs("\\t",stdout); break;
            default:
                if (*p < 0x20u) (void)printf("\\u%04x",(unsigned)*p);
                else (void)putchar((int)*p);
                break;
        }
    }
    (void)putchar('"');
}

static int audit_stdin(void) {
    char *json = read_stdin_all(16384u);
    char prompt[2049], text[4097], rules_path[1025];
    NiyahRuleKB *rules = NULL;
    const char *violation = NULL;
    uint8_t digest[32];
    char digest_hex[65];
    KHZQ_Result metric;
    int digest_rc;
    if (!json) {
        (void)fputs("{\"error\":\"invalid or oversized input\"}\n", stdout);
        return 3;
    }
    if (json_get_string(json,"prompt",prompt,sizeof(prompt))!=0 ||
        json_get_string(json,"text",text,sizeof(text))!=0 ||
        json_get_string(json,"rules",rules_path,sizeof(rules_path))!=0) {
        free(json);
        (void)fputs("{\"error\":\"expected JSON string fields: prompt, text, rules\"}\n", stdout);
        return 3;
    }
    free(json);

    if (rules_path[0]) {
        rules = niyah_rule_load(rules_path);
        if (!rules) {
            (void)fputs("{\"error\":\"rules file could not be loaded\"}\n", stdout);
            return 3;
        }
        violation = niyah_rule_check(rules,prompt,text);
    }

    digest_rc = niyah_proof_generate(prompt,text,rules_path[0]?rules_path:NULL,digest);
    if (digest_rc != 0) {
        if (rules) niyah_rule_free(rules);
        (void)fputs("{\"error\":\"rules file could not be hashed\"}\n", stdout);
        return 3;
    }
    niyah_hash_to_hex(digest,digest_hex);
    metric = khz_q_verify_output(text,0.85f);

    (void)printf("{\"audit_passed\":%s,\"integrity_sha256\":\"%s\",\"structure_energy\":%.6f,\"rule_violation\":",
                 violation?"false":"true",digest_hex,(double)metric.energy_preserved);
    if (violation) json_print_string(violation);
    else (void)fputs("null",stdout);
    (void)fputs("}\n",stdout);
    if (rules) niyah_rule_free(rules);
    return violation ? 1 : 0;
}

static int parse_backend(const char *name, RagBackend *backend) {
    if (!name || !backend) return -1;
    if (!strcmp(name,"ddg")) *backend=RAG_BACKEND_DDG;
    else if (!strcmp(name,"searxng")) *backend=RAG_BACKEND_SEARXNG;
    else if (!strcmp(name,"bing")) *backend=RAG_BACKEND_BING;
    else return -1;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        (void)fprintf(stderr,"usage: %s --smoke | --audit-stdin | --rag [ddg|searxng|bing] | --model <file> [--rules file]\n",argv[0]);
        return 3;
    }
    if (!strcmp(argv[1],"--smoke")) return run_all_smoke();
    if (!strcmp(argv[1],"--audit-stdin")) return audit_stdin();
    if (!strcmp(argv[1],"--rag")) {
        RagBackend backend=RAG_BACKEND_DDG;
        if (argc >= 3 && parse_backend(argv[2],&backend)!=0) return 3;
        rag_loop(backend,NULL);
        return 0;
    }
    if (!strcmp(argv[1],"--model")) {
        NiyahModel *m=NULL;
        NiyahRuleKB *rules=NULL;
        int rc;
        if (argc < 3) return 3;
        if (argc > 3) {
            if (argc != 5 || strcmp(argv[3],"--rules") != 0) return 3;
            rules=niyah_rule_load(argv[4]);
            if (!rules) return 3;
        }
        rc=niyah_load(&m,argv[2]);
        if(rc!=0 || !m) { if(rules)niyah_rule_free(rules); return 1; }
        interactive_loop(m,rules);
        niyah_free(m);
        if(rules)niyah_rule_free(rules);
        return 0;
    }
    if (!strcmp(argv[1],"--interactive")) {
        (void)fprintf(stderr,"--interactive without a model was removed; use --model <file>\n");
        return 3;
    }
    return 3;
}
