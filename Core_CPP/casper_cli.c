/* casper_cli.c — Casper retrieval CLI. C11. */
#include "casper_rag.h"
#include "rule_parser.h"
#include "proof_generator.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void json_str(FILE *fp, const char *s) {
    const unsigned char *p;
    fputc('"', fp);
    if (s) {
        for (p=(const unsigned char *)s; *p; ++p) {
            switch (*p) {
                case '"': fputs("\\\"",fp); break;
                case '\\': fputs("\\\\",fp); break;
                case '\n': fputs("\\n",fp); break;
                case '\r': fputs("\\r",fp); break;
                case '\t': fputs("\\t",fp); break;
                default:
                    if (*p < 0x20u) fprintf(fp,"\\u%04x",(unsigned)*p);
                    else fputc((int)*p,fp);
                    break;
            }
        }
    }
    fputc('"', fp);
}

static int cmd_verify(const char *integrity_path) {
    bool ok = niyah_proof_verify_stored(integrity_path);
    printf("{\"integrity_file\":");
    json_str(stdout, integrity_path);
    printf(",\"valid\":%s}\n", ok ? "true" : "false");
    return ok ? 0 : 1;
}

static void build_answer(const RagCtx *ctx, char *out, size_t max) {
    const RagResult *top;
    if (!ctx || !out || !max) return;
    if (ctx->n_results <= 0) {
        (void)snprintf(out, max, "No sources found for this query.");
        return;
    }
    top = &ctx->results[0];
    (void)snprintf(out, max, "Source: %s\n\n%s",
                   top->title[0] ? top->title : top->url,
                   top->snippet[0] ? top->snippet : "(no snippet)");
}

int main(int argc, char **argv) {
    const char *query;
    const char *rules_path;
    RagCtx *ctx;
    NiyahRuleKB *kb = NULL;
    char answer[2048];
    const char *violation = NULL;
    bool rejected = false;
    uint8_t integrity_bytes[32];
    char integrity_hex[65];
    char integrity_path[256];
    int path_len;
    int i;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <query> [rules.nrule] | --verify <integrity-file>\n", argv[0]);
        return 3;
    }
    if (!strcmp(argv[1], "--verify")) {
        if (argc < 3) return 3;
        return cmd_verify(argv[2]);
    }

    query = argv[1];
    rules_path = argc >= 3 ? argv[2] : NULL;
    ctx = casper_rag_query(query);
    if (!ctx) {
        printf("{\"error\":\"rag allocation failure\"}\n");
        return 2;
    }
    if (ctx->n_results <= 0) {
        printf("{\"query\":");
        json_str(stdout, query);
        printf(",\"error\":\"no results - transport failure, backend rejection, or no match\",\"confidence\":0.0}\n");
        casper_rag_free(ctx);
        return 2;
    }

    if (rules_path) {
        kb = niyah_rule_load(rules_path);
        if (!kb) {
            fprintf(stderr, "[casper] failed to load rules: %s\n", rules_path);
            casper_rag_free(ctx);
            return 3;
        }
    }

    build_answer(ctx, answer, sizeof(answer));
    if (kb) {
        violation = niyah_rule_check(kb, query, answer);
        if (violation) {
            if (!strcmp(violation, "REJECTED")) {
                rejected = true;
                (void)snprintf(answer, sizeof(answer), "[Output rejected by rules]");
            } else {
                strncpy(answer, violation, sizeof(answer)-1u);
                answer[sizeof(answer)-1u] = '\0';
            }
        }
    }

    if (niyah_proof_generate(query, answer, rules_path, integrity_bytes) != 0) {
        fprintf(stderr, "[casper] failed to hash rule contents: %s\n", rules_path ? rules_path : "(none)");
        if (kb) niyah_rule_free(kb);
        casper_rag_free(ctx);
        return 3;
    }
    niyah_hash_to_hex(integrity_bytes, integrity_hex);
    path_len = snprintf(integrity_path, sizeof(integrity_path), "casper_%.8s.integrity", integrity_hex);
    if (path_len < 0 || (size_t)path_len >= sizeof(integrity_path)) {
        if (kb) niyah_rule_free(kb);
        casper_rag_free(ctx);
        return 3;
    }
    if (niyah_proof_save(integrity_path, integrity_bytes, query, answer, rules_path) != 0) {
        fprintf(stderr, "[casper] integrity record write failed: %s\n", integrity_path);
        if (kb) niyah_rule_free(kb);
        casper_rag_free(ctx);
        return 1;
    }

    printf("{\n  \"query\":");
    json_str(stdout, query);
    printf(",\n  \"answer\":");
    json_str(stdout, answer);
    printf(",\n");
    printf("  \"confidence\":%.3f,\n  \"elapsed_ms\":%u,\n  \"rule_violated\":%s,\n  \"rejected\":%s,\n",
           (double)ctx->confidence, ctx->elapsed_ms,
           violation ? "true" : "false", rejected ? "true" : "false");
    printf("  \"integrity_sha256\":\"%s\",\n  \"integrity_file\":", integrity_hex);
    json_str(stdout, integrity_path);
    printf(",\n  \"n_sources\":%d,\n  \"sources\":[\n", ctx->n_results);
    for (i=0;i<ctx->n_results;++i) {
        const RagResult *result=&ctx->results[i];
        char source_hex[65];
        niyah_hash_to_hex(result->sha256, source_hex);
        printf("    {\"n\":%d,\"score\":%.3f,\"sha256\":\"%s\",\"title\":",
               i+1, (double)result->score, source_hex);
        json_str(stdout, result->title);
        printf(",\"url\":");
        json_str(stdout, result->url);
        printf(",\"snippet\":");
        json_str(stdout, result->snippet);
        printf("}%s\n", i+1<ctx->n_results ? "," : "");
    }
    printf("  ]\n}\n");

    if (kb) niyah_rule_free(kb);
    casper_rag_free(ctx);
    return violation ? 1 : 0;
}
