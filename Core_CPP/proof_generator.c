/*
 * proof_generator.c — NIYAH Proof Generation & Verification
 *
 * Public-domain SHA-256 + proof audit trail.
 * Based on the FIPS 180-4 specification.
 *
 * Zero external dependencies. C11 clean.
 *
 * Standalone test:
 *   gcc -O2 -std=c11 -Wall -Wextra -Werror -Wstrict-prototypes
 *       -Wcast-align -DPROOF_STANDALONE_TEST proof_generator.c -o test_proof
 *   ./test_proof
 */

#include "proof_generator.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * §0  SHA-256 (FIPS 180-4, public domain)
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

#define ROTR(x,n) (((x)>>(n))|((x)<<(32-(n))))
#define CH(x,y,z) (((x)&(y))^(~(x)&(z)))
#define MAJ(x,y,z) (((x)&(y))^((x)&(z))^((y)&(z)))
#define EP0(x) (ROTR(x,2)^ROTR(x,13)^ROTR(x,22))
#define EP1(x) (ROTR(x,6)^ROTR(x,11)^ROTR(x,25))
#define SIG0(x) (ROTR(x,7)^ROTR(x,18)^((x)>>3))
#define SIG1(x) (ROTR(x,17)^ROTR(x,19)^((x)>>10))

static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,
    0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,
    0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,
    0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,
    0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,
    0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

typedef struct {
    uint32_t state[8];
    uint64_t bitcount;
    uint8_t  buffer[64];
    uint32_t buflen;
} SHA256_CTX;

static void sha256_init(SHA256_CTX *ctx) {
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
    ctx->bitcount = 0;
    ctx->buflen = 0;
}

