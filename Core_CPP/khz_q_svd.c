/*
 * khz_q_svd.c — KHZ_Q numerical text-shape heuristic
 *
 * Hybrid inference use:
 *   Encode -> Generate -> Decode -> [khz_q_verify_output] -> Re-sample / accept
 *
 * This implementation operates on byte-derived positional bucket statistics.
 * It does not establish semantic correctness, factual truth, ethics, intent,
 * or policy compliance.
 *
 * Constraints:
 *   - Zero external dependencies (libc + libm only)
 *   - No dynamic allocation in this translation unit
 *   - C11 clean: compiles with -Wall -Wextra -Werror -pedantic
 *   - SIMD-agnostic scalar core
 */

#include "khz_q_svd.h"
#include <math.h>
#include <string.h>
#include <stdint.h>

#define KHZ_PENALTY_THRESHOLD  1.0f
#define KHZ_JACOBI_EPS         1e-9f

/*
 * Partition the UTF-8 byte sequence into KHZ_MAX_N positional buckets.
 * Diagonal entries hold normalized bucket magnitudes. Off-diagonal entries
 * hold scaled absolute differences between bucket magnitudes.
 */
void khz_q_build_ngram_matrix(const char *text,
                              float M[KHZ_MAX_N][KHZ_MAX_N])
{
    memset(M, 0, sizeof(float) * KHZ_MAX_N * KHZ_MAX_N);
    if (!text || !*text) return;

    uint32_t len = 0;
    while (text[len]) len++;
    if (len == 0) return;

    uint32_t bucket_sz = (len + KHZ_MAX_N - 1) / KHZ_MAX_N;
    if (bucket_sz == 0) bucket_sz = 1;

    float freq[KHZ_MAX_N] = {0};
    for (uint32_t k = 0; k < len; k++) {
        int b = (int)(k / bucket_sz);
        if (b >= KHZ_MAX_N) b = KHZ_MAX_N - 1;
        freq[b] += (float)(unsigned char)text[k];
    }

    float fmax = 0.0f;
    for (int i = 0; i < KHZ_MAX_N; i++)
        if (freq[i] > fmax) fmax = freq[i];
    if (fmax < 1e-9f) fmax = 1.0f;
    for (int i = 0; i < KHZ_MAX_N; i++)
        freq[i] /= fmax;

    for (int i = 0; i < KHZ_MAX_N; i++) {
        M[i][i] = freq[i];
        for (int j = i + 1; j < KHZ_MAX_N; j++) {
            float c = fabsf(freq[i] - freq[j]) * 0.15f;
            M[i][j] = c;
            M[j][i] = c;
        }
    }
}

/*
 * Jacobi diagonalization of the symmetric matrix A. The implementation uses
 * the absolute diagonal values after convergence as the spectral magnitudes
 * consumed by the heuristic. This routine is numerical machinery only.
 */
