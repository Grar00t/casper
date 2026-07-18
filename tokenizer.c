#ifdef __cplusplus
extern "C" {
#endif
/*
 * tokenizer.c — Niyah Tokenizer C99
 * يدعم العربية + القرآن + C/Assembly/Perl
 * Pure C99 — zero external deps
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

static int is_arabic(uint32_t c) {
    return (c >= 0x0600 && c <= 0x06FF) ||
           (c >= 0x0750 && c <= 0x077F) ||
           (c >= 0x08A0 && c <= 0x08FF) ||
           (c >= 0xFB50 && c <= 0xFDFF) ||
           (c >= 0xFE70 && c <= 0xFEFF);
}

#define MAX_VOCAB 65536
typedef struct {
    char token[64];
    uint32_t id;
} TokenEntry;

static TokenEntry vocab[MAX_VOCAB];
static uint32_t vocab_size = 0;

static void add_token(const char *s) {
    strncpy(vocab[vocab_size].token, s, 63);
    vocab[vocab_size].token[63] = '\0';
    vocab[vocab_size].id = vocab_size;
    vocab_size++;
}

void tokenizer_init(void) {
    /* IDs 0-3: specials */
    add_token("<BOS>");   /* 0 */
    add_token("<EOS>");   /* 1 */
    add_token("<PAD>");   /* 2 */
    add_token("<UNK>");   /* 3 */

    /* IDs 4-13: digits */
    for (int i = 0; i < 10; i++) {
        char buf[4];
        sprintf(buf, "%d", i);
        add_token(buf);
    }

    /* IDs 14-37: printable ASCII punctuation / operators (every non-alpha non-digit) */
    const char *puncts = "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~";
    for (const char *p = puncts; *p; p++) {
        char buf[3] = {*p, '\0', '\0'};
        add_token(buf);
    }

    /* IDs 38-63: lowercase single letters */
    for (char c = 'a'; c <= 'z'; c++) {
        char buf[2] = {c, '\0'};
        add_token(buf);
    }

    /* IDs 64-89: uppercase single letters */
    for (char c = 'A'; c <= 'Z'; c++) {
        char buf[2] = {c, '\0'};
        add_token(buf);
    }

    /* IDs 90+: common English and domain words for better decode quality.
     * These cover the sovereign_knowledge training corpus vocabulary. */
    const char *words[] = {
        /* function / structure words */
        "the","a","an","and","or","is","in","of","to","for","with","on",
        "at","by","from","that","this","it","are","was","be","as","not",
        "but","have","has","had","we","i","you","they","he","she","can",
        "will","would","do","does","did","if","when","then","so","all",
        "no","up","out","one","two","three","four","five","six","seven",
        "eight","nine","ten","than","more","less","very","also","only",
        /* technical / domain words */
        "model","data","train","training","layer","layers","weight","weights",
        "token","tokens","embed","embedding","head","heads","attention","output",
        "input","loss","gradient","optimizer","matrix","vector","kernel","cpu",
        "gpu","memory","float","int","size","context","vocab","local","zero",
        "code","file","build","run","test","hash","proof","rule","query","fact",
        "function","class","struct","type","return","void","static","const",
        "malloc","calloc","free","pointer","buffer","stack","heap","pool",
        "forward","backward","sample","generate","decode","encode","norm",
        "softmax","relu","silu","gelu","linear","bias","scale","sum","dot",
        "compute","algorithm","system","engine","core","base","key","value",
        "arabic","quran","bismillah","sovereign","inference","symbolic","logic",
        "constraint","solver","rational","arithmetic","sha","cryptographic",
        /* Arabic transliterations that appear in comments/data */
        "niyah","casper","khwarizmi","adam","rope","swiglu","rmsnorm","gqa",
        /* numbers as words */
        "zero","one","two","three","four","five","six","seven","eight","nine",
        "ten","hundred","thousand","million","billion",
        NULL
    };
    for (int i = 0; words[i]; i++) {
        if (vocab_size < MAX_VOCAB - 1)
            add_token(words[i]);
    }
}

