#include "casper_chronicle.h"
#include "proof_generator.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if !defined(_WIN32)
#include <sys/resource.h>
#endif

#define CHR_FIELD_MAX 128u
#define CHR_MAX_EVENTS 100000u
#define CHR_MAGIC "CASPER-CHRON-V1"
#define CHR_MAGIC_LEN 16u

typedef struct { int64_t num; int64_t den; } ChrRat;
typedef struct {
    uint8_t event_id[32], span_id[32];
    uint64_t byte_start, byte_end, line_start, line_end;
    uint8_t span_hash[32];
    int amount_present;
    ChrRat amount;
    char subject[CHR_FIELD_MAX], predicate[CHR_FIELD_MAX], object[CHR_FIELD_MAX];
    char currency[CHR_FIELD_MAX], time_expr[CHR_FIELD_MAX], modality[CHR_FIELD_MAX];
    char polarity[CHR_FIELD_MAX], status[CHR_FIELD_MAX];
} ChrEvent;
typedef struct {
    uint8_t document_hash[32];
    uint64_t source_size, imported_at;
    uint8_t *source;
    ChrEvent *events;
    uint32_t event_count;
} ChrStore;
typedef struct { char *data; size_t len, cap; } ChrBuf;

void casper_chronicle_free(void *ptr) { free(ptr); }
static void store_free(ChrStore *s) {
    if (!s) return;
    free(s->source); free(s->events); memset(s, 0, sizeof(*s));
}
static int buf_reserve(ChrBuf *b, size_t extra) {
    size_t need; char *next;
    if (extra > SIZE_MAX - b->len - 1u) return -1;
    need = b->len + extra + 1u;
    if (need <= b->cap) return 0;
    if (b->cap == 0u) b->cap = 256u;
    while (b->cap < need) {
        if (b->cap > SIZE_MAX / 2u) { b->cap = need; break; }
        b->cap *= 2u;
    }
    next = (char *)realloc(b->data, b->cap);
    if (!next) return -1;
    b->data = next; return 0;
}
static int buf_addn(ChrBuf *b, const char *s, size_t n) {
    if (buf_reserve(b, n) != 0) return -1;
    memcpy(b->data + b->len, s, n); b->len += n; b->data[b->len] = '\0'; return 0;
}
static int buf_add(ChrBuf *b, const char *s) { return buf_addn(b, s, strlen(s)); }
static int buf_add_u64(ChrBuf *b, uint64_t v) {
    char tmp[32]; int n = snprintf(tmp, sizeof(tmp), "%" PRIu64, v);
    return n < 0 ? -1 : buf_addn(b, tmp, (size_t)n);
}
static int buf_add_i64(ChrBuf *b, int64_t v) {
    char tmp[32]; int n = snprintf(tmp, sizeof(tmp), "%" PRId64, v);
    return n < 0 ? -1 : buf_addn(b, tmp, (size_t)n);
}
static int buf_add_json(ChrBuf *b, const uint8_t *s, size_t n) {
    size_t i;
    if (buf_add(b, "\"") != 0) return -1;
    for (i = 0u; i < n; ++i) {
        unsigned char c = s[i]; char esc[7];
        if (c == '"' || c == '\\') {
            esc[0] = '\\'; esc[1] = (char)c;
            if (buf_addn(b, esc, 2u) != 0) return -1;
        } else if (c == '\n') { if (buf_add(b, "\\n") != 0) return -1; }
        else if (c == '\r') { if (buf_add(b, "\\r") != 0) return -1; }
        else if (c == '\t') { if (buf_add(b, "\\t") != 0) return -1; }
        else if (c < 0x20u) {
            int m = snprintf(esc, sizeof(esc), "\\u%04x", (unsigned)c);
            if (m != 6 || buf_addn(b, esc, 6u) != 0) return -1;
        } else if (buf_addn(b, (const char *)&s[i], 1u) != 0) return -1;
    }
    return buf_add(b, "\"");
}
static int utf8_valid(const uint8_t *s, size_t n) {
    size_t i = 0u;
    while (i < n) {
        uint8_t c = s[i]; uint32_t cp; size_t extra;
        if (c == 0u) return 0;
        if (c < 0x80u) { ++i; continue; }
        if ((c & 0xe0u) == 0xc0u) { cp = c & 0x1fu; extra = 1u; if (cp < 2u) return 0; }
        else if ((c & 0xf0u) == 0xe0u) { cp = c & 0x0fu; extra = 2u; }
        else if ((c & 0xf8u) == 0xf0u) { cp = c & 0x07u; extra = 3u; }
        else return 0;
        if (extra > n - i - 1u) return 0;
        while (extra-- > 0u) {
            uint8_t d = s[++i];
            if ((d & 0xc0u) != 0x80u) return 0;
            cp = (cp << 6) | (uint32_t)(d & 0x3fu);
        }
        if (cp > 0x10ffffu || (cp >= 0xd800u && cp <= 0xdfffu)) return 0;
        if ((cp < 0x800u && c >= 0xe0u) || (cp < 0x10000u && c >= 0xf0u)) return 0;
        ++i;
    }
    return 1;
}
static int read_file(const char *path, uint8_t **data, size_t *size) {
    FILE *f; long end; uint8_t *p;
    if (!path || !data || !size) return -1;
    f = fopen(path, "rb"); if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0 || (end = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f); return -1;
    }
    p = (uint8_t *)malloc((size_t)end + 1u); if (!p) { fclose(f); return -1; }
    if ((size_t)end != fread(p, 1u, (size_t)end, f) || fclose(f) != 0) { free(p); return -1; }
    p[(size_t)end] = 0u; *data = p; *size = (size_t)end; return 0;
}
static int64_t gcd64(int64_t a, int64_t b) {
    while (b != 0) { int64_t t = a % b; a = b; b = t; }
    return a < 0 ? -a : a;
}
static int parse_rat(const char *s, ChrRat *out) {
    char *slash, *end1, *end2; long long n, d = 1; char tmp[CHR_FIELD_MAX]; int64_t g;
    size_t len = strlen(s);
    if (len == 0u || len >= sizeof(tmp)) return -1;
    memcpy(tmp, s, len + 1u); slash = strchr(tmp, '/'); if (slash) *slash++ = '\0';
    errno = 0; n = strtoll(tmp, &end1, 10);
    if (errno || *end1 != '\0' || n < 0) return -1;
    if (slash) { errno = 0; d = strtoll(slash, &end2, 10); if (errno || *end2 != '\0' || d <= 0) return -1; }
    g = gcd64((int64_t)n, (int64_t)d); out->num = (int64_t)n / g; out->den = (int64_t)d / g; return 0;
}
static int rat_cmp(ChrRat a, ChrRat b) {
#if defined(__SIZEOF_INT128__)
#  if defined(__GNUC__)
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wpedantic"
#  endif
    __int128 left = (__int128)a.num * b.den;
    __int128 right = (__int128)b.num * a.den;
#  if defined(__GNUC__)
#    pragma GCC diagnostic pop
#  endif
    return left < right ? -1 : left > right ? 1 : 0;
#else
    if (a.den == b.den) return a.num < b.num ? -1 : a.num > b.num ? 1 : 0;
    return 0;
#endif
}
static int rat_add(ChrRat a, ChrRat b, ChrRat *out) {
    int64_t g = gcd64(a.den, b.den), x = b.den / g, y = a.den / g;
    int64_t left, right, den, norm;
    if ((a.num && x > INT64_MAX / a.num) || (b.num && y > INT64_MAX / b.num) || a.den > INT64_MAX / x) return -1;
    left = a.num * x; right = b.num * y;
    if (right > INT64_MAX - left) return -1;
    den = a.den * x; norm = gcd64(left + right, den);
    out->num = (left + right) / norm; out->den = den / norm; return 0;
}
static void put_u32(FILE *f, uint32_t v, int *ok) {
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    if (*ok && fwrite(b, 1u, sizeof(b), f) != sizeof(b)) *ok = 0;
}
static void put_u64(FILE *f, uint64_t v, int *ok) {
    uint8_t b[8]; size_t i;
    for (i = 0u; i < 8u; ++i) b[i] = (uint8_t)(v >> (8u * i));
    if (*ok && fwrite(b, 1u, sizeof(b), f) != sizeof(b)) *ok = 0;
}
static int get_u32(FILE *f, uint32_t *v) {
    uint8_t b[4];
    if (fread(b, 1u, sizeof(b), f) != sizeof(b)) return -1;
    *v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24); return 0;
}
static int get_u64(FILE *f, uint64_t *v) {
    uint8_t b[8]; size_t i;
    if (fread(b, 1u, sizeof(b), f) != sizeof(b)) return -1;
    *v = 0u; for (i = 0u; i < 8u; ++i) *v |= (uint64_t)b[i] << (8u * i); return 0;
}
static void put_text(FILE *f, const char *s, int *ok) {
    size_t n = strlen(s);
    if (n >= CHR_FIELD_MAX) { *ok = 0; return; }
    put_u32(f, (uint32_t)n, ok);
    if (*ok && n && fwrite(s, 1u, n, f) != n) *ok = 0;
}
static int get_text(FILE *f, char out[CHR_FIELD_MAX]) {
    uint32_t n;
    if (get_u32(f, &n) != 0 || n >= CHR_FIELD_MAX) return -1;
    if (n && fread(out, 1u, n, f) != n) return -1;
    out[n] = '\0'; return 0;
}
static void hash_event_ids(const uint8_t doc[32], uint64_t start, uint64_t end,
                           const uint8_t *line, size_t line_len, uint8_t event_id[32], uint8_t span_id[32]) {
    uint8_t *m = (uint8_t *)malloc(56u + line_len); size_t i;
    if (!m) { memset(event_id, 0, 32u); memset(span_id, 0, 32u); return; }
    memcpy(m, "event-v1", 8u); memcpy(m + 8u, doc, 32u);
    for (i = 0u; i < 8u; ++i) m[40u + i] = (uint8_t)(start >> (8u * i));
    for (i = 0u; i < 8u; ++i) m[48u + i] = (uint8_t)(end >> (8u * i));
    memcpy(m + 56u, line, line_len); niyah_sha256(m, 56u + line_len, event_id);
    memcpy(m, "span-v1 ", 8u); niyah_sha256(m, 56u + line_len, span_id); free(m);
}
static int field_copy(char out[CHR_FIELD_MAX], const char *s, size_t n) {
    if (n == 0u || n >= CHR_FIELD_MAX) return -1;
    memcpy(out, s, n); out[n] = '\0'; return 0;
}
static int parse_event_line(const uint8_t *line, size_t len, uint64_t start,
                            uint64_t line_no, const uint8_t doc[32], ChrEvent *e) {
    const char prefix[] = "@chronicle\t"; const char *fields[9]; size_t sizes[9], i;
    size_t pos = sizeof(prefix) - 1u;
    if (len < pos || memcmp(line, prefix, pos) != 0) return 1;
    for (i = 0u; i < 9u; ++i) {
        size_t begin = pos;
        while (pos < len && line[pos] != '\t') ++pos;
        fields[i] = (const char *)line + begin; sizes[i] = pos - begin;
        if (i < 8u) { if (pos == len) return -1; ++pos; }
    }
    if (pos != len) return -1;
    memset(e, 0, sizeof(*e));
    if (field_copy(e->subject, fields[0], sizes[0]) != 0 ||
        field_copy(e->predicate, fields[1], sizes[1]) != 0 ||
        field_copy(e->object, fields[2], sizes[2]) != 0 ||
        field_copy(e->currency, fields[4], sizes[4]) != 0 ||
        field_copy(e->time_expr, fields[5], sizes[5]) != 0 ||
        field_copy(e->modality, fields[6], sizes[6]) != 0 ||
        field_copy(e->polarity, fields[7], sizes[7]) != 0 ||
        field_copy(e->status, fields[8], sizes[8]) != 0) return -1;
    if (!(strcmp(e->predicate, "BORROWED_FROM") == 0 || strcmp(e->predicate, "PAID_TO") == 0 ||
          strcmp(e->predicate, "INTERMEDIARY_FOR") == 0 || strcmp(e->predicate, "TRANSFER_TO") == 0)) return -1;
    if (!(strcmp(e->polarity, "POSITIVE") == 0 || strcmp(e->polarity, "NEGATIVE") == 0)) return -1;
    if (!(strcmp(e->status, "ASSERTED") == 0 || strcmp(e->status, "CONFIRMED") == 0 ||
          strcmp(e->status, "PENDING") == 0 || strcmp(e->status, "DENIED") == 0)) return -1;
    if (sizes[3] == 1u && fields[3][0] == '-') { e->amount_present = 0; e->amount = (ChrRat){0, 1}; }
    else {
        char amount[CHR_FIELD_MAX];
        if (field_copy(amount, fields[3], sizes[3]) != 0 || parse_rat(amount, &e->amount) != 0) return -1;
        e->amount_present = 1;
    }
    e->byte_start = start; e->byte_end = start + len; e->line_start = line_no; e->line_end = line_no;
    niyah_sha256(line, len, e->span_hash);
    hash_event_ids(doc, e->byte_start, e->byte_end, line, len, e->event_id, e->span_id); return 0;
}
static int source_size(const char *path, uint64_t *size) {
    FILE *f = fopen(path, "rb"); long end;
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0 || (end = ftell(f)) < 0 || fclose(f) != 0) return -1;
    *size = (uint64_t)end; return 0;
}
static int extract_events_file(const char *path, ChrStore *s) {
    FILE *f = fopen(path, "rb"); uint8_t *line = NULL; size_t len = 0u, cap = 0u;
    uint64_t offset = 0u, line_start = 0u, line_no = 1u; uint32_t count = 0u; int ch, rc = 0;
    if (!f) return -1;
    while ((ch = fgetc(f)) != EOF) {
        if (ch == '\n') {
            size_t event_len = len;
            ChrEvent parsed;
            if (event_len && line[event_len - 1u] == '\r') --event_len;
            if (!utf8_valid(line, event_len)) { rc = -1; break; }
            rc = parse_event_line(line, event_len, line_start, line_no, s->document_hash, &parsed);
            if (rc < 0) break;
            if (rc == 0) {
                ChrEvent *next;
                if (count >= CHR_MAX_EVENTS) { rc = -1; break; }
                next = (ChrEvent *)realloc(s->events, ((size_t)count + 1u) * sizeof(*next));
                if (!next) { rc = -1; break; }
                s->events = next; s->events[count++] = parsed;
            }
            len = 0u; ++offset; line_start = offset; ++line_no; rc = 0; continue;
        }
        if (len == cap) {
            size_t next_cap = cap ? cap * 2u : 256u;
            uint8_t *next;
            if (next_cap < cap) { rc = -1; break; }
            next = (uint8_t *)realloc(line, next_cap);
            if (!next) { rc = -1; break; }
            line = next; cap = next_cap;
        }
        line[len++] = (uint8_t)ch; ++offset;
    }
    if (rc == 0 && ferror(f)) rc = -1;
    if (rc == 0 && len) {
        size_t event_len = len; ChrEvent parsed;
        if (event_len && line[event_len - 1u] == '\r') --event_len;
        if (!utf8_valid(line, event_len)) rc = -1;
        else {
            rc = parse_event_line(line, event_len, line_start, line_no, s->document_hash, &parsed);
            if (rc == 0) {
                ChrEvent *next = (ChrEvent *)realloc(s->events, ((size_t)count + 1u) * sizeof(*next));
                if (!next || count >= CHR_MAX_EVENTS) rc = -1;
                else { s->events = next; s->events[count++] = parsed; }
            }
            if (rc == 1) rc = 0;
        }
    }
    free(line);
    if (fclose(f) != 0) rc = -1;
    if (rc == 0) s->event_count = count;
    return rc;
}
static int store_save_from_file(const char *path, const char *source_path, const ChrStore *s) {
    FILE *f = fopen(path, "wb"); uint32_t i; int ok = f != NULL;
    FILE *source = NULL; uint8_t chunk[65536]; size_t n, copied = 0u;
    if (!f) return -1;
    if (fwrite(CHR_MAGIC, 1u, CHR_MAGIC_LEN, f) != CHR_MAGIC_LEN) ok = 0;
    if (ok && fwrite(s->document_hash, 1u, 32u, f) != 32u) ok = 0;
    put_u64(f, s->source_size, &ok); put_u64(f, s->imported_at, &ok); put_u32(f, s->event_count, &ok);
    if (ok) source = fopen(source_path, "rb");
    if (!source) ok = 0;
    while (ok && (n = fread(chunk, 1u, sizeof(chunk), source)) > 0u) {
        if (fwrite(chunk, 1u, n, f) != n) ok = 0;
        copied += n;
    }
    if (source && (ferror(source) || fclose(source) != 0)) ok = 0;
    if ((uint64_t)copied != s->source_size) ok = 0;
    for (i = 0u; ok && i < s->event_count; ++i) {
        const ChrEvent *e = &s->events[i];
        if (fwrite(e->event_id, 1u, 32u, f) != 32u || fwrite(e->span_id, 1u, 32u, f) != 32u) ok = 0;
        put_u64(f, e->byte_start, &ok); put_u64(f, e->byte_end, &ok);
        put_u64(f, e->line_start, &ok); put_u64(f, e->line_end, &ok);
        if (ok && fwrite(e->span_hash, 1u, 32u, f) != 32u) ok = 0;
        if (ok && fputc(e->amount_present, f) == EOF) ok = 0;
        put_u64(f, (uint64_t)e->amount.num, &ok); put_u64(f, (uint64_t)e->amount.den, &ok);
        put_text(f, e->subject, &ok); put_text(f, e->predicate, &ok); put_text(f, e->object, &ok);
        put_text(f, e->currency, &ok); put_text(f, e->time_expr, &ok); put_text(f, e->modality, &ok);
        put_text(f, e->polarity, &ok); put_text(f, e->status, &ok);
    }
    if (fclose(f) != 0) ok = 0;
    if (!ok) remove(path);
    return ok ? 0 : -1;
}
static int store_load(const char *path, ChrStore *s) {
    FILE *f = fopen(path, "rb"); char magic[CHR_MAGIC_LEN]; uint32_t i; uint8_t actual[32]; int tail;
    memset(s, 0, sizeof(*s));
    if (!f || fread(magic, 1u, sizeof(magic), f) != sizeof(magic) || memcmp(magic, CHR_MAGIC, CHR_MAGIC_LEN) != 0 ||
        fread(s->document_hash, 1u, 32u, f) != 32u || get_u64(f, &s->source_size) != 0 ||
        get_u64(f, &s->imported_at) != 0 || get_u32(f, &s->event_count) != 0 || s->event_count > CHR_MAX_EVENTS ||
        s->source_size > SIZE_MAX - 1u) { if (f) fclose(f); return -1; }
    s->source = (uint8_t *)malloc((size_t)s->source_size + 1u);
    s->events = s->event_count ? (ChrEvent *)calloc(s->event_count, sizeof(*s->events)) : NULL;
    if (!s->source || (s->event_count && !s->events) ||
        (s->source_size && fread(s->source, 1u, (size_t)s->source_size, f) != (size_t)s->source_size)) {
        fclose(f); store_free(s); return -1;
    }
    s->source[s->source_size] = 0u;
    for (i = 0u; i < s->event_count; ++i) {
        ChrEvent *e = &s->events[i]; uint8_t present; uint64_t num, den;
        if (fread(e->event_id, 1u, 32u, f) != 32u || fread(e->span_id, 1u, 32u, f) != 32u ||
            get_u64(f, &e->byte_start) != 0 || get_u64(f, &e->byte_end) != 0 ||
            get_u64(f, &e->line_start) != 0 || get_u64(f, &e->line_end) != 0 ||
            fread(e->span_hash, 1u, 32u, f) != 32u || (tail = fgetc(f)) == EOF ||
            get_u64(f, &num) != 0 || get_u64(f, &den) != 0 ||
            get_text(f, e->subject) != 0 || get_text(f, e->predicate) != 0 || get_text(f, e->object) != 0 ||
            get_text(f, e->currency) != 0 || get_text(f, e->time_expr) != 0 || get_text(f, e->modality) != 0 ||
            get_text(f, e->polarity) != 0 || get_text(f, e->status) != 0) {
            fclose(f); store_free(s); return -1;
        }
        present = (uint8_t)tail; e->amount_present = present ? 1 : 0;
        e->amount.num = (int64_t)num; e->amount.den = (int64_t)den;
        if (present > 1u || den == 0u || e->byte_start > e->byte_end || e->byte_end > s->source_size) {
            fclose(f); store_free(s); return -1;
        }
    }
    tail = fgetc(f);
    if (tail != EOF || ferror(f) || fclose(f) != 0 || !utf8_valid(s->source, (size_t)s->source_size)) {
        store_free(s); return -1;
    }
    niyah_sha256(s->source, (size_t)s->source_size, actual);
    if (memcmp(actual, s->document_hash, 32u) != 0) { store_free(s); return -1; }
    for (i = 0u; i < s->event_count; ++i) {
        ChrEvent *e = &s->events[i];
        niyah_sha256(s->source + e->byte_start, (size_t)(e->byte_end - e->byte_start), actual);
        if (memcmp(actual, e->span_hash, 32u) != 0) { store_free(s); return -1; }
    }
    return 0;
}
static char *path_suffix(const char *path, const char *suffix) {
    size_t a = strlen(path), b = strlen(suffix); char *out = (char *)malloc(a + b + 1u);
    if (!out) return NULL;
    memcpy(out, path, a); memcpy(out + a, suffix, b + 1u); return out;
}
static int write_hex(FILE *f, const uint8_t *p, size_t n) {
    static const char x[] = "0123456789abcdef"; size_t i;
    for (i = 0u; i < n; ++i)
        if (fputc(x[p[i] >> 4], f) == EOF || fputc(x[p[i] & 15u], f) == EOF) return -1;
    return 0;
}
static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static uint8_t *decode_hex(const char *s, size_t *n) {
    size_t len = strlen(s), i; uint8_t *p;
    if ((len & 1u) != 0u) return NULL;
    p = (uint8_t *)malloc(len / 2u + 1u); if (!p) return NULL;
    for (i = 0u; i < len; i += 2u) {
        int a = hex_nibble(s[i]), b = hex_nibble(s[i + 1u]);
        if (a < 0 || b < 0) { free(p); return NULL; }
        p[i / 2u] = (uint8_t)((a << 4) | b);
    }
    p[len / 2u] = 0u; *n = len / 2u; return p;
}
static int write_receipt(const char *path, const char *kind, const char *store_path,
                         const char *question, const uint8_t result_hash[32],
                         const uint8_t document_hash[32]) {
    FILE *f = fopen(path, "wb"); uint8_t store_hash[32]; char hex[65];
    int ok = f != NULL && niyah_sha256_file(store_path, store_hash);
    if (!f) return -1;
    if (ok && fprintf(f, "CASPER-CHRONICLE-INTEGRITY-RECEIPT-V1\nkind:%s\nstore_path_hex:", kind) < 0) ok = 0;
    if (ok && write_hex(f, (const uint8_t *)store_path, strlen(store_path)) != 0) ok = 0;
    if (ok && fputs("\nquestion_hex:", f) == EOF) ok = 0;
    if (ok && write_hex(f, (const uint8_t *)(question ? question : ""), question ? strlen(question) : 0u) != 0) ok = 0;
    niyah_hash_to_hex(store_hash, hex); if (ok && fprintf(f, "\nstore_sha256:%s\n", hex) < 0) ok = 0;
    niyah_hash_to_hex(document_hash, hex); if (ok && fprintf(f, "document_sha256:%s\n", hex) < 0) ok = 0;
    niyah_hash_to_hex(result_hash, hex); if (ok && fprintf(f, "result_sha256:%s\n", hex) < 0) ok = 0;
    if (fclose(f) != 0) ok = 0;
    if (!ok) remove(path);
    return ok ? 0 : -1;
}
static int same_tuple(const ChrEvent *a, const ChrEvent *b) {
    return strcmp(a->subject, b->subject) == 0 && strcmp(a->predicate, b->predicate) == 0 &&
           strcmp(a->object, b->object) == 0 && a->amount_present == b->amount_present &&
           (!a->amount_present || rat_cmp(a->amount, b->amount) == 0) && strcmp(a->currency, b->currency) == 0;
}
static int opposite_conflict(const ChrStore *s, const ChrEvent *focus) {
    uint32_t i;
    for (i = 0u; i < s->event_count; ++i) {
        const ChrEvent *e = &s->events[i];
        if (e != focus && same_tuple(e, focus) && strcmp(e->polarity, focus->polarity) != 0) return 1;
    }
    return 0;
}
static int is_positive(const ChrEvent *e) {
    return strcmp(e->polarity, "POSITIVE") == 0 && strcmp(e->status, "DENIED") != 0;
}
static int is_intermediary(const ChrStore *s, const char *person, const char *lender, uint32_t *index) {
    uint32_t i;
    for (i = 0u; i < s->event_count; ++i) {
        const ChrEvent *e = &s->events[i];
        if (strcmp(e->predicate, "INTERMEDIARY_FOR") == 0 && strcmp(e->subject, person) == 0 &&
            strcmp(e->object, lender) == 0 && is_positive(e)) {
            if (index) *index = i;
            return 1;
        }
    }
    return 0;
}
static int entity_boundary(const unsigned char *p) {
    if (*p == '\0') return 1;
    if (*p < 0x80u) return !((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'Z') ||
                             (*p >= 'a' && *p <= 'z') || *p == '_');
    return (p[0] == 0xd8u && (p[1] == 0x8cu || p[1] == 0x9bu || p[1] == 0x9fu));
}
static int contains_entity(const char *text, const char *entity) {
    const char *p = text; size_t n = strlen(entity);
    while ((p = strstr(p, entity)) != NULL) {
        int left = p == text || entity_boundary((const unsigned char *)p - 1u);
        int right = entity_boundary((const unsigned char *)p + n);
        if (left && right) return 1;
        ++p;
    }
    return 0;
}
static int add_evidence_json(ChrBuf *b, const ChrStore *s, const ChrEvent *e, int comma) {
    char hex[65];
    if (comma && buf_add(b, ",") != 0) return -1;
    niyah_hash_to_hex(e->event_id, hex);
    if (buf_add(b, "{\"event_id\":\"") != 0 || buf_add(b, hex) != 0) return -1;
    niyah_hash_to_hex(e->span_id, hex);
    if (buf_add(b, "\",\"evidence_span_id\":\"") != 0 || buf_add(b, hex) != 0) return -1;
    niyah_hash_to_hex(s->document_hash, hex);
    if (buf_add(b, "\",\"document_sha256\":\"") != 0 || buf_add(b, hex) != 0 ||
        buf_add(b, "\",\"byte_start\":") != 0 || buf_add_u64(b, e->byte_start) != 0 ||
        buf_add(b, ",\"byte_end\":") != 0 || buf_add_u64(b, e->byte_end) != 0 ||
        buf_add(b, ",\"line_start\":") != 0 || buf_add_u64(b, e->line_start) != 0 ||
        buf_add(b, ",\"line_end\":") != 0 || buf_add_u64(b, e->line_end) != 0) return -1;
    niyah_hash_to_hex(e->span_hash, hex);
    if (buf_add(b, ",\"exact_span_sha256\":\"") != 0 || buf_add(b, hex) != 0 ||
        buf_add(b, "\",\"text\":") != 0 ||
        buf_add_json(b, s->source + e->byte_start, (size_t)(e->byte_end - e->byte_start)) != 0 ||
        buf_add(b, "}") != 0) return -1;
    return 0;
}
static int reason_json(const ChrStore *s, const char *question, char **out) {
    const ChrEvent *debt = NULL; uint32_t debt_i = 0u, i;
    ChrRat settled = {0, 1}; int uncertain = 0, conflict = 0, evidence_count = 0;
    uint8_t *used = (uint8_t *)calloc(s->event_count ? s->event_count : 1u, 1u);
    const char *status; ChrBuf b = {0};
    if (!used) return -1;
    for (i = 0u; i < s->event_count; ++i) {
        const ChrEvent *e = &s->events[i];
        if (strcmp(e->predicate, "BORROWED_FROM") == 0 && is_positive(e) && e->amount_present &&
            contains_entity(question, e->subject) && contains_entity(question, e->object)) {
            debt = e; debt_i = i; break;
        }
    }
    if (debt) { used[debt_i] = 1u; conflict = opposite_conflict(s, debt); }
    if (debt && !conflict) for (i = 0u; i < s->event_count; ++i) {
        const ChrEvent *e = &s->events[i]; uint32_t intermediary_i = 0u;
        int direct, via_intermediary, eligible;
        if (!is_positive(e) || !e->amount_present || strcmp(e->subject, debt->subject) != 0 ||
            !(strcmp(e->predicate, "PAID_TO") == 0 || strcmp(e->predicate, "TRANSFER_TO") == 0)) continue;
        direct = strcmp(e->object, debt->object) == 0;
        via_intermediary = is_intermediary(s, e->object, debt->object, &intermediary_i);
        eligible = strcmp(e->predicate, "PAID_TO") == 0
                   ? strcmp(e->status, "PENDING") != 0
                   : strcmp(e->status, "CONFIRMED") == 0;
        if (opposite_conflict(s, e)) { conflict = 1; used[i] = 1u; continue; }
        if (eligible && (direct || via_intermediary)) {
            if (rat_add(settled, e->amount, &settled) != 0) { conflict = 1; break; }
            used[i] = 1u; if (via_intermediary) used[intermediary_i] = 1u;
        } else { uncertain = 1; used[i] = 1u; }
    }
    if (conflict) status = "CONFLICT";
    else if (!debt) status = "UNKNOWN";
    else if (rat_cmp(settled, debt->amount) > 0) status = "CONFLICT";
    else if (rat_cmp(settled, debt->amount) == 0) status = "SUPPORTED";
    else if (settled.num > 0) status = "PARTIAL";
    else status = uncertain ? "UNKNOWN" : "UNKNOWN";
    if (buf_add(&b, "{\"status\":\"") != 0 || buf_add(&b, status) != 0 ||
        buf_add(&b, "\",\"question\":") != 0 ||
        buf_add_json(&b, (const uint8_t *)question, strlen(question)) != 0 ||
        buf_add(&b, ",\"debt\":") != 0) goto fail;
    if (debt) {
        if (buf_add(&b, "{\"num\":") != 0 || buf_add_i64(&b, debt->amount.num) != 0 ||
            buf_add(&b, ",\"den\":") != 0 || buf_add_i64(&b, debt->amount.den) != 0 ||
            buf_add(&b, ",\"currency\":") != 0 ||
            buf_add_json(&b, (const uint8_t *)debt->currency, strlen(debt->currency)) != 0 ||
            buf_add(&b, "}") != 0) goto fail;
    } else if (buf_add(&b, "null") != 0) goto fail;
    if (buf_add(&b, ",\"settled\":{\"num\":") != 0 || buf_add_i64(&b, settled.num) != 0 ||
        buf_add(&b, ",\"den\":") != 0 || buf_add_i64(&b, settled.den) != 0 ||
        buf_add(&b, "},\"limits\":\"explicit @chronicle records only; integrity is not truth\",\"evidence\":[") != 0)
        goto fail;
    for (i = 0u; i < s->event_count; ++i) if (used[i]) {
        if (add_evidence_json(&b, s, &s->events[i], evidence_count != 0) != 0) goto fail;
        ++evidence_count;
    }
    if (buf_add(&b, "]}") != 0) goto fail;
    free(used); *out = b.data; return 0;
fail:
    free(used); free(b.data); return -1;
}
int casper_chronicle_ingest(const char *input_path, char **store_path_out, char **receipt_path_out) {
    ChrStore s, old; char *store_path = NULL, *receipt_path = NULL;
    uint8_t result_hash[32]; int same = 0;
    if (!input_path || !store_path_out || !receipt_path_out) return 2;
    memset(&s, 0, sizeof(s)); memset(&old, 0, sizeof(old));
    store_path = path_suffix(input_path, ".chronicle");
    if (store_path) receipt_path = path_suffix(store_path, ".receipt");
    if (!store_path || !receipt_path || source_size(input_path, &s.source_size) != 0 ||
        !niyah_sha256_file(input_path, s.document_hash)) goto fail;
    s.imported_at = (uint64_t)time(NULL);
    if (store_load(store_path, &old) == 0) {
        same = old.source_size == s.source_size && memcmp(old.document_hash, s.document_hash, 32u) == 0;
        store_free(&old);
    }
    if (!same && (extract_events_file(input_path, &s) != 0 ||
                  store_save_from_file(store_path, input_path, &s) != 0)) goto fail;
    if (!niyah_sha256_file(store_path, result_hash) ||
        write_receipt(receipt_path, "INGEST", store_path, "", result_hash, s.document_hash) != 0) goto fail;
    store_free(&s); *store_path_out = store_path; *receipt_path_out = receipt_path; return 0;
fail:
    store_free(&s); store_free(&old); free(store_path); free(receipt_path); return 1;
}
int casper_chronicle_query(const char *store_path, const char *question,
                           char **json_out, char **receipt_path_out) {
    ChrStore s; char *json = NULL, *suffix = NULL, *receipt = NULL;
    uint8_t qh[32], rh[32]; char qhex[65];
    memset(&s, 0, sizeof(s));
    if (!store_path || !question || !json_out || !receipt_path_out ||
        !utf8_valid((const uint8_t *)question, strlen(question))) return 2;
    if (store_load(store_path, &s) != 0 || reason_json(&s, question, &json) != 0) {
        store_free(&s); return 1;
    }
    niyah_sha256((const uint8_t *)question, strlen(question), qh); niyah_hash_to_hex(qh, qhex);
    suffix = (char *)malloc(34u);
    if (suffix) snprintf(suffix, 34u, ".query-%.16s.receipt", qhex);
    if (suffix) receipt = path_suffix(store_path, suffix);
    niyah_sha256((const uint8_t *)json, strlen(json), rh);
    if (!suffix || !receipt || write_receipt(receipt, "QUERY", store_path, question, rh, s.document_hash) != 0) {
        free(json); free(suffix); free(receipt); store_free(&s); return 1;
    }
    free(suffix); store_free(&s); *json_out = json; *receipt_path_out = receipt; return 0;
}
static int line_value(char *line, const char *key, char **value) {
    size_t n = strlen(key);
    if (strncmp(line, key, n) != 0) return 0;
    *value = line + n; return 1;
}
int casper_chronicle_verify(const char *receipt_path) {
    FILE *f; char line[8192], kind[16] = "", store_hex[4096] = "", question_hex[4096] = "";
    char store_hash_hex[65] = "", doc_hex[65] = "", result_hex[65] = "", *v;
    uint8_t *store_bytes = NULL, *question_bytes = NULL;
    size_t store_n = 0u, question_n = 0u;
    ChrStore s; uint8_t actual[32]; char actual_hex[65]; char *json = NULL; int ok = 0;
    memset(&s, 0, sizeof(s));
    if (!receipt_path || !(f = fopen(receipt_path, "rb"))) return 1;
    if (!fgets(line, sizeof(line), f) ||
        (strcmp(line, "CASPER-CHRONICLE-INTEGRITY-RECEIPT-V1\n") != 0 &&
         strcmp(line, "CASPER-CHRONICLE-INTEGRITY-RECEIPT-V1\r\n") != 0)) {
        fclose(f); return 1;
    }
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line_value(line, "kind:", &v)) snprintf(kind, sizeof(kind), "%s", v);
        else if (line_value(line, "store_path_hex:", &v)) snprintf(store_hex, sizeof(store_hex), "%s", v);
        else if (line_value(line, "question_hex:", &v)) snprintf(question_hex, sizeof(question_hex), "%s", v);
        else if (line_value(line, "store_sha256:", &v)) snprintf(store_hash_hex, sizeof(store_hash_hex), "%s", v);
        else if (line_value(line, "document_sha256:", &v)) snprintf(doc_hex, sizeof(doc_hex), "%s", v);
        else if (line_value(line, "result_sha256:", &v)) snprintf(result_hex, sizeof(result_hex), "%s", v);
    }
    if (ferror(f) || fclose(f) != 0) return 1;
    store_bytes = decode_hex(store_hex, &store_n); question_bytes = decode_hex(question_hex, &question_n);
    if (!store_bytes || !question_bytes || memchr(store_bytes, 0, store_n) || memchr(question_bytes, 0, question_n))
        goto done;
    if (!niyah_sha256_file((const char *)store_bytes, actual)) goto done;
    niyah_hash_to_hex(actual, actual_hex); if (strcmp(actual_hex, store_hash_hex) != 0) goto done;
    if (store_load((const char *)store_bytes, &s) != 0) goto done;
    niyah_hash_to_hex(s.document_hash, actual_hex);
    if (strcmp(actual_hex, doc_hex) != 0) goto done_store;
    if (strcmp(kind, "INGEST") == 0) niyah_sha256_file((const char *)store_bytes, actual);
    else if (strcmp(kind, "QUERY") == 0 &&
             reason_json(&s, (const char *)question_bytes, &json) == 0)
        niyah_sha256((const uint8_t *)json, strlen(json), actual);
    else goto done_store;
    niyah_hash_to_hex(actual, actual_hex); ok = strcmp(actual_hex, result_hex) == 0;
