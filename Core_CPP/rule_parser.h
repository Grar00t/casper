/*
 * rule_parser.h — NIYAH Rule Parser for .nrule files
 *
 * Human-readable rule format for symbolic verification of neural output.
 * Zero external dependencies. C11 clean. C++17 compatible.
 *
 * Rule syntax:
 *   rule: "IF question CONTAINS 'X' THEN answer = 'Y'"
 *   rule: "IF output CONTAINS 'X' THEN output = REJECTED"
 *   rule: "IF question CONTAINS 'X' THEN answer MUST contain 'Y'"
 *   rule: "ALWAYS output safe language"
 *   // comments and blank lines are ignored
 */
#ifndef RULE_PARSER_H
#define RULE_PARSER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Constants
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
#define NIYAH_RULE_FIELD_MAX   32
#define NIYAH_RULE_VALUE_MAX   512

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Rule types
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
typedef enum {
    NIYAH_RULE_IF_THEN,
    NIYAH_RULE_ALWAYS
} NiyahRuleKind;

typedef enum {
    NIYAH_RULE_CONTAINS,
    NIYAH_RULE_EQUALS,
    NIYAH_RULE_NOT_EQUALS
} NiyahRuleOp;

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Condition: field OP value
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
typedef struct {
    char         field[NIYAH_RULE_FIELD_MAX];
    NiyahRuleOp  op;
    char         value[NIYAH_RULE_VALUE_MAX];
} NiyahRuleCond;

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Action: what to do when condition matches
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
typedef struct {
    char  field[NIYAH_RULE_FIELD_MAX];
    char  value[NIYAH_RULE_VALUE_MAX];
    bool  is_rejection;     /* output = REJECTED */
    bool  must_contain;     /* answer MUST contain '...' */
    char  must_value[NIYAH_RULE_VALUE_MAX];
} NiyahRuleAction;

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Rule — linked list node
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
typedef struct NiyahRule {
    NiyahRuleKind    kind;
    NiyahRuleCond   *conditions;
    uint32_t         n_conditions;
    NiyahRuleAction  action;
    struct NiyahRule *next;
} NiyahRule;

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Knowledge base — linked list of rules
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
typedef struct {
    NiyahRule *head;
    uint32_t   count;
} NiyahRuleKB;

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * API
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

/* Load rules from a .nrule file. Returns NULL on I/O error. */
NiyahRuleKB *niyah_rule_load(const char *path);

/* Load rules from an in-memory string. */
NiyahRuleKB *niyah_rule_parse(const char *text);

/* Free all rules. */
void niyah_rule_free(NiyahRuleKB *kb);

/*
 * Check output text against all rules.
 *
 * question: the user's input prompt.
 * output:   the generated text to verify.
 *
 * Returns:
 *   NULL if no rule is violated.
 *   Static replacement string if a rule triggers a replacement.
 *   "REJECTED" if the output is rejected outright.
 *
 * The returned pointer is valid until the KB is freed.
 */
const char *niyah_rule_check(const NiyahRuleKB *kb,
                             const char *question,
                             const char *output);

/* Smoke test — returns failed-assertion count (0 = all pass) */
int niyah_rule_smoke(void);

#ifdef __cplusplus
}
#endif
#endif /* RULE_PARSER_H */
