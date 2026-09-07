/* proof_generator.h — SHA-256 integrity records for Casper/NIYAH. */
#ifndef PROOF_GENERATOR_H
#define PROOF_GENERATOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void niyah_sha256(const uint8_t *data, size_t len, uint8_t out[32]);
void niyah_hash_to_hex(const uint8_t hash[32], char hex[65]);

/*
 * Compute SHA-256 over prompt, output, and the bytes of rule_file.
 * rule_file may be NULL. If a non-NULL rule file cannot be read, returns -1
 * and zeroes proof. This is an integrity digest, not a digital signature.
 */
int niyah_proof_generate(const char *prompt, const char *output,
                         const char *rule_file, uint8_t proof[32]);

/*
 * Save a self-contained NIYAH-INTEGRITY-V2 record. The supplied digest must
 * match the supplied prompt/output/rules or the write is rejected.
 */
int niyah_proof_save(const char *path, const uint8_t proof[32],
                     const char *prompt, const char *output,
                     const char *rule_file);

/* Verify a record against caller-supplied prompt/output/rules. */
bool niyah_proof_verify(const char *proof_path,
                        const char *prompt,
                        const char *output,
                        const char *rule_file);

/* Verify using the prompt/output/rule path embedded in a V2 record. */
bool niyah_proof_verify_stored(const char *proof_path);

int niyah_proof_smoke(void);

#ifdef __cplusplus
}
#endif
#endif
