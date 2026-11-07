/* Read-only mapped GGUF container inspection (not GGUF tensor inference). */
#ifndef NIYAH_GGUF_MAP_H
#define NIYAH_GGUF_MAP_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const unsigned char *bytes;
    uint64_t file_size;
    uint64_t tensor_count;
    uint64_t metadata_count;
    uint32_t version;
#ifdef _WIN32
    void *file_handle;
    void *map_handle;
#else
    int fd;
#endif
} NiyahGGUFMap;
/* Caller supplies a zero-initialized struct and closes after successful open.
 * Only the header is validated; use a separate tensor parser before inference. */
int niyah_gguf_map_open(NiyahGGUFMap *map, const char *utf8_path);
const unsigned char *niyah_gguf_map_range(const NiyahGGUFMap *map,
                                           uint64_t offset, uint64_t length);
void niyah_gguf_map_close(NiyahGGUFMap *map);
int niyah_gguf_map_smoke(void);
#ifdef __cplusplus
}
#endif
#endif
