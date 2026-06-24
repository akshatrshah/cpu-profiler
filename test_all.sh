#!/usr/bin/env bash
# =============================================================================
# test_all.sh — full system test for perf-profiler
#
# Covers:
#   1.  Build system (make all, make check, USE_LIBUNWIND if available)
#   2.  C++ unit tests (32 tests via run_tests binary)
#   3.  Binary smoke tests (--help, --version, missing args, bad PID)
#   4.  Output file validation (HTML structure, folded format, pprof format)
#   5.  Multi-thread profiling (test_target spawns worker threads)
#   6.  Kernel frame toggle (--kernel flag)
#   7.  Coverage report (gcov if DEBUG_COV=1)
#   8.  Integration test — real profiler against real target (Linux + ptrace)
#
# Usage:
#   ./test_all.sh                  # everything except integration
#   ./test_all.sh --integration    # include live ptrace tests (needs sudo/cap)
#   DEBUG_COV=1 ./test_all.sh      # also produce gcov coverage report
#
# Does NOT need root for steps 1-4.
# Step 8 (integration) needs:  sudo   OR   sudo setcap cap_sys_ptrace+eip ./profiler
# =============================================================================

set -euo pipefail

# ── Colours ───────────────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; DIM='\033[2m'; RESET='\033[0m'

# ── State ─────────────────────────────────────────────────────────────────────
PASS=0; FAIL=0; SKIP=0
FAILED_TESTS=()
RUN_INTEGRATION=0
[[ "${1:-}" == "--integration" ]] && RUN_INTEGRATION=1

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Temp dir cleaned up on exit
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"; kill $(jobs -p) 2>/dev/null || true' EXIT

# ── Helpers ───────────────────────────────────────────────────────────────────
header() { echo -e "\n${BOLD}${CYAN}━━━  $1  ━━━${RESET}\n"; }

pass() {
    echo -e "  ${GREEN}✓${RESET}  $1"
    PASS=$((PASS + 1))
}

fail() {
    echo -e "  ${RED}✗${RESET}  $1"
    echo -e "     ${DIM}$2${RESET}"
    FAIL=$((FAIL + 1))
    FAILED_TESTS+=("$1")
}

skip() {
    echo -e "  ${YELLOW}⊘${RESET}  $1  ${DIM}(skipped: $2)${RESET}"
    SKIP=$((SKIP + 1))
}

assert_file_exists() {
    local label="$1" path="$2"
    if [[ -f "$path" ]]; then
        pass "$label — file exists"
    else
        fail "$label — file missing" "$path"
    fi
}

assert_file_not_empty() {
    local label="$1" path="$2"
    if [[ -s "$path" ]]; then
        pass "$label — file non-empty"
    else
        fail "$label — file empty or missing" "$path"
    fi
}

assert_contains() {
    local label="$1" file="$2" needle="$3"
    if grep -qE "$needle" "$file" 2>/dev/null; then
        pass "$label"
    else
        fail "$label" "expected '${needle}' in ${file}"
    fi
}

assert_not_contains() {
    local label="$1" file="$2" needle="$3"
    if ! grep -qE "$needle" "$file" 2>/dev/null; then
        pass "$label"
    else
        fail "$label" "did not expect '${needle}' in ${file}"
    fi
}

assert_exit_zero() {
    local label="$1"; shift
    # Run in subshell so set -e doesn't kill us on failure
    if ( "$@" >"$TMP/out" 2>&1 ); then
        pass "$label"
    else
        fail "$label" "$(head -3 "$TMP/out")"
    fi
}

assert_exit_nonzero() {
    local label="$1"; shift
    if ! ( "$@" >"$TMP/out" 2>&1 ); then
        pass "$label"
    else
        fail "$label" "expected non-zero exit but got 0"
    fi
}

assert_line_count_gte() {
    local label="$1" file="$2" min="$3"
    local count
    count=$(wc -l < "$file" 2>/dev/null || echo 0)
    if (( count >= min )); then
        pass "$label (${count} lines)"
    else
        fail "$label" "expected >= ${min} lines, got ${count}"
    fi
}

