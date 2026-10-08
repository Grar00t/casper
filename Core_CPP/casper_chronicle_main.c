#include "casper_chronicle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *program) {
    fprintf(stderr,
            "usage:\n"
            "  %s ingest FILE\n"
            "  %s query STORE \"QUESTION\"\n"
            "  %s verify RECEIPT\n"
            "  %s --self-check\n"
            "  %s --benchmark\n",
            program, program, program, program, program);
}

int main(int argc, char **argv) {
    char *output = NULL;
    char *receipt = NULL;
    int rc;
    if (argc == 2 && strcmp(argv[1], "--self-check") == 0) {
        return casper_chronicle_self_test();
    }
    if (argc == 2 && strcmp(argv[1], "--benchmark") == 0) {
        return casper_chronicle_benchmark();
    }
    if (argc == 3 && strcmp(argv[1], "ingest") == 0) {
        rc = casper_chronicle_ingest(argv[2], &output, &receipt);
        if (rc == 0) {
            printf("store=%s\nreceipt=%s\n", output, receipt);
        }
        casper_chronicle_free(output);
        casper_chronicle_free(receipt);
        return rc;
    }
    if (argc == 4 && strcmp(argv[1], "query") == 0) {
        rc = casper_chronicle_query(argv[2], argv[3], &output, &receipt);
        if (rc == 0) {
            printf("%s\nreceipt=%s\n", output, receipt);
        }
        casper_chronicle_free(output);
        casper_chronicle_free(receipt);
        return rc;
    }
    if (argc == 3 && strcmp(argv[1], "verify") == 0) {
        rc = casper_chronicle_verify(argv[2]);
        printf("%s\n", rc == 0 ? "VALID" : "INVALID");
        return rc;
    }
    usage(argv[0]);
    return 2;
}
