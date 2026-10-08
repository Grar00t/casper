#ifndef CASPER_CHRONICLE_H
#define CASPER_CHRONICLE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Chronicle v1 consumes explicit @chronicle event records embedded in UTF-8
 * source text. It does not infer facts from unrestricted prose.
 */
int casper_chronicle_ingest(const char *input_path, char **store_path_out,
                            char **receipt_path_out);
int casper_chronicle_query(const char *store_path, const char *question,
                           char **json_out, char **receipt_path_out);
/* Lexical-only search in source text, not semantic reasoning. */
int casper_chronicle_find(const char *store_path, const char *query,
                          char **json_out, char **receipt_path_out);
int casper_chronicle_verify(const char *receipt_path);
int casper_chronicle_self_test(void);
int casper_chronicle_benchmark(void);
void casper_chronicle_free(void *ptr);

#ifdef __cplusplus
}
#endif

#endif
