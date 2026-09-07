# Casper / NIYAH

Casper is a C11 codebase containing a small neural runtime/trainer, symbolic reasoning, rational constraints, `.nrule` verification, SHA-256 proof files, and a web-retrieval CLI. The repository also contains optional Node.js and Windows WPF interfaces.

## Build the C Runtime

Requirements:

- GCC or Clang
- `bash`
- `libm`
- `curl` only when using RAG on Linux/macOS

```bash
git clone https://github.com/Grar00t/casper.git
cd casper
bash scripts/build.sh --arch generic
```

Build and run the current C smoke checks:

```bash
bash scripts/build.sh --arch generic --smoke
```

Debug build with AddressSanitizer/UBSan when supported:

```bash
bash scripts/build.sh --debug --arch generic --smoke
```

Artifacts are written to `build/`:

```text
build/niyah
build/trainer
build/niyah_hybrid
build/casper
build/bench_niyah
build/tokenizer_test
```

The build uses C11 plus warnings-as-errors. `--arch generic` avoids host-specific `-march=native` output.

## PowerShell Wrapper

`scripts/niyah.ps1` calls the same Bash build path, so it requires a Windows environment with `bash` and GCC/Clang available.

```powershell
.\scripts\niyah.ps1 build
.\scripts\niyah.ps1 smoke
.\scripts\niyah.ps1 bench
.\scripts\niyah.ps1 train
```

## Components

| Path | Implemented role |
|---|---|
| `Core_CPP/niyah_core.c` | model allocation, inference, sampling, persistence, legacy output-head adaptation |
| `Core_CPP/niyah_train_full.c` | deterministic initialization and full-parameter truncated-BPTT training |
| `Core_CPP/niyah_train.c` | training executable |
| `Core_CPP/hybrid_reasoner.c` | terms, unification, clause solving |
| `Core_CPP/constraint_solver.c` | rational constraints and propagation |
| `Core_CPP/rule_parser.c` | `.nrule` parsing and verification |
| `Core_CPP/proof_generator.c` | SHA-256 and proof generation/verification |
| `Core_CPP/khz_q_svd.c` | numerical output gate |
| `Core_CPP/casper_rag.c` | HTTP search transport, parsing, ranking, trace/context hashing |
| `Core_CPP/casper_cli.c` | query/proof CLI |
| `Core_CPP/niyah_hybrid_main.c` | hybrid CLI |
| `tokenizer.c` | tokenizer |
| `niyah_engine_local/` | optional Node.js runtime |
| `UI_CSharp/` | optional Windows WPF UI |

## Casper Query CLI

The standalone Casper CLI accepts a query as its first argument:

```bash
./build/casper "example query"
```

Select the retrieval backend with `CASPER_BACKEND`:

```bash
CASPER_BACKEND=ddg ./build/casper "example query"
CASPER_BACKEND=bing ./build/casper "example query"
SEARXNG_HOST=search.example.test CASPER_BACKEND=searxng ./build/casper "example query"
```

`SEARXNG_HOST` is the host value consumed by the current transport. The current C transport constructs HTTPS requests; a plain-HTTP SearXNG instance is not supported by this interface as currently implemented.

On Windows the C RAG path uses WinHTTP. On POSIX it invokes the `curl` executable. Network-backed results are not deterministic because remote content and availability can change.

## Hybrid CLI

Current supported entry points include:

```bash
./build/niyah_hybrid --smoke
./build/niyah_hybrid --rag
./build/niyah_hybrid --model model.bin --interactive
```

`--rag` currently uses the backend wired by `niyah_hybrid_main.c`; do not assume the standalone Casper CLI backend selection syntax applies to this command.

## Training

```bash
./build/trainer Data_Training/sovereign_knowledge.txt 3 0.001 0.0001
```

The trainer derives `vocab_size` from the live tokenizer, applies deterministic non-zero initialization, and updates token embeddings, all attention and FFN projections, RMSNorm scales, and the LM head. A successful run writes `niyah_trained.bin`.

The training algorithm uses a deliberate detached-KV boundary: each position backpropagates through its current Q/K/V path, but future losses do not propagate into earlier cached K/V states. This is full-parameter truncated backpropagation, not exact full-sequence BPTT. The C self-check includes an overfit regression that requires loss reduction and a change in an attention backbone matrix.

An optional fifth argument supplies the deterministic initialization seed:

```bash
./build/trainer Data_Training/sovereign_knowledge.txt 3 0.001 0.0001 0x434153504552
```

## Proof Verification

The standalone Casper CLI exposes proof verification:

```bash
./build/casper --verify response.proof
```

Proof support is implemented in `Core_CPP/proof_generator.c`. A proof file verifies the data encoded by that format; it is not a general cryptographic attestation of external data, model quality, or remote sources.

## Constraint Arithmetic

Constraint values use integer numerator/denominator representation. Where available, comparisons use `__int128` cross-multiplication. The portability fallback on targets without `__int128` uses floating-point comparison and therefore must not be described as exact for every int64 input.

## CI

GitHub Actions builds and smokes the C runtime with GCC and Clang, checks Node.js source syntax, and builds the WPF UI on Windows. The C smoke path runs the trainer overfit/backbone-update regression. CI is the repository-level evidence for buildability; documentation claims are not treated as implementation evidence.
