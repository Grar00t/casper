/*
 * rule_parser.c — NIYAH Rule Parser for .nrule files
 *
 * Recursive descent parser for the .nrule format.
 * Matches rules against question/output text.
 *
 * Zero external dependencies. C11 clean.
 *
 * Standalone test:
 *   gcc -O2 -std=c11 -Wall -Wextra -Werror -Wstrict-prototypes
 *       -Wcast-align -DRULE_STANDALONE_TEST rule_parser.c -o test_rules
 *   ./test_rules
 */

#include "rule_parser.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * §0  Utility
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

static void *rule_malloc(size_t n) {
    void *p = malloc(n);
    if (!p) { fprintf(stderr, "[niyah_rule] OOM: %zu bytes\n", n); abort(); }
    return p;
}

static void *rule_calloc(size_t count, size_t sz) {
    void *p = calloc(count, sz);
    if (!p) { fprintf(stderr, "[niyah_rule] OOM\n"); abort(); }
    return p;
}

/* Case-insensitive substring search */
static const char *ci_strstr(const char *haystack, const char *needle) {
    if (!needle[0]) return haystack;
    size_t nlen = strlen(needle);
    for (; *haystack; haystack++) {
        bool match = true;
        for (size_t i = 0; i < nlen; i++) {
            if (!haystack[i]) { match = false; break; }
            if (tolower((unsigned char)haystack[i]) !=
                tolower((unsigned char)needle[i])) {
                match = false; break;
            }
        }
        if (match) return haystack;
    }
    return NULL;
}

/* Case-insensitive string comparison for a prefix match */
static bool ci_starts_with(const char *s, const char *prefix) {
    while (*prefix) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix))
            return false;
        s++; prefix++;
    }
    return true;
}

