/* Bounded literal grammar: three UTF-8 tokens and one terminal ASCII period.
 * Inclusion point: after field_copy, before parse_event_line. No inference. */
static int literal_space(uint8_t c) { return c == ' ' || c == '\t'; }

static int literal_token_valid(const uint8_t *text, size_t size) {
    size_t i;
    for (i = 0u; i < size; ++i) {
        uint8_t c = text[i];
        if (c < 0x20u || c == 0x7fu || c == '.' || c == ',' || c == ';' ||
            c == ':' || c == '!' || c == '?' || c == '@') return 0;
        if (i + 1u < size && c == 0xd8u &&
            (text[i + 1u] == 0x8cu || text[i + 1u] == 0x9bu || text[i + 1u] == 0x9fu)) return 0;
        if (i + 2u < size && c == 0xe2u && text[i + 1u] == 0x80u &&
            text[i + 2u] >= 0xa4u && text[i + 2u] <= 0xa9u) return 0;
        if (i + 2u < size && c == 0xe3u && text[i + 1u] == 0x80u && text[i + 2u] == 0x82u) return 0;
        if (i + 2u < size && c == 0xefu && text[i + 1u] == 0xbcu &&
            (text[i + 2u] == 0x8eu || text[i + 2u] == 0x81u || text[i + 2u] == 0x9fu)) return 0;
    }
    return 1;
}

static int literal_split(const uint8_t *line, size_t len, char fields[3][CHR_FIELD_MAX]) {
    size_t pos = 0u, begin[3], sizes[3], i;
    while (len > 0u && literal_space(line[len - 1u])) --len;
    if (len == 0u || line[len - 1u] != '.') return 1;
    --len;
    for (i = 0u; i < 3u; ++i) {
        while (pos < len && literal_space(line[pos])) ++pos;
        begin[i] = pos;
        while (pos < len && !literal_space(line[pos])) ++pos;
        sizes[i] = pos - begin[i];
        if (sizes[i] == 0u || !literal_token_valid(line + begin[i], sizes[i])) return 1;
    }
    while (pos < len && literal_space(line[pos])) ++pos;
    if (pos != len) return 1;
    for (i = 0u; i < 3u; ++i)
        if (field_copy(fields[i], (const char *)line + begin[i], sizes[i]) != 0) return -1;
    return 0;
}

static int parse_literal_line(const uint8_t *line, size_t len, ChrEvent *e) {
    char fields[3][CHR_FIELD_MAX]; int rc;
    if (!line || !e || !utf8_valid(line, len)) return -1;
    rc = literal_split(line, len, fields);
    if (rc != 0) return rc;
    memset(e, 0, sizeof(*e));
    memcpy(e->subject, fields[0], strlen(fields[0]) + 1u);
    memcpy(e->predicate, fields[1], strlen(fields[1]) + 1u);
    memcpy(e->object, fields[2], strlen(fields[2]) + 1u);
    e->amount = (ChrRat){0, 1};
    memcpy(e->currency, "-", sizeof("-"));
    memcpy(e->time_expr, "-", sizeof("-"));
    memcpy(e->modality, "LITERAL", sizeof("LITERAL"));
    memcpy(e->polarity, "POSITIVE", sizeof("POSITIVE"));
    memcpy(e->status, "ASSERTED", sizeof("ASSERTED"));
    return 0;
}
