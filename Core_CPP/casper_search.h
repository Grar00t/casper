/*
 * casper_search.h — Web Search Pipe for Casper Engine
 *
 * Fetches search results from DDG/Bing HTML (no API key needed).
 * Returns grounded context snippets for RAG-style reasoning.
 *
 * Zero external dependencies beyond WinHTTP (Windows) / libcurl (Linux).
 * C11 clean.
 *
 * Usage:
 *   CasperSearchResult results[8];
 *   int n = casper_search("best practices SIMD AVX2", results, 8);
 *   for (int i = 0; i < n; i++) { ... results[i].snippet ... }
 *   casper_search_free(results, n);
 */

#ifndef CASPER_SEARCH_H
#define CASPER_SEARCH_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Constants
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
#define CASPER_SEARCH_MAX_RESULTS  8
#define CASPER_SEARCH_URL_MAX      512
#define CASPER_SEARCH_TITLE_MAX    256
#define CASPER_SEARCH_SNIPPET_MAX  1024
#define CASPER_SEARCH_TIMEOUT_MS   8000

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Search engines supported (tried in order on failure)
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
typedef enum {
    CASPER_ENGINE_DDG   = 0,   /* DuckDuckGo  — no API key, privacy-first */
    CASPER_ENGINE_BING  = 1,   /* Bing HTML   — no API key                */
    CASPER_ENGINE_BRAVE = 2,   /* Brave Search — no API key               */
} CasperSearchEngine;

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * One search result
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
typedef struct {
    char    url    [CASPER_SEARCH_URL_MAX];
    char    title  [CASPER_SEARCH_TITLE_MAX];
    char    snippet[CASPER_SEARCH_SNIPPET_MAX];
    uint8_t sha256 [32];    /* SHA-256 of (url + snippet) — proof anchor  */
    float   relevance;      /* 0.0–1.0 keyword overlap score              */
} CasperSearchResult;

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Reasoning trace step (what casper did, why, what it found)
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
typedef enum {
    TRACE_PARSE   = 0,  /* parsed user query                  */
    TRACE_SEARCH  = 1,  /* issued web search                  */
    TRACE_FETCH   = 2,  /* fetched URL / snippet              */
    TRACE_RANK    = 3,  /* ranked results by relevance        */
    TRACE_UNIFY   = 4,  /* symbolic unification / check       */
    TRACE_COMPOSE = 5,  /* composed final answer              */
    TRACE_WARN    = 6,  /* flagged inconsistency or low conf  */
} CasperTraceKind;

typedef struct {
    CasperTraceKind kind;
    uint32_t        step_ms;        /* wall-clock ms since query start    */
    char            detail[256];    /* human-readable, English + Arabic   */
    float           confidence;     /* 0.0–1.0                            */
} CasperTraceStep;

#define CASPER_TRACE_MAX_STEPS 32

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Full reasoning context for one query
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
typedef struct {
    char               query   [512];
    CasperSearchResult results [CASPER_SEARCH_MAX_RESULTS];
    int                n_results;
    CasperTraceStep    trace   [CASPER_TRACE_MAX_STEPS];
    int                n_steps;
    float              overall_confidence;  /* 0.0–1.0                    */
    uint8_t            chain_hash[32];      /* SHA-256 of full trace      */
    bool               has_contradiction;  /* symbolic check found conflict */
} CasperContext;

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Public API
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

/*
 * casper_search()
 *
 * Query web search engines (no API key). Fills results[] with up to
 * max_results entries. Returns actual count (0 on failure / offline).
 * Thread-safe. Caller must call casper_search_free() when done.
 */
int casper_search(const char *query,
                  CasperSearchResult *results,
                  int max_results,
                  CasperSearchEngine engine);

/*
 * casper_build_context()
 *
 * Full pipeline: search → rank → SHA-256 anchor each result → build trace.
 * Returns a heap-allocated CasperContext. Caller frees with casper_ctx_free().
 */
CasperContext *casper_build_context(const char *query,
                                    CasperSearchEngine engine);

/* Free a CasperContext returned by casper_build_context(). */
void casper_ctx_free(CasperContext *ctx);

/*
 * casper_ctx_to_json()
 *
 * Serialize a CasperContext to a JSON string (malloc'd, caller frees).
 * Format the UI workbench expects — see casper_workbench_v3.html.
 *
 * JSON schema:
 * {
 *   "query": "...",
 *   "confidence": 0.86,
 *   "has_contradiction": false,
 *   "chain_hash": "abcdef...",
 *   "trace": [
 *     {"step": 1, "kind": "parse",  "ms": 0,   "detail": "...", "conf": 1.0},
 *     ...
 *   ],
 *   "sources": [
 *     {"n": 1, "url": "...", "title": "...", "snippet": "...",
 *      "sha256": "...", "relevance": 0.92},
 *     ...
 *   ]
 * }
 */
char *casper_ctx_to_json(const CasperContext *ctx);

/*
 * casper_search_available()
 *
 * Returns true if network is reachable (non-blocking 1s probe).
 * Used to decide offline vs online mode at startup.
 */
bool casper_search_available(void);

#ifdef __cplusplus
}
#endif
#endif /* CASPER_SEARCH_H */
