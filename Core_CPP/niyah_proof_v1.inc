/* NIYAH-PROOF-V1: SHA-256(prompt bytes || output bytes || rule-file bytes).
 * V1 is intentionally NOT compatible with the legacy domain-separated V2.
 * The V1 .proof format has exactly eight LF-delimited ASCII lines.
 * Rule bytes are verified against the original rule file, not a stored digest
 * substituted for the file. No-rule receipts bind zero rule bytes.
 */
static int proof_v1_compute(const char *prompt, const char *output,
                            const char *rules_path, uint8_t digest[32],
                            uint8_t rule_digest[32], uint64_t *rule_bytes) {
    SHA256_CTX hash, rules;
    uint8_t buf[8192];
    uint64_t total = 0u;
    FILE *f = NULL;
    if (!prompt || !output || !digest || !rule_digest || !rule_bytes) return -1;
    sha256_init(&hash);
    sha256_init(&rules);
    sha256_update(&hash, (const uint8_t *)prompt, strlen(prompt));
    sha256_update(&hash, (const uint8_t *)output, strlen(output));
    if (rules_path && rules_path[0]) {
        f = niyah_fopen_utf8(rules_path, "rb");
        if (!f) return -1;
        for (;;) {
            size_t n = fread(buf, 1u, sizeof(buf), f);
            if (n) {
                if ((uint64_t)n > UINT64_MAX - total) { (void)fclose(f); return -1; }
                total += (uint64_t)n;
                sha256_update(&hash, buf, n);
                sha256_update(&rules, buf, n);
            }
            if (ferror(f)) { (void)fclose(f); return -1; }
            if (n < sizeof(buf)) {
                if (!feof(f)) { (void)fclose(f); return -1; }
                break;
            }
        }
        if (fclose(f) != 0) return -1;
    }
    sha256_final(&rules, rule_digest);
    sha256_final(&hash, digest);
    *rule_bytes = total;
    return 0;
}

int niyah_proof_generate_v1(const char *prompt, const char *output,
                            const char *rules_path, uint8_t proof[32]) {
    uint8_t rules[32];
    uint64_t count;
    return proof_v1_compute(prompt, output, rules_path, proof, rules, &count);
}

/* Canonical parser: accepts ASCII printable fields and LF line endings only.
 * A trailing unterminated line, CRLF, embedded NUL, and control bytes fail.
 */
static char *proof_v1_line(FILE *f, size_t max_len) {
    size_t cap = 128u, n = 0u;
    int ch;
    char *s;
    if (!f || !max_len) return NULL;
    s = (char *)malloc(cap);
    if (!s) return NULL;
    while ((ch = fgetc(f)) != EOF) {
        if (ch == '\n') { s[n] = '\0'; return s; }
        if (ch < 32 || ch > 126 || n >= max_len) break;
        if (n + 1u >= cap) {
            size_t next = cap * 2u;
            char *grown;
            if (next > max_len + 1u) next = max_len + 1u;
            if (next <= cap) break;
            grown = (char *)realloc(s, next);
            if (!grown) break;
            s = grown; cap = next;
        }
        s[n++] = (char)ch;
    }
    free(s);
    return NULL;
}
static char *proof_v1_field(FILE *f, const char *key, size_t max_len) {
    char *line = proof_v1_line(f, max_len);
    size_t n = strlen(key);
    char *result;
    if (!line) return NULL;
    if (strncmp(line, key, n) != 0) { free(line); return NULL; }
    result = dup_text(line + n);
    free(line);
    return result;
}
static int proof_v1_uint64(const char *s, uint64_t *out) {
    uint64_t n = 0u;
    const char *p = s;
    if (!p || !*p || (p[0] == '0' && p[1])) return 0;
    for (; *p; ++p) {
        uint32_t digit;
        if (*p < '0' || *p > '9') return 0;
        digit = (uint32_t)(*p - '0');
        if (n > (UINT64_MAX - digit) / 10u) return 0;
        n = n * 10u + digit;
    }
    *out = n;
    return 1;
}

