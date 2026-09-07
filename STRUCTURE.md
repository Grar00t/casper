# Project Structure

Generated from `git ls-files` on `integrate-teacher-training` at
`f30f8712ae0831ac641861042ba0335c91afb000`. Documentation is not
implementation evidence.

The previous tree listing included paths that are not present
(`.agents/`, `CLAUDE.md`) and omitted `tools/` and `niyah_train_full.*`.

```text
.
├── .github/
│   └── workflows/
│       └── ci.yml
├── .vscode/
│   └── settings.json
├── Core_CPP/
│   ├── bench_niyah.c
│   ├── casper_cli.c
│   ├── casper_rag.c
│   ├── casper_rag.h
│   ├── constraint_solver.c
│   ├── constraint_solver.h
│   ├── hybrid_reasoner.c
│   ├── hybrid_reasoner.h
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
│   └── rule_parser.h
├── Data_Training/
│   ├── safety.nrule
│   ├── sources/
│   │   ├── languages/
│   │   │   └── en_ar.txt
│   │   ├── programming/
│   │   │   └── code_cpp_assembly.txt
│   │   ├── quran/
│   │   │   └── test.txt
│   │   └── test.txt
│   └── sovereign_knowledge.txt
├── Math_ASM/
│   └── avx_mult.asm
├── UI_CSharp/
│   ├── App.xaml
│   ├── App.xaml.cs
│   ├── AssemblyInfo.cs
│   ├── CasperBridge.cs
│   ├── CasperUI.csproj
│   ├── MainWindow.xaml
│   ├── MainWindow.xaml.cs
│   ├── PtyBridge.cs
│   ├── app.manifest
│   └── casper_workbench.html
├── include/
│   ├── casper_ffi.h
│   └── tokenizer.h
├── niyah_engine_local/
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
├── scripts/
│   ├── build.sh
│   ├── build_corpus.ps1
│   ├── build_trainer.ps1
│   ├── niyah.ps1
│   └── run_trainer.ps1
├── tools/
│   ├── generate_teacher_dataset.py
│   └── train_casper.py
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

Tracked raw text under `Data_Training/` is not a teacher SFT JSONL set.
`.gitignore` ignores `Data_Training/sources/languages/` and
`Data_Training/sources/programming/`, but those paths remain tracked.

| Path | Bytes |
| --- | --- |
| `Data_Training/sources/languages/en_ar.txt` | 379922 |
| `Data_Training/sources/programming/code_cpp_assembly.txt` | 123733 |
| `Data_Training/safety.nrule` | 1252 |
| `Data_Training/sovereign_knowledge.txt` | 134 |
| `Data_Training/sources/test.txt` | 17 |
| `Data_Training/sources/quran/test.txt` | 17 |

`get_real_data.py` writes `sources/languages/ar.txt`,
`sources/languages/en.txt`, and `sources/programming/code_c.txt`. Those
names are not the tracked files above.

Do not feed these dumps into `tools/train_casper.py`. That path requires
JSONL with `instruction` and `response`.

## Build artifacts

`scripts/build.sh` writes binaries under `build/`, which is ignored.
`scripts/build_trainer.ps1` still points at `Core_CPP/trainer.cpp`,
which is not in the tree. The live C trainer is `./build/trainer` from
`scripts/build.sh`.

## Verified integration contracts

- SFT prompt text is `Instruction:\n...\nResponse:\n` in both
  `tools/train_casper.py` and `Core_CPP/niyah_hybrid_main.c`.
- Tokenizer vocabulary v2 size is 1756: historical ids `0..1499`, byte
  fallback `1500..1755`.
- C `tokenizer_encode` wraps BOS/EOS. Hybrid inference strips a trailing
  EOS from the prompt before generation. Python SFT builds
  `[BOS] + prompt_ids + response_ids + [EOS]` with prompt/response
  encoded `bos=False, eos=False`.
- `build/niyah` self-check includes the C trainer overfit/backbone
  regression. `build/casper --self-check` covers CLI ranking only.
- `scripts/build.sh --smoke` runs `build/niyah` and
  `build/niyah_hybrid --smoke`. It does not invoke `build/casper`.
  CI additionally runs `./build/casper --self-check`.

## Open integration defects

Recorded for specialist branches. Not fixed on `audit/integration`.

| Owner | Defect |
| --- | --- |
| Agent 3 ML/data | `tools/generate_teacher_dataset.py` default `--max-tokens` is 256. Empty final `content` was observed at that cap; 1024 completed those requests. |
| Agent 3 ML/data | Generator stores `message.content` only, but resume/load paths do not reject existing JSONL rows that contain `reasoning` / `reasoning_content`. |
| Agent 4 repo/CI | `scripts/build_trainer.ps1` compiles missing `Core_CPP/trainer.cpp`. `scripts/run_trainer.ps1` expects `Core_CPP/trainer.exe`. |
| Agent 4 repo/CI | Tracked corpus files under gitignored `languages/` and `programming/` directories. |
| Agent 4 repo/CI | `niyah_engine_local/server.js` sets `Access-Control-Allow-Origin: *` and serves `GET /fetch?url=` as an open proxy; listen address is not restricted to loopback. Header comment names an Azure VM / nginx deployment. |
| Agent 2 native | `Math_ASM/avx_mult.asm` is not referenced by `scripts/build.sh`. |
| Agent 2 native | Two JSON serialisers: `casper_rag_to_json` vs CLI JSON in `casper_cli.c`. |