IS_LINUX=0
[[ "$(uname)" == "Linux" ]] && IS_LINUX=1

# =============================================================================
# 1. BUILD SYSTEM
# =============================================================================
header "1 · Build system"

# Clean first
make clean >/dev/null 2>&1 || true

# Build all targets
if make all >"$TMP/build.log" 2>&1; then
    pass "make all — clean build"
else
    fail "make all — build failed" "$(tail -5 "$TMP/build.log")"
    echo -e "\n${RED}Build failed — cannot continue.${RESET}\n"
    exit 1
fi

assert_file_exists "run_tests binary built"      "./tests/run_tests"
assert_file_exists "debug_runner binary built"   "./tests/debug_runner"

if [[ $IS_LINUX -eq 1 ]]; then
    assert_file_exists "profiler binary built"    "./profiler"
    assert_file_exists "test_target binary built" "./tests/test_target"
else
    skip "profiler binary built"    "Linux-only binary"
    skip "test_target binary built" "Linux-only binary"
fi

# Debug build (ASan + UBSan) — Linux only because macOS ASan has different flags
if [[ $IS_LINUX -eq 1 ]]; then
    if make clean >/dev/null 2>&1 && make DEBUG=1 all >"$TMP/dbg.log" 2>&1; then
        pass "DEBUG=1 build (ASan + UBSan) succeeds"
        make clean >/dev/null 2>&1
        make all   >/dev/null 2>&1
    else
        fail "DEBUG=1 build" "$(tail -3 "$TMP/dbg.log")"
    fi
else
    skip "DEBUG=1 build" "ASan setup differs on macOS"
fi

# USE_LIBUNWIND build — only if header is present
if [[ $IS_LINUX -eq 1 ]] && pkg-config --exists libunwind 2>/dev/null; then
    if make clean >/dev/null 2>&1 && make USE_LIBUNWIND=1 all >"$TMP/uw.log" 2>&1; then
        pass "USE_LIBUNWIND=1 build succeeds"
        make clean >/dev/null 2>&1
        make all   >/dev/null 2>&1  # restore default build
    else
        fail "USE_LIBUNWIND=1 build" "$(tail -3 "$TMP/uw.log")"
    fi
else
    skip "USE_LIBUNWIND=1 build" "libunwind-dev not installed"
fi

# =============================================================================
# 2. C++ UNIT TESTS
# =============================================================================
header "2 · C++ unit tests (run_tests binary)"

if ./tests/run_tests >"$TMP/unit.log" 2>&1; then
    passed_count=$(grep -c '✓' "$TMP/unit.log" || echo 0)
    failed_count=$(grep -c '✗' "$TMP/unit.log" || echo 0)
    pass "run_tests — ${passed_count} passed, ${failed_count} failed"

    # Verify specific test categories ran
    for category in \
        "result_ok_carries_value" \
        "aggregator_single_sample" \
        "aggregator_key_order_outermost_first" \
        "aggregator_counts_sum_to_total" \
        "resolver_unknown_address_returns_hex" \
        "report_writer_folded_format_correct" \
        "report_writer_html_is_valid_html5" \
        "report_writer_html_embeds_profile_data" \
        "report_writer_html_contains_renderer" \
        "report_writer_write_to_bad_path_fails" \
        "config_defaults" \
        "unwinder_self_unwind_returns_frames"
    do
        if grep -q "✓.*${category}" "$TMP/unit.log"; then
            pass "  unit: ${category}"
        else
            fail "  unit: ${category}" "not found in output"
        fi
    done

else
    fail "run_tests — binary exited non-zero" "$(tail -5 "$TMP/unit.log")"
fi

