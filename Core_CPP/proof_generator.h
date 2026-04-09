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

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Chain-of-Thought Proof Trail
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

/* Step types in a reasoning chain */
typedef enum {
    NIYAH_STEP_NEURAL_GEN,    /* Neural network generated text */
    NIYAH_STEP_RULE_CHECK,    /* Rule was checked against output */
    NIYAH_STEP_RULE_PASS,     /* Output passed a rule */
    NIYAH_STEP_RULE_FAIL,     /* Output violated a rule */
    NIYAH_STEP_RESAMPLE,      /* Re-sampling due to violation */
    NIYAH_STEP_SYM_QUERY,     /* Symbolic reasoner query */
    NIYAH_STEP_SYM_RESULT,    /* Symbolic reasoner result */
    NIYAH_STEP_CSP_CHECK,     /* Constraint solver check */
    NIYAH_STEP_FINAL_ACCEPT,  /* Final output accepted */
    NIYAH_STEP_FINAL_REJECT,  /* Final output rejected */
} NiyahProofStepKind;

/* One step in the chain-of-thought */
typedef struct {
    NiyahProofStepKind kind;
    uint32_t           step_id;       /* 0-based sequential */
    uint32_t           parent_id;     /* parent step (0xFFFFFFFF = root) */
    char               description[512];
    char               data[1024];    /* relevant data (rule text, output snippet, etc.) */
    uint8_t            step_hash[32]; /* SHA-256 of this step's content */
    double             timestamp_ms;  /* monotonic time from start */
} NiyahProofStep;

/* Full chain-of-thought proof trail */
typedef struct {
    NiyahProofStep *steps;
    uint32_t        count;
    uint32_t        capacity;
    uint8_t         chain_hash[32]; /* rolling hash of all steps */
    char            prompt[512];
    char            final_output[2048];
    double          start_time_ms;
} NiyahProofChain;

/* Create/destroy */
NiyahProofChain *niyah_proof_chain_alloc(const char *prompt);
void             niyah_proof_chain_free(NiyahProofChain *chain);

/* Add a step — returns step_id */
uint32_t niyah_proof_chain_add(NiyahProofChain *chain,
                               NiyahProofStepKind kind,
                               uint32_t parent_id,
                               const char *description,
                               const char *data);

/* Finalize: set final output, compute chain hash */
void niyah_proof_chain_finalize(NiyahProofChain *chain, const char *final_output);

/* Save chain to .proof file (extended format) */
int niyah_proof_chain_save(const NiyahProofChain *chain, const char *path);

/* Verify a chain proof file */
bool niyah_proof_chain_verify(const char *proof_path);

/* Print chain to stdout (human-readable) */
void niyah_proof_chain_print(const NiyahProofChain *chain);

/* Get the number of rule violations in the chain */
uint32_t niyah_proof_chain_violations(const NiyahProofChain *chain);

/* Get explainability report: which rules were applied */
void niyah_proof_chain_explain(const NiyahProofChain *chain);

#ifdef __cplusplus
}
#endif
#endif /* PROOF_GENERATOR_H */
