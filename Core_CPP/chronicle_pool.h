#ifndef CHRONICLE_POOL_H
#define CHRONICLE_POOL_H
#include <stddef.h>

#define CHRONICLE_POOL_SIZE (16u * 1024u * 1024u)
/* One process-local arena. Calls are serial; outputs live until explicitly freed. */
void *chr_malloc(size_t size);
void *chr_calloc(size_t count, size_t size);
void *chr_realloc(void *ptr, size_t size);
void chr_free(void *ptr);
size_t chr_pool_used(void);
size_t chr_pool_peak(void);
void chr_pool_reset_peak(void);
#endif
