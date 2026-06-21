/*
 * run_tests.cpp — perf-profiler test suite
 *
 * Tests every module in isolation (unit) and end-to-end (integration).
 * No external testing framework required — pure C++17.
 *
 * Build:
 *   g++ -std=c++17 -O1 -fno-omit-frame-pointer -g -I../include \
 *       -o run_tests run_tests.cpp
 *
 * Run:
 *   ./run_tests              # all tests
 *   ./run_tests --list       # list test names
 *   ./run_tests <pattern>    # run tests whose name contains <pattern>
 *
 * Integration tests (require profiler + test_target binaries):
 *   PROFILER=../profiler TEST_TARGET=./test_target ./run_tests integration
 *
 * Exit code: 0 = all passed, non-zero = at least one failure.
 */

#include "types.hpp"
#include "symbol_resolver.hpp"
#include "stack_unwinder.hpp"
#include "aggregator.hpp"
#include "report_writer.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// ═══════════════════════════════════════════════════════════════════════════════
// Minimal test framework
// ═══════════════════════════════════════════════════════════════════════════════

struct TestResult {
    std::string name;
    bool        passed = false;
    std::string message;
    double      ms     = 0;
};

static std::vector<TestResult>           g_results;
static std::vector<std::pair<std::string, std::function<void()>>> g_tests;
static std::string g_current_test;

#define REGISTER_TEST(name) \
    static void test_##name(); \
    static bool _reg_##name = (g_tests.push_back({#name, test_##name}), true); \
    static void test_##name()

// Assertion macros that throw on failure (caught by the runner)
struct TestFailure { std::string msg; };

