/* casper_rag.h — minimal web retrieval for the native Casper CLI. */
#ifndef CASPER_RAG_H
#define CASPER_RAG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RAG_MAX_RESULTS 8
#define RAG_URL_MAX 512
#define RAG_TITLE_MAX 256
#define RAG_SNIPPET_MAX 1024
#define RAG_CONTEXT_MAX 8192
#define RAG_TRACE_MAX 32
#define RAG_TIMEOUT_MS 7000

typedef struct {
    char url[RAG_URL_MAX];
    char title[RAG_TITLE_MAX];
    char snippet[RAG_SNIPPET_MAX];
    float score;
    uint8_t sha256[32];
} RagResult;

typedef enum {
    TRACE_PARSE = 0,
    TRACE_SEARCH = 1,
    TRACE_FETCH = 2,
    TRACE_RANK = 3,
    TRACE_CONTEXT = 4,
    TRACE_WARN = 5
} TraceKind;

typedef struct {
    TraceKind kind;
    uint32_t elapsed_ms;
    float confidence;
    char detail[256];
} TraceStep;

typedef struct {
    char query[512];
    RagResult results[RAG_MAX_RESULTS];
    int n_results;
    char context[RAG_CONTEXT_MAX];
    TraceStep trace[RAG_TRACE_MAX];
    int n_steps;
    float confidence;
    uint8_t context_sha256[32];
    uint32_t elapsed_ms;
} RagCtx;

/*
 * Query DuckDuckGo's HTML endpoint and rank returned snippets lexically.
 * Windows uses WinHTTP. POSIX uses the curl executable at runtime.
 */
RagCtx *casper_rag_query(const char *query);
void casper_rag_free(RagCtx *ctx);

#ifdef __cplusplus
}
#endif
#endif
