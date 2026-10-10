#define _GNU_SOURCE
#include "proof_generator.h"
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

static const char * const proof_path = "proof-stream-test.proof";
static unsigned char *proof_data;
static size_t proof_size;
static int fail_read;
static size_t malloc_seen;
static size_t malloc_fail_at;
static size_t malloc_fail_hits;

FILE *__real_fopen(const char *, const char *);
void *__real_malloc(size_t);
FILE *__wrap_fopen(const char *, const char *);
void *__wrap_malloc(size_t);

void *__wrap_malloc(size_t n) {
    ++malloc_seen;
    if (malloc_fail_at && malloc_seen == malloc_fail_at) {
        ++malloc_fail_hits;
        return NULL;
    }
    return __real_malloc(n);
}

typedef struct { size_t pos; } Cookie;
static ssize_t faulty_read(void *ctx, char *buf, size_t cap) {
    Cookie *c = (Cookie *)ctx;
    if (c->pos == proof_size) { errno = EIO; return -1; }
    size_t n = proof_size - c->pos;
    if (n > cap) n = cap;
    memcpy(buf, proof_data + c->pos, n);
    c->pos += n;
    return (ssize_t)n;
}
static int cookie_close(void *ctx) { free(ctx); return 0; }
FILE *__wrap_fopen(const char *path, const char *mode) {
    if (fail_read && strcmp(path, proof_path) == 0 && mode[0] == 'r') {
        cookie_io_functions_t io = {0};
        Cookie *c = (Cookie *)malloc(sizeof(*c));
        if (!c) return NULL;
        c->pos = 0u;
        io.read = faulty_read;
        io.close = cookie_close;
        FILE *f = fopencookie(c, "r", io);
        if (!f) free(c);
        return f;
    }
    return __real_fopen(path, mode);
}

static int load_proof(void) {
    FILE *f = __real_fopen(proof_path, "rb");
    if (!f) return 1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 1; }
    long n = ftell(f);
    if (n <= 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 1; }
    proof_size = (size_t)n;
    proof_data = (unsigned char *)malloc(proof_size);
    if (!proof_data) { fclose(f); return 1; }
    if (fread(proof_data, 1u, proof_size, f) != proof_size) { fclose(f); return 1; }
    return fclose(f) != 0;
}

int main(void) {
    uint8_t proof[32], digest[32];
    bool bound = false, verified = false;
    unsigned failures = 0;
    const char *prompt = "hello", *answer = "world", *rules = "rules";
    niyah_proof_generate(prompt, answer, rules, proof);
    if (niyah_proof_save(proof_path, proof, prompt, answer, rules) != 0) return 2;
    if (load_proof()) return 2;
    if (!niyah_proof_verify(proof_path, prompt, answer, rules) ||
        !niyah_proof_verify_saved(proof_path, NULL, &bound, &verified)) {
        fprintf(stderr, "BROKEN_VALID_FIXTURE\n");
        return 2;
    }
    /* The cookie returns every valid byte, then EIO instead of EOF. */
    fail_read = 1;
    if (niyah_proof_verify(proof_path, prompt, answer, rules)) {
        puts("FAIL_READ_ERROR_ACCEPTED"); ++failures;
    } else puts("PASS_READ_ERROR_REJECTED");
    if (niyah_sha256_file(proof_path, digest)) {
        puts("FAIL_HASH_READ_ERROR_ACCEPTED"); ++failures;
    } else puts("PASS_HASH_READ_ERROR_REJECTED");
    fail_read = 0;
    /* Known 7-line proof format: allocation #10 is the read after the final field. */
    malloc_seen = 0u;
    malloc_fail_at = 10u;
    if (niyah_proof_verify_saved(proof_path, NULL, &bound, &verified)) {
        puts("FAIL_ALLOC_ERROR_ACCEPTED"); ++failures;
    } else puts("PASS_ALLOC_ERROR_REJECTED");
    malloc_fail_at = 0u;
    if (malloc_fail_hits != 1u) {
        fprintf(stderr, "BROKEN_ALLOC_ORACLE: hits=%zu calls=%zu\n", malloc_fail_hits, malloc_seen);
        return 2;
    }
    free(proof_data);
    if (remove(proof_path) != 0) return 2;
    printf("proof_stream_regression_failures=%u\n", failures);
    return failures ? 1 : 0;
}
