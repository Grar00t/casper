/* Bounded original Arabic sentence grammar. No allocation or normalization.
 * Included after field_copy and parse_rat; event spans are assigned by caller. */
typedef struct { char *next; } ChrArabicCursor;

static int arabic_take(ChrArabicCursor *cursor, const char *separator,
                       char out[CHR_FIELD_MAX]) {
    char *end = strstr(cursor->next, separator); size_t size;
    if (!end) return 1;
    size = (size_t)(end - cursor->next);
    if (size == 0u) return 1;
    if (field_copy(out, cursor->next, size) != 0) return -1;
    cursor->next = end + strlen(separator);
    return 0;
}

static int arabic_reserved(const char *word, size_t size) {
    static const char *const words[] = {
        "لم", "لن", "لا", "ليس", "ليست", "قد", "ربما", "إذا", "إن", "إذ", "سوف",
        "قال", "قالت", "أن", "أنه", "بأنه", "من", "إلى", "مبلغ", "في", "اليوم",
        "وسيط", "عنه", "استلام", "وصول", "الحوالة", "سدد", "اقترض", "نفى", "أكد", "أقر",
        "حوّل", "حول", "ولكن", "لكن", "حسب", "لعل", "يقول", "يزعم", "زعم"
    };
    size_t i;
    for (i = 0u; i < sizeof(words) / sizeof(words[0]); ++i)
        if (strlen(words[i]) == size && memcmp(word, words[i], size) == 0) return 1;
    return 0;
}

static int arabic_name_character(const unsigned char **cursor) {
    const unsigned char *p = *cursor; unsigned int code;
    if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
        *cursor = p + 1u; return 1;
    }
    if (*p < 0xd8u || *p > 0xdbu || (p[1] & 0xc0u) != 0x80u) return 0;
    code = ((unsigned int)(p[0] & 0x1fu) << 6u) | (unsigned int)(p[1] & 0x3fu);
    if (!((code >= 0x621u && code <= 0x63au) ||
          (code >= 0x641u && code <= 0x65fu) || (code >= 0x670u && code <= 0x6d3u))) return 0;
    *cursor = p + 2u; return 1;
}

static int arabic_name_valid(const char *name) {
    const unsigned char *p = (const unsigned char *)name, *word = p;
    if (*p == '\0' || *p == ' ') return 0;
    while (*p) {
        if (*p == ' ') {
            if (p == word || arabic_reserved((const char *)word, (size_t)(p - word))) return 0;
            word = ++p;
        } else if (!arabic_name_character(&p)) return 0;
    }
    return p != word && !arabic_reserved((const char *)word, (size_t)(p - word));
}

static int arabic_time_valid(const char *time) {
    static const char *const ordinals[] = {"الأول", "الثاني", "الثالث", "الرابع", "الخامس",
        "السادس", "السابع", "الثامن", "التاسع", "العاشر"};
    const char prefix[] = "اليوم "; size_t i;
    if (strncmp(time, prefix, sizeof(prefix) - 1u) != 0) return 0;
    for (i = 0u; i < sizeof(ordinals) / sizeof(ordinals[0]); ++i)
        if (strcmp(time + sizeof(prefix) - 1u, ordinals[i]) == 0) return 1;
    return 0;
}

static int arabic_amount(const char *text, ChrRat *out) {
    char digits[CHR_FIELD_MAX]; size_t i = 0u, count = 0u, len = strlen(text);
    while (i < len) {
        unsigned char byte = (unsigned char)text[i++];
        if (count >= sizeof(digits) - 1u) return -1;
        if ((byte >= '0' && byte <= '9') || byte == '/') digits[count++] = (char)byte;
        else if (byte == 0xd9u && i < len && (unsigned char)text[i] >= 0xa0u &&
                 (unsigned char)text[i] <= 0xa9u) {
            digits[count++] = (char)('0' + ((unsigned char)text[i++] - 0xa0u));
        } else return -1;
    }
    digits[count] = '\0';
    return parse_rat(digits, out);
}

static int arabic_money(const char *text, ChrEvent *event) {
    const char *space = strchr(text, ' '); const char *currency; char number[CHR_FIELD_MAX];
    if (!space || space == text) return 1;
    currency = space + 1u;
    if (strcmp(currency, "ريال سعودي") == 0 || strcmp(currency, "SAR") == 0) currency = "SAR";
    else if (strcmp(currency, "دولار أمريكي") == 0 || strcmp(currency, "USD") == 0) currency = "USD";
    else return 1;
    if (field_copy(number, text, (size_t)(space - text)) != 0 ||
        arabic_amount(number, &event->amount) != 0) return -1;
    if (field_copy(event->currency, currency, strlen(currency)) != 0) return -1;
    event->amount_present = 1;
    return 0;
}

