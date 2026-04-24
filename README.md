# Casper Engine

**NIYAH — Sovereign Hybrid Neuro-Symbolic Inference Engine**

> نحن ورثة الخوارزمي — لا يوجد مستحيل في الدنيا
> *We are the heirs of Al-Khwarizmi — nothing is impossible.*

A from-scratch C11 inference and training engine that fuses a Transformer neural core with a symbolic reasoning layer, cryptographic proof generation, and constraint solving. Zero external dependencies. Runs on $35 hardware.

---

## Architecture Overview

```
                    ┌──────────────────────────────────┐
                    │         Hybrid CLI               │
                    │   (niyah_hybrid_main.c)           │
                    └──────┬─────────────┬─────────────┘
                           │             │
              ┌────────────▼──┐    ┌─────▼──────────────┐
              │  Neural Core  │    │  Symbolic Reasoner  │
              │ (niyah_core)  │    │ (hybrid_reasoner)   │
              │               │    │                     │
              │  Transformer  │    │  Unification        │
              │  GQA Attn     │    │  Backward Chaining  │
              │  SwiGLU FFN   │    │  Knowledge Base     │
              │  RoPE         │    └────────┬────────────┘
              │  RMSNorm      │             │
              │  SIMD Kernels │    ┌────────▼────────────┐
              └───────┬───────┘    │  Constraint Solver  │
                      │            │  (constraint_solver) │
                      │            │  Rational Arithmetic │
              ┌───────▼───────┐    │  Bounds Propagation │
              │   Tokenizer   │    └────────┬────────────┘
              │   (UTF-8)     │             │
              └───────┬───────┘    ┌────────▼────────────┐
                      │            │   Rule Parser       │
                      ▼            │   (.nrule format)   │
                ┌──────────┐       └────────┬────────────┘
                │  Output   │               │
                │  Text     │◄──────────────┘
                └─────┬────┘        Verify & Enforce
                      │
              ┌───────▼───────┐
              │ Proof Generator│
              │  SHA-256 Hash  │
              │  Audit Trail   │
              └───────────────┘
```

### Design Principles

- **Zero dependencies** — libc + libm only. No OpenSSL, no Boost, no BLAS.
- **Single-pool allocation** — one `malloc`, zero fragmentation, deterministic teardown.
- **SIMD everywhere** — AVX2+FMA (x86_64), NEON (aarch64), scalar fallback. Compile-time selection.
- **C11 clean** — compiles with `-Wall -Wextra -Werror -pedantic`. C++17 compatible headers.
- **Offline-first** — no network calls, no telemetry, no cloud. Fully sovereign.

---

## Features

### Neural Core (`niyah_core.c` — 809 lines)
- Transformer decoder with Grouped-Query Attention (GQA)
- SwiGLU feed-forward network with configurable multiplier
- Rotary Position Embeddings (RoPE) with adjustable theta
- RMSNorm (pre-attention and pre-FFN)
- KV-cache with head-major layout for optimal attention locality
- Adam optimizer with weight decay for training
- Top-p (nucleus) sampling with temperature control
- Model serialization to `.bin` format (64-byte header)

### Symbolic Reasoner (`hybrid_reasoner.c` — 801 lines)
- First-order logic terms: atoms, variables, compound terms
- Robinson's unification algorithm with occurs check
- Backward chaining (Prolog-style) with configurable depth limit
- Knowledge base with clause indexing
- Variable renaming for clause isolation

### Constraint Solver (`constraint_solver.c` — 479 lines)
- Exact rational arithmetic (int64 numerator/denominator)
- Linear inequality constraints (≤, ≥, =)
- Bounds propagation solver with iterative tightening
- Integrates with the symbolic reasoner to prune impossible bindings

