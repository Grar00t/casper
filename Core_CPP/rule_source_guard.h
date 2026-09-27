#ifndef RULE_SOURCE_GUARD_H
#define RULE_SOURCE_GUARD_H

#include <stdbool.h>
#include "rule_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Validate that a supplied rule source is fully accounted for by the current
 * permissive parser. A supplied rules file must contain at least one rule;
 * every nonblank/noncomment line must be a rule declaration; and the number
 * of declarations must equal the number successfully parsed into the KB.
 *
 * This is a fail-closed boundary guard. It does not change the legacy parser
 * semantics used by existing in-memory callers.
 */
bool niyah_rule_source_guard(const char *source, const NiyahRuleKB *kb);

#ifdef __cplusplus
}
#endif
#endif /* RULE_SOURCE_GUARD_H */
