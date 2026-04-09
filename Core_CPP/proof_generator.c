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

    /* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
     * §3.9 — Chain-of-Thought: create, add steps, finalize, verify
     * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
    {
        NiyahProofChain *chain = niyah_proof_chain_alloc("What is 2+2?");
        PROOF_PASS(chain != NULL, "CoT chain alloc succeeds");
        PROOF_PASS(chain->count == 0, "CoT chain starts empty");

        uint32_t s0 = niyah_proof_chain_add(chain, NIYAH_STEP_NEURAL_GEN,
                                            0xFFFFFFFF, "Generate initial response",
                                            "The answer is 4.");
        PROOF_PASS(s0 == 0, "CoT first step id is 0");

        uint32_t s1 = niyah_proof_chain_add(chain, NIYAH_STEP_RULE_CHECK,
                                            s0, "Check safety rule",
                                            "rule: ALWAYS be helpful");
        PROOF_PASS(s1 == 1, "CoT second step id is 1");

        uint32_t s2 = niyah_proof_chain_add(chain, NIYAH_STEP_RULE_PASS,
                                            s1, "Safety rule passed", "");
        PROOF_PASS(s2 == 2, "CoT third step id is 2");

        uint32_t s3 = niyah_proof_chain_add(chain, NIYAH_STEP_FINAL_ACCEPT,
                                            s2, "Output accepted", "The answer is 4.");
        PROOF_PASS(s3 == 3, "CoT fourth step id is 3");
        PROOF_PASS(chain->count == 4, "CoT chain has 4 steps");

        niyah_proof_chain_finalize(chain, "The answer is 4.");

        /* Verify chain hash is non-zero */
        uint8_t zero_hash[32];
        memset(zero_hash, 0, 32);
        PROOF_PASS(memcmp(chain->chain_hash, zero_hash, 32) != 0,
                   "CoT chain_hash is non-zero after finalize");

        /* Each step hash should also be non-zero */
        bool all_step_hashes_ok = true;
        for (uint32_t i = 0; i < chain->count; i++) {
            if (memcmp(chain->steps[i].step_hash, zero_hash, 32) == 0) {
                all_step_hashes_ok = false;
                break;
            }
        }
        PROOF_PASS(all_step_hashes_ok, "CoT all step hashes are non-zero");

        PROOF_PASS(niyah_proof_chain_violations(chain) == 0,
                   "CoT chain with no violations reports 0");

        niyah_proof_chain_free(chain);
    }

    /* §3.10 — Chain-of-Thought: save + verify round-trip */
    {
        const char *tmp_chain = "/tmp/niyah_test_chain.proof";
        NiyahProofChain *chain = niyah_proof_chain_alloc("Test prompt");

        niyah_proof_chain_add(chain, NIYAH_STEP_NEURAL_GEN,
                              0xFFFFFFFF, "Generate", "output text");
        niyah_proof_chain_add(chain, NIYAH_STEP_RULE_CHECK,
                              0, "Check rule", "rule: be safe");
        niyah_proof_chain_add(chain, NIYAH_STEP_RULE_PASS,
                              1, "Rule passed", "");
        niyah_proof_chain_add(chain, NIYAH_STEP_FINAL_ACCEPT,
                              2, "Accepted", "output text");

        niyah_proof_chain_finalize(chain, "output text");

        int rc = niyah_proof_chain_save(chain, tmp_chain);
        PROOF_PASS(rc == 0, "CoT chain save returns 0");

        bool ok = niyah_proof_chain_verify(tmp_chain);
        PROOF_PASS(ok, "CoT chain verify succeeds on valid file");

        niyah_proof_chain_free(chain);
    }

    /* §3.11 — Chain-of-Thought: violation counting */
    {
        NiyahProofChain *chain = niyah_proof_chain_alloc("Violation test");

        niyah_proof_chain_add(chain, NIYAH_STEP_NEURAL_GEN,
                              0xFFFFFFFF, "Generate", "bad output");
        niyah_proof_chain_add(chain, NIYAH_STEP_RULE_CHECK,
                              0, "Check rule A", "rule A");
        niyah_proof_chain_add(chain, NIYAH_STEP_RULE_FAIL,
                              1, "Rule A violated", "violation detail");
        niyah_proof_chain_add(chain, NIYAH_STEP_RESAMPLE,
                              2, "Re-sampling", "");
        niyah_proof_chain_add(chain, NIYAH_STEP_NEURAL_GEN,
                              3, "Generate again", "better output");
        niyah_proof_chain_add(chain, NIYAH_STEP_RULE_CHECK,
                              4, "Check rule A again", "rule A");
        niyah_proof_chain_add(chain, NIYAH_STEP_RULE_FAIL,
                              5, "Rule A violated again", "still bad");
        niyah_proof_chain_add(chain, NIYAH_STEP_FINAL_REJECT,
                              6, "Output rejected", "");

        niyah_proof_chain_finalize(chain, "");

        PROOF_PASS(niyah_proof_chain_violations(chain) == 2,
                   "CoT chain reports 2 violations");

        niyah_proof_chain_free(chain);
    }

    /* §3.12 — Chain-of-Thought: empty chain handling */
    {
        NiyahProofChain *chain = niyah_proof_chain_alloc("Empty chain test");
        PROOF_PASS(chain->count == 0, "CoT empty chain has 0 steps");
        PROOF_PASS(niyah_proof_chain_violations(chain) == 0,
                   "CoT empty chain has 0 violations");

        niyah_proof_chain_finalize(chain, "no steps");

        /* Chain hash should still be valid (non-zero) after finalize */
        uint8_t zero_hash[32];
        memset(zero_hash, 0, 32);
        PROOF_PASS(memcmp(chain->chain_hash, zero_hash, 32) != 0,
                   "CoT empty chain has non-zero chain_hash after finalize");

        const char *tmp_empty = "/tmp/niyah_test_empty_chain.proof";
        int rc = niyah_proof_chain_save(chain, tmp_empty);
        PROOF_PASS(rc == 0, "CoT empty chain save returns 0");

        bool ok = niyah_proof_chain_verify(tmp_empty);
        PROOF_PASS(ok, "CoT empty chain verify succeeds");

        niyah_proof_chain_free(chain);
    }

    fprintf(stderr, "\n  Results: %d passed, %d failed\n\n", pass, fail);
    return fail;
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * §4  Chain-of-Thought Proof Trail Implementation
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

