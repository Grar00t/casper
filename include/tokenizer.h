/*
 * tokenizer.h - public API for tokenizer.c
 *
 * Ids are stable and deterministic. Vocabulary v2 preserves the historical
 * ids 0..1499 and appends 256 UTF-8 byte-fallback ids so unseen text is no
 * longer collapsed to <UNK>.
 */
#ifndef CASPER_TOKENIZER_H
#define CASPER_TOKENIZER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TOK_BOS 0u
#define TOK_EOS 1u
#define TOK_PAD 2u
#define TOK_UNK 3u

/* Historical ids occupy 0..1499; byte fallback occupies 1500..1755. */
#define TOK_BYTE_COUNT 256u
#define TOK_MAX_VOCAB 2048u
#define TOK_STR_MAX 64u

void tokenizer_init(void);
uint32_t tokenizer_vocab_size(void);
uint32_t tokenizer_encode(const char *text, uint32_t *tokens, uint32_t max_len);
char *tokenizer_decode(const uint32_t *tokens, uint32_t n);
void tokenizer_free_string(char *s);
void tokenizer_free(void);

#ifdef __cplusplus
}
#endif
#endif /* CASPER_TOKENIZER_H */
