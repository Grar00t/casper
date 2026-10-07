# Casper / NIYAH

Casper is a C11 neural/runtime and symbolic reasoning codebase with rational constraints, `.nrule` verification, SHA-256 integrity receipts, and a web-retrieval CLI. The repository also contains optional Node.js and Windows WPF interfaces.

## Build the C Runtime

Requirements:

- GCC or Clang
- `bash`
- `libm`
- `curl` only when using RAG on Linux/macOS

```bash
git clone https://github.com/Grar00t/casper.git
cd casper
bash scripts/build.sh --arch generic --smoke
```

Debug build with AddressSanitizer/UBSan when supported:

```bash
bash scripts/build.sh --debug --arch generic --smoke
```

Artifacts are written under `build/`.

## Training

Casper now has two explicit training paths. They are not equivalent and are documented separately.

### 1. Native Windows/CUDA full-sequence training

`tools/train_casper.py` mirrors the C inference architecture in PyTorch, runs ordinary autograd through the complete causal sequence, updates all parameters with AdamW, and exports the native NIYAH `.bin` layout consumed by `niyah_load()`.

This is the recommended path for distillation/SFT from a local teacher such as a llama.cpp server because it can use a Windows CUDA-enabled PyTorch build directly.

Input is JSONL with at least `instruction` and `response` fields:

```json
{"id":"casper-000001","instruction":"Explain SHA-256.","response":"..."}
```

Windows example:

```powershell
python tools\train_casper.py C:\path\to\casper_seed.jsonl `
  --device cuda `
  --epochs 12 `
  --output casper_trained.bin `
  --checkpoint casper_training.pt
```

Or through the wrapper:

```powershell
.\scripts\niyah.ps1 train C:\path\to\casper_seed.jsonl 12 -Device cuda
```

Resume is guarded by both model configuration and dataset SHA-256:

```powershell
.\scripts\niyah.ps1 train C:\path\to\casper_seed.jsonl 20 -Device cuda -Resume
```

The Python trainer performs a deterministic ID-based train/validation split, supervises response tokens rather than prompt tokens, supports mixed precision and gradient accumulation, clips gradients, writes a resumable `.pt` checkpoint, exports a C-runtime-compatible `.bin`, and writes a manifest with dataset/model hashes and losses.

PyTorch is a **training-only** dependency. The exported C runtime does not depend on PyTorch.

### 2. Native C full-parameter truncated-BPTT trainer

The C trainer remains implemented and test-covered:

```bash
./build/trainer Data_Training/sovereign_knowledge.txt 3 0.001 0.0001
```

It derives `vocab_size` from the live tokenizer, applies deterministic non-zero initialization, and updates token embeddings, attention/FFN projections, RMSNorm scales, and the LM head. A successful run writes `niyah_trained.bin`.

Its causal KV cache is a deliberate detached-gradient boundary: future losses do not backpropagate into earlier cached K/V states. This is full-parameter **truncated** backpropagation, not exact full-sequence BPTT. The C self-check includes an overfit regression requiring loss reduction and a changed attention backbone matrix.

An optional fifth argument supplies the deterministic initialization seed:

```bash
./build/trainer Data_Training/sovereign_knowledge.txt 3 0.001 0.0001 0x434153504552
```

PowerShell exposes this reference path separately:

```powershell
.\scripts\niyah.ps1 train-c Data_Training\sovereign_knowledge.txt
```

## Tokenizer Contract

The tokenizer preserves historical ids `0..1499` and appends ids `1500..1755` as raw UTF-8 byte fallback tokens. Known English/domain tokens and Arabic codepoints keep their previous ids. Previously unseen English words, whitespace, emoji, CJK, and other UTF-8 text therefore no longer collapse to `<UNK>`.

The current live vocabulary size is **1756**. The build-time tokenizer self-test requires exact UTF-8 round trips for known words, unseen English, Arabic, non-Arabic Unicode, and whitespace.

Old checkpoints were not trained on the appended byte-fallback ids; retraining is recommended before relying on those ids.

## PowerShell Wrapper

```powershell
.\scripts\niyah.ps1 build
.\scripts\niyah.ps1 smoke
.\scripts\niyah.ps1 bench
.\scripts\niyah.ps1 train C:\path\to\teacher.jsonl -Device cuda
.\scripts\niyah.ps1 train-c Data_Training\sovereign_knowledge.txt
```

C build/run and `train-c` require `bash` plus GCC/Clang. Full-model `train` runs natively through Python and does not require Bash.

## Components

| Path | Implemented role |
|---|---|
| `Core_CPP/niyah_core.c` | allocation, inference, sampling, persistence, legacy output-head adaptation |
| `Core_CPP/niyah_train_full.c` | deterministic initialization and full-parameter truncated-BPTT C training |
| `Core_CPP/niyah_train.c` | C training executable |
| `tools/train_casper.py` | full-sequence PyTorch/CUDA training, checkpoint/resume, native `.bin` export |
| `tokenizer.c` | deterministic UTF-8 tokenizer with byte fallback |
| `Core_CPP/hybrid_reasoner.c` | terms, Robinson unification, occurs-check, backward chaining |
| `Core_CPP/constraint_solver.c` | rational constraints and propagation |
| `Core_CPP/rule_parser.c` | deterministic `.nrule` text matching and replacement/rejection |
| `Core_CPP/proof_generator.c` | SHA-256 integrity receipt generation/verification |
| `Core_CPP/khz_q_svd.c` | numerical text-shape/coherence heuristic |
| `Core_CPP/casper_rag.c` | HTTP search transport, parsing, ranking, trace/context hashing |
| `Core_CPP/casper_cli.c` | query/integrity-receipt CLI |
| `Core_CPP/niyah_hybrid_main.c` | hybrid CLI and C11 audit bridge |
| `niyah_engine_local/` | optional Node.js runtime |
| `UI_CSharp/` | optional Windows WPF UI |

