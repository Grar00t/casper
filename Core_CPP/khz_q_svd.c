/* khz_q_svd.c — byte-structure SVD diagnostic. */
#include "khz_q_svd.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#define KHZ_RESIDUAL_THRESHOLD 1.0f
#define KHZ_JACOBI_EPS 1e-9f

void khz_q_build_byte_matrix(const char *text,
                             float matrix[KHZ_MAX_N][KHZ_MAX_N]) {
    uint32_t len = 0u;
    uint32_t bucket_size;
    float frequency[KHZ_MAX_N] = {0};
    float max_frequency = 0.0f;
    int i;
    int j;

    memset(matrix, 0, sizeof(float) * KHZ_MAX_N * KHZ_MAX_N);
    if (!text || !text[0]) return;

    while (text[len]) ++len;
    if (len == 0u) return;
    bucket_size = (len + KHZ_MAX_N - 1u) / KHZ_MAX_N;
    if (bucket_size == 0u) bucket_size = 1u;

    for (uint32_t k = 0u; k < len; ++k) {
        int bucket = (int)(k / bucket_size);
        if (bucket >= KHZ_MAX_N) bucket = KHZ_MAX_N - 1;
        frequency[bucket] += (float)(unsigned char)text[k];
    }

    for (i = 0; i < KHZ_MAX_N; ++i)
        if (frequency[i] > max_frequency) max_frequency = frequency[i];
    if (max_frequency < KHZ_JACOBI_EPS) max_frequency = 1.0f;
    for (i = 0; i < KHZ_MAX_N; ++i) frequency[i] /= max_frequency;

    for (i = 0; i < KHZ_MAX_N; ++i) {
        matrix[i][i] = frequency[i];
        for (j = i + 1; j < KHZ_MAX_N; ++j) {
            float coupling = fabsf(frequency[i] - frequency[j]) * 0.15f;
            matrix[i][j] = coupling;
            matrix[j][i] = coupling;
        }
    }
}

void khz_q_jacobi_svd(float matrix[KHZ_MAX_N][KHZ_MAX_N],
                      float singular_values[KHZ_MAX_N],
                      int n,
                      int max_iter) {
    int iter;
    int p;
    int q;
    int i;
    int j;

    if (!matrix || !singular_values || n <= 0 || n > KHZ_MAX_N || max_iter <= 0) return;

    for (iter = 0; iter < max_iter; ++iter) {
        float off_diagonal_energy = 0.0f;
        for (p = 0; p < n; ++p)
            for (q = p + 1; q < n; ++q)
                off_diagonal_energy += matrix[p][q] * matrix[p][q];
        if (off_diagonal_energy < KHZ_JACOBI_EPS) break;

        for (p = 0; p < n - 1; ++p) {
            for (q = p + 1; q < n; ++q) {
                float apq = matrix[p][q];
                float app;
                float aqq;
                float denominator;
                float tau;
                float t;
                float c;
                float s;

                if (fabsf(apq) < KHZ_JACOBI_EPS) continue;
                app = matrix[p][p];
                aqq = matrix[q][q];
                denominator = 2.0f * apq;
                if (fabsf(denominator) < KHZ_JACOBI_EPS) continue;

                tau = (aqq - app) / denominator;
                t = tau >= 0.0f
                    ? 1.0f / (tau + sqrtf(1.0f + tau * tau))
                    : -1.0f / (-tau + sqrtf(1.0f + tau * tau));
                c = 1.0f / sqrtf(1.0f + t * t);
                s = t * c;

                for (i = 0; i < n; ++i) {
                    float aip = matrix[i][p];
                    float aiq = matrix[i][q];
                    matrix[i][p] = c * aip - s * aiq;
                    matrix[i][q] = s * aip + c * aiq;
                }
                for (j = 0; j < n; ++j) {
                    float apj = matrix[p][j];
                    float aqj = matrix[q][j];
                    matrix[p][j] = c * apj - s * aqj;
                    matrix[q][j] = s * apj + c * aqj;
                }
            }
        }
    }

    for (i = 0; i < n; ++i) singular_values[i] = fabsf(matrix[i][i]);
}

static void sort_descending(float values[KHZ_MAX_N], int n) {
    int i;
    int j;
    for (i = 0; i < n - 1; ++i) {
        for (j = i + 1; j < n; ++j) {
            if (values[j] > values[i]) {
                float tmp = values[i];
                values[i] = values[j];
                values[j] = tmp;
            }
        }
    }
}

float khz_q_residual_penalty(float singular_values[KHZ_MAX_N],
                             int rank_used,
                             int n) {
    float total = 0.0f;
    float kept = 0.0f;
    int i;
    if (!singular_values || n <= 0 || n > KHZ_MAX_N || rank_used < 0 || rank_used > n) return 10.0f;
    for (i = 0; i < n; ++i) total += singular_values[i] * singular_values[i];
    for (i = 0; i < rank_used; ++i) kept += singular_values[i] * singular_values[i];
    if (total < KHZ_JACOBI_EPS) return 10.0f;
    return ((total - kept) / total) * 10.0f;
}

KHZQ_Result khz_q_analyze_output(const char *text, float target_energy) {
    KHZQ_Result result;
    float matrix[KHZ_MAX_N][KHZ_MAX_N];
    float singular_values[KHZ_MAX_N] = {0};
    float total_energy = 0.0f;
    float cumulative = 0.0f;
    int rank_used = 0;
    int i;

    memset(&result, 0, sizeof(result));
    if (target_energy <= 0.0f) target_energy = 0.50f;
    if (target_energy > 1.0f) target_energy = 1.0f;

    khz_q_build_byte_matrix(text, matrix);
    khz_q_jacobi_svd(matrix, singular_values, KHZ_MAX_N, KHZ_JACOBI_ITER);
    sort_descending(singular_values, KHZ_MAX_N);

    for (i = 0; i < KHZ_MAX_N; ++i)
        total_energy += singular_values[i] * singular_values[i];

    if (total_energy > KHZ_JACOBI_EPS) {
        for (i = 0; i < KHZ_MAX_N; ++i) {
            cumulative += singular_values[i] * singular_values[i];
            ++rank_used;
            if ((cumulative / total_energy) >= target_energy) break;
        }
    }

    result.rank_used = rank_used;
    result.energy_preserved = total_energy > KHZ_JACOBI_EPS ? cumulative / total_energy : 0.0f;
    result.residual_penalty = khz_q_residual_penalty(singular_values, rank_used, KHZ_MAX_N);
    for (i = 0; i < rank_used; ++i) result.sigma[i] = singular_values[i];
    result.passes_threshold = total_energy > KHZ_JACOBI_EPS
        && result.energy_preserved >= target_energy
        && result.residual_penalty < KHZ_RESIDUAL_THRESHOLD;
    return result;
}

#ifdef KHZQ_STANDALONE_TEST
#include <stdio.h>
int main(void) {
    int failed = 0;
    KHZQ_Result empty = khz_q_analyze_output("", 0.50f);
    KHZQ_Result text = khz_q_analyze_output("bismillah bismillah bismillah", 0.50f);
    if (empty.energy_preserved != 0.0f || empty.rank_used != 0 || empty.passes_threshold) ++failed;
    if (text.rank_used < 1 || text.rank_used > KHZ_MAX_N) ++failed;
    printf("khz_q_structure_smoke failed=%d\n", failed);
    return failed ? 1 : 0;
}
#endif
