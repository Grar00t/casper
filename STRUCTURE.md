# Project Structure

Generated from the tree, not written by hand. Regenerate after any file move:

```bash
git ls-files | tree --fromfile -a --noreport
```

The previous revision declared 7 files that do not exist
(`Core_CPP/casper_core.cpp`, `matrix.cpp`, `trainer.cpp`, `trainer_real.cpp`,
`trainer_real_fix.cpp`, `scripts/build_msvc.ps1`, `test_generation.c`) and
omitted 19 that do. It also claimed "6 directories, 37 files".

```text
.
├── .github/
├── .vscode/
│   └── settings.json
├── Core_CPP/
│   ├── bench_niyah.c
│   ├── casper_cli.c              # CLI entry point, emits the JSON contract
│   ├── casper_chronicle.c        # offline evidence store and reasoner
│   ├── casper_chronicle.h        # Chronicle public C API
│   ├── casper_chronicle_main.c   # Chronicle CLI entry point
│   ├── casper_rag.c              # search transport, parser, ranker
│   ├── casper_rag.h              # the live search contract
│   ├── constraint_solver.c
│   ├── constraint_solver.h
│   ├── hybrid_reasoner.c
│   ├── hybrid_reasoner.h
│   ├── khz_q_ising.c
│   ├── khz_q_ising.h
│   ├── khz_q_svd.c
│   ├── khz_q_svd.h
│   ├── niyah_core.c
│   ├── niyah_core.h
│   ├── niyah_hybrid_main.c
│   ├── niyah_main.c
│   ├── niyah_train.c
│   ├── niyah_train_full.c
│   ├── niyah_train_full.h
│   ├── proof_generator.c
│   ├── proof_generator.h
│   ├── rule_parser.c
│   ├── rule_parser.h
│   ├── rule_source_guard.c
│   └── rule_source_guard.h
├── Data_Training/
│   ├── safety.nrule                      # 1252 B
│   ├── sources/
│   │   ├── languages/
│   │   │   └── en_ar.txt                 # 379922 B
│   │   └── programming/
│   │       └── code_cpp_assembly.txt     # 123733 B
│   └── sovereign_knowledge.txt           # 134 B
├── UI_CSharp/
│   ├── App.xaml
│   ├── App.xaml.cs
│   ├── AssemblyInfo.cs
│   ├── CasperBridge.cs           # window.casper host object for WebView2
│   ├── CasperUI.csproj
│   ├── MainWindow.xaml
│   ├── MainWindow.xaml.cs
│   ├── PtyBridge.cs              # ConPTY session
│   ├── app.manifest
│   └── casper_workbench.html
├── docs/
│   └── CHRONICLE_CONTRACT.md      # versioned input/reasoning/receipt contract
├── include/
│   ├── casper_ffi.h
│   └── tokenizer.h
├── niyah_engine_local/           # Node service, no model weights
│   ├── lib/
│   │   ├── memory.js
│   │   ├── niyahEngine.js
│   │   ├── phiEngine.js
│   │   ├── reasoner.js
│   │   ├── relevance.js
│   │   └── searchProvider.js
│   ├── routes/
│   │   └── niyah.js
│   ├── package-lock.json
│   ├── package.json
│   └── server.js
├── tests/
│   └── fixtures/
│       └── chronicle_ahmed_ar.txt
├── scripts/
│   ├── build.sh                  # the only build entry point
│   ├── build_corpus.ps1
│   ├── build_trainer.ps1
│   ├── niyah.ps1
│   └── run_trainer.ps1
├── .gitattributes
├── .gitignore
├── AGENTS.md
├── README.md
├── STRUCTURE.md
├── get_real_data.py
├── index.html
└── tokenizer.c
```

## Training data

Measured with `find Data_Training -type f -printf '%s %p\n'`, not inferred from
the top-level listing:

| Path | Bytes |
| --- | --- |
| `Data_Training/sources/languages/en_ar.txt` | 379922 |
| `Data_Training/sources/programming/code_cpp_assembly.txt` | 123733 |
| `Data_Training/safety.nrule` | 1252 |
| `Data_Training/sovereign_knowledge.txt` | 134 |
| **total** | **505041** |

It is raw text, not instruction pairs. It is enough to exercise the tokenizer and
the byte-level path; it is not a supervised fine-tuning set.

`get_real_data.py` writes `sources/languages/ar.txt`, `sources/languages/en.txt`
and `sources/programming/code_c.txt` -- none of which are the files above. A
fetch therefore grows a second, parallel set of filenames instead of extending
the corpus. Pick one naming scheme before the next fetch.

## Build artifacts

`scripts/build.sh` writes every binary to `build/`, which is ignored. Nothing
compiled belongs in this tree. Two binaries were previously committed because
their names were absent from `.gitignore`:

| Path | Size | Status |
| --- | --- | --- |
| `casper_engine` | 137880 B | removed |
| `Core_CPP/trainer` | 28824 B | removed |

Both are gone from the tree and still present in history, so a clone still pays
for them. Removing them from history rewrites every commit id and is a separate,
deliberate decision.

## Removed in this cleanup

| Path | Reason |
| --- | --- |
| `casper_engine` | compiled artifact |
| `Core_CPP/trainer` | compiled artifact |
| `.gitmodules` | declared `proof/llm-core-logic`, absent from the tree, so `clone --recursive` failed |
| `Core_CPP/build_gcc.sh` | superseded by `scripts/build.sh` |
| `scripts/build_gcc.sh` | superseded by `scripts/build.sh` |
| `scripts/gen_rag.py` | hardcoded `C:/Users/sulaimanalshammari/...`; rewrote `Core_CPP/casper_rag.c` from byte literals and stopped after the header, truncating the file |
| `scripts/fix_rag.py` | hardcoded path; both substitutions it applied are already present in the committed source |
| `Core_CPP/casper_search.h` | 7054 B. `casper_search`, `casper_build_context`, `casper_ctx_free`, `casper_ctx_to_json` and `casper_search_available` were declared, implemented nowhere, and referenced by no source file: `grep -rn casper_search --include='*.c'` returned 0, and a second pass over `*.h`, `*.cs`, `*.html`, `*.ps1` and `*.md` returned only this document's own note |

## Known defects, not yet fixed

| Where | Defect |
| --- | --- |
| `Core_CPP/casper_rag.c` vs `Core_CPP/casper_cli.c` | two JSON serialisers. `casper_rag_to_json` emits `query, confidence, elapsed_ms, chain_hash, n_sources, n_steps`; the CLI emits those plus `answer`, `proof` and `sources[]` |


## Legacy Node security boundary

The legacy Node fetch endpoint requires `CASPER_FETCH_ALLOW_HOSTS`, accepts only HTTP/HTTPS targets, rejects redirects, and CORS is allowlist-based through `CASPER_CORS_ORIGINS`. These controls are present on `main`; repository CI passed after the security change was merged.


## Removed placeholder corpus files

`Data_Training/sources/test.txt` and `Data_Training/sources/quran/test.txt` were identical 17-byte `hello world test` placeholders. They were not representative training data and the latter path falsely implied Quranic content, so both were removed.
