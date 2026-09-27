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
 * Generate a V2 proof hash bound to:
 *   prompt || NUL || output || NUL || SHA256(rule-file bytes)
 *
 * rule_file is a filesystem path. NULL or "" means no rule file and binds
 * SHA256(empty). If a non-empty rule_file cannot be read, proof is zeroed.
 */
void niyah_proof_generate(const char *prompt, const char *output,
                          const char *rule_file, uint8_t proof[32]);

/*
 * Save a NIYAH-PROOF-V2 file. Prompt/output are escaped onto single lines,
 * and rules_path/rules_hash identify the exact rule file bytes used.
 * Returns 0 on success, -1 on I/O/rule-file error.
 */
int niyah_proof_save(const char *path, const uint8_t proof[32],
                     const char *prompt, const char *output,
                     const char *rule_file);

/*
 * Verify a proof file.
 *
 * V2 proofs are self-describing: embedded escaped prompt/output and rules_path
 * are used, so callers may pass NULL for prompt/output/rule_file. The current
 * rule file must still exist and match the recorded content hash.
 *
 * V1 proofs retain the legacy caller-supplied verification behavior.
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