int niyah_proof_save_v1(const char *path, const uint8_t proof[32],
                        const char *prompt, const char *output,
                        const char *rules_path) {
    uint8_t computed[32], rules[32], ph[32], oh[32];
    uint64_t rule_bytes;
    char hex[65];
    FILE *f;
    int ok = 1;
    if (!path || !proof || !prompt || !output ||
        strlen(prompt) > NIYAH_PROOF_MAX_HEX_BYTES / 2u ||
        strlen(output) > NIYAH_PROOF_MAX_HEX_BYTES / 2u ||
        proof_v1_compute(prompt, output, rules_path, computed, rules, &rule_bytes) != 0 ||
        memcmp(proof, computed, 32u) != 0) return -1;
    f = niyah_fopen_utf8(path, "wb");
    if (!f) return -1;
    if (fputs("NIYAH-PROOF-V1\n", f) == EOF) ok = 0;
    niyah_hash_to_hex(computed, hex);
    if (fprintf(f, "hash: %s\n", hex) < 0) ok = 0;
    hash_text(prompt, ph); niyah_hash_to_hex(ph, hex);
    if (fprintf(f, "prompt_hash: %s\n", hex) < 0) ok = 0;
    hash_text(output, oh); niyah_hash_to_hex(oh, hex);
    if (fprintf(f, "output_hash: %s\n", hex) < 0) ok = 0;
    niyah_hash_to_hex(rules, hex);
    if (fprintf(f, "rules_hash: %s\n", hex) < 0) ok = 0;
    if (fprintf(f, "rules_bytes: %llu\n", (unsigned long long)rule_bytes) < 0) ok = 0;
    if (write_hex_text(f, "prompt_hex: ", prompt) != 0) ok = 0;
    if (write_hex_text(f, "output_hex: ", output) != 0) ok = 0;
    if (ferror(f)) ok = 0;
    if (fclose(f) != 0) ok = 0;
    return ok ? 0 : -1;
}

bool niyah_proof_verify_saved_v1(const char *path, const char *rules_path,
                                  bool *rules_bound, bool *rules_verified) {
    FILE *f = NULL;
    char *header = NULL, *h = NULL, *ph = NULL, *oh = NULL, *rh = NULL;
    char *rlen = NULL, *px = NULL, *ox = NULL, *prompt = NULL, *output = NULL;
    uint8_t stored[32], prompt_hash[32], output_hash[32], rule_hash[32];
    uint8_t computed[32], computed_rules[32], expected_prompt[32], expected_output[32];
    uint64_t expected_bytes = 0u, computed_bytes = 0u;
    bool ok = false;
    int extra;
    if (rules_bound) *rules_bound = false;
    if (rules_verified) *rules_verified = false;
    if (!path) return false;
    f = niyah_fopen_utf8(path, "rb");
    if (!f || !proof_file_size_ok(f)) goto done;
    header = proof_v1_line(f, 64u);
    if (!header || strcmp(header, "NIYAH-PROOF-V1") != 0) goto done;
    h = proof_v1_field(f, "hash: ", 80u);
    ph = proof_v1_field(f, "prompt_hash: ", 90u);
    oh = proof_v1_field(f, "output_hash: ", 90u);
    rh = proof_v1_field(f, "rules_hash: ", 90u);
    rlen = proof_v1_field(f, "rules_bytes: ", 40u);
    px = proof_v1_field(f, "prompt_hex: ", NIYAH_PROOF_MAX_HEX_BYTES + 24u);
    ox = proof_v1_field(f, "output_hex: ", NIYAH_PROOF_MAX_HEX_BYTES + 24u);
    if (!h || !ph || !oh || !rh || !rlen || !px || !ox) goto done;
    extra = fgetc(f);
    if (extra != EOF || ferror(f)) goto done;
    if (!proof_v1_uint64(rlen, &expected_bytes) ||
        !hex_to_hash(h, stored) || !hex_to_hash(ph, prompt_hash) ||
        !hex_to_hash(oh, output_hash) || !hex_to_hash(rh, rule_hash)) goto done;
    if (rules_bound) *rules_bound = expected_bytes != 0u;
    prompt = decode_hex_text(px);
    output = decode_hex_text(ox);
    if (!prompt || !output) goto done;
    hash_text(prompt, expected_prompt); hash_text(output, expected_output);
    if (memcmp(prompt_hash, expected_prompt, 32u) != 0 ||
        memcmp(output_hash, expected_output, 32u) != 0) goto done;
    if (expected_bytes != 0u && (!rules_path || !rules_path[0])) goto done;
    if (proof_v1_compute(prompt, output, rules_path,
                         computed, computed_rules, &computed_bytes) != 0) goto done;
    if (expected_bytes != computed_bytes ||
        memcmp(computed_rules, rule_hash, 32u) != 0 ||
        memcmp(computed, stored, 32u) != 0) goto done;
    ok = true;
    if (rules_verified) *rules_verified = true;
done:
    if (f) (void)fclose(f);
    free(header); free(h); free(ph); free(oh); free(rh); free(rlen);
    free(px); free(ox); free(prompt); free(output);
    return ok;
}
