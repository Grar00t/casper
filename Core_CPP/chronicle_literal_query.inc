/* Inclusion point: after add_evidence_json, before reason_json.
 * The result describes an exact source statement, never external truth. */
static int literal_query_fields(const char *question, char fields[3][CHR_FIELD_MAX]) {
    const char prefix[] = "@claim\t"; size_t pos = sizeof(prefix) - 1u, i;
    if (strncmp(question, prefix, pos) != 0) return -1;
    for (i = 0u; i < 3u; ++i) {
        size_t begin = pos;
        while (question[pos] && question[pos] != '\t') {
            if ((unsigned char)question[pos] < 0x20u || question[pos] == 0x7f) return -1;
            ++pos;
        }
        if (field_copy(fields[i], question + begin, pos - begin) != 0) return -1;
        if (i < 2u) { if (question[pos] != '\t') return -1; ++pos; }
    }
    return question[pos] == '\0' ? 0 : -1;
}

static int literal_matches(const ChrStore *store, const ChrEvent *event,
                           char fields[3][CHR_FIELD_MAX]) {
    return event->byte_start < store->source_size && store->source[event->byte_start] != '@' &&
           strcmp(event->modality, "LITERAL") == 0 &&
           strcmp(event->polarity, "POSITIVE") == 0 &&
           strcmp(event->status, "ASSERTED") == 0 &&
           strcmp(event->subject, fields[0]) == 0 &&
           strcmp(event->predicate, fields[1]) == 0 &&
           strcmp(event->object, fields[2]) == 0;
}

static int literal_json_field(ChrBuf *buf, const char *name, const char *value) {
    return buf_add(buf, ",\"") != 0 || buf_add(buf, name) != 0 ||
           buf_add(buf, "\":") != 0 ||
           buf_add_json(buf, (const uint8_t *)value, strlen(value)) != 0 ? -1 : 0;
}

static int literal_json_header(ChrBuf *buf, const char *question,
                               char fields[3][CHR_FIELD_MAX], int matched) {
    if (buf_add(buf, matched ? "{\"status\":\"EXACT_STATED\"" : "{\"status\":\"UNRESOLVED\"") != 0 ||
        literal_json_field(buf, "assessment", "LITERAL_RETRIEVAL") != 0 ||
        literal_json_field(buf, "question", question) != 0 ||
        literal_json_field(buf, "subject", fields[0]) != 0 ||
        literal_json_field(buf, "predicate", fields[1]) != 0 ||
        literal_json_field(buf, "object", fields[2]) != 0 ||
        literal_json_field(buf, "limits", "three-token literal retrieval; no inference or truth guarantee") != 0 ||
        buf_add(buf, ",\"evidence\":[") != 0) return -1;
    return 0;
}

static int literal_claim_json(const ChrStore *store, const char *question, char **out) {
    char fields[3][CHR_FIELD_MAX]; uint32_t i, count = 0u; ChrBuf buf = {0};
    if (!store || !question || !out || literal_query_fields(question, fields) != 0) return -1;
    for (i = 0u; i < store->event_count; ++i)
        if (literal_matches(store, &store->events[i], fields)) ++count;
    if (literal_json_header(&buf, question, fields, count != 0u) != 0) goto fail;
    count = 0u;
    for (i = 0u; i < store->event_count; ++i) if (literal_matches(store, &store->events[i], fields)) {
        if (add_evidence_json(&buf, store, &store->events[i], count != 0u) != 0) goto fail;
        ++count;
    }
    if (buf_add(&buf, "]}") != 0) goto fail;
    *out = buf.data; return 0;
fail:
    chr_free(buf.data); return -1;
}
