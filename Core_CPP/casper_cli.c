/* casper_cli.c — Casper search/retrieval CLI with integrity receipts. C11. */
#include "casper_rag.h"
#include "rule_parser.h"
#include "rule_source_guard.h"
#include "proof_generator.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

#define CASPER_RULE_FILE_MAX (1024u * 1024u)

static void configure_utf8_console(void) {
#ifdef _WIN32
    (void)SetConsoleOutputCP(CP_UTF8);
    (void)SetConsoleCP(CP_UTF8);
#endif
}

static size_t bounded_strlen(const char *s, size_t max) {
    size_t n = 0u;
    if (!s) return 0u;
    while (n < max && s[n] != '\0') ++n;
    return n;
}

static void json_str(FILE *fp, const char *s) {
    fputc('"', fp);
    if (s) {
        for (const unsigned char *p=(const unsigned char *)s; *p; ++p) {
            switch (*p) {
                case '"': fputs("\\\"",fp); break;
                case '\\': fputs("\\\\",fp); break;
                case '\n': fputs("\\n",fp); break;
                case '\r': fputs("\\r",fp); break;
                case '\t': fputs("\\t",fp); break;
                default: if (*p < 0x20u) fprintf(fp,"\\u%04x",*p); else fputc(*p,fp);
            }
        }
    }
    fputc('"', fp);
}

static char *read_text_file(const char *path, size_t max_bytes) {
    FILE *fp;
    long len;
    char *buf;
    size_t got;
    if (!path) return NULL;
    fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    len = ftell(fp);
    if (len < 0 || (size_t)len > max_bytes) { fclose(fp); return NULL; }
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return NULL; }
    buf = (char *)malloc((size_t)len + 1u);
    if (!buf) { fclose(fp); return NULL; }
    got = fread(buf, 1u, (size_t)len, fp);
    if (got != (size_t)len || ferror(fp) || fclose(fp) != 0) { free(buf); return NULL; }
    buf[got] = '\0';
    return buf;
}

