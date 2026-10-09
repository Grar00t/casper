#include "casper_chronicle.h"
#include "chronicle_pool.h"
#include "proof_generator.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHR_FIELD_MAX 128u
#define CHR_MAX_EVENTS 4096u
#define CHR_MAX_SOURCE (8u * 1024u * 1024u)
#define CHR_RECORD_BYTES (9u * CHR_FIELD_MAX + 16u)
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

void casper_chronicle_free(void *ptr) { chr_free(ptr); }
static void store_free(ChrStore *s) {
    if (!s) return;
    chr_free(s->source); chr_free(s->events); memset(s, 0, sizeof(*s));
}
static int buf_reserve(ChrBuf *b, size_t extra) {
    size_t need, capacity; char *next;
    if (extra > SIZE_MAX - b->len - 1u) return -1;
    need = b->len + extra + 1u;
    if (need <= b->cap) return 0;
    capacity = b->cap ? b->cap : 256u;
    while (capacity < need) {
        if (capacity > SIZE_MAX / 2u) { capacity = need; break; }
        capacity *= 2u;
    }
    next = (char *)chr_realloc(b->data, capacity);
    if (!next) return -1;
    b->data = next; b->cap = capacity; return 0;
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
    FILE *f; long end; uint8_t *p; int ok;
    if (!path || !data || !size) return -1;
    f = fopen(path, "rb"); if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0 || (end = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f); return -1;
    }
    if ((uint64_t)end > CHRONICLE_POOL_SIZE - 1u) { fclose(f); return -1; }
    p = (uint8_t *)chr_malloc((size_t)end + 1u); if (!p) { fclose(f); return -1; }
    ok = (size_t)end == fread(p, 1u, (size_t)end, f);
    if (fclose(f) != 0) ok = 0;
    if (!ok) { chr_free(p); return -1; }
    p[(size_t)end] = 0u; *data = p; *size = (size_t)end; return 0;
}
static int64_t gcd64(int64_t a, int64_t b) {
    while (b != 0) { int64_t t = a % b; a = b; b = t; }
    return a < 0 ? -a : a;
}
static int parse_rat(const char *s, ChrRat *out) {
    int64_t values[2] = {0, 0}, g; size_t part = 0u, digits = 0u, i, len = strlen(s);
    if (len == 0u || len >= CHR_FIELD_MAX) return -1;
    for (i = 0u; i < len; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == '/' && part == 0u && digits != 0u) { part = 1u; digits = 0u; continue; }
        if (c < '0' || c > '9' || values[part] > (INT64_MAX - (c - '0')) / 10) return -1;
        values[part] = values[part] * 10 + (c - '0'); ++digits;
    }
    if (digits == 0u) return -1;
    if (part == 0u) values[1] = 1;
    if (values[1] == 0) return -1;
    g = gcd64(values[0], values[1]); out->num = values[0] / g; out->den = values[1] / g; return 0;
}
static int rat_cmp(ChrRat a, ChrRat b) {
    int direction = 1;
    /* Continued fractions compare nonnegative rationals without multiplication. */
    for (;;) {
        int64_t aq = a.num / a.den, bq = b.num / b.den;
        int64_t ar = a.num % a.den, br = b.num % b.den;
        if (aq != bq) return aq < bq ? -direction : direction;
        if (ar == 0 || br == 0) return ar == br ? 0 : ar == 0 ? -direction : direction;
        a.num = a.den; a.den = ar; b.num = b.den; b.den = br; direction = -direction;
    }
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
static int hash_event_ids(const uint8_t doc[32], uint64_t start, uint64_t end,
                           const uint8_t *line, size_t line_len, uint8_t event_id[32], uint8_t span_id[32]) {
    uint8_t m[56u + CHR_RECORD_BYTES]; size_t i;
    if (line_len > CHR_RECORD_BYTES) return -1;
    memcpy(m, "event-v1", 8u); memcpy(m + 8u, doc, 32u);
    for (i = 0u; i < 8u; ++i) m[40u + i] = (uint8_t)(start >> (8u * i));
    for (i = 0u; i < 8u; ++i) m[48u + i] = (uint8_t)(end >> (8u * i));
    memcpy(m + 56u, line, line_len); niyah_sha256(m, 56u + line_len, event_id);
    memcpy(m, "span-v1 ", 8u); niyah_sha256(m, 56u + line_len, span_id); return 0;
}
static int field_copy(char out[CHR_FIELD_MAX], const char *s, size_t n) {
    if (n == 0u || n >= CHR_FIELD_MAX) return -1;
    memcpy(out, s, n); out[n] = '\0'; return 0;
}
#include "chronicle_literal_parse.inc"
static int parse_event_line(const uint8_t *line, size_t len, uint64_t start,
                            uint64_t line_no, const uint8_t doc[32], ChrEvent *e) {
    const char prefix[] = "@chronicle\t"; const char *fields[9]; size_t sizes[9], i;
    size_t pos = sizeof(prefix) - 1u;
    if (len < pos || memcmp(line, prefix, pos) != 0) {
        int literal_rc;
        if (len >= 10u && memcmp(line, "@chronicle", 10u) == 0) return -1;
        literal_rc = parse_literal_line(line, len, e);
        if (literal_rc != 0) return literal_rc;
        e->byte_start = start; e->byte_end = start + len;
        e->line_start = line_no; e->line_end = line_no;
        niyah_sha256(line, len, e->span_hash);
        return hash_event_ids(doc, start, start + len, line, len, e->event_id, e->span_id);
    }
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
    return hash_event_ids(doc, e->byte_start, e->byte_end, line, len, e->event_id, e->span_id);
}
static int extract_events(ChrStore *s) {
    size_t pos; uint64_t line_no; uint32_t count; unsigned pass;
    for (pass = 0u; pass < 2u; ++pass) {
    pos = 0u; line_no = 1u; count = 0u;
    while (pos < (size_t)s->source_size) {
        size_t start = pos, end; ChrEvent parsed; int rc;
        while (pos < (size_t)s->source_size && s->source[pos] != '\n') ++pos;
        end = pos; if (end > start && s->source[end - 1u] == '\r') --end;
        rc = parse_event_line(s->source + start, end - start, (uint64_t)start, line_no, s->document_hash, &parsed);
        if (rc < 0) return -1;
        if (rc == 0) {
            if (count >= CHR_MAX_EVENTS) return -1;
            if (pass != 0u) s->events[count] = parsed;
            ++count;
        }
        if (pos < (size_t)s->source_size) ++pos;
        ++line_no;
    }
    if (pass == 0u && count) {
        s->events = (ChrEvent *)chr_calloc(count, sizeof(*s->events));
        if (!s->events) return -1;
    }
    }
    s->event_count = count; return 0;
}
static int store_save(const char *path, const ChrStore *s) {
    FILE *f = fopen(path, "wbx"); uint32_t i; int ok = f != NULL;
    if (!f) return -1;
    if (fwrite(CHR_MAGIC, 1u, CHR_MAGIC_LEN, f) != CHR_MAGIC_LEN) ok = 0;
    if (ok && fwrite(s->document_hash, 1u, 32u, f) != 32u) ok = 0;
    put_u64(f, s->source_size, &ok); put_u64(f, s->imported_at, &ok); put_u32(f, s->event_count, &ok);
    if (ok && s->source_size && fwrite(s->source, 1u, (size_t)s->source_size, f) != (size_t)s->source_size) ok = 0;
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
static int event_equal(const ChrEvent *a, const ChrEvent *b) {
    return memcmp(a->event_id, b->event_id, sizeof(a->event_id)) == 0 &&
        memcmp(a->span_id, b->span_id, sizeof(a->span_id)) == 0 &&
        memcmp(a->span_hash, b->span_hash, sizeof(a->span_hash)) == 0 &&
        a->byte_start == b->byte_start && a->byte_end == b->byte_end &&
        a->line_start == b->line_start && a->line_end == b->line_end &&
        a->amount_present == b->amount_present && a->amount.num == b->amount.num && a->amount.den == b->amount.den &&
        memcmp(a->subject, b->subject, sizeof(a->subject)) == 0 &&
        memcmp(a->predicate, b->predicate, sizeof(a->predicate)) == 0 &&
        memcmp(a->object, b->object, sizeof(a->object)) == 0 &&
        memcmp(a->currency, b->currency, sizeof(a->currency)) == 0 &&
        memcmp(a->time_expr, b->time_expr, sizeof(a->time_expr)) == 0 &&
        memcmp(a->modality, b->modality, sizeof(a->modality)) == 0 &&
        memcmp(a->polarity, b->polarity, sizeof(a->polarity)) == 0 &&
        memcmp(a->status, b->status, sizeof(a->status)) == 0;
}
static int store_matches_source(const ChrStore *s) {
    size_t pos = 0u; uint64_t line_no = 1u; uint32_t count = 0u;
    while (pos < (size_t)s->source_size) {
        size_t start = pos, end; ChrEvent canonical; int parsed;
        while (pos < (size_t)s->source_size && s->source[pos] != '\n') ++pos;
        end = pos;
        if (end > start && s->source[end - 1u] == '\r') --end;
        parsed = parse_event_line(s->source + start, end - start, (uint64_t)start,
                                  line_no, s->document_hash, &canonical);
        if (parsed < 0) return -1;
        if (parsed == 0) {
            if (count >= s->event_count || !event_equal(&canonical, &s->events[count])) return -1;
            ++count;
        }
        if (pos < (size_t)s->source_size) ++pos;
        ++line_no;
    }
    return count == s->event_count ? 0 : -1;
}
static int store_load(const char *path, ChrStore *s) {
    FILE *f = fopen(path, "rb"); char magic[CHR_MAGIC_LEN]; uint32_t i; uint8_t actual[32]; int tail;
    memset(s, 0, sizeof(*s));
    if (!f || fread(magic, 1u, sizeof(magic), f) != sizeof(magic) || memcmp(magic, CHR_MAGIC, CHR_MAGIC_LEN) != 0 ||
        fread(s->document_hash, 1u, 32u, f) != 32u || get_u64(f, &s->source_size) != 0 ||
        get_u64(f, &s->imported_at) != 0 || get_u32(f, &s->event_count) != 0 || s->event_count > CHR_MAX_EVENTS ||
        s->source_size > CHR_MAX_SOURCE || s->source_size > SIZE_MAX - 1u) { if (f) fclose(f); return -1; }
    /* Fail closed on forged source sizes or event counts before allocation.
     * Every serialized v1 event occupies at least 177 bytes. */
    {
        long body_start = ftell(f), file_end;
        const uint64_t min_event_bytes = 177u;
        uint64_t remaining;
        if (body_start < 0 || fseek(f, 0, SEEK_END) != 0 ||
            (file_end = ftell(f)) < 0 || fseek(f, body_start, SEEK_SET) != 0 ||
            file_end < body_start) { fclose(f); return -1; }
        remaining = (uint64_t)(file_end - body_start);
        if (s->source_size > remaining ||
            (uint64_t)s->event_count > (remaining - s->source_size) / min_event_bytes) {
            fclose(f); return -1;
        }
    }
    s->source = (uint8_t *)chr_malloc((size_t)s->source_size + 1u);
    s->events = s->event_count ? (ChrEvent *)chr_calloc(s->event_count, sizeof(*s->events)) : NULL;
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
        present = (uint8_t)tail;
        if (present > 1u || den == 0u || num > INT64_MAX || den > INT64_MAX ||
            e->byte_start > e->byte_end || e->byte_end > s->source_size) {
            fclose(f); store_free(s); return -1;
        }
        e->amount_present = present ? 1 : 0;
        e->amount.num = (int64_t)num; e->amount.den = (int64_t)den;
    }
    tail = fgetc(f);
    { int bad = tail != EOF || ferror(f);
      if (fclose(f) != 0) bad = 1;
      if (bad || !utf8_valid(s->source, (size_t)s->source_size)) {
        store_free(s); return -1;
      }
    }
    niyah_sha256(s->source, (size_t)s->source_size, actual);
    if (memcmp(actual, s->document_hash, 32u) != 0) { store_free(s); return -1; }
    if (store_matches_source(s) != 0) { store_free(s); return -1; }
    return 0;
}
static char *path_suffix(const char *path, const char *suffix) {
    size_t a = strlen(path), b = strlen(suffix); char *out = (char *)chr_malloc(a + b + 1u);
    if (!out) return NULL;
    memcpy(out, path, a); memcpy(out + a, suffix, b + 1u); return out;
}
static void encode_hex(char *out, const uint8_t *p, size_t n) {
    static const char x[] = "0123456789abcdef"; size_t i;
    for (i = 0u; i < n; ++i) {
        out[i * 2u] = x[p[i] >> 4]; out[i * 2u + 1u] = x[p[i] & 15u];
    }
    out[n * 2u] = '\0';
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
    p = (uint8_t *)chr_malloc(len / 2u + 1u); if (!p) return NULL;
    for (i = 0u; i < len; i += 2u) {
        int a = hex_nibble(s[i]), b = hex_nibble(s[i + 1u]);
        if (a < 0 || b < 0) { chr_free(p); return NULL; }
        p[i / 2u] = (uint8_t)((a << 4) | b);
    }
    p[len / 2u] = 0u; *n = len / 2u; return p;
}
#define CHR_RECEIPT_VALUE_MAX 4096u
#define CHR_RECEIPT_LINE_MAX (CHR_RECEIPT_VALUE_MAX + 32u)
#define CHR_RECEIPT_BYTES_MAX (2u * CHR_RECEIPT_VALUE_MAX + 512u)
static int receipt_existing_matches(const char *path, const char *data, size_t size) {
    FILE *f = fopen(path, "rb"); size_t pos = 0u; int ok = 1;
    if (!f) return -1;
    while (ok && pos < size) {
        int c = fgetc(f);
        if (c == EOF || (unsigned char)c != (unsigned char)data[pos]) ok = 0;
        ++pos;
    }
    if (fgetc(f) != EOF || ferror(f)) ok = 0;
    if (fclose(f) != 0) ok = 0;
    return ok ? 0 : -1;
}
static int receipt_write_once(const char *path, const char *data, size_t size) {
    FILE *f = fopen(path, "wbx"); int ok;
    if (!f) return receipt_existing_matches(path, data, size);
    ok = fwrite(data, 1u, size, f) == size;
    if (fclose(f) != 0) ok = 0;
    if (!ok) { if (remove(path) != 0) return -1; }
    return ok ? 0 : -1;
}
static int write_receipt(const char *path, const char *kind, const char *store_path,
                         const char *question, const uint8_t result_hash[32],
                         const uint8_t document_hash[32]) {
    uint8_t store_hash[32]; char store_hex[65], doc_hex[65], result_hex[65]; int written;
    char path_hex[CHR_RECEIPT_VALUE_MAX], question_hex[CHR_RECEIPT_VALUE_MAX], data[CHR_RECEIPT_BYTES_MAX];
    size_t path_len = strlen(store_path), question_len = question ? strlen(question) : 0u;
    if (path_len == 0u || path_len > (CHR_RECEIPT_VALUE_MAX - 1u) / 2u ||
        question_len > (CHR_RECEIPT_VALUE_MAX - 1u) / 2u ||
        (strcmp(kind, "INGEST") != 0 && strcmp(kind, "QUERY") != 0 && strcmp(kind, "FIND") != 0) ||
        (strcmp(kind, "INGEST") == 0 && question_len != 0u)) return -1;
    if (!niyah_sha256_file(store_path, store_hash)) return -1;
    encode_hex(path_hex, (const uint8_t *)store_path, path_len);
    encode_hex(question_hex, (const uint8_t *)(question ? question : ""), question_len);
    niyah_hash_to_hex(store_hash, store_hex); niyah_hash_to_hex(document_hash, doc_hex); niyah_hash_to_hex(result_hash, result_hex);
    written = snprintf(data, sizeof(data), "CASPER-CHRONICLE-INTEGRITY-RECEIPT-V1\nkind:%s\nstore_path_hex:%s\n"
                       "question_hex:%s\nstore_sha256:%s\ndocument_sha256:%s\nresult_sha256:%s\n",
                       kind, path_hex, question_hex, store_hex, doc_hex, result_hex);
    if (written < 0 || (size_t)written >= sizeof(data)) return -1;
    return receipt_write_once(path, data, (size_t)written);
}
static int same_tuple(const ChrEvent *a, const ChrEvent *b) {
    return strcmp(a->subject, b->subject) == 0 && strcmp(a->predicate, b->predicate) == 0 &&
           strcmp(a->object, b->object) == 0 && a->amount_present == b->amount_present &&
           (!a->amount_present || rat_cmp(a->amount, b->amount) == 0) &&
           strcmp(a->currency, b->currency) == 0 && strcmp(a->time_expr, b->time_expr) == 0;
}
static int is_established(const ChrEvent *e) {
    /* Only an explicitly CONFIRMED input record can establish a debt/link/payment.
     * This is not independent verification of real-world truth. */
    return strcmp(e->modality, "ASSERTED") == 0 &&
           strcmp(e->status, "CONFIRMED") == 0;
}
static int opposite_conflict(const ChrStore *s, const ChrEvent *focus, uint8_t *used) {
    uint32_t i; int conflict = 0;
    for (i = 0u; i < s->event_count; ++i) {
        const ChrEvent *e = &s->events[i];
        if (e != focus && same_tuple(e, focus) && is_established(e) &&
            strcmp(e->polarity, focus->polarity) != 0) { used[i] = 1u; conflict = 1; }
    }
    return conflict;
}
static int is_positive(const ChrEvent *e) {
    return strcmp(e->polarity, "POSITIVE") == 0 && strcmp(e->status, "DENIED") != 0;
}
static int entity_boundary(const unsigned char *p) {
    if (*p == '\0') return 1;
    if (*p < 0x80u) return !((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'Z') ||
                             (*p >= 'a' && *p <= 'z') || *p == '_');
    return (p[0] == 0xd8u && (p[1] == 0x8cu || p[1] == 0x9bu || p[1] == 0x9fu));
}
static int left_entity_boundary(const char *text, const char *p) {
    const unsigned char *previous;
    if (p == text) return 1;
    previous = (const unsigned char *)p - 1u;
    while (previous > (const unsigned char *)text && (*previous & 0xc0u) == 0x80u) --previous;
    return entity_boundary(previous);
}
static int contains_entity(const char *text, const char *entity) {
    const char *p = text; size_t n = strlen(entity);
    if (n == 0u) return 0;
    while ((p = strstr(p, entity)) != NULL) {
        int left = left_entity_boundary(text, p);
        int right = entity_boundary((const unsigned char *)p + n);
        if (left && right) return 1;
        ++p;
    }
    return 0;
}
typedef struct {
    const ChrEvent *debt;
    ChrRat settled;
    uint8_t *used;
    int conflict, ambiguous, payments, unresolved_duplicate, unresolved_related;
} ChrReason;
static void select_debt(const ChrStore *s, const char *question, ChrReason *r) {
    uint32_t i;
    for (i = 0u; i < s->event_count; ++i) {
        const ChrEvent *e = &s->events[i];
        if (strcmp(e->predicate, "BORROWED_FROM") != 0 || !is_positive(e) ||
            !e->amount_present || !is_established(e) ||
            !contains_entity(question, e->subject) ||
            !contains_entity(question, e->object)) continue;
        r->used[i] = 1u;
        /* Repeated observations of the same exact debt tuple select one
         * claim, while keeping each source event in the evidence ledger.
         * A different matching tuple is a competing obligation (UNKNOWN). */
        if (!r->debt) r->debt = e;
        else if (!same_tuple(r->debt, e)) r->ambiguous = 1;
    }
    if (r->ambiguous) r->debt = NULL;
    if (r->debt) r->conflict = opposite_conflict(s, r->debt, r->used);
}
static int is_payment(const ChrEvent *e) {
    return strcmp(e->predicate, "PAID_TO") == 0 || strcmp(e->predicate, "TRANSFER_TO") == 0;
}
static int payment_eligible(const ChrEvent *e) {
    return is_payment(e) && is_positive(e) && is_established(e) && e->amount_present &&
           (strcmp(e->predicate, "PAID_TO") == 0 || strcmp(e->status, "CONFIRMED") == 0);
}
/* Return 1 for an established link, 0 for no link, -1 for a
 * confirmed contradiction. An attempted payment via a contradicted
 * link is evidence of the dispute, NOT eligible settlement. */
static int linked_intermediary(const ChrStore *s, const ChrEvent *payment, ChrReason *r) {
    uint32_t i; int linked = 0, contradicted = 0;
    for (i = 0u; i < s->event_count; ++i) {
        const ChrEvent *e = &s->events[i];
        if (strcmp(e->predicate, "INTERMEDIARY_FOR") != 0 ||
            strcmp(e->subject, payment->object) != 0 || strcmp(e->object, r->debt->object) != 0) continue;
        r->used[i] = 1u;
        if (!is_positive(e) || !is_established(e)) continue;
        if (opposite_conflict(s, e, r->used)) {
            r->conflict = 1;
            contradicted = 1;
        } else linked = 1;
    }
    return contradicted ? -1 : linked;
}
static int duplicate_payment(const ChrStore *s, uint32_t index) {
    uint32_t i; const ChrEvent *focus = &s->events[index];
    for (i = 0u; i < index; ++i) {
        const ChrEvent *e = &s->events[i];
        if (payment_eligible(e) && strcmp(e->subject, focus->subject) == 0 &&
            strcmp(e->object, focus->object) == 0 && rat_cmp(e->amount, focus->amount) == 0 &&
            strcmp(e->currency, focus->currency) == 0 && strcmp(e->time_expr, focus->time_expr) == 0)
            return 1;
    }
    return 0;
}
static void settle_payments(const ChrStore *s, ChrReason *r) {
    uint32_t i;
    for (i = 0u; i < s->event_count; ++i) {
        const ChrEvent *e = &s->events[i]; int linked;
        if (!is_payment(e) || !is_positive(e) || strcmp(e->subject, r->debt->subject) != 0) continue;
        linked = strcmp(e->object, r->debt->object) == 0 ? 1 : linked_intermediary(s, e, r);
        if (linked == 0) continue;
        r->used[i] = 1u;
        /* The disputed transfer must be auditable, never counted as paid. */
        if (linked < 0) continue;
        if (!payment_eligible(e) || strcmp(e->currency, r->debt->currency) != 0) {
            r->unresolved_related = 1;
            continue;
        }
        if (opposite_conflict(s, e, r->used)) { r->conflict = 1; continue; }
        if (duplicate_payment(s, i)) { r->unresolved_duplicate = 1; continue; }
        if (rat_add(r->settled, e->amount, &r->settled) != 0) { r->conflict = 1; break; }
        ++r->payments;
    }
}
static const char *reason_status(const ChrReason *r) {
    if (r->conflict) return "CONFLICT";
    if (!r->debt || r->ambiguous || !is_established(r->debt) || !r->payments) return "UNKNOWN";
    if (rat_cmp(r->settled, r->debt->amount) > 0) return "CONFLICT";
    if (rat_cmp(r->settled, r->debt->amount) == 0)
        return (r->unresolved_duplicate || r->unresolved_related) ? "UNKNOWN" : "SUPPORTED";
    return r->settled.num > 0 ? "PARTIAL" : "UNKNOWN";
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
#include "chronicle_literal_query.inc"
static int reason_json(const ChrStore *s, const char *question, char **out) {
    const ChrEvent *debt; uint32_t i; int evidence_count = 0;
    ChrReason r = {0};
    uint8_t *used;
    const char *status; ChrBuf b = {0};
    if (strncmp(question, "@claim\t", 7u) == 0) return literal_claim_json(s, question, out);
    used = (uint8_t *)chr_calloc(s->event_count ? s->event_count : 1u, 1u);
    if (!used) return -1;
    r.used = used; r.settled.den = 1; select_debt(s, question, &r); debt = r.debt;
    if (debt && !r.conflict && is_established(debt) && strcmp(debt->currency, "-") != 0)
        settle_payments(s, &r);
    status = reason_status(&r);
    if (buf_add(&b, "{\"status\":\"") != 0 || buf_add(&b, status) != 0 ||
        buf_add(&b, "\",\"assessment\":\"DEBT_SETTLEMENT\",\"question\":") != 0 ||
        buf_add_json(&b, (const uint8_t *)question, strlen(question)) != 0 ||
        buf_add(&b, ",\"debt\":") != 0) goto fail;
    if (debt) {
        if (buf_add(&b, "{\"num\":") != 0 || buf_add_i64(&b, debt->amount.num) != 0 ||
            buf_add(&b, ",\"den\":") != 0 || buf_add_i64(&b, debt->amount.den) != 0 ||
            buf_add(&b, ",\"currency\":") != 0 ||
            buf_add_json(&b, (const uint8_t *)debt->currency, strlen(debt->currency)) != 0 ||
            buf_add(&b, ",\"borrower\":") != 0 ||
            buf_add_json(&b, (const uint8_t *)debt->subject, strlen(debt->subject)) != 0 ||
            buf_add(&b, ",\"lender\":") != 0 ||
            buf_add_json(&b, (const uint8_t *)debt->object, strlen(debt->object)) != 0 ||
            buf_add(&b, "}") != 0) goto fail;
    } else if (buf_add(&b, "null") != 0) goto fail;
    if (buf_add(&b, ",\"settled\":{\"num\":") != 0 || buf_add_i64(&b, r.settled.num) != 0 ||
        buf_add(&b, ",\"den\":") != 0 || buf_add_i64(&b, r.settled.den) != 0 ||
        buf_add(&b, "},\"limits\":\"explicit @chronicle records only; question selects exact entities; "
                    "status evaluates debt settlement, not question truth; integrity is not truth\",\"evidence\":[") != 0)
        goto fail;
    for (i = 0u; i < s->event_count; ++i) if (used[i]) {
        if (add_evidence_json(&b, s, &s->events[i], evidence_count != 0) != 0) goto fail;
        ++evidence_count;
    }
    if (buf_add(&b, "]}") != 0) goto fail;
    chr_free(used); *out = b.data; return 0;
fail:
    chr_free(used); chr_free(b.data); return -1;
}
/* Bounded exact-byte lexical retrieval over unmodified UTF-8 source lines.
 * This is NOT an inference or semantic comprehension layer. */
typedef struct {
    size_t start, end;
    uint64_t line_no;
    unsigned hits;
} ChrFound;
static int token_is_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '.' || c == ',' || c == ';' || c == ':' || c == '?' || c == '!';
}
static int line_has_bytes(const uint8_t *line, size_t n, const char *needle, size_t len) {
    size_t i;
    if (!len || len > n) return 0;
    for (i = 0u; i <= n - len; ++i)
        if (line[i] == (uint8_t)needle[0] && memcmp(line + i, needle, len) == 0) return 1;
    return 0;
}
static int find_json(const ChrStore *s, const char *query, char **out) {
    struct { const char *p; size_t len; } terms[16];
    ChrFound best[8]; size_t pos = 0u, nt = 0u, returned = 0u, total = 0u;
    size_t qi = 0u, qlen = strlen(query), i, j; uint64_t line_no = 1u;
    ChrBuf b = {0};
    char hex[65]; 
    if (!qlen || qlen > 1024u || !utf8_valid((const uint8_t *)query, qlen)) return -1;
    while (qi < qlen) {
        size_t begin;
        while (qi < qlen && token_is_space((unsigned char)query[qi])) ++qi;
        if (qi >= qlen) break;
        begin = qi;
        while (qi < qlen && !token_is_space((unsigned char)query[qi])) ++qi;
        if (nt >= sizeof(terms) / sizeof(terms[0])) return -1;
        terms[nt].p = query + begin; terms[nt].len = qi - begin; ++nt;
    }
    if (!nt) return -1;
    while (pos < (size_t)s->source_size) {
        size_t begin = pos, end; unsigned score = 0u;
        while (pos < (size_t)s->source_size && s->source[pos] != '\n') ++pos;
        end = pos;
        if (end > begin && s->source[end - 1u] == '\r') --end;
        if (pos < (size_t)s->source_size) ++pos;
        if (end > begin) {
            for (i = 0u; i < nt; ++i)
                score += (unsigned)line_has_bytes(s->source + begin, end - begin,
                                                  terms[i].p, terms[i].len);
        }
        if (score) {
            ChrFound candidate = {begin, end, line_no, score};
            ++total;
            if (returned < sizeof(best) / sizeof(best[0])) best[returned++] = candidate;
            else {
                size_t worst = 0u;
                for (i = 1u; i < returned; ++i)
                    if (best[i].hits < best[worst].hits ||
                        (best[i].hits == best[worst].hits && best[i].start > best[worst].start))
                        worst = i;
                if (score > best[worst].hits) best[worst] = candidate;
            }
        }
        ++line_no;
    }
    for (i = 1u; i < returned; ++i) {
        ChrFound cur = best[i]; j = i;
        while (j > 0u && (best[j - 1u].hits < cur.hits ||
               (best[j - 1u].hits == cur.hits && best[j - 1u].start > cur.start))) {
            best[j] = best[j - 1u]; --j;
        }
        best[j] = cur;
    }
    niyah_hash_to_hex(s->document_hash, hex);
    if (buf_add(&b, "{\"status\":\"") != 0 ||
        buf_add(&b, total ? "MATCHES" : "NO_MATCH") != 0 ||
        buf_add(&b, "\",\"query\":") != 0 ||
        buf_add_json(&b, (const uint8_t *)query, qlen) != 0 ||
        buf_add(&b, ",\"document_sha256\":\"") != 0 ||
        buf_add(&b, hex) != 0 ||
        buf_add(&b, "\",\"total_matching_lines\":") != 0 ||
        buf_add_u64(&b, (uint64_t)total) != 0 ||
        buf_add(&b, ",\"lexical_only\":true,\"matches\":[") != 0) goto error;
    for (i = 0u; i < returned; ++i) {
        uint8_t hash[32];
        if (i && buf_add(&b, ",") != 0) goto error;
        if (buf_add(&b, "{\"line\":") != 0 || buf_add_u64(&b, best[i].line_no) != 0 ||
            buf_add(&b, ",\"byte_start\":") != 0 ||
            buf_add_u64(&b, (uint64_t)best[i].start) != 0 ||
            buf_add(&b, ",\"byte_end\":") != 0 ||
            buf_add_u64(&b, (uint64_t)best[i].end) != 0 ||
            buf_add(&b, ",\"matched_terms\":") != 0 ||
            buf_add_u64(&b, best[i].hits) != 0) goto error;
        niyah_sha256(s->source + best[i].start, best[i].end - best[i].start, hash);
        niyah_hash_to_hex(hash, hex);
        if (buf_add(&b, ",\"exact_span_sha256\":\"") != 0 || buf_add(&b, hex) != 0 ||
            buf_add(&b, "\",\"text\":") != 0 ||
            buf_add_json(&b, s->source + best[i].start, best[i].end - best[i].start) != 0 ||
            buf_add(&b, "}") != 0) goto error;
    }
    if (buf_add(&b, "]}") != 0) goto error;
    *out = b.data; return 0;
error:
    chr_free(b.data); return -1;
}

int casper_chronicle_ingest(const char *input_path, char **store_path_out, char **receipt_path_out) {
    ChrStore s, old; char *store_path = NULL, *receipt_path = NULL; size_t size;
    uint8_t result_hash[32]; int same = 0;
    if (!input_path || !store_path_out || !receipt_path_out) return 2;
    memset(&s, 0, sizeof(s)); memset(&old, 0, sizeof(old));
    store_path = path_suffix(input_path, ".chronicle");
    if (store_path) receipt_path = path_suffix(store_path, ".receipt");
    if (!store_path || !receipt_path) goto fail;
    if (store_load(store_path, &old) == 0) {
        if (!niyah_sha256_file(input_path, s.document_hash)) goto fail;
        same = memcmp(old.document_hash, s.document_hash, 32u) == 0;
        store_free(&old);
        if (!same) goto fail;
    }
    if (!same) {
        if (read_file(input_path, &s.source, &size) != 0 || size > CHR_MAX_SOURCE ||
            !utf8_valid(s.source, size)) goto fail;
        s.source_size = (uint64_t)size; s.imported_at = (uint64_t)time(NULL);
        niyah_sha256(s.source, size, s.document_hash);
        if (extract_events(&s) != 0 || store_save(store_path, &s) != 0) goto fail;
    }
    if (!niyah_sha256_file(store_path, result_hash) ||
        write_receipt(receipt_path, "INGEST", store_path, "", result_hash, s.document_hash) != 0) goto fail;
    store_free(&s); *store_path_out = store_path; *receipt_path_out = receipt_path; return 0;
fail:
    store_free(&s); store_free(&old); chr_free(store_path); chr_free(receipt_path); return 1;
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
    suffix = (char *)chr_malloc(34u);
    if (suffix) snprintf(suffix, 34u, ".query-%.16s.receipt", qhex);
    if (suffix) receipt = path_suffix(store_path, suffix);
    niyah_sha256((const uint8_t *)json, strlen(json), rh);
    if (!suffix || !receipt || write_receipt(receipt, "QUERY", store_path, question, rh, s.document_hash) != 0) {
        chr_free(json); chr_free(suffix); chr_free(receipt); store_free(&s); return 1;
    }
    chr_free(suffix); store_free(&s); *json_out = json; *receipt_path_out = receipt; return 0;
}
int casper_chronicle_find(const char *store_path, const char *query,
                          char **json_out, char **receipt_path_out) {
    ChrStore s; char *json = NULL, *suffix = NULL, *receipt = NULL;
    uint8_t qh[32], rh[32]; char qhex[65];
    memset(&s, 0, sizeof(s));
    if (!store_path || !query || !json_out || !receipt_path_out ||
        !utf8_valid((const uint8_t *)query, strlen(query))) return 2;
    if (store_load(store_path, &s) != 0 || find_json(&s, query, &json) != 0) {
        store_free(&s); return 1;
    }
    niyah_sha256((const uint8_t *)query, strlen(query), qh); niyah_hash_to_hex(qh, qhex);
    suffix = (char *)chr_malloc(33u);
    if (suffix) snprintf(suffix, 33u, ".find-%.16s.receipt", qhex);
    if (suffix) receipt = path_suffix(store_path, suffix);
    niyah_sha256((const uint8_t *)json, strlen(json), rh);
    if (!suffix || !receipt || write_receipt(receipt, "FIND", store_path, query, rh, s.document_hash) != 0) {
        chr_free(json); chr_free(suffix); chr_free(receipt); store_free(&s); return 1;
    }
    chr_free(suffix); store_free(&s); *json_out = json; *receipt_path_out = receipt; return 0;
}

typedef struct {
    char kind[16], store_hex[CHR_RECEIPT_VALUE_MAX], question_hex[CHR_RECEIPT_VALUE_MAX];
    char store_hash_hex[65], doc_hex[65], result_hex[65];
    unsigned seen;
} ChrReceipt;
static int receipt_read_line(FILE *f, char line[CHR_RECEIPT_LINE_MAX]) {
    size_t n = 0u; int c;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') {
            if (n != 0u && line[n - 1u] == '\r') --n;
            line[n] = '\0'; return 1;
        }
        if (c == 0 || n >= CHR_RECEIPT_LINE_MAX - 1u) return -1;
        line[n++] = (char)c;
    }
    return n == 0u && !ferror(f) ? 0 : -1;
}
static int receipt_field(ChrReceipt *r, const char *line) {
    const char *keys[] = {"kind:", "store_path_hex:", "question_hex:", "store_sha256:", "document_sha256:", "result_sha256:"};
    char *values[] = {r->kind, r->store_hex, r->question_hex, r->store_hash_hex, r->doc_hex, r->result_hex};
    const size_t caps[] = {sizeof(r->kind), sizeof(r->store_hex), sizeof(r->question_hex), 65u, 65u, 65u};
    size_t i;
    for (i = 0u; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        size_t key_len = strlen(keys[i]), value_len; unsigned bit = 1u << i;
        if (strncmp(line, keys[i], key_len) != 0) continue;
        value_len = strlen(line + key_len);
        if ((r->seen & bit) != 0u || value_len >= caps[i] || (i >= 3u && value_len != 64u)) return -1;
        memcpy(values[i], line + key_len, value_len + 1u); r->seen |= bit; return 0;
    }
    return -1;
}
static int receipt_read(const char *path, ChrReceipt *r) {
    FILE *f = fopen(path, "rb"); char line[CHR_RECEIPT_LINE_MAX]; int rc, ok;
    if (!f) return -1;
    memset(r, 0, sizeof(*r));
    ok = receipt_read_line(f, line) == 1 && strcmp(line, "CASPER-CHRONICLE-INTEGRITY-RECEIPT-V1") == 0;
    while (ok && (rc = receipt_read_line(f, line)) != 0) {
        if (rc < 0 || receipt_field(r, line) != 0) ok = 0;
    }
    if (fclose(f) != 0) ok = 0;
    if (!ok || r->seen != 63u || r->store_hex[0] == '\0') return -1;
    if (strcmp(r->kind, "INGEST") == 0) return r->question_hex[0] == '\0' ? 0 : -1;
    return (strcmp(r->kind, "QUERY") == 0 || strcmp(r->kind, "FIND") == 0) ? 0 : -1;
}
int casper_chronicle_verify(const char *receipt_path) {
    ChrReceipt receipt;
    uint8_t *store_bytes = NULL, *question_bytes = NULL;
    size_t store_n = 0u, question_n = 0u;
    ChrStore s; uint8_t actual[32]; char actual_hex[65]; char *json = NULL; int ok = 0;
    memset(&s, 0, sizeof(s));
    if (!receipt_path || receipt_read(receipt_path, &receipt) != 0) return 1;
    store_bytes = decode_hex(receipt.store_hex, &store_n); question_bytes = decode_hex(receipt.question_hex, &question_n);
    if (!store_bytes || !question_bytes || memchr(store_bytes, 0, store_n) ||
        !utf8_valid(question_bytes, question_n))
        goto done;
    if (!niyah_sha256_file((const char *)store_bytes, actual)) goto done;
    niyah_hash_to_hex(actual, actual_hex); if (strcmp(actual_hex, receipt.store_hash_hex) != 0) goto done;
    if (store_load((const char *)store_bytes, &s) != 0) goto done;
    niyah_hash_to_hex(s.document_hash, actual_hex);
    if (strcmp(actual_hex, receipt.doc_hex) != 0) goto done_store;
    if (strcmp(receipt.kind, "INGEST") == 0) {
        if (!niyah_sha256_file((const char *)store_bytes, actual)) goto done_store;
    } else if (strcmp(receipt.kind, "QUERY") == 0 &&
             reason_json(&s, (const char *)question_bytes, &json) == 0)
        niyah_sha256((const uint8_t *)json, strlen(json), actual);
    else if (strcmp(receipt.kind, "FIND") == 0 &&
             find_json(&s, (const char *)question_bytes, &json) == 0)
        niyah_sha256((const uint8_t *)json, strlen(json), actual);
    else goto done_store;
    niyah_hash_to_hex(actual, actual_hex); ok = strcmp(actual_hex, receipt.result_hex) == 0;
done_store:
    store_free(&s);
done:
    chr_free(store_bytes); chr_free(question_bytes); chr_free(json); return ok ? 0 : 1;
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
    if (full) { remove(full); chr_free(full); }
}
static int path_available(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return 0; }
    return errno == ENOENT;
}
static int scratch_available(const char *path, const char *question, const char *second) {
    char *store = path_suffix(path, ".chronicle"), *receipt = NULL, *proof = NULL;
    uint8_t hash[32]; char hex[65], suffix[34]; int ok = 0;
    if (!store || !path_available(path) || !path_available(store)) goto done;
    receipt = path_suffix(store, ".receipt");
    if (!receipt || !path_available(receipt)) goto done;
    niyah_sha256((const uint8_t *)question, strlen(question), hash); niyah_hash_to_hex(hash, hex);
    snprintf(suffix, sizeof(suffix), ".query-%.16s.receipt", hex);
    proof = path_suffix(store, suffix);
    if (!proof || !path_available(proof)) goto done;
    chr_free(proof); proof = NULL;
    if (second) {
        niyah_sha256((const uint8_t *)second, strlen(second), hash); niyah_hash_to_hex(hash, hex);
        snprintf(suffix, sizeof(suffix), ".query-%.16s.receipt", hex);
        proof = path_suffix(store, suffix);
        if (!proof || !path_available(proof)) goto done;
    }
    ok = 1;
done:
    chr_free(store); chr_free(receipt); chr_free(proof); return ok;
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
    if (!scratch_available(path, question, "هل سدد أحمدان دين خالد؟") ||
        !scratch_available(conflict_path, question, NULL) ||
        !scratch_available(missing_path, question, NULL)) return 1;
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
    chr_free(bytes); chr_free(json); chr_free(qreceipt); chr_free(receipt); chr_free(store);
    chr_free(json2); chr_free(qreceipt2); chr_free(receipt2); chr_free(store2);
    chr_free(json3); chr_free(qreceipt3); chr_free(receipt3); chr_free(store3);
    chr_free(repeat_store); chr_free(repeat_receipt); chr_free(similar_json); chr_free(similar_receipt);
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
static int benchmark_corpus(const char *path, size_t *bytes_out) {
    static const char *const records[] = {
        "@chronicle\tAhmed\tBORROWED_FROM\tKhalid\t100\tSAR\tT1\tASSERTED\tPOSITIVE\tCONFIRMED\n",
        "@chronicle\tSaud\tINTERMEDIARY_FOR\tKhalid\t-\tSAR\tT2\tASSERTED\tPOSITIVE\tCONFIRMED\n",
        "@chronicle\tAhmed\tPAID_TO\tSaud\t40\tSAR\tT3\tASSERTED\tPOSITIVE\tCONFIRMED\n",
        "@chronicle\tAhmed\tTRANSFER_TO\tSaud\t60\tSAR\tT4\tUNCERTAIN\tPOSITIVE\tPENDING\n"
    };
    FILE *f = fopen(path, "wbx"); size_t i, gap; int ok = 1;
    if (!f) return -1;
    *bytes_out = 0u;
    for (i = 0u; ok && i < 4u; ++i) {
        size_t n = strlen(records[i]);
        if (fwrite(records[i], 1u, n, f) != n) { ok = 0; break; }
        *bytes_out += n;
        if (i == 3u) break;
        for (gap = 0u; gap < 333320u; ++gap) {
            if (fwrite("word ", 1u, 5u, f) != 5u) { ok = 0; break; }
            *bytes_out += 5u;
        }
        if (fputc('\n', f) == EOF) ok = 0;
        ++*bytes_out;
    }
    if (fclose(f) != 0) ok = 0;
    return ok ? 0 : -1;
}
static double elapsed(struct timespec begin, struct timespec end) {
    return (double)(end.tv_sec - begin.tv_sec) +
           (double)(end.tv_nsec - begin.tv_nsec) / 1000000000.0;
}
int casper_chronicle_benchmark(void) {
    const char *path = "chronicle-benchmark.txt";
    char *store = NULL, *receipt = NULL, *json = NULL, *qreceipt = NULL;
    struct timespec begin, end; size_t bytes_n; double ingest_s, query_s, verify_s;
    uint8_t hash[32]; char corpus_hex[65], proof_hex[65]; int rc = 1;
    if (!scratch_available(path, "Ahmed Khalid", NULL) || benchmark_corpus(path, &bytes_n) != 0) return 1;
    chr_pool_reset_peak();
    if (timespec_get(&begin, TIME_UTC) != TIME_UTC) goto done;
    if (casper_chronicle_ingest(path, &store, &receipt) != 0) goto done;
    if (timespec_get(&end, TIME_UTC) != TIME_UTC) goto done;
    ingest_s = elapsed(begin, end);
    if (timespec_get(&begin, TIME_UTC) != TIME_UTC) goto done;
    if (casper_chronicle_query(store, "Ahmed Khalid", &json, &qreceipt) != 0 ||
        !expect_status(json, "PARTIAL") ||
        !strstr(json, "\"settled\":{\"num\":40,\"den\":1}")) goto done;
    if (timespec_get(&end, TIME_UTC) != TIME_UTC) goto done;
    query_s = elapsed(begin, end);
    if (timespec_get(&begin, TIME_UTC) != TIME_UTC || casper_chronicle_verify(qreceipt) != 0) goto done;
    if (timespec_get(&end, TIME_UTC) != TIME_UTC) goto done;
    verify_s = elapsed(begin, end);
    if (!niyah_sha256_file(path, hash)) goto done;
    niyah_hash_to_hex(hash, corpus_hex);
    if (!niyah_sha256_file(qreceipt, hash)) goto done;
    niyah_hash_to_hex(hash, proof_hex);
    printf("benchmark_words=1000000\ninput_bytes=%zu\nextracted_events=4\n"
           "elapsed_seconds=%.9f\nthroughput_bytes_per_second=%.3f\n"
           "query_seconds=%.9f\nverify_seconds=%.9f\n"
           "arena_capacity_bytes=%u\narena_peak_bytes=%zu\n"
           "corpus_sha256=%s\nreceipt_sha256=%s\nstatus=PARTIAL\nsettled=40/100\n",
           bytes_n, ingest_s, ingest_s > 0.0 ? (double)bytes_n / ingest_s : 0.0,
           query_s, verify_s, CHRONICLE_POOL_SIZE, chr_pool_peak(), corpus_hex, proof_hex);
    rc = 0;
done:
    if (qreceipt) remove(qreceipt);
    if (receipt) remove(receipt);
    if (store) remove(store);
    remove(path);
    chr_free(json); chr_free(qreceipt); chr_free(receipt); chr_free(store);
    printf("arena_live_after_free=%zu\n", chr_pool_used());
    return rc || chr_pool_used() != 0u ? 1 : 0;
}