### Rule Parser (`rule_parser.c` — 604 lines)
- Human-readable `.nrule` format for output verification
- Recursive descent parser
- Supports: `IF`/`THEN`, `ALWAYS`, `CONTAINS`, `EQUALS`, `NOT_EQUALS`, `AND`, `REJECTED`, `MUST contain`
- Case-insensitive matching
- Sequence-level verification: generate full response → decode → verify → re-sample if violated

### Proof Generator (`proof_generator.c` — 402 lines)
- Public-domain SHA-256 (FIPS 180-4) — no OpenSSL dependency
- Proof hash: `SHA-256(prompt ∥ output ∥ rule_file_contents)`
- Machine-verifiable `.proof` file format (NIYAH-PROOF-V1)
- Offline audit trail for every inference

### SIMD Kernels
| Operation | AVX2+FMA | NEON | Scalar |
|-----------|----------|------|--------|
| RMSNorm   | ✓        | ✓    | ✓      |
| MatVec    | ✓        | ✓    | ✓      |
| SwiGLU    | ✓        | ✓    | ✓      |
| Softmax   | ✓        | ✓    | ✓      |

---

## Quick Start

### Linux / macOS (GCC or Clang)

```bash
# Clone
git clone https://github.com/grar00t/Casper_Engine.git
cd Casper_Engine

# Build core binaries (auto-detects arch + SIMD)
bash scripts/build_gcc.sh

# Run smoke tests (neural core)
RUN_SMOKE=1 bash scripts/build_gcc.sh

# Build hybrid binary (neural + symbolic)
gcc -O2 -std=c11 -Wall -Wextra -Werror \
    Core_CPP/niyah_core.c Core_CPP/hybrid_reasoner.c \
    Core_CPP/constraint_solver.c Core_CPP/rule_parser.c \
    Core_CPP/proof_generator.c Core_CPP/niyah_hybrid_main.c \
    tokenizer.c -o niyah_hybrid -lm

# Run all 96 tests (neural + symbolic + constraints + rules + proofs)
./niyah_hybrid --smoke
```

### Windows (MSVC / PowerShell)

```powershell
cd C:\Users\You\Casper_Engine

# Unified entrypoint
.\scripts\niyah.ps1 build
.\scripts\niyah.ps1 smoke
.\scripts\niyah.ps1 train Data_Training\sovereign_knowledge.txt 3 0.001
```

### Training

```bash
# Standalone trainer
gcc -O2 -std=c11 Core_CPP/niyah_core.c Core_CPP/niyah_train.c \
    tokenizer.c -o niyah_train -lm

# Train on corpus (3 epochs, lr=0.001)
./niyah_train Data_Training/sovereign_knowledge.txt 3 0.001

# Save model
./niyah_hybrid --model niyah_trained.bin --save
```

---

## Hybrid Inference

The hybrid engine generates text with the neural Transformer, then verifies the output against symbolic rules before returning it.

### Flow

1. **Encode** — tokenize prompt (UTF-8, Arabic-aware)
2. **Generate** — autoregressive forward pass with KV-cache
3. **Decode** — detokenize to candidate text
4. **Verify** — check against `.nrule` constraints
5. **Re-sample** — if violated, adjust seed and retry (up to `max_retries`)
6. **Prove** — compute SHA-256 proof hash over (prompt, output, rules)
7. **Return** — verified output + proof

### Usage

```bash
# Interactive mode with rules
./niyah_hybrid --model model.bin --rules safety.nrule --interactive

# Verify a proof file
./niyah_hybrid --verify-proof response.proof
```

### Rule Format (`.nrule`)

```
// Comments start with //
rule: "IF question CONTAINS 'password' THEN REJECTED"
rule: "IF output CONTAINS 'hack' THEN answer = 'I cannot help with that.'"
rule: "ALWAYS output MUST contain 'bismillah'"
```

### Proof File Format (`.proof`)

```
NIYAH-PROOF-V1
hash: a1b2c3d4...
prompt_hash: e5f6a7b8...
output_hash: c9d0e1f2...
rules_hash: 34567890...
timestamp: 2026-04-09T12:00:00Z
```

