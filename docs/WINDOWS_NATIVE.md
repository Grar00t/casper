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
receipt before returning the response. Receipts currently use
`NIYAH-PROOF-V2` and cryptographically bind prompt bytes, response bytes
and the exact rule-file hash. This establishes integrity, not authenticity,
legal validity, factual accuracy, or model correctness.

`--inspect-gguf` only memory-maps a GGUF file read-only and checks a minimal
v2/v3 header and file range. **It cannot load tensors, dequantize GGUF weights,
or run GGUF inference.** Native `.bin` inference remains separate.

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

The pool allocation covers weights, KV cache, forward scratch and
the new generation sampler workspace. Auxiliary non-hot paths can allocate.
A passing smoke/ASan test does not establish absence of all memory errors.
The public `include/casper_ffi.h` contract predates these changes and
is not evidence that a loadable FFI DLL is shipped here.
The C native model format is not GGUF.
Production release requires model checkpoints, end-to-end source/citation
checks, concurrency and extended malformed-input testing, ABI compatibility
verification and a complete supported toolchain/CI matrix.
