/*
 * casper_cli.c — Casper sovereign search agent, CLI entry point.
 *
 * Links: casper_rag.c  rule_parser.c  proof_generator.c
 *
 * Usage:
 *   casper.exe "your question here"
 *   casper.exe "your question here"  path\to\rules.nrule
 *   casper.exe --verify  path\to\output.proof
 *
 * Output (stdout): JSON
 *   { "query":..., "confidence":..., "answer":...,
 *     "proof": "abcdef...", "sources":[...], "violated": false }
 *
 * Exit codes:
 *   0  clean answer produced
 *   1  rule violation — answer replaced or rejected
 *   2  no results (offline or no match)
 *   3  bad arguments
 *
 * Build (MSVC, run from repo root):
 *   cl /nologo /W4 /WX /wd4996 /O2 /std:c17 /arch:AVX2 ^
 *      Core_CPP\casper_cli.c Core_CPP\casper_rag.c       ^
 *      Core_CPP\rule_parser.c Core_CPP\proof_generator.c ^
 *      /Fe:casper.exe /link winhttp.lib
 *
 * Build (GCC/Linux):
 *   gcc -O2 -std=c11 -Wall -Wextra -Werror                \
 *       Core_CPP/casper_cli.c Core_CPP/casper_rag.c        \
 *       Core_CPP/rule_parser.c Core_CPP/proof_generator.c  \
 *       -o casper -lm
 */

#include "casper_rag.h"
#include "rule_parser.h"
#include "proof_generator.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ── helpers ──────────────────────────────────────────────────────────────── */

/* Write a JSON-escaped string to fp (handles \, ", \n, \r, \t). */
static void json_str(FILE *fp, const char *s) {
    fputc('"', fp);
    if (!s) { fputc('"', fp); return; }
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
            case '"':  fputs("\\\"", fp); break;
            case '\\': fputs("\\\\", fp); break;
            case '\n': fputs("\\n",  fp); break;
            case '\r': fputs("\\r",  fp); break;
            case '\t': fputs("\\t",  fp); break;
            default:
                if (*p < 0x20) fprintf(fp, "\\u%04x", *p);
                else           fputc(*p, fp);
        }
    }
    fputc('"', fp);
}

/* Build a plain-text answer from RAG context (first 1 KB of assembled ctx). */
static void build_answer(const RagCtx *ctx, char *out, size_t max) {
    if (ctx->n_results == 0) {
        snprintf(out, max, "No sources found for this query.");
        return;
    }
    /* Lead with top-ranked snippet — honest, traceable. */
    const RagResult *top = &ctx->results[0];
    snprintf(out, max,
        "Source: %s\n\n%s",
        top->title[0] ? top->title : top->url,
        top->snippet[0] ? top->snippet : "(no snippet)");
}

/* ── --verify mode ────────────────────────────────────────────────────────── */

