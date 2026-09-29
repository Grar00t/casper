#ifndef KHZ_Q_SVD_H
#define KHZ_Q_SVD_H

/*
 * KHZ_Q numerical text-coherence heuristic.
 *
 * This layer derives an 8x8 matrix from byte-frequency buckets, applies a
 * Jacobi diagonalization step, and gates on retained spectral energy plus a
 * residual-energy penalty. It does NOT establish factual truth, ethics,
 * semantic correctness, or policy compliance.
 *
 * Zero external dependencies. C11 clean. libc + libm only.
 */

#include <stdint.h>
#include <stdbool.h>

#define KHZ_MAX_N        8      /* matrix dimension               */
#define KHZ_CHI_E_MAX    8      /* maximum retained rank          */
#define KHZ_JACOBI_ITER  100    /* Jacobi sweep iterations        */

typedef struct {
    float sigma[KHZ_CHI_E_MAX]; /* top diagonal magnitudes          */
    float energy_preserved;     /* 0.0 - 1.0 retained-energy ratio  */
    float penalty_nasl;         /* 0.0 - 10.0 residual-energy score */
    int   chi_e;                /* rank used to hit target energy   */
    bool  is_coherent;          /* heuristic threshold decision     */
} KHZQ_Result;

/*
 * khz_q_verify_output()
 *
 * Takes candidate generated text and a target energy threshold.
 *
 * Steps:
 *   1. Partition UTF-8 bytes into eight positional buckets and derive a
 *      symmetric frequency-difference matrix.
 *   2. Apply Jacobi rotations to diagonalize that matrix.
 *   3. Keep the minimum rank whose cumulative squared magnitude reaches the
 *      target energy.
 *   4. Compute the residual-energy penalty.
 *   5. Return the numerical threshold decision.
 *
 * The result is a text-shape heuristic. It must not be interpreted as a
 * semantic, ethical, factual, or logical proof.
 */
KHZQ_Result khz_q_verify_output(const char *generated_text,
                                float       target_energy);

/* Lower-level helpers (exposed for unit testing). */
void  khz_q_build_ngram_matrix(const char *text,
                               float M[KHZ_MAX_N][KHZ_MAX_N]);
void  khz_q_jacobi_svd(float A[KHZ_MAX_N][KHZ_MAX_N],
                       float S[KHZ_MAX_N],
                       int n, int max_iter);
float khz_q_penalty(float S[KHZ_MAX_N], int chi_e, int n);

#endif /* KHZ_Q_SVD_H */
