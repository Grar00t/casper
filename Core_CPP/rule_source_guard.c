/* rule_source_guard.c — fail-closed validation around the legacy rule parser. */
#include "rule_source_guard.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

static const char *skip_space_no_nl(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r') ++p;
    return p;
}

static bool rule_line_shape_valid(const char *line, size_t len)
{
    static const char prefix[] = "rule:";
    size_t i;
    const char *p;
    const char *limit = line + len;

    if (len < sizeof(prefix) - 1u) return false;
    for (i = 0u; i < sizeof(prefix) - 1u; ++i) {
        if (tolower((unsigned char)line[i]) != prefix[i]) return false;
    }

    p = line + (sizeof(prefix) - 1u);
    while (p < limit && (*p == ' ' || *p == '\t' || *p == '\r')) ++p;
    if (p >= limit || *p != '"') return false;
    ++p;

    /* The legacy parser has no escaped-quote syntax: first quote closes body. */
    while (p < limit && *p != '"') ++p;
    if (p >= limit) return false;
    ++p;

    while (p < limit && (*p == ' ' || *p == '\t' || *p == '\r')) ++p;
    if (p == limit) return true;
    return (size_t)(limit - p) >= 2u && p[0] == '/' && p[1] == '/';
}

bool niyah_rule_source_guard(const char *source, const NiyahRuleKB *kb)
{
    const char *p;
    uint32_t declared = 0u;

    if (!source || !kb) return false;
    p = source;

    while (*p) {
        const char *line = skip_space_no_nl(p);
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);

        while (len > 0u && (line[len - 1u] == ' ' ||
                            line[len - 1u] == '\t' ||
                            line[len - 1u] == '\r')) {
            --len;
        }

        if (len == 0u) {
            /* blank */
        } else if (len >= 2u && line[0] == '/' && line[1] == '/') {
            /* comment */
        } else {
            if (!rule_line_shape_valid(line, len)) return false;
            ++declared;
        }

        if (!end) break;
        p = end + 1;
    }

    /* A supplied rules file with no effective rules is not a safety policy. */
    if (declared == 0u) return false;
    return kb->count == declared;
}
