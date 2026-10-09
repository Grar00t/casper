/* Exact, bounded query forms. Included before select_debt; no normalization. */
#define CHR_QUESTION_MAX_BYTES (2u * CHR_FIELD_MAX + 128u)
typedef struct { const char *prefix, *separator, *suffix; } ChrQuestionFrame;

static const ChrQuestionFrame question_frames[] = {
    {"", " ", ""}, {"", " ", "?"}, {"", " ", "؟"}, {"،", " ", "؟"},
    {"Did ", " repay ", "?"},
    {"هل سدد ", " دين ", "؟"}, {"هل سدد ", " دين ", "?"},
    {"ما حالة سداد دين ", " إلى ", "؟"}, {"ما حالة سداد دين ", " إلى ", "?"}
};

static int question_reserved_word(const char *word, size_t size) {
    static const char *const reserved[] = {
        "هل", "سدد", "دين", "ما", "حالة", "سداد", "إلى", "لم", "لن", "لا",
        "ليس", "ليست", "ربما", "إذا", "قد", "Did", "did", "repay", "not", "Not", "NOT"
    };
    size_t i;
    for (i = 0u; i < sizeof(reserved) / sizeof(reserved[0]); ++i)
        if (strlen(reserved[i]) == size && memcmp(word, reserved[i], size) == 0) return 1;
    return 0;
}

static int question_plain_name(const char *name) {
    const char *p = name, *word;
    if (strstr(name, "؟") || strstr(name, "،") || strstr(name, "؛")) return 0;
    do {
        word = p;
        while (*p && *p != ' ') {
            if ((unsigned char)*p < 0x20u || (unsigned char)*p == 0x7fu ||
                strchr("?!;:,.()[]{}<>\"'\\", *p)) return 0;
            ++p;
        }
        if (p == word || question_reserved_word(word, (size_t)(p - word))) return 0;
        if (*p == '\0') return 1;
        ++p;
    } while (*p);
    return 0;
}

static int question_frame_matches(const char *question, const char *borrower,
                                 const char *lender, const ChrQuestionFrame *frame) {
    char expected[CHR_QUESTION_MAX_BYTES];
    int n = snprintf(expected, sizeof(expected), "%s%s%s%s%s", frame->prefix, borrower,
                     frame->separator, lender, frame->suffix);
    return n >= 0 && (size_t)n < sizeof(expected) && strcmp(question, expected) == 0;
}

static int question_selects_pair(const char *question, const char *borrower, const char *lender) {
    static const ChrQuestionFrame exact = {"@settlement\t", "\t", ""};
    size_t i;
    if (!question || !borrower || !lender || !borrower[0] || !lender[0] ||
        strlen(borrower) >= CHR_FIELD_MAX || strlen(lender) >= CHR_FIELD_MAX ||
        strpbrk(borrower, "\t\r\n") || strpbrk(lender, "\t\r\n")) return 0;
    if (question_frame_matches(question, borrower, lender, &exact)) return 1;
    if (!question_plain_name(borrower) || !question_plain_name(lender)) return 0;
    for (i = 0u; i < sizeof(question_frames) / sizeof(question_frames[0]); ++i)
        if (question_frame_matches(question, borrower, lender, &question_frames[i])) return 1;
    return 0;
}