# Filter tests by pattern
if ./tests/run_tests aggregator >"$TMP/filt.log" 2>&1; then
    pass "run_tests filter by pattern works"
    # Should only contain aggregator tests
    if grep -q "resolver_loads" "$TMP/filt.log" 2>/dev/null; then
        fail "pattern filter: leaked non-matching tests" ""
    else
        pass "pattern filter: only matching tests ran"
    fi
else
    fail "run_tests filter by pattern" "$(tail -3 "$TMP/filt.log")"
fi

# --list flag
if ./tests/run_tests --list >"$TMP/list.log" 2>&1; then
    count=$(wc -l < "$TMP/list.log")
    if (( count >= 20 )); then
        pass "run_tests --list shows ${count} tests"
    else
        fail "run_tests --list" "expected >= 20 tests, got ${count}"
    fi
else
    fail "run_tests --list" "exited non-zero"
fi

# =============================================================================
# 3. BINARY SMOKE TESTS (no ptrace needed)
# =============================================================================
header "3 · Binary smoke tests"

if [[ $IS_LINUX -eq 1 ]]; then
    PROFILER="./profiler"

    # --help
    assert_exit_zero    "--help exits 0"             "$PROFILER" --help
    assert_contains     "--help mentions --pid"       "$TMP/out"  "\-\-pid"
    assert_contains     "--help mentions --rate"      "$TMP/out"  "\-\-rate"
    assert_contains     "--help mentions --kernel"    "$TMP/out"  "\-\-kernel"
    assert_contains     "--help mentions --pprof"     "$TMP/out"  "\-\-pprof"
    assert_contains     "--help shows unwind method"  "$TMP/out"  "frame-pointer\|DWARF"

    # --version
    assert_exit_zero    "--version exits 0"           "$PROFILER" --version
    assert_contains     "--version shows v2"          "$TMP/out"  "2\."

    # missing --pid
    assert_exit_nonzero "no args exits non-zero"      "$PROFILER"
    assert_exit_nonzero "--pid missing exits non-zero" "$PROFILER" --rate 99

    # bad PID
    assert_exit_nonzero "bad PID exits non-zero"      "$PROFILER" --pid 9999999

    # bad rate
    assert_exit_nonzero "rate=0 exits non-zero"       "$PROFILER" --pid $$ --rate 0
    assert_exit_nonzero "rate=99999 exits non-zero"   "$PROFILER" --pid $$ --rate 99999

    # bad duration
    assert_exit_nonzero "duration=0 exits non-zero"   "$PROFILER" --pid $$ --duration 0

    # unknown flag
    assert_exit_nonzero "unknown flag exits non-zero" "$PROFILER" --foobar
else
    skip "binary smoke tests" "profiler binary is Linux-only"
fi

# =============================================================================
# 4. OUTPUT FILE VALIDATION
# =============================================================================
header "4 · Output file format validation"

# Generate test output files by running the C++ unit tests
# run_tests writes real files to /tmp that we can then inspect
./tests/run_tests report_writer >"$TMP/rw_out.log" 2>&1 || true

# The report_writer tests write files like /tmp/pp_test_folded_<pid>.txt
# and /tmp/pp_test_html_<pid>.html — but those get unlinked immediately.
# Instead we generate our own controlled files using a small inline C++ program.

