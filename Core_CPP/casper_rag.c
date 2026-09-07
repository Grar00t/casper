/* casper_rag.c — native DuckDuckGo retrieval/parser/ranker. C11. */
#ifndef _WIN32
#  define _POSIX_C_SOURCE 200809L
#endif

#include "casper_rag.h"
#include "proof_generator.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <winhttp.h>
#endif

#define HBUF_MAX (256u * 1024u)
#define RAG_MIN_INTERVAL_MS 2000u
#define RAG_MAX_ATTEMPTS 3
#define RAG_BACKOFF_BASE_MS 1500u
#define DDG_HOST "html.duckduckgo.com"

static const char rag_user_agent[] =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} HBuf;

static uint32_t ms_now(void) {
#ifdef _WIN32
    return (uint32_t)GetTickCount();
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return (uint32_t)((double)clock() * 1000.0 / (double)CLOCKS_PER_SEC);
    }
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
#endif
}

static void sleep_ms(uint32_t ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)((ms % 1000u) * 1000000u);
    (void)nanosleep(&ts, NULL);
#endif
}

static void throttle(void) {
    static uint32_t last_ms = 0u;
    static int have_last = 0;
    if (have_last) {
        uint32_t elapsed = ms_now() - last_ms;
        if (elapsed < RAG_MIN_INTERVAL_MS) sleep_ms(RAG_MIN_INTERVAL_MS - elapsed);
    }
    last_ms = ms_now();
    have_last = 1;
}

static HBuf *hbuf_new(void) {
    HBuf *buf = (HBuf *)calloc(1u, sizeof(*buf));
    if (!buf) return NULL;
    buf->cap = 8192u;
    buf->buf = (char *)malloc(buf->cap);
    if (!buf->buf) {
        free(buf);
        return NULL;
    }
    buf->buf[0] = '\0';
    return buf;
}

static void hbuf_free(HBuf *buf) {
    if (!buf) return;
    free(buf->buf);
    free(buf);
}

static int hbuf_append(HBuf *buf, const char *data, size_t n) {
    size_t need;
    if (!buf || (!data && n != 0u)) return -1;
    if (buf->len > HBUF_MAX - 1u || n > HBUF_MAX - buf->len - 1u) return -1;
    need = buf->len + n + 1u;
    if (need > buf->cap) {
        size_t next = buf->cap;
        char *p;
        while (next < need && next < HBUF_MAX) {
            if (next > HBUF_MAX / 2u) {
                next = HBUF_MAX;
                break;
            }
            next *= 2u;
        }
        if (next < need) return -1;
        p = (char *)realloc(buf->buf, next);
        if (!p) return -1;
        buf->buf = p;
        buf->cap = next;
    }
    if (n != 0u) memcpy(buf->buf + buf->len, data, n);
    buf->len += n;
    buf->buf[buf->len] = '\0';
    return 0;
}

static void url_encode(const char *input, char *output, size_t max) {
    static const char hex[] = "0123456789ABCDEF";
    size_t out = 0u;
    if (!output || max == 0u) return;
    if (!input) {
        output[0] = '\0';
        return;
    }
    while (*input && out + 3u < max) {
        unsigned char c = (unsigned char)*input++;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            output[out++] = (char)c;
        } else {
            output[out++] = '%';
            output[out++] = hex[c >> 4];
            output[out++] = hex[c & 0x0fu];
        }
    }
    output[out] = '\0';
}