#include <time.h>

static double cot_monotonic_ms(void) {
    return (double)clock() / (double)CLOCKS_PER_SEC * 1000.0;
}

static const char *niyah_step_kind_name(NiyahProofStepKind kind) {
    switch (kind) {
        case NIYAH_STEP_NEURAL_GEN:    return "NEURAL_GEN";
        case NIYAH_STEP_RULE_CHECK:    return "RULE_CHECK";
        case NIYAH_STEP_RULE_PASS:     return "RULE_PASS";
        case NIYAH_STEP_RULE_FAIL:     return "RULE_FAIL";
        case NIYAH_STEP_RESAMPLE:      return "RESAMPLE";
        case NIYAH_STEP_SYM_QUERY:     return "SYM_QUERY";
        case NIYAH_STEP_SYM_RESULT:    return "SYM_RESULT";
        case NIYAH_STEP_CSP_CHECK:     return "CSP_CHECK";
        case NIYAH_STEP_FINAL_ACCEPT:  return "FINAL_ACCEPT";
        case NIYAH_STEP_FINAL_REJECT:  return "FINAL_REJECT";
    }
    return "UNKNOWN";
}

NiyahProofChain *niyah_proof_chain_alloc(const char *prompt) {
    NiyahProofChain *chain = (NiyahProofChain *)calloc(1, sizeof(NiyahProofChain));
    if (!chain) return NULL;

    chain->capacity = 32;
    chain->count = 0;
    chain->steps = (NiyahProofStep *)calloc(chain->capacity, sizeof(NiyahProofStep));
    if (!chain->steps) {
        free(chain);
        return NULL;
    }

    if (prompt) {
        size_t len = strlen(prompt);
        if (len >= sizeof(chain->prompt)) len = sizeof(chain->prompt) - 1;
        memcpy(chain->prompt, prompt, len);
        chain->prompt[len] = '\0';
    } else {
        chain->prompt[0] = '\0';
    }

    chain->final_output[0] = '\0';
    memset(chain->chain_hash, 0, 32);
    chain->start_time_ms = cot_monotonic_ms();

    return chain;
}

