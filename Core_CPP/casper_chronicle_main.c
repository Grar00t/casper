#include "casper_chronicle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>
#define CHRONICLE_CLI_MAX_ARGS 6
#define CHRONICLE_CLI_ARG_BYTES 4096
int wmain(int argc, wchar_t **argv);
#endif

static void usage(const char *program) {
    fprintf(stderr,
            "usage:\n"
            "  %s ingest FILE\n"
            "  %s query STORE \"QUESTION\"\n"
            "  %s find STORE \"TERMS\"\n"
            "  %s claim STORE SUBJECT PREDICATE OBJECT\n"
            "  %s verify RECEIPT\n"
            "  %s --self-check\n"
            "  %s --benchmark\n",
            program, program, program, program, program, program, program);
}

static int chronicle_main(int argc, char **argv) {
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
    if (argc == 4 && strcmp(argv[1], "find") == 0) {
        rc = casper_chronicle_find(argv[2], argv[3], &output, &receipt);
        if (rc == 0) printf("%s\nreceipt=%s\n", output, receipt);
        casper_chronicle_free(output);
        casper_chronicle_free(receipt);
        return rc;
    }
    if (argc == 6 && strcmp(argv[1], "claim") == 0) {
        char claim[512]; int n;
        if (strlen(argv[3]) >= 128u || strlen(argv[4]) >= 128u || strlen(argv[5]) >= 128u ||
            strchr(argv[3], '\t') || strchr(argv[4], '\t') || strchr(argv[5], '\t')) return 2;
        n = snprintf(claim, sizeof(claim), "@claim\t%s\t%s\t%s", argv[3], argv[4], argv[5]);
        if (n < 0 || (size_t)n >= sizeof(claim)) return 2;
        rc = casper_chronicle_query(argv[2], claim, &output, &receipt);
        if (rc == 0) printf("%s\nreceipt=%s\n", output, receipt);
        casper_chronicle_free(output);
        casper_chronicle_free(receipt);
        return rc;
    }
    if (argc == 3 && strcmp(argv[1], "verify") == 0) {
        rc = casper_chronicle_verify(argv[2]);
        printf("%s\n", rc == 0 ? "VALID" : rc == 3 ? "UNSUPPORTED" : "INVALID");
        return rc;
    }
    usage(argv[0]);
    return 2;
}

#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
    char storage[CHRONICLE_CLI_MAX_ARGS][CHRONICLE_CLI_ARG_BYTES];
    char *utf8_argv[CHRONICLE_CLI_MAX_ARGS + 1]; int i;
    if (argc < 1 || argc > CHRONICLE_CLI_MAX_ARGS || !argv) return 2;
    for (i = 0; i < argc; ++i) {
        if (!argv[i] || WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1,
                storage[i], CHRONICLE_CLI_ARG_BYTES, NULL, NULL) == 0) return 2;
        utf8_argv[i] = storage[i];
    }
    utf8_argv[argc] = NULL;
    return chronicle_main(argc, utf8_argv);
}
#else
int main(int argc, char **argv) { return chronicle_main(argc, argv); }
#endif