---

## Project Layout

```
Casper_Engine/
├── Core_CPP/                    # Engine source code
│   ├── niyah_core.h             # Public API header (189 lines)
│   ├── niyah_core.c             # Neural Transformer engine (809 lines)
│   ├── hybrid_reasoner.h/c      # Symbolic reasoner (949 lines)
│   ├── constraint_solver.h/c    # Linear constraint solver (575 lines)
│   ├── rule_parser.h/c          # .nrule format parser (723 lines)
│   ├── proof_generator.h/c      # SHA-256 + proof system (466 lines)
│   ├── niyah_hybrid_main.c      # Hybrid CLI entrypoint (434 lines)
│   ├── niyah_main.c             # Neural-only smoke test (18 lines)
│   ├── niyah_train.c            # Standalone trainer (186 lines)
│   └── bench_niyah.c            # Benchmarking harness (223 lines)
├── tokenizer.c                  # UTF-8 tokenizer with encode/decode (186 lines)
├── Data_Training/               # Training datasets
├── Math_ASM/                    # Assembly experiments (AVX)
├── UI_CSharp/                   # Optional C# manager UI
└── scripts/                     # Build & automation
    ├── build_gcc.sh             # GCC/Clang build (Linux/macOS)
    ├── build_msvc.ps1           # MSVC build (Windows)
    └── niyah.ps1                # Unified PowerShell wrapper
```

**Total codebase:** ~4,750 lines of C11 (excluding tests embedded in source).

---

## Building

### Requirements

- C11 compiler: GCC 7+, Clang 6+, or MSVC 2019+
- No external libraries required
- Optional: `cppcheck` for static analysis

### Build Flags

```bash
# Release (default)
gcc -O3 -std=c11 -Wall -Wextra -Werror -DNDEBUG ...

# Debug with sanitizers
gcc -O0 -g3 -std=c11 -Wall -Wextra -Werror \
    -fsanitize=address,undefined ...

# Force architecture
bash scripts/build_gcc.sh --arch arm64
bash scripts/build_gcc.sh --arch x86_64

# With static analysis
RUN_LINT=1 bash scripts/build_gcc.sh
```

### Standalone Tests

Each subsystem compiles independently for isolated testing:

```bash
# Symbolic reasoner (21 tests)
gcc -O2 -std=c11 -Wall -Wextra -Werror \
    Core_CPP/hybrid_reasoner.c -DSYM_STANDALONE_TEST -o test_reasoner && ./test_reasoner

# Constraint solver (19 tests)
gcc -O2 -std=c11 -Wall -Wextra -Werror \
    Core_CPP/constraint_solver.c -DCSP_STANDALONE_TEST -o test_csp && ./test_csp

# Rule parser (22 tests)
gcc -O2 -std=c11 -Wall -Wextra -Werror \
    Core_CPP/rule_parser.c -DRULE_STANDALONE_TEST -o test_rules && ./test_rules

# Proof generator (11 tests, NIST SHA-256 vectors)
gcc -O2 -std=c11 -Wall -Wextra -Werror \
    Core_CPP/proof_generator.c -DPROOF_STANDALONE_TEST -o test_proof && ./test_proof

# Full suite (96 tests across all subsystems)
./niyah_hybrid --smoke
```

---

## Model Format (`.bin`)

| Offset | Size | Field |
|--------|------|-------|
| 0x00   | 4    | Magic (`0x4E595148` = "NYQH") |
| 0x04   | 4    | Version |
| 0x08   | 4    | Embedding dimension |
| 0x0C   | 4    | Number of attention heads |
| 0x10   | 4    | Number of KV heads (GQA) |
| 0x14   | 4    | Number of layers |
| 0x18   | 4    | FFN multiplier |
| 0x1C   | 4    | Vocabulary size |
| 0x20   | 4    | Context length |
| 0x24   | 4    | RoPE theta (float) |
| 0x28   | 4    | RMS epsilon (float) |
| 0x2C   | 4    | Flags (reserved) |
| 0x30   | 16   | Padding |
| 0x40   | ...  | Weight data (IEEE 754 float32) |