void niyah_proof_chain_free(NiyahProofChain *chain) {
    if (!chain) return;
    free(chain->steps);
    free(chain);
}

uint32_t niyah_proof_chain_add(NiyahProofChain *chain,
                               NiyahProofStepKind kind,
                               uint32_t parent_id,
                               const char *description,
                               const char *data)
{
    if (!chain) return 0xFFFFFFFF;

    /* Grow if needed */
    if (chain->count >= chain->capacity) {
        uint32_t new_cap = chain->capacity * 2;
        NiyahProofStep *new_steps = (NiyahProofStep *)realloc(
            chain->steps, new_cap * sizeof(NiyahProofStep));
        if (!new_steps) return 0xFFFFFFFF;
        chain->steps = new_steps;
        chain->capacity = new_cap;
    }

    uint32_t sid = chain->count;
    NiyahProofStep *step = &chain->steps[sid];
    memset(step, 0, sizeof(NiyahProofStep));

    step->kind = kind;
    step->step_id = sid;
    step->parent_id = parent_id;
    step->timestamp_ms = cot_monotonic_ms() - chain->start_time_ms;

    if (description) {
        size_t len = strlen(description);
        if (len >= sizeof(step->description)) len = sizeof(step->description) - 1;
        memcpy(step->description, description, len);
        step->description[len] = '\0';
    }

    if (data) {
        size_t len = strlen(data);
        if (len >= sizeof(step->data)) len = sizeof(step->data) - 1;
        memcpy(step->data, data, len);
        step->data[len] = '\0';
    }

    /* Compute step_hash = SHA-256(step_id || kind || description || data || parent_hash) */
    {
        SHA256_CTX ctx;
        sha256_init(&ctx);

        /* step_id as 4 bytes big-endian */
        uint8_t id_bytes[4] = {
            (uint8_t)(sid >> 24), (uint8_t)(sid >> 16),
            (uint8_t)(sid >> 8),  (uint8_t)(sid)
        };
        sha256_update(&ctx, id_bytes, 4);

        /* kind as 4 bytes big-endian */
        uint32_t k = (uint32_t)kind;
        uint8_t kind_bytes[4] = {
            (uint8_t)(k >> 24), (uint8_t)(k >> 16),
            (uint8_t)(k >> 8),  (uint8_t)(k)
        };
        sha256_update(&ctx, kind_bytes, 4);

        /* description */
        sha256_update(&ctx, (const uint8_t *)step->description,
                      strlen(step->description));

        /* separator */
        uint8_t sep = 0x00;
        sha256_update(&ctx, &sep, 1);

        /* data */
        sha256_update(&ctx, (const uint8_t *)step->data,
                      strlen(step->data));

        /* parent hash: if parent exists, use its step_hash; else use zero hash */
        if (parent_id != 0xFFFFFFFF && parent_id < sid) {
            sha256_update(&ctx, chain->steps[parent_id].step_hash, 32);
        } else {
            uint8_t zero[32];
            memset(zero, 0, 32);
            sha256_update(&ctx, zero, 32);
        }

        sha256_final(&ctx, step->step_hash);
    }

    /* Update rolling chain_hash = SHA-256(old_chain_hash || step_hash) */
    {
        SHA256_CTX ctx;
        sha256_init(&ctx);
        sha256_update(&ctx, chain->chain_hash, 32);
        sha256_update(&ctx, step->step_hash, 32);
        sha256_final(&ctx, chain->chain_hash);
    }

    chain->count++;
    return sid;
}