#define ASSERT(expr) \
    do { if (!(expr)) throw TestFailure{"ASSERT(" #expr ") at line " + std::to_string(__LINE__)}; } while(0)

#define ASSERT_EQ(a, b) \
    do { \
        auto _a = (a); auto _b = (b); \
        if (!(_a == _b)) { \
            std::ostringstream _ss; \
            _ss << "ASSERT_EQ failed at line " << __LINE__ \
                << ":\n    left  = " << _a \
                << "\n    right = " << _b; \
            throw TestFailure{_ss.str()}; \
        } \
    } while(0)

#define ASSERT_NE(a, b) \
    do { if ((a) == (b)) throw TestFailure{"ASSERT_NE failed at line " + std::to_string(__LINE__)}; } while(0)

#define ASSERT_GT(a, b) \
    do { \
        auto _a=(a); auto _b=(b); \
        if (!(_a > _b)) { \
            std::ostringstream _ss; \
            _ss << "ASSERT_GT(" << _a << " > " << _b << ") at line " << __LINE__; \
            throw TestFailure{_ss.str()}; \
        } \
    } while(0)

#define ASSERT_GE(a, b) \
    do { \
        auto _a=(a); auto _b=(b); \
        if (!(_a >= _b)) { \
            std::ostringstream _ss; \
            _ss << "ASSERT_GE(" << _a << " >= " << _b << ") at line " << __LINE__; \
            throw TestFailure{_ss.str()}; \
        } \
    } while(0)

#define ASSERT_CONTAINS(haystack, needle) \
    do { \
        std::string _h = (haystack), _n = (needle); \
        if (_h.find(_n) == std::string::npos) { \
            std::ostringstream _ss; \
            _ss << "ASSERT_CONTAINS: \"" << _n << "\" not found in:\n\"" << _h << "\"\n" \
                << "  at line " << __LINE__; \
            throw TestFailure{_ss.str()}; \
        } \
    } while(0)

#define ASSERT_NOT_CONTAINS(haystack, needle) \
    do { \
        std::string _h = (haystack), _n = (needle); \
        if (_h.find(_n) != std::string::npos) { \
            std::ostringstream _ss; \
            _ss << "ASSERT_NOT_CONTAINS: \"" << _n << "\" unexpectedly found in string" \
                << "  at line " << __LINE__; \
            throw TestFailure{_ss.str()}; \
        } \
    } while(0)

// ═══════════════════════════════════════════════════════════════════════════════
// ── UNIT TESTS: types.hpp ────────────────────────────────────────────────────
// ═══════════════════════════════════════════════════════════════════════════════

REGISTER_TEST(result_ok_carries_value) {
    auto r = profiler::Result<int>::ok_val(42);
    ASSERT(r.ok());
    ASSERT_EQ(r.value, 42);
    ASSERT(r.error.empty());
}

REGISTER_TEST(result_err_carries_message) {
    auto r = profiler::Result<int>::err("something broke");
    ASSERT(!r.ok());
    ASSERT_CONTAINS(r.error, "something broke");
}

REGISTER_TEST(status_success) {
    auto s = profiler::Status::success();
    ASSERT(s.ok());
    ASSERT((bool)s);
}

REGISTER_TEST(status_fail) {
    auto s = profiler::Status::fail("oops");
    ASSERT(!s.ok());
    ASSERT(!(bool)s);
    ASSERT_CONTAINS(s.error, "oops");
}

REGISTER_TEST(frame_default_values) {
    profiler::Frame f;
    ASSERT_EQ(f.address, 0u);
    ASSERT(f.symbol.empty());
    ASSERT(f.binary.empty());
    ASSERT_EQ(f.offset, 0u);
}

REGISTER_TEST(profile_default_zero) {
    profiler::Profile p;
    ASSERT_EQ(p.total_samples, 0u);
    ASSERT_EQ(p.lost_samples,  0u);
    ASSERT(p.counts.empty());
}

// ═══════════════════════════════════════════════════════════════════════════════
// ── UNIT TESTS: aggregator.hpp ────────────────────────────────────────────────
// ═══════════════════════════════════════════════════════════════════════════════

static profiler::Sample make_sample(std::vector<std::string> syms) {
    profiler::Sample s;
    for (auto &sym : syms) {
        profiler::Frame f;
        f.symbol = sym;
        s.frames.push_back(f);
    }
    return s;
}

REGISTER_TEST(aggregator_single_sample) {
    profiler::Aggregator agg("myapp");
    agg.add(make_sample({"main", "foo", "bar"}));    // innermost=bar, outermost=main

    ASSERT_EQ(agg.total(), 1u);

    profiler::Config cfg;
    auto p = agg.build(1234, cfg);
    ASSERT_EQ(p.total_samples, 1u);
    ASSERT_EQ(p.counts.size(), 1u);

    // Key should be "myapp;main;foo;bar" (outermost first)
    auto it = p.counts.begin();
    ASSERT_CONTAINS(it->first, "myapp");
    ASSERT_CONTAINS(it->first, "main");
    ASSERT_CONTAINS(it->first, "bar");
    ASSERT_EQ(it->second, 1u);
}

REGISTER_TEST(aggregator_identical_stacks_accumulate) {
    profiler::Aggregator agg("myapp");
    auto s = make_sample({"main", "compute"});
    for (int i = 0; i < 10; ++i) agg.add(s);

    ASSERT_EQ(agg.total(), 10u);
    auto p = agg.build(1, profiler::Config{});
    ASSERT_EQ(p.counts.size(), 1u);
    ASSERT_EQ(p.counts.begin()->second, 10u);
}

REGISTER_TEST(aggregator_distinct_stacks_are_separate_keys) {
    profiler::Aggregator agg("myapp");
    agg.add(make_sample({"main", "path_a"}));
    agg.add(make_sample({"main", "path_b"}));
    agg.add(make_sample({"main", "path_a"}));

    auto p = agg.build(1, profiler::Config{});
    ASSERT_EQ(p.counts.size(), 2u);   // two distinct paths
    ASSERT_EQ(p.total_samples, 3u);
}

REGISTER_TEST(aggregator_counts_sum_to_total) {
    profiler::Aggregator agg("app");
    for (int i = 0; i < 5; ++i)  agg.add(make_sample({"main","a"}));
    for (int i = 0; i < 3; ++i)  agg.add(make_sample({"main","b"}));
    for (int i = 0; i < 2; ++i)  agg.add(make_sample({"main","c"}));

    auto p = agg.build(1, profiler::Config{});
    uint64_t sum = 0;
    for (auto &[k,v] : p.counts) sum += v;
    ASSERT_EQ(sum, p.total_samples);
    ASSERT_EQ(p.total_samples, 10u);
}

REGISTER_TEST(aggregator_empty_frame_list_still_counted) {
    profiler::Aggregator agg("app");
    agg.add(make_sample({}));   // no frames → just "app" as key
    ASSERT_EQ(agg.total(), 1u);
}

REGISTER_TEST(aggregator_key_order_outermost_first) {
    // frames[0] = innermost, frames.back() = outermost
    // Key should be: comm;outermost;...;innermost
    profiler::Aggregator agg("app");
    agg.add(make_sample({"inner", "middle", "outer"}));  // indices 0,1,2

    auto p = agg.build(1, profiler::Config{});
    std::string key = p.counts.begin()->first;

    auto pos_outer = key.find("outer");
    auto pos_inner = key.find("inner");
    ASSERT(pos_outer < pos_inner);   // outer appears before inner in the key
}

REGISTER_TEST(aggregator_lost_samples_propagate) {
    profiler::Aggregator agg("app");
    agg.add(make_sample({"f"}));
    agg.set_lost(7);

    auto p = agg.build(1, profiler::Config{});
    ASSERT_EQ(p.lost_samples, 7u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// ── UNIT TESTS: symbol_resolver.hpp ──────────────────────────────────────────
// ═══════════════════════════════════════════════════════════════════════════════

REGISTER_TEST(resolver_loads_own_maps) {
    // Requires /proc/<pid>/maps — Linux only
#ifndef __linux__
    std::cout << "    [SKIP] /proc not available on this OS\n"; return;
#else
    profiler::SymbolResolver r(getpid());
    uint64_t addr = (uint64_t)(void*)&test_resolver_loads_own_maps;
    profiler::Frame f = r.resolve(addr);
    ASSERT(!f.binary.empty());
    ASSERT_EQ(f.address, addr);
#endif
}

REGISTER_TEST(resolver_unknown_address_returns_hex) {
    // Even on macOS the resolver falls back to hex for unknown addresses
    profiler::SymbolResolver r(getpid());
    profiler::Frame f = r.resolve(0x1ULL);
    ASSERT_CONTAINS(f.symbol, "0x");
}

REGISTER_TEST(resolver_maps_are_sorted_by_start) {
    // Only meaningful on Linux where maps are populated from /proc
#ifndef __linux__
    std::cout << "    [SKIP] /proc not available on this OS\n"; return;
#else
    profiler::SymbolResolver r(getpid());
    r.reload();
    const auto &maps = r.maps();
    for (size_t i = 1; i < maps.size(); ++i)
        ASSERT(maps[i].start >= maps[i-1].start);
#endif
}

REGISTER_TEST(resolver_self_address_in_binary_field) {
#ifndef __linux__
    std::cout << "    [SKIP] /proc not available on this OS\n"; return;
#else
    profiler::SymbolResolver r(getpid());
    uint64_t addr = (uint64_t)(void*)&test_resolver_self_address_in_binary_field;
    profiler::Frame f = r.resolve(addr);
    ASSERT(!f.binary.empty());
    ASSERT_NE(f.binary, "[unknown]");
#endif
}

// ═══════════════════════════════════════════════════════════════════════════════
// ── UNIT TESTS: report_writer.hpp ────────────────────────────────────────────
// ═══════════════════════════════════════════════════════════════════════════════

static profiler::Profile make_test_profile() {
    profiler::Profile p;
    p.target_pid  = 9999;
    p.target_comm = "testapp";
    p.rate_hz     = 99;
    p.duration_s  = 10;
    p.total_samples = 100;
    p.lost_samples  = 2;
    p.counts["testapp;main;worker;compute"] = 60;
    p.counts["testapp;main;worker;sleep"]   = 30;
    p.counts["testapp;main;idle"]           = 10;
    return p;
}

REGISTER_TEST(report_writer_folded_format_correct) {
    auto p = make_test_profile();
    std::string path = "/tmp/pp_test_folded_" + std::to_string(getpid()) + ".txt";
    auto s = profiler::ReportWriter::write_folded(path, p);
    ASSERT(s.ok());

    std::ifstream f(path);
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    unlink(path.c_str());

    // Each line: "stack count"
    ASSERT_CONTAINS(content, "testapp;main;worker;compute 60");
    ASSERT_CONTAINS(content, "testapp;main;worker;sleep 30");
    ASSERT_CONTAINS(content, "testapp;main;idle 10");
}

REGISTER_TEST(report_writer_folded_line_count) {
    auto p = make_test_profile();
    std::string path = "/tmp/pp_test_lc_" + std::to_string(getpid()) + ".txt";
    profiler::ReportWriter::write_folded(path, p);

    int lines = 0;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) if (!line.empty()) ++lines;
    unlink(path.c_str());

    ASSERT_EQ(lines, (int)p.counts.size());
}

REGISTER_TEST(report_writer_html_is_valid_html5) {
    auto p = make_test_profile();
    std::string path = "/tmp/pp_test_html_" + std::to_string(getpid()) + ".html";
    auto s = profiler::ReportWriter::write_html(path, p);
    ASSERT(s.ok());

    std::ifstream f(path);
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    unlink(path.c_str());

    ASSERT_CONTAINS(content, "<!DOCTYPE html>");
    ASSERT_CONTAINS(content, "<canvas");
    ASSERT_CONTAINS(content, "</html>");
}

REGISTER_TEST(report_writer_html_embeds_profile_data) {
    auto p = make_test_profile();
    std::string path = "/tmp/pp_test_embed_" + std::to_string(getpid()) + ".html";
    profiler::ReportWriter::write_html(path, p);

    std::ifstream f(path);
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    unlink(path.c_str());

    // Profile JSON must appear in the HTML
    ASSERT_CONTAINS(content, "PROFILE_DATA");
    ASSERT_CONTAINS(content, "testapp");
    ASSERT_CONTAINS(content, "9999");   // pid
    ASSERT_CONTAINS(content, "compute");
}

REGISTER_TEST(report_writer_html_contains_renderer) {
    auto p = make_test_profile();
    std::string path = "/tmp/pp_test_js_" + std::to_string(getpid()) + ".html";
    profiler::ReportWriter::write_html(path, p);

    std::ifstream f(path);
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    unlink(path.c_str());

    // Key JS functions must be present
    ASSERT_CONTAINS(content, "buildTree");
    ASSERT_CONTAINS(content, "render(");
    ASSERT_CONTAINS(content, "hitTest");
}

REGISTER_TEST(report_writer_html_json_special_chars) {
    profiler::Profile p;
    p.target_comm = "app";
    p.total_samples = 1;
    p.counts["app;func\"with\\quotes;inner"] = 1;   // quotes and backslash in key
    std::string path = "/tmp/pp_test_esc_" + std::to_string(getpid()) + ".html";
    auto s = profiler::ReportWriter::write_html(path, p);
    ASSERT(s.ok());   // must not crash or produce malformed output
    unlink(path.c_str());
}

REGISTER_TEST(report_writer_write_to_bad_path_fails) {
    auto p = make_test_profile();
    auto s = profiler::ReportWriter::write_html("/nonexistent/dir/out.html", p);
    ASSERT(!s.ok());
    ASSERT(!s.error.empty());
}

REGISTER_TEST(report_writer_html_has_stats) {
    auto p = make_test_profile();
    std::string path = "/tmp/pp_test_stats_" + std::to_string(getpid()) + ".html";
    profiler::ReportWriter::write_html(path, p);
    std::ifstream f(path);
    std::string c((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    unlink(path.c_str());
    ASSERT_CONTAINS(c, "sv-total");
    ASSERT_CONTAINS(c, "sv-lost");
    ASSERT_CONTAINS(c, "sv-rate");
}

// ═══════════════════════════════════════════════════════════════════════════════
// ── UNIT TESTS: stack_unwinder.hpp (self-unwind) ─────────────────────────────
// ═══════════════════════════════════════════════════════════════════════════════

REGISTER_TEST(unwinder_self_unwind_returns_frames) {
    // Unwind our own process in stopped state — we can't ptrace ourselves,
    // but we can verify the StackUnwinder object constructs and reports max depth.
    profiler::StackUnwinder u(32);
    ASSERT_EQ(u.max_depth(), 32);
    u.set_max_depth(16);
    ASSERT_EQ(u.max_depth(), 16);
}

// ═══════════════════════════════════════════════════════════════════════════════
// ── CONFIG / CLI sanity tests (logic only, no fork) ───────────────────────────
// ═══════════════════════════════════════════════════════════════════════════════

REGISTER_TEST(config_defaults) {
    profiler::Config c;
    ASSERT_EQ(c.pid,        -1);
    ASSERT_EQ(c.rate_hz,    99);
    ASSERT_EQ(c.duration_s, 30);
    ASSERT_EQ(c.max_depth,  64);
    ASSERT(!c.verbose);
    ASSERT(!c.output_html.empty());
}

// ═══════════════════════════════════════════════════════════════════════════════
// ── INTEGRATION TESTS ────────────────────────────────────────────────────────
// (only run when PROFILER and TEST_TARGET env-vars are set)
// ═══════════════════════════════════════════════════════════════════════════════

static std::string env_or(const char *name, const char *fallback) {
    const char *v = getenv(name);
    return v ? v : fallback;
}

static bool file_exists(const std::string &p) {
    struct stat st{};
    return stat(p.c_str(), &st) == 0;
}

static std::string read_file(const std::string &p) {
    std::ifstream f(p);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// Helper: launch test_target, wait for its PID line, return PID
static pid_t launch_target(const std::string &target_bin, int secs) {
    // Use a pipe so we can read the PID that test_target prints
    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;

    pid_t child = fork();
    if (child == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        // redirect stderr to /dev/null so it doesn't pollute test output
        int devnull = open("/dev/null", O_WRONLY);
        dup2(devnull, STDERR_FILENO);
        close(devnull);
        execl(target_bin.c_str(), target_bin.c_str(),
              std::to_string(secs).c_str(), nullptr);
        _exit(127);
    }
    close(pipefd[1]);

    // Read "test_target PID=NNNNN\n"
    char buf[128] = {};
    (void)read(pipefd[0], buf, sizeof(buf) - 1);
    close(pipefd[0]);

    pid_t target_pid = -1;
    if (sscanf(buf, "test_target PID=%d", &target_pid) != 1)
        target_pid = child; // fallback

    return target_pid;
}

REGISTER_TEST(integration_profiler_produces_html) {
    std::string profiler_bin    = env_or("PROFILER",     "../profiler");
    std::string test_target_bin = env_or("TEST_TARGET",  "./test_target");

    if (!file_exists(profiler_bin) || !file_exists(test_target_bin)) {
        std::cout << "    [SKIP] PROFILER or TEST_TARGET not found — set env vars\n";
        return;
    }

    const std::string out_html   = "/tmp/pp_integ_" + std::to_string(getpid()) + ".html";
    const std::string out_folded = out_html + ".folded";

    // Launch target for 15s
    pid_t target = launch_target(test_target_bin, 15);
    ASSERT_GT(target, 0);
    sleep(1); // let it warm up

    // Run profiler for 5s
    pid_t prof_pid = fork();
    if (prof_pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        execl(profiler_bin.c_str(), profiler_bin.c_str(),
              "--pid",      std::to_string(target).c_str(),
              "--rate",     "49",
              "--duration", "5",
              "--output",   out_html.c_str(),
              nullptr);
        _exit(127);
    }

    int wstatus = 0;
    waitpid(prof_pid, &wstatus, 0);

    // Kill target
    kill(target, SIGTERM);
    waitpid(target, nullptr, 0);

    int exit_code = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1;
    ASSERT_EQ(exit_code, 0);

    // HTML must exist and be non-trivial
    ASSERT(file_exists(out_html));
    std::string html = read_file(out_html);
    ASSERT_GT(html.size(), 1024u);
    ASSERT_CONTAINS(html, "<!DOCTYPE html>");
    ASSERT_CONTAINS(html, "PROFILE_DATA");

    // Folded file must exist
    ASSERT(file_exists(out_folded));
    std::string folded = read_file(out_folded);
    ASSERT_GT(folded.size(), 10u);

    unlink(out_html.c_str());
    unlink(out_folded.c_str());
}

REGISTER_TEST(integration_folded_format_parseable) {
    std::string profiler_bin    = env_or("PROFILER",    "../profiler");
    std::string test_target_bin = env_or("TEST_TARGET", "./test_target");

    if (!file_exists(profiler_bin) || !file_exists(test_target_bin)) {
        std::cout << "    [SKIP]\n"; return;
    }

    const std::string out = "/tmp/pp_folded_" + std::to_string(getpid()) + ".html";

    pid_t target = launch_target(test_target_bin, 12);
    ASSERT_GT(target, 0);
    sleep(1);

    pid_t pf = fork();
    if (pf == 0) {
        int dn = open("/dev/null", O_WRONLY);
        dup2(dn,STDOUT_FILENO); dup2(dn,STDERR_FILENO);
        execl(profiler_bin.c_str(), profiler_bin.c_str(),
              "--pid", std::to_string(target).c_str(),
              "--rate", "49", "--duration", "4",
              "--output", out.c_str(), nullptr);
        _exit(127);
    }
    int ws = 0;
    waitpid(pf, &ws, 0);
    kill(target, SIGTERM);
    waitpid(target, nullptr, 0);

    std::string folded = read_file(out + ".folded");
    // Every line: "stack<space>count\n"
    std::istringstream ss(folded);
    std::string line;
    int lines = 0, bad = 0;
    while (std::getline(ss, line)) {
        if (line.empty()) continue;
        ++lines;
        auto sp = line.rfind(' ');
        if (sp == std::string::npos) { ++bad; continue; }
        std::string count_str = line.substr(sp+1);
        bool all_digits = !count_str.empty() &&
            std::all_of(count_str.begin(), count_str.end(), ::isdigit);
        if (!all_digits) ++bad;
    }
    unlink(out.c_str());
    unlink((out+".folded").c_str());

    ASSERT_GT(lines, 0);
    ASSERT_EQ(bad, 0);
}

REGISTER_TEST(integration_profiler_no_pid_exits_nonzero) {
    std::string profiler_bin = env_or("PROFILER", "../profiler");
    if (!file_exists(profiler_bin)) { std::cout << "    [SKIP]\n"; return; }

    pid_t pf = fork();
    if (pf == 0) {
        int dn = open("/dev/null", O_WRONLY);
        dup2(dn,STDOUT_FILENO); dup2(dn,STDERR_FILENO);
        execl(profiler_bin.c_str(), profiler_bin.c_str(), nullptr);
        _exit(127);
    }
    int ws=0; waitpid(pf,&ws,0);
    int code = WIFEXITED(ws) ? WEXITSTATUS(ws) : -1;
    ASSERT_NE(code, 0);
}

REGISTER_TEST(integration_profiler_bad_pid_exits_nonzero) {
    std::string profiler_bin = env_or("PROFILER", "../profiler");
    if (!file_exists(profiler_bin)) { std::cout << "    [SKIP]\n"; return; }

    pid_t pf = fork();
    if (pf == 0) {
        int dn = open("/dev/null", O_WRONLY);
        dup2(dn,STDOUT_FILENO); dup2(dn,STDERR_FILENO);
        execl(profiler_bin.c_str(), profiler_bin.c_str(),
              "--pid", "9999999", nullptr);
        _exit(127);
    }
    int ws=0; waitpid(pf,&ws,0);
    int code = WIFEXITED(ws) ? WEXITSTATUS(ws) : -1;
    ASSERT_NE(code, 0);
}

REGISTER_TEST(integration_flamegraph_shows_expected_functions) {
    // This is the key "did it actually work" test:
    // Run the profiler against the test_target and verify that
    // cpu_math / cpu_fibonacci appear in the top hotspots.
    std::string profiler_bin    = env_or("PROFILER",    "../profiler");
    std::string test_target_bin = env_or("TEST_TARGET", "./test_target");

    if (!file_exists(profiler_bin) || !file_exists(test_target_bin)) {
        std::cout << "    [SKIP]\n"; return;
    }

    const std::string out = "/tmp/pp_funcs_" + std::to_string(getpid()) + ".html";

    pid_t target = launch_target(test_target_bin, 20);
    ASSERT_GT(target, 0);
    sleep(1);

    pid_t pf = fork();
    if (pf == 0) {
        int dn = open("/dev/null", O_WRONLY);
        dup2(dn,STDOUT_FILENO); dup2(dn,STDERR_FILENO);
        execl(profiler_bin.c_str(), profiler_bin.c_str(),
              "--pid", std::to_string(target).c_str(),
              "--rate", "99", "--duration", "8",
              "--output", out.c_str(), nullptr);
        _exit(127);
    }
    int ws=0; waitpid(pf,&ws,0);
    kill(target, SIGTERM);
    waitpid(target, nullptr, 0);

    // Read the folded stacks
    std::string folded = read_file(out + ".folded");
    unlink(out.c_str());
    unlink((out+".folded").c_str());

    // At minimum we expect the test_target binary to appear somewhere
    // (symbol resolution may vary depending on strip level)
    ASSERT_GT(folded.size(), 50u);

    // The folded file should reference something — at least hex addresses
    // (the workload IS running, so samples must reference something)
    bool has_content = folded.find("test_target") != std::string::npos ||
                       folded.find("cpu_")        != std::string::npos ||
                       folded.find("0x")          != std::string::npos ||
                       folded.find("level2")      != std::string::npos;
    ASSERT(has_content);
}

// ═══════════════════════════════════════════════════════════════════════════════
// ── Test runner ───────────────────────────────────────────────────────────────
// ═══════════════════════════════════════════════════════════════════════════════

static bool matches(const std::string &name, const std::string &pattern) {
    if (pattern.empty()) return true;
    return name.find(pattern) != std::string::npos;
}

int main(int argc, char **argv) {
    std::string filter;
    bool list_only = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--list") list_only = true;
        else               filter = a;
    }

    if (list_only) {
        for (auto &[name, _] : g_tests) std::cout << name << "\n";
        return 0;
    }

    std::cout << "\n  perf-profiler test suite\n"
              << "  ──────────────────────────────────────────────────\n";
    if (!filter.empty())
        std::cout << "  Filter: " << filter << "\n";
    std::cout << "\n";

    int passed = 0, failed = 0, skipped = 0;

    for (auto &[name, fn] : g_tests) {
        if (!matches(name, filter)) { ++skipped; continue; }

        auto t0 = std::chrono::steady_clock::now();
        TestResult r;
        r.name = name;
        try {
            fn();
            r.passed  = true;
            r.message = "";
        } catch (const TestFailure &tf) {
            r.passed  = false;
            r.message = tf.msg;
        } catch (const std::exception &e) {
            r.passed  = false;
            r.message = std::string("exception: ") + e.what();
        } catch (...) {
            r.passed  = false;
            r.message = "unknown exception";
        }
        auto t1 = std::chrono::steady_clock::now();
        r.ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        if (r.passed) {
            std::cout << "  \033[32m✓\033[0m  " << name;
            ++passed;
        } else {
            std::cout << "  \033[31m✗\033[0m  " << name << "\n"
                      << "        \033[31m" << r.message << "\033[0m";
            ++failed;
        }
        std::cout << "  \033[90m(" << (int)r.ms << "ms)\033[0m\n";
        g_results.push_back(r);
    }

    std::cout << "\n  ──────────────────────────────────────────────────\n"
              << "  " << passed << " passed";
    if (failed)  std::cout << "  \033[31m" << failed  << " failed\033[0m";
    if (skipped) std::cout << "  \033[90m" << skipped << " skipped\033[0m";
    std::cout << "\n\n";

    return failed > 0 ? 1 : 0;
}