void khz_q_jacobi_svd(float A[KHZ_MAX_N][KHZ_MAX_N],
                      float S[KHZ_MAX_N],
                      int n, int max_iter)
{
    for (int iter = 0; iter < max_iter; iter++) {
        float off = 0.0f;
        for (int p = 0; p < n; p++)
            for (int q = p + 1; q < n; q++)
                off += A[p][q] * A[p][q];
        if (off < KHZ_JACOBI_EPS) break;

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

    for (int i = 0; i < n; i++)
        S[i] = fabsf(A[i][i]);
}

static void sort_descending(float S[KHZ_MAX_N], int n)
{
    for (int i = 0; i < n - 1; i++)
        for (int j = i + 1; j < n; j++)
            if (S[j] > S[i]) {
                float tmp = S[i]; S[i] = S[j]; S[j] = tmp;
            }
}

/* Residual-energy ratio scaled to [0,10]. Historical API name retained. */
float khz_q_penalty(float S[KHZ_MAX_N], int chi_e, int n)
{
    float total = 0.0f, kept = 0.0f;
    for (int i = 0; i < n; i++)         total += S[i] * S[i];
    for (int i = 0; i < chi_e; i++)     kept  += S[i] * S[i];
    if (total < 1e-9f) return 10.0f;
    return ((total - kept) / total) * 10.0f;
}

KHZQ_Result khz_q_verify_output(const char *generated_text,
                                float       target_energy)
{
    KHZQ_Result res;
    memset(&res, 0, sizeof(res));

    if (target_energy <= 0.0f) target_energy = 0.50f;
    if (target_energy >  1.0f) target_energy = 1.0f;

    int n = KHZ_MAX_N;
    float M[KHZ_MAX_N][KHZ_MAX_N];
    float S[KHZ_MAX_N];

    khz_q_build_ngram_matrix(generated_text, M);
    khz_q_jacobi_svd(M, S, n, KHZ_JACOBI_ITER);
    sort_descending(S, n);

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

    res.chi_e             = chi_e;
    res.energy_preserved  = (total_energy > 1e-9f)
                            ? (cumulative / total_energy)
                            : 0.0f;
    res.penalty_nasl      = khz_q_penalty(S, chi_e, n);

    for (int i = 0; i < chi_e && i < KHZ_CHI_E_MAX; i++)
        res.sigma[i] = S[i];

    res.is_coherent = (res.energy_preserved >= target_energy) &&
                      (res.penalty_nasl     <  KHZ_PENALTY_THRESHOLD);

    return res;
}

#ifdef KHZQ_STANDALONE_TEST
#include <stdio.h>

int main(void)
{
    int pass = 0, fail = 0;

    {
        const char *t = "bismillah bismillah bismillah bismillah ";
        KHZQ_Result r = khz_q_verify_output(t, 0.50f);
        int ok = (r.energy_preserved >= 0.50f);
        printf("[%s] T1 repeated text: energy=%.4f penalty=%.4f chi_e=%d\n",
               ok ? "PASS" : "FAIL",
               r.energy_preserved, r.penalty_nasl, r.chi_e);
        ok ? pass++ : fail++;
    }

    {
        KHZQ_Result r = khz_q_verify_output("", 0.50f);
        int ok = (!r.is_coherent);
        printf("[%s] T2 empty text: energy=%.4f penalty=%.4f\n",
               ok ? "PASS" : "FAIL",
               r.energy_preserved, r.penalty_nasl);
        ok ? pass++ : fail++;
    }

    {
        KHZQ_Result r = khz_q_verify_output("test", 1.5f);
        int ok = (r.chi_e > 0 && r.chi_e <= KHZ_MAX_N);
        printf("[%s] T3 clamp: chi_e=%d energy=%.4f\n",
               ok ? "PASS" : "FAIL", r.chi_e, r.energy_preserved);
        ok ? pass++ : fail++;
    }

    {
        const char *t = "\xd8\xa8\xd8\xb3\xd9\x85 \xd8\xa7\xd9\x84\xd9\x84\xd9\x87";
        KHZQ_Result r = khz_q_verify_output(t, 0.80f);
        int ok = (r.chi_e >= 1);
        printf("[%s] T4 Arabic UTF-8 bytes: energy=%.4f chi_e=%d\n",
               ok ? "PASS" : "FAIL",
               r.energy_preserved, r.chi_e);
        ok ? pass++ : fail++;
    }

    {
        const char *lo = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
        const char *hi = "a1!b2@c3#d4$e5%f6^g7&h8*i9(j0)k_l+m-n=o|p";
        KHZQ_Result r_lo = khz_q_verify_output(lo, 0.50f);
        KHZQ_Result r_hi = khz_q_verify_output(hi, 0.50f);
        int ok = (r_hi.penalty_nasl >= r_lo.penalty_nasl);
        printf("[%s] T5 byte-pattern penalty: low=%.4f high=%.4f\n",
               ok ? "PASS" : "FAIL",
               r_lo.penalty_nasl, r_hi.penalty_nasl);
        ok ? pass++ : fail++;
    }

    printf("\nKHZ_Q numerical smoke: %d passed, %d failed\n", pass, fail);
    return (fail == 0) ? 0 : 1;
}
#endif