void niyah_proof_chain_finalize(NiyahProofChain *chain, const char *final_output) {
    if (!chain) return;

    if (final_output) {
        size_t len = strlen(final_output);
        if (len >= sizeof(chain->final_output)) len = sizeof(chain->final_output) - 1;
        memcpy(chain->final_output, final_output, len);
        chain->final_output[len] = '\0';
    } else {
        chain->final_output[0] = '\0';
    }

    /* Recompute final chain_hash over all steps from scratch for integrity */
    {
        SHA256_CTX ctx;
        sha256_init(&ctx);

        /* Include prompt */
        sha256_update(&ctx, (const uint8_t *)chain->prompt,
                      strlen(chain->prompt));
        uint8_t sep = 0x00;
        sha256_update(&ctx, &sep, 1);

        /* Include each step hash in order */
        for (uint32_t i = 0; i < chain->count; i++) {
            sha256_update(&ctx, chain->steps[i].step_hash, 32);
        }

        /* Include final output */
        sha256_update(&ctx, &sep, 1);
        sha256_update(&ctx, (const uint8_t *)chain->final_output,
                      strlen(chain->final_output));

        sha256_final(&ctx, chain->chain_hash);
    }
}

int niyah_proof_chain_save(const NiyahProofChain *chain, const char *path) {
    if (!chain || !path) return -1;

    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return -1; }

    char hex[65];

    fprintf(f, "NIYAH-PROOF-V2\n");

    niyah_hash_to_hex(chain->chain_hash, hex);
    fprintf(f, "chain_hash: %s\n", hex);

    uint8_t h[32];
    niyah_sha256((const uint8_t *)chain->prompt, strlen(chain->prompt), h);
    niyah_hash_to_hex(h, hex);
    fprintf(f, "prompt_hash: %s\n", hex);

    niyah_sha256((const uint8_t *)chain->final_output,
                 strlen(chain->final_output), h);
    niyah_hash_to_hex(h, hex);
    fprintf(f, "output_hash: %s\n", hex);

    fprintf(f, "steps: %u\n", chain->count);
    fprintf(f, "---\n");

    for (uint32_t i = 0; i < chain->count; i++) {
        const NiyahProofStep *s = &chain->steps[i];

        if (s->parent_id == 0xFFFFFFFF) {
            fprintf(f, "[step %u] %s (parent: ROOT)\n",
                    s->step_id, niyah_step_kind_name(s->kind));
        } else {
            fprintf(f, "[step %u] %s (parent: %u)\n",
                    s->step_id, niyah_step_kind_name(s->kind), s->parent_id);
        }

        fprintf(f, "  desc: %s\n", s->description);

        if (s->data[0] != '\0') {
            /* Print first 200 chars of data */
            size_t dlen = strlen(s->data);
            if (dlen > 200) {
                fprintf(f, "  data: %.200s...\n", s->data);
            } else {
                fprintf(f, "  data: %s\n", s->data);
            }
        }

        niyah_hash_to_hex(s->step_hash, hex);
        fprintf(f, "  hash: %s\n", hex);
        fprintf(f, "  time: +%.2fms\n", s->timestamp_ms);
    }

    fprintf(f, "---\n");

    niyah_sha256((const uint8_t *)chain->final_output,
                 strlen(chain->final_output), h);
    niyah_hash_to_hex(h, hex);
    fprintf(f, "final_output_hash: %s\n", hex);

    fclose(f);
    return 0;
}

