#include "niyah_router.h"
#include "tokenizer.h"

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t vocab_size;
    uint32_t class_count;
    uint32_t seed;
    uint32_t reserved0;
    uint32_t reserved1;
    uint32_t reserved2;
} NiyahRouterDiskHeader;

_Static_assert(sizeof(NiyahRouterDiskHeader) == 32U, "router header size");

static int size_mul_ok(size_t a, size_t b, size_t *out)
{
    if (out == NULL) return 0;
    if (a != 0U && b > SIZE_MAX / a) return 0;
    *out = a * b;
    return 1;
}

static int router_tag_fast_path(const char *prompt, NiyahRoute *route)
{
    const unsigned char *p = (const unsigned char *)prompt;

    if (prompt == NULL || route == NULL) return 0;
    while (*p != 0U && isspace(*p)) ++p;

    if (strncmp((const char *)p, "<CODE>", 6U) == 0) {
        *route = NIYAH_ROUTE_CODE;
        return 1;
    }
    if (strncmp((const char *)p, "<NAJDI>", 7U) == 0 ||
        strncmp((const char *)p, "<HIJAZI>", 8U) == 0) {
        *route = NIYAH_ROUTE_DIALECT;
        return 1;
    }
    if (strncmp((const char *)p, "<LOGIC>", 7U) == 0) {
        *route = NIYAH_ROUTE_LOGIC;
        return 1;
    }
    return 0;
}

int niyah_router_head_load(NiyahRouterHead *head, const char *path)
{
    FILE *f = NULL;
    NiyahRouterDiskHeader disk;
    long file_size;
    size_t weight_count;
    size_t weight_bytes;
    size_t expected_bytes;

    if (head == NULL || path == NULL) return -1;
    memset(head, 0, sizeof(*head));

    f = fopen(path, "rb");
    if (f == NULL) return -2;

    if (fseek(f, 0L, SEEK_END) != 0) {
        fclose(f);
        return -3;
    }
    file_size = ftell(f);
    if (file_size < 0L || fseek(f, 0L, SEEK_SET) != 0) {
        fclose(f);
        return -3;
    }
    if (fread(&disk, sizeof(disk), 1U, f) != 1U) {
        fclose(f);
        return -4;
    }
    if (disk.magic != NIYAH_ROUTER_MAGIC ||
        disk.version != NIYAH_ROUTER_VERSION ||
        disk.class_count != NIYAH_ROUTER_CLASS_COUNT ||
        disk.vocab_size == 0U) {
        fclose(f);
        return -5;
    }

    if (!size_mul_ok((size_t)disk.vocab_size,
                     (size_t)NIYAH_ROUTER_CLASS_COUNT,
                     &weight_count) ||
        !size_mul_ok(weight_count, sizeof(float), &weight_bytes)) {
        fclose(f);
        return -6;
    }
    expected_bytes = sizeof(disk) + weight_bytes +
                     (size_t)NIYAH_ROUTER_CLASS_COUNT * sizeof(float);
    if ((size_t)file_size != expected_bytes) {
        fclose(f);
        return -7;
    }

    head->weights = (float *)malloc(weight_bytes);
    if (head->weights == NULL) {
        fclose(f);
        return -8;
    }
    if (fread(head->weights, sizeof(float), weight_count, f) != weight_count ||
        fread(head->bias, sizeof(float),
              (size_t)NIYAH_ROUTER_CLASS_COUNT, f) !=
              (size_t)NIYAH_ROUTER_CLASS_COUNT) {
        niyah_router_head_free(head);
        fclose(f);
        return -9;
    }

    head->vocab_size = disk.vocab_size;
    fclose(f);
    return 0;
}

void niyah_router_head_free(NiyahRouterHead *head)
{
    if (head == NULL) return;
    free(head->weights);
    memset(head, 0, sizeof(*head));
}

int niyah_router_classify(NiyahModel *model,
                          const NiyahRouterHead *head,
                          const char *prompt,
                          NiyahRoute *route,
                          float scores[NIYAH_ROUTER_CLASS_COUNT])
{
    uint32_t *tokens = NULL;
    uint32_t token_count;
    uint32_t i;
    uint32_t cap;
    float *logits = NULL;
    float local_scores[NIYAH_ROUTER_CLASS_COUNT];
    uint32_t best = 0U;

    if (model == NULL || head == NULL || prompt == NULL || route == NULL ||
        head->weights == NULL || head->vocab_size != model->cfg.vocab_size) {
        return -1;
    }

    if (router_tag_fast_path(prompt, route) != 0) {
        local_scores[0] = (*route == NIYAH_ROUTE_LOGIC) ? 1.0f : 0.0f;
        local_scores[1] = (*route == NIYAH_ROUTE_DIALECT) ? 1.0f : 0.0f;
        local_scores[2] = (*route == NIYAH_ROUTE_CODE) ? 1.0f : 0.0f;
        if (scores != NULL) {
            (void)memcpy(scores, local_scores, sizeof(local_scores));
        }
        return 0;
    }

    if (model->cfg.ctx_len == UINT32_MAX) return -2;
    cap = model->cfg.ctx_len + 1U;
    tokens = (uint32_t *)calloc((size_t)cap, sizeof(uint32_t));
    if (tokens == NULL) return -3;

    tokenizer_init();
    token_count = tokenizer_encode(prompt, tokens, cap);
    if (token_count < 2U) {
        free(tokens);
        return -4;
    }

    for (i = 0U; i + 1U < token_count && i < model->cfg.ctx_len; ++i) {
        logits = niyah_forward(model, tokens[i], i);
        if (logits == NULL) {
            free(tokens);
            return -5;
        }
    }
    free(tokens);
    if (logits == NULL) return -5;

    niyah_matvec_f32(local_scores, head->weights, logits,
                     (size_t)NIYAH_ROUTER_CLASS_COUNT,
                     (size_t)head->vocab_size);
    for (i = 0U; i < NIYAH_ROUTER_CLASS_COUNT; ++i) {
        local_scores[i] += head->bias[i];
        if (i == 0U || local_scores[i] > local_scores[best]) best = i;
    }

    *route = (NiyahRoute)best;
    if (scores != NULL) {
        (void)memcpy(scores, local_scores, sizeof(local_scores));
    }
    return 0;
}

const char *niyah_router_route_name(NiyahRoute route)
{
    switch (route) {
        case NIYAH_ROUTE_LOGIC: return "logic";
        case NIYAH_ROUTE_DIALECT: return "dialect";
        case NIYAH_ROUTE_CODE: return "code";
        default: return "unknown";
    }
}

const char *niyah_router_select_adapter(NiyahRoute route,
                                        const NiyahRouterAdapters *adapters)
{
    if (adapters == NULL) return NULL;
    switch (route) {
        case NIYAH_ROUTE_LOGIC: return adapters->logic_adapter;
        case NIYAH_ROUTE_DIALECT: return adapters->dialect_adapter;
        case NIYAH_ROUTE_CODE: return adapters->code_adapter;
        default: return NULL;
    }
}
