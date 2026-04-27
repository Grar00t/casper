#!/usr/bin/env bash
# =============================================================================
# build_gcc.sh — Build NIYAH-CORE and trainer with GCC or Clang
#
# Usage:
#   ./scripts/build_gcc.sh                   # auto-detect arch + Release
#   ./scripts/build_gcc.sh --debug           # Debug with sanitizers
#   ./scripts/build_gcc.sh --compiler clang  # force clang/clang++
#   ./scripts/build_gcc.sh --arch arm64      # force ARM64 flags
#
# Outputs:
#   Core_CPP/niyah     — inference smoke-test binary
#   Core_CPP/trainer   — training simulation binary
# =============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG="release"
FORCE_ARCH=""
CC_OVERRIDE=""
CXX_OVERRIDE=""

# ── argument parsing ──────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --debug)    CONFIG="debug"    ;;
        --release)  CONFIG="release"  ;;
        --arch)     FORCE_ARCH="$2"; shift ;;
        --compiler) CC_OVERRIDE="$2"; CXX_OVERRIDE="${2/gcc/g++}"; CXX_OVERRIDE="${CXX_OVERRIDE/clang/clang++}"; shift ;;
        *) echo "[build_gcc] Unknown flag: $1"; exit 1 ;;
    esac
    shift
done

# ── detect compiler ───────────────────────────────────────────────────────────
if [[ -n "$CC_OVERRIDE" ]]; then
    CC="$CC_OVERRIDE"
    CXX="$CXX_OVERRIDE"
elif command -v gcc &>/dev/null; then
    CC=gcc; CXX=g++
elif command -v clang &>/dev/null; then
    CC=clang; CXX=clang++
else
    echo "[build_gcc] ERROR: no C compiler found (gcc / clang)."
    echo "  Ubuntu/Debian: sudo apt-get install build-essential"
    echo "  macOS:         xcode-select --install"
    exit 1
fi

echo "[build_gcc] CC  = $($CC --version | head -1)"
echo "[build_gcc] CXX = $($CXX --version | head -1)"

# ── detect architecture ───────────────────────────────────────────────────────
if [[ -n "$FORCE_ARCH" ]]; then
    ARCH="$FORCE_ARCH"
else
    UNAME_M="$(uname -m)"
    case "$UNAME_M" in
        x86_64)          ARCH="x86_64"  ;;
        aarch64|arm64)   ARCH="arm64"   ;;
        *)               ARCH="generic" ;;
    esac
fi
echo "[build_gcc] Arch   = $ARCH"
echo "[build_gcc] Config = $CONFIG"

# ── architecture-specific flags ───────────────────────────────────────────────
case "$ARCH" in
    x86_64)
        ARCH_FLAGS="-mavx2 -mfma -march=native"
        ;;
    arm64)
        ARCH_FLAGS="-march=armv8.2-a"
        ;;
    *)
        ARCH_FLAGS=""
        ;;
esac

# ── warning flags ─────────────────────────────────────────────────────────────
WARN_C="-Wall -Wextra -Werror -Wstrict-prototypes -Wmissing-prototypes \
        -Wcast-align -Wwrite-strings -Wshadow -pedantic"
WARN_CXX="-Wall -Wextra -Werror -Wcast-align -Wshadow"

# ── config flags ──────────────────────────────────────────────────────────────
if [[ "$CONFIG" == "release" ]]; then
    OPT_C="-O3 -DNDEBUG"
    OPT_CXX="-O3 -DNDEBUG"
    LINK_FLAGS="-flto"
else
    OPT_C="-O0 -g3 -DDEBUG"
    OPT_CXX="-O0 -g3 -DDEBUG"
    LINK_FLAGS=""
    # Add sanitizers if supported
    if $CC -fsanitize=address -x c /dev/null -o /dev/null 2>/dev/null; then
        OPT_C="$OPT_C -fsanitize=address,undefined"
        OPT_CXX="$OPT_CXX -fsanitize=address,undefined"
        LINK_FLAGS="$LINK_FLAGS -fsanitize=address,undefined"
        echo "[build_gcc] Sanitizers: ASan + UBSan enabled"
    else
        echo "[build_gcc] Sanitizers not available, skipping"
    fi
fi

# ── build function ────────────────────────────────────────────────────────────
build_c() {
    local out="$1"; shift
    local sources=("$@")
    echo ""
    echo "[build_gcc] Building: $out"
    echo "  cmd> $CC $OPT_C $ARCH_FLAGS $WARN_C ${sources[*]} -o $out -lm"
    # shellcheck disable=SC2086
    $CC $OPT_C $ARCH_FLAGS $WARN_C "${sources[@]}" -o "$out" -lm $LINK_FLAGS
    local sz
    sz=$(stat -c%s "$out" 2>/dev/null || stat -f%z "$out" 2>/dev/null || echo "?")
    echo "[build_gcc] OK  $out  ($(( sz / 1024 )) KB)"
}