## Security and Semantic Boundaries

The repository uses some historical names such as `KHZ_Q`, `penalty_nasl`, and "coherence". These identifiers must not be interpreted as evidence of factual truth, ethics, policy compliance, or formal correctness.

- Casper/NIYAH is a native user-space program. It is not a Ring-0 kernel component and is not bare-metal software.
- `NiyahModel` uses a contiguous pool for neural weights, KV cache, layer metadata, and scratch space. Other subsystems still use ordinary heap allocation where required. Single-pool model allocation is not a memory-safety proof.
- `khz_q_svd.c` is a numerical heuristic over byte-frequency buckets and retained spectral energy. It does not understand meaning or prove that output is true or ethical.
- `.nrule` is a deterministic text-rule filter (`CONTAINS`, equality, replacement, rejection). It is separate from the Prolog-like symbolic reasoner.
- `hybrid_reasoner.c` and `constraint_solver.c` are implemented subsystems, but the current neural generation path does not invoke them as final authorities. Wiring them into generation requires an explicit semantic contract rather than an implicit claim.
- "Sovereign" is a deployment/design objective (local execution, minimized dependencies), not a formally verified security property.
- SIMD paths can change floating-point reduction order. Identical seeds do not by themselves establish bit-identical logits across compilers or architectures.

## Casper Query CLI

```bash
./build/casper "example query"
```

Select the retrieval backend with `CASPER_BACKEND`:

```bash
CASPER_BACKEND=ddg ./build/casper "example query"
CASPER_BACKEND=bing ./build/casper "example query"
SEARXNG_HOST=search.example.test CASPER_BACKEND=searxng ./build/casper "example query"
```

On Windows the C RAG path uses WinHTTP. On POSIX it invokes the `curl` executable. Network-backed results are not deterministic because remote content and availability can change.

## Optional Node Runtime Security

The optional `niyah_engine_local/` service fails closed for browser CORS and the legacy URL fetch endpoint unless explicit allowlists are configured.

- `CASPER_CORS_ORIGINS`: comma-separated exact browser origins allowed to receive CORS responses.
- `CASPER_FETCH_ALLOW_HOSTS`: comma-separated exact hostnames permitted for `GET /fetch?url=`.
- Redirects are rejected by the legacy fetch endpoint so an allowed host cannot redirect into an unapproved target.

Example:

```bash
CASPER_CORS_ORIGINS=http://127.0.0.1:3000,http://localhost:3000 \
CASPER_FETCH_ALLOW_HOSTS=example.com,www.example.com \
node niyah_engine_local/server.js
```

## Hybrid CLI and Audit Bridge

```bash
./build/niyah_hybrid --smoke
./build/niyah_hybrid --rag
./build/niyah_hybrid --model casper_trained.bin --interactive
```

The Node runtime uses a bounded stdin JSON audit contract:

```bash
printf '%s' '{"prompt":"hello","text":"candidate answer","rules":"Data_Training/safety.nrule"}' \
  | ./build/niyah_hybrid --audit-stdin
```

The audit response separates the local KHZ_Q/text-rule gate from receipt verification. `verified` and `local_gate_verified` mean the local heuristic/rule checks passed and a receipt was created; `receipt_verified` remains false until a saved receipt is independently verified. This path explicitly reports `factual_truth_verified=false`.

`--rag` currently uses the backend wired by `niyah_hybrid_main.c`; do not assume the standalone Casper CLI backend selection syntax applies to this command.
```

## Integrity Receipt Verification

New receipts use `NIYAH-PROOF-V2`. The receipt binds:

- SHA-256 of the exact prompt bytes,
- SHA-256 of the exact output bytes,
- SHA-256 of the exact applied rule-file bytes, or SHA-256 of an empty rule set.

Prompt and output payloads are stored as hex in the receipt so multiline text is unambiguous.

For a receipt with no rules:

```bash
./build/casper --verify response.proof
```

For a rules-bound receipt, supply the rule file whose current bytes must match the bound hash:

```bash
./build/casper --verify response.proof Data_Training/safety.nrule
```

The CLI separates `receipt_valid`, `rules_bound`, `rules_verified`, and final `valid`. A receipt is an integrity checksum/receipt. It is not a digital signature, authenticity proof, factual-truth proof, external-source attestation, model-quality certificate, or legal/compliance determination. A proof file verifies only the data encoded by that format and the supplied rule bytes when rules are bound.

## Constraint Arithmetic

Constraint values use integer numerator/denominator representation. Where available, comparisons use `__int128` cross-multiplication. The portability fallback on targets without `__int128` uses floating-point comparison and therefore is not exact for every int64 input.

## CI

GitHub Actions builds and smokes the C runtime with GCC and Clang, runs the debug sanitizer smoke path, exercises the `--audit-stdin` safe/reject contract, syntax-checks the Python training tools and Node.js sources, and builds the WPF UI on Windows. The C smoke path includes the trainer overfit/backbone-update regression and proof receipt smoke checks. CI is repository-level evidence for the exercised build/test contracts; documentation claims are not implementation evidence.
