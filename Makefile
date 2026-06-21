# =============================================================================
# perf-profiler — Makefile
#
# Targets:
#   make              Build profiler (Linux only) and test binaries
#   make check        Build & run unit tests  ← works on macOS too
#   make integration  Full integration tests  (Linux + ptrace permission)
#   make clean        Remove build artefacts
# =============================================================================

CXX      ?= g++
CXXFLAGS  = -std=c++17 -Wall -Wextra -Wpedantic -I./include
LDFLAGS   =

ifeq ($(DEBUG),1)
  CXXFLAGS += -O0 -g3 -fsanitize=address,undefined
  LDFLAGS  += -fsanitize=address,undefined
else
  CXXFLAGS += -O2 -g
endif

CXXFLAGS += -fno-omit-frame-pointer

# Detect OS
UNAME := $(shell uname)

TESTS_BIN       = tests/run_tests
DEBUG_BIN       = tests/debug_runner
TEST_TARGET_BIN = tests/test_target
PROFILER_BIN    = profiler

.PHONY: all check integration clean help

# ── Default target ────────────────────────────────────────────────────────────
ifeq ($(UNAME), Linux)
all: $(PROFILER_BIN) $(TEST_TARGET_BIN) $(TESTS_BIN) $(DEBUG_BIN)
	@echo ""
	@echo "  Build complete."
	@echo "  make check    — unit tests"
	@echo "  make debug    — interactive terminal walkthrough"
	@echo "  make integration — full end-to-end (needs ptrace permission)"
	@echo ""
else
all: $(TESTS_BIN) $(DEBUG_BIN)
	@echo ""
	@echo "  macOS detected — profiler engine is Linux-only."
	@echo "  Built: unit tests + interactive debug runner"
	@echo "  make check    — run unit tests"
	@echo "  make debug    — interactive terminal walkthrough  ← try this"
	@echo ""
endif

# ── profiler (Linux only) ─────────────────────────────────────────────────────
$(PROFILER_BIN): src/main.cpp include/*.hpp
	$(CXX) $(CXXFLAGS) -o $@ src/main.cpp $(LDFLAGS)
	@echo "  Built: $@"

# ── test_target (Linux only — uses fork/exec patterns for profiling) ──────────
$(TEST_TARGET_BIN): tests/test_target.cpp
	$(CXX) -std=c++17 -O1 -g -fno-omit-frame-pointer -o $@ $<
	@echo "  Built: $@"

# ── Unit + integration test runner (builds everywhere) ───────────────────────
$(TESTS_BIN): tests/run_tests.cpp include/*.hpp
	$(CXX) $(CXXFLAGS) -o $@ tests/run_tests.cpp $(LDFLAGS)
	@echo "  Built: $@"

# ── Interactive debug runner (builds everywhere) ──────────────────────────────
$(DEBUG_BIN): tests/debug_runner.cpp include/*.hpp
	$(CXX) $(CXXFLAGS) -o $@ tests/debug_runner.cpp $(LDFLAGS)
	@echo "  Built: $@"

# ── Launch the debug runner interactively ────────────────────────────────────
debug: $(DEBUG_BIN)
	@echo ""
	./$(DEBUG_BIN)

# ── Run unit tests ────────────────────────────────────────────────────────────
check: $(TESTS_BIN)
	@echo ""
	./$(TESTS_BIN)

# ── Run integration tests (Linux + root/CAP_SYS_PTRACE) ──────────────────────
integration: $(PROFILER_BIN) $(TEST_TARGET_BIN) $(TESTS_BIN)
	@echo ""
	PROFILER=./$(PROFILER_BIN) TEST_TARGET=./$(TEST_TARGET_BIN) \
	    ./$(TESTS_BIN) integration

clean:
	rm -f $(PROFILER_BIN) $(TEST_TARGET_BIN) $(TESTS_BIN)
	rm -f /tmp/pp_*.html /tmp/pp_*.folded /tmp/pp_*.txt

help:
	@echo ""
	@echo "  make              Build everything (profiler on Linux, tests everywhere)"
	@echo "  make check        Run unit tests — works on macOS and Linux"
	@echo "  make integration  Full end-to-end tests (Linux + ptrace permission)"
	@echo "  make DEBUG=1 all  Build with AddressSanitizer + UBSan"
	@echo "  make clean        Remove binaries"
	@echo ""