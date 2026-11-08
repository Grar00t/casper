/* Independent Win32 FFI host: loads casper.dll without an import library. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef int (*fn_init)(const char *);
typedef int (*fn_judge)(const char *, char *, int);
typedef void (*fn_shutdown)(void);
typedef const char *(*fn_version)(void);

#define REQ(kind) "{\"module\":\"safety\",\"verdict_kind\":\"" kind "\",\"target\":null,\"confidence\":0.98,\"proportionality\":2,\"risk_level\":6,\"attack_certainty\":0.99,\"target_legitimacy\":0.95}"
static int failures = 0;
static fn_judge judge_fn = NULL;

static void check(int good, const char *label) {
    if (!good) { fprintf(stderr, "FFI_FAIL=%s\n", label); ++failures; }
}
static int evaluate(const char *request, const char *expected) {
    char out[256] = {0};
    int n = judge_fn(request, out, (int)sizeof(out));
    return n > 0 && (size_t)n == strlen(out) && strstr(out, expected) != NULL;
}
static DWORD WINAPI concurrency_test(LPVOID ctx) {
    (void)ctx;
    for (int i = 0; i < 500; ++i) {
        if (!evaluate(REQ("strike"), "\"decision\":\"deny\"")) return 1u;
        if (!evaluate(REQ("report"), "\"decision\":\"allow\"")) return 2u;
    }
    return 0u;
}
static int load_function(HMODULE lib, const char *name, void *output) {
    FARPROC proc = GetProcAddress(lib, name);
    if (!proc) return 0;
    memcpy(output, &proc, sizeof(proc));
    return 1;
}

int main(void) {
    HMODULE lib = LoadLibraryW(L"build\\release\\casper.dll");
    fn_init init_fn = NULL;
    fn_shutdown shutdown_fn = NULL;
    fn_version version_fn = NULL;
    char tiny[5] = {'Q', 'Q', 'Q', 'Q', 'Q'};
    char empty[32] = {0};
    HANDLE threads[4] = {0};
    if (!lib) { fprintf(stderr, "FFI_FAIL=LoadLibrary\n"); return 2; }
    check(load_function(lib, "casper_init", &init_fn), "casper_init export");
    check(load_function(lib, "casper_judge_evaluate", &judge_fn), "casper_judge_evaluate export");
    check(load_function(lib, "casper_shutdown", &shutdown_fn), "casper_shutdown export");
    check(load_function(lib, "casper_version", &version_fn), "casper_version export");
    if (!init_fn || !judge_fn || !shutdown_fn || !version_fn) {
        FreeLibrary(lib); return 1;
    }
    check(version_fn() && strcmp(version_fn(), "casper/ffi-1.0-policy-only") == 0,
          "honest policy-only version");
    check(judge_fn(REQ("ignore"), empty, sizeof(empty)) == -1,
          "uninitialized FFI fails closed");
    check(init_fn("not-a-real-model.gguf") != 0, "reject unimplemented model loading");
    check(init_fn(NULL) == 0, "initialize policy-only runtime");
    check(evaluate(REQ("ignore"), "\"decision\":\"allow\""), "ignore allows non-action");
    check(evaluate(REQ("report"), "\"decision\":\"allow\""), "validated report allowed");
    check(evaluate(REQ("defend"), "\"decision\":\"allow\""), "validated defence allowed");
    check(evaluate(REQ("strike"), "\"decision\":\"deny\""), "strike denied");
    check(evaluate("{\"module\":\"x\",\"verdict_kind\":\"report\",\"target\":null,\"confidence\":0.1,\"proportionality\":1,\"risk_level\":2,\"attack_certainty\":0.4,\"target_legitimacy\":0.3}",
                   "\"decision\":\"uncertain\""), "low confidence uncertain");
    check(judge_fn("{}", empty, sizeof(empty)) == -1, "missing fields rejected");
    check(judge_fn("{\"module\":\"x\",\"module\":\"y\"}", empty, sizeof(empty)) == -1,
          "duplicate key rejected");
    check(judge_fn(REQ("report") " trailing", empty, sizeof(empty)) == -1,
          "trailing data rejected");
    check(judge_fn(REQ("ignore"), tiny, (int)sizeof(tiny)) == -1 &&
          memcmp(tiny, "QQQQQ", sizeof(tiny)) == 0, "bounded out buffer untouched");
    check(judge_fn(REQ("ignore"), NULL, 12) == -1, "null out rejected");
    check(judge_fn(REQ("ignore"), empty, -1) == -1, "negative cap rejected");
    for (int i = 0; i < 4; ++i) {
        threads[i] = CreateThread(NULL, 0, concurrency_test, NULL, 0, NULL);
        check(threads[i] != NULL, "thread creation");
    }
    for (int i = 0; i < 4; ++i) {
        DWORD code = 999u;
        if (!threads[i]) continue;
        check(WaitForSingleObject(threads[i], 15000) == WAIT_OBJECT_0, "thread join");
        check(GetExitCodeThread(threads[i], &code) && code == 0u,
              "concurrent judgement");
        CloseHandle(threads[i]);
    }
    shutdown_fn();
    check(judge_fn(REQ("ignore"), empty, sizeof(empty)) == -1,
          "shutdown invalidates judgement");
    check(init_fn(NULL) == 0, "reinit after shutdown");
    shutdown_fn();
    check(FreeLibrary(lib) != 0, "FreeLibrary");
    if (failures == 0) puts("FFI_HOST_REGRESSION=PASS (4 exports, concurrency, bounds, policy)");
    return failures ? 1 : 0;
}
