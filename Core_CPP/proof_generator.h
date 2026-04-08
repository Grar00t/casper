/*
 * proof_generator.h — NIYAH Proof Generation & Verification
 *
 * SHA-256 hashing + proof audit trail for hybrid inference.
 * Public-domain SHA-256 implementation (no OpenSSL dependency).
 *
 * Zero external dependencies. C11 clean. C++17 compatible.
 */
#ifndef PROOF_GENERATOR_H
#define PROOF_GENERATOR_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * SHA-256
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

/* Compute SHA-256 hash of data[0..len-1]. Result in out[32]. */
void niyah_sha256(const uint8_t *data, size_t len, uint8_t out[32]);

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Proof generation / verification
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

/*
 * Generate proof hash: SHA-256(prompt || output || rule_file_contents).
 * rule_file may be NULL (hashed as empty string).
 */
void niyah_proof_generate(const char *prompt, const char *output,
                          const char *rule_file, uint8_t proof[32]);

/*
 * Save proof to a .proof file (human-readable + machine-verifiable).
 * Returns 0 on success, -1 on I/O error.
 */
int niyah_proof_save(const char *path, const uint8_t proof[32],
                     const char *prompt, const char *output,
                     const char *rule_file);

/*
 * Verify a .proof file by re-computing the hash and comparing.
 * Returns true if the proof matches.
 */
bool niyah_proof_verify(const char *proof_path,
                        const char *prompt,
                        const char *output,
                        const char *rule_file);

/* Convert 32-byte hash to 64-char hex string (null-terminated, needs 65 bytes) */
void niyah_hash_to_hex(const uint8_t hash[32], char hex[65]);

/* Smoke test — returns failed-assertion count (0 = all pass) */
int niyah_proof_smoke(void);

#ifdef __cplusplus
}
#endif
#endif /* PROOF_GENERATOR_H */
