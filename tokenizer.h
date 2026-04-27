#ifndef NIYAH_TOKENIZER_H
#define NIYAH_TOKENIZER_H

/*
 * tokenizer.h — Niyah Tokenizer public API
 * Pure C99 — zero external deps. UTF-8 aware (Arabic + ASCII).
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the global vocabulary. Call once before encode/decode. */
void tokenizer_init(void);

/* Free / reset vocabulary state. */
void tokenizer_free(void);

/*
 * tokenizer_encode()
 * Encode `text` (UTF-8) into `tokens` (max `max_len` ids).
 * Returns the number of tokens written.
 */
uint32_t tokenizer_encode(const char *text, uint32_t *tokens, uint32_t max_len);

/*
 * tokenizer_decode()
 * Decode `n` token ids back into a heap-allocated UTF-8 string.
 * Caller owns the returned pointer (free with `free()`).
 * Returns NULL on allocation failure.
 */
char *tokenizer_decode(const uint32_t *tokens, uint32_t n);

#ifdef __cplusplus
}
#endif

#endif /* NIYAH_TOKENIZER_H */
