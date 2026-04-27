# Contributing to Casper Engine

Thanks for your interest in Casper Engine — a hybrid neuro-symbolic reasoning
engine written in pure C11 with **zero runtime dependencies** beyond `libc` and `libm`.

This document explains how to build, test, and submit changes.

---

## Code of Conduct

Be technical, be kind, be precise. Disagreements are settled by reading the
spec, the source, and (when in doubt) running the tests. No personal attacks,
no off-topic noise.

---

## Repository Layout

```
Casper_Engine/
├── Core_CPP/              # neural + symbolic + constraint + rules + proof + KHZ_Q (C11)
│   ├── niyah_core.c       # Transformer + GQA + RoPE + RMSNorm + SwiGLU + Adam
│   ├── hybrid_reasoner.c  # Plan → Generate → Verify pipeline
│   ├── constraint_solver.c# rational arithmetic + bounds propagation
│   ├── rule_parser.c      # .nrule grammar
│   ├── proof_generator.c  # SHA-256 audit trail (FIPS 180-4)
│   ├── khz_q_svd.c        # KHZ_Q SVD Ethical Prism (8x8 Jacobi SVD)
│   └── niyah_hybrid_main.c# CLI entrypoint + smoke harness
├── include/               # public headers (casper_ffi.h — stable ABI v1.0)
├── Math_ASM/              # arch-specific kernels (AVX2 / NEON / scalar)
├── UI_CSharp/             # optional .NET binding (uses casper_ffi.h)
├── scripts/               # build_gcc.sh, niyah.ps1
├── tokenizer.{c,h}        # BPE-style tokenizer
├── Makefile               # unified build (auto-detects arch + SIMD)
├── README.md
├── LICENSE                # Apache 2.0
└── CONTRIBUTING.md        # ← you are here
```

---

## Building

### One command, full stack

```bash
make            # builds niyah_hybrid + niyah_train + Core_CPP/niyah + Core_CPP/trainer
```

The Makefile auto-detects:
- `x86_64` → enables `-mavx2 -mfma`
- `aarch64` / `arm64` → enables NEON (always-on for ARMv8)
- anything else → falls back to scalar kernels

### Other useful targets

| Target            | What it does                                                            |
|-------------------|-------------------------------------------------------------------------|
| `make hybrid`     | Build only `niyah_hybrid` (the all-in-one CLI)                          |
| `make train`      | Build only `niyah_train` (standalone trainer)                           |
| `make test`       | Build then run the full 101-test smoke suite                            |
| `make test-all`   | Same as `test`                                                          |
| `make test-reasoner` / `test-csp` / `test-rules` / `test-proof` | Run a focused subset |
| `make debug`      | Build with `-O0 -g -fsanitize=address,undefined` for ASan/UBSan         |
| `make clean`      | Remove all build artifacts                                              |
| `make install`    | Install `niyah_hybrid` + `niyah_train` to `/usr/local/bin` (sudo)       |
| `make uninstall`  | Remove installed binaries                                               |
| `make help`       | Print every target                                                      |

### Compiler standards (non-negotiable)

Every translation unit must compile cleanly under:

```
-O2 -std=c11
-Wall -Wextra -Werror
-Wstrict-prototypes -Wmissing-prototypes
-Wcast-align -Wwrite-strings -Wshadow -pedantic
```

If your patch breaks any of those flags, CI will reject it. Run `make` locally
before submitting — it uses exactly these flags.

---

## Testing

The smoke harness is the source of truth: if `./niyah_hybrid --smoke` doesn't
pass cleanly, the patch isn't ready.

```bash
make test
```

Expected output ends with:

```
========================================
ALL SMOKE TESTS PASSED
========================================
Total: 101/101
```

Current breakdown:

| Suite                   | Tests |
|-------------------------|-------|
| Neural Core             | 18    |
| Symbolic Reasoner       | 21    |
| Constraint Solver       | 19    |
| Rule Parser             | 22    |
| Proof Generator         | 11    |
| KHZ_Q SVD Ethical Prism | 5     |
| Hybrid Integration      | 5     |
| **Total**               | **101** |

When you add a feature, add a test. When you fix a bug, add the regression test
that fails before your fix and passes after.

---

## Pull Request Workflow

1. **Fork** `Grar00t/Casper_Engine` and create a topic branch:
   ```
   git checkout -b feat/short-description
   ```
2. **Make your changes.** One logical change per PR. Don't bundle unrelated edits.
3. **Run `make clean && make && make test`** locally. All 101 tests must pass.
4. **Run `make debug`** if you touched memory layout, pointer arithmetic, or
   anything reading/writing buffers. ASan/UBSan must be clean.
5. **Commit** using clear, scoped messages:
   ```
   feat(hybrid): add early-stop heuristic for KHZ_Q exhaustion
   fix(tokenizer): handle empty input without segfault
   docs(readme): correct build command for khz_q_svd.c
   refactor(svd): extract Givens rotation into static helper
   ```
6. **Push** and open a PR against `main`. Describe:
   - What changed
   - Why it changed
   - How you tested it (paste the relevant `make test` output)

---

## Coding Standards

- **Pure C11.** No C++. No platform-specific compiler extensions outside
  `Math_ASM/` (where intrinsics are gated by arch macros).
- **No new runtime dependencies.** Only `libc` and `libm`. If you think you
  need another library, open an issue first.
- **Determinism matters.** The proof generator hashes the full reasoning
  trace; any non-determinism (`time()`, `rand()` without seed, threading)
  must be either gated behind a flag or reproducibly seeded.
- **No globals where state can be local.** Pass context structs explicitly.
- **Free what you allocate.** Run `make debug` if in doubt — ASan catches the
  rest.
- **Comment the why, not the what.** Code says what; comments say why.
- **ASCII source files** — no smart quotes, em-dashes, or Unicode in code.
  English in code, English in commit messages. (The repo welcomes non-English
  issue discussion.)

---

## Architecture: What Goes Where

| If you're adding…                                | …put it in            |
|--------------------------------------------------|-----------------------|
| A new attention variant or normalization         | `Core_CPP/niyah_core.c` |
| A new reasoning step in Plan→Generate→Verify     | `Core_CPP/hybrid_reasoner.c` |
| A new constraint type or propagator              | `Core_CPP/constraint_solver.c` |
| A new rule grammar feature                       | `Core_CPP/rule_parser.c` |
| A new audit-trail field                          | `Core_CPP/proof_generator.c` |
| A new ethical/coherence check                    | `Core_CPP/khz_q_svd.c` |
| A new SIMD kernel                                | `Math_ASM/`           |
| A new public FFI entrypoint                      | `include/casper_ffi.h` (bump ABI version) |

**ABI stability:** `include/casper_ffi.h` is the stable boundary used by sister
projects (KSpike, haven-niyah-engine). Don't break it. If you must add a new
entrypoint, append it — never reorder or repurpose existing symbols. Bump the
ABI version in the header comment.

---

## Filing Bugs

Open a GitHub issue with:

- Architecture (`uname -m`)
- Compiler version (`gcc --version` / `clang --version`)
- The exact `make` command you ran
- The full failing output (compiler error, ASan trace, smoke-test failure, …)
- A minimal reproducer if you have one

For security-relevant bugs (memory corruption, KHZ_Q bypass, proof forgery),
**don't** open a public issue — see the SECURITY note in the README.

---

## License

Casper Engine is licensed under **Apache License 2.0** (see `LICENSE`).
By submitting a contribution you agree to license it under the same terms.

---

— maintained by [@Grar00t](https://github.com/Grar00t)