#ifdef _WIN32
static HBuf *http_get(const char *path, unsigned *status_out) {
    HBuf *out = NULL;
    HINTERNET session = NULL;
    HINTERNET connection = NULL;
    HINTERNET request = NULL;
    wchar_t *wpath = NULL;
    int path_wlen;
    BOOL ok;

    if (status_out) *status_out = 0u;
    if (!path) return NULL;

    path_wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (path_wlen <= 0) return NULL;
    wpath = (wchar_t *)calloc((size_t)path_wlen, sizeof(wchar_t));
    if (!wpath) return NULL;
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, path_wlen)) {
        free(wpath);
        return NULL;
    }

    out = hbuf_new();
    if (!out) {
        free(wpath);
        return NULL;
    }

    session = WinHttpOpen(
        L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!session) goto fail;

    connection = WinHttpConnect(session, L"html.duckduckgo.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connection) goto fail;

    request = WinHttpOpenRequest(
        connection,
        L"GET",
        wpath,
        NULL,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!request) goto fail;

    (void)WinHttpAddRequestHeaders(
        request,
        L"Accept: text/html,application/xhtml+xml,*/*;q=0.8\r\n"
        L"Accept-Language: en-US,en;q=0.9,ar;q=0.8\r\n",
        (DWORD)-1,
        WINHTTP_ADDREQ_FLAG_ADD);
    (void)WinHttpSetTimeouts(request, RAG_TIMEOUT_MS, RAG_TIMEOUT_MS, RAG_TIMEOUT_MS, RAG_TIMEOUT_MS);

    ok = WinHttpSendRequest(
        request,
        WINHTTP_NO_ADDITIONAL_HEADERS,
        0,
        WINHTTP_NO_REQUEST_DATA,
        0,
        0,
        0) && WinHttpReceiveResponse(request, NULL);
    if (!ok) goto fail;

    {
        DWORD code = 0;
        DWORD code_size = (DWORD)sizeof(code);
        if (WinHttpQueryHeaders(
                request,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX,
                &code,
                &code_size,
                WINHTTP_NO_HEADER_INDEX)) {
            if (status_out) *status_out = (unsigned)code;
        }
    }

    while (1) {
        DWORD available = 0;
        char chunk[8192];
        DWORD read = 0;
        DWORD want;
        if (!WinHttpQueryDataAvailable(request, &available) || available == 0u) break;
        want = available < (DWORD)sizeof(chunk) ? available : (DWORD)sizeof(chunk);
        if (!WinHttpReadData(request, chunk, want, &read) || read == 0u) goto fail;
        if (hbuf_append(out, chunk, (size_t)read) != 0) goto fail;
    }

    free(wpath);
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    if (out->len == 0u) {
        hbuf_free(out);
        return NULL;
    }
    return out;

fail:
    free(wpath);
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
    hbuf_free(out);
    return NULL;
}
#else
static char *find_last_status_marker(char *text) {
    char *last = NULL;
    char *cursor = text;
    while (cursor && *cursor) {
        char *found = strstr(cursor, "\n__STATUS__");
        if (!found) break;
        last = found;
        cursor = found + 1;
    }
    return last;
}

static HBuf *http_get(const char *path, unsigned *status_out) {
    char cmd[2048];
    FILE *pipe;
    HBuf *out;
    char chunk[8192];
    size_t got;
    char *marker;

    if (status_out) *status_out = 0u;
    if (!path) return NULL;
    if (snprintf(
            cmd,
            sizeof(cmd),
            "curl -sS -L --max-time %u --compressed "
            "-A '%s' "
            "-H 'Accept: text/html,application/xhtml+xml,*/*;q=0.8' "
            "-H 'Accept-Language: en-US,en;q=0.9,ar;q=0.8' "
            "-w '\\n__STATUS__%%{http_code}' "
            "'https://" DDG_HOST "%s' 2>/dev/null",
            (unsigned)(RAG_TIMEOUT_MS / 1000u),
            rag_user_agent,
            path) >= (int)sizeof(cmd)) {
        return NULL;
    }

    pipe = popen(cmd, "r");
    if (!pipe) return NULL;
    out = hbuf_new();
    if (!out) {
        (void)pclose(pipe);
        return NULL;
    }

    while ((got = fread(chunk, 1u, sizeof(chunk), pipe)) > 0u) {
        if (hbuf_append(out, chunk, got) != 0) {
            hbuf_free(out);
            (void)pclose(pipe);
            return NULL;
        }
    }
    (void)pclose(pipe);

    marker = out->len ? find_last_status_marker(out->buf) : NULL;
    if (marker) {
        if (status_out) *status_out = (unsigned)strtoul(marker + 11, NULL, 10);
        *marker = '\0';
        out->len = (size_t)(marker - out->buf);
    }
    if (out->len == 0u) {
        hbuf_free(out);
        return NULL;
    }
    return out;
}
#endif

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *text) {
    char *read;
    char *write;
    if (!text) return;
    read = text;
    write = text;
    while (*read) {
        if (read[0] == '%' && read[1] && read[2]) {
            int hi = hex_value(read[1]);
            int lo = hex_value(read[2]);
            if (hi >= 0 && lo >= 0) {
                *write++ = (char)((hi << 4) | lo);
                read += 3;
                continue;
            }
        }
        *write++ = (*read == '+') ? ' ' : *read;
        ++read;
    }
    *write = '\0';
}

