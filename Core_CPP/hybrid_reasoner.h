/*
 * hybrid_reasoner.h — NIYAH Symbolic Reasoning Engine
 *
 * Lightweight Prolog-like inference: terms, unification, backward chaining.
 * Zero external dependencies. C11 clean. C++17 compatible.
 *
 * Part of Casper_Engine hybrid neuro-symbolic system.
 */
#ifndef HYBRID_REASONER_H
#define HYBRID_REASONER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Constants
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
#define NIYAH_SYM_NAME_MAX   64
#define NIYAH_SYM_MAX_ARITY  16
#define NIYAH_SYM_MAX_DEPTH  256

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Term — the fundamental symbolic unit
 *
 * Three kinds:
 *   ATOM      — ground constant ("alice", "5", "true")
 *   VAR       — logical variable ("X", "Y") — uppercase first char
 *   COMPOUND  — functor + arguments: parent(alice, bob)
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
typedef enum {
    NIYAH_SYM_ATOM,
    NIYAH_SYM_VAR,
    NIYAH_SYM_COMPOUND
} NiyahSymTermKind;

typedef struct NiyahSymTerm {
    NiyahSymTermKind      kind;
    char                  name[NIYAH_SYM_NAME_MAX];
    struct NiyahSymTerm **args;      /* NULL for atoms/vars */
    uint32_t              arity;     /* 0 for atoms/vars   */
} NiyahSymTerm;

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Substitution — variable bindings from unification
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
typedef struct {
    char          var_name[NIYAH_SYM_NAME_MAX];
    NiyahSymTerm *binding;
} NiyahSymBinding;

typedef struct {
    NiyahSymBinding *bindings;
    uint32_t         count;
    uint32_t         capacity;
} NiyahSymSubst;

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Clause — Horn clause: head :- body1, body2, ...
 *
 * A fact is a clause with body_len == 0.
 * A rule is a clause with body_len > 0.
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
typedef struct {
    NiyahSymTerm  *head;
    NiyahSymTerm **body;
    uint32_t       body_len;
} NiyahSymClause;

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Knowledge Base — collection of clauses
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
typedef struct {
    NiyahSymClause *clauses;
    uint32_t        count;
    uint32_t        capacity;
} NiyahSymKB;

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Term construction / destruction
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
NiyahSymTerm *niyah_sym_atom(const char *name);
NiyahSymTerm *niyah_sym_var(const char *name);
NiyahSymTerm *niyah_sym_compound(const char *functor,
                                 NiyahSymTerm **args, uint32_t arity);
NiyahSymTerm *niyah_sym_term_clone(const NiyahSymTerm *t);
void          niyah_sym_term_free(NiyahSymTerm *t);
bool          niyah_sym_term_equal(const NiyahSymTerm *a,
                                   const NiyahSymTerm *b);

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Substitution
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
void          niyah_sym_subst_init(NiyahSymSubst *s);
void          niyah_sym_subst_free(NiyahSymSubst *s);
NiyahSymTerm *niyah_sym_subst_lookup(const NiyahSymSubst *s,
                                     const char *var_name);
void          niyah_sym_subst_bind(NiyahSymSubst *s,
                                   const char *var_name,
                                   NiyahSymTerm *binding);
NiyahSymTerm *niyah_sym_subst_apply(const NiyahSymSubst *s,
                                    const NiyahSymTerm *t);
NiyahSymSubst niyah_sym_subst_clone(const NiyahSymSubst *s);

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Unification (Robinson's algorithm)
 *
 * Returns true if a and b can be unified under substitution s.
 * On success, s is extended with new bindings.
 * On failure, s may be partially modified.
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
bool niyah_sym_unify(const NiyahSymTerm *a, const NiyahSymTerm *b,
                     NiyahSymSubst *s);

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Knowledge Base
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
NiyahSymKB *niyah_sym_kb_alloc(void);
void        niyah_sym_kb_add_fact(NiyahSymKB *kb, NiyahSymTerm *head);
void        niyah_sym_kb_add_rule(NiyahSymKB *kb, NiyahSymTerm *head,
                                  NiyahSymTerm **body, uint32_t body_len);
void        niyah_sym_kb_free(NiyahSymKB *kb);

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Backward chaining query
 *
 * Attempts to prove `goal` against kb using depth-first
 * backward chaining. If successful, `result` contains
 * variable bindings. max_depth prevents infinite recursion.
 *
 * Returns true if goal is provable.
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
bool niyah_sym_query(const NiyahSymKB *kb, const NiyahSymTerm *goal,
                     NiyahSymSubst *result, uint32_t max_depth);

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Smoke test — returns failed-assertion count (0 = all pass)
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
int niyah_sym_smoke(void);

#ifdef __cplusplus
}
#endif
#endif /* HYBRID_REASONER_H */
