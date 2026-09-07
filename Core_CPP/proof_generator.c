/* proof_generator.c — SHA-256 integrity records. C11. */
#include "proof_generator.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROTR(x,n) (((x)>>(n))|((x)<<(32-(n))))
#define CH(x,y,z) (((x)&(y))^(~(x)&(z)))
#define MAJ(x,y,z) (((x)&(y))^((x)&(z))^((y)&(z)))
#define EP0(x) (ROTR(x,2)^ROTR(x,13)^ROTR(x,22))
#define EP1(x) (ROTR(x,6)^ROTR(x,11)^ROTR(x,25))
#define SIG0(x) (ROTR(x,7)^ROTR(x,18)^((x)>>3))
#define SIG1(x) (ROTR(x,17)^ROTR(x,19)^((x)>>10))

static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

typedef struct {
    uint32_t state[8];
    uint64_t bitcount;
    uint8_t buffer[64];
    uint32_t buflen;
} SHA256_CTX;

static void sha256_init(SHA256_CTX *c) {
    c->state[0]=0x6a09e667;c->state[1]=0xbb67ae85;c->state[2]=0x3c6ef372;c->state[3]=0xa54ff53a;
    c->state[4]=0x510e527f;c->state[5]=0x9b05688c;c->state[6]=0x1f83d9ab;c->state[7]=0x5be0cd19;
    c->bitcount=0;c->buflen=0;
}

static void sha256_transform(SHA256_CTX *c, const uint8_t b[64]) {
    uint32_t W[64],a,bv,d,e,f,g,h,t1,t2,cc;
    int i;
    for(i=0;i<16;i++) W[i]=((uint32_t)b[i*4]<<24)|((uint32_t)b[i*4+1]<<16)|((uint32_t)b[i*4+2]<<8)|b[i*4+3];
    for(i=16;i<64;i++) W[i]=SIG1(W[i-2])+W[i-7]+SIG0(W[i-15])+W[i-16];
    a=c->state[0];bv=c->state[1];cc=c->state[2];d=c->state[3];e=c->state[4];f=c->state[5];g=c->state[6];h=c->state[7];
    for(i=0;i<64;i++){t1=h+EP1(e)+CH(e,f,g)+K[i]+W[i];t2=EP0(a)+MAJ(a,bv,cc);h=g;g=f;f=e;e=d+t1;d=cc;cc=bv;bv=a;a=t1+t2;}
    c->state[0]+=a;c->state[1]+=bv;c->state[2]+=cc;c->state[3]+=d;c->state[4]+=e;c->state[5]+=f;c->state[6]+=g;c->state[7]+=h;
}

static void sha256_update(SHA256_CTX *c, const uint8_t *data, size_t len) {
    if (!data && len) return;
    while (len) {
        size_t n = 64u - c->buflen;
        if (n > len) n = len;
        memcpy(c->buffer + c->buflen, data, n);
        c->buflen += (uint32_t)n;
        data += n;
        len -= n;
        if (c->buflen == 64u) {
            sha256_transform(c, c->buffer);
            c->bitcount += 512u;
            c->buflen = 0u;
        }
    }
}

static void sha256_final(SHA256_CTX *c, uint8_t out[32]) {
    int i;
    c->bitcount += (uint64_t)c->buflen * 8u;
    c->buffer[c->buflen++] = 0x80u;
    if (c->buflen > 56u) {
        while (c->buflen < 64u) c->buffer[c->buflen++] = 0u;
        sha256_transform(c, c->buffer);
        c->buflen = 0u;
    }
    while (c->buflen < 56u) c->buffer[c->buflen++] = 0u;
    for (i=7;i>=0;i--) c->buffer[c->buflen++] = (uint8_t)(c->bitcount >> (i*8));
    sha256_transform(c, c->buffer);
    for(i=0;i<8;i++) {
        out[i*4]=(uint8_t)(c->state[i]>>24);
        out[i*4+1]=(uint8_t)(c->state[i]>>16);
        out[i*4+2]=(uint8_t)(c->state[i]>>8);
        out[i*4+3]=(uint8_t)c->state[i];
    }
}

void niyah_sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
    SHA256_CTX c;
    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, out);
}

void niyah_hash_to_hex(const uint8_t h[32], char hex[65]) {
    static const char x[]="0123456789abcdef";
    int i;
    for(i=0;i<32;i++){hex[i*2]=x[h[i]>>4];hex[i*2+1]=x[h[i]&15u];}
    hex[64]='\0';
}

static int hex_nibble(char c) {
    if (c>='0'&&c<='9') return c-'0';
    if (c>='a'&&c<='f') return c-'a'+10;
    if (c>='A'&&c<='F') return c-'A'+10;
    return -1;
}

static bool hex_to_hash(const char *s, uint8_t h[32]) {
    int i;
    if(!s || strlen(s)!=64u) return false;
    for(i=0;i<32;i++) {
        int hi=hex_nibble(s[i*2]);
        int lo=hex_nibble(s[i*2+1]);
        if(hi<0||lo<0) return false;
        h[i]=(uint8_t)((hi<<4)|lo);
    }
    return true;
}

