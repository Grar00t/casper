#ifndef KHZ_Q_SVD_H
#define KHZ_Q_SVD_H

/*
 * KHZ_Q Ethical Prism — SVD Verification Layer
 * Integrates into Casper_Engine Verify step.
 * Zero external dependencies. C11 clean. libc + libm only.
 */

#include <stdint.h>
#include <stdbool.h>

#define KHZ_MAX_N        8      /* matrix dimension               */
#define KHZ_CHI_E_MAX    8      /* maximum ethical rank           */
#define KHZ_JACOBI_ITER  100    /* Jacobi SVD sweep iterations    */

/* Result returned to the Verify step */
typedef struct {
    float sigma[KHZ_CHI_E_MAX]; /* top singular values (descending) */
    float energy_preserved;     /* 0.0 – 1.0 ratio                  */
    float penalty_nasl;         /* 0.0 – 10.0 disruption score      */
    int   chi_e;                /* actual rank used                 */
    bool  is_coherent;          /* true  → Accept, false → Re-sample */
} KHZQ_Result;

/*
 * khz_q_verify_output()
 *
 * Takes the candidate generated text and a target energy threshold
 * (e.g. 0.85f for 85 %).
 *
 * Steps:
 *   1. Build 8x8 co-occurrence relationship matrix from char n-grams.
 *   2. One-sided Jacobi SVD (pure C11, no LAPACK/BLAS).
 *   3. Adaptive chi truncation: keep fewest singular values whose
 *      cumulative energy >= target_energy.
 *   4. Compute Penalty_Nasl from truncation residual.
 *   5. Return KHZQ_Result with sovereign decision.
 */
KHZQ_Result khz_q_verify_output(const char *generated_text,
                                float       target_energy);

/* Lower-level helpers (exposed for unit testing) */
void  khz_q_build_ngram_matrix(const char *text,
                               float M[KHZ_MAX_N][KHZ_MAX_N]);
void  khz_q_jacobi_svd(float A[KHZ_MAX_N][KHZ_MAX_N],
                       float S[KHZ_MAX_N],
                       int n, int max_iter);
float khz_q_penalty(float S[KHZ_MAX_N], int chi_e, int n);

#endif /* KHZ_Q_SVD_H */
