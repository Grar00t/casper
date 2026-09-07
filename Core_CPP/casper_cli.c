/* casper_cli.c — Casper sovereign search agent CLI. C11. */
#include "casper_rag.h"
#include "rule_parser.h"
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

static void configure_utf8_console(void) {
#ifdef _WIN32
    (void)SetConsoleOutputCP(CP_UTF8);
    (void)SetConsoleCP(CP_UTF8);
#endif
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

/*
 * Backend selection.
 *
 * This used to be RAG_BACKEND_DDG, hardcoded at the single call site, so
 * every invocation scraped html.duckduckgo.com with no way to switch. Set
 * CASPER_BACKEND=searxng (or bing) to point somewhere else without a rebuild.
 */
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

/* Convert DuckDuckGo redirect links back to their actual destination URL. */
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

/* Keep the emitted source hash bound to the emitted normalized URL + snippet. */
static void rehash_result(RagResult *r) {
    uint8_t anchor[RAG_URL_MAX + 1u + RAG_SNIPPET_MAX];
    size_t ul;
    size_t sl;
    size_t n;

    if (!r) return;
    ul = strnlen(r->url, sizeof(r->url));
    sl = strnlen(r->snippet, sizeof(r->snippet));
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

/*
 * Defensive normalization at the CLI boundary. The RAG layer already ranks
 * results, but the CLI must never trust array order when choosing an answer
 * or emitting source order. This also normalizes DDG destinations and keeps
 * the source hashes aligned with what the CLI actually emits.
 */
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

static int cmd_verify(const char *proof_path) {
    FILE *fp=fopen(proof_path,"r");
    if(!fp){fprintf(stderr,"[casper] cannot open proof file: %s\n",proof_path);return 3;}
    char line[4096], prompt[1024]={0}, output[1024]={0};
    while(fgets(line,sizeof(line),fp)){
        if(!strncmp(line,"prompt: ",8)){size_t l=strlen(line+8);if(l&&line[8+l-1]=='\n')line[8+l-1]='\0';strncpy(prompt,line+8,sizeof(prompt)-1);prompt[sizeof(prompt)-1]='\0';}
        else if(!strncmp(line,"output: ",8)){size_t l=strlen(line+8);if(l&&line[8+l-1]=='\n')line[8+l-1]='\0';strncpy(output,line+8,sizeof(output)-1);output[sizeof(output)-1]='\0';}
    }
    fclose(fp);
    bool ok=niyah_proof_verify(proof_path,prompt,output,NULL);
    printf("{\"proof_path\":");json_str(stdout,proof_path);printf(",\"valid\":%s}\n",ok?"true":"false");
    return ok?0:1;
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
    if (ctx.n_results != RAG_MAX_RESULTS) {
        fputs("CASPER SELF-CHECK FAIL bounds\n", stderr);
        return 1;
    }
    if (ctx.results[0].score != 1.000f || strcmp(ctx.results[0].title, "best") != 0) {
        fputs("CASPER SELF-CHECK FAIL ranking\n", stderr);
        return 1;
    }

    build_answer(&ctx, answer, sizeof(answer));
    if (!strstr(answer, "best snippet")) {
        fputs("CASPER SELF-CHECK FAIL answer-source\n", stderr);
        return 1;
    }

    for (i = 0; i < ctx.n_results; ++i) {
        if (!strcmp(ctx.results[i].url, "https://example.com/low")) {
            int j;
            saw_unwrapped = 1;
            for (j = 0; j < 32; ++j) {
                if (ctx.results[i].sha256[j] != 0u) {
                    saw_hash = 1;
                    break;
                }
            }
            break;
        }
    }
    if (!saw_unwrapped) {
        fputs("CASPER SELF-CHECK FAIL ddg-url\n", stderr);
        return 1;
    }
    if (!saw_hash) {
        fputs("CASPER SELF-CHECK FAIL source-hash\n", stderr);
        return 1;
    }

    puts("CASPER SELF-CHECK PASS");
    return 0;
}

int main(int argc,char **argv){
    configure_utf8_console();
    if(argc<2){fprintf(stderr,"usage: %s <query> [rules.nrule] | --verify <proof> | --self-check\n",argv[0]);return 3;}
    if(!strcmp(argv[1],"--self-check")) return cmd_self_check();
    if(!strcmp(argv[1],"--verify")){if(argc<3)return 3;return cmd_verify(argv[2]);}

    const char *query=argv[1];
    const char *rules_path=argc>=3?argv[2]:NULL;
    RagCtx *ctx=casper_rag_query(query,pick_backend(),rules_path);
    if(!ctx){printf("{\"error\":\"rag allocation failure\"}\n");return 2;}
    if(ctx->n_results<=0){
        printf("{\n  \"query\":");json_str(stdout,query);
        printf(",\n  \"answer\":\"\",\n  \"error\":\"no results - offline or no match\",\n  \"relevance_score\":0.000,\n  \"confidence\":0.000,\n  \"confidence_kind\":\"top_lexical_relevance\",\n  \"mean_relevance\":0.000,\n  \"elapsed_ms\":%u,\n  \"violated\":false,\n  \"rejected\":false,\n  \"proof\":null,\n  \"proof_file\":null,\n  \"n_sources\":0,\n  \"sources\":[]\n}\n",ctx->elapsed_ms);
        casper_rag_free(ctx); return 2;
    }

    normalize_results(ctx);

    NiyahRuleKB *kb=NULL;
    if(rules_path){
        kb=niyah_rule_load(rules_path);
        if(!kb){fprintf(stderr,"[casper] failed to load rules: %s\n",rules_path);casper_rag_free(ctx);return 3;}
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
    niyah_proof_generate(query,answer,rules_path,proof_bytes);
    char proof_hex[65];niyah_hash_to_hex(proof_bytes,proof_hex);
    char proof_path[256];
    int pn=snprintf(proof_path,sizeof(proof_path),"casper_%.8s.proof",proof_hex);
    if(pn<0 || (size_t)pn>=sizeof(proof_path)){if(kb)niyah_rule_free(kb);casper_rag_free(ctx);return 3;}
    if(niyah_proof_save(proof_path,proof_bytes,query,answer,rules_path)!=0){
        fprintf(stderr,"[casper] proof write failed: %s\n",proof_path);
        if(kb){niyah_rule_free(kb);}
        casper_rag_free(ctx);
        return 1;
    }

    {
        double top_relevance = ctx->n_results > 0 ? (double)ctx->results[0].score : 0.0;
        printf("{\n  \"query\":");json_str(stdout,query);printf(",\n  \"answer\":");json_str(stdout,answer);printf(",\n");
        printf("  \"relevance_score\":%.3f,\n  \"confidence\":%.3f,\n  \"confidence_kind\":\"top_lexical_relevance\",\n  \"mean_relevance\":%.3f,\n  \"elapsed_ms\":%u,\n  \"violated\":%s,\n  \"rejected\":%s,\n",
               top_relevance,top_relevance,(double)ctx->confidence,ctx->elapsed_ms,violation?"true":"false",rejected?"true":"false");
    }
    printf("  \"proof\":\"%s\",\n  \"proof_file\":",proof_hex);json_str(stdout,proof_path);printf(",\n  \"n_sources\":%d,\n  \"sources\":[\n",ctx->n_results);
    for(int i=0;i<ctx->n_results;++i){
        const RagResult *r=&ctx->results[i];char src_hex[65];niyah_hash_to_hex(r->sha256,src_hex);
        printf("    {\"n\":%d,\"score\":%.3f,\"sha256\":\"%s\",\"title\":",i+1,(double)r->score,src_hex);json_str(stdout,r->title);printf(",\"url\":");json_str(stdout,r->url);printf(",\"snippet\":");json_str(stdout,r->snippet);printf("}%s\n",i+1<ctx->n_results?",":"");
    }
    printf("  ]\n}\n");

    if(kb)niyah_rule_free(kb);
    casper_rag_free(ctx);
    return violation?1:0;
}