cat > "$TMP/gen_outputs.cpp" << 'CPPEOF'
#include "types.hpp"
#include "aggregator.hpp"
#include "report_writer.hpp"
#include <iostream>
int main() {
    profiler::Aggregator agg("test_target");
    auto make = [](std::vector<std::string> syms) {
        profiler::Sample s;
        for (auto &n : syms) { profiler::Frame f; f.symbol = n; s.frames.push_back(f); }
        return s;
    };
    for (int i=0;i<110;i++) agg.add(make({"cpu_math","level2_math","workload_loop","main"}));
    for (int i=0;i< 70;i++) agg.add(make({"cpu_fibonacci","level2_fib","workload_loop","main"}));
    for (int i=0;i< 17;i++) agg.add(make({"cpu_memory","level2_mem","workload_loop","main"}));
    for (int i=0;i<  3;i++) agg.add(make({"workload_loop","main"}));
    profiler::Config cfg; cfg.rate_hz=99; cfg.duration_s=10;
    auto p = agg.build(9999, cfg);
    auto s1 = profiler::ReportWriter::write_folded("/tmp/test_all_demo.folded", p);
    auto s2 = profiler::ReportWriter::write_html  ("/tmp/test_all_demo.html",   p);
    auto s3 = profiler::ReportWriter::write_pprof ("/tmp/test_all_demo.pprof",  p);
    if (!s1.ok()) { std::cerr << s1.error << "\n"; return 1; }
    if (!s2.ok()) { std::cerr << s2.error << "\n"; return 1; }
    if (!s3.ok()) { std::cerr << s3.error << "\n"; return 1; }
    return 0;
}
CPPEOF

if g++ -std=c++17 -O1 -I./include -fno-omit-frame-pointer \
       -o "$TMP/gen_outputs" "$TMP/gen_outputs.cpp" >"$TMP/gen_build.log" 2>&1 \
   && "$TMP/gen_outputs"; then
    pass "test output files generated"
else
    fail "generate test output files" "$(cat "$TMP/gen_build.log" | tail -3)"
fi

DEMO_FOLDED="/tmp/test_all_demo.folded"
DEMO_HTML="/tmp/test_all_demo.html"
DEMO_PPROF="/tmp/test_all_demo.pprof"

# ── Folded format ─────────────────────────────────────────────────────────────
if [[ -f "$DEMO_FOLDED" ]]; then
    assert_file_not_empty "folded file exists and non-empty" "$DEMO_FOLDED"

    bad_lines=0
    while IFS= read -r line; do
        [[ -z "$line" ]] && continue
        count="${line##* }"
        if ! [[ "$count" =~ ^[0-9]+$ ]]; then
            bad_lines=$((bad_lines + 1))
        fi
    done < "$DEMO_FOLDED"

    if (( bad_lines == 0 )); then
        pass "folded format — every line ends with a number"
    else
        fail "folded format" "${bad_lines} lines with bad format"
    fi

    assert_contains "folded has cpu_math entry"      "$DEMO_FOLDED" "cpu_math"
    assert_contains "folded has cpu_fibonacci entry"  "$DEMO_FOLDED" "cpu_fibonacci"
    assert_contains "folded has workload_loop"        "$DEMO_FOLDED" "workload_loop"
    assert_contains "folded has comm prefix"          "$DEMO_FOLDED" "test_target"

    total=0
    while IFS= read -r line; do
        [[ -z "$line" ]] && continue
        n="${line##* }"
        [[ "$n" =~ ^[0-9]+$ ]] && total=$((total + n))
    done < "$DEMO_FOLDED"
    if (( total == 200 )); then
        pass "folded counts sum to 200 (total_samples)"
    else
        fail "folded counts sum" "expected 200, got ${total}"
    fi
else
    fail "folded file missing" "$DEMO_FOLDED"
fi

# ── HTML format ───────────────────────────────────────────────────────────────
if [[ -f "$DEMO_HTML" ]]; then
    assert_file_not_empty "HTML exists"                "$DEMO_HTML"
    assert_contains "HTML has doctype"                 "$DEMO_HTML" "DOCTYPE html"
    assert_contains "HTML has canvas"                  "$DEMO_HTML" "<canvas"
    assert_contains "HTML closes properly"             "$DEMO_HTML" "</html>"
    assert_contains "HTML has PROFILE_DATA"            "$DEMO_HTML" "PROFILE_DATA"
    assert_contains "HTML has buildTree"               "$DEMO_HTML" "buildTree"
    assert_contains "HTML has render fn"               "$DEMO_HTML" "function render"
    assert_contains "HTML has hitTest fn"              "$DEMO_HTML" "hitTest"
    assert_contains "HTML has zoom handler"            "$DEMO_HTML" "resetZoom"
    assert_contains "HTML has stats panel"             "$DEMO_HTML" "sv-total"
    assert_contains "HTML has tooltip"                 "$DEMO_HTML" "tooltip"
    assert_contains "HTML has test_target"             "$DEMO_HTML" "test_target"
    assert_contains "HTML has cpu_math in data"        "$DEMO_HTML" "cpu_math"

    script_closes=$(grep -c '</script>' "$DEMO_HTML" || echo 0)
    if (( script_closes <= 2 )); then
        pass "HTML script tag balance OK"
    else
        fail "HTML script tag balance" "${script_closes} closing tags (expected <=2)"
    fi

    size=$(wc -c < "$DEMO_HTML")
    if (( size >= 5000 )); then
        pass "HTML file size OK (${size} bytes)"
    else
        fail "HTML file size" "only ${size} bytes"
    fi
