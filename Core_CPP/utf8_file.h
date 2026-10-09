#ifndef NIYAH_UTF8_FILE_H
#define NIYAH_UTF8_FILE_H

#include <stdio.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <errno.h>
#include <wchar.h>

/* Stack-only boundary buffers; input and stored paths remain exact UTF-8. */
#define NIYAH_PATH_UTF16_UNITS 32768
#define NIYAH_MODE_UTF16_UNITS 8

static inline int niyah_to_utf16(const char *text, wchar_t *wide, int capacity) {
    if (!text) { errno = EINVAL; return -1; }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, capacity) == 0) {
        errno = GetLastError() == ERROR_INSUFFICIENT_BUFFER ? ENAMETOOLONG : EINVAL;
        return -1;
    }
    return 0;
}

static inline FILE *niyah_fopen_utf8(const char *path, const char *mode) {
    wchar_t wide_path[NIYAH_PATH_UTF16_UNITS], wide_mode[NIYAH_MODE_UTF16_UNITS];
    if (niyah_to_utf16(path, wide_path, NIYAH_PATH_UTF16_UNITS) != 0 ||
        niyah_to_utf16(mode, wide_mode, NIYAH_MODE_UTF16_UNITS) != 0) return NULL;
    return _wfopen(wide_path, wide_mode);
}

static inline int niyah_remove_utf8(const char *path) {
    wchar_t wide_path[NIYAH_PATH_UTF16_UNITS];
    if (niyah_to_utf16(path, wide_path, NIYAH_PATH_UTF16_UNITS) != 0) return -1;
    return _wremove(wide_path);
}
#else
static inline FILE *niyah_fopen_utf8(const char *path, const char *mode) {
    return fopen(path, mode);
}

static inline int niyah_remove_utf8(const char *path) { return remove(path); }
#endif

#endif
