/* Native read-only GGUF mapping; metadata/tensors are NOT decoded. */
#include "niyah_gguf_map.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static uint32_t le32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t le64(const unsigned char *p) {
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4u) << 32);
}
void niyah_gguf_map_close(NiyahGGUFMap *m) {
    if (!m) return;
#ifdef _WIN32
    if (m->bytes) (void)UnmapViewOfFile(m->bytes);
    if (m->map_handle) (void)CloseHandle((HANDLE)m->map_handle);
    if (m->file_handle) (void)CloseHandle((HANDLE)m->file_handle);
#else
    if (m->bytes) (void)munmap((void *)m->bytes, (size_t)m->file_size);
    if (m->bytes && m->fd >= 0) (void)close(m->fd);
#endif
    memset(m, 0, sizeof(*m));
}
int niyah_gguf_map_open(NiyahGGUFMap *m, const char *path) {
    uint64_t n;
    if (!m || !path || !path[0]) return -1;
    memset(m, 0, sizeof(*m));
#ifdef _WIN32
    {
        int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                        path, -1, NULL, 0);
        wchar_t *wide;
        HANDLE f, fm;
        LARGE_INTEGER size;
        const void *view;
        if (count <= 0 || (size_t)count > SIZE_MAX / sizeof(wchar_t)) return -2;
        wide = (wchar_t *)calloc((size_t)count, sizeof(wchar_t));
        if (!wide) return -2;
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                 path, -1, wide, count)) { free(wide); return -2; }
        f = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        free(wide);
        if (f == INVALID_HANDLE_VALUE) return -2;
        if (!GetFileSizeEx(f, &size) || size.QuadPart < 24 ||
            (uint64_t)size.QuadPart > (uint64_t)SIZE_MAX) {
            (void)CloseHandle(f); return -3;
        }
        fm = CreateFileMappingW(f, NULL, PAGE_READONLY, 0, 0, NULL);
        if (!fm) { (void)CloseHandle(f); return -2; }
        view = MapViewOfFile(fm, FILE_MAP_READ, 0, 0, 0);
        if (!view) { (void)CloseHandle(fm); (void)CloseHandle(f); return -2; }
        m->file_handle = f;
        m->map_handle = fm;
        m->bytes = (const unsigned char *)view;
        m->file_size = (uint64_t)size.QuadPart;
    }
#else
    {
        struct stat st;
        void *view;
        int fd = open(path, O_RDONLY);
        if (fd < 0) return -2;
        if (fstat(fd, &st) != 0 || st.st_size < 24 ||
            (uint64_t)st.st_size > (uint64_t)SIZE_MAX) {
            (void)close(fd); return -3;
        }
        view = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (view == MAP_FAILED) { (void)close(fd); return -2; }
        m->fd = fd;
        m->bytes = (const unsigned char *)view;
        m->file_size = (uint64_t)st.st_size;
    }
#endif
    n = m->file_size;
    if (memcmp(m->bytes, "GGUF", 4u) != 0 ||
        (le32(m->bytes + 4u) != 2u && le32(m->bytes + 4u) != 3u) ||
        le64(m->bytes + 8u) > n / 24u ||
        le64(m->bytes + 16u) > n / 12u) {
        niyah_gguf_map_close(m);
        return -4;
    }
    m->version = le32(m->bytes + 4u);
    m->tensor_count = le64(m->bytes + 8u);
    m->metadata_count = le64(m->bytes + 16u);
    return 0;
}
const unsigned char *niyah_gguf_map_range(const NiyahGGUFMap *m,
                                          uint64_t offset, uint64_t length) {
    if (!m || !m->bytes || offset > m->file_size ||
        length > m->file_size - offset ||
        offset > (uint64_t)SIZE_MAX) return NULL;
    return m->bytes + (size_t)offset;
}
int niyah_gguf_map_smoke(void) {
    static const char path[] = "build/gguf_map_smoke.gguf";
    unsigned char header[24] = {'G','G','U','F',3,0,0,0};
    NiyahGGUFMap m = {0};
    FILE *f;
    int failures = 0;
    f = fopen(path, "wb");
    if (!f) return 1;
    if (fwrite(header, 1u, sizeof(header), f) != sizeof(header)) ++failures;
    if (fclose(f) != 0) ++failures;
    if (failures == 0) {
        if (niyah_gguf_map_open(&m, path) != 0) ++failures;
        else {
            if (m.version != 3u || m.file_size != 24u ||
                m.tensor_count != 0u || m.metadata_count != 0u) ++failures;
            if (niyah_gguf_map_range(&m, 23u, 1u) != m.bytes + 23u) ++failures;
            if (niyah_gguf_map_range(&m, 24u, 1u) != NULL) ++failures;
            niyah_gguf_map_close(&m);
        }
    }
    header[4] = 1u;
    f = fopen(path, "wb");
    if (!f) ++failures;
    else {
        if (fwrite(header, 1u, sizeof(header), f) != sizeof(header)) ++failures;
        if (fclose(f) != 0) ++failures;
        if (niyah_gguf_map_open(&m, path) == 0) {
            ++failures; niyah_gguf_map_close(&m);
        }
    }
    if (remove(path) != 0) ++failures;
    return failures;
}
