/*
 * tokenizer.c - deterministic UTF-8 tokenizer for Casper/NIYAH.
 *
 * Vocabulary v2 preserves every historical id and appends a byte fallback:
 *
 *      0 ..    3   <BOS> <EOS> <PAD> <UNK>
 *      4 ..   13   digits
 *     14 ..   45   ASCII punctuation
 *     46 ..   71   a..z
 *     72 ..   97   A..Z
 *     98 ..  267   compact English/domain word list
 *    268 .. 1499   Arabic codepoints
 *   1500 .. 1755   raw UTF-8 byte values 0x00..0xFF
 *
 * Known tokens stay compact. Unknown words, whitespace and unsupported
 * Unicode are represented losslessly as UTF-8 bytes instead of collapsing to
 * <UNK>. Matching is exact and case-sensitive so encode/decode never changes
 * teacher text such as SHA, TCP, Casper, or mixed-case identifiers.
 */

#include "tokenizer.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char token[TOK_STR_MAX];
    uint32_t id;
} TokenEntry;

typedef struct {
    uint32_t first;
    uint32_t last;
} CodepointRange;

static const CodepointRange arabic_ranges[] = {
    {0x0600u, 0x06FFu},
    {0x0750u, 0x077Fu},
    {0x08A0u, 0x08FFu},
    {0xFB50u, 0xFDFFu},
    {0xFE70u, 0xFEFFu}
};
#define ARABIC_RANGE_COUNT (sizeof(arabic_ranges) / sizeof(arabic_ranges[0]))

static TokenEntry vocab[TOK_MAX_VOCAB];
static uint32_t vocab_size = 0u;
static uint32_t word_end = 0u;
static uint32_t char_base = 0u;
static uint32_t char_count = 0u;
static uint32_t byte_base = 0u;
static int initialized = 0;

static uint32_t add_token(const char *s)
{
    if (!s || !s[0] || vocab_size >= TOK_MAX_VOCAB) return TOK_UNK;
    (void)strncpy(vocab[vocab_size].token, s, TOK_STR_MAX - 1u);
    vocab[vocab_size].token[TOK_STR_MAX - 1u] = '\0';
    vocab[vocab_size].id = vocab_size;
    return vocab_size++;
}

