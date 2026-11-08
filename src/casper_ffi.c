/* Casper FFI v1: deterministic, policy-only judgement ABI (no model inference).
 * This module has no network or external dependencies. */
#define CASPER_BUILD_DLL 1
#include "casper_ffi.h"
#include <ctype.h>
#include <math.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_REQUEST_BYTES 8192u
static volatile LONG active = 0;
static const char version[] = "casper/ffi-1.0-policy-only";

typedef struct {
    const char *p;
    const char *end;
} Reader;

static void ws(Reader *r) {
    while (r->p < r->end && (*r->p == ' ' || *r->p == '\n' ||
           *r->p == '\r' || *r->p == '\t')) ++r->p;
}
static int accept(Reader *r, char c) {
    ws(r);
    if (r->p == r->end || *r->p != c) return 0;
    ++r->p;
    return 1;
}
static int hex4(Reader *r, unsigned *cp) {
    unsigned v = 0u;
    for (int i = 0; i < 4; ++i) {
        unsigned h;
        char c;
        if (r->p == r->end) return 0;
        c = *r->p++;
        if (c >= '0' && c <= '9') h = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') h = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') h = (unsigned)(c - 'A' + 10);
        else return 0;
        v = (v << 4) | h;
    }
    *cp = v;
    return 1;
}
/* Validates a full JSON string and optionally copies unescaped ASCII text.
 * Raw Unicode is permitted for values but escaped field names are rejected. */
static int json_string(Reader *r, char *out, size_t cap, int *nonempty) {
    size_t n = 0u;
    if (r->p == r->end || *r->p++ != '"') return 0;
    while (r->p < r->end && *r->p != '"') {
        unsigned char c = (unsigned char)*r->p++;
        if (c < 0x20u) return 0;
        if (c == '\\') {
            unsigned cp;
            if (out || r->p == r->end) return 0; /* No ambiguous escaped keys. */
            c = (unsigned char)*r->p++;
            if (c == 'u') {
                if (!hex4(r, &cp)) return 0;
                if (cp >= 0xd800u && cp <= 0xdbffu) {
                    unsigned low;
                    if (r->end - r->p < 6 || r->p[0] != '\\' ||
                        r->p[1] != 'u') return 0;
                    r->p += 2;
                    if (!hex4(r, &low) || low < 0xdc00u || low > 0xdfffu)
                        return 0;
                } else if (cp >= 0xdc00u && cp <= 0xdfffu) return 0;
            } else if (!(c == '"' || c == '\\' || c == '/' || c == 'b' ||
                         c == 'f' || c == 'n' || c == 'r' || c == 't')) return 0;
        }
        if (out) {
            if (n + 1u >= cap) return 0;
            out[n] = (char)c;
        }
        ++n;
    }
    if (r->p == r->end) return 0;
    ++r->p;
    if (out) out[n] = '\0';
    if (nonempty) *nonempty = (n != 0u);
    return 1;
}
static int json_number(Reader *r, double *out) {
    const char *begin = r->p;
    char num[64];
    size_t n;
    char *tail;
    if (r->p < r->end && *r->p == '-') ++r->p;
    if (r->p == r->end) return 0;
    if (*r->p == '0') {
        ++r->p;
        if (r->p < r->end && isdigit((unsigned char)*r->p)) return 0;
    } else {
        if (*r->p < '1' || *r->p > '9') return 0;
        while (r->p < r->end && isdigit((unsigned char)*r->p)) ++r->p;
    }
    if (r->p < r->end && *r->p == '.') {
        ++r->p;
        if (r->p == r->end || !isdigit((unsigned char)*r->p)) return 0;
        while (r->p < r->end && isdigit((unsigned char)*r->p)) ++r->p;
    }
    if (r->p < r->end && (*r->p == 'e' || *r->p == 'E')) {
        ++r->p;
        if (r->p < r->end && (*r->p == '+' || *r->p == '-')) ++r->p;
        if (r->p == r->end || !isdigit((unsigned char)*r->p)) return 0;
        while (r->p < r->end && isdigit((unsigned char)*r->p)) ++r->p;
    }
    n = (size_t)(r->p - begin);
    if (n == 0u || n >= sizeof(num)) return 0;
    memcpy(num, begin, n);
    num[n] = '\0';
    *out = strtod(num, &tail);
    return *tail == '\0' && isfinite(*out);
}

