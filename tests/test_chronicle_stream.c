#include "casper_chronicle.h"
#include <stdio.h>
#include <string.h>

#define MAX_READ_BYTES (64u * 1024u)
#define SOURCE_ROWS 40000u
static size_t oversized_reads;
size_t __real_fread(void *ptr, size_t size, size_t count, FILE *stream);
size_t __wrap_fread(void *ptr, size_t size, size_t count, FILE *stream);
size_t __wrap_fread(void *ptr, size_t size, size_t count, FILE *stream) {
    if (size && count > MAX_READ_BYTES / size) {
        ++oversized_reads;
        return 0u;
    }
    return __real_fread(ptr, size, count, stream);
}

int main(void) {
    FILE *stream = fopen("stream-source.txt", "wbx");
    char *store = NULL, *receipt = NULL;
    size_t i;
    int rc, ingest_rc;
    if (!stream) return 1;
    for (i = 0u; i < SOURCE_ROWS; ++i) {
        if (fputs("Uninterpreted original narrative bytes are retained.\n", stream) < 0) {
            fclose(stream);
            return 1;
        }
    }
    if (fclose(stream) != 0) return 1;
    rc = casper_chronicle_ingest("stream-source.txt", &store, &receipt);
    ingest_rc = rc;
    if (rc == 0) rc = casper_chronicle_verify(receipt);
    printf("stream_ingest_exit=%d stream_verify_exit=%d oversized_reads=%zu read_ceiling_bytes=%u\n",
           ingest_rc, rc, oversized_reads, MAX_READ_BYTES);
    casper_chronicle_free(store);
    casper_chronicle_free(receipt);
    return rc == 0 && oversized_reads == 0u ? 0 : 1;
}
