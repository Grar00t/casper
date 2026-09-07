# Casper repository instructions

## Ground truth

Read the current implementation before changing it. Do not infer behavior from file names, historical documentation, deleted scripts, or previous model output.

Primary paths:

- `scripts/build.sh` — native build entry point; outputs to `build/`.
- `Core_CPP/niyah_core.c` — Transformer forward pass, serialization, sampling, output-layer training step.
- `Core_CPP/hybrid_reasoner.c` — symbolic terms, unification, clauses, backward chaining.
- `Core_CPP/constraint_solver.c` — rational linear constraints.
- `Core_CPP/rule_parser.c` — `.nrule` parser/checker.
- `Core_CPP/proof_generator.c` — SHA-256 integrity records; not signatures.
- `Core_CPP/khz_q_svd.c` — byte-structure diagnostic; not semantic verification.
- `Core_CPP/casper_rag.c` — native DuckDuckGo retrieval.
- `Core_CPP/niyah_hybrid_main.c` — native CLI and Node audit bridge.
- `niyah_engine_local/` — local Node service.
- `UI_CSharp/` — optional WPF/WebView2 shell.

## Required checks

Native changes:

```bash
bash scripts/build.sh --arch generic --lint --smoke
```

Node changes:

```bash
cd niyah_engine_local
npm ci --ignore-scripts
node --check server.js
node --check routes/niyah.js
for f in lib/*.js; do node --check "$f"; done
```

Desktop changes:

```powershell
dotnet build UI_CSharp/CasperUI.csproj -c Release
```

Do not document a fixed test count. Report the actual command and result.

## Implementation constraints

1. Keep the serialized 64-byte `NiyahConfig` layout stable unless `NIYAH_VER` is intentionally bumped and migration is handled.
2. Preserve scalar code paths when editing SIMD kernels.
3. Preserve the rational invariants and explicit overflow handling in `constraint_solver.c`.
4. Preserve occurs-check and recursion limits in the symbolic reasoner.
5. Do not treat `khz_q_svd` output as truth, ethics, safety, or semantic evidence.
6. Do not call SHA-256 integrity records signatures or authenticated proofs.
7. Do not add a zero-filled/untrained model path and present it as inference. Neural generation requires loaded weights.
8. Do not add hardcoded public IPs, cloud fallbacks, credentials, tenant/resource names, or machine-specific deployment paths.
9. The Node service must remain loopback-bound by default. Any wider bind must be an explicit operator choice.
10. Public page fetching must reject private, loopback, link-local, and special-use targets and must revalidate redirects.
11. The POSIX native retrieval path requires the `curl` executable at runtime; do not claim the complete repository has only libc/libm dependencies.
12. Prefer removing an unsupported option over retaining a CLI flag that does not work.

## Evidence rules

- Never claim a build, test, benchmark, model capability, security property, or deployment state without evidence from the current repository/run.
- Never fabricate line numbers, pass counts, artifacts, endpoints, or model names.
- Documentation must describe executable behavior, including limitations.
- If a documented feature is not implemented, either implement it with tests or remove the claim.

## Scope discipline

Fix demonstrated defects and misleading surfaces. Do not replace functioning subsystems merely for style. When an API or file is redundant and has no current caller, removal is preferred to maintaining parallel stale paths.
