#include "chronicle_pool.h"
#include <stdint.h>
#include <string.h>

typedef union ChrBlock ChrBlock;
union ChrBlock {
    struct { size_t capacity; ChrBlock *next; int used; } h;
    max_align_t alignment;
};
/* The union gives both the header and every payload max_align_t alignment. */
static union { max_align_t alignment; unsigned char bytes[CHRONICLE_POOL_SIZE]; } _pool;
static ChrBlock *first;
static size_t live_bytes, peak_bytes;

static void initialize(void) {
    if (first) return;
    first = (ChrBlock *)(void *)_pool.bytes;
    first->h.capacity = sizeof(_pool.bytes) - sizeof(ChrBlock);
    first->h.next = NULL;
    first->h.used = 0;
}
static void split_block(ChrBlock *block, size_t size) {
    ChrBlock *tail;
    if (block->h.capacity - size < sizeof(ChrBlock) + _Alignof(max_align_t)) return;
    tail = (ChrBlock *)(void *)((unsigned char *)(void *)(block + 1) + size);
    tail->h.capacity = block->h.capacity - size - sizeof(ChrBlock);
    tail->h.next = block->h.next;
    tail->h.used = 0;
    block->h.capacity = size;
    block->h.next = tail;
}
void *chr_malloc(size_t size) {
    ChrBlock *b;
    const size_t alignment = _Alignof(max_align_t);
    if (size == 0u) size = 1u;
    if (size > CHRONICLE_POOL_SIZE - sizeof(ChrBlock) - (alignment - 1u)) return NULL;
    size = (size + alignment - 1u) / alignment * alignment;
    initialize();
    for (b = first; b; b = b->h.next) {
        if (b->h.used || b->h.capacity < size) continue;
        split_block(b, size);
        b->h.used = 1;
        live_bytes += b->h.capacity + sizeof(ChrBlock);
        if (live_bytes > peak_bytes) peak_bytes = live_bytes;
        return b + 1;
    }
    return NULL;
}
void *chr_calloc(size_t count, size_t size) {
    void *p;
    if (size && count > SIZE_MAX / size) return NULL;
    p = chr_malloc(count * size);
    if (p) memset(p, 0, count * size);
    return p;
}
void chr_free(void *ptr) {
    ChrBlock *b, *previous = NULL;
    if (!ptr) return;
    for (b = first; b && (void *)(b + 1) != ptr; b = b->h.next) previous = b;
    if (!b || !b->h.used) return;
    live_bytes -= b->h.capacity + sizeof(ChrBlock);
    b->h.used = 0;
    if (b->h.next && !b->h.next->h.used) {
        b->h.capacity += sizeof(ChrBlock) + b->h.next->h.capacity;
        b->h.next = b->h.next->h.next;
    }
    if (previous && !previous->h.used) {
        previous->h.capacity += sizeof(ChrBlock) + b->h.capacity;
        previous->h.next = b->h.next;
    }
}
void *chr_realloc(void *ptr, size_t size) {
    ChrBlock *b; void *next;
    if (!ptr) return chr_malloc(size);
    if (!size) { chr_free(ptr); return NULL; }
    for (b = first; b && (void *)(b + 1) != ptr; b = b->h.next) { }
    if (!b || !b->h.used) return NULL;
    if (size <= b->h.capacity) return ptr;
    next = chr_malloc(size);
    if (!next) return NULL;
    memcpy(next, ptr, b->h.capacity);
    chr_free(ptr);
    return next;
}
size_t chr_pool_used(void) { return live_bytes; }
size_t chr_pool_peak(void) { return peak_bytes; }
void chr_pool_reset_peak(void) { peak_bytes = live_bytes; }