static void sha256_transform(SHA256_CTX *ctx, const uint8_t block[64]) {
    uint32_t W[64], a, b, c, d, e, f, g, h, t1, t2;

    for (int i = 0; i < 16; i++)
        W[i] = ((uint32_t)block[i*4] << 24) | ((uint32_t)block[i*4+1] << 16)
             | ((uint32_t)block[i*4+2] << 8) | block[i*4+3];
    for (int i = 16; i < 64; i++)
        W[i] = SIG1(W[i-2]) + W[i-7] + SIG0(W[i-15]) + W[i-16];

    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

    for (int i = 0; i < 64; i++) {
        t1 = h + EP1(e) + CH(e,f,g) + K[i] + W[i];
        t2 = EP0(a) + MAJ(a,b,c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b;
    ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f;
    ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_update(SHA256_CTX *ctx, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        ctx->buffer[ctx->buflen++] = data[i];
        if (ctx->buflen == 64) {
            sha256_transform(ctx, ctx->buffer);
            ctx->bitcount += 512;
            ctx->buflen = 0;
        }
    }
}

static void sha256_final(SHA256_CTX *ctx, uint8_t hash[32]) {
    ctx->bitcount += (uint64_t)ctx->buflen * 8;

    ctx->buffer[ctx->buflen++] = 0x80;
    if (ctx->buflen > 56) {
        while (ctx->buflen < 64) ctx->buffer[ctx->buflen++] = 0;
        sha256_transform(ctx, ctx->buffer);
        ctx->buflen = 0;
    }
    while (ctx->buflen < 56) ctx->buffer[ctx->buflen++] = 0;

    for (int i = 7; i >= 0; i--)
        ctx->buffer[ctx->buflen++] = (uint8_t)(ctx->bitcount >> (i * 8));

    sha256_transform(ctx, ctx->buffer);

    for (int i = 0; i < 8; i++) {
        hash[i*4]   = (uint8_t)(ctx->state[i] >> 24);
        hash[i*4+1] = (uint8_t)(ctx->state[i] >> 16);
        hash[i*4+2] = (uint8_t)(ctx->state[i] >> 8);
        hash[i*4+3] = (uint8_t)(ctx->state[i]);
    }
}

void niyah_sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
    SHA256_CTX ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, out);
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * §1  Utility
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

void niyah_hash_to_hex(const uint8_t hash[32], char hex[65]) {
    static const char hexc[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        hex[i*2]   = hexc[hash[i] >> 4];
        hex[i*2+1] = hexc[hash[i] & 0x0f];
    }
    hex[64] = '\0';
}

static bool hex_to_hash(const char *hex, uint8_t hash[32]) {
    for (int i = 0; i < 32; i++) {
        unsigned hi, lo;
        char ch = hex[i*2];
        if (ch >= '0' && ch <= '9') hi = (unsigned)(ch - '0');
        else if (ch >= 'a' && ch <= 'f') hi = (unsigned)(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') hi = (unsigned)(ch - 'A' + 10);
        else return false;

        char cl = hex[i*2+1];
        if (cl >= '0' && cl <= '9') lo = (unsigned)(cl - '0');
        else if (cl >= 'a' && cl <= 'f') lo = (unsigned)(cl - 'a' + 10);
        else if (cl >= 'A' && cl <= 'F') lo = (unsigned)(cl - 'A' + 10);
        else return false;

        hash[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * §2  Proof generation
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

void niyah_proof_generate(const char *prompt, const char *output,
                          const char *rule_file, uint8_t proof[32])
{
    SHA256_CTX ctx;
    sha256_init(&ctx);

    if (prompt)
        sha256_update(&ctx, (const uint8_t *)prompt, strlen(prompt));
    /* Separator byte to prevent concatenation ambiguity */
    uint8_t sep = 0x00;
    sha256_update(&ctx, &sep, 1);

    if (output)
        sha256_update(&ctx, (const uint8_t *)output, strlen(output));
    sha256_update(&ctx, &sep, 1);

    if (rule_file)
        sha256_update(&ctx, (const uint8_t *)rule_file, strlen(rule_file));

    sha256_final(&ctx, proof);
}

int niyah_proof_save(const char *path, const uint8_t proof[32],
                     const char *prompt, const char *output,
                     const char *rule_file)
{
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return -1; }

    char hex[65];

    fprintf(f, "NIYAH-PROOF-V1\n");

    niyah_hash_to_hex(proof, hex);
    fprintf(f, "hash: %s\n", hex);

    /* Hash individual components for auditability */
    uint8_t h[32];

    if (prompt) {
        niyah_sha256((const uint8_t *)prompt, strlen(prompt), h);
        niyah_hash_to_hex(h, hex);
    } else {
        memset(hex, '0', 64); hex[64] = '\0';
    }
    fprintf(f, "prompt_hash: %s\n", hex);

    if (output) {
        niyah_sha256((const uint8_t *)output, strlen(output), h);
        niyah_hash_to_hex(h, hex);
    } else {
        memset(hex, '0', 64); hex[64] = '\0';
    }
    fprintf(f, "output_hash: %s\n", hex);

    if (rule_file) {
        niyah_sha256((const uint8_t *)rule_file, strlen(rule_file), h);
        niyah_hash_to_hex(h, hex);
    } else {
        memset(hex, '0', 64); hex[64] = '\0';
    }
    fprintf(f, "rules_hash: %s\n", hex);

    fclose(f);
    return 0;
}

bool niyah_proof_verify(const char *proof_path,
                        const char *prompt,
                        const char *output,
                        const char *rule_file)
{
    FILE *f = fopen(proof_path, "r");
    if (!f) { perror(proof_path); return false; }

    /* Read the stored hash */
    char line[256];
    uint8_t stored_hash[32];
    bool found_hash = false;

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "hash: ", 6) == 0) {
            char *hex = line + 6;
            /* Trim newline */
            size_t len = strlen(hex);
            while (len > 0 && (hex[len-1] == '\n' || hex[len-1] == '\r'))
                hex[--len] = '\0';
            if (len == 64 && hex_to_hash(hex, stored_hash))
                found_hash = true;
            break;
        }
    }
    fclose(f);

    if (!found_hash) return false;

    /* Re-compute proof hash */
    uint8_t computed[32];
    niyah_proof_generate(prompt, output, rule_file, computed);

    return memcmp(stored_hash, computed, 32) == 0;
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * §3  Smoke test
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

#define PROOF_PASS(cond, label) do { \
    if (cond) { pass++; fprintf(stderr, "  [PASS] %s\n", label); } \
    else      { fail++; fprintf(stderr, "  [FAIL] %s\n", label); } \
} while(0)

int niyah_proof_smoke(void) {
    int pass = 0, fail = 0;

    fprintf(stderr, "\n+--------------------------------------+\n");
    fprintf(stderr, "|  NIYAH Proof Generator Smoke Test    |\n");
    fprintf(stderr, "+--------------------------------------+\n");

    /* §3.1 — SHA-256 of empty string */
    {
        uint8_t hash[32];
        niyah_sha256((const uint8_t *)"", 0, hash);
        char hex[65];
        niyah_hash_to_hex(hash, hex);
        /* Known: SHA-256("") = e3b0c44298fc1c149afbf4c8996fb924...  */
        PROOF_PASS(strncmp(hex, "e3b0c44298fc1c14", 16) == 0,
                   "SHA-256('') prefix matches NIST vector");
        fprintf(stderr, "  hash: %s\n", hex);
    }

    /* §3.2 — SHA-256 of "abc" */
    {
        uint8_t hash[32];
        niyah_sha256((const uint8_t *)"abc", 3, hash);
        char hex[65];
        niyah_hash_to_hex(hash, hex);
        /* Known: SHA-256("abc") = ba7816bf8f01cfea... */
        PROOF_PASS(strncmp(hex, "ba7816bf8f01cfea", 16) == 0,
                   "SHA-256('abc') prefix matches NIST vector");
    }

    /* §3.3 — SHA-256 deterministic */
    {
        uint8_t h1[32], h2[32];
        const char *msg = "niyah sovereign engine";
        niyah_sha256((const uint8_t *)msg, strlen(msg), h1);
        niyah_sha256((const uint8_t *)msg, strlen(msg), h2);
        PROOF_PASS(memcmp(h1, h2, 32) == 0, "SHA-256 is deterministic");
    }

    /* §3.4 — SHA-256 different inputs produce different hashes */
    {
        uint8_t h1[32], h2[32];
        niyah_sha256((const uint8_t *)"hello", 5, h1);
        niyah_sha256((const uint8_t *)"world", 5, h2);
        PROOF_PASS(memcmp(h1, h2, 32) != 0, "different inputs → different hashes");
    }

    /* §3.5 — Proof generate + verify */
    {
        uint8_t proof[32];
        niyah_proof_generate("what is 2+2", "4", "rule: \"ALWAYS be helpful\"", proof);
        char hex[65];
        niyah_hash_to_hex(proof, hex);
        PROOF_PASS(strlen(hex) == 64, "proof hash is 64 hex chars");
        fprintf(stderr, "  proof: %s\n", hex);
    }

    /* §3.6 — Proof save + verify round-trip */
    {
        const char *tmp = "/tmp/niyah_test.proof";
        const char *prompt = "hello world";
        const char *output = "I am NIYAH";
        const char *rules  = "rule: \"ALWAYS be safe\"";

        uint8_t proof[32];
        niyah_proof_generate(prompt, output, rules, proof);

        int rc = niyah_proof_save(tmp, proof, prompt, output, rules);
        PROOF_PASS(rc == 0, "proof save returns 0");

        bool ok = niyah_proof_verify(tmp, prompt, output, rules);
        PROOF_PASS(ok, "proof verify succeeds with correct data");

        /* Tampered output should fail */
        bool bad = niyah_proof_verify(tmp, prompt, "TAMPERED", rules);
        PROOF_PASS(!bad, "proof verify fails with tampered output");

        /* Tampered rules should fail */
        bad = niyah_proof_verify(tmp, prompt, output, "DIFFERENT RULES");
        PROOF_PASS(!bad, "proof verify fails with tampered rules");
    }

    /* §3.7 — Hex round-trip */
    {
        uint8_t h[32], h2[32];
        niyah_sha256((const uint8_t *)"test", 4, h);
        char hex[65];
        niyah_hash_to_hex(h, hex);
        bool ok = hex_to_hash(hex, h2);
        PROOF_PASS(ok && memcmp(h, h2, 32) == 0, "hex encode/decode round-trip");
    }

    /* §3.8 — Proof with NULL rule_file */
    {
        uint8_t proof[32];
        niyah_proof_generate("prompt", "output", NULL, proof);
        char hex[65];
        niyah_hash_to_hex(proof, hex);
        PROOF_PASS(strlen(hex) == 64, "proof with NULL rules produces valid hash");
    }

    fprintf(stderr, "\n  Results: %d passed, %d failed\n\n", pass, fail);
    return fail;
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * §4  Standalone test entry point
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

#ifdef PROOF_STANDALONE_TEST
int main(void) {
    int failed = niyah_proof_smoke();
    if (failed == 0)
        printf("PROOF SMOKE PASS - 0 failed\n");
    else
        printf("PROOF SMOKE FAIL - %d failed\n", failed);
    return failed;
}
#endif