static size_t utf8_put(uint32_t uc, char *out, size_t max)
{
    if (!out || max < 5u) return 0u;
    if (uc < 0x80u) {
        out[0] = (char)uc;
        out[1] = '\0';
        return 1u;
    }
    if (uc < 0x800u) {
        out[0] = (char)(0xC0u | (uc >> 6));
        out[1] = (char)(0x80u | (uc & 0x3Fu));
        out[2] = '\0';
        return 2u;
    }
    if (uc < 0x10000u) {
        out[0] = (char)(0xE0u | (uc >> 12));
        out[1] = (char)(0x80u | ((uc >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (uc & 0x3Fu));
        out[3] = '\0';
        return 3u;
    }
    out[0] = (char)(0xF0u | (uc >> 18));
    out[1] = (char)(0x80u | ((uc >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((uc >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (uc & 0x3Fu));
    out[4] = '\0';
    return 4u;
}

static uint32_t codepoint_id(uint32_t uc)
{
    uint32_t offset = 0u;
    size_t r;

    if (char_count == 0u) return TOK_UNK;
    for (r = 0u; r < ARABIC_RANGE_COUNT; ++r) {
        uint32_t span = arabic_ranges[r].last - arabic_ranges[r].first + 1u;
        if (uc >= arabic_ranges[r].first && uc <= arabic_ranges[r].last) {
            return char_base + offset + (uc - arabic_ranges[r].first);
        }
        offset += span;
    }
    return TOK_UNK;
}

/* Exact lookup only. Case folding would make encode/decode lossy. */
static uint32_t lookup(const char *s)
{
    uint32_t j;

    if (!s || !s[0]) return TOK_UNK;
    for (j = 0u; j < word_end; ++j) {
        if (strcmp(vocab[j].token, s) == 0) return vocab[j].id;
    }
    return TOK_UNK;
}

static void emit_bytes(const unsigned char *bytes, size_t n,
                       uint32_t *tokens, uint32_t *pos, uint32_t max_len)
{
    size_t i;
    if (!bytes || !tokens || !pos || byte_base == 0u) return;
    for (i = 0u; i < n && *pos + 1u < max_len; ++i) {
        tokens[(*pos)++] = byte_base + (uint32_t)bytes[i];
    }
}

void tokenizer_init(void)
{
    static const char *const words[] = {
        "the","a","an","and","or","is","in","of","to","for","with","on","at","by","from",
        "that","this","it","are","was","be","as","not","but","have","has","had","we","i","you",
        "they","he","she","can","will","would","do","does","did","if","when","then","so","all",
        "no","up","out","than",
        "more","less","very","also","only","model","data","train","training","layer","layers","weight",
        "weights","token","tokens","embed","embedding","head","heads","attention","output","input","loss",
        "gradient","optimizer","matrix","vector","kernel","cpu","gpu","memory","float","int","size","context",
        "vocab","local","code","file","build","run","test","hash","proof","rule","query","fact",
        "function","class","struct","type","return","void","static","const","malloc","calloc","free","pointer",
        "buffer","stack","heap","pool","forward","backward","sample","generate","decode","encode","norm","softmax",
        "relu","silu","gelu","linear","bias","scale","sum","dot","compute","algorithm","system","engine","core",
        "base","key","value","arabic","quran","bismillah","inference","symbolic","logic","constraint","solver",
        "rational","arithmetic","sha","cryptographic","niyah","casper","khwarizmi","adam","rope","swiglu","rmsnorm",
        "gqa","zero","one","two","three","four","five","six","seven","eight","nine","ten","hundred","thousand",
        "million","billion", NULL
    };
    static const char punctuation[] = "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~";
    const char *p;
    size_t i;
    size_t r;
    int d;
    int b;
    char c;

    if (initialized) return;
    vocab_size = 0u;
    word_end = 0u;
    char_base = 0u;
    char_count = 0u;
    byte_base = 0u;

    (void)add_token("<BOS>");
    (void)add_token("<EOS>");
    (void)add_token("<PAD>");
    (void)add_token("<UNK>");

    for (d = 0; d < 10; ++d) {
        char buf[4];
        (void)snprintf(buf, sizeof(buf), "%d", d);
        (void)add_token(buf);
    }
    for (p = punctuation; *p; ++p) {
        char buf[2] = {'\0', '\0'};
        buf[0] = *p;
        (void)add_token(buf);
    }
    for (c = 'a'; c <= 'z'; ++c) {
        char buf[2] = {'\0', '\0'};
        buf[0] = c;
        (void)add_token(buf);
    }
    for (c = 'A'; c <= 'Z'; ++c) {
        char buf[2] = {'\0', '\0'};
        buf[0] = c;
        (void)add_token(buf);
    }
    for (i = 0u; words[i]; ++i) (void)add_token(words[i]);
    word_end = vocab_size;

    char_base = vocab_size;
    for (r = 0u; r < ARABIC_RANGE_COUNT; ++r) {
        uint32_t uc;
        for (uc = arabic_ranges[r].first; uc <= arabic_ranges[r].last; ++uc) {
            char utf8[8];
            if (utf8_put(uc, utf8, sizeof(utf8)) == 0u) continue;
            if (add_token(utf8) == TOK_UNK && vocab_size >= TOK_MAX_VOCAB) break;
        }
    }
    char_count = vocab_size - char_base;

    byte_base = vocab_size;
    for (b = 0; b < (int)TOK_BYTE_COUNT; ++b) {
        char label[8];
        (void)snprintf(label, sizeof(label), "<%02X>", (unsigned int)b);
        if (add_token(label) == TOK_UNK && vocab_size >= TOK_MAX_VOCAB) break;
    }

    initialized = 1;
}

uint32_t tokenizer_vocab_size(void)
{
    return vocab_size;
}

uint32_t tokenizer_encode(const char *text, uint32_t *tokens, uint32_t max_len)
{
    const unsigned char *p;
    uint32_t pos = 0u;

    if (!initialized) tokenizer_init();
    if (!tokens || max_len < 2u) return 0u;
    if (!text) text = "";

    tokens[pos++] = TOK_BOS;
    p = (const unsigned char *)text;

    while (*p && pos + 1u < max_len) {
        if (*p >= 0xC0u) {
            const unsigned char *start = p;
            uint32_t uc = 0xFFFDu;
            uint32_t id;
            size_t width = 1u;

            if ((*p & 0xE0u) == 0xC0u && p[1] && (p[1] & 0xC0u) == 0x80u) {
                uc = ((uint32_t)(p[0] & 0x1Fu) << 6) | (uint32_t)(p[1] & 0x3Fu);
                width = 2u;
            } else if ((*p & 0xF0u) == 0xE0u && p[1] && p[2]
                       && (p[1] & 0xC0u) == 0x80u && (p[2] & 0xC0u) == 0x80u) {
                uc = ((uint32_t)(p[0] & 0x0Fu) << 12)
                   | ((uint32_t)(p[1] & 0x3Fu) << 6)
                   | (uint32_t)(p[2] & 0x3Fu);
                width = 3u;
            } else if ((*p & 0xF8u) == 0xF0u && p[1] && p[2] && p[3]
                       && (p[1] & 0xC0u) == 0x80u && (p[2] & 0xC0u) == 0x80u
                       && (p[3] & 0xC0u) == 0x80u) {
                uc = ((uint32_t)(p[0] & 0x07u) << 18)
                   | ((uint32_t)(p[1] & 0x3Fu) << 12)
                   | ((uint32_t)(p[2] & 0x3Fu) << 6)
                   | (uint32_t)(p[3] & 0x3Fu);
                width = 4u;
            }

            id = codepoint_id(uc);
            if (id != TOK_UNK) tokens[pos++] = id;
            else emit_bytes(start, width, tokens, &pos, max_len);
            p += width;
            continue;
        }

        if (*p >= 0x80u || isspace(*p)) {
            emit_bytes(p, 1u, tokens, &pos, max_len);
            ++p;
            continue;
        }

        if (ispunct(*p)) {
            char sym[2] = {'\0', '\0'};
            sym[0] = (char)*p;
            tokens[pos++] = lookup(sym);
            ++p;
            continue;
        }

        {
            unsigned char word[TOK_STR_MAX];
            size_t i = 0u;
            uint32_t id;
            while (*p && *p < 0x80u && !isspace(*p) && !ispunct(*p)
                   && i + 1u < sizeof(word)) {
                word[i++] = *p++;
            }
            word[i] = '\0';
            if (i == 0u) continue;
            id = lookup((const char *)word);
            if (id != TOK_UNK) tokens[pos++] = id;
            else emit_bytes(word, i, tokens, &pos, max_len);
        }
    }

    tokens[pos++] = TOK_EOS;
    return pos;
}

char *tokenizer_decode(const uint32_t *tokens, uint32_t n)
{
    size_t cap;
    size_t pos = 0u;
    char *out;
    uint32_t i;

    if (!initialized) tokenizer_init();
    if (!tokens && n != 0u) return NULL;

    cap = (size_t)n * (TOK_STR_MAX + 1u) + 1u;
    out = (char *)malloc(cap);
    if (!out) return NULL;

    for (i = 0u; i < n; ++i) {
        uint32_t id = tokens[i];
        if (id == TOK_BOS || id == TOK_EOS || id == TOK_PAD) continue;

        if (byte_base != 0u && id >= byte_base && id < byte_base + TOK_BYTE_COUNT) {
            if (pos + 1u >= cap) break;
            out[pos++] = (char)(unsigned char)(id - byte_base);
            continue;
        }

        {
            const char *word = (id < vocab_size) ? vocab[id].token : vocab[TOK_UNK].token;
            size_t wlen = strlen(word);
            if (pos + wlen >= cap) break;
            (void)memcpy(out + pos, word, wlen);
            pos += wlen;
        }
    }

    out[pos] = '\0';
    return out;
}

void tokenizer_free_string(char *s)
{
    free(s);
}

void tokenizer_free(void)
{
    vocab_size = 0u;
    word_end = 0u;
    char_base = 0u;
    char_count = 0u;
    byte_base = 0u;
    initialized = 0;
}

#ifdef TOKENIZER_TEST
int main(void)
{
    static const char *const cases[] = {
        "malloc allocates heap memory",
        "unknown teacher vocabulary survives exactly",
        "SHA-256 Casper TCP UDP",
        "\xd8\xa8\xd8\xb3\xd9\x85 \xd8\xa7\xd9\x84\xd9\x84\xd9\x87",
        "casper \xd9\x86\xd9\x8a\xd8\xa9 engine",
        "emoji: \xf0\x9f\xa7\xa0 CJK: \xe6\xb1\x89\xe5\xad\x97",
        "line one\nline two\tindent",
        NULL
    };
    uint32_t tokens[2048];
    size_t i;
    int failures = 0;

    tokenizer_init();
    (void)printf("vocab_size = %u  (cap %u)\n", tokenizer_vocab_size(), TOK_MAX_VOCAB);

    for (i = 0u; cases[i]; ++i) {
        uint32_t n = tokenizer_encode(cases[i], tokens, 2048u);
        char *rt = tokenizer_decode(tokens, n);
        int bad = (rt == NULL) || strcmp(rt, cases[i]) != 0;
        (void)printf("case %zu -> %u tokens%s\n", i + 1u, n, bad ? " [FAIL]" : "");
        if (bad) {
            (void)fprintf(stderr, "expected: %s\nactual:   %s\n", cases[i], rt ? rt : "(null)");
            ++failures;
        }
        tokenizer_free_string(rt);
    }

    tokenizer_free();
    if (failures != 0) {
        (void)printf("FAIL: %d tokenizer round-trip case(s)\n", failures);
        return 1;
    }
    (void)printf("PASS: tokenizer UTF-8 round trips are exact\n");
    return 0;
}
#endif