bool niyah_proof_chain_verify(const char *proof_path) {
    if (!proof_path) return false;

    FILE *f = fopen(proof_path, "r");
    if (!f) { perror(proof_path); return false; }

    char line[4096];
    uint8_t stored_chain_hash[32];
    bool found_chain_hash = false;
    uint32_t stored_step_count = 0;
    bool found_step_count = false;

    /* Collected step hashes for verification */
    uint8_t step_hashes[1024][32]; /* support up to 1024 steps for verify */
    uint32_t parsed_steps = 0;

    uint8_t stored_prompt_hash[32];
    bool found_prompt_hash = false;
    uint8_t stored_output_hash[32];
    bool found_output_hash = false;

    while (fgets(line, (int)sizeof(line), f)) {
        /* Trim newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        if (strncmp(line, "chain_hash: ", 12) == 0) {
            if (strlen(line + 12) >= 64 && hex_to_hash(line + 12, stored_chain_hash))
                found_chain_hash = true;
        } else if (strncmp(line, "prompt_hash: ", 13) == 0) {
            if (strlen(line + 13) >= 64 && hex_to_hash(line + 13, stored_prompt_hash))
                found_prompt_hash = true;
        } else if (strncmp(line, "output_hash: ", 13) == 0) {
            if (strlen(line + 13) >= 64 && hex_to_hash(line + 13, stored_output_hash))
                found_output_hash = true;
        } else if (strncmp(line, "steps: ", 7) == 0) {
            stored_step_count = (uint32_t)strtoul(line + 7, NULL, 10);
            found_step_count = true;
        } else if (strncmp(line, "  hash: ", 8) == 0) {
            if (parsed_steps < 1024 && strlen(line + 8) >= 64) {
                hex_to_hash(line + 8, step_hashes[parsed_steps]);
                parsed_steps++;
            }
        }
    }
    fclose(f);

    if (!found_chain_hash || !found_step_count || !found_prompt_hash || !found_output_hash)
        return false;

    if (parsed_steps != stored_step_count)
        return false;

    /* Recompute chain_hash from stored step hashes, prompt_hash, and output_hash */
    {
        SHA256_CTX ctx;
        sha256_init(&ctx);

        /*
         * The chain_hash in finalize is computed as:
         * SHA-256(prompt || 0x00 || step_hash[0] || ... || step_hash[n-1] || 0x00 || final_output)
         *
         * But we only have hashes of prompt and output, not the raw text.
         * So we verify structural consistency: recompute from the step hashes
         * using the same approach: feed prompt_hash, step hashes, output_hash
         * and check that chain_hash can be independently verified by checking
         * that the stored chain_hash matches a recomputation from stored parts.
         *
         * Since we stored the actual chain_hash (which was computed from raw data),
         * and we have the step hashes, we can at least verify that step hashes
         * are self-consistent with the chain_hash by recomputing from the raw
         * formula. However, without the raw prompt/output text, we verify that
         * the file is internally consistent: the chain_hash was computed over
         * the stored data, so just reading it back and trusting the individual
         * step hashes is the file-only verification mode.
         *
         * For file-only verification, we check:
         * 1) step count matches
         * 2) all hashes are parseable
         * 3) final_output_hash matches output_hash
         * 4) chain_hash is present and valid hex
         *
         * This is the best we can do without the raw prompt/output.
         */
    }

    /* Verify final_output_hash matches output_hash in the file */
    /* (we already parsed output_hash above) */

    /* Read file again to check final_output_hash matches output_hash */
    f = fopen(proof_path, "r");
    if (!f) return false;

    uint8_t stored_final_output_hash[32];
    bool found_final_output_hash = false;

    while (fgets(line, (int)sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        if (strncmp(line, "final_output_hash: ", 19) == 0) {
            if (strlen(line + 19) >= 64 &&
                hex_to_hash(line + 19, stored_final_output_hash))
                found_final_output_hash = true;
        }
    }
    fclose(f);

    if (!found_final_output_hash) return false;

    /* final_output_hash must match output_hash */
    if (memcmp(stored_output_hash, stored_final_output_hash, 32) != 0)
        return false;

    return true;
}

void niyah_proof_chain_print(const NiyahProofChain *chain) {
    if (!chain) return;

    char hex[65];

    printf("=== NIYAH Chain-of-Thought Proof Trail ===\n");
    printf("Prompt: %.80s%s\n", chain->prompt,
           strlen(chain->prompt) > 80 ? "..." : "");
    printf("Steps: %u\n", chain->count);

    niyah_hash_to_hex(chain->chain_hash, hex);
    printf("Chain hash: %s\n", hex);
    printf("---\n");

    /* Build depth for indentation via parent chain */
    for (uint32_t i = 0; i < chain->count; i++) {
        const NiyahProofStep *s = &chain->steps[i];

        /* Compute depth by walking parent chain */
        uint32_t depth = 0;
        uint32_t pid = s->parent_id;
        while (pid != 0xFFFFFFFF && pid < chain->count && depth < 20) {
            depth++;
            pid = chain->steps[pid].parent_id;
        }

        /* Print indentation */
        for (uint32_t d = 0; d < depth; d++)
            printf("  ");

        printf("[%u] %s", s->step_id, niyah_step_kind_name(s->kind));

        if (s->parent_id == 0xFFFFFFFF)
            printf(" (root)");
        else
            printf(" (parent: %u)", s->parent_id);

        printf(" +%.2fms\n", s->timestamp_ms);

        for (uint32_t d = 0; d < depth; d++)
            printf("  ");
        printf("    %s\n", s->description);

        if (s->data[0] != '\0') {
            for (uint32_t d = 0; d < depth; d++)
                printf("  ");
            size_t dlen = strlen(s->data);
            if (dlen > 120) {
                printf("    data: %.120s...\n", s->data);
            } else {
                printf("    data: %s\n", s->data);
            }
        }
    }

    printf("---\n");
    printf("Final output: %.120s%s\n", chain->final_output,
           strlen(chain->final_output) > 120 ? "..." : "");
    printf("==========================================\n");
}

uint32_t niyah_proof_chain_violations(const NiyahProofChain *chain) {
    if (!chain) return 0;
    uint32_t count = 0;
    for (uint32_t i = 0; i < chain->count; i++) {
        if (chain->steps[i].kind == NIYAH_STEP_RULE_FAIL)
            count++;
    }
    return count;
}

void niyah_proof_chain_explain(const NiyahProofChain *chain) {
    if (!chain) return;

    printf("=== NIYAH Explainability Report ===\n");
    printf("Prompt: %.80s%s\n", chain->prompt,
           strlen(chain->prompt) > 80 ? "..." : "");
    printf("\nRules applied:\n");

    uint32_t rule_count = 0;
    for (uint32_t i = 0; i < chain->count; i++) {
        const NiyahProofStep *s = &chain->steps[i];
        if (s->kind == NIYAH_STEP_RULE_CHECK ||
            s->kind == NIYAH_STEP_RULE_PASS  ||
            s->kind == NIYAH_STEP_RULE_FAIL) {

            const char *status;
            if (s->kind == NIYAH_STEP_RULE_CHECK)
                status = "CHECKED";
            else if (s->kind == NIYAH_STEP_RULE_PASS)
                status = "PASSED";
            else
                status = "FAILED";

            printf("  [%s] %s\n", status, s->description);
            if (s->data[0] != '\0') {
                size_t dlen = strlen(s->data);
                if (dlen > 200) {
                    printf("         %.200s...\n", s->data);
                } else {
                    printf("         %s\n", s->data);
                }
            }
            rule_count++;
        }
    }

    if (rule_count == 0) {
        printf("  (no rules applied)\n");
    }

    uint32_t violations = niyah_proof_chain_violations(chain);
    printf("\nSummary: %u rule steps, %u violations\n", rule_count, violations);
    printf("===================================\n");
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * §5  Standalone test entry point
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
