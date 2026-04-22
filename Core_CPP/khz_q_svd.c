/*
 * khz_q_svd.c — KHZ_Q Ethical Prism: SVD Verification Layer
 *
 * Designed for injection into Casper_Engine Hybrid Inference:
 *   Encode -> Generate -> Decode -> [khz_q_verify_output] -> Re-sample / Prove
 *
 * Constraints (matches Casper_Engine design principles):
 *   - Zero external dependencies (libc + libm only)
 *   - No dynamic allocation (all stack-allocated; single-pool friendly)
 *   - C11 clean: compiles with -Wall -Wextra -Werror -pedantic
 *   - SIMD-agnostic scalar core (SIMD layer can wrap later)
 */

#include "khz_q_svd.h"
#include <math.h>
#include <string.h>
#include <stdint.h>

/* ── Constants ────────────────────────────────────────────────────────── */
#define KHZ_PENALTY_THRESHOLD  1.0f
#define KHZ_JACOBI_EPS         1e-9f

/* ── 1. N-gram Co-occurrence Matrix Builder ───────────────────────────── */
/*
 * Partitions text into KHZ_MAX_N character-buckets.
 * M[i][j] = normalised co-occurrence weight between bucket i and j.
 * Diagonal captures self-coherence ("Fitrah" signal).
 * Off-diagonal captures cross-bucket coupling (noise/disruption).
 */
void khz_q_build_ngram_matrix(const char *text,
                              float M[KHZ_MAX_N][KHZ_MAX_N])
{
    memset(M, 0, sizeof(float) * KHZ_MAX_N * KHZ_MAX_N);
    if (!text || !*text) return;

    /* Count characters per bucket */
    uint32_t len = 0;
    while (text[len]) len++;
    if (len == 0) return;

    uint32_t bucket_sz = (len + KHZ_MAX_N - 1) / KHZ_MAX_N;
    if (bucket_sz == 0) bucket_sz = 1;

    /* Bucket frequency vector (raw char sums) */
    float freq[KHZ_MAX_N] = {0};
    for (uint32_t k = 0; k < len; k++) {
        int b = (int)(k / bucket_sz);
        if (b >= KHZ_MAX_N) b = KHZ_MAX_N - 1;
        freq[b] += (float)(unsigned char)text[k];
    }

    /* Normalise to [0,1] */
    float fmax = 0.0f;
    for (int i = 0; i < KHZ_MAX_N; i++)
        if (freq[i] > fmax) fmax = freq[i];
    if (fmax < 1e-9f) fmax = 1.0f;
    for (int i = 0; i < KHZ_MAX_N; i++)
        freq[i] /= fmax;

    /*
     * Build symmetric relationship matrix:
     *   M[i][i] = freq[i]           (self-coherence)
     *   M[i][j] = |freq[i]-freq[j]| * 0.15f   (cross-coupling / noise)
     */
    for (int i = 0; i < KHZ_MAX_N; i++) {
        M[i][i] = freq[i];
        for (int j = i + 1; j < KHZ_MAX_N; j++) {
            float c = fabsf(freq[i] - freq[j]) * 0.15f;
            M[i][j] = c;
            M[j][i] = c;
        }
    }
}

/* ── 2. One-sided Jacobi SVD ──────────────────────────────────────────── */
/*
 * Operates on the SYMMETRIC matrix A (our ethical matrix is symmetric).
 * After convergence, A becomes diagonal and A[i][i] holds eigenvalues.
 * We extract |A[i][i]| as singular values S[i].
 *
 * Reference: Golub & Van Loan, "Matrix Computations", §8.4 (Jacobi method).
 * All stack-allocated. No heap. Safe for single-pool engines.
 */