static int request_parse(const char *s, int *kind, double metric[5]) {
    static const char *const keys[] = {
        "module", "verdict_kind", "target", "confidence",
        "proportionality", "risk_level", "attack_certainty",
        "target_legitimacy"
    };
    Reader r;
    unsigned mask = 0u;
    size_t len = 0u;
    if (!s) return 0;
    while (len <= MAX_REQUEST_BYTES && s[len]) ++len;
    if (len == 0u || len > MAX_REQUEST_BYTES) return 0;
    r.p = s; r.end = s + len;
    if (!accept(&r, '{')) return 0;
    for (;;) {
        char key[64], val[32];
        int field = -1, nonempty = 0;
        double value;
        ws(&r);
        if (r.p == r.end || *r.p != '"') return 0;
        if (!json_string(&r, key, sizeof(key), NULL) || !accept(&r, ':'))
            return 0;
        for (int i = 0; i < 8; ++i)
            if (strcmp(key, keys[i]) == 0) { field = i; break; }
        if (field < 0 || (mask & (1u << field))) return 0;
        mask |= (1u << field);
        ws(&r);
        if (field == 0 || field == 1) {
            if (!json_string(&r, val, sizeof(val), &nonempty) || !nonempty)
                return 0;
            if (field == 1) {
                if (strcmp(val, "ignore") == 0) *kind = 0;
                else if (strcmp(val, "report") == 0) *kind = 1;
                else if (strcmp(val, "defend") == 0) *kind = 2;
                else if (strcmp(val, "strike") == 0) *kind = 3;
                else return 0;
            }
        } else if (field == 2) {
            if (r.end - r.p >= 4 && memcmp(r.p, "null", 4) == 0)
                r.p += 4;
            else if (!json_string(&r, NULL, 0u, NULL)) return 0;
        } else {
            if (!json_number(&r, &value)) return 0;
            if (field == 4 || field == 5) {
                if (value < 0.0 || value > 10.0 || floor(value) != value) return 0;
            } else if (value < 0.0 || value > 1.0) return 0;
            metric[field - 3] = value;
        }
        ws(&r);
        if (r.p == r.end) return 0;
        if (*r.p == '}') { ++r.p; break; }
        if (*r.p++ != ',') return 0;
    }
    ws(&r);
    return r.p == r.end && mask == 0xffu;
}

CASPER_FFI_API int casper_init(const char *model_path) {
    /* No embedded model exists. Explicit paths must never silently pass. */
    if (model_path && model_path[0] != '\0') return -1;
    (void)InterlockedExchange(&active, 1);
    return 0;
}
CASPER_FFI_API int casper_judge_evaluate(const char *req_json, char *out, int out_cap) {
    static const char allow_ignore[] =
        "{\"decision\":\"allow\",\"rationale\":\"Non-action requested\"}";
    static const char allow_report[] =
        "{\"decision\":\"allow\",\"rationale\":\"Report criteria met\"}";
    static const char allow_defend[] =
        "{\"decision\":\"allow\",\"rationale\":\"Defensive criteria met\"}";
    static const char deny_strike[] =
        "{\"decision\":\"deny\",\"rationale\":\"Autonomous strike prohibited\"}";
    static const char uncertain[] =
        "{\"decision\":\"uncertain\",\"rationale\":\"Insufficient verified criteria\"}";
    const char *result = uncertain;
    double m[5] = {0};
    int kind = -1;
    size_t n;
    if (InterlockedCompareExchange(&active, 0, 0) != 1 ||
        !out || out_cap < 1 || !request_parse(req_json, &kind, m)) return -1;
    if (kind == 0) result = allow_ignore;
    else if (kind == 1 && m[0] >= 0.7 && m[4] >= 0.7) result = allow_report;
    else if (kind == 2 && m[0] >= 0.9 && m[3] >= 0.9 &&
             m[4] >= 0.8 && m[1] <= m[2]) result = allow_defend;
    else if (kind == 3) result = deny_strike;
    n = strlen(result);
    if (n >= (size_t)out_cap) return -1;
    memcpy(out, result, n + 1u);
    return (int)n;
}
CASPER_FFI_API void casper_shutdown(void) {
    (void)InterlockedExchange(&active, 0);
}
CASPER_FFI_API const char *casper_version(void) {
    return version;
}
