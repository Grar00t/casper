#ifndef CASPER_CHRONICLE_H
#define CASPER_CHRONICLE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Chronicle v2 consumes explicit @chronicle event records, bounded Arabic debt
 * sentences (CHRONICLE_ARABIC.md), and three-token literals (CHRONICLE_LITERAL.md).
 * Existing v1 stores retain the original explicit-record grammar.
 * One shared 16 MiB arena; serial calls only. Release successful outputs with
 * casper_chronicle_free. Error returns do not transfer ownership of any output.
 * It does not infer facts from unrestricted prose.
 */
int casper_chronicle_ingest(const char *input_path, char **store_path_out,
                            char **receipt_path_out);
int casper_chronicle_query(const char *store_path, const char *question,
                           char **json_out, char **receipt_path_out);
/* Lexical-only search in source text, not semantic reasoning. */
int casper_chronicle_find(const char *store_path, const char *query,
                          char **json_out, char **receipt_path_out);
/* 0: VALID, 3: historical query result UNSUPPORTED, other nonzero: INVALID. */
int casper_chronicle_verify(const char *receipt_path);
int casper_chronicle_self_test(void);
int casper_chronicle_benchmark(void);
void casper_chronicle_free(void *ptr);

#ifdef __cplusplus
}
#endif

#endif
