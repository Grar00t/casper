/*
 * khz_q_ising.c — exact classical Ising on 8 spins.
 * H(s) = sum h_i s_i + sum_{i<j} J_ij s_i s_j, s_i in {+1,-1}.
 * libc + libm only. Stack only. Not quantum. Not fiqh.
 */

#include "khz_q_ising.h"
#include <math.h>
#include <string.h>

void khz_ising_clear(KHZ_IsingModel *m)
{
    memset(m, 0, sizeof(*m));
    m->n = KHZ_ISING_N;
}

void khz_ising_set_field(KHZ_IsingModel *m, int i, float h)
{
    if (!m || i < 0 || i >= (int)m->n) return;
    m->h[i] = h;
}

void khz_ising_set_couple(KHZ_IsingModel *m, int i, int j, float Jij)
{
    if (!m || i == j) return;
    if (i < 0 || j < 0 || i >= (int)m->n || j >= (int)m->n) return;
    m->J[i][j] = Jij;
    m->J[j][i] = Jij;
}

void khz_ising_forbid_aligned(KHZ_IsingModel *m, int i, int j, float penalty)
{
    if (penalty < 0.0f) penalty = -penalty;
    khz_ising_set_couple(m, i, j, penalty);
}

float khz_ising_energy(const KHZ_IsingModel *m, const int8_t *spins)
{
    if (!m || !spins) return 0.0f;
    const int n = (int)m->n;
    float H = 0.0f;
    for (int i = 0; i < n; i++)
        H += m->h[i] * (float)spins[i];
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            H += m->J[i][j] * (float)spins[i] * (float)spins[j];
    return H;
}

float khz_ising_violation(const KHZ_IsingModel *m, const int8_t *spins, float j_thresh)
{
    if (!m || !spins) return 0.0f;
    if (j_thresh < 0.0f) j_thresh = 0.0f;
    const int n = (int)m->n;
    float v = 0.0f;
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (m->J[i][j] > j_thresh && spins[i] == spins[j])
                v += m->J[i][j];
        }
    }
    return v;
}

void khz_ising_from_matrix(KHZ_IsingModel *m,
                           float M[KHZ_MAX_N][KHZ_MAX_N],
                           float field_scale,
                           float couple_scale)
{
    khz_ising_clear(m);
    if (field_scale == 0.0f) field_scale = 1.0f;
    if (couple_scale == 0.0f) couple_scale = 1.0f;
    for (int i = 0; i < KHZ_MAX_N; i++)
        m->h[i] = -field_scale * M[i][i];
    for (int i = 0; i < KHZ_MAX_N; i++) {
        for (int j = i + 1; j < KHZ_MAX_N; j++) {
            float noise = M[i][j];
            if (noise < 0.0f) noise = -noise;
            m->J[i][j] = couple_scale * noise;
            m->J[j][i] = m->J[i][j];
        }
    }
}

static void mask_to_spins(uint32_t mask, int n, int8_t *spins)
{
    for (int i = 0; i < n; i++)
        spins[i] = (mask & (1u << i)) ? (int8_t)1 : (int8_t)-1;
}

KHZ_IsingResult khz_ising_solve_exact(const KHZ_IsingModel *m, float max_energy)
{
    KHZ_IsingResult out;
    memset(&out, 0, sizeof(out));
    if (!m || m->n == 0 || m->n > KHZ_ISING_N) {
        out.feasible = false;
        out.energy = 0.0f;
        return out;
    }

    const int n = (int)m->n;
    const uint32_t limit = 1u << n;
    int8_t spins[KHZ_ISING_N];
    float best = 0.0f;
    uint32_t best_mask = 0;
    int have = 0;

    for (uint32_t mask = 0; mask < limit; mask++) {
        mask_to_spins(mask, n, spins);
        float H = khz_ising_energy(m, spins);
        if (!have || H < best) {
            best = H;
            best_mask = mask;
            have = 1;
        }
    }

    mask_to_spins(best_mask, n, out.spins);
    out.energy = best;
    out.best_mask = best_mask;
    out.violation = khz_ising_violation(m, out.spins, 0.0f);
    out.feasible = have && (best <= max_energy);
    return out;
}

KHZ_IsingResult khz_q_ising_verify(const char *generated_text, float max_energy)
{
    float M[KHZ_MAX_N][KHZ_MAX_N];
    KHZ_IsingModel model;
    khz_q_build_ngram_matrix(generated_text, M);
    khz_ising_from_matrix(&model, M, 1.0f, 1.0f);
    return khz_ising_solve_exact(&model, max_energy);
}

#ifdef KHZ_ISING_STANDALONE_TEST
#include <stdio.h>

int main(void)
{
    int pass = 0, fail = 0;

    {
        KHZ_IsingModel m;
        khz_ising_clear(&m);
        m.n = 2;
        khz_ising_set_couple(&m, 0, 1, 1.0f);
        KHZ_IsingResult r = khz_ising_solve_exact(&m, 0.0f);
        int ok = (r.spins[0] != r.spins[1]) && (r.energy <= -1.0f + 1e-6f);
        printf("[%s] T1 antiferro: s0=%d s1=%d H=%.4f\n",
               ok ? "PASS" : "FAIL", r.spins[0], r.spins[1], r.energy);
        ok ? pass++ : fail++;
    }

    {
        KHZ_IsingModel m;
        khz_ising_clear(&m);
        m.n = 1;
        khz_ising_set_field(&m, 0, -2.0f);
        KHZ_IsingResult r = khz_ising_solve_exact(&m, 0.0f);
        int ok = (r.spins[0] == 1);
        printf("[%s] T2 field: s0=%d H=%.4f\n",
               ok ? "PASS" : "FAIL", r.spins[0], r.energy);
        ok ? pass++ : fail++;
    }

    {
        KHZ_IsingResult r = khz_q_ising_verify("aaaaaaaaaaaaaaaa", 100.0f);
        int ok = r.feasible;
        printf("[%s] T3 text gate: H=%.4f viol=%.4f mask=%u\n",
               ok ? "PASS" : "FAIL", r.energy, r.violation, r.best_mask);
        ok ? pass++ : fail++;
    }

    printf("\nKHZ_Q Ising Smoke: %d passed, %d failed\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
#endif
