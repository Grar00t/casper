# Native Casper Engine on Windows

## Build and verify

Run from a normal PowerShell prompt on Windows x64 with Visual Studio 2022
Community or Build Tools installed with the MSVC C toolchain:

```powershell
cd D:\projects\casper
.\scripts\build_windows.cmd --smoke
.\scripts\build_windows.cmd --asan
```

The build invokes `VsDevCmd.bat`, requires no Python, GCC, or Bash,
uses /std:c11 /W4 /WX, and links WinHTTP from the Windows SDK.
Executables and test fixtures stay under `build/`.

## CLI

```powershell
.\build\casper_engine.exe --smoke
.\build\casper_engine.exe --inspect-gguf .\my-model.gguf
.\build\casper_engine.exe --verify-proof .\receipt.proof
.\build\casper_engine.exe --verify-proof .\receipt.proof .\rules.nrule
.\build\casper_engine.exe --interactive .\native-model.bin
```

`--model` is an alias for `--interactive`. An actual native NIYAH .bin
checkpoint is required; the CLI does not silently create an untrained model.
Interactive generation must save and self-check a `casper-response-XXXXXX.proof`
receipt before returning the response. New receipts use `NIYAH-PROOF-V1`:
`SHA-256(prompt bytes || output bytes || exact rule-file bytes)`.
Legacy V2 verification remains available. Receipts establish integrity, not
authenticity, legal validity, factual accuracy, or model correctness.

`--inspect-gguf` checks the v2/v3 GGUF header without inference.
`--gguf-forward <model.gguf> <token-id> [token-id...]` runs a native dense
LLaMA/Qwen3-style forward pass and reports finite logits/argmax. Supported
GGML tensor types are explicitly validated on load (including F32, F16 and
selected Q4/Q5/Q6 variants); unsupported model layouts fail closed. GGUF
weights and all forward workspace use a single allocated runtime `_pool`,
with file mapping closed after loading. This is a bounded forward-test path,
not tokenizer-driven chat generation or independent numerical parity proof.
Native `.bin` inference remains separate.

## Retrieval policy

RAG is disabled by default. The only supported backend is a manually
configured local SearXNG instance listening on numeric IPv4 loopback:

```powershell
$env:SEARXNG_HOST='127.0.0.1:8888'
.\build\casper_engine.exe --rag
```

No automatic Bing/DDG fallback is permitted. WinHTTP bypasses proxies,
disables redirects, and binds to 127.0.0.1 explicitly. A local SearXNG
instance can itself query the public internet depending on its configuration.
Its own outbound security is not proven by this engine.

## Limitations and verification scope

The GGUF forward path allocates a single contiguous `_pool` for the
validated tensor bytes, KV cache, forward scratch, and logits, and releases
the read-only model mapping after loading. Auxiliary non-hot setup paths can
allocate. The native `.bin` path retains its distinct format.
The Windows Release build creates `casper.dll` and tests its exported FFI
functions with `test_ffi_host.exe`. A passing smoke/ASan test does not
establish absence of every memory error.
Production release requires model checkpoints, end-to-end source/citation
checks, concurrency and extended malformed-input testing, ABI compatibility
verification and a complete supported toolchain/CI matrix.
