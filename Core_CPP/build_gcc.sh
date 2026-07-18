#!/usr/bin/env bash
# scripts/build_gcc.sh — NIYAH v3.0 build script
#
# Usage:
#   bash scripts/build_gcc.sh              # release (auto-detects arch+SIMD)
#   bash scripts/build_gcc.sh --debug      # debug + ASan + UBSan
#   RUN_LINT=1   bash scripts/build_gcc.sh # cppcheck gate before compile
#   RUN_SMOKE=1  bash scripts/build_gcc.sh # run smoke test after build
#   RUN_BENCH=1  bash scripts/build_gcc.sh # run bench after build
#
# Supports:
#   aarch64 / Snapdragon X Elite  → -march=armv8.2-a (NEON auto)
#   x86_64  with AVX2             → -mavx2 -mfma -march=native
#   x86_64  without AVX2          → -march=native (scalar fallback)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$ROOT/build"
CORE="$ROOT/Core_CPP"
INCLUDE="$ROOT/include"
BENCH_DIR="$ROOT/bench"

CC="${CC:-gcc}"
ARCH="$(uname -m)"
CONFIG="release"
ASAN=0
RUN_LINT="${RUN_LINT:-0}"
RUN_SMOKE="${RUN_SMOKE:-0}"
RUN_BENCH="${RUN_BENCH:-0}"

for arg in "$@"; do
    case "$arg" in
        --debug)   CONFIG=debug;   ASAN=1 ;;
        --release) CONFIG=release         ;;
        --lint)    RUN_LINT=1             ;;
        --smoke)   RUN_SMOKE=1            ;;
        --bench)   RUN_BENCH=1            ;;
    esac
done

echo "═══════════════════════════════════════════════"
echo "  NIYAH v3.0 Build"
echo "  CC:     $("$CC" --version | head -1)"
echo "  Arch:   $ARCH"
echo "  Config: $CONFIG"
echo "═══════════════════════════════════════════════"

# ── Detect SIMD flags ─────────────────────────────────────────────
case "$ARCH" in
    aarch64|arm64)
        SIMD_FLAGS="-march=armv8.2-a"
        SIMD_NAME="NEON (armv8.2-a)"
        ;;
    x86_64)
        if echo "" | "$CC" -x c -mavx2 -mfma -E - >/dev/null 2>&1; then
            SIMD_FLAGS="-mavx2 -mfma -march=native"
            SIMD_NAME="AVX2+FMA"
        else
            SIMD_FLAGS="-march=native"
            SIMD_NAME="Scalar (no AVX2)"
        fi
        ;;
    *)
        SIMD_FLAGS=""
        SIMD_NAME="Scalar (unknown arch)"
        ;;
esac
echo "  SIMD:   $SIMD_NAME"
echo ""

# ── Build flags ───────────────────────────────────────────────────
WARN="-Wall -Wextra -Werror -Wstrict-prototypes -Wmissing-prototypes \
      -Wcast-align -Wwrite-strings -Wshadow -pedantic"

if [[ "$CONFIG" == "release" ]]; then
    OPT="-O3 -DNDEBUG"
    LDFLAGS="-lm"
else
    OPT="-O1 -g -DDEBUG"
    LDFLAGS="-lm"
    if [[ "$ASAN" -eq 1 ]]; then
        OPT="$OPT -fsanitize=address,undefined -fno-omit-frame-pointer"
        LDFLAGS="$LDFLAGS -fsanitize=address,undefined"
    fi
fi

CFLAGS="-std=c11 $WARN $SIMD_FLAGS $OPT -I$INCLUDE"

mkdir -p "$BUILD_DIR"

# ── §1  cppcheck static analysis gate ────────────────────────────
if [[ "$RUN_LINT" -eq 1 ]]; then
    echo "── cppcheck ───────────────────────────────────"
    if ! command -v cppcheck &>/dev/null; then
        echo "  [ERROR] cppcheck not found — install: apt install cppcheck"
        exit 1
    fi
    cppcheck \
        --error-exitcode=1 \
        --enable=warning,style,performance,portability \
        --suppress=missingIncludeSystem \
        --suppress=unusedFunction \
        --std=c11 \
        -I "$INCLUDE" \
        "$CORE/niyah_core.c" \
        "$CORE/niyah_main.c"
    echo "  [PASS] cppcheck: 0 issues"
    echo ""
fi

# ── §2  Build main binary ─────────────────────────────────────────
NIYAH_BIN="$BUILD_DIR/niyah"
echo "── Build: niyah ───────────────────────────────"
echo "  cmd> $CC $CFLAGS Core_CPP/niyah_core.c Core_CPP/niyah_main.c -o build/niyah $LDFLAGS"
# shellcheck disable=SC2086
"$CC" $CFLAGS \
    "$CORE/niyah_core.c" \
    "$CORE/niyah_main.c" \
    -o "$NIYAH_BIN" $LDFLAGS
echo "  [OK] $NIYAH_BIN"

# ── §3  Build bench binary (if bench dir exists) ──────────────────
BENCH_BIN="$BUILD_DIR/bench_niyah"
if [[ -f "$BENCH_DIR/bench_niyah.c" ]]; then
    echo ""
    echo "── Build: bench_niyah ─────────────────────────"
    # shellcheck disable=SC2086
    "$CC" $CFLAGS \
        "$BENCH_DIR/bench_niyah.c" \
        "$CORE/niyah_core.c" \
        -o "$BENCH_BIN" $LDFLAGS
    echo "  [OK] $BENCH_BIN"
fi

# ── §4  Smoke test ────────────────────────────────────────────────
if [[ "$RUN_SMOKE" -eq 1 ]]; then
    echo ""
    echo "── Smoke test ─────────────────────────────────"
    "$NIYAH_BIN"
fi

# ── §5  Bench ─────────────────────────────────────────────────────
if [[ "$RUN_BENCH" -eq 1 && -f "$BENCH_BIN" ]]; then
    echo ""
    echo "── Benchmark ──────────────────────────────────"
    "$BENCH_BIN"
fi

# ── §6  Artifact manifest ─────────────────────────────────────────
echo ""
echo "═══════════════════ Artifacts ═════════════════"
for f in "$NIYAH_BIN" "$BENCH_BIN"; do
    [[ -f "$f" ]] || continue
    SIZE=$(stat -c%s "$f" 2>/dev/null || stat -f%z "$f")
    HASH=$(sha256sum "$f" 2>/dev/null | awk '{print $1}' || \
           shasum -a 256 "$f"  | awk '{print $1}')
    printf "  %-20s  %7d bytes\n  SHA256: %s\n\n" \
           "$(basename "$f")" "$SIZE" "$HASH"
done
echo "  Config=$CONFIG  SIMD=$SIMD_NAME"