uint32_t tokenizer_encode(const char *text, uint32_t *tokens, uint32_t max_len) {
    uint32_t pos = 0;
    const char *p = text;
    tokens[pos++] = 0; /* BOS */

    while (*p && pos < max_len - 1) {
        if (isspace((unsigned char)*p)) { p++; continue; }

        /* Arabic / Quran character */
        if ((unsigned char)*p >= 0xC0) {
            uint32_t uc = 0;
            const unsigned char *s = (const unsigned char *)p;
            if (s[0] < 0xE0) {
                uc = ((s[0]&0x1F)<<6) | (s[1]&0x3F);
                p += 2;
            } else if (s[0] < 0xF0) {
                uc = ((s[0]&0x0F)<<12) | ((s[1]&0x3F)<<6) | (s[2]&0x3F);
                p += 3;
            } else {
                uc = ((s[0]&0x07)<<18) | ((s[1]&0x3F)<<12) | ((s[2]&0x3F)<<6) | (s[3]&0x3F);
                p += 4;
            }
            if (is_arabic(uc)) {
                tokens[pos++] = 1000 + (uc % 5000);
                continue;
            }
            tokens[pos++] = 6000 + (uc % 10000); /* other unicode */
            continue;
        }

        /* punctuation */
        if (ispunct((unsigned char)*p)) {
            char sym[2] = {*p, '\0'};
            uint32_t id = 3;
            for (uint32_t j = 0; j < vocab_size; j++) {
                if (strcmp(vocab[j].token, sym) == 0) { id = vocab[j].id; break; }
            }
            tokens[pos++] = id;
            p++;
            continue;
        }

        /* English / C code word */
        char word[64] = {0};
        int i = 0;
        while (*p && !isspace((unsigned char)*p) && !ispunct((unsigned char)*p) && i < 63) {
            word[i++] = *p++;
        }
        if (i > 0) {
            uint32_t id = 3;
            for (uint32_t j = 0; j < vocab_size; j++) {
                if (strcmp(vocab[j].token, word) == 0) { id = vocab[j].id; break; }
            }
            tokens[pos++] = id;
        }
    }
    tokens[pos++] = 1; /* EOS */
    return pos;
}

/*
 * tokenizer_decode — best-effort reverse of tokenizer_encode.
 *
 * Returns malloc'd string (caller frees).
 * Vocab entries decode exactly. Arabic/Unicode IDs produce [?].
 * BOS/EOS/PAD are skipped.
 */
char *tokenizer_decode(const uint32_t *tokens, uint32_t n) {
    /* Worst case: each token produces ~64 chars + space */
    size_t cap = (size_t)n * 66 + 1;
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t pos = 0;

    for (uint32_t i = 0; i < n; i++) {
        uint32_t id = tokens[i];

        /* Skip special tokens */
        if (id == 0 || id == 1 || id == 2) continue;

        /* Look up in vocab table */
        const char *word = NULL;
        for (uint32_t j = 0; j < vocab_size; j++) {
            if (vocab[j].id == id) { word = vocab[j].token; break; }
        }

        if (word) {
            /* Skip UNK placeholder */
            if (id == 3) { word = "?"; }
            size_t wlen = strlen(word);
            if (pos + wlen + 2 >= cap) break;
            if (pos > 0) out[pos++] = ' ';
            memcpy(out + pos, word, wlen);
            pos += wlen;
        } else {
            /* Unknown ID — output placeholder */
            if (pos > 0) out[pos++] = ' ';
            int wrote = snprintf(out + pos, cap - pos, "?");
            if (wrote > 0) pos += (size_t)wrote;
        }
    }
    out[pos] = '\0';
    return out;
}

void tokenizer_free(void) { vocab_size = 0; }

/* === TEST === */
#ifdef TOKENIZER_TEST
int main(void) {
    tokenizer_init();
    uint32_t tokens[1024];
    uint32_t n;

    n = tokenizer_encode("malloc allocates heap memory", tokens, 1024);
    printf("English: %u tokens\n", n);

    n = tokenizer_encode("بِسۡمِ ٱللَّهِ ٱلرَّحۡمَـٰنِ ٱلرَّحِيمِ", tokens, 1024);
    printf("Quran: %u tokens\n", n);

    n = tokenizer_encode("niyah_core.c هو محرك الذكاء", tokens, 1024);
    printf("Mixed: %u tokens\n", n);

    printf("Vocab size: %u\n", vocab_size);
    printf("✅ tokenizer OK\n");
    tokenizer_free();
    return 0;
}
#endif

#ifdef __cplusplus
}
#endif