static int sha256_update_file(SHA256_CTX *ctx, const char *path) {
    FILE *f;
    uint8_t buf[4096];
    size_t n;
    if (!path) return 0;
    f = fopen(path, "rb");
    if (!f) return -1;
    while ((n = fread(buf, 1u, sizeof(buf), f)) > 0u) sha256_update(ctx, buf, n);
    if (ferror(f)) { fclose(f); return -1; }
    return fclose(f) == 0 ? 0 : -1;
}

static int sha256_file(const char *path, uint8_t out[32]) {
    SHA256_CTX c;
    sha256_init(&c);
    if (sha256_update_file(&c, path) != 0) return -1;
    sha256_final(&c, out);
    return 0;
}

int niyah_proof_generate(const char *prompt, const char *output,
                         const char *rule_file, uint8_t proof[32]) {
    static const uint8_t domain[] = "NIYAH-INTEGRITY-V2";
    const uint8_t sep = 0u;
    SHA256_CTX c;
    if (!proof) return -1;
    memset(proof, 0, 32u);
    sha256_init(&c);
    sha256_update(&c, domain, sizeof(domain)-1u);
    sha256_update(&c, &sep, 1u);
    if (prompt) sha256_update(&c, (const uint8_t *)prompt, strlen(prompt));
    sha256_update(&c, &sep, 1u);
    if (output) sha256_update(&c, (const uint8_t *)output, strlen(output));
    sha256_update(&c, &sep, 1u);
    if (rule_file && sha256_update_file(&c, rule_file) != 0) return -1;
    sha256_final(&c, proof);
    return 0;
}

static int write_hex_text(FILE *f, const char *key, const char *text) {
    const unsigned char *p;
    if (fprintf(f, "%s", key) < 0) return -1;
    if (text) {
        for (p=(const unsigned char *)text; *p; ++p) {
            if (fprintf(f, "%02x", (unsigned)*p) < 0) return -1;
        }
    }
    return fputc('\n', f) == EOF ? -1 : 0;
}

int niyah_proof_save(const char *path, const uint8_t proof[32],
                     const char *prompt, const char *output,
                     const char *rule_file) {
    FILE *f;
    uint8_t expected[32], h[32];
    char hex[65];
    int rc = 0;
    if (!path || !proof) return -1;
    if (niyah_proof_generate(prompt, output, rule_file, expected) != 0) return -1;
    if (memcmp(expected, proof, 32u) != 0) return -1;

    f = fopen(path, "w");
    if (!f) return -1;
    if (fprintf(f, "NIYAH-INTEGRITY-V2\n") < 0) rc = -1;

    niyah_hash_to_hex(proof, hex);
    if (!rc && fprintf(f, "hash: %s\n", hex) < 0) rc = -1;

    niyah_sha256((const uint8_t *)(prompt ? prompt : ""), prompt ? strlen(prompt) : 0u, h);
    niyah_hash_to_hex(h, hex);
    if (!rc && fprintf(f, "prompt_hash: %s\n", hex) < 0) rc = -1;

    niyah_sha256((const uint8_t *)(output ? output : ""), output ? strlen(output) : 0u, h);
    niyah_hash_to_hex(h, hex);
    if (!rc && fprintf(f, "output_hash: %s\n", hex) < 0) rc = -1;

    if (rule_file) {
        if (sha256_file(rule_file, h) != 0) rc = -1;
    } else {
        niyah_sha256((const uint8_t *)"", 0u, h);
    }
    niyah_hash_to_hex(h, hex);
    if (!rc && fprintf(f, "rules_hash: %s\n", hex) < 0) rc = -1;
    if (!rc && write_hex_text(f, "prompt_hex: ", prompt ? prompt : "") != 0) rc = -1;
    if (!rc && write_hex_text(f, "output_hex: ", output ? output : "") != 0) rc = -1;
    if (!rc && write_hex_text(f, "rules_path_hex: ", rule_file ? rule_file : "") != 0) rc = -1;
    if (ferror(f)) rc = -1;
    if (fclose(f) != 0) rc = -1;
    return rc;
}

static char *read_line_alloc(FILE *f) {
    size_t cap = 256u, len = 0u;
    char *s = (char *)malloc(cap);
    int ch;
    if (!s) return NULL;
    while ((ch = fgetc(f)) != EOF) {
        if (ch == '\n') break;
        if (len + 1u >= cap) {
            size_t next = cap > SIZE_MAX / 2u ? 0u : cap * 2u;
            char *p;
            if (!next) { free(s); return NULL; }
            p = (char *)realloc(s, next);
            if (!p) { free(s); return NULL; }
            s = p;
            cap = next;
        }
        s[len++] = (char)ch;
    }
    if (ch == EOF && len == 0u) { free(s); return NULL; }
    if (len && s[len-1u] == '\r') --len;
    s[len] = '\0';
    return s;
}

