/* casper_ffi.h — Stable ABI between KSpike (Rust) and Casper Engine (C11).
 *
 * Version 1.0 — frozen 2026-04-25.
 *
 * Compile-time contract:
 *   - libcasper.so MUST export the four symbols below with these exact
 *     signatures.
 *   - All strings are UTF-8, NUL-terminated.
 *   - All JSON payloads use the schemas defined in the comments below.
 *   - The library is responsible for its own thread-safety; KSpike calls
 *     these functions from any tokio worker.
 *
 *   Casper-Sovereign-1.0
 */

#ifndef KSPIKE_CASPER_FFI_H
#define KSPIKE_CASPER_FFI_H

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise the Casper Engine. `model_path` may point to a model directory,
 * a single weights file, or be NULL to use a built-in default. Returns 0 on
 * success, non-zero on failure. */
int casper_init(const char *model_path);

/* Evaluate a judgement request.
 *
 * `req_json` schema:
 *   {
 *     "module":            "<string>",
 *     "verdict_kind":      "ignore" | "report" | "defend" | "strike",
 *     "target":            "<string|null>",
 *     "confidence":        <0..1 float>,
 *     "proportionality":   <0..10 int>,
 *     "risk_level":        <0..10 int>,
 *     "attack_certainty":  <0..1 float>,
 *     "target_legitimacy": <0..1 float>
 *   }
 *
 * `out` is written with a UTF-8 NUL-terminated JSON payload of:
 *   {
 *     "decision": "allow" | "deny" | "uncertain",
 *     "rationale": "<string>"
 *   }
 *
 * Returns the number of bytes written (excluding NUL) on success, or -1 on
 * error. If `out_cap` is too small to hold the response plus its terminator,
 * the function returns -1 without writing past `out_cap`.
 */
int casper_judge_evaluate(const char *req_json, char *out, int out_cap);

/* Release all resources. After calling this, no other casper_* function may
 * be called until casper_init() is called again. */
void casper_shutdown(void);

/* Returns a static, NUL-terminated string identifying the Casper Engine
 * build. KSpike logs this on startup so the audit trail captures the model
 * provenance. The returned pointer is valid for the lifetime of the
 * library. */
const char *casper_version(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KSPIKE_CASPER_FFI_H */