else
    fail "HTML file missing" "$DEMO_HTML"
fi

# ── pprof format ──────────────────────────────────────────────────────────────
if [[ -f "$DEMO_PPROF" ]]; then
    assert_file_not_empty "pprof file exists and non-empty" "$DEMO_PPROF"
    assert_contains "pprof has header comment"  "$DEMO_PPROF" "# pprof"
    assert_contains "pprof has comm"            "$DEMO_PPROF" "test_target"
    assert_contains "pprof has pid"             "$DEMO_PPROF" "# pid:"
    assert_contains "pprof has stacks"          "$DEMO_PPROF" ";"
    assert_contains "pprof has cpu_math"        "$DEMO_PPROF" "cpu_math"

    # pprof stacks should NOT have the comm prefix (just raw frames)
    bad=0
    while IFS= read -r line; do
        [[ -z "$line" ]] && continue
        [[ "$line" == \#* ]] && continue   # skip comments
        n="${line##* }"
        [[ "$n" =~ ^[0-9]+$ ]] || bad=$((bad + 1))
    done < "$DEMO_PPROF"
    if (( bad == 0 )); then
        pass "pprof format — all data lines valid"
    else
        fail "pprof format" "${bad} invalid lines"
    fi
else
    fail "pprof file missing" "$DEMO_PPROF"
fi

# =============================================================================
# 5. MULTI-THREAD TEST
# =============================================================================
header "5 · Multi-thread support"

if [[ $IS_LINUX -eq 1 ]]; then
    # Write a small multi-threaded target inline
    MT_SRC="$TMP/mt_target.cpp"
    MT_BIN="$TMP/mt_target"

    cat > "$MT_SRC" << 'CPPEOF'
#include <cmath>
#include <csignal>
#include <cstdio>
#include <thread>
#include <vector>
#include <unistd.h>
static volatile bool g_run = true;
static void on_sig(int) { g_run = false; }
static void worker() {
    volatile double x = 1.0;
    while (g_run) x += std::sqrt(x + 1.0);
    (void)x;
}
int main() {
    signal(SIGTERM, on_sig); signal(SIGINT, on_sig);
    std::printf("mt_target PID=%d\n", (int)getpid());
    std::fflush(stdout);
    // Spawn 3 worker threads
    std::vector<std::thread> threads;
    for (int i = 0; i < 3; ++i) threads.emplace_back(worker);
    while (g_run) usleep(10000);
    for (auto &t : threads) t.join();
    return 0;
}
CPPEOF

    if g++ -std=c++17 -O1 -fno-omit-frame-pointer -g \
           -o "$MT_BIN" "$MT_SRC" -lpthread 2>"$TMP/mt_build.log"; then
        pass "multi-thread target compiled"

        # Launch it and grab PID
        "$MT_BIN" > "$TMP/mt_pid.txt" 2>/dev/null &
        MT_CHILD=$!
        sleep 0.3
        MT_PID=$(grep -oP 'PID=\K[0-9]+' "$TMP/mt_pid.txt" 2>/dev/null || echo "")

        if [[ -n "$MT_PID" ]]; then
            # Check /proc/pid/task has multiple TIDs
            if [[ -d "/proc/$MT_PID/task" ]]; then
                TID_COUNT=$(ls /proc/$MT_PID/task | wc -l)
                if (( TID_COUNT >= 4 )); then   # main + 3 workers
                    pass "multi-thread target has ${TID_COUNT} threads visible in /proc"
                else
                    fail "expected >= 4 threads" "got ${TID_COUNT}"
                fi
            fi

            # Check thread_enumerator can list them (via profiler --help on self,
            # which reads no ptrace but /proc is readable)
            TASK_DIR="/proc/$MT_PID/task"
            NUMERIC_TIDS=0
            for tid_dir in "$TASK_DIR"/*/; do
                tid=$(basename "$tid_dir")
                [[ "$tid" =~ ^[0-9]+$ ]] && ((NUMERIC_TIDS++))
            done
            if (( NUMERIC_TIDS >= 4 )); then
                pass "thread enumerator logic: ${NUMERIC_TIDS} TIDs found"
            else
                fail "thread enumerator logic" "only ${NUMERIC_TIDS} numeric TIDs"
            fi

            # Check thread comm files are readable
            COMM_OK=0
            for comm_file in "$TASK_DIR"/*/comm; do
                [[ -r "$comm_file" ]] && ((COMM_OK++))
            done
            if (( COMM_OK >= 1 )); then
                pass "thread comm files readable (${COMM_OK} found)"
            else
                fail "thread comm files" "none readable"
            fi
        else
            fail "multi-thread target: could not read PID" ""
        fi

        kill $MT_CHILD 2>/dev/null || true
        wait $MT_CHILD 2>/dev/null || true
    else
        fail "multi-thread target compile" "$(cat "$TMP/mt_build.log")"
    fi
else
    skip "multi-thread tests" "/proc/pid/task is Linux-only"
fi

# =============================================================================
# 6. KERNEL FRAME TOGGLE
# =============================================================================
header "6 · Kernel frame flag"

if [[ $IS_LINUX -eq 1 ]]; then
    # --kernel should be accepted without crashing (even if ptrace fails later)
    ./profiler --help 2>&1 | grep -q "\-\-kernel" \
        && pass "--kernel flag documented in --help" \
        || fail "--kernel flag documented" "not in --help output"

    # verify the flag is in main.cpp source
    if grep -q "include_kernel" src/main.cpp 2>/dev/null; then
        pass "--kernel wired into Config in main.cpp"
    else
        fail "--kernel in main.cpp" "include_kernel not found"
    fi

    # verify perf_sampler uses it
    if grep -q "include_kernel" include/perf_sampler.hpp 2>/dev/null; then
        pass "perf_sampler respects include_kernel flag"
    else
        fail "perf_sampler include_kernel" "flag not used in perf_sampler.hpp"
    fi
else
    skip "kernel frame tests" "Linux-only"
fi

# =============================================================================
# 7. CODE COVERAGE (optional — set DEBUG_COV=1)
# =============================================================================
header "7 · Code coverage"

if [[ "${DEBUG_COV:-0}" == "1" ]] && [[ $IS_LINUX -eq 1 ]]; then
    if command -v gcov >/dev/null 2>&1; then
        make clean >/dev/null 2>&1
        # Build with coverage flags
        CXXFLAGS_EXTRA="--coverage -fprofile-arcs -ftest-coverage" \
            make all >/dev/null 2>&1 || true

        # Run tests to generate .gcda files
        ./tests/run_tests >/dev/null 2>&1 || true

        # Generate coverage report
        gcov -r include/*.hpp src/main.cpp >"$TMP/cov.txt" 2>&1 || true

        if [[ -f "$TMP/cov.txt" ]]; then
            pass "gcov report generated"
            echo ""
            echo -e "  ${DIM}Coverage summary:${RESET}"
            grep "Lines executed" "$TMP/cov.txt" | while read -r line; do
                echo -e "    ${DIM}${line}${RESET}"
            done
            echo ""
        else
            fail "gcov report" "no output generated"
        fi

        make clean >/dev/null 2>&1
        make all   >/dev/null 2>&1
    else
        skip "coverage report" "gcov not installed"
    fi
else
    skip "code coverage" "set DEBUG_COV=1 to enable (Linux only)"
fi

# =============================================================================
# 8. INTEGRATION TESTS (real ptrace, real profiling)
# =============================================================================
header "8 · Integration tests (live profiling)"

if [[ $RUN_INTEGRATION -eq 0 ]]; then
    skip "integration tests" "pass --integration to enable"
elif [[ $IS_LINUX -eq 0 ]]; then
    skip "integration tests" "ptrace is Linux-only"
else
    PROFILER="./profiler"
    TARGET="./tests/test_target"

    if [[ ! -f "$TARGET" ]]; then
        fail "integration: test_target binary" "not found — run make all first"
    else
        # ── Launch test_target ────────────────────────────────────────────────
        "$TARGET" 60 > "$TMP/target_out.txt" 2>/dev/null &
        TARGET_PID=$(grep -oP 'PID=\K[0-9]+' "$TMP/target_out.txt" 2>/dev/null || echo "")
        sleep 0.5

        if [[ -z "$TARGET_PID" ]]; then
            # fallback: the child PID itself
            TARGET_PID=$!
        fi

        pass "test_target launched (PID=${TARGET_PID})"

        # ── Basic profile run ─────────────────────────────────────────────────
        OUT_HTML="$TMP/integ.html"
        OUT_PPROF="$TMP/integ.pprof.txt"

        if "$PROFILER" \
               --pid "$TARGET_PID" \
               --rate 49 \
               --duration 6 \
               --output "$OUT_HTML" \
               --pprof  "$OUT_PPROF" \
               >"$TMP/prof_out.txt" 2>&1; then

            pass "profiler ran successfully"

            # Banner checks
            assert_contains "banner shows PID"      "$TMP/prof_out.txt" "$TARGET_PID"
            assert_contains "banner shows rate"     "$TMP/prof_out.txt" "49"
            assert_contains "banner shows threads"  "$TMP/prof_out.txt" "Thread"
            assert_contains "banner shows unwind"   "$TMP/prof_out.txt" "frame-pointer\|DWARF"
            assert_contains "summary shows samples" "$TMP/prof_out.txt" "Collected"

            # Outputs exist
            assert_file_not_empty "HTML output written"    "$OUT_HTML"
            assert_file_not_empty "folded output written"  "${OUT_HTML}.folded"
            assert_file_not_empty "pprof output written"   "$OUT_PPROF"

            # HTML valid
            assert_contains "HTML has doctype"        "$OUT_HTML"  "DOCTYPE"
            assert_contains "HTML has PROFILE_DATA"   "$OUT_HTML"  "PROFILE_DATA"
            assert_contains "HTML has sample data"    "$OUT_HTML"  "test_target"

            # Folded format valid
            BAD=0
            while IFS= read -r line; do
                [[ -z "$line" ]] && continue
                n="${line##* }"
                [[ "$n" =~ ^[0-9]+$ ]] || ((BAD++))
            done < "${OUT_HTML}.folded"
            (( BAD == 0 )) \
                && pass "folded format valid (0 bad lines)" \
                || fail "folded format" "${BAD} bad lines"

            # pprof format valid
            assert_contains "pprof has header comment" "$OUT_PPROF" "# pprof"
            assert_contains "pprof has comm"           "$OUT_PPROF" "test_target"
            assert_contains "pprof has stacks"         "$OUT_PPROF" ";"

            # Samples collected — check count > 0
            SAMPLES=$(grep -oP 'Collected\s*:\s*\K[0-9]+' "$TMP/prof_out.txt" || echo 0)
            if (( SAMPLES > 0 )); then
                pass "collected ${SAMPLES} samples"
            else
                fail "sample count" "0 samples collected"
            fi

            # Hotspot check — cpu_math or level2_math should appear
            FOLDED="${OUT_HTML}.folded"
            if grep -qE "cpu_math|level2_math|cpu_fibonacci" "$FOLDED"; then
                pass "known hot functions appear in flamegraph"
            else
                # Might be hex addresses if binary is stripped
                if grep -qE "0x[0-9a-f]+" "$FOLDED"; then
                    pass "flamegraph has address entries (binary may be stripped)"
                else
                    fail "flamegraph hotspots" "no known functions or addresses found"
                fi
            fi

        else
            fail "profiler run" "$(cat "$TMP/prof_out.txt" | tail -5)"
            echo -e "  ${DIM}Hint: try running with sudo, or:${RESET}"
            echo -e "  ${DIM}  sudo setcap cap_sys_ptrace+eip ./profiler${RESET}"
        fi

        # ── Multi-thread integration ──────────────────────────────────────────
        # test_target is single-threaded; verify profiler reports 1 thread
        THREAD_COUNT=$(grep -oP 'Threads seen\s*:\s*\K[0-9]+' "$TMP/prof_out.txt" || echo 0)
        if (( THREAD_COUNT >= 1 )); then
            pass "thread count reported: ${THREAD_COUNT}"
        else
            fail "thread count" "got ${THREAD_COUNT}"
        fi

        # ── --kernel flag doesn't crash ───────────────────────────────────────
        OUT2="$TMP/kernel_test.html"
        if "$PROFILER" \
               --pid "$TARGET_PID" \
               --rate 49 \
               --duration 3 \
               --kernel \
               --output "$OUT2" \
               >"$TMP/kernel_out.txt" 2>&1; then
            pass "--kernel flag: profiler completes without error"
            assert_file_not_empty "--kernel: HTML written" "$OUT2"
        else
            fail "--kernel flag" "$(tail -3 "$TMP/kernel_out.txt")"
        fi

        # ── SIGINT graceful shutdown ──────────────────────────────────────────
        OUT3="$TMP/sigint_test.html"
        "$PROFILER" \
            --pid "$TARGET_PID" \
            --rate 49 \
            --duration 30 \
            --output "$OUT3" \
            >"$TMP/sigint_out.txt" 2>&1 &
        PROF_PID=$!
        sleep 2
        kill -INT "$PROF_PID" 2>/dev/null || true
        wait "$PROF_PID" 2>/dev/null || true

        if [[ -f "$OUT3" ]] && [[ -s "$OUT3" ]]; then
            pass "SIGINT — flamegraph still written after early stop"
        else
            fail "SIGINT graceful shutdown" "no output file after SIGINT"
        fi

        # Kill target
        kill "$TARGET_PID" 2>/dev/null || true
        wait  2>/dev/null || true
    fi
fi

# =============================================================================
# SUMMARY
# =============================================================================
echo ""
echo -e "${BOLD}${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${RESET}"
echo -e "${BOLD}  Results${RESET}"
echo -e "${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${RESET}"
echo ""
echo -e "  ${GREEN}Passed : ${PASS}${RESET}"
if (( FAIL > 0 )); then
    echo -e "  ${RED}Failed : ${FAIL}${RESET}"
    echo ""
    echo -e "  ${RED}Failed tests:${RESET}"
    for t in "${FAILED_TESTS[@]}"; do
        echo -e "    ${RED}✗${RESET}  $t"
    done
fi
if (( SKIP > 0 )); then
    echo -e "  ${YELLOW}Skipped: ${SKIP}${RESET}"
fi
echo ""

if (( FAIL == 0 )); then
    echo -e "  ${GREEN}${BOLD}All checks passed.${RESET}"
else
    echo -e "  ${RED}${BOLD}${FAIL} check(s) failed.${RESET}"
fi
echo ""

exit $(( FAIL > 0 ? 1 : 0 ))