static char *decode_hex_text(const char *line, const char *prefix) {
    size_t prefix_len = strlen(prefix);
    size_t hex_len;
    char *out;
    size_t i;
    if (strncmp(line, prefix, prefix_len) != 0) return NULL;
    line += prefix_len;
    hex_len = strlen(line);
    if ((hex_len & 1u) != 0u) return NULL;
    out = (char *)malloc(hex_len / 2u + 1u);
    if (!out) return NULL;
    for (i=0u;i<hex_len;i+=2u) {
        int hi=hex_nibble(line[i]);
        int lo=hex_nibble(line[i+1u]);
        if (hi<0 || lo<0) { free(out); return NULL; }
        out[i/2u]=(char)((hi<<4)|lo);
        if (out[i/2u] == '\0') { free(out); return NULL; }
    }
    out[hex_len/2u]='\0';
    return out;
}

static bool read_stored_hash(const char *proof_path, uint8_t expected[32]) {
    FILE *f;
    char *line;
    bool ok = false;
    f = fopen(proof_path, "r");
    if (!f) return false;
    line = read_line_alloc(f);
    if (!line || strcmp(line, "NIYAH-INTEGRITY-V2") != 0) { free(line); fclose(f); return false; }
    free(line);
    while ((line = read_line_alloc(f)) != NULL) {
        if (strncmp(line, "hash: ", 6u) == 0) {
            ok = hex_to_hash(line + 6u, expected);
            free(line);
            break;
        }
        free(line);
    }
    fclose(f);
    return ok;
}

bool niyah_proof_verify(const char *proof_path, const char *prompt,
                        const char *output, const char *rule_file) {
    uint8_t expected[32], actual[32];
    if (!proof_path) return false;
    if (!read_stored_hash(proof_path, expected)) return false;
    if (niyah_proof_generate(prompt, output, rule_file, actual) != 0) return false;
    return memcmp(expected, actual, 32u) == 0;
}

bool niyah_proof_verify_stored(const char *proof_path) {
    FILE *f;
    char *line;
    char *prompt = NULL, *output = NULL, *rules = NULL;
    bool have_prompt = false, have_output = false, have_rules = false;
    bool ok = false;
    if (!proof_path) return false;
    f = fopen(proof_path, "r");
    if (!f) return false;
    line = read_line_alloc(f);
    if (!line || strcmp(line, "NIYAH-INTEGRITY-V2") != 0) { free(line); fclose(f); return false; }
    free(line);
    while ((line = read_line_alloc(f)) != NULL) {
        char *decoded = NULL;
        if (!have_prompt && strncmp(line, "prompt_hex: ", 12u) == 0) {
            decoded = decode_hex_text(line, "prompt_hex: ");
            if (!decoded) { free(line); goto done; }
            prompt = decoded; have_prompt = true;
        } else if (!have_output && strncmp(line, "output_hex: ", 12u) == 0) {
            decoded = decode_hex_text(line, "output_hex: ");
            if (!decoded) { free(line); goto done; }
            output = decoded; have_output = true;
        } else if (!have_rules && strncmp(line, "rules_path_hex: ", 16u) == 0) {
            decoded = decode_hex_text(line, "rules_path_hex: ");
            if (!decoded) { free(line); goto done; }
            rules = decoded; have_rules = true;
        }
        free(line);
    }
    if (have_prompt && have_output && have_rules)
        ok = niyah_proof_verify(proof_path, prompt, output, rules[0] ? rules : NULL);
done:
    fclose(f);
    free(prompt);
    free(output);
    free(rules);
    return ok;
}

int niyah_proof_smoke(void) {
    int fail = 0;
    uint8_t h[32];
    char hex[65];
    const char *proof_path = "niyah_test.proof";
    const char *rules_path = "niyah_test.rules";
    FILE *rf;

    niyah_sha256((const uint8_t *)"", 0u, h);
    niyah_hash_to_hex(h, hex);
    if (strncmp(hex, "e3b0c44298fc1c14", 16u) != 0) ++fail;
    niyah_sha256((const uint8_t *)"abc", 3u, h);
    niyah_hash_to_hex(h, hex);
    if (strncmp(hex, "ba7816bf8f01cfea", 16u) != 0) ++fail;

    rf = fopen(rules_path, "w");
    if (!rf) return fail + 1;
    if (fputs("rule: test\n", rf) == EOF || fclose(rf) != 0) { remove(rules_path); return fail + 1; }

    if (niyah_proof_generate("hello", "world\nline2", rules_path, h) != 0) ++fail;
    else if (niyah_proof_save(proof_path, h, "hello", "world\nline2", rules_path) != 0) ++fail;
    else {
        if (!niyah_proof_verify(proof_path, "hello", "world\nline2", rules_path)) ++fail;
        if (!niyah_proof_verify_stored(proof_path)) ++fail;
        if (niyah_proof_verify(proof_path, "hello", "tampered", rules_path)) ++fail;
        rf = fopen(rules_path, "w");
        if (!rf) ++fail;
        else {
            if (fputs("rule: changed\n", rf) == EOF || fclose(rf) != 0) ++fail;
            else if (niyah_proof_verify_stored(proof_path)) ++fail;
        }
    }
    remove(proof_path);
    remove(rules_path);
    return fail;
}