Header is exactly 64 bytes. Weights follow in row-major order.

---

## API Reference

### Core Neural API (`niyah_core.h`)

```c
NiyahModel *niyah_alloc(const NiyahConfig *cfg);
void        niyah_free(NiyahModel *m);
int         niyah_save(const NiyahModel *m, const char *path);
int         niyah_load(NiyahModel **out, const char *path);
float      *niyah_forward(NiyahModel *m, uint32_t token, uint32_t pos);
uint32_t    niyah_sample(const float *logits, uint32_t vocab, NiyahSampler *s);
float       niyah_train_step(NiyahModel *m, NiyahAdam *opt,
                             const uint32_t *tokens, uint32_t n);
const char *niyah_simd_name(void);   // "AVX2+FMA" | "NEON" | "Scalar"
size_t      niyah_param_count(const NiyahModel *m);
```

### Hybrid API

```c
char *niyah_hybrid_generate(NiyahModel *m, const char *prompt,
                            const NiyahHybridOpts *opts,
                            NiyahSampler *sampler,
                            uint8_t proof_out[32]);
```

### Symbolic Reasoner API

```c
NiyahSymKB  *niyah_sym_kb_alloc(void);
void         niyah_sym_kb_add(NiyahSymKB *kb, NiyahSymClause clause);
bool         niyah_sym_unify(NiyahSymTerm *a, NiyahSymTerm *b, NiyahSymSubst *s);
bool         niyah_sym_query(NiyahSymKB *kb, NiyahSymTerm *goal,
                             NiyahSymSubst *result, uint32_t max_depth);
```

### Proof API

```c
void niyah_sha256(const uint8_t *data, size_t len, uint8_t out[32]);
void niyah_proof_generate(const char *prompt, const char *output,
                          const char *rule_file, uint8_t proof[32]);
int  niyah_proof_save(const char *path, const uint8_t proof[32],
                      const char *prompt, const char *output,
                      const char *rule_file);
bool niyah_proof_verify(const char *proof_path, const char *prompt,
                        const char *output, const char *rule_file);
```

---

## Roadmap

- [ ] GGUF model format import
- [ ] Multi-threaded inference (pthreads)
- [ ] Quantization (INT8/INT4)
- [ ] Extended context (sliding window + ALiBi)
- [ ] WebAssembly target
- [ ] Constraint solver integration with symbolic reasoner during unification

---

## License

This project is maintained by [Suliman Alshammari (Grar00t)](https://github.com/grar00t).

---

## Sister Projects

Casper Engine is the **reasoning brain** of the gratech sovereign stack.
Its shield-arm is **[KSpike](https://github.com/Grar00t/kspike)** — a dual-mode,
judge-gated kernel defense & active-response framework. KSpike embeds the
`KHZ_Q` Khawarizmi-Fitrah balancer (distilled from 115 protocol revisions)
as the ethical kernel of its Judge layer, and will delegate contextual
adjudication to Casper Engine over FFI in v0.2.

- 🧠 [Casper Engine](https://github.com/Grar00t/Casper_Engine) — hybrid neuro-symbolic reasoning (C11, zero-deps).
- 🛡️ [KSpike](https://github.com/Grar00t/kspike) — Casper-governed kernel defense (Rust).
- 🧬 [haven-niyah-engine](https://github.com/Grar00t/haven-niyah-engine) — Arabic-first three-lobe LLM.
- 🌐 [k-forge](https://github.com/Grar00t/k-forge) — P2P, crypto-signed VCS.
- 💻 [khawrizm-os](https://github.com/Grar00t/khawrizm-os) — Sovereign ARM64 Linux.
