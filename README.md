# Casper / NIYAH

Casper is a C11 neural/runtime and symbolic reasoning codebase with rational constraints, `.nrule` verification, SHA-256 proof files, and a web-retrieval CLI. The repository also contains optional Node.js and Windows WPF interfaces.

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

There are two distinct training paths. They are intentionally not presented as equivalent.

### Full-model training (supported for teacher distillation/SFT)

`tools/train_casper.py` reproduces the C runtime architecture in PyTorch, optimizes **all** model parameters with AdamW, and exports the native `NIYAH` `.bin` layout consumed by `niyah_load()`.

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

Or through the PowerShell wrapper:

```powershell
.\scripts\niyah.ps1 train C:\path\to\casper_seed.jsonl 12
```

Resume only when the dataset and architecture match the checkpoint:

```powershell
.\scripts\niyah.ps1 train C:\path\to\casper_seed.jsonl 20 -Resume
```

The trainer:

- performs a deterministic ID-based train/validation split;
- supervises assistant/response tokens rather than prompt tokens;
- supports CUDA mixed precision and gradient accumulation;
- clips gradients;
- writes a resumable `.pt` checkpoint;
- exports a C-runtime-compatible `.bin` after every epoch;
- writes a manifest with dataset/model SHA-256 values and train/validation losses.

PyTorch is a **training-only** dependency. The exported C inference runtime does not depend on PyTorch.

### C compatibility trainer

`build/trainer` and `niyah_train_step()` remain for ABI compatibility and small diagnostics. They update **only `lm_head`**, not the transformer body. They are not the supported path for teacher distillation or full-model training.

```bash
./build/trainer Data_Training/sovereign_knowledge.txt 3 0.001 0.0001
```

The compatibility trainer now uses deterministic non-zero initialization and the live tokenizer vocabulary instead of a zero-initialized model.

## Tokenizer Contract

`tokenizer.c` keeps the historical ids `0..1499` and appends ids `1500..1755` as raw UTF-8 byte fallback tokens. Known compact English/domain tokens and Arabic codepoints retain their existing ids. Previously unseen English words, whitespace, emoji, CJK, and other UTF-8 text therefore no longer collapse to `<UNK>`.

The current live vocabulary size is 1756. The build-time tokenizer self-test requires exact UTF-8 round trips for known words, unseen English, Arabic, non-Arabic Unicode, and whitespace.

Existing old checkpoints were not trained on the new byte-fallback ids; retraining is recommended before relying on those ids.

## PowerShell Wrapper

```powershell
.\scripts\niyah.ps1 build
.\scripts\niyah.ps1 smoke
.\scripts\niyah.ps1 bench
.\scripts\niyah.ps1 train C:\path\to\teacher.jsonl
.\scripts\niyah.ps1 train-head Data_Training\sovereign_knowledge.txt
```

C build/run actions require `bash` plus GCC/Clang. Full-model `train` runs natively through Python and does not require Bash.

## Components

| Path | Implemented role |
|---|---|
| `Core_CPP/niyah_core.c` | allocation, forward computation, sampling, compatibility head-only Adam step, save/load |
| `Core_CPP/niyah_init.c` | deterministic non-zero C weight initialization |
| `tools/train_casper.py` | full-model PyTorch training, checkpoint/resume, native `.bin` export |
| `Core_CPP/niyah_train.c` | compatibility head-only C training executable |
| `tokenizer.c` | deterministic UTF-8 tokenizer with byte fallback |
| `Core_CPP/hybrid_reasoner.c` | terms, unification, clause solving |
| `Core_CPP/constraint_solver.c` | rational constraints and propagation |
| `Core_CPP/rule_parser.c` | `.nrule` parsing and verification |
| `Core_CPP/proof_generator.c` | SHA-256 and proof generation/verification |
| `Core_CPP/khz_q_svd.c` | numerical output gate |
| `Core_CPP/casper_rag.c` | HTTP search transport, parsing, ranking, trace/context hashing |
| `Core_CPP/casper_cli.c` | query/proof CLI |
| `Core_CPP/niyah_hybrid_main.c` | hybrid CLI |
| `niyah_engine_local/` | optional Node.js runtime |
| `UI_CSharp/` | optional Windows WPF UI |

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

## Hybrid CLI

```bash
./build/niyah_hybrid --smoke
./build/niyah_hybrid --rag
./build/niyah_hybrid --model casper_trained.bin --interactive
```

## Proof Verification

```bash
./build/casper --verify response.proof
```

A proof file verifies the data encoded by that format; it is not a general cryptographic attestation of external data, model quality, or remote sources.

## Constraint Arithmetic

Constraint values use integer numerator/denominator representation. Where available, comparisons use `__int128` cross-multiplication. The portability fallback on targets without `__int128` uses floating-point comparison and therefore is not exact for every int64 input.

## CI

GitHub Actions builds and smokes the C runtime with GCC and Clang, compiles the Python training tool for syntax, checks Node.js source syntax, and builds the WPF UI on Windows. CI is repository-level evidence for buildability; documentation claims are not implementation evidence.
