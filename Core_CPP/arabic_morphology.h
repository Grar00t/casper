/*
 * arabic_morphology.h — NIYAH Arabic Morphological Analysis Engine
 *
 * دمج الصرف العربي في النواة هو استرداد للهوية التقنية
 * Integrating Arabic morphology into the core reclaims technical identity.
 *
 * Features:
 *   - Tashkeel (diacritics) stripping
 *   - Unicode normalization (alef variants, taa marbuta, etc.)
 *   - Trilateral & quadrilateral root extraction
 *   - Arabic verb form (wazn) identification (Forms I–X)
 *   - Morpheme segmentation (prefix / stem / suffix)
 *   - Common Arabic patterns (أوزان)
 *
 * Zero external dependencies. C11 clean. C++17 compatible.
 */
#ifndef ARABIC_MORPHOLOGY_H
#define ARABIC_MORPHOLOGY_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Constants — Arabic Unicode ranges
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
#define NIYAH_AR_BLOCK_START   0x0600u
#define NIYAH_AR_BLOCK_END     0x06FFu
#define NIYAH_AR_TASHKEEL_MIN  0x064Bu
#define NIYAH_AR_TASHKEEL_MAX  0x0652u
#define NIYAH_AR_TATWEEL       0x0640u

/* Common letter codepoints */
#define NIYAH_AR_HAMZA         0x0621u
#define NIYAH_AR_ALEF_MADDA    0x0622u
#define NIYAH_AR_ALEF_HAMZA_A  0x0623u
#define NIYAH_AR_WAW_HAMZA     0x0624u
#define NIYAH_AR_ALEF_HAMZA_B  0x0625u
#define NIYAH_AR_YAA_HAMZA     0x0626u
#define NIYAH_AR_ALEF          0x0627u
#define NIYAH_AR_BAA           0x0628u
#define NIYAH_AR_TAA_MARBUTA   0x0629u
#define NIYAH_AR_TAA           0x062Au
#define NIYAH_AR_THAA          0x062Bu
#define NIYAH_AR_JEEM          0x062Cu
#define NIYAH_AR_HAA_SM        0x062Du   /* حاء */
#define NIYAH_AR_KHAA          0x062Eu
#define NIYAH_AR_DAL           0x062Fu
#define NIYAH_AR_DHAL          0x0630u
#define NIYAH_AR_RAA           0x0631u
#define NIYAH_AR_ZAY           0x0632u
#define NIYAH_AR_SEEN          0x0633u
#define NIYAH_AR_SHEEN         0x0634u
#define NIYAH_AR_SAD           0x0635u
#define NIYAH_AR_DAD           0x0636u
#define NIYAH_AR_TAA_M         0x0637u   /* طاء */
#define NIYAH_AR_DHAA          0x0638u
#define NIYAH_AR_AIN           0x0639u
#define NIYAH_AR_GHAIN         0x063Au
#define NIYAH_AR_FAA           0x0641u
#define NIYAH_AR_QAF           0x0642u
#define NIYAH_AR_KAF           0x0643u
#define NIYAH_AR_LAM           0x0644u
#define NIYAH_AR_MEEM          0x0645u
#define NIYAH_AR_NOON          0x0646u
#define NIYAH_AR_HAA           0x0647u   /* هاء */
#define NIYAH_AR_WAW           0x0648u
#define NIYAH_AR_ALEF_MAQSURA 0x0649u
#define NIYAH_AR_YAA           0x064Au

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Text normalization
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

/*
 * Strip tashkeel (diacritics) and tatweel (kashida) from Arabic text.
 * Writes result to dst (null-terminated). Returns bytes written (excl NUL).
 * dst must be at least as large as src.
 */
size_t niyah_ar_strip_tashkeel(const char *src, char *dst, size_t dst_size);

/*
 * Normalize Arabic text:
 *   - Strip tashkeel
 *   - Alef variants (أ إ آ) → ا
 *   - Alef maqsura (ى) → ي
 *   - Taa marbuta (ة) → ه
 *   - Remove tatweel (ـ)
 */
size_t niyah_ar_normalize(const char *src, char *dst, size_t dst_size);

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Root extraction
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

/* Verb form (وزن) */
typedef enum {
    NIYAH_AR_FORM_UNKNOWN = 0,
    NIYAH_AR_FORM_I   = 1,    /* فَعَلَ */
    NIYAH_AR_FORM_II  = 2,    /* فَعَّلَ */
    NIYAH_AR_FORM_III = 3,    /* فَاعَلَ */
    NIYAH_AR_FORM_IV  = 4,    /* أَفْعَلَ */
    NIYAH_AR_FORM_V   = 5,    /* تَفَعَّلَ */
    NIYAH_AR_FORM_VI  = 6,    /* تَفَاعَلَ */
    NIYAH_AR_FORM_VII = 7,    /* اِنْفَعَلَ */
    NIYAH_AR_FORM_VIII= 8,    /* اِفْتَعَلَ */
    NIYAH_AR_FORM_IX  = 9,    /* اِفْعَلَّ */
    NIYAH_AR_FORM_X   = 10,   /* اِسْتَفْعَلَ */
    NIYAH_AR_FORM_NOUN= 11,   /* Noun pattern */
} NiyahArForm;

/* Root extraction result */
typedef struct {
    uint32_t root[4];       /* Root letter codepoints (3 or 4) */
    uint32_t root_len;      /* Number of root letters (typically 3) */
    char     root_utf8[20]; /* UTF-8 encoded root string */
    NiyahArForm form;       /* Detected verb form */
    float    confidence;    /* 0.0–1.0 */
} NiyahArRoot;

/*
 * Extract the root from an Arabic word.
 * Returns true if a root was found, false otherwise.
 */
bool niyah_ar_extract_root(const char *word, NiyahArRoot *out);

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Morpheme segmentation
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

typedef struct {
    char prefix[32];        /* Extracted prefix (UTF-8) */
    char stem[64];          /* Core stem (UTF-8) */
    char suffix[32];        /* Extracted suffix (UTF-8) */
    char root[20];          /* Extracted root (UTF-8) */
    NiyahArForm form;       /* Detected pattern */
    uint32_t n_morphemes;   /* Total morpheme count */
} NiyahArMorphemes;

/*
 * Segment an Arabic word into prefix + stem + suffix.
 * Returns true if segmentation succeeded.
 */
bool niyah_ar_segment(const char *word, NiyahArMorphemes *out);

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Utilities
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

/* Check if codepoint is an Arabic letter (U+0621–U+064A) */
bool niyah_ar_is_letter(uint32_t cp);

/* Check if codepoint is a tashkeel mark (U+064B–U+0652) */
bool niyah_ar_is_tashkeel(uint32_t cp);

/* Decode one UTF-8 codepoint. Returns bytes consumed (0 on error). */
uint32_t niyah_ar_utf8_decode(const char *s, uint32_t *cp_out);

/* Encode one codepoint to UTF-8. Returns bytes written. dst must have 4+ bytes. */
uint32_t niyah_ar_utf8_encode(uint32_t cp, char *dst);

/* Get human-readable name for a verb form */
const char *niyah_ar_form_name(NiyahArForm f);

/* Smoke test — returns failed-assertion count (0 = all pass) */
int niyah_ar_smoke(void);

#ifdef __cplusplus
}
#endif
#endif /* ARABIC_MORPHOLOGY_H */