build_cxx() {
    local out="$1"; shift
    local sources=("$@")
    echo ""
    echo "[build_gcc] Building: $out"
    echo "  cmd> $CXX -std=c++17 $OPT_CXX $ARCH_FLAGS $WARN_CXX ${sources[*]} -o $out"
    # shellcheck disable=SC2086
    $CXX -std=c++17 $OPT_CXX $ARCH_FLAGS $WARN_CXX "${sources[@]}" -o "$out" $LINK_FLAGS
    local sz
    sz=$(stat -c%s "$out" 2>/dev/null || stat -f%z "$out" 2>/dev/null || echo "?")
    echo "[build_gcc] OK  $out  ($(( sz / 1024 )) KB)"
}

# ── targets ───────────────────────────────────────────────────────────────────
build_c \
    "$ROOT/Core_CPP/niyah" \
    "$ROOT/Core_CPP/niyah_core.c" \
    "$ROOT/Core_CPP/niyah_main.c"

# Hybrid binary — full neuro-symbolic engine (neural + symbolic + KHZ_Q + proof + rules)
build_c \
    "$ROOT/niyah_hybrid" \
    "$ROOT/Core_CPP/niyah_core.c" \
    "$ROOT/Core_CPP/hybrid_reasoner.c" \
    "$ROOT/Core_CPP/constraint_solver.c" \
    "$ROOT/Core_CPP/rule_parser.c" \
    "$ROOT/Core_CPP/proof_generator.c" \
    "$ROOT/Core_CPP/khz_q_svd.c" \
    "$ROOT/Core_CPP/niyah_hybrid_main.c" \
    "$ROOT/tokenizer.c"

# Standalone trainer (C only)
build_c \
    "$ROOT/niyah_train" \
    "$ROOT/Core_CPP/niyah_core.c" \
    "$ROOT/Core_CPP/niyah_train.c" \
    "$ROOT/tokenizer.c"

build_cxx \
    "$ROOT/Core_CPP/trainer" \
    "$ROOT/Core_CPP/trainer.cpp"

# ── checksums + sizes ─────────────────────────────────────────────────────────
echo ""
echo "[build_gcc] Artifacts:"
for art in "$ROOT/Core_CPP/niyah" "$ROOT/Core_CPP/trainer"; do
    if [[ -f "$art" ]]; then
        if command -v sha256sum &>/dev/null; then
            hash=$(sha256sum "$art" | awk '{print $1}')
        elif command -v shasum &>/dev/null; then
            hash=$(shasum -a 256 "$art" | awk '{print $1}')
        else
            hash="(no sha256 tool)"
        fi
        sz=$(stat -c%s "$art" 2>/dev/null || stat -f%z "$art")
        printf "  SHA256 %s  %s  (%d KB)\n" "$hash" "$(basename "$art")" "$(( sz / 1024 ))"
    fi
done

echo ""
echo "[build_gcc] Build complete ($CONFIG / $ARCH)."

# ── optional cppcheck static analysis ────────────────────────────────────────
# Enable with: RUN_LINT=1 bash scripts/build_gcc.sh
# Requires: sudo apt-get install cppcheck  /  brew install cppcheck
if [[ "${RUN_LINT:-0}" == "1" ]]; then
    echo ""
    if command -v cppcheck &>/dev/null; then
        echo "[build_gcc] Running cppcheck..."
        cppcheck \
            --enable=all \
            --error-exitcode=1 \
            --suppress=missingIncludeSystem \
            --suppress=unusedFunction \
            --std=c99 \
            --language=c \
            "$ROOT/Core_CPP/niyah_core.c" \
            "$ROOT/Core_CPP/niyah_main.c" \
            2>&1
        echo "[build_gcc] cppcheck PASSED"
    else
        echo "[build_gcc] WARNING: cppcheck not found — skipping lint."
        echo "  Install: sudo apt-get install cppcheck  OR  brew install cppcheck"
    fi
fi

# ── optional smoke-test run ───────────────────────────────────────────────────
if [[ "${RUN_SMOKE:-0}" == "1" ]]; then
    echo ""
    echo "[build_gcc] Running neural smoke test..."
    cd "$ROOT/Core_CPP"
    ./niyah
    echo "[build_gcc] Neural smoke test PASSED"
    echo ""
    echo "[build_gcc] Running hybrid integration suite (96 tests)..."
    cd "$ROOT"
    ./niyah_hybrid --smoke
    echo "[build_gcc] Hybrid suite complete"
fi
