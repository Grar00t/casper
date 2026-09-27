/*
 * proof_generator.h — NIYAH integrity receipt generation & verification
 *
 * SHA-256 hashing + audit receipt support for hybrid inference.
 * The receipt binds prompt bytes, output bytes, and the exact rule material
 * (or an empty rule set). It is an integrity receipt, not an authenticity,
 * factual-correctness, or policy-compliance attestation.
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

/* Compute SHA-256 hash of data[0..len-1]. Result in out[32]. */
void niyah_sha256(const uint8_t *data, size_t len, uint8_t out[32]);

/* Hash a file byte-for-byte. Returns true on success. */
bool niyah_sha256_file(const char *path, uint8_t out[32]);

/* Convert 32-byte hash to 64-char hex string (null-terminated, needs 65 bytes). */
void niyah_hash_to_hex(const uint8_t hash[32], char hex[65]);

/*
 * Generate a V2 receipt hash from prompt/output/rule material.
 * rule_material is the exact rule-file CONTENT, not a path. NULL is the empty
 * rule set. The V2 construction is domain-separated and hashes components
 * before combining them.
 */
void niyah_proof_generate(const char *prompt, const char *output,
                          const char *rule_material, uint8_t proof[32]);

/* Same V2 construction when the caller already has SHA-256(rule material). */
void niyah_proof_generate_hashed(const char *prompt, const char *output,
                                 const uint8_t rules_hash[32],
                                 uint8_t proof[32]);

/*
 * Save a V2 .proof receipt. rule_material is exact rule-file CONTENT.
 * Returns 0 on success, -1 on I/O error.
 */
int niyah_proof_save(const char *path, const uint8_t proof[32],
                     const char *prompt, const char *output,
                     const char *rule_material);

/* Save a V2 receipt when the caller already has SHA-256(rule material). */
int niyah_proof_save_hashed(const char *path, const uint8_t proof[32],
                            const char *prompt, const char *output,
                            const uint8_t rules_hash[32]);

/*
 * Verify a V2 receipt against supplied prompt/output/rule material.
 * This verifies byte-level receipt integrity only.
 */
bool niyah_proof_verify(const char *proof_path,
                        const char *prompt,
                        const char *output,
                        const char *rule_material);

/* Smoke test — returns failed-assertion count (0 = all pass). */
int niyah_proof_smoke(void);

#ifdef __cplusplus
}
#endif
#endif /* PROOF_GENERATOR_H */