static void strip_tags(const char *html, char *out, size_t max) {
    size_t n = 0u;
    int in_tag = 0;
    const char *p;
    if (!out || max == 0u) return;
    out[0] = '\0';
    if (!html) return;

    for (p = html; *p && n + 1u < max; ++p) {
        if (*p == '<') {
            in_tag = 1;
            continue;
        }
        if (*p == '>') {
            in_tag = 0;
            if (n != 0u && out[n - 1u] != ' ') out[n++] = ' ';
            continue;
        }
        if (in_tag) continue;

        if (*p == '&') {
            if (!strncmp(p, "&amp;", 5u)) { out[n++] = '&'; p += 4; }
            else if (!strncmp(p, "&lt;", 4u)) { out[n++] = '<'; p += 3; }
            else if (!strncmp(p, "&gt;", 4u)) { out[n++] = '>'; p += 3; }
            else if (!strncmp(p, "&quot;", 6u)) { out[n++] = '"'; p += 5; }
            else if (!strncmp(p, "&#39;", 5u)) { out[n++] = '\''; p += 4; }
            else if (!strncmp(p, "&nbsp;", 6u)) { out[n++] = ' '; p += 5; }
            else out[n++] = '&';
            continue;
        }

        if (*p == '\r' || *p == '\n' || *p == '\t') {
            if (n != 0u && out[n - 1u] != ' ') out[n++] = ' ';
        } else {
            out[n++] = *p;
        }
    }
    while (n != 0u && out[n - 1u] == ' ') --n;
    out[n] = '\0';
}

static int parse_ddg(const char *html, RagResult *results, int max_results) {
    int count = 0;
    const char *cursor = html;
    if (!html || !results || max_results <= 0) return 0;

    while (count < max_results && cursor && *cursor) {
        const char *href = strstr(cursor, "result__a\" href=\"");
        const char *end;
        const char *amp;
        const char *gt;
        const char *title_end;
        const char *snippet;
        size_t len;

        if (href) {
            href += 17;
        } else {
            href = strstr(cursor, "/l/?uddg=");
            if (!href) break;
            href += 9;
        }

        end = strchr(href, '"');
        amp = strchr(href, '&');
        if (amp && (!end || amp < end)) end = amp;
        if (!end) break;

        len = (size_t)(end - href);
        if (len >= RAG_URL_MAX) len = RAG_URL_MAX - 1u;
        memcpy(results[count].url, href, len);
        results[count].url[len] = '\0';
        url_decode(results[count].url);
        cursor = end;

        gt = strchr(cursor, '>');
        if (!gt) break;
        title_end = strstr(gt + 1, "</a>");
        if (!title_end) title_end = strstr(gt + 1, "</");
        if (title_end) {
            char raw[RAG_TITLE_MAX * 2u];
            size_t title_len = (size_t)(title_end - (gt + 1));
            if (title_len >= sizeof(raw)) title_len = sizeof(raw) - 1u;
            memcpy(raw, gt + 1, title_len);
            raw[title_len] = '\0';
            strip_tags(raw, results[count].title, sizeof(results[count].title));
            cursor = title_end;
        } else {
            results[count].title[0] = '\0';
        }

        snippet = strstr(cursor, "result__snippet");
        if (snippet) {
            const char *snippet_start = strchr(snippet, '>');
            if (snippet_start) {
                const char *snippet_end;
                ++snippet_start;
                snippet_end = strstr(snippet_start, "</");
                if (snippet_end) {
                    char raw[RAG_SNIPPET_MAX * 2u];
                    size_t snippet_len = (size_t)(snippet_end - snippet_start);
                    if (snippet_len >= sizeof(raw)) snippet_len = sizeof(raw) - 1u;
                    memcpy(raw, snippet_start, snippet_len);
                    raw[snippet_len] = '\0';
                    strip_tags(raw, results[count].snippet, sizeof(results[count].snippet));
                    cursor = snippet_end;
                }
            }
        }

        if (!strncmp(results[count].url, "http://", 7u) ||
            !strncmp(results[count].url, "https://", 8u)) {
            ++count;
        } else {
            ++cursor;
        }
    }
    return count;
}