/* Skip whitespace */
static const char *skip_ws(const char *p) {
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

/* Extract a quoted string: 'value' — returns pointer past closing quote.
 * Fills dst up to max_len-1 chars. */
static const char *extract_quoted(const char *p, char *dst, size_t max_len) {
    p = skip_ws(p);
    if (*p != '\'') return NULL;
    p++;
    size_t i = 0;
    while (*p && *p != '\'') {
        if (i < max_len - 1) dst[i++] = *p;
        p++;
    }
    dst[i] = '\0';
    if (*p == '\'') p++;
    return p;
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * §1  Rule parsing
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

/* Parse a field name: "question", "output", "answer" */
static const char *parse_field(const char *p, char *field, size_t max_len) {
    p = skip_ws(p);
    size_t i = 0;
    while (*p && isalpha((unsigned char)*p)) {
        if (i < max_len - 1) field[i++] = (char)tolower((unsigned char)*p);
        p++;
    }
    field[i] = '\0';
    return p;
}

/* Parse a condition: field OP 'value' */
static const char *parse_condition(const char *p, NiyahRuleCond *cond) {
    memset(cond, 0, sizeof(*cond));

    p = parse_field(p, cond->field, NIYAH_RULE_FIELD_MAX);
    p = skip_ws(p);

    /* Operator */
    if (ci_starts_with(p, "CONTAINS")) {
        cond->op = NIYAH_RULE_CONTAINS;
        p += 8;
    } else if (ci_starts_with(p, "!=")) {
        cond->op = NIYAH_RULE_NOT_EQUALS;
        p += 2;
    } else if (*p == '=') {
        cond->op = NIYAH_RULE_EQUALS;
        p++;
    } else {
        return NULL;
    }

    p = extract_quoted(p, cond->value, NIYAH_RULE_VALUE_MAX);
    return p;
}

/* Parse an action: field = 'value' | field MUST contain 'value' | REJECTED */
static const char *parse_action(const char *p, NiyahRuleAction *action) {
    memset(action, 0, sizeof(*action));
    p = skip_ws(p);

    /* Check for REJECTED */
    if (ci_starts_with(p, "REJECTED")) {
        action->is_rejection = true;
        return p + 8;
    }

    /* Parse field */
    p = parse_field(p, action->field, NIYAH_RULE_FIELD_MAX);
    p = skip_ws(p);

    /* Check for MUST contain */
    if (ci_starts_with(p, "MUST")) {
        p = skip_ws(p + 4);
        if (ci_starts_with(p, "contain")) {
            p += 7;
            if (*p == 's') p++;  /* "contains" or "contain" */
            action->must_contain = true;
            p = extract_quoted(p, action->must_value, NIYAH_RULE_VALUE_MAX);
            return p;
        }
        return NULL;
    }

    /* field = 'value' or field = REJECTED */
    if (*p == '=') {
        p = skip_ws(p + 1);
        if (ci_starts_with(p, "REJECTED")) {
            action->is_rejection = true;
            return p + 8;
        }
        p = extract_quoted(p, action->value, NIYAH_RULE_VALUE_MAX);
        return p;
    }

    /* Bare action text (for ALWAYS): just grab rest of line */
    size_t i = 0;
    while (*p && *p != '"' && *p != '\n') {
        if (i < NIYAH_RULE_VALUE_MAX - 1) action->value[i++] = *p;
        p++;
    }
    action->value[i] = '\0';
    /* Trim trailing whitespace */
    while (i > 0 && isspace((unsigned char)action->value[i-1]))
        action->value[--i] = '\0';
    return p;
}

/* Parse a single rule body (inside quotes):
 *   IF condition [AND condition]... THEN action
 *   ALWAYS action */
static NiyahRule *parse_rule_body(const char *body) {
    NiyahRule *rule = rule_calloc(1, sizeof(*rule));
    const char *p = skip_ws(body);

    if (ci_starts_with(p, "IF")) {
        rule->kind = NIYAH_RULE_IF_THEN;
        p = skip_ws(p + 2);

        /* Parse conditions (linked by AND) */
        NiyahRuleCond conds[16];
        uint32_t nc = 0;

        p = parse_condition(p, &conds[nc]);
        if (!p) { free(rule); return NULL; }
        nc++;

        while (p) {
            p = skip_ws(p);
            if (ci_starts_with(p, "AND")) {
                p = skip_ws(p + 3);
                if (nc >= 16) break;
                p = parse_condition(p, &conds[nc]);
                if (!p) break;
                nc++;
            } else {
                break;
            }
        }

        /* Expect THEN */
        p = skip_ws(p);
        if (!p || !ci_starts_with(p, "THEN")) {
            free(rule); return NULL;
        }
        p = skip_ws(p + 4);

        /* Copy conditions */
        rule->n_conditions = nc;
        rule->conditions = rule_malloc(nc * sizeof(NiyahRuleCond));
        memcpy(rule->conditions, conds, nc * sizeof(NiyahRuleCond));

        /* Parse action */
        p = parse_action(p, &rule->action);
        if (!p) { free(rule->conditions); free(rule); return NULL; }

    } else if (ci_starts_with(p, "ALWAYS")) {
        rule->kind = NIYAH_RULE_ALWAYS;
        p = skip_ws(p + 6);
        rule->n_conditions = 0;
        rule->conditions = NULL;
        p = parse_action(p, &rule->action);
        if (!p) { free(rule); return NULL; }

    } else {
        free(rule);
        return NULL;
    }

    return rule;
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * §2  File / string parsing
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

NiyahRuleKB *niyah_rule_parse(const char *text) {
    NiyahRuleKB *kb = rule_calloc(1, sizeof(*kb));
    const char *p = text;

    while (*p) {
        /* Skip whitespace and blank lines */
        p = skip_ws(p);
        if (!*p) break;

        /* Skip comment lines */
        if (*p == '/' && *(p+1) == '/') {
            while (*p && *p != '\n') p++;
            if (*p) p++;
            continue;
        }

        /* Look for rule: "..." */
        if (ci_starts_with(p, "rule:")) {
            p = skip_ws(p + 5);
            if (*p != '"') { while (*p && *p != '\n') p++; continue; }
            p++;  /* skip opening " */

            /* Extract rule body up to closing " */
            char body[2048];
            size_t bi = 0;
            while (*p && *p != '"') {
                if (bi < sizeof(body) - 1) body[bi++] = *p;
                p++;
            }
            body[bi] = '\0';
            if (*p == '"') p++;

            NiyahRule *rule = parse_rule_body(body);
            if (rule) {
                rule->next = kb->head;
                kb->head = rule;
                kb->count++;
            }
        } else {
            /* Skip unknown line */
            while (*p && *p != '\n') p++;
            if (*p) p++;
        }
    }

    return kb;
}

NiyahRuleKB *niyah_rule_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return NULL; }

    /* Read entire file */
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return rule_calloc(1, sizeof(NiyahRuleKB)); }

    char *buf = rule_malloc((size_t)len + 1);
    size_t nread = fread(buf, 1, (size_t)len, f);
    buf[nread] = '\0';
    fclose(f);

    NiyahRuleKB *kb = niyah_rule_parse(buf);
    free(buf);
    return kb;
}

