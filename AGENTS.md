# Casper / NIYAH Engine — Agent Instructions

**NIYAH v3.0** is a sovereign, local-first, neuro-symbolic AI inference and training engine written in C11. It fuses a Transformer neural core with a Prolog-like symbolic reasoner, exact rational constraint solver, and SHA-256-signed proof generation. Zero external runtime dependencies (libc + libm only).

---

## Architecture

See [README.md](./README.md) for the full architecture diagram. Key subsystems:

| File | Role |
|---|---|
| `Core_CPP/niyah_core.c` | Transformer decoder — GQA, SwiGLU, RoPE, RMSNorm, single-pool alloc |
| `Core_CPP/hybrid_reasoner.c` | Prolog-like symbolic reasoner — Robinson unification, backward chaining |
| `Core_CPP/constraint_solver.c` | Exact rational linear constraint solver (int64 num/den, never float) |
| `Core_CPP/rule_parser.c` | `.nrule` format parser |
| `Core_CPP/proof_generator.c` | SHA-256 proof generation and verification |
| `Core_CPP/khz_q_svd.c` | Math-coherence gate (energy threshold ≥ 0.85 required) |
| `tokenizer.c` | UTF-8 tokenizer with Arabic support |
| `Core_CPP/niyah_hybrid_main.c` | CLI entry point — wires all subsystems together |
| `UI_CSharp/` | Optional C# UI layer |

---

## Build Commands

### Linux / macOS (GCC)

```bash
# Release (auto-detects AVX2/NEON/scalar)
bash Core_CPP/build_gcc.sh

# Debug + AddressSanitizer + UBSan
bash Core_CPP/build_gcc.sh --debug

# Smoke tests
RUN_SMOKE=1 bash Core_CPP/build_gcc.sh
```

### Windows (MSVC via PowerShell)

```powershell
# Build
powershell -ExecutionPolicy Bypass -File .\scripts\niyah.ps1 build

# Smoke tests (expected: 96 tests pass)
powershell -ExecutionPolicy Bypass -File .\scripts\niyah.ps1 smoke

# Benchmark
powershell -ExecutionPolicy Bypass -File .\scripts\niyah.ps1 bench
```

### Full Hybrid Test Suite (Linux)

```bash
gcc -O2 -std=c11 -Wall -Wextra -Werror \
  Core_CPP/niyah_core.c Core_CPP/hybrid_reasoner.c \
  Core_CPP/constraint_solver.c Core_CPP/rule_parser.c \
  Core_CPP/proof_generator.c Core_CPP/khz_q_svd.c \
  Core_CPP/niyah_hybrid_main.c tokenizer.c \
  -o niyah_hybrid -lm
./niyah_hybrid --smoke
```

Expected: **96 tests**. Any failure, crash, or count change = blocked.

---

## Non-Negotiable Rules

1. **No external dependencies** — libc + libm only. No OpenSSL, BLAS, LAPACK, vendor SDKs.
2. **Single-pool allocation** — slice from `m->_pool` via `niyah_alloc`. No loose `malloc`/`free` in hot paths.
3. **Compile clean** — all new/modified code must pass `-std=c11 -Wall -Wextra -Werror`.
4. **Exact arithmetic** — `constraint_solver.c` uses `int64_t` num/den rationals. Never convert to float.
5. **Occurs check** — never disable in `hybrid_reasoner.c`. Reject cyclic substitutions.
6. **Verification gates** — all five gates must pass for accepted output: KHZ math-coherence (≥ 0.85), rule validation, proof generation, SHA-256 signing. Gates cannot be bypassed in production paths.
7. **Scalar fallback** — every SIMD path (AVX2, NEON) must have an equivalent scalar C11 path.
8. **Minimal-change rule** — smallest patch that satisfies the requirement. No unrelated refactoring.
9. **UTF-8 by code point** — tokenizer processes Arabic natively; never treat continuation bytes as characters.
10. **Determinism** — identical inputs → identical solver decisions, rule results, proof hashes.

---

## Verification Order (for non-trivial changes)

1. Compile the affected target with warnings enabled.
2. Run the narrowest relevant test.
3. Run the debug sanitizer build (`--debug`).
4. Run neural smoke tests when `niyah_core.c` or tensor kernels are touched.
5. Run full hybrid suite (`--smoke`, 96 tests) when symbolic/constraint/proof/parser/tokenizer code is touched.
6. Compare SIMD output against scalar reference when SIMD code is touched.
7. Run benchmarks only after correctness is established.

---

## Key Invariants by Subsystem

### Neural Core (`niyah_core.c`)
- `NiyahConfig` is serialized verbatim to the `.bin` header (64 bytes). Any field change requires bumping `NIYAH_VER` (`0x0005`).
- KV cache uses head-major layout: `kv_k[layer][head][seq_pos][head_dim]`.
- Pool size must be pre-calculated with overflow-safe arithmetic before allocation.

### Symbolic Reasoner (`hybrid_reasoner.c`)
- Variables: uppercase first character (e.g., `X`, `Result`). Atoms: lowercase.
- `NIYAH_SYM_MAX_DEPTH = 256` — recursion limit enforced.
- Unification must terminate even on adversarial/malformed input.

### Constraint Solver (`constraint_solver.c`)
- `NiyahCspRat` invariant: `den > 0`, `gcd(|num|, den) == 1`, zero = `{0, 1}`.
- Max 32 variables (`NIYAH_CSP_MAX_VARS`), 64 constraints (`NIYAH_CSP_MAX_CONSTRAINTS`).
- Overflow in int64 arithmetic → return explicit error status, never wrap/clamp.

### Proof Generator (`proof_generator.c`)
- `niyah_proof_generate`: SHA-256 over `prompt || output || rule_file_contents`.
- `.proof` files must verify with `niyah_proof_verify`. Tampered or truncated = fail closed.
- Hash canonical byte representations only — never hash padding or uninitialized memory.

### Rule Parser (`rule_parser.c`)
- `.nrule` format. Malformed input → deterministic rejection (no partial acceptance).
- Parser output must be stable for identical input across runs.

---

## Evidence Protocol

Every material claim about the implementation must be verifiable:

- Inspect the actual file before asserting behavior.
- Cite path and line range: `Core_CPP/niyah_core.c:42-87`.
- Use status tags: `VERIFIED` / `INFERRED` / `CONTRADICTED` / `UNKNOWN`.
- Never fabricate line numbers, test results, or command output.

---

## Prohibited Actions

- Adding external runtime or build dependencies
- Converting `constraint_solver.c` exact arithmetic to floating point
- Disabling the occurs check in `hybrid_reasoner.c`
- Removing scalar fallback paths
- Bypassing verification gates in production code
- Introducing hidden `malloc`/`free` in inference/training hot paths
- Changing public symbol names, `.bin` serialization format, `.nrule` grammar, or proof format without explicit migration handling
- Committing secrets, credentials, or machine-specific paths
