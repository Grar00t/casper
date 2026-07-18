/*
 * casper_rag.h — Retrieval-Augmented Generation pipe for Casper
 *
 * Architecture:
 *   query → [web_fetch] → [snippet_extract] → [relevance_rank]
 *         → [context_build] → [symbolic_check] → [JSON_trace]
 *
 * Search backends (no API key required):
 *   - DuckDuckGo HTML  (default, always available)
 *   - SearxNG          (self-hosted, optional — set SEARXNG_URL env var)
 *   - Bing HTML        (fallback)
 *
 * Zero external dependencies: WinHTTP on Windows, libcurl on Linux.
 * C11 clean. Thread-safe (each CasperRagCtx is independent).
 */
#ifndef CASPER_RAG_H
#define CASPER_RAG_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Limits ─────────────────────────────────────────────────────── */
#define RAG_MAX_RESULTS     8
#define RAG_URL_MAX         512
#define RAG_TITLE_MAX       256
#define RAG_SNIPPET_MAX     1024
#define RAG_CONTEXT_MAX     8192   /* assembled context fed to the model  */
#define RAG_TRACE_MAX       32     /* max reasoning steps logged          */
#define RAG_TIMEOUT_MS      7000

/* ── One web result ─────────────────────────────────────────────── */
typedef struct {
    char    url    [RAG_URL_MAX];
    char    title  [RAG_TITLE_MAX];
    char    snippet[RAG_SNIPPET_MAX];
    float   score;                  /* keyword overlap 0.0–1.0            */
    uint8_t sha256 [32];            /* SHA-256(url + snippet) proof anchor */
} RagResult;

/* ── One reasoning step ─────────────────────────────────────────── */
typedef enum {
    TRACE_PARSE    = 0,
    TRACE_SEARCH   = 1,
    TRACE_FETCH    = 2,
    TRACE_RANK     = 3,
    TRACE_CONTEXT  = 4,
    TRACE_SYMBOLIC = 5,
    TRACE_COMPOSE  = 6,
    TRACE_WARN     = 7,
} TraceKind;

typedef struct {
    TraceKind kind;
    uint32_t  elapsed_ms;
    float     confidence;
    char      detail[256];
} TraceStep;

/* ── Full RAG context for one query ─────────────────────────────── */
typedef struct {
    char       query       [512];
    RagResult  results     [RAG_MAX_RESULTS];
    int        n_results;
    char       context     [RAG_CONTEXT_MAX]; /* assembled text for model  */
    TraceStep  trace       [RAG_TRACE_MAX];
    int        n_steps;
    float      confidence;                    /* 0.0–1.0 overall           */
    bool       contradiction;                 /* symbolic check found one  */
    uint8_t    chain_hash  [32];              /* SHA-256 of full trace     */
    uint32_t   elapsed_ms;                    /* total wall-clock time      */
} RagCtx;

/* ── Backend selection ──────────────────────────────────────────── */
typedef enum {
    RAG_BACKEND_DDG     = 0,   /* DuckDuckGo HTML — default              */
    RAG_BACKEND_SEARXNG = 1,   /* SearxNG self-hosted (needs SEARXNG_URL) */
    RAG_BACKEND_BING    = 2,   /* Bing HTML — fallback                   */
} RagBackend;

/* ── Public API ─────────────────────────────────────────────────── */

/*
 * casper_rag_query()
 *
 * Full pipeline: search → fetch → rank → build context → symbolic check.
 * Returns heap-allocated RagCtx. Free with casper_rag_free().
 * Returns NULL if network is unavailable (caller handles offline mode).
 *
 * backend: which search engine to try first (falls back automatically).
 * rules_path: path to .nrule file for symbolic check, or NULL to skip.
 */
RagCtx *casper_rag_query(const char *query,
                          RagBackend  backend,
                          const char *rules_path);

/* Free a RagCtx returned by casper_rag_query(). */
void casper_rag_free(RagCtx *ctx);

/*
 * casper_rag_to_json()
 *
 * Serialize RagCtx to a malloc'd JSON string (caller frees).
 * This is what the UI workbench reads.
 *
 * Schema:
 * {
 *   "query":          "...",
 *   "confidence":     0.86,
 *   "contradiction":  false,
 *   "elapsed_ms":     412,
 *   "chain_hash":     "abcdef...",
 *   "trace": [
 *     { "step": 1, "kind": "parse",  "ms": 0,   "conf": 1.0, "detail": "..." },
 *     ...
 *   ],
 *   "sources": [
 *     { "n": 1, "url": "...", "title": "...", "snippet": "...",
 *       "sha256": "...", "score": 0.92 },
 *     ...
 *   ],
 *   "context": "assembled plain-text context fed to model"
 * }
 */
char *casper_rag_to_json(const RagCtx *ctx);

/*
 * casper_rag_online()
 *
 * Non-blocking check: can we reach the search backend?
 * Returns true if a 1-second probe succeeds.
 */
bool casper_rag_online(RagBackend backend);

#ifdef __cplusplus
}
#endif
#endif /* CASPER_RAG_H */
