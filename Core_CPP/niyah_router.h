#ifndef NIYAH_ROUTER_H
#define NIYAH_ROUTER_H

#include "niyah_core.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NIYAH_ROUTER_MAGIC UINT32_C(0x3354524E)
#define NIYAH_ROUTER_VERSION UINT32_C(1)
#define NIYAH_ROUTER_CLASS_COUNT UINT32_C(3)

typedef enum {
    NIYAH_ROUTE_LOGIC = 0,
    NIYAH_ROUTE_DIALECT = 1,
    NIYAH_ROUTE_CODE = 2
} NiyahRoute;

typedef struct {
    uint32_t vocab_size;
    float *weights; /* [3 x vocab_size], row-major */
    float bias[NIYAH_ROUTER_CLASS_COUNT];
} NiyahRouterHead;

typedef struct {
    const char *logic_adapter;
    const char *dialect_adapter;
    const char *code_adapter;
} NiyahRouterAdapters;

int niyah_router_head_load(NiyahRouterHead *head, const char *path);
void niyah_router_head_free(NiyahRouterHead *head);

int niyah_router_classify(NiyahModel *model,
                          const NiyahRouterHead *head,
                          const char *prompt,
                          NiyahRoute *route,
                          float scores[NIYAH_ROUTER_CLASS_COUNT]);

const char *niyah_router_route_name(NiyahRoute route);
const char *niyah_router_select_adapter(NiyahRoute route,
                                        const NiyahRouterAdapters *adapters);

#ifdef __cplusplus
}
#endif

#endif