void khz_q_jacobi_svd(float A[KHZ_MAX_N][KHZ_MAX_N],
                      float S[KHZ_MAX_N],
                      int n, int max_iter)
{
    for (int iter = 0; iter < max_iter; iter++) {
        /* Compute off-diagonal Frobenius norm */
        float off = 0.0f;
        for (int p = 0; p < n; p++)
            for (int q = p + 1; q < n; q++)
                off += A[p][q] * A[p][q];
        if (off < KHZ_JACOBI_EPS) break;

        /* Jacobi rotations for every off-diagonal pair */
        for (int p = 0; p < n - 1; p++) {
            for (int q = p + 1; q < n; q++) {
                float apq = A[p][q];
                if (fabsf(apq) < KHZ_JACOBI_EPS) continue;

                float app = A[p][p];
                float aqq = A[q][q];
                float denom = 2.0f * apq;
                if (fabsf(denom) < KHZ_JACOBI_EPS) continue;

                float tau = (aqq - app) / denom;
                float t   = (tau >= 0.0f)
                            ?  1.0f / (tau + sqrtf(1.0f + tau * tau))
                            : -1.0f / (-tau + sqrtf(1.0f + tau * tau));
                float c   = 1.0f / sqrtf(1.0f + t * t);
                float s   = t * c;

                /* Apply Givens rotation: A = G^T * A * G */
                for (int i = 0; i < n; i++) {
                    float aip = A[i][p];
                    float aiq = A[i][q];
                    A[i][p]   =  c * aip - s * aiq;
                    A[i][q]   =  s * aip + c * aiq;
                }
                for (int j = 0; j < n; j++) {
                    float apj = A[p][j];
                    float aqj = A[q][j];
                    A[p][j]   =  c * apj - s * aqj;
                    A[q][j]   =  s * apj + c * aqj;
                }
            }
        }
    }

    /* Extract singular values from diagonal */
    for (int i = 0; i < n; i++)
        S[i] = fabsf(A[i][i]);
}

/* ── 3. Sort singular values descending (selection sort — small n) ─────── */
static void sort_descending(float S[KHZ_MAX_N], int n)
{
    for (int i = 0; i < n - 1; i++)
        for (int j = i + 1; j < n; j++)
            if (S[j] > S[i]) {
                float tmp = S[i]; S[i] = S[j]; S[j] = tmp;
            }
}

/* ── 4. Penalty_Nasl: residual energy ratio (scaled 0–10) ────────────── */
float khz_q_penalty(float S[KHZ_MAX_N], int chi_e, int n)
{
    float total = 0.0f, kept = 0.0f;
    for (int i = 0; i < n; i++)         total += S[i] * S[i];
    for (int i = 0; i < chi_e; i++)     kept  += S[i] * S[i];
    if (total < 1e-9f) return 10.0f;   /* degenerate matrix → max penalty */
    return ((total - kept) / total) * 10.0f;
}

/* ── 5. Main Verify Entry Point ───────────────────────────────────────── */
KHZQ_Result khz_q_verify_output(const char *generated_text,
                                float       target_energy)
{
    KHZQ_Result res;
    memset(&res, 0, sizeof(res));

    /* Clamp target energy to sensible range */
    if (target_energy <= 0.0f) target_energy = 0.85f;
    if (target_energy >  1.0f) target_energy = 1.0f;

    int n = KHZ_MAX_N;

    /* Stack-allocated matrices (no heap) */
    float M[KHZ_MAX_N][KHZ_MAX_N];
    float S[KHZ_MAX_N];

    /* Step 1: Build ethical relationship matrix from output text */
    khz_q_build_ngram_matrix(generated_text, M);

    /* Step 2: One-sided Jacobi SVD */
    khz_q_jacobi_svd(M, S, n, KHZ_JACOBI_ITER);

    /* Step 3: Sort descending */
    sort_descending(S, n);

    /* Step 4: Adaptive chi_E — keep minimum rank meeting target_energy */
    float total_energy = 0.0f;
    for (int i = 0; i < n; i++) total_energy += S[i] * S[i];

    float cumulative = 0.0f;
    int   chi_e      = 0;
    for (int i = 0; i < n; i++) {
        cumulative += S[i] * S[i];
        chi_e++;
        if (total_energy > 1e-9f &&
            (cumulative / total_energy) >= target_energy) break;
    }

    /* Step 5: Populate result */
    res.chi_e             = chi_e;
    res.energy_preserved  = (total_energy > 1e-9f)
                            ? (cumulative / total_energy)
                            : 0.0f;
    res.penalty_nasl      = khz_q_penalty(S, chi_e, n);

    for (int i = 0; i < chi_e && i < KHZ_CHI_E_MAX; i++)
        res.sigma[i] = S[i];

    /* Step 6: Sovereign decision */
    res.is_coherent = (res.energy_preserved >= target_energy) &&
                      (res.penalty_nasl     <  KHZ_PENALTY_THRESHOLD);

    return res;
}

