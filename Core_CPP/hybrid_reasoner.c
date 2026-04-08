/*
 * hybrid_reasoner.c — NIYAH Symbolic Reasoning Engine
 *
 * Lightweight Prolog-like inference engine:
 *   - Term representation (atoms, variables, compound terms)
 *   - Robinson's unification algorithm
 *   - Backward chaining with depth-first search
 *
 * Zero external dependencies. C11 clean.
 *
 * Standalone test:
 *   gcc -O2 -std=c11 -Wall -Wextra -Werror -Wcast-align
 *       -DSYM_STANDALONE_TEST hybrid_reasoner.c -o test_reasoner
 *   ./test_reasoner
 */

#include "hybrid_reasoner.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * §0  Utility
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

static void *sym_malloc(size_t n) {
    void *p = malloc(n);
    if (!p) { fprintf(stderr, "[niyah_sym] OOM: %zu bytes\n", n); abort(); }
    return p;
}

static void *sym_calloc(size_t count, size_t sz) {
    void *p = calloc(count, sz);
    if (!p) { fprintf(stderr, "[niyah_sym] OOM: %zu*%zu bytes\n", count, sz); abort(); }
    return p;
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * §1  Term construction / destruction
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

static NiyahSymTerm *term_new(NiyahSymTermKind kind, const char *name) {
    NiyahSymTerm *t = sym_calloc(1, sizeof(*t));
    t->kind = kind;
    if (name) {
        size_t len = strlen(name);
        if (len >= NIYAH_SYM_NAME_MAX) len = NIYAH_SYM_NAME_MAX - 1;
        memcpy(t->name, name, len);
        t->name[len] = '\0';
    }
    return t;
}

NiyahSymTerm *niyah_sym_atom(const char *name) {
    return term_new(NIYAH_SYM_ATOM, name);
}

NiyahSymTerm *niyah_sym_var(const char *name) {
    return term_new(NIYAH_SYM_VAR, name);
}

NiyahSymTerm *niyah_sym_compound(const char *functor,
                                 NiyahSymTerm **args, uint32_t arity)
{
    assert(arity <= NIYAH_SYM_MAX_ARITY);
    NiyahSymTerm *t = term_new(NIYAH_SYM_COMPOUND, functor);
    t->arity = arity;
    if (arity > 0) {
        t->args = sym_malloc(arity * sizeof(NiyahSymTerm *));
        for (uint32_t i = 0; i < arity; i++)
            t->args[i] = niyah_sym_term_clone(args[i]);
    }
    return t;
}

NiyahSymTerm *niyah_sym_term_clone(const NiyahSymTerm *t) {
    if (!t) return NULL;
    NiyahSymTerm *c = term_new(t->kind, t->name);
    c->arity = t->arity;
    if (t->arity > 0 && t->args) {
        c->args = sym_malloc(t->arity * sizeof(NiyahSymTerm *));
        for (uint32_t i = 0; i < t->arity; i++)
            c->args[i] = niyah_sym_term_clone(t->args[i]);
    }
    return c;
}

void niyah_sym_term_free(NiyahSymTerm *t) {
    if (!t) return;
    if (t->args) {
        for (uint32_t i = 0; i < t->arity; i++)
            niyah_sym_term_free(t->args[i]);
        free(t->args);
    }
    free(t);
}

bool niyah_sym_term_equal(const NiyahSymTerm *a, const NiyahSymTerm *b) {
    if (!a || !b) return a == b;
    if (a->kind != b->kind) return false;
    if (strcmp(a->name, b->name) != 0) return false;
    if (a->arity != b->arity) return false;
    for (uint32_t i = 0; i < a->arity; i++) {
        if (!niyah_sym_term_equal(a->args[i], b->args[i]))
            return false;
    }
    return true;
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * §2  Substitution
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

void niyah_sym_subst_init(NiyahSymSubst *s) {
    memset(s, 0, sizeof(*s));
}

void niyah_sym_subst_free(NiyahSymSubst *s) {
    if (!s) return;
    for (uint32_t i = 0; i < s->count; i++)
        niyah_sym_term_free(s->bindings[i].binding);
    free(s->bindings);
    s->bindings = NULL;
    s->count = s->capacity = 0;
}

NiyahSymTerm *niyah_sym_subst_lookup(const NiyahSymSubst *s,
                                     const char *var_name)
{
    for (uint32_t i = 0; i < s->count; i++) {
        if (strcmp(s->bindings[i].var_name, var_name) == 0)
            return s->bindings[i].binding;
    }
    return NULL;
}

void niyah_sym_subst_bind(NiyahSymSubst *s, const char *var_name,
                          NiyahSymTerm *binding)
{
    /* Grow if needed */
    if (s->count >= s->capacity) {
        uint32_t new_cap = s->capacity == 0 ? 8 : s->capacity * 2;
        s->bindings = realloc(s->bindings, new_cap * sizeof(NiyahSymBinding));
        if (!s->bindings) { fprintf(stderr, "[niyah_sym] OOM\n"); abort(); }
        s->capacity = new_cap;
    }
    NiyahSymBinding *b = &s->bindings[s->count++];
    size_t len = strlen(var_name);
    if (len >= NIYAH_SYM_NAME_MAX) len = NIYAH_SYM_NAME_MAX - 1;
    memcpy(b->var_name, var_name, len);
    b->var_name[len] = '\0';
    b->binding = niyah_sym_term_clone(binding);
}

/* Apply substitution: replace all variables with their bindings */
NiyahSymTerm *niyah_sym_subst_apply(const NiyahSymSubst *s,
                                    const NiyahSymTerm *t)
{
    if (!t) return NULL;

    if (t->kind == NIYAH_SYM_VAR) {
        NiyahSymTerm *bound = niyah_sym_subst_lookup(s, t->name);
        if (bound) {
            /* Recursively apply in case of chained bindings */
            return niyah_sym_subst_apply(s, bound);
        }
        return niyah_sym_term_clone(t);
    }

    if (t->kind == NIYAH_SYM_ATOM) {
        return niyah_sym_term_clone(t);
    }

    /* COMPOUND: apply substitution to each argument */
    NiyahSymTerm *result = sym_calloc(1, sizeof(*result));
    result->kind = NIYAH_SYM_COMPOUND;
    memcpy(result->name, t->name, NIYAH_SYM_NAME_MAX);
    result->arity = t->arity;
    if (t->arity > 0) {
        result->args = sym_malloc(t->arity * sizeof(NiyahSymTerm *));
        for (uint32_t i = 0; i < t->arity; i++)
            result->args[i] = niyah_sym_subst_apply(s, t->args[i]);
    }
    return result;
}

NiyahSymSubst niyah_sym_subst_clone(const NiyahSymSubst *s) {
    NiyahSymSubst c;
    niyah_sym_subst_init(&c);
    for (uint32_t i = 0; i < s->count; i++)
        niyah_sym_subst_bind(&c, s->bindings[i].var_name,
                             s->bindings[i].binding);
    return c;
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * §3  Unification (Robinson's algorithm)
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

/* Walk substitution chain to find deepest binding */
static const NiyahSymTerm *walk(const NiyahSymSubst *s,
                                const NiyahSymTerm *t)
{
    while (t && t->kind == NIYAH_SYM_VAR) {
        NiyahSymTerm *bound = niyah_sym_subst_lookup(s, t->name);
        if (!bound) break;
        t = bound;
    }
    return t;
}

/* Occurs check: does var_name appear in term t? */
static bool occurs_in(const NiyahSymSubst *s, const char *var_name,
                      const NiyahSymTerm *t)
{
    t = walk(s, t);
    if (!t) return false;
    if (t->kind == NIYAH_SYM_VAR)
        return strcmp(t->name, var_name) == 0;
    if (t->kind == NIYAH_SYM_ATOM)
        return false;
    for (uint32_t i = 0; i < t->arity; i++) {
        if (occurs_in(s, var_name, t->args[i]))
            return true;
    }
    return false;
}

bool niyah_sym_unify(const NiyahSymTerm *a, const NiyahSymTerm *b,
                     NiyahSymSubst *s)
{
    a = walk(s, a);
    b = walk(s, b);

    if (!a || !b) return false;

    /* Same variable */
    if (a->kind == NIYAH_SYM_VAR && b->kind == NIYAH_SYM_VAR
        && strcmp(a->name, b->name) == 0)
        return true;

    /* Bind variable to term (with occurs check) */
    if (a->kind == NIYAH_SYM_VAR) {
        if (occurs_in(s, a->name, b)) return false;
        niyah_sym_subst_bind(s, a->name, (NiyahSymTerm *)b);
        return true;
    }
    if (b->kind == NIYAH_SYM_VAR) {
        if (occurs_in(s, b->name, a)) return false;
        niyah_sym_subst_bind(s, b->name, (NiyahSymTerm *)a);
        return true;
    }

    /* Both atoms */
    if (a->kind == NIYAH_SYM_ATOM && b->kind == NIYAH_SYM_ATOM)
        return strcmp(a->name, b->name) == 0;

    /* Both compounds: same functor and arity, unify args pairwise */
    if (a->kind == NIYAH_SYM_COMPOUND && b->kind == NIYAH_SYM_COMPOUND) {
        if (strcmp(a->name, b->name) != 0) return false;
        if (a->arity != b->arity) return false;
        for (uint32_t i = 0; i < a->arity; i++) {
            if (!niyah_sym_unify(a->args[i], b->args[i], s))
                return false;
        }
        return true;
    }

    /* Mismatched kinds */
    return false;
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * §4  Knowledge Base
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

NiyahSymKB *niyah_sym_kb_alloc(void) {
    NiyahSymKB *kb = sym_calloc(1, sizeof(*kb));
    kb->capacity = 16;
    kb->clauses = sym_malloc(kb->capacity * sizeof(NiyahSymClause));
    return kb;
}

static void kb_grow(NiyahSymKB *kb) {
    if (kb->count < kb->capacity) return;
    kb->capacity *= 2;
    kb->clauses = realloc(kb->clauses, kb->capacity * sizeof(NiyahSymClause));
    if (!kb->clauses) { fprintf(stderr, "[niyah_sym] OOM\n"); abort(); }
}

void niyah_sym_kb_add_fact(NiyahSymKB *kb, NiyahSymTerm *head) {
    kb_grow(kb);
    NiyahSymClause *c = &kb->clauses[kb->count++];
    c->head = niyah_sym_term_clone(head);
    c->body = NULL;
    c->body_len = 0;
}

void niyah_sym_kb_add_rule(NiyahSymKB *kb, NiyahSymTerm *head,
                           NiyahSymTerm **body, uint32_t body_len)
{
    kb_grow(kb);
    NiyahSymClause *c = &kb->clauses[kb->count++];
    c->head = niyah_sym_term_clone(head);
    c->body_len = body_len;
    if (body_len > 0) {
        c->body = sym_malloc(body_len * sizeof(NiyahSymTerm *));
        for (uint32_t i = 0; i < body_len; i++)
            c->body[i] = niyah_sym_term_clone(body[i]);
    } else {
        c->body = NULL;
    }
}

void niyah_sym_kb_free(NiyahSymKB *kb) {
    if (!kb) return;
    for (uint32_t i = 0; i < kb->count; i++) {
        niyah_sym_term_free(kb->clauses[i].head);
        for (uint32_t j = 0; j < kb->clauses[i].body_len; j++)
            niyah_sym_term_free(kb->clauses[i].body[j]);
        free(kb->clauses[i].body);
    }
    free(kb->clauses);
    free(kb);
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * §5  Backward chaining
 *
 * To prove a goal:
 *   1. For each clause in KB whose head unifies with goal:
 *      a. If fact (no body): success with current substitution.
 *      b. If rule: recursively prove each body term.
 *   2. If no clause matches: fail.
 *
 * Variable renaming: each clause use gets unique variable names
 * to avoid capture (e.g., X → _X_0, _X_1, ...).
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

/* Rename variables in a term with a unique suffix */
static NiyahSymTerm *rename_vars(const NiyahSymTerm *t, uint32_t gen) {
    if (!t) return NULL;

    if (t->kind == NIYAH_SYM_VAR) {
        char new_name[NIYAH_SYM_NAME_MAX];
        /* Truncate base name to leave room for prefix + suffix */
        snprintf(new_name, sizeof(new_name), "_%.*s_%u",
                 (int)(NIYAH_SYM_NAME_MAX - 14), t->name, gen);
        return niyah_sym_var(new_name);
    }

    if (t->kind == NIYAH_SYM_ATOM)
        return niyah_sym_term_clone(t);

    /* COMPOUND: rename vars in each argument */
    NiyahSymTerm *result = sym_calloc(1, sizeof(*result));
    result->kind = NIYAH_SYM_COMPOUND;
    memcpy(result->name, t->name, NIYAH_SYM_NAME_MAX);
    result->arity = t->arity;
    result->args = sym_malloc(t->arity * sizeof(NiyahSymTerm *));
    for (uint32_t i = 0; i < t->arity; i++)
        result->args[i] = rename_vars(t->args[i], gen);
    return result;
}

/* Internal recursive prover */
static bool prove(const NiyahSymKB *kb, const NiyahSymTerm *goal,
                  NiyahSymSubst *s, uint32_t depth, uint32_t *gen)
{
    if (depth == 0) return false;

    /* Apply current substitution to goal */
    NiyahSymTerm *resolved = niyah_sym_subst_apply(s, goal);

    for (uint32_t ci = 0; ci < kb->count; ci++) {
        const NiyahSymClause *clause = &kb->clauses[ci];

        /* Rename clause variables to avoid capture */
        uint32_t this_gen = (*gen)++;
        NiyahSymTerm *rhead = rename_vars(clause->head, this_gen);

        /* Try to unify goal with clause head */
        NiyahSymSubst trial = niyah_sym_subst_clone(s);
        if (!niyah_sym_unify(resolved, rhead, &trial)) {
            niyah_sym_term_free(rhead);
            niyah_sym_subst_free(&trial);
            continue;
        }

        /* Fact: no body to prove */
        if (clause->body_len == 0) {
            niyah_sym_term_free(rhead);
            niyah_sym_subst_free(s);
            *s = trial;
            niyah_sym_term_free(resolved);
            return true;
        }

        /* Rule: prove each body goal */
        NiyahSymTerm **rbody = sym_malloc(clause->body_len * sizeof(NiyahSymTerm *));
        for (uint32_t i = 0; i < clause->body_len; i++)
            rbody[i] = rename_vars(clause->body[i], this_gen);

        bool all_proved = true;
        for (uint32_t i = 0; i < clause->body_len; i++) {
            if (!prove(kb, rbody[i], &trial, depth - 1, gen)) {
                all_proved = false;
                break;
            }
        }

        for (uint32_t i = 0; i < clause->body_len; i++)
            niyah_sym_term_free(rbody[i]);
        free(rbody);
        niyah_sym_term_free(rhead);

        if (all_proved) {
            niyah_sym_subst_free(s);
            *s = trial;
            niyah_sym_term_free(resolved);
            return true;
        }

        niyah_sym_subst_free(&trial);
    }

    niyah_sym_term_free(resolved);
    return false;
}

bool niyah_sym_query(const NiyahSymKB *kb, const NiyahSymTerm *goal,
                     NiyahSymSubst *result, uint32_t max_depth)
{
    if (max_depth == 0) max_depth = NIYAH_SYM_MAX_DEPTH;
    uint32_t gen = 0;
    return prove(kb, goal, result, max_depth, &gen);
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * §6  Smoke test
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

#define SYM_PASS(cond, label) do { \
    if (cond) { pass++; fprintf(stderr, "  [PASS] %s\n", label); } \
    else      { fail++; fprintf(stderr, "  [FAIL] %s\n", label); } \
} while(0)

int niyah_sym_smoke(void) {
    int pass = 0, fail = 0;

    fprintf(stderr, "\n+--------------------------------------+\n");
    fprintf(stderr, "|  NIYAH Symbolic Reasoner Smoke Test  |\n");
    fprintf(stderr, "+--------------------------------------+\n");

    /* §6.1 — Atom creation and equality */
    {
        NiyahSymTerm *a = niyah_sym_atom("hello");
        NiyahSymTerm *b = niyah_sym_atom("hello");
        NiyahSymTerm *c = niyah_sym_atom("world");
        SYM_PASS(niyah_sym_term_equal(a, b), "atom equality: same");
        SYM_PASS(!niyah_sym_term_equal(a, c), "atom equality: different");
        niyah_sym_term_free(a);
        niyah_sym_term_free(b);
        niyah_sym_term_free(c);
    }

    /* §6.2 — Variable creation */
    {
        NiyahSymTerm *x = niyah_sym_var("X");
        SYM_PASS(x->kind == NIYAH_SYM_VAR, "var creation");
        SYM_PASS(strcmp(x->name, "X") == 0, "var name");
        niyah_sym_term_free(x);
    }

    /* §6.3 — Compound term */
    {
        NiyahSymTerm *a = niyah_sym_atom("alice");
        NiyahSymTerm *b = niyah_sym_atom("bob");
        NiyahSymTerm *args[2] = { a, b };
        NiyahSymTerm *t = niyah_sym_compound("parent", args, 2);
        SYM_PASS(t->kind == NIYAH_SYM_COMPOUND, "compound kind");
        SYM_PASS(t->arity == 2, "compound arity");
        SYM_PASS(strcmp(t->name, "parent") == 0, "compound functor");
        niyah_sym_term_free(t);
        niyah_sym_term_free(a);
        niyah_sym_term_free(b);
    }

    /* §6.4 — Unification: var with atom */
    {
        NiyahSymTerm *x = niyah_sym_var("X");
        NiyahSymTerm *five = niyah_sym_atom("5");
        NiyahSymSubst s;
        niyah_sym_subst_init(&s);
        bool ok = niyah_sym_unify(x, five, &s);
        SYM_PASS(ok, "unify(X, 5) succeeds");
        NiyahSymTerm *bound = niyah_sym_subst_lookup(&s, "X");
        SYM_PASS(bound && strcmp(bound->name, "5") == 0,
                 "X bound to 5");
        niyah_sym_subst_free(&s);
        niyah_sym_term_free(x);
        niyah_sym_term_free(five);
    }

    /* §6.5 — Unification: two atoms (same) */
    {
        NiyahSymTerm *a = niyah_sym_atom("cat");
        NiyahSymTerm *b = niyah_sym_atom("cat");
        NiyahSymSubst s;
        niyah_sym_subst_init(&s);
        SYM_PASS(niyah_sym_unify(a, b, &s), "unify(cat, cat) succeeds");
        niyah_sym_subst_free(&s);
        niyah_sym_term_free(a);
        niyah_sym_term_free(b);
    }

    /* §6.6 — Unification: two atoms (different) */
    {
        NiyahSymTerm *a = niyah_sym_atom("cat");
        NiyahSymTerm *b = niyah_sym_atom("dog");
        NiyahSymSubst s;
        niyah_sym_subst_init(&s);
        SYM_PASS(!niyah_sym_unify(a, b, &s), "unify(cat, dog) fails");
        niyah_sym_subst_free(&s);
        niyah_sym_term_free(a);
        niyah_sym_term_free(b);
    }

    /* §6.7 — Unification: compound terms */
    {
        /* parent(X, bob) unifies with parent(alice, Y) */
        NiyahSymTerm *x = niyah_sym_var("X");
        NiyahSymTerm *bob1 = niyah_sym_atom("bob");
        NiyahSymTerm *args1[2] = { x, bob1 };
        NiyahSymTerm *t1 = niyah_sym_compound("parent", args1, 2);

        NiyahSymTerm *alice = niyah_sym_atom("alice");
        NiyahSymTerm *y = niyah_sym_var("Y");
        NiyahSymTerm *args2[2] = { alice, y };
        NiyahSymTerm *t2 = niyah_sym_compound("parent", args2, 2);

        NiyahSymSubst s;
        niyah_sym_subst_init(&s);
        bool ok = niyah_sym_unify(t1, t2, &s);
        SYM_PASS(ok, "unify compound parent(X,bob) = parent(alice,Y)");

        NiyahSymTerm *bx = niyah_sym_subst_lookup(&s, "X");
        NiyahSymTerm *by = niyah_sym_subst_lookup(&s, "Y");
        SYM_PASS(bx && strcmp(bx->name, "alice") == 0, "X = alice");
        SYM_PASS(by && strcmp(by->name, "bob") == 0, "Y = bob");

        niyah_sym_subst_free(&s);
        niyah_sym_term_free(t1);
        niyah_sym_term_free(t2);
        niyah_sym_term_free(x);
        niyah_sym_term_free(bob1);
        niyah_sym_term_free(alice);
        niyah_sym_term_free(y);
    }

    /* §6.8 — Occurs check */
    {
        /* X cannot unify with f(X) */
        NiyahSymTerm *x = niyah_sym_var("X");
        NiyahSymTerm *x2 = niyah_sym_var("X");
        NiyahSymTerm *args[1] = { x2 };
        NiyahSymTerm *fx = niyah_sym_compound("f", args, 1);

        NiyahSymSubst s;
        niyah_sym_subst_init(&s);
        SYM_PASS(!niyah_sym_unify(x, fx, &s), "occurs check: X != f(X)");
        niyah_sym_subst_free(&s);
        niyah_sym_term_free(x);
        niyah_sym_term_free(fx);
        niyah_sym_term_free(x2);
    }

    /* §6.9 — Knowledge base: simple fact query */
    {
        NiyahSymKB *kb = niyah_sym_kb_alloc();

        /* Fact: parent(alice, bob) */
        NiyahSymTerm *alice = niyah_sym_atom("alice");
        NiyahSymTerm *bob = niyah_sym_atom("bob");
        NiyahSymTerm *fargs[2] = { alice, bob };
        NiyahSymTerm *fact = niyah_sym_compound("parent", fargs, 2);
        niyah_sym_kb_add_fact(kb, fact);

        /* Query: parent(alice, X) */
        NiyahSymTerm *a2 = niyah_sym_atom("alice");
        NiyahSymTerm *qx = niyah_sym_var("X");
        NiyahSymTerm *qargs[2] = { a2, qx };
        NiyahSymTerm *query = niyah_sym_compound("parent", qargs, 2);

        NiyahSymSubst result;
        niyah_sym_subst_init(&result);
        bool found = niyah_sym_query(kb, query, &result, 0);
        SYM_PASS(found, "query parent(alice, X) succeeds");

        /* Check that X is bound to bob */
        NiyahSymTerm *applied = niyah_sym_subst_apply(&result, qx);
        SYM_PASS(applied && strcmp(applied->name, "bob") == 0,
                 "X = bob in query result");
        niyah_sym_term_free(applied);

        niyah_sym_subst_free(&result);
        niyah_sym_term_free(query);
        niyah_sym_term_free(fact);
        niyah_sym_term_free(alice);
        niyah_sym_term_free(bob);
        niyah_sym_term_free(a2);
        niyah_sym_term_free(qx);
        niyah_sym_kb_free(kb);
    }

    /* §6.10 — Backward chaining: transitive rule */
    {
        /*
         * Facts:
         *   parent(alice, bob)
         *   parent(bob, charlie)
         *
         * Rule:
         *   ancestor(X, Y) :- parent(X, Y)
         *   ancestor(X, Y) :- parent(X, Z), ancestor(Z, Y)
         *
         * Query: ancestor(alice, charlie) → should succeed
         */
        NiyahSymKB *kb = niyah_sym_kb_alloc();

        /* Facts */
        NiyahSymTerm *alice = niyah_sym_atom("alice");
        NiyahSymTerm *bob   = niyah_sym_atom("bob");
        NiyahSymTerm *charlie = niyah_sym_atom("charlie");

        NiyahSymTerm *f1args[2] = { alice, bob };
        NiyahSymTerm *f1 = niyah_sym_compound("parent", f1args, 2);
        niyah_sym_kb_add_fact(kb, f1);

        NiyahSymTerm *f2args[2] = { bob, charlie };
        NiyahSymTerm *f2 = niyah_sym_compound("parent", f2args, 2);
        niyah_sym_kb_add_fact(kb, f2);

        /* Rule 1: ancestor(X, Y) :- parent(X, Y) */
        {
            NiyahSymTerm *rx = niyah_sym_var("X");
            NiyahSymTerm *ry = niyah_sym_var("Y");
            NiyahSymTerm *hargs[2] = { rx, ry };
            NiyahSymTerm *head = niyah_sym_compound("ancestor", hargs, 2);

            NiyahSymTerm *bx = niyah_sym_var("X");
            NiyahSymTerm *by = niyah_sym_var("Y");
            NiyahSymTerm *bargs[2] = { bx, by };
            NiyahSymTerm *body_t = niyah_sym_compound("parent", bargs, 2);
            NiyahSymTerm *body[1] = { body_t };

            niyah_sym_kb_add_rule(kb, head, body, 1);

            niyah_sym_term_free(head);
            niyah_sym_term_free(body_t);
            niyah_sym_term_free(rx);
            niyah_sym_term_free(ry);
            niyah_sym_term_free(bx);
            niyah_sym_term_free(by);
        }

        /* Rule 2: ancestor(X, Y) :- parent(X, Z), ancestor(Z, Y) */
        {
            NiyahSymTerm *rx = niyah_sym_var("X");
            NiyahSymTerm *ry = niyah_sym_var("Y");
            NiyahSymTerm *hargs[2] = { rx, ry };
            NiyahSymTerm *head = niyah_sym_compound("ancestor", hargs, 2);

            NiyahSymTerm *bx = niyah_sym_var("X");
            NiyahSymTerm *bz = niyah_sym_var("Z");
            NiyahSymTerm *b1args[2] = { bx, bz };
            NiyahSymTerm *b1 = niyah_sym_compound("parent", b1args, 2);

            NiyahSymTerm *bz2 = niyah_sym_var("Z");
            NiyahSymTerm *by  = niyah_sym_var("Y");
            NiyahSymTerm *b2args[2] = { bz2, by };
            NiyahSymTerm *b2 = niyah_sym_compound("ancestor", b2args, 2);

            NiyahSymTerm *body[2] = { b1, b2 };
            niyah_sym_kb_add_rule(kb, head, body, 2);

            niyah_sym_term_free(head);
            niyah_sym_term_free(b1);
            niyah_sym_term_free(b2);
            niyah_sym_term_free(rx);
            niyah_sym_term_free(ry);
            niyah_sym_term_free(bx);
            niyah_sym_term_free(bz);
            niyah_sym_term_free(bz2);
            niyah_sym_term_free(by);
        }

        /* Query: ancestor(alice, charlie) */
        {
            NiyahSymTerm *qa = niyah_sym_atom("alice");
            NiyahSymTerm *qc = niyah_sym_atom("charlie");
            NiyahSymTerm *qargs[2] = { qa, qc };
            NiyahSymTerm *query = niyah_sym_compound("ancestor", qargs, 2);

            NiyahSymSubst result;
            niyah_sym_subst_init(&result);
            bool found = niyah_sym_query(kb, query, &result, 0);
            SYM_PASS(found, "ancestor(alice, charlie) via backward chaining");
            niyah_sym_subst_free(&result);
            niyah_sym_term_free(query);
            niyah_sym_term_free(qa);
            niyah_sym_term_free(qc);
        }

        /* Query: ancestor(alice, X) — should find bob or charlie */
        {
            NiyahSymTerm *qa = niyah_sym_atom("alice");
            NiyahSymTerm *qx = niyah_sym_var("X");
            NiyahSymTerm *qargs[2] = { qa, qx };
            NiyahSymTerm *query = niyah_sym_compound("ancestor", qargs, 2);

            NiyahSymSubst result;
            niyah_sym_subst_init(&result);
            bool found = niyah_sym_query(kb, query, &result, 0);
            SYM_PASS(found, "ancestor(alice, X) finds a descendant");

            if (found) {
                NiyahSymTerm *val = niyah_sym_subst_apply(&result, qx);
                fprintf(stderr, "  X = %s\n", val ? val->name : "(null)");
                niyah_sym_term_free(val);
            }

            niyah_sym_subst_free(&result);
            niyah_sym_term_free(query);
            niyah_sym_term_free(qa);
            niyah_sym_term_free(qx);
        }

        /* Negative query: ancestor(charlie, alice) should fail */
        {
            NiyahSymTerm *qc = niyah_sym_atom("charlie");
            NiyahSymTerm *qa = niyah_sym_atom("alice");
            NiyahSymTerm *qargs[2] = { qc, qa };
            NiyahSymTerm *query = niyah_sym_compound("ancestor", qargs, 2);

            NiyahSymSubst result;
            niyah_sym_subst_init(&result);
            bool found = niyah_sym_query(kb, query, &result, 0);
            SYM_PASS(!found, "ancestor(charlie, alice) correctly fails");
            niyah_sym_subst_free(&result);
            niyah_sym_term_free(query);
            niyah_sym_term_free(qc);
            niyah_sym_term_free(qa);
        }

        niyah_sym_term_free(f1);
        niyah_sym_term_free(f2);
        niyah_sym_term_free(alice);
        niyah_sym_term_free(bob);
        niyah_sym_term_free(charlie);
        niyah_sym_kb_free(kb);
    }

    /* §6.11 — Clone and apply */
    {
        NiyahSymTerm *x = niyah_sym_var("X");
        NiyahSymTerm *five = niyah_sym_atom("5");
        NiyahSymSubst s;
        niyah_sym_subst_init(&s);
        niyah_sym_subst_bind(&s, "X", five);

        NiyahSymTerm *applied = niyah_sym_subst_apply(&s, x);
        SYM_PASS(applied && applied->kind == NIYAH_SYM_ATOM
                 && strcmp(applied->name, "5") == 0,
                 "subst_apply(X) = 5");
        niyah_sym_term_free(applied);
        niyah_sym_subst_free(&s);
        niyah_sym_term_free(x);
        niyah_sym_term_free(five);
    }

    fprintf(stderr, "\n  Results: %d passed, %d failed\n\n", pass, fail);
    return fail;
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * §7  Standalone test entry point
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

#ifdef SYM_STANDALONE_TEST
int main(void) {
    int failed = niyah_sym_smoke();
    if (failed == 0)
        printf("SYMBOLIC SMOKE PASS - 0 failed\n");
    else
        printf("SYMBOLIC SMOKE FAIL - %d failed\n", failed);
    return failed;
}
#endif
