#include "casper_chronicle.h"
#include "chronicle_pool.h"
#include <stdio.h>
#include <string.h>

#define MAX_FAILURE_SITES 128u
static size_t allocation_calls, fail_at, injected, checks;
static char receipts[4][256];
void *__real_chr_malloc(size_t size);
void *__real_chr_calloc(size_t count, size_t size);
void *__real_chr_realloc(void *ptr, size_t size);
void *__wrap_chr_malloc(size_t size);
void *__wrap_chr_calloc(size_t count, size_t size);
void *__wrap_chr_realloc(void *ptr, size_t size);

/* Intercept Chronicle's arena requests, leaving stdio's allocations alone. */
static int must_fail(void) {
    ++allocation_calls;
    if (allocation_calls != fail_at) return 0;
    ++injected;
    return 1;
}
void *__wrap_chr_malloc(size_t size) { return must_fail() ? NULL : __real_chr_malloc(size); }
void *__wrap_chr_calloc(size_t count, size_t size) { return must_fail() ? NULL : __real_chr_calloc(count, size); }
void *__wrap_chr_realloc(void *ptr, size_t size) { return must_fail() ? NULL : __real_chr_realloc(ptr, size); }

#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "allocation failure line=%d fail_at=%zu calls=%zu arena=%zu\n", __LINE__, fail_at, allocation_calls, chr_pool_used()); return 1; } } while (0)

static int write_source(const char *path) {
    FILE *file = fopen(path, "wbx"); int ok;
    if (!file) return 1;
    ok = fputs("Alice knows Bob.\n"
               "@chronicle\tAlice\tBORROWED_FROM\tBob\t100\tSAR\tT1\tASSERTED\tPOSITIVE\tCONFIRMED\n"
               "@chronicle\tAlice\tPAID_TO\tBob\t100\tSAR\tT2\tASSERTED\tPOSITIVE\tCONFIRMED\n", file) >= 0;
    if (fclose(file) != 0) ok = 0;
    return ok ? 0 : 1;
}

static int invoke(size_t operation, const char *source, char **first, char **second) {
    const char *store = "fault-source.txt.chronicle";
    if (operation < 2u) return casper_chronicle_ingest(source, first, second);
    if (operation == 2u) return casper_chronicle_query(store, "@claim\tAlice\tknows\tBob", first, second);
    if (operation == 3u) return casper_chronicle_query(store, "Alice Bob", first, second);
    if (operation == 4u) return casper_chronicle_find(store, "Alice", first, second);
    return casper_chronicle_verify(receipts[operation - 5u]);
}

static int prepare(void) {
    size_t operation;
    CHECK(write_source("fault-source.txt") == 0);
    for (operation = 1u; operation <= 4u; ++operation) {
        char *first = NULL, *second = NULL;
        CHECK(invoke(operation, "fault-source.txt", &first, &second) == 0);
        CHECK(first != NULL && second != NULL);
        CHECK(strlen(second) < sizeof(receipts[0]));
        memcpy(receipts[operation - 1u], second, strlen(second) + 1u);
        casper_chronicle_free(first); casper_chronicle_free(second);
        CHECK(chr_pool_used() == 0u);
    }
    return 0;
}

static int sweep(size_t operation, size_t *observed) {
    size_t index; int rc = 1;
    static const char *const names[] = {"new-ingest", "existing-ingest", "query-literal", "query-debt",
        "find", "verify-ingest", "verify-literal", "verify-debt", "verify-find"};
    for (index = 1u; index <= MAX_FAILURE_SITES; ++index) {
        char source[64] = "fault-source.txt", *first = NULL, *second = NULL;
        if (operation == 0u) {
            int n = snprintf(source, sizeof(source), "new-source-%zu.txt", index);
            CHECK(n > 0 && (size_t)n < sizeof(source)); CHECK(write_source(source) == 0);
        }
        allocation_calls = 0u; fail_at = index; injected = 0u;
        rc = invoke(operation, source, &first, &second);
        if (injected != 0u) {
            ++*observed; CHECK(rc != 0); CHECK(first == NULL && second == NULL);
        } else CHECK(rc == 0);
        casper_chronicle_free(first); casper_chronicle_free(second);
        CHECK(chr_pool_used() == 0u);
        if (injected == 0u) break;
    }
    CHECK(index <= MAX_FAILURE_SITES);
    printf("operation=%s allocation_failure_sites=%zu recovery_exit=%d\n", names[operation], index - 1u, rc);
    return 0;
}

int main(void) {
    size_t operation, observed = 0u;
    CHECK(prepare() == 0);
    for (operation = 0u; operation < 9u; ++operation) CHECK(sweep(operation, &observed) == 0);
    printf("allocation_fault_checks=%zu injections=%zu failures=0 arena_live_bytes=%zu\n", checks, observed, chr_pool_used());
    return 0;
}