static void ascii_lower_copy(char *out, size_t max, const char *in) {
    size_t i = 0u;
    if (!out || max == 0u) return;
    if (!in) {
        out[0] = '\0';
        return;
    }
    while (in[i] && i + 1u < max) {
        unsigned char c = (unsigned char)in[i];
        out[i] = c < 0x80u ? (char)tolower(c) : (char)c;
        ++i;
    }
    out[i] = '\0';
}

static int is_query_delimiter(unsigned char c) {
    return isspace(c) || c == ',' || c == '.' || c == ';' || c == ':' ||
           c == '!' || c == '?' || c == '(' || c == ')' || c == '[' ||
           c == ']' || c == '{' || c == '}' || c == '"' || c == '\'';
}

static float score_relevance(const char *query, const RagResult *result) {
    char title[RAG_TITLE_MAX];
    char snippet[RAG_SNIPPET_MAX];
    const unsigned char *p;
    int total = 0;
    int hits = 0;

    if (!query || !result) return 0.0f;
    ascii_lower_copy(title, sizeof(title), result->title);
    ascii_lower_copy(snippet, sizeof(snippet), result->snippet);
    p = (const unsigned char *)query;

    while (*p) {
        char token[128];
        size_t n = 0u;
        while (*p && is_query_delimiter(*p)) ++p;
        while (*p && !is_query_delimiter(*p)) {
            unsigned char c = *p++;
            if (n + 1u < sizeof(token)) token[n++] = c < 0x80u ? (char)tolower(c) : (char)c;
        }
        token[n] = '\0';
        if (n >= 3u) {
            ++total;
            if (strstr(title, token) || strstr(snippet, token)) ++hits;
        }
    }
    return total ? (float)hits / (float)total : 0.0f;
}

static int result_compare(const void *a, const void *b) {
    const RagResult *left = (const RagResult *)a;
    const RagResult *right = (const RagResult *)b;
    int url_cmp;
    if (left->score < right->score) return 1;
    if (left->score > right->score) return -1;
    url_cmp = strcmp(left->url, right->url);
    return url_cmp != 0 ? url_cmp : strcmp(left->title, right->title);
}

static void trace_add(
    RagCtx *ctx,
    TraceKind kind,
    float confidence,
    uint32_t start_ms,
    const char *format,
    ...) {
    TraceStep *step;
    va_list args;
    if (!ctx || ctx->n_steps >= RAG_TRACE_MAX) return;
    step = &ctx->trace[ctx->n_steps++];
    step->kind = kind;
    step->elapsed_ms = ms_now() - start_ms;
    step->confidence = confidence;
    va_start(args, format);
    (void)vsnprintf(step->detail, sizeof(step->detail), format, args);
    va_end(args);
}

static HBuf *http_get_retry(const char *path, RagCtx *ctx, uint32_t start_ms) {
    int attempt;
    for (attempt = 1; attempt <= RAG_MAX_ATTEMPTS; ++attempt) {
        unsigned status = 0u;
        HBuf *response;
        throttle();
        response = http_get(path, &status);
        if (response && (status == 200u || status == 0u)) return response;
        hbuf_free(response);

        if (status == 403u || status == 429u || status == 503u) {
            uint32_t wait = RAG_BACKOFF_BASE_MS << (attempt - 1);
            trace_add(ctx, TRACE_WARN, 0.0f, start_ms,
                      "http %u rate-limited; wait %u ms (%d/%d)",
                      status, wait, attempt, RAG_MAX_ATTEMPTS);
            sleep_ms(wait);
            continue;
        }
        if (status != 0u) trace_add(ctx, TRACE_WARN, 0.0f, start_ms, "http %u", status);
        return NULL;
    }
    trace_add(ctx, TRACE_WARN, 0.0f, start_ms, "blocked after %d attempts", RAG_MAX_ATTEMPTS);
    return NULL;
}

