#include "chronicle_pool.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "pool failure line %d\n", __LINE__); return 1; } } while (0)
int main(void) {
    size_t checks = 0, i; unsigned char *a, *b; void *slots[128];
    CHECK(chr_malloc(SIZE_MAX) == NULL);
    CHECK(chr_calloc(SIZE_MAX, 2u) == NULL);
    for (i = 0; i < 128u; ++i) {
        slots[i] = chr_calloc(i + 1u, 1u);
        CHECK(slots[i] != NULL);
        CHECK((uintptr_t)slots[i] % _Alignof(max_align_t) == 0u);
        CHECK(((unsigned char *)slots[i])[i] == 0u);
    }
    for (i = 0; i < 128u; i += 2u) chr_free(slots[i]);
    for (i = 1; i < 128u; i += 2u) chr_free(slots[i]);
    CHECK(chr_pool_used() == 0u);
    a = chr_malloc(1024u); CHECK(a != NULL); memset(a, 0x5a, 1024u);
    CHECK(chr_realloc(a, SIZE_MAX) == NULL);
    CHECK(a[1023] == 0x5a);
    b = chr_realloc(a, 4096u); CHECK(b != NULL); CHECK(b[1023] == 0x5a);
    chr_free(b);
    a = chr_malloc(CHRONICLE_POOL_SIZE - 128u); CHECK(a != NULL);
    CHECK(chr_malloc(1024u) == NULL);
    chr_free(a); CHECK(chr_pool_used() == 0u);
    CHECK(chr_pool_peak() <= CHRONICLE_POOL_SIZE);
    printf("chronicle_pool_checks=%zu failures=0\n", checks);
    return 0;
}
