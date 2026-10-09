#include "casper_chronicle.h"
#include "chronicle_pool.h"
#include "proof_generator.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Link wrapping measures direct application allocation calls, not libc's stdio. */
static size_t heap_calls;
void *__wrap_malloc(size_t size);
void *__wrap_calloc(size_t count, size_t size);
void *__wrap_realloc(void *ptr, size_t size);
void __wrap_free(void *ptr);
void *__wrap_malloc(size_t size) { (void)size; ++heap_calls; return NULL; }
void *__wrap_calloc(size_t count, size_t size) { (void)count; return __wrap_malloc(size); }
void *__wrap_realloc(void *ptr, size_t size) { (void)ptr; return __wrap_malloc(size); }
void __wrap_free(void *ptr) { if (ptr) ++heap_calls; }

#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "api failure line %d\n", __LINE__); return 1; } } while (0)
static int digest_matches(const uint8_t *data, size_t size, const char *expected) {
    uint8_t digest[32]; char hex[65];
    niyah_sha256(data, size, digest); niyah_hash_to_hex(digest, hex);
    return strcmp(hex, expected) == 0;
}
int main(void) {
    size_t checks = 0u, i;
    FILE *f; uint8_t *million; void *held;
    char *store = NULL, *receipt = NULL, *json = NULL, *proof = NULL;
    const char *question = "@claim\tAlice\tknows\tBob";
    CHECK(digest_matches((const uint8_t *)"", 0u,
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK(digest_matches((const uint8_t *)"abc", 3u,
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK(digest_matches((const uint8_t *)"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56u,
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    million = chr_malloc(1000000u); CHECK(million != NULL); memset(million, 'a', 1000000u);
    CHECK(digest_matches(million, 1000000u,
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
    chr_free(million);
    f = fopen("api.txt", "wbx"); CHECK(f != NULL);
    CHECK(fputs("Alice knows Bob.\n", f) >= 0); CHECK(fclose(f) == 0);
    CHECK(casper_chronicle_ingest("api.txt", &store, &receipt) == 0);
    CHECK(casper_chronicle_verify(receipt) == 0);
    for (i = 0u; i < 25u; ++i) {
        CHECK(casper_chronicle_query(store, question, &json, &proof) == 0);
        CHECK(strstr(json, "EXACT_STATED") != NULL);
        CHECK(casper_chronicle_verify(proof) == 0);
        casper_chronicle_free(json); casper_chronicle_free(proof);
        json = NULL; proof = NULL;
    }
    held = chr_malloc(CHRONICLE_POOL_SIZE - 1024u); CHECK(held != NULL);
    CHECK(casper_chronicle_query(store, question, &json, &proof) != 0);
    CHECK(json == NULL && proof == NULL); chr_free(held);
    CHECK(casper_chronicle_query(store, question, &json, &proof) == 0);
    CHECK(remove(proof) == 0); CHECK(remove(receipt) == 0); CHECK(remove(store) == 0);
    CHECK(remove("api.txt") == 0);
    casper_chronicle_free(json); casper_chronicle_free(proof);
    casper_chronicle_free(store); casper_chronicle_free(receipt);
    CHECK(chr_pool_used() == 0u); CHECK(heap_calls == 0u);
    printf("chronicle_api_checks=%zu failures=0 direct_heap_calls=%zu arena_live_bytes=%zu\n",
           checks, heap_calls, chr_pool_used());
    return 0;
}