RagCtx *casper_rag_query(const char *query) {
    RagCtx *ctx;
    uint32_t start_ms;
    char encoded[512];
    char path[640];
    HBuf *response;
    float score_sum = 0.0f;
    size_t context_len = 0u;
    int i;

    if (!query || !query[0]) return NULL;
    ctx = (RagCtx *)calloc(1u, sizeof(*ctx));
    if (!ctx) return NULL;
    start_ms = ms_now();
    (void)snprintf(ctx->query, sizeof(ctx->query), "%s", query);
    trace_add(ctx, TRACE_PARSE, 0.0f, start_ms, "parsed %zu bytes", strlen(query));

    url_encode(query, encoded, sizeof(encoded));
    if (snprintf(path, sizeof(path), "/html/?q=%s", encoded) >= (int)sizeof(path)) {
        trace_add(ctx, TRACE_WARN, 0.0f, start_ms, "query too long");
        ctx->elapsed_ms = ms_now() - start_ms;
        return ctx;
    }
    trace_add(ctx, TRACE_SEARCH, 0.0f, start_ms, "GET https://" DDG_HOST "%s", path);

    response = http_get_retry(path, ctx, start_ms);
    if (!response || response->len == 0u) {
        trace_add(ctx, TRACE_WARN, 0.0f, start_ms, "no response");
        hbuf_free(response);
        ctx->elapsed_ms = ms_now() - start_ms;
        return ctx;
    }

    trace_add(ctx, TRACE_FETCH, 1.0f, start_ms, "received %zu bytes", response->len);
    ctx->n_results = parse_ddg(response->buf, ctx->results, RAG_MAX_RESULTS);
    hbuf_free(response);

    for (i = 0; i < ctx->n_results; ++i) {
        uint8_t anchor[RAG_URL_MAX + 1u + RAG_SNIPPET_MAX];
        size_t url_len = strlen(ctx->results[i].url);
        size_t snippet_len = strlen(ctx->results[i].snippet);
        size_t anchor_len = url_len + 1u + snippet_len;

        ctx->results[i].score = score_relevance(query, &ctx->results[i]);
        score_sum += ctx->results[i].score;
        memcpy(anchor, ctx->results[i].url, url_len);
        anchor[url_len] = 0u;
        memcpy(anchor + url_len + 1u, ctx->results[i].snippet, snippet_len);
        niyah_sha256(anchor, anchor_len, ctx->results[i].sha256);
    }

    qsort(ctx->results, (size_t)ctx->n_results, sizeof(ctx->results[0]), result_compare);
    ctx->confidence = ctx->n_results ? score_sum / (float)ctx->n_results : 0.0f;
    trace_add(ctx, TRACE_RANK, ctx->confidence, start_ms, "results %d", ctx->n_results);

    for (i = 0; i < ctx->n_results; ++i) {
        int written;
        size_t added;
        if (context_len >= RAG_CONTEXT_MAX - 1u) break;
        written = snprintf(
            ctx->context + context_len,
            RAG_CONTEXT_MAX - context_len,
            "[%d] %s\n%s\n\n",
            i + 1,
            ctx->results[i].title,
            ctx->results[i].snippet);
        if (written < 0) break;
        added = (size_t)written;
        if (added >= RAG_CONTEXT_MAX - context_len) {
            context_len = RAG_CONTEXT_MAX - 1u;
            break;
        }
        context_len += added;
    }

    trace_add(ctx, TRACE_CONTEXT, ctx->confidence, start_ms, "context %zu bytes", context_len);
    niyah_sha256((const uint8_t *)ctx->context, context_len, ctx->context_sha256);
    ctx->elapsed_ms = ms_now() - start_ms;
    return ctx;
}

void casper_rag_free(RagCtx *ctx) {
    free(ctx);
}
