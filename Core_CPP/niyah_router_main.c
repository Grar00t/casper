#include "niyah_router.h"
#include "tokenizer.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    NiyahModel *model = NULL;
    NiyahRouterHead head;
    NiyahRoute route;
    float scores[NIYAH_ROUTER_CLASS_COUNT];
    NiyahRouterAdapters adapters;
    const char *selected = NULL;
    int rc;

    if (argc != 4 && argc != 7) {
        (void)fprintf(stderr,
                      "usage: %s MODEL.bin ROUTER_HEAD.bin PROMPT "
                      "[LOGIC_ADAPTER DIALECT_ADAPTER CODE_ADAPTER]\n",
                      argv[0]);
        return 2;
    }

    rc = niyah_load(&model, argv[1]);
    if (rc != 0 || model == NULL) {
        (void)fprintf(stderr, "niyah_load failed: %d\n", rc);
        return 3;
    }

    rc = niyah_router_head_load(&head, argv[2]);
    if (rc != 0) {
        (void)fprintf(stderr, "router_head_load failed: %d\n", rc);
        niyah_free(model);
        return 4;
    }

    rc = niyah_router_classify(model, &head, argv[3], &route, scores);
    if (rc != 0) {
        (void)fprintf(stderr, "router_classify failed: %d\n", rc);
        niyah_router_head_free(&head);
        niyah_free(model);
        tokenizer_free();
        return 5;
    }

    if (argc == 7) {
        adapters.logic_adapter = argv[4];
        adapters.dialect_adapter = argv[5];
        adapters.code_adapter = argv[6];
        selected = niyah_router_select_adapter(route, &adapters);
    }

    (void)printf("route=%s score_logic=%.7g score_dialect=%.7g "
                 "score_code=%.7g",
                 niyah_router_route_name(route),
                 scores[0], scores[1], scores[2]);
    if (selected != NULL) {
        (void)printf(" adapter=%s", selected);
    }
    (void)printf("\n");

    niyah_router_head_free(&head);
    niyah_free(model);
    tokenizer_free();
    return 0;
}
