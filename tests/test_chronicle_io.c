#include "casper_chronicle.h"
#include "chronicle_pool.h"
#include <stdio.h>
#include <string.h>

/* Inject short reads without changing any user file; observe follow-up I/O. */
enum { NO_FAULT, SOURCE_SHORT, RECEIPT_SHORT };
static int fault, failed_read;
static FILE *watched;
static size_t reads_after_failure;
FILE *__real_fopen(const char *path, const char *mode);
size_t __real_fread(void *ptr, size_t size, size_t count, FILE *stream);
int __real_fgetc(FILE *stream);
int __real_fclose(FILE *stream);
FILE *__wrap_fopen(const char *path, const char *mode);
size_t __wrap_fread(void *ptr, size_t size, size_t count, FILE *stream);
int __wrap_fgetc(FILE *stream);
int __wrap_fclose(FILE *stream);

FILE *__wrap_fopen(const char *path, const char *mode) {
    FILE *stream = __real_fopen(path, mode);
    const char *target = fault == SOURCE_SHORT ? "io.txt" : "io.txt.chronicle.receipt";
    if (fault && stream && strcmp(path, target) == 0 && strcmp(mode, "rb") == 0)
        watched = stream;
    return stream;
}

size_t __wrap_fread(void *ptr, size_t size, size_t count, FILE *stream) {
    if (stream == watched && fault == SOURCE_SHORT && count > 1u) {
        failed_read = 1;
        return __real_fread(ptr, size, count - 1u, stream);
    }
    return __real_fread(ptr, size, count, stream);
}

int __wrap_fgetc(FILE *stream) {
    if (stream == watched) {
        if (failed_read) ++reads_after_failure;
        if (fault == RECEIPT_SHORT) { failed_read = 1; return EOF; }
    }
    return __real_fgetc(stream);
}

int __wrap_fclose(FILE *stream) {
    if (stream == watched) watched = NULL;
    return __real_fclose(stream);
}

static int run_fault(int mode) {
    char *store = NULL, *receipt = NULL;
    int rc, fail;
    fault = mode; failed_read = 0; reads_after_failure = 0u;
    rc = casper_chronicle_ingest("io.txt", &store, &receipt);
    fault = NO_FAULT;
    fail = rc == 0 || !failed_read || reads_after_failure != 0u || store || receipt;
    printf("io_fault=%d exit=%d reads_after_failure=%zu outputs_null=%d\n",
           mode, rc, reads_after_failure, store == NULL && receipt == NULL);
    casper_chronicle_free(store); casper_chronicle_free(receipt);
    return fail || chr_pool_used() != 0u;
}

int main(void) {
    FILE *stream = fopen("io.txt", "wbx");
    char *store = NULL, *receipt = NULL;
    int failures = 0;
    if (!stream) return 1;
    if (fputs("Alice knows Bob.\n", stream) < 0 || fclose(stream) != 0) return 1;
    failures += run_fault(SOURCE_SHORT);
    if (casper_chronicle_ingest("io.txt", &store, &receipt) != 0) return 1;
    casper_chronicle_free(store); casper_chronicle_free(receipt);
    failures += run_fault(RECEIPT_SHORT);
    if (casper_chronicle_verify("io.txt.chronicle.receipt") != 0) ++failures;
    if (remove("io.txt.chronicle.receipt") != 0 || remove("io.txt.chronicle") != 0 ||
        remove("io.txt") != 0 || chr_pool_used() != 0u) ++failures;
    printf("chronicle_io_cases=3 failures=%d arena_live_bytes=%zu\n", failures, chr_pool_used());
    return failures ? 1 : 0;
}