static RagBackend pick_backend(void) {
    const char *name = getenv("CASPER_BACKEND");
    if (!name || !name[0]) return RAG_BACKEND_DDG;
    if (!strcmp(name, "searxng")) return RAG_BACKEND_SEARXNG;
    if (!strcmp(name, "bing"))    return RAG_BACKEND_BING;
    if (!strcmp(name, "ddg"))     return RAG_BACKEND_DDG;
    fprintf(stderr, "[casper] unknown CASPER_BACKEND=%s (ddg|bing|searxng), using ddg\n", name);
    return RAG_BACKEND_DDG;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void percent_decode(const char *in, char *out, size_t max) {
    size_t o = 0u;
    if (!out || max == 0u) return;
    if (!in) { out[0] = '\0'; return; }
    while (*in && o + 1u < max) {
        if (in[0] == '%' && in[1] && in[2]) {
            int hi = hexval(in[1]);
            int lo = hexval(in[2]);
            if (hi >= 0 && lo >= 0) {
                out[o++] = (char)((hi << 4) | lo);
                in += 3;
                continue;
            }
        }
        out[o++] = (*in == '+') ? ' ' : *in;
        ++in;
    }
    out[o] = '\0';
}

static void normalize_result_url(RagResult *r) {
    const char *marker;
    const char *end;
    char encoded[RAG_URL_MAX];
    char decoded[RAG_URL_MAX];
    size_t n;
    if (!r || !r->url[0]) return;
    if (!strstr(r->url, "duckduckgo.com/l/")) return;
    marker = strstr(r->url, "uddg=");
    if (!marker) return;
    marker += 5;
    end = strchr(marker, '&');
    n = end ? (size_t)(end - marker) : strlen(marker);
    if (n >= sizeof(encoded)) n = sizeof(encoded) - 1u;
    memcpy(encoded, marker, n);
    encoded[n] = '\0';
    percent_decode(encoded, decoded, sizeof(decoded));
    if (!strncmp(decoded, "http://", 7) || !strncmp(decoded, "https://", 8)) {
        (void)snprintf(r->url, sizeof(r->url), "%s", decoded);
    }
}

static void rehash_result(RagResult *r) {
    uint8_t anchor[RAG_URL_MAX + 1u + RAG_SNIPPET_MAX];
    size_t ul;
    size_t sl;
    size_t n;
    if (!r) return;
    ul = bounded_strlen(r->url, sizeof(r->url));
    sl = bounded_strlen(r->snippet, sizeof(r->snippet));
    if (ul >= RAG_URL_MAX || sl >= RAG_SNIPPET_MAX) {
        memset(r->sha256, 0, sizeof(r->sha256));
        return;
    }
    n = ul + 1u + sl;
    memcpy(anchor, r->url, ul);
    anchor[ul] = 0u;
    memcpy(anchor + ul + 1u, r->snippet, sl);
    niyah_sha256(anchor, n, r->sha256);
}

static int result_cmp_cli(const void *a, const void *b) {
    const RagResult *ra = (const RagResult *)a;
    const RagResult *rb = (const RagResult *)b;
    if (ra->score < rb->score) return 1;
    if (ra->score > rb->score) return -1;
    {
        int u = strcmp(ra->url, rb->url);
        if (u) return u;
    }
    return strcmp(ra->title, rb->title);
}

static void normalize_results(RagCtx *ctx) {
    int i;
    if (!ctx || ctx->n_results <= 0) return;
    if (ctx->n_results > RAG_MAX_RESULTS) ctx->n_results = RAG_MAX_RESULTS;
    for (i = 0; i < ctx->n_results; ++i) {
        normalize_result_url(&ctx->results[i]);
        rehash_result(&ctx->results[i]);
    }
    qsort(ctx->results, (size_t)ctx->n_results, sizeof(ctx->results[0]), result_cmp_cli);
}

static int cmd_verify(const char *proof_path, const char *rules_path) {
    bool rules_bound = false;
    bool rules_verified = false;
    bool receipt_valid = niyah_proof_verify_saved(proof_path, rules_path,
                                                   &rules_bound, &rules_verified);
    bool valid = receipt_valid && (!rules_bound || rules_verified);
    printf("{\"proof_path\":");
    json_str(stdout, proof_path);
    printf(",\"receipt_valid\":%s,\"rules_bound\":%s,\"rules_verified\":%s,\"valid\":%s}\n",
           receipt_valid ? "true" : "false",
           rules_bound ? "true" : "false",
           rules_verified ? "true" : "false",
           valid ? "true" : "false");
    return valid ? 0 : 1;
}

static void build_answer(const RagCtx *ctx,char *out,size_t max){
    if(!ctx||!out||!max)return;
    if(ctx->n_results<=0){snprintf(out,max,"No sources found for this query.");return;}
    const RagResult *top=&ctx->results[0];
    snprintf(out,max,"Source: %s\n\n%s",top->title[0]?top->title:top->url,top->snippet[0]?top->snippet:"(no snippet)");
}

static int cmd_self_check(void) {
    RagCtx ctx;
    char answer[512];
    int saw_unwrapped = 0;
    int saw_hash = 0;
    int i;
    memset(&ctx, 0, sizeof(ctx));
    ctx.n_results = RAG_MAX_RESULTS + 2;
    ctx.results[0].score = 0.500f;
    (void)snprintf(ctx.results[0].title, sizeof(ctx.results[0].title), "%s", "low");
    (void)snprintf(ctx.results[0].snippet, sizeof(ctx.results[0].snippet), "%s", "low snippet");
    (void)snprintf(ctx.results[0].url, sizeof(ctx.results[0].url), "%s",
                   "//duckduckgo.com/l/?uddg=https%3A%2F%2Fexample.com%2Flow&amp;rut=x");
    ctx.results[1].score = 1.000f;
    (void)snprintf(ctx.results[1].title, sizeof(ctx.results[1].title), "%s", "best");
    (void)snprintf(ctx.results[1].snippet, sizeof(ctx.results[1].snippet), "%s", "best snippet");
    (void)snprintf(ctx.results[1].url, sizeof(ctx.results[1].url), "%s", "https://example.com/best");
    ctx.results[2].score = 0.667f;
    (void)snprintf(ctx.results[2].title, sizeof(ctx.results[2].title), "%s", "middle");
    (void)snprintf(ctx.results[2].snippet, sizeof(ctx.results[2].snippet), "%s", "middle snippet");
    (void)snprintf(ctx.results[2].url, sizeof(ctx.results[2].url), "%s", "https://example.com/middle");
    normalize_results(&ctx);
    if (ctx.n_results != RAG_MAX_RESULTS) { fputs("CASPER SELF-CHECK FAIL bounds\n", stderr); return 1; }
    if (ctx.results[0].score != 1.000f || strcmp(ctx.results[0].title, "best") != 0) { fputs("CASPER SELF-CHECK FAIL ranking\n", stderr); return 1; }
    build_answer(&ctx, answer, sizeof(answer));
    if (!strstr(answer, "best snippet")) { fputs("CASPER SELF-CHECK FAIL answer-source\n", stderr); return 1; }
    for (i = 0; i < ctx.n_results; ++i) {
        if (!strcmp(ctx.results[i].url, "https://example.com/low")) {
            int j;
            saw_unwrapped = 1;
            for (j = 0; j < 32; ++j) {
                if (ctx.results[i].sha256[j] != 0u) { saw_hash = 1; break; }
            }
            break;
        }
    }
    if (!saw_unwrapped) { fputs("CASPER SELF-CHECK FAIL ddg-url\n", stderr); return 1; }
    if (!saw_hash) { fputs("CASPER SELF-CHECK FAIL source-hash\n", stderr); return 1; }
    puts("CASPER SELF-CHECK PASS");
    return 0;
}

int main(int argc,char **argv){
    configure_utf8_console();
    if(argc<2){fprintf(stderr,"usage: %s <query> [rules.nrule] | --verify <proof> [rules.nrule] | --self-check\n",argv[0]);return 3;}
    if(!strcmp(argv[1],"--self-check")) return cmd_self_check();
    if(!strcmp(argv[1],"--verify")){if(argc<3)return 3;return cmd_verify(argv[2],argc>=4?argv[3]:NULL);}

    const char *query=argv[1];
    const char *rules_path=argc>=3?argv[2]:NULL;
    RagCtx *ctx=casper_rag_query(query,pick_backend(),rules_path);
    if(!ctx){
        printf("{\"query\":");json_str(stdout,query);
        printf(",\"answer\":\"\",\"error\":\"rag allocation failure\",\"relevance_score\":0.000,\"confidence\":0.000,\"confidence_kind\":\"top_lexical_relevance\",\"mean_relevance\":0.000,\"elapsed_ms\":0,\"violated\":false,\"rejected\":false,\"proof\":null,\"proof_file\":null,\"n_sources\":0,\"sources\":[]}\n");
        return 2;
    }
    if(ctx->n_results<=0){
        printf("{\n  \"query\":");json_str(stdout,query);
        printf(",\n  \"answer\":\"\",\n  \"error\":\"no results - offline or no match\",\n  \"relevance_score\":0.000,\n  \"confidence\":0.000,\n  \"confidence_kind\":\"top_lexical_relevance\",\n  \"mean_relevance\":0.000,\n  \"elapsed_ms\":%u,\n  \"violated\":false,\n  \"rejected\":false,\n  \"proof\":null,\n  \"proof_file\":null,\n  \"n_sources\":0,\n  \"sources\":[]\n}\n",ctx->elapsed_ms);
        casper_rag_free(ctx); return 2;
    }

    normalize_results(ctx);
    NiyahRuleKB *kb=NULL;
    char *rules_text=NULL;
    uint8_t rules_hash[32];
    const uint8_t *rules_hash_ptr=NULL;
    if(rules_path){
        rules_text=read_text_file(rules_path,CASPER_RULE_FILE_MAX);
        if(!rules_text || !niyah_sha256_file(rules_path,rules_hash)){
            fprintf(stderr,"[casper] failed to read/hash rules: %s\n",rules_path);
            free(rules_text);casper_rag_free(ctx);return 3;
        }
        kb=niyah_rule_parse(rules_text);
        if(!kb || !niyah_rule_source_guard(rules_text,kb)){
            fprintf(stderr,"[casper] invalid or partial rules policy: %s\n",rules_path);
            if(kb)niyah_rule_free(kb);free(rules_text);casper_rag_free(ctx);return 3;
        }
        rules_hash_ptr=rules_hash;
    }

    char answer[2048];
    build_answer(ctx,answer,sizeof(answer));
    const char *violation=NULL;
    bool rejected=false;
    if(kb){
        violation=niyah_rule_check(kb,query,answer);
        if(violation){
            if(!strcmp(violation,"REJECTED")){rejected=true;snprintf(answer,sizeof(answer),"[Output rejected by symbolic rules]");}
            else {strncpy(answer,violation,sizeof(answer)-1);answer[sizeof(answer)-1]='\0';}
        }
    }

    uint8_t proof_bytes[32];
    niyah_proof_generate_hashed(query,answer,rules_hash_ptr,proof_bytes);
    char proof_hex[65];niyah_hash_to_hex(proof_bytes,proof_hex);
    char proof_path[256];
    int pn=snprintf(proof_path,sizeof(proof_path),"casper_%.8s.proof",proof_hex);
    if(pn<0 || (size_t)pn>=sizeof(proof_path)){if(kb)niyah_rule_free(kb);free(rules_text);casper_rag_free(ctx);return 3;}
    if(niyah_proof_save_hashed(proof_path,proof_bytes,query,answer,rules_hash_ptr)!=0){
        fprintf(stderr,"[casper] proof write failed: %s\n",proof_path);
        if(kb)niyah_rule_free(kb);
        free(rules_text);
        casper_rag_free(ctx);
        return 1;
    }

    {
        double top_relevance = ctx->n_results > 0 ? (double)ctx->results[0].score : 0.0;
        printf("{\n  \"query\":");json_str(stdout,query);printf(",\n  \"answer\":");json_str(stdout,answer);printf(",\n");
        printf("  \"relevance_score\":%.3f,\n  \"confidence\":%.3f,\n  \"confidence_kind\":\"top_lexical_relevance\",\n  \"mean_relevance\":%.3f,\n  \"elapsed_ms\":%u,\n  \"violated\":%s,\n  \"rejected\":%s,\n",
               top_relevance,top_relevance,(double)ctx->confidence,ctx->elapsed_ms,violation?"true":"false",rejected?"true":"false");
    }
    printf("  \"proof_kind\":\"NIYAH-PROOF-V2\",\n  \"rules_bound\":%s,\n  \"proof\":\"%s\",\n  \"proof_file\":",rules_hash_ptr?"true":"false",proof_hex);json_str(stdout,proof_path);printf(",\n  \"n_sources\":%d,\n  \"sources\":[\n",ctx->n_results);
    for(int i=0;i<ctx->n_results;++i){
        const RagResult *r=&ctx->results[i];char src_hex[65];niyah_hash_to_hex(r->sha256,src_hex);
        printf("    {\"n\":%d,\"score\":%.3f,\"sha256\":\"%s\",\"title\":",i+1,(double)r->score,src_hex);json_str(stdout,r->title);printf(",\"url\":");json_str(stdout,r->url);printf(",\"snippet\":");json_str(stdout,r->snippet);printf("}%s\n",i+1<ctx->n_results?",":"");
    }
    printf("  ]\n}\n");

    if(kb)niyah_rule_free(kb);
    free(rules_text);
    casper_rag_free(ctx);
    return violation?1:0;
}