done_store:
    store_free(&s);
done:
    free(store_bytes); free(question_bytes); free(json); return ok ? 0 : 1;
}
static int write_bytes(const char *path, const uint8_t *p, size_t n) {
    FILE *f = fopen(path, "wb"); int ok;
    if (!f) return -1;
    ok = fwrite(p, 1u, n, f) == n;
    if (fclose(f) != 0) ok = 0;
    return ok ? 0 : -1;
}
static int expect_status(const char *json, const char *status) {
    char expected[64];
    snprintf(expected, sizeof(expected), "\"status\":\"%s\"", status);
    return strstr(json, expected) != NULL;
}
static void remove_query_receipt(const char *path, const char *question) {
    uint8_t hash[32]; char hex[65], suffix[34]; char *full;
    niyah_sha256((const uint8_t *)question, strlen(question), hash); niyah_hash_to_hex(hash, hex);
    snprintf(suffix, sizeof(suffix), ".query-%.16s.receipt", hex);
    full = path_suffix(path, suffix);
    if (full) { remove(full); free(full); }
}
int casper_chronicle_self_test(void) {
    static const char story[] =
        "بداية القصة عن أحمد وخالد.\r\n"
        "@chronicle\tأحمد\tBORROWED_FROM\tخالد\t100/1\tSAR\tاليوم الأول\tASSERTED\tPOSITIVE\tCONFIRMED\r\n"
        "تفاصيل بعيدة لا تغيّر الدين.\r\n"
        "@chronicle\tسعود\tINTERMEDIARY_FOR\tخالد\t-\tSAR\tلاحقًا\tASSERTED\tPOSITIVE\tCONFIRMED\r\n"
        "@chronicle\tأحمد\tPAID_TO\tسعود\t40/1\tSAR\tاليوم الثاني\tASSERTED\tPOSITIVE\tCONFIRMED\r\n"
        "ثم ظهرت حوالة لم يثبت وصولها.\r\n"
        "@chronicle\tأحمد\tTRANSFER_TO\tسعود\t60/1\tSAR\tاليوم الثالث\tUNCERTAIN\tPOSITIVE\tPENDING\r\n"
        "اسم مختلف: أحمدان.\r\n";
    static const char conflict_story[] =
        "@chronicle\tأحمد\tBORROWED_FROM\tخالد\t100/1\tSAR\tT1\tASSERTED\tPOSITIVE\tCONFIRMED\n"
        "@chronicle\tأحمد\tBORROWED_FROM\tخالد\t100/1\tSAR\tT1\tASSERTED\tNEGATIVE\tCONFIRMED\n";
    static const char missing_link_story[] =
        "@chronicle\tأحمد\tBORROWED_FROM\tخالد\t100/1\tSAR\tT1\tASSERTED\tPOSITIVE\tCONFIRMED\n"
        "@chronicle\tأحمد\tPAID_TO\tسعود\t40/1\tSAR\tT2\tASSERTED\tPOSITIVE\tCONFIRMED\n";
    const char *path = "chronicle-self-test.txt";
    const char *conflict_path = "chronicle-conflict-test.txt";
    const char *missing_path = "chronicle-missing-test.txt";
    const char *question = "هل سدد أحمد دين خالد؟";
    char *store = NULL, *receipt = NULL, *json = NULL, *qreceipt = NULL;
    char *store2 = NULL, *receipt2 = NULL, *json2 = NULL, *qreceipt2 = NULL;
    char *store3 = NULL, *receipt3 = NULL, *json3 = NULL, *qreceipt3 = NULL;
    char *repeat_store = NULL, *repeat_receipt = NULL, *similar_json = NULL, *similar_receipt = NULL;
    uint8_t *bytes = NULL; size_t n = 0u; ChrStore loaded; int fail = 0;
    uint8_t before[32], after[32];
    memset(&loaded, 0, sizeof(loaded));
    if (write_bytes(path, (const uint8_t *)story, sizeof(story) - 1u) != 0 ||
        casper_chronicle_ingest(path, &store, &receipt) != 0) return 1;
    if (casper_chronicle_verify(receipt) != 0) ++fail;
    if (casper_chronicle_query(store, question, &json, &qreceipt) != 0 ||
        !expect_status(json, "PARTIAL") ||
        strstr(json, "\"settled\":{\"num\":40,\"den\":1}") == NULL ||
        strstr(json, "أحمدان") != NULL || casper_chronicle_verify(qreceipt) != 0) ++fail;
    if (!niyah_sha256_file(store, before) ||
        casper_chronicle_ingest(path, &repeat_store, &repeat_receipt) != 0 ||
        !niyah_sha256_file(store, after) || memcmp(before, after, 32u) != 0) ++fail;
    if (casper_chronicle_query(store, "هل سدد أحمدان دين خالد؟", &similar_json, &similar_receipt) != 0 ||
        !expect_status(similar_json, "UNKNOWN")) ++fail;
    if (store_load(store, &loaded) != 0) ++fail;
    else {
        uint32_t i;
        for (i = 0u; i < loaded.event_count; ++i) {
            uint8_t h[32];
            niyah_sha256(loaded.source + loaded.events[i].byte_start,
                         (size_t)(loaded.events[i].byte_end - loaded.events[i].byte_start), h);
            if (memcmp(h, loaded.events[i].span_hash, 32u) != 0) ++fail;
        }
        store_free(&loaded);
    }
    if (read_file(store, &bytes, &n) != 0 || n < 64u) ++fail;
    else {
        bytes[48] ^= 1u;
        if (write_bytes(store, bytes, n) != 0 || casper_chronicle_verify(receipt) == 0) ++fail;
        bytes[48] ^= 1u; if (write_bytes(store, bytes, n) != 0) ++fail;
    }
    if (write_bytes(conflict_path, (const uint8_t *)conflict_story, sizeof(conflict_story) - 1u) != 0 ||
        casper_chronicle_ingest(conflict_path, &store2, &receipt2) != 0 ||
        casper_chronicle_query(store2, question, &json2, &qreceipt2) != 0 ||
        !expect_status(json2, "CONFLICT")) ++fail;
    if (write_bytes(missing_path, (const uint8_t *)missing_link_story, sizeof(missing_link_story) - 1u) != 0 ||
        casper_chronicle_ingest(missing_path, &store3, &receipt3) != 0 ||
        casper_chronicle_query(store3, question, &json3, &qreceipt3) != 0 ||
        !expect_status(json3, "UNKNOWN")) ++fail;
    free(bytes); free(json); free(qreceipt); free(receipt); free(store);
    free(json2); free(qreceipt2); free(receipt2); free(store2);
    free(json3); free(qreceipt3); free(receipt3); free(store3);
    free(repeat_store); free(repeat_receipt); free(similar_json); free(similar_receipt);
    remove_query_receipt("chronicle-self-test.txt.chronicle", question);
    remove_query_receipt("chronicle-self-test.txt.chronicle", "هل سدد أحمدان دين خالد؟");
    remove_query_receipt("chronicle-conflict-test.txt.chronicle", question);
    remove_query_receipt("chronicle-missing-test.txt.chronicle", question);
    remove(path); remove("chronicle-self-test.txt.chronicle"); remove("chronicle-self-test.txt.chronicle.receipt");
    remove(conflict_path); remove("chronicle-conflict-test.txt.chronicle"); remove("chronicle-conflict-test.txt.chronicle.receipt");
    remove(missing_path); remove("chronicle-missing-test.txt.chronicle"); remove("chronicle-missing-test.txt.chronicle.receipt");
    fprintf(stderr, "Chronicle self-check: %s (%d failures)\n", fail == 0 ? "PASS" : "FAIL", fail);
    return fail == 0 ? 0 : 1;
}
int casper_chronicle_benchmark(void) {
    const size_t words = 1000000u, token_n = 5u, bytes_n = words * token_n;
    uint8_t *data = (uint8_t *)malloc(bytes_n); char *store = NULL, *receipt = NULL;
    struct timespec begin, end; size_t i; double seconds; int rc;
#if !defined(_WIN32)
    struct rusage usage;
#endif
    if (!data) return 1;
    for (i = 0u; i < words; ++i) memcpy(data + i * token_n, "word ", token_n);
    if (write_bytes("chronicle-benchmark.txt", data, bytes_n) != 0) { free(data); return 1; }
    timespec_get(&begin, TIME_UTC);
    rc = casper_chronicle_ingest("chronicle-benchmark.txt", &store, &receipt);
    timespec_get(&end, TIME_UTC);
    seconds = (double)(end.tv_sec - begin.tv_sec) + (double)(end.tv_nsec - begin.tv_nsec) / 1000000000.0;
    printf("benchmark_words=%zu\ninput_bytes=%zu\nelapsed_seconds=%.6f\ntracked_input_bytes=%zu\n",
           words, bytes_n, seconds, bytes_n + 1u);
#if !defined(_WIN32)
    if (getrusage(RUSAGE_SELF, &usage) == 0) printf("peak_rss_kib=%ld\n", usage.ru_maxrss);
#else
    printf("peak_rss_kib=unavailable\n");
#endif
    free(data); free(store); free(receipt);
    remove("chronicle-benchmark.txt"); remove("chronicle-benchmark.txt.chronicle");
    remove("chronicle-benchmark.txt.chronicle.receipt");
    return rc;
}
