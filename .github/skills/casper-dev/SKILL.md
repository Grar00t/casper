---
name: casper-dev
description: Development workflow for the Casper repository. Use for C11 engine, local Node service, or WPF shell changes.
---

# Casper development

## Source of truth

- `scripts/build.sh`: native C11 build entry point; outputs to `build/`.
- `Core_CPP/niyah_core.c`: Transformer inference/training core.
- `Core_CPP/niyah_hybrid_main.c`: model CLI, smoke tests, RAG loop, and `--audit-stdin` bridge.
- `Core_CPP/rule_parser.c`: `.nrule` parser and output checks.
- `Core_CPP/proof_generator.c`: SHA-256 integrity records. These are hashes, not signatures or proofs of semantic correctness.
- `Core_CPP/casper_rag.c`: C retrieval transport/parser/ranker.
- `niyah_engine_local/`: loopback-only Node/Express service by default.
- `UI_CSharp/`: optional Windows WPF/WebView2 shell.

## Required verification

```bash
bash scripts/build.sh --arch generic --lint --smoke
```

For Node files:

```bash
cd niyah_engine_local
npm ci --ignore-scripts
node --check server.js
node --check routes/niyah.js
for f in lib/*.js; do node --check "$f"; done
```

For the desktop shell:

```powershell
dotnet build UI_CSharp/CasperUI.csproj -c Release
```

GitHub Actions runs these checks in `.github/workflows/ci.yml`.

## Constraints

1. Do not invent test counts, benchmark results, model quality, or security guarantees.
2. No model weights are stored in this repository. Neural generation requires a valid model file loaded with `--model`.
3. Treat KHZ-Q SVD output as a byte-structure diagnostic only; it is not semantic, ethical, factual, or safety verification.
4. Treat SHA-256 integrity records as tamper-detection data only; they are not authenticated signatures.
5. The Node server binds to `127.0.0.1` unless `HOST` is explicitly changed.
6. Do not add hardcoded cloud endpoints, public IP fallbacks, credentials, or deployment-specific resource names.
7. Read implementation before changing documentation. README claims must match executable code.