static int cmd_verify(const char *proof_path) {
    /* Re-open the .proof file and print whether hash matches stored value.
     * niyah_proof_verify() re-computes SHA-256(prompt||output||rules)
     * and compares against the stored hash line. */
    FILE *fp = fopen(proof_path, "r");
    if (!fp) {
        fprintf(stderr, "[casper] cannot open proof file: %s\n", proof_path);
        return 3;
    }
    /* Read prompt / output / rules lines from the .proof file header. */
    char line[4096];
    char prompt[1024] = {0}, output[1024] = {0};
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "prompt: ", 8) == 0) {
            size_t l = strlen(line + 8);
            if (l > 0 && line[8 + l - 1] == '\n') line[8 + l - 1] = '\0';
            strncpy(prompt, line + 8, sizeof(prompt) - 1);
        } else if (strncmp(line, "output: ", 8) == 0) {
            size_t l = strlen(line + 8);
            if (l > 0 && line[8 + l - 1] == '\n') line[8 + l - 1] = '\0';
            strncpy(output, line + 8, sizeof(output) - 1);
        }
    }
    fclose(fp);

    bool ok = niyah_proof_verify(proof_path, prompt, output, NULL);
    printf("{\"proof_path\":\"%s\",\"valid\":%s}\n",
           proof_path, ok ? "true" : "false");
    return ok ? 0 : 1;
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr,
            "Usage:\n"
            "  casper.exe \"question\"  [rules.nrule]\n"
            "  casper.exe --verify    proof_file.proof\n");
        return 3;
    }

    /* --verify mode */
    if (strcmp(argv[1], "--verify") == 0) {
        if (argc < 3) {
            fprintf(stderr, "[casper] --verify requires a .proof path\n");
            return 3;
        }
        return cmd_verify(argv[2]);
    }

    const char *query      = argv[1];
    const char *rules_path = (argc >= 3) ? argv[2] : NULL;

    /* ── Lobe 1: Perceptual retrieval ─────────────────────────────────────
     * casper_rag_query() does:
     *   1. URL-encode query
     *   2. GET html.duckduckgo.com/html/?q=...  (WinHTTP / libcurl)
     *   3. Parse result__a + result__snippet blocks
     *   4. Score each result: hits(query_words ∩ title+snippet) / total_words
     *   5. Sort descending by score
     *   6. SHA-256(url + snippet) per result — proof anchor
     *   7. Assemble ctx->context (plain text, ≤8192 bytes)
     * Falls back to Bing HTML if DDG returns 0 results.
     * Returns NULL if both fail (offline path).
     */
    RagCtx *ctx = casper_rag_query(query, RAG_BACKEND_DDG, rules_path);

    if (!ctx || ctx->n_results == 0) {
        printf("{\"query\":");
        json_str(stdout, query);
        printf(",\"error\":\"no results — offline or no match\",\"confidence\":0.0}\n");
        if (ctx) casper_rag_free(ctx);
        return 2;
    }

    /* ── Lobe 2: Symbolic rule check ──────────────────────────────────────
     * rule_parser reads .nrule file (linked list of NiyahRule).
     * niyah_rule_check() walks every rule:
     *   IF-THEN: all conditions must match (case-insensitive CONTAINS/EQUALS)
     *   ALWAYS:  always fires, checks action
     * Returns NULL (pass) or replacement string / "REJECTED".
     */
    NiyahRuleKB  *kb        = rules_path ? niyah_rule_load(rules_path) : NULL;
    char          answer[2048];
    build_answer(ctx, answer, sizeof(answer));

    const char *violation = NULL;
    bool        rejected  = false;

    if (kb) {
        violation = niyah_rule_check(kb, query, answer);
        if (violation) {
            if (strcmp(violation, "REJECTED") == 0) {
                rejected = true;
                strncpy(answer, "[Output rejected by symbolic rules]",
                        sizeof(answer) - 1);
            } else {
                /* Rule provides a replacement string */
                strncpy(answer, violation, sizeof(answer) - 1);
            }
            answer[sizeof(answer) - 1] = '\0';
        }
    }

    /* ── Lobe 3: Proof generation ─────────────────────────────────────────
     * SHA-256(query ∥ 0x00 ∥ answer ∥ 0x00 ∥ rule_file_contents)
     * Writes casper_<hex8>.proof next to the binary.
     * proof_hash lets anyone re-verify offline: re-hash same inputs,
     * compare against stored value.
     */
    uint8_t proof_bytes[32];
    niyah_proof_generate(query, answer, rules_path, proof_bytes);

    char proof_hex[65];
    niyah_hash_to_hex(proof_bytes, proof_hex);

    /* Save .proof sidecar */
    char proof_path_out[256];
    snprintf(proof_path_out, sizeof(proof_path_out),
             "casper_%.8s.proof", proof_hex);
    niyah_proof_save(proof_path_out, proof_bytes, query, answer, rules_path);

    /* ── Emit JSON to stdout ──────────────────────────────────────────────
     * Downstream: casper_workbench_v3.html reads this via fetch() or
     * a named pipe. The UI maps sources[] to the Proof Ledger panel.
     */
    printf("{\n");
    printf("  \"query\":      "); json_str(stdout, query);      printf(",\n");
    printf("  \"answer\":     "); json_str(stdout, answer);     printf(",\n");
    printf("  \"confidence\": %.3f,\n", (double)ctx->confidence);
    printf("  \"elapsed_ms\": %u,\n",  ctx->elapsed_ms);
    printf("  \"violated\":   %s,\n",  violation ? "true" : "false");
    printf("  \"rejected\":   %s,\n",  rejected  ? "true" : "false");
    printf("  \"proof\":      \"%s\",\n", proof_hex);
    printf("  \"proof_file\": \"%s\",\n", proof_path_out);
    printf("  \"n_sources\":  %d,\n",  ctx->n_results);
    printf("  \"sources\": [\n");
    for (int i = 0; i < ctx->n_results; i++) {
        const RagResult *r = &ctx->results[i];
        char src_hex[65];
        niyah_hash_to_hex(r->sha256, src_hex);
        printf("    {\"n\":%d, \"score\":%.3f, \"sha256\":\"%s\",\n",
               i + 1, (double)r->score, src_hex);
        printf("     \"title\":"); json_str(stdout, r->title);   printf(",\n");
        printf("     \"url\":  "); json_str(stdout, r->url);     printf(",\n");
        printf("     \"snippet\":"); json_str(stdout, r->snippet); printf("}");
        if (i + 1 < ctx->n_results) printf(",");
        printf("\n");
    }
    printf("  ]\n");
    printf("}\n");

    /* Cleanup */
    if (kb)  niyah_rule_free(kb);
    casper_rag_free(ctx);

    return violation ? 1 : 0;
}