void niyah_rule_free(NiyahRuleKB *kb) {
    if (!kb) return;
    NiyahRule *r = kb->head;
    while (r) {
        NiyahRule *next = r->next;
        free(r->conditions);
        free(r);
        r = next;
    }
    free(kb);
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * §3  Rule checking
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

/* Get the text corresponding to a field name */
static const char *get_field_text(const char *field,
                                  const char *question,
                                  const char *output)
{
    if (strcmp(field, "question") == 0) return question;
    if (strcmp(field, "output")   == 0) return output;
    if (strcmp(field, "answer")   == 0) return output;
    return NULL;
}

/* Check a single condition */
static bool check_condition(const NiyahRuleCond *cond,
                            const char *question,
                            const char *output)
{
    const char *text = get_field_text(cond->field, question, output);
    if (!text) return false;

    switch (cond->op) {
    case NIYAH_RULE_CONTAINS:
        return ci_strstr(text, cond->value) != NULL;
    case NIYAH_RULE_EQUALS:
        return strcmp(text, cond->value) == 0;
    case NIYAH_RULE_NOT_EQUALS:
        return strcmp(text, cond->value) != 0;
    }
    return false;
}

/* Static buffer for rejection string */
static const char REJECTED[] = "REJECTED";

const char *niyah_rule_check(const NiyahRuleKB *kb,
                             const char *question,
                             const char *output)
{
    if (!kb || !output) return NULL;
    if (!question) question = "";

    for (const NiyahRule *r = kb->head; r; r = r->next) {
        bool triggered = false;

        if (r->kind == NIYAH_RULE_ALWAYS) {
            triggered = true;
        } else {
            /* IF-THEN: all conditions must match */
            triggered = true;
            for (uint32_t i = 0; i < r->n_conditions; i++) {
                if (!check_condition(&r->conditions[i], question, output)) {
                    triggered = false;
                    break;
                }
            }
        }

        if (!triggered) continue;

        /* Action applies */
        if (r->action.is_rejection)
            return REJECTED;

        if (r->action.must_contain) {
            /* Check if output MUST contain the required text */
            const char *text = get_field_text(
                r->action.field[0] ? r->action.field : "output",
                question, output);
            if (text && !ci_strstr(text, r->action.must_value))
                return REJECTED;  /* violated: required text not found */
        }

        if (r->action.value[0]) {
            /* Replacement action: check if the output already matches */
            /* If not, the rule wants to replace/override the output */
            const char *text = get_field_text(
                r->action.field[0] ? r->action.field : "output",
                question, output);
            if (text && strcmp(text, r->action.value) != 0)
                return r->action.value;
        }
    }

    return NULL; /* all rules pass */
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * §4  Smoke test
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

#define RULE_PASS(cond, label) do { \
    if (cond) { pass++; fprintf(stderr, "  [PASS] %s\n", label); } \
    else      { fail++; fprintf(stderr, "  [FAIL] %s\n", label); } \
} while(0)

int niyah_rule_smoke(void) {
    int pass = 0, fail = 0;

    fprintf(stderr, "\n+--------------------------------------+\n");
    fprintf(stderr, "|  NIYAH Rule Parser Smoke Test        |\n");
    fprintf(stderr, "+--------------------------------------+\n");

    /* §4.1 — Parse basic IF-THEN rule */
    {
        const char *src =
            "rule: \"IF question CONTAINS 'cure for cancer' "
            "THEN answer MUST contain 'consult doctor'\"\n";
        NiyahRuleKB *kb = niyah_rule_parse(src);
        RULE_PASS(kb != NULL, "parse returns non-null");
        RULE_PASS(kb->count == 1, "parsed 1 rule");
        if (kb->head) {
            RULE_PASS(kb->head->kind == NIYAH_RULE_IF_THEN, "kind = IF_THEN");
            RULE_PASS(kb->head->n_conditions == 1, "1 condition");
            RULE_PASS(kb->head->action.must_contain, "action is MUST contain");
        }
        niyah_rule_free(kb);
    }

    /* §4.2 — Parse REJECTED rule */
    {
        const char *src =
            "rule: \"IF output CONTAINS 'vaccine causes autism' "
            "THEN output = REJECTED\"\n";
        NiyahRuleKB *kb = niyah_rule_parse(src);
        RULE_PASS(kb != NULL && kb->count == 1, "parse REJECTED rule");
        if (kb->head) {
            RULE_PASS(kb->head->action.is_rejection, "action is rejection");
        }
        niyah_rule_free(kb);
    }

    /* §4.3 — Parse ALWAYS rule */
    {
        const char *src =
            "rule: \"ALWAYS refuse to generate personal phone numbers\"\n";
        NiyahRuleKB *kb = niyah_rule_parse(src);
        RULE_PASS(kb != NULL && kb->count == 1, "parse ALWAYS rule");
        if (kb->head) {
            RULE_PASS(kb->head->kind == NIYAH_RULE_ALWAYS, "kind = ALWAYS");
        }
        niyah_rule_free(kb);
    }

    /* §4.4 — Parse multiple rules with comments */
    {
        const char *src =
            "// Medical rules\n"
            "rule: \"IF question CONTAINS 'explosive' "
            "THEN answer = 'I cannot answer that'\"\n"
            "\n"
            "// Safety\n"
            "rule: \"IF output CONTAINS 'hack into' "
            "THEN output = REJECTED\"\n"
            "rule: \"ALWAYS output safe language\"\n";
        NiyahRuleKB *kb = niyah_rule_parse(src);
        RULE_PASS(kb != NULL && kb->count == 3, "parse 3 rules with comments");
        niyah_rule_free(kb);
    }

    /* §4.5 — Check: CONTAINS match triggers replacement */
    {
        const char *src =
            "rule: \"IF question CONTAINS 'how to make explosive' "
            "THEN answer = 'I cannot answer that'\"\n";
        NiyahRuleKB *kb = niyah_rule_parse(src);
        const char *result;

        result = niyah_rule_check(kb, "how to make explosive devices", "here's how");
        RULE_PASS(result != NULL, "CONTAINS trigger fires");
        RULE_PASS(result && strcmp(result, "I cannot answer that") == 0,
                  "replacement text correct");

        result = niyah_rule_check(kb, "what is the weather", "it's sunny");
        RULE_PASS(result == NULL, "non-matching question passes");
        niyah_rule_free(kb);
    }

    /* §4.6 — Check: REJECTED rule */
    {
        const char *src =
            "rule: \"IF output CONTAINS 'vaccine causes autism' "
            "THEN output = REJECTED\"\n";
        NiyahRuleKB *kb = niyah_rule_parse(src);

        const char *result = niyah_rule_check(kb, "tell me about vaccines",
                                              "the vaccine causes autism myth");
        RULE_PASS(result != NULL && strcmp(result, "REJECTED") == 0,
                  "REJECTED output detected");

        result = niyah_rule_check(kb, "tell me about vaccines",
                                  "vaccines are safe and effective");
        RULE_PASS(result == NULL, "safe output passes");
        niyah_rule_free(kb);
    }

    /* §4.7 — Check: MUST contain rule */
    {
        const char *src =
            "rule: \"IF question CONTAINS 'cure for cancer' "
            "THEN answer MUST contain 'consult doctor'\"\n";
        NiyahRuleKB *kb = niyah_rule_parse(src);

        const char *result;
        result = niyah_rule_check(kb, "what is the cure for cancer",
                                  "drink bleach");
        RULE_PASS(result != NULL && strcmp(result, "REJECTED") == 0,
                  "MUST contain violation → REJECTED");

        result = niyah_rule_check(kb, "what is the cure for cancer",
                                  "please consult doctor for advice");
        RULE_PASS(result == NULL, "MUST contain satisfied → passes");
        niyah_rule_free(kb);
    }

    /* §4.8 — Check: case-insensitive matching */
    {
        const char *src =
            "rule: \"IF question CONTAINS 'HACK' "
            "THEN answer = 'I cannot help with that'\"\n";
        NiyahRuleKB *kb = niyah_rule_parse(src);

        const char *result = niyah_rule_check(kb, "how to hack wifi",
                                              "sure thing");
        RULE_PASS(result != NULL, "case-insensitive CONTAINS works");
        niyah_rule_free(kb);
    }

    /* §4.9 — Check: multiple conditions with AND */
    {
        const char *src =
            "rule: \"IF question CONTAINS 'password' AND "
            "question CONTAINS 'steal' THEN answer = REJECTED\"\n";
        NiyahRuleKB *kb = niyah_rule_parse(src);

        const char *result;
        result = niyah_rule_check(kb, "how to steal someone's password",
                                  "here's how");
        RULE_PASS(result != NULL && strcmp(result, "REJECTED") == 0,
                  "AND conditions both match → REJECTED");

        result = niyah_rule_check(kb, "how to reset my password",
                                  "go to settings");
        RULE_PASS(result == NULL, "AND: only one condition matches → passes");
        niyah_rule_free(kb);
    }

    /* §4.10 — Empty KB: everything passes */
    {
        NiyahRuleKB *kb = niyah_rule_parse("");
        RULE_PASS(kb != NULL && kb->count == 0, "empty KB parses ok");
        const char *result = niyah_rule_check(kb, "hello", "world");
        RULE_PASS(result == NULL, "empty KB: all output passes");
        niyah_rule_free(kb);
    }

    fprintf(stderr, "\n  Results: %d passed, %d failed\n\n", pass, fail);
    return fail;
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * §5  Standalone test entry point
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

#ifdef RULE_STANDALONE_TEST
int main(void) {
    int failed = niyah_rule_smoke();
    if (failed == 0)
        printf("RULE SMOKE PASS - 0 failed\n");
    else
        printf("RULE SMOKE FAIL - %d failed\n", failed);
    return failed;
}
#endif