static int arabic_complete(ChrEvent *event, const char *predicate, const char *time) {
    if (!arabic_name_valid(event->subject) || !arabic_name_valid(event->object) ||
        !arabic_time_valid(time)) return 1;
    if (field_copy(event->predicate, predicate, strlen(predicate)) != 0 ||
        field_copy(event->time_expr, time, strlen(time)) != 0) return -1;
    return 0;
}

static int arabic_borrow(ChrArabicCursor *cursor, ChrEvent *event, int acknowledgment) {
    char money[CHR_FIELD_MAX]; int rc;
    rc = arabic_take(cursor, acknowledgment ? " بأنه اقترض من " : " من ", event->subject);
    if (rc != 0) return rc;
    rc = arabic_take(cursor, " مبلغ ", event->object);
    if (rc != 0) return rc;
    rc = arabic_take(cursor, " في ", money);
    if (rc != 0) return rc;
    rc = arabic_money(money, event);
    return rc != 0 ? rc : arabic_complete(event, "BORROWED_FROM", cursor->next);
}

static int arabic_receipt(ChrArabicCursor *cursor, ChrEvent *event) {
    char money[CHR_FIELD_MAX]; int rc;
    rc = arabic_take(cursor, " استلام مبلغ ", event->object);
    if (rc != 0) return rc;
    rc = arabic_take(cursor, " من ", money);
    if (rc != 0) return rc;
    rc = arabic_take(cursor, " في ", event->subject);
    if (rc != 0) return rc;
    rc = arabic_money(money, event);
    return rc != 0 ? rc : arabic_complete(event, "PAID_TO", cursor->next);
}

static int arabic_intermediary(ChrArabicCursor *cursor, ChrEvent *event) {
    int rc = arabic_take(cursor, " أن ", event->object);
    if (rc != 0) return rc;
    rc = arabic_take(cursor, " وسيط عنه في ", event->subject);
    return rc != 0 ? rc : arabic_complete(event, "INTERMEDIARY_FOR", cursor->next);
}

static int arabic_transfer(ChrArabicCursor *cursor, ChrEvent *event) {
    char money[CHR_FIELD_MAX], time[CHR_FIELD_MAX]; int rc;
    rc = arabic_take(cursor, " إلى ", event->subject);
    if (rc != 0) return rc;
    rc = arabic_take(cursor, " مبلغ ", event->object);
    if (rc != 0) return rc;
    rc = arabic_take(cursor, " في ", money);
    if (rc != 0) return rc;
    rc = arabic_take(cursor, " ولم يثبت وصول الحوالة", time);
    if (rc != 0 || cursor->next[0] != '\0') return rc != 0 ? rc : 1;
    rc = arabic_money(money, event);
    return rc != 0 ? rc : arabic_complete(event, "TRANSFER_TO", time);
}

static int arabic_prefix(const uint8_t *text, size_t len) {
    static const char *const prefixes[] = {"أقر ", "اقترض ", "أكد ", "نفى ", "حوّل "};
    size_t i;
    for (i = 0u; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i)
        if (len >= strlen(prefixes[i]) &&
            memcmp(text, prefixes[i], strlen(prefixes[i])) == 0) return (int)i;
    return -1;
}

static int arabic_dispatch(char *text, int kind, ChrEvent *event) {
    ChrArabicCursor cursor; char *space = strchr(text, ' ');
    if (!space) return 1;
    cursor.next = space + 1u;
    if (kind == 0 || kind == 1) return arabic_borrow(&cursor, event, kind == 0);
    if (kind == 4) return arabic_transfer(&cursor, event);
    if (strstr(cursor.next, " استلام مبلغ ")) return arabic_receipt(&cursor, event);
    if (kind == 2 || kind == 3) return arabic_intermediary(&cursor, event);
    return 1;
}

static int parse_arabic_line(const uint8_t *line, size_t len, ChrEvent *event) {
    char text[CHR_RECORD_BYTES + 1u]; int kind, rc;
    if (!line || !event || !utf8_valid(line, len)) return -1;
    kind = arabic_prefix(line, len);
    if (kind < 0) return 1;
    if (len == 0u || line[len - 1u] != '.') return 1;
    if (len > CHR_RECORD_BYTES) return -1;
    memcpy(text, line, len); text[len - 1u] = '\0';
    memset(event, 0, sizeof(*event)); event->amount = (ChrRat){0, 1};
    memcpy(event->currency, "-", sizeof("-"));
    memcpy(event->modality, kind == 4 ? "UNCERTAIN" : "ASSERTED", kind == 4 ? sizeof("UNCERTAIN") : sizeof("ASSERTED"));
    memcpy(event->polarity, kind == 3 ? "NEGATIVE" : "POSITIVE", sizeof("POSITIVE"));
    memcpy(event->status, kind == 4 ? "PENDING" : "CONFIRMED", kind == 4 ? sizeof("PENDING") : sizeof("CONFIRMED"));
    rc = arabic_dispatch(text, kind, event);
    return rc;
}
