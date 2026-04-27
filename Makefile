# =============================================================================
# Casper Engine — NIYAH Hybrid Neuro-Symbolic Inference
# =============================================================================
# Quick reference:
#   make            # Build everything (release)
#   make hybrid     # Build hybrid binary only
#   make train      # Build trainer only
#   make test       # Build + run all 96 smoke tests
#   make debug      # Debug build with sanitizers
#   make clean      # Remove all artifacts
#   make install    # Install to /usr/local/bin
#   make help       # Show this list
# =============================================================================

CC      ?= gcc
CXX     ?= g++
STD      = -std=c11
CXXSTD   = -std=c++17

# ── Architecture detection ───────────────────────────────────────────────────
UNAME_M := $(shell uname -m)
ifeq ($(UNAME_M),x86_64)
    ARCH_FLAGS = -mavx2 -mfma -march=native
    SIMD_NAME  = AVX2+FMA
else ifneq (,$(filter $(UNAME_M),aarch64 arm64))
    ARCH_FLAGS = -march=armv8.2-a
    SIMD_NAME  = NEON
else
    ARCH_FLAGS =
    SIMD_NAME  = Scalar
endif

# ── Warning flags ────────────────────────────────────────────────────────────
WARN_C   = -Wall -Wextra -Werror -Wstrict-prototypes -Wmissing-prototypes \
           -Wcast-align -Wwrite-strings -Wshadow -pedantic
WARN_CXX = -Wall -Wextra -Werror -Wcast-align -Wshadow

# ── Build profile ────────────────────────────────────────────────────────────
ifeq ($(DEBUG),1)
    OPT      = -O0 -g3 -DDEBUG -fsanitize=address,undefined
    LDFLAGS  = -fsanitize=address,undefined
    PROFILE  = debug
else
    OPT      = -O3 -DNDEBUG
    LDFLAGS  = -flto
    PROFILE  = release
endif

CFLAGS   = $(STD)    $(OPT) $(ARCH_FLAGS) $(WARN_C)
CXXFLAGS = $(CXXSTD) $(OPT) $(ARCH_FLAGS) $(WARN_CXX)

# ── Sources ──────────────────────────────────────────────────────────────────
HYBRID_SRCS = \
    Core_CPP/niyah_core.c \
    Core_CPP/hybrid_reasoner.c \
    Core_CPP/constraint_solver.c \
    Core_CPP/rule_parser.c \
    Core_CPP/proof_generator.c \
    Core_CPP/khz_q_svd.c \
    Core_CPP/niyah_hybrid_main.c \
    tokenizer.c

TRAIN_SRCS = \
    Core_CPP/niyah_core.c \
    Core_CPP/niyah_train.c \
    tokenizer.c

NEURAL_SRCS = \
    Core_CPP/niyah_core.c \
    Core_CPP/niyah_main.c

# ── Targets ──────────────────────────────────────────────────────────────────
.PHONY: all hybrid train neural trainer_cxx test test-hybrid test-reasoner \
        test-csp test-rules test-proof clean install help info debug

all: info hybrid niyah_train Core_CPP/niyah Core_CPP/trainer
	@echo ""
	@echo "[make] Build complete ($(PROFILE) / $(SIMD_NAME))."
	@echo "[make] Binaries:"
	@ls -la niyah_hybrid niyah_train Core_CPP/niyah Core_CPP/trainer 2>/dev/null | awk '{printf "  %s\n", $$0}'

info:
	@echo "[make] CC      = $(CC)"
	@echo "[make] Profile = $(PROFILE)"
	@echo "[make] Arch    = $(UNAME_M) ($(SIMD_NAME))"
	@echo ""

hybrid: niyah_hybrid

niyah_hybrid: $(HYBRID_SRCS)
	@echo "[make] Building niyah_hybrid (full hybrid engine)..."
	$(CC) $(CFLAGS) $(HYBRID_SRCS) -o niyah_hybrid -lm $(LDFLAGS)
	@printf "[make]   OK  niyah_hybrid  (%d KB)\n" $$(($$(stat -c%s niyah_hybrid 2>/dev/null || stat -f%z niyah_hybrid) / 1024))

train: niyah_train

niyah_train: $(TRAIN_SRCS)
	@echo "[make] Building niyah_train (standalone trainer)..."
	$(CC) $(CFLAGS) $(TRAIN_SRCS) -o niyah_train -lm $(LDFLAGS)
	@printf "[make]   OK  niyah_train   (%d KB)\n" $$(($$(stat -c%s niyah_train 2>/dev/null || stat -f%z niyah_train) / 1024))