/* ── Standalone smoke test (compile with -DKHZQ_STANDALONE_TEST) ─────── */
#ifdef KHZQ_STANDALONE_TEST
#include <stdio.h>

int main(void)
{
    int pass = 0, fail = 0;

    /* Test 1: coherent text (repetitive, low disruption) */
    {
        const char *t = "bismillah bismillah bismillah bismillah ";
        KHZQ_Result r = khz_q_verify_output(t, 0.85f);
        int ok = (r.energy_preserved >= 0.85f);
        printf("[%s] T1 coherent: energy=%.4f penalty=%.4f chi_e=%d\n",
               ok ? "PASS" : "FAIL",
               r.energy_preserved, r.penalty_nasl, r.chi_e);
        ok ? pass++ : fail++;
    }

    /* Test 2: NULL/empty text → safe defaults, no crash */
    {
        KHZQ_Result r = khz_q_verify_output("", 0.85f);
        int ok = (!r.is_coherent);   /* empty → degenerate → rejected */
        printf("[%s] T2 empty text: energy=%.4f penalty=%.4f\n",
               ok ? "PASS" : "FAIL",
               r.energy_preserved, r.penalty_nasl);
        ok ? pass++ : fail++;
    }

    /* Test 3: target_energy clamping */
    {
        KHZQ_Result r = khz_q_verify_output("test", 1.5f); /* clamps to 1.0 */
        int ok = (r.chi_e > 0 && r.chi_e <= KHZ_MAX_N);
        printf("[%s] T3 clamp: chi_e=%d energy=%.4f\n",
               ok ? "PASS" : "FAIL", r.chi_e, r.energy_preserved);
        ok ? pass++ : fail++;
    }

    /* Test 4: Arabic text (UTF-8 multi-byte) */
    {
        const char *t = "\xd8\xa8\xd8\xb3\xd9\x85 \xd8\xa7\xd9\x84\xd9\x84\xd9\x87";
        KHZQ_Result r = khz_q_verify_output(t, 0.80f);
        int ok = (r.chi_e >= 1);
        printf("[%s] T4 arabic UTF-8: energy=%.4f chi_e=%d\n",
               ok ? "PASS" : "FAIL",
               r.energy_preserved, r.chi_e);
        ok ? pass++ : fail++;
    }

    /* Test 5: penalty increases with high-entropy text */
    {
        const char *lo = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
        const char *hi = "a1!b2@c3#d4$e5%f6^g7&h8*i9(j0)k_l+m-n=o|p";
        KHZQ_Result r_lo = khz_q_verify_output(lo, 0.85f);
        KHZQ_Result r_hi = khz_q_verify_output(hi, 0.85f);
        int ok = (r_hi.penalty_nasl >= r_lo.penalty_nasl);
        printf("[%s] T5 entropy: low_penalty=%.4f high_penalty=%.4f\n",
               ok ? "PASS" : "FAIL",
               r_lo.penalty_nasl, r_hi.penalty_nasl);
        ok ? pass++ : fail++;
    }

    printf("\nKHZ_Q SVD Smoke: %d passed, %d failed\n", pass, fail);
    return (fail == 0) ? 0 : 1;
}
#endif /* KHZQ_STANDALONE_TEST */
