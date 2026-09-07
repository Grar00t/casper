# Casper

Casper is a small experimental C11 language-model runtime plus local retrieval and rule-checking components. The repository also contains an optional Node.js service and a Windows WPF/WebView2 shell.

The implementation is the source of truth. This repository does **not** ship trained model weights and does **not** claim that hashes, SVD metrics, retrieval scores, or rule checks prove factual or semantic correctness.

## Implemented components

### Native C11

- `Core_CPP/niyah_core.c`
  - decoder-only Transformer forward pass
  - GQA attention, RoPE, RMSNorm, SwiGLU, KV cache
  - temperature/top-p sampler
  - model save/load
  - output-layer Adam training step
- `Core_CPP/hybrid_reasoner.c`
  - symbolic terms, unification, clauses, and backward chaining
- `Core_CPP/constraint_solver.c`
  - integer/rational linear-constraint support
- `Core_CPP/rule_parser.c`
  - `.nrule` parsing and prompt/output checks
- `Core_CPP/proof_generator.c`
  - SHA-256 integrity records over prompt, output, and optional rule-file bytes
  - integrity records are not signatures and provide no authenticity without a trusted external key/signature layer
- `Core_CPP/khz_q_svd.c`
  - byte-position/SVD structure diagnostic only
- `Core_CPP/casper_rag.c`
  - DuckDuckGo HTML retrieval
  - WinHTTP on Windows
  - `curl` executable on POSIX systems
  - snippet parsing, lexical relevance score, per-source SHA-256, context SHA-256
- `Core_CPP/niyah_hybrid_main.c`
  - native smoke entry point
  - `--audit-stdin` bridge for the Node service
  - interactive retrieval loop
  - model-backed generation when a valid `.bin` model is supplied

### Local Node service

`niyah_engine_local/` provides a loopback HTTP service. It binds to `127.0.0.1` by default.

Implemented search order:

1. configured SearXNG instance
2. configured Brave Search API
3. DuckDuckGo HTML fallback

Fetched result pages are restricted to public HTTP(S) targets and size-limited. The service can optionally call a local OpenAI-compatible `llama-server` through `PHI_HOST`.

### Windows desktop shell

`UI_CSharp/` is a .NET 9 WPF/WebView2 shell. It starts the local Node service when available and can invoke the native executable. There is no hardcoded public-server fallback.

## Build

### Linux

```bash
bash scripts/build.sh --arch generic --lint --smoke
```

Artifacts are written to `build/`.

Useful variants:

```bash
bash scripts/build.sh --debug --arch generic
bash scripts/build.sh --release --arch native
bash scripts/build.sh --smoke
```

### PowerShell wrapper

```powershell
.\scripts\niyah.ps1 build
.\scripts\niyah.ps1 smoke
.\scripts\niyah.ps1 bench
```

The wrapper calls the same `scripts/build.sh` pipeline and resolves artifacts from `build/`.

### Desktop shell

```powershell
dotnet build UI_CSharp/CasperUI.csproj -c Release
```

## Native CLI

```bash
# Native self-checks
./build/niyah_hybrid --smoke

# DuckDuckGo retrieval loop; type "quit" to exit
./build/niyah_hybrid --rag

# Run a real serialized model
./build/niyah_hybrid --model model.bin

# Run a model with rule checks
./build/niyah_hybrid --model model.bin --rules Data_Training/safety.nrule

# One-shot retrieval and integrity record
./build/casper_cli "query"
./build/casper_cli "query" Data_Training/safety.nrule

# Verify a stored integrity record against its embedded prompt/output/rule path
./build/casper_cli --verify casper_deadbeef.integrity
```

`--interactive` without a model is intentionally rejected. Allocating an untrained zero-filled model is not treated as inference.

## C11 audit bridge

The Node service calls:

```bash
printf '%s' '{"prompt":"hello","text":"candidate output","rules":""}' \
  | ./build/niyah_hybrid --audit-stdin
```

Example output shape:

```json
{
  "audit_passed": true,
  "integrity_sha256": "<64 lowercase hex characters>",
  "structure_energy": 0.0,
  "rule_violation": null
}
```

`audit_passed` means only that the configured `.nrule` checks returned no violation. `structure_energy` is a byte-structure diagnostic and is not used as a semantic acceptance gate.

## Training

Build first, then supply a real text corpus:

```bash
./build/trainer Data_Training/sovereign_knowledge.txt 3 0.001 0.0001
```

The current native trainer updates the output layer; it is not a full end-to-end Transformer training implementation.

To assemble tracked source text deterministically on PowerShell:

```powershell
.\scripts\build_corpus.ps1
```

The corpus script now only merges actual `.txt` files under `Data_Training/sources/`; it does not generate synthetic filler.

`get_real_data.py` is an optional data-acquisition helper that uses Hugging Face `datasets` and network access. It is not part of the runtime.

## Requirements

Native core:

- C11 compiler: GCC or Clang on the supported build script path
- libc and libm
- `cppcheck` only when `--lint` is requested
- POSIX retrieval additionally requires the `curl` executable at runtime
- Windows retrieval uses WinHTTP

Node service:

- Node.js 18+
- dependencies from `niyah_engine_local/package-lock.json`
- no cloud LLM is required; optional inference expects a reachable OpenAI-compatible local server

Desktop shell:

- .NET 9 SDK
- Microsoft WebView2 runtime/package

## Model file format

`NiyahConfig` is serialized verbatim as a 64-byte header, followed by float32 model weights.

| Offset | Size | Field |
| ---: | ---: | --- |
| `0x00` | 4 | magic `NYQH` |
| `0x04` | 4 | version |
| `0x08` | 4 | embedding dimension |
| `0x0C` | 4 | attention heads |
| `0x10` | 4 | KV heads |
| `0x14` | 4 | layers |
| `0x18` | 4 | FFN multiplier |
| `0x1C` | 4 | vocabulary size |
| `0x20` | 4 | context length |
| `0x24` | 4 | RoPE theta |
| `0x28` | 4 | RMS epsilon |
| `0x2C` | 4 | flags |
| `0x30` | 16 | reserved padding |
| `0x40` | ... | float32 weights |

Changing this serialized layout requires a `NIYAH_VER` bump.

## Verification

The repository CI definition is `.github/workflows/ci.yml` and covers:

- native build, lint, and smoke checks
- Node syntax and URL-filtering checks
- `--audit-stdin` contract
- .NET desktop build

Do not replace executable checks with fixed pass-count claims in documentation.
