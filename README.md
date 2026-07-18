# Casper Engine - NIYAH v3.0

A C11 inference and training engine combining a Transformer neural core with symbolic
reasoning, cryptographic proof generation, and a linear constraint solver.
No external runtime dependencies beyond libc and libm.

---

## Subsystems

### Neural Core (niyah_core.c)
- Transformer decoder with Grouped-Query Attention (GQA)
- SwiGLU feed-forward network
- Rotary Position Embeddings (RoPE)
- RMSNorm (pre-attention and pre-FFN)
- KV-cache with head-major layout
- Adam optimizer for training
- Top-p nucleus sampling with temperature control
- Model serialization to .bin format (64-byte header)

### Symbolic Reasoner (hybrid_reasoner.c)
- First-order logic terms: atoms, variables, compound terms
- Robinson unification algorithm with occurs check
- Backward chaining (Prolog-style) with configurable depth limit
- Knowledge base with clause indexing

### Constraint Solver (constraint_solver.c)
- Exact rational arithmetic (int64 numerator/denominator, never float)
- Linear inequality constraints
- Bounds propagation with iterative tightening
- Integrates with the symbolic reasoner to prune impossible bindings

### Rule Parser (rule_parser.c)
- Human-readable .nrule format for output verification
- Recursive descent parser
- Sequence-level verification: generate -> verify -> re-sample if violated

### Proof Generator (proof_generator.c)
- SHA-256 (FIPS 180-4), no external crypto library
- Proof hash: SHA-256(prompt || output || rule_file_contents)
- Machine-verifiable .proof file (NIYAH-PROOF-V1)

### Math-Coherence Gate (khz_q_svd.c)
- Energy-based coherence check (threshold >= 0.85)
- All verification gates must pass before output is accepted

### RAG Pipeline (casper_rag.c)
- Web search: DuckDuckGo HTML, Bing, or self-hosted SearXNG (no API key)
- WinHTTP on Windows; libcurl stub on Linux
- HTML snippet extraction, TF keyword relevance scoring
- SHA-256 anchor per result, chain hash over full context
- JSON trace output for UI integration

---

## Quick Start

### Linux / macOS

    git clone https://github.com/Grar00t/casper.git
    cd casper
    bash scripts/build_gcc.sh
    RUN_SMOKE=1 bash scripts/build_gcc.sh
    ./niyah_hybrid --smoke

### Windows (PowerShell)

    .\scripts\niyah.ps1 build
    .\scripts\niyah.ps1 smoke
    .\scripts\niyah.ps1 bench

### Training

    gcc -O2 -std=c11 Core_CPP/niyah_core.c Core_CPP/niyah_train.c tokenizer.c -o niyah_train -lm
    ./niyah_train Data_Training/sovereign_knowledge.txt 3 0.001

---

## CLI Reference

    # Interactive neural mode
    ./niyah_hybrid --model model.bin --rules safety.nrule --interactive

    # RAG mode (web retrieval, no model weights required)
    ./niyah_hybrid --rag
    ./niyah_hybrid --rag --backend bing
    ./niyah_hybrid --rag --backend searxng

    # Verify a proof file
    ./niyah_hybrid --verify-proof response.proof

---

## Project Layout

    casper/
    +-- Core_CPP/
    |   +-- niyah_core.h / .c          Neural Transformer engine
    |   +-- hybrid_reasoner.h / .c     Symbolic reasoner
    |   +-- constraint_solver.h / .c   Rational arithmetic constraint solver
    |   +-- rule_parser.h / .c         .nrule format parser
    |   +-- proof_generator.h / .c     SHA-256 proof system
    |   +-- khz_q_svd.h / .c           Math-coherence gate
    |   +-- casper_rag.h / .c          RAG web search pipeline
    |   +-- niyah_hybrid_main.c        Hybrid CLI entry point
    |   +-- niyah_train.c              Standalone trainer
    |   +-- bench_niyah.c              Benchmark harness
    +-- tokenizer.c                    UTF-8 tokenizer (Arabic-aware)
    +-- Data_Training/                 Training data
    +-- scripts/
    |   +-- build_gcc.sh               GCC/Clang build (Linux/macOS)
    |   +-- build_msvc.ps1             MSVC build (Windows)
    |   +-- niyah.ps1                  Unified PowerShell entry point
    +-- include/
    |   +-- casper_ffi.h               Stable C ABI for external integration
    +-- AGENTS.md                      Developer and agent guidelines

---

## Build Requirements

- C11 compiler: GCC 7+, Clang 6+, or MSVC 2019+
- No external libraries required

## Model File Format (.bin)

| Offset | Size | Field                 |
|--------|------|-----------------------|
| 0x00   | 4    | Magic (NYQH)          |
| 0x04   | 4    | Version               |
| 0x08   | 4    | Embedding dimension   |
| 0x0C   | 4    | Attention heads       |
| 0x10   | 4    | KV heads (GQA)        |
| 0x14   | 4    | Layers                |
| 0x18   | 4    | FFN multiplier        |
| 0x1C   | 4    | Vocabulary size       |
| 0x20   | 4    | Context length        |
| 0x24   | 4    | RoPE theta (float)    |
| 0x28   | 4    | RMS epsilon (float)   |
| 0x2C   | 20   | Reserved (zero)       |
| 0x40   | ...  | Weight data (float32) |

Any change to this layout requires bumping NIYAH_VER in niyah_core.h.
