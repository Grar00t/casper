/*
 * constraint_solver.h — NIYAH Linear Constraint Solver
 *
 * Simplex-inspired solver for small systems of linear inequalities
 * over rational numbers. Designed for symbolic reasoning constraints.
 *
 * Zero external dependencies. C11 clean. C++17 compatible.
 */
#ifndef CONSTRAINT_SOLVER_H
#define CONSTRAINT_SOLVER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Constants
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
#define NIYAH_CSP_MAX_VARS        32
#define NIYAH_CSP_MAX_CONSTRAINTS 64

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Rational number — exact arithmetic, no FP errors
 *
 * Invariant: den > 0, gcd(|num|, den) == 1
 * Zero is represented as {0, 1}.
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
typedef struct {
    int64_t num;
    int64_t den;
} NiyahCspRat;

/* Rational arithmetic */
NiyahCspRat niyah_csp_rat(int64_t num, int64_t den);
NiyahCspRat niyah_csp_rat_add(NiyahCspRat a, NiyahCspRat b);
NiyahCspRat niyah_csp_rat_sub(NiyahCspRat a, NiyahCspRat b);
NiyahCspRat niyah_csp_rat_mul(NiyahCspRat a, NiyahCspRat b);
NiyahCspRat niyah_csp_rat_div(NiyahCspRat a, NiyahCspRat b);
NiyahCspRat niyah_csp_rat_neg(NiyahCspRat a);
int         niyah_csp_rat_cmp(NiyahCspRat a, NiyahCspRat b);
bool        niyah_csp_rat_eq(NiyahCspRat a, NiyahCspRat b);
double      niyah_csp_rat_to_double(NiyahCspRat r);

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Constraint: sum(coeff[i] * var[i]) op bound
 *
 * Example: 2*x + 3*y <= 10
 *   coeffs = [2, 3], var_ids = [0, 1], n_terms = 2
 *   op = NIYAH_CSP_LE, bound = 10
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
typedef enum {
    NIYAH_CSP_LE,   /* <= */
    NIYAH_CSP_GE,   /* >= */
    NIYAH_CSP_EQ    /* == */
} NiyahCspOp;

typedef struct {
    NiyahCspRat coeffs[NIYAH_CSP_MAX_VARS];
    uint32_t    var_ids[NIYAH_CSP_MAX_VARS];
    uint32_t    n_terms;
    NiyahCspOp  op;
    NiyahCspRat bound;
} NiyahCspConstraint;

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Constraint system
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
typedef struct {
    NiyahCspConstraint constraints[NIYAH_CSP_MAX_CONSTRAINTS];
    uint32_t count;
    uint32_t n_vars;
} NiyahCspSystem;

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * API
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
void niyah_csp_init(NiyahCspSystem *sys, uint32_t n_vars);
bool niyah_csp_add(NiyahCspSystem *sys, NiyahCspConstraint c);

/* Check if the system is satisfiable */
bool niyah_csp_feasible(const NiyahCspSystem *sys);

/* Find a feasible solution. values[0..n_vars-1] filled on success. */
bool niyah_csp_solve(const NiyahCspSystem *sys, NiyahCspRat *values);

/* Smoke test */
int niyah_csp_smoke(void);

#ifdef __cplusplus
}
#endif
#endif /* CONSTRAINT_SOLVER_H */
