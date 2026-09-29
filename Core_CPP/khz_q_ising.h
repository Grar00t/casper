#ifndef KHZ_Q_ISING_H
#define KHZ_Q_ISING_H

#include <stdbool.h>
#include <stdint.h>
#include "khz_q_svd.h"

#define KHZ_ISING_N KHZ_MAX_N
#define KHZ_ISING_STATES (1u << KHZ_ISING_N)

typedef struct {
    float h[KHZ_ISING_N];
    float J[KHZ_ISING_N][KHZ_ISING_N];
    uint32_t n;
} KHZ_IsingModel;

typedef struct {
    int8_t spins[KHZ_ISING_N];
    float energy;
    float violation;
    uint32_t best_mask;
    bool feasible;
} KHZ_IsingResult;

void khz_ising_clear(KHZ_IsingModel *m);
void khz_ising_set_field(KHZ_IsingModel *m, int i, float h);
void khz_ising_set_couple(KHZ_IsingModel *m, int i, int j, float Jij);
void khz_ising_forbid_aligned(KHZ_IsingModel *m, int i, int j, float penalty);
float khz_ising_energy(const KHZ_IsingModel *m, const int8_t *spins);
float khz_ising_violation(const KHZ_IsingModel *m, const int8_t *spins, float j_thresh);
void khz_ising_from_matrix(KHZ_IsingModel *m,
                           float M[KHZ_MAX_N][KHZ_MAX_N],
                           float field_scale,
                           float couple_scale);
KHZ_IsingResult khz_ising_solve_exact(const KHZ_IsingModel *m, float max_energy);
KHZ_IsingResult khz_q_ising_verify(const char *generated_text, float max_energy);

#endif
