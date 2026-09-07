#ifndef KHZ_Q_SVD_H
#define KHZ_Q_SVD_H

#include <stdbool.h>

#define KHZ_MAX_N 8
#define KHZ_JACOBI_ITER 100

typedef struct {
    float sigma[KHZ_MAX_N];
    float energy_preserved;
    float residual_penalty;
    int rank_used;
    int chi_e; /* compatibility alias; identical to rank_used */
    bool passes_threshold;
} KHZQ_Result;

/*
 * Analyze byte-position distribution with a small symmetric matrix and
 * Jacobi decomposition. This is a structural diagnostic only. It does not
 * measure meaning, factuality, ethics, safety, or model correctness.
 */
KHZQ_Result khz_q_analyze_output(const char *text, float target_energy);

/* Compatibility entry point for existing callers; same structural metric. */
static inline KHZQ_Result khz_q_verify_output(const char *text, float target_energy) {
    return khz_q_analyze_output(text, target_energy);
}

void khz_q_build_byte_matrix(const char *text,
                             float matrix[KHZ_MAX_N][KHZ_MAX_N]);
void khz_q_jacobi_svd(float matrix[KHZ_MAX_N][KHZ_MAX_N],
                      float singular_values[KHZ_MAX_N],
                      int n,
                      int max_iter);
float khz_q_residual_penalty(float singular_values[KHZ_MAX_N],
                             int rank_used,
                             int n);

#endif