neural: Core_CPP/niyah

Core_CPP/niyah: $(NEURAL_SRCS)
	@echo "[make] Building Core_CPP/niyah (neural smoke binary)..."
	$(CC) $(CFLAGS) $(NEURAL_SRCS) -o Core_CPP/niyah -lm $(LDFLAGS)

trainer_cxx: Core_CPP/trainer

Core_CPP/trainer: Core_CPP/trainer.cpp
	@echo "[make] Building Core_CPP/trainer (C++ trainer)..."
	$(CXX) $(CXXFLAGS) Core_CPP/trainer.cpp -o Core_CPP/trainer $(LDFLAGS)

# ── Test targets ─────────────────────────────────────────────────────────────
test: niyah_hybrid
	@echo ""
	@echo "[make] Running 96 hybrid smoke tests..."
	@./niyah_hybrid --smoke

test-hybrid: test

test-reasoner:
	@echo "[make] Symbolic reasoner standalone test (21 tests)..."
	@$(CC) $(CFLAGS) Core_CPP/hybrid_reasoner.c -DSYM_STANDALONE_TEST -o /tmp/test_reasoner -lm
	@/tmp/test_reasoner

test-csp:
	@echo "[make] Constraint solver standalone test (19 tests)..."
	@$(CC) $(CFLAGS) Core_CPP/constraint_solver.c -DCSP_STANDALONE_TEST -o /tmp/test_csp -lm
	@/tmp/test_csp

test-rules:
	@echo "[make] Rule parser standalone test (22 tests)..."
	@$(CC) $(CFLAGS) Core_CPP/rule_parser.c -DRULE_STANDALONE_TEST -o /tmp/test_rules -lm
	@/tmp/test_rules

test-proof:
	@echo "[make] Proof generator standalone test (11 tests + NIST vectors)..."
	@$(CC) $(CFLAGS) Core_CPP/proof_generator.c -DPROOF_STANDALONE_TEST -o /tmp/test_proof -lm
	@/tmp/test_proof

test-all: test-reasoner test-csp test-rules test-proof test-hybrid
	@echo ""
	@echo "[make] All test suites complete."

# ── Debug build ──────────────────────────────────────────────────────────────
debug:
	@$(MAKE) DEBUG=1 all

# ── Maintenance ──────────────────────────────────────────────────────────────
clean:
	@echo "[make] Cleaning artifacts..."
	@rm -f niyah_hybrid niyah_train Core_CPP/niyah Core_CPP/trainer
	@rm -f *.proof *.bin
	@rm -f /tmp/test_reasoner /tmp/test_csp /tmp/test_rules /tmp/test_proof
	@echo "[make] Done."

PREFIX ?= /usr/local
install: niyah_hybrid niyah_train
	@echo "[make] Installing to $(PREFIX)/bin (sudo required)..."
	@install -d $(PREFIX)/bin
	@install -m 755 niyah_hybrid $(PREFIX)/bin/casper-niyah
	@install -m 755 niyah_train  $(PREFIX)/bin/casper-train
	@echo "[make] Installed:"
	@echo "  $(PREFIX)/bin/casper-niyah"
	@echo "  $(PREFIX)/bin/casper-train"

uninstall:
	@rm -f $(PREFIX)/bin/casper-niyah $(PREFIX)/bin/casper-train
	@echo "[make] Uninstalled from $(PREFIX)/bin"

help:
	@echo "Casper Engine — Build Targets"
	@echo "============================="
	@echo "  make              Build all (release)"
	@echo "  make hybrid       Build niyah_hybrid only"
	@echo "  make train        Build niyah_train only"
	@echo "  make neural       Build Core_CPP/niyah (smoke binary)"
	@echo "  make trainer_cxx  Build Core_CPP/trainer (C++ trainer)"
	@echo ""
	@echo "  make test         Build + run 96 hybrid tests"
	@echo "  make test-all     Run every standalone subsystem test"
	@echo "  make test-reasoner / test-csp / test-rules / test-proof"
	@echo ""
	@echo "  make debug        Debug build with ASan + UBSan"
	@echo "  make clean        Remove all built artifacts"
	@echo "  make install      Install to \$$PREFIX/bin (default /usr/local/bin)"
	@echo "  make uninstall    Remove installed binaries"
	@echo ""
	@echo "Variables:"
	@echo "  CC=clang DEBUG=1 PREFIX=~/.local"
