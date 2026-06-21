/*
 * debug_runner.cpp — Interactive terminal debugger for perf-profiler internals
 *
 * This is NOT a test suite. It is a live walkthrough of every module.
 * You run it, pick a test case from the menu, and watch every internal
 * step print to the terminal in real time.
 *
 * Build:
 *   g++ -std=c++17 -O0 -g -fno-omit-frame-pointer -I../include \
 *       -o debug_runner debug_runner.cpp
 *
 * Run:
 *   ./debug_runner
 *
 * No root needed. No ptrace. Shows the logic of every module with
 * real data so you can see exactly what's happening inside.
 */

#include "types.hpp"
#include "aggregator.hpp"
#include "report_writer.hpp"
#include "symbol_resolver.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

// ── Terminal colours ──────────────────────────────────────────────────────────
#define CLR_RESET  "\033[0m"
#define CLR_BOLD   "\033[1m"
#define CLR_DIM    "\033[2m"
#define CLR_RED    "\033[31m"
#define CLR_GREEN  "\033[32m"
#define CLR_YELLOW "\033[33m"
#define CLR_BLUE   "\033[34m"
#define CLR_CYAN   "\033[36m"
#define CLR_WHITE  "\033[37m"

static void pause_for_user(const std::string &msg = "  [press ENTER to continue]") {
    std::cout << CLR_DIM << msg << CLR_RESET << "\n";
    std::cin.get();
}

static void section(const std::string &title) {
    std::cout << "\n" << CLR_BOLD << CLR_CYAN
              << "┌─────────────────────────────────────────────────────┐\n"
              << "│  " << std::left << std::setw(51) << title << "│\n"
              << "└─────────────────────────────────────────────────────┘"
              << CLR_RESET << "\n\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
}

static void step(int n, const std::string &what) {
    std::cout << CLR_BOLD << CLR_YELLOW << "  [step " << n << "] " << CLR_RESET
              << what << "\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
}

static void log(const std::string &msg) {
    std::cout << CLR_DIM << "           " << CLR_RESET << msg << "\n";
}

static void ok(const std::string &msg) {
    std::cout << CLR_GREEN << "  ✓  " << CLR_RESET << msg << "\n";
}

static void info(const std::string &label, const std::string &val) {
    std::cout << "  " << CLR_DIM << std::left << std::setw(28) << label
              << CLR_RESET << CLR_BLUE << val << CLR_RESET << "\n";
}

static void arrow(const std::string &from, const std::string &to,
                  const std::string &note = "") {
    std::cout << "  " << CLR_YELLOW << from << CLR_RESET
              << "  →  " << CLR_GREEN << to << CLR_RESET;
    if (!note.empty()) std::cout << "  " << CLR_DIM << "(" << note << ")" << CLR_RESET;
    std::cout << "\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
}

// ═════════════════════════════════════════════════════════════════════════════
// TEST CASE 1 — types.hpp : Result<T> and Status
// Shows how error handling works across the whole codebase
// ═════════════════════════════════════════════════════════════════════════════

void tc_types() {
    section("TEST 1 — types.hpp  (Result<T> and Status)");

    std::cout << "Every function in this project returns either Result<T> or Status\n"
              << "instead of throwing exceptions. Here is what that looks like live.\n\n";

    pause_for_user();

    step(1, "Create a successful Result<int>");
    auto r1 = profiler::Result<int>::ok_val(42);
    log("profiler::Result<int>::ok_val(42)");
    info("r1.ok()    →", r1.ok() ? "true" : "false");
    info("r1.value   →", std::to_string(r1.value));
    info("r1.error   →", r1.error.empty() ? "(empty)" : r1.error);
    ok("Value carried through cleanly");

    std::cout << "\n";
    pause_for_user();

    step(2, "Create a failed Result<int>");
    auto r2 = profiler::Result<int>::err("ptrace SEIZE: Operation not permitted");
    log("profiler::Result<int>::err(\"ptrace SEIZE: ...\")");
    info("r2.ok()    →", r2.ok() ? "true" : "false");
    info("r2.value   →", std::to_string(r2.value) + "  (zero — don't use this)");
    info("r2.error   →", r2.error);
    ok("Error message carried through — caller can print it directly");

    std::cout << "\n";
    pause_for_user();

    step(3, "Status (void-like result — success)");
    auto s1 = profiler::Status::success();
    log("profiler::Status::success()");
    info("s1.ok()    →", s1.ok() ? "true" : "false");
    info("bool(s1)   →", (bool)s1 ? "true" : "false");
    ok("Used by report_writer::write_html() etc. when there is no value to return");

    std::cout << "\n";
    pause_for_user();

    step(4, "Status (failure)");
    auto s2 = profiler::Status::fail("Cannot open: /nonexistent/dir/out.html");
    log("profiler::Status::fail(\"Cannot open: ...\")");
    info("s2.ok()    →", s2.ok() ? "true" : "false");
    info("s2.error   →", s2.error);
    ok("main.cpp checks if(!status) and prints status.error — one consistent pattern everywhere");

    pause_for_user();
}

// ═════════════════════════════════════════════════════════════════════════════
// TEST CASE 2 — aggregator.hpp
// Shows exactly how raw samples become the collapsed stack counts
// ═════════════════════════════════════════════════════════════════════════════

static profiler::Sample make_sample(const std::string & /*comm*/,
                                    std::vector<std::string> syms) {
    profiler::Sample s;
    for (auto &sym : syms) {
        profiler::Frame f;
        f.symbol = sym;
        s.frames.push_back(f);
    }
    return s;
}

void tc_aggregator() {
    section("TEST 2 — aggregator.hpp  (samples → collapsed stacks)");

    std::cout << "The aggregator receives one Sample per profiler interrupt.\n"
              << "Each Sample is a call stack — a list of frames, innermost first.\n"
              << "It converts them into the 'collapsed stack' format used by flamegraphs.\n\n";

    pause_for_user();

    profiler::Aggregator agg("test_target");

    step(1, "Feed sample #1 — deep in cpu_math");
    auto s1 = make_sample("test_target",
        {"cpu_math", "level2_math", "workload_loop", "main"});
    std::cout << "\n  Raw frames (frames[0] = innermost):\n";
    for (int i = 0; i < (int)s1.frames.size(); ++i)
        std::cout << "    frames[" << i << "] = " << CLR_BLUE
                  << s1.frames[i].symbol << CLR_RESET << "\n";
    agg.add(s1);
    std::cout << "\n  Aggregator sees this and builds the key:\n";
    std::cout << "    comm  = test_target\n";
    std::cout << "    then appends frames in REVERSE (outermost → innermost):\n";
    std::cout << CLR_GREEN
              << "    key = \"test_target;main;workload_loop;level2_math;cpu_math\"\n"
              << CLR_RESET;
    std::cout << "    counts[key]++ → now 1\n\n";

    pause_for_user();

    step(2, "Feed sample #2 — same stack again (program still in cpu_math)");
    agg.add(s1);
    std::cout << "    Same key hit again → counts[key]++ → now 2\n\n";

    pause_for_user();

    step(3, "Feed sample #3 — now in cpu_fibonacci");
    auto s2 = make_sample("test_target",
        {"cpu_fibonacci", "level2_fib", "workload_loop", "main"});
    std::cout << "\n  Raw frames:\n";
    for (int i = 0; i < (int)s2.frames.size(); ++i)
        std::cout << "    frames[" << i << "] = " << CLR_BLUE
                  << s2.frames[i].symbol << CLR_RESET << "\n";
    agg.add(s2);
    std::cout << "\n  New key created:\n"
              << CLR_GREEN
              << "    \"test_target;main;workload_loop;level2_fib;cpu_fibonacci\"\n"
              << CLR_RESET
              << "    counts[new_key]++ → 1\n\n";

    pause_for_user();

    step(4, "Feed 7 more samples with the same math distribution");
    for (int i = 0; i < 5; ++i) agg.add(s1);  // cpu_math x5
    for (int i = 0; i < 2; ++i) agg.add(s2);  // cpu_fibonacci x2
    std::cout << "    Added 5 more cpu_math, 2 more cpu_fibonacci\n\n";

    pause_for_user();

    step(5, "Build the final Profile and inspect counts");
    profiler::Config cfg;
    cfg.rate_hz    = 99;
    cfg.duration_s = 10;
    auto profile = agg.build(getpid(), cfg);

    std::cout << "\n  Profile summary:\n";
    info("total_samples →", std::to_string(profile.total_samples));
    info("unique stacks →", std::to_string(profile.counts.size()));
    std::cout << "\n  All collapsed stacks (what goes into flamegraph):\n\n";

    for (auto &[key, cnt] : profile.counts) {
        double pct = 100.0 * cnt / profile.total_samples;
        std::cout << "  " << CLR_BLUE << std::setw(5) << cnt << CLR_RESET
                  << "  " << CLR_DIM << std::fixed << std::setprecision(1)
                  << std::setw(5) << pct << "%" << CLR_RESET
                  << "  " << key << "\n";
    }

    std::cout << "\n";
    ok("This exact map is what report_writer serialises to JSON and embeds in the HTML");
    pause_for_user();
}

// ═════════════════════════════════════════════════════════════════════════════
// TEST CASE 3 — symbol_resolver.hpp
// Shows /proc/self/maps parsing and ELF lookup in our own process
// ═════════════════════════════════════════════════════════════════════════════

void tc_resolver() {
    section("TEST 3 — symbol_resolver.hpp  (/proc/maps + ELF .symtab)");

    std::cout << "When the profiler captures a raw address like 0x401234,\n"
              << "it needs to turn that into a human-readable name.\n"
              << "This module does it in two steps: maps → ELF → symbol name.\n\n";

    pause_for_user();

#ifndef __linux__
    std::cout << CLR_YELLOW
              << "  [SKIP] /proc/self/maps does not exist on macOS.\n"
              << "  This module is Linux-only. On Linux you would see:\n"
              << "    - All memory regions of the process printed\n"
              << "    - Address → binary lookup\n"
              << "    - ELF .symtab binary search for function name\n"
              << CLR_RESET << "\n";
    pause_for_user();
    return;
#else
    profiler::SymbolResolver resolver(getpid());

    step(1, "Parse /proc/self/maps");
    resolver.reload();
    const auto &maps = resolver.maps();
    std::cout << "\n  Found " << CLR_BLUE << maps.size() << CLR_RESET
              << " memory regions for PID " << getpid() << "\n\n";
    std::cout << "  First 8 regions:\n";
    std::cout << "  " << CLR_DIM
              << std::left << std::setw(20) << "start"
              << std::setw(20) << "end"
              << std::setw(8)  << "exec?"
              << "path\n" << CLR_RESET;
    int shown = 0;
    for (auto &r : maps) {
        if (shown++ >= 8) break;
        char buf[64];
        snprintf(buf, sizeof(buf), "0x%lx", r.start);
        std::string s_str = buf;
        snprintf(buf, sizeof(buf), "0x%lx", r.end);
        std::string e_str = buf;
        std::cout << "  "
                  << std::left << std::setw(20) << s_str
                  << std::setw(20) << e_str
                  << std::setw(8)  << (r.exec ? "YES" : "no")
                  << (r.path.empty() ? "[anon]" : r.path) << "\n";
    }
    std::cout << "  " << CLR_DIM << "  ... " << (maps.size()-8)
              << " more regions\n" << CLR_RESET << "\n";

    pause_for_user();

    step(2, "Resolve this function's own address");
    uint64_t self_addr = (uint64_t)(void*)&tc_resolver;
    char addr_buf[32];
    snprintf(addr_buf, sizeof(addr_buf), "0x%lx", self_addr);

    std::cout << "\n  Raw address of tc_resolver() = " << CLR_YELLOW
              << addr_buf << CLR_RESET << "\n\n";
    std::cout << "  Resolver does binary search over " << maps.size()
              << " regions to find which one contains this address...\n\n";

    profiler::Frame f = resolver.resolve(self_addr);

    std::cout << "  Result:\n";
    info("f.address  →", addr_buf);
    info("f.binary   →", f.binary);
    info("f.offset   →", [&]{
        char b[32]; snprintf(b, sizeof(b), "0x%lx", f.offset); return std::string(b);
    }());
    info("f.symbol   →", f.symbol.empty() ? "(no debug symbols — stripped binary)" : f.symbol);

    std::cout << "\n";
    ok("Address resolved to binary name + file offset");

    pause_for_user();

    step(3, "Resolve an address that has no mapping");
    profiler::Frame f2 = resolver.resolve(0x1ULL);
    std::cout << "\n  Trying to resolve address 0x1...\n\n";
    info("f2.binary  →", f2.binary);
    info("f2.symbol  →", f2.symbol);
    ok("Falls back to hex — never crashes, always returns something");

    pause_for_user();

    step(4, "What happens with a kernel address (very high address)");
    profiler::Frame f3 = resolver.resolve(0xffffffff81000000ULL);
    char kaddr[32];
    snprintf(kaddr, sizeof(kaddr), "0x%lx", (uint64_t)0xffffffff81000000ULL);
    std::cout << "\n  Trying to resolve " << kaddr << " (kernel space)...\n\n";
    info("f3.binary  →", f3.binary);
    info("f3.symbol  →", f3.symbol);
    ok("Also falls back to hex — profiler has exclude_kernel=1 so this rarely happens");

    pause_for_user();
#endif
}

// ═════════════════════════════════════════════════════════════════════════════
// TEST CASE 4 — report_writer.hpp
// Shows the full pipeline: Profile → JSON → HTML → file
// ═════════════════════════════════════════════════════════════════════════════

void tc_report_writer() {
    section("TEST 4 — report_writer.hpp  (Profile → HTML flamegraph)");

    std::cout << "The report writer takes the aggregated Profile and turns it\n"
              << "into two things:\n"
              << "  1. A .folded file  (plain text, Brendan Gregg format)\n"
              << "  2. A .html file    (self-contained interactive flamegraph)\n\n";

    pause_for_user();

    step(1, "Build a realistic Profile (simulating 200 samples)");

    profiler::Profile p;
    p.target_pid    = 9999;
    p.target_comm   = "test_target";
    p.rate_hz       = 99;
    p.duration_s    = 10;
    p.total_samples = 200;
    p.lost_samples  = 3;
    p.counts["test_target;main;workload_loop;level2_math;cpu_math"]        = 110;
    p.counts["test_target;main;workload_loop;level2_fib;cpu_fibonacci"]    =  70;
    p.counts["test_target;main;workload_loop;level2_mem;cpu_memory"]       =  17;
    p.counts["test_target;main;workload_loop"]                             =   3;

    std::cout << "\n  Profile contents:\n";
    info("target_pid     →", std::to_string(p.target_pid));
    info("target_comm    →", p.target_comm);
    info("total_samples  →", std::to_string(p.total_samples));
    info("lost_samples   →", std::to_string(p.lost_samples));
    info("unique stacks  →", std::to_string(p.counts.size()));
    std::cout << "\n  Stack breakdown:\n";
    for (auto &[k, v] : p.counts) {
        double pct = 100.0 * v / p.total_samples;
        std::cout << "  " << CLR_BLUE << std::setw(5) << v << CLR_RESET
                  << "  " << CLR_DIM << std::fixed << std::setprecision(1)
                  << std::setw(5) << pct << "%" << CLR_RESET
                  << "  " << k << "\n";
    }

    pause_for_user();

    step(2, "Write .folded file and show its contents");
    std::string folded_path = "/tmp/debug_runner_demo.folded";
    auto s1 = profiler::ReportWriter::write_folded(folded_path, p);

    if (s1.ok()) {
        std::cout << "\n  Written to: " << CLR_GREEN << folded_path << CLR_RESET << "\n\n";
        std::ifstream f(folded_path);
        std::string line;
        std::cout << "  Contents:\n";
        while (std::getline(f, line))
            std::cout << "    " << CLR_DIM << line << CLR_RESET << "\n";
        std::cout << "\n";
        ok("This is the exact format flamegraph.pl expects");
    } else {
        std::cout << CLR_RED << "  Error: " << s1.error << CLR_RESET << "\n";
    }

    pause_for_user();

    step(3, "Write .html file");
    std::string html_path = "/tmp/debug_runner_demo.html";
    auto s2 = profiler::ReportWriter::write_html(html_path, p);

    if (s2.ok()) {
        // Count file size and lines
        std::ifstream f(html_path, std::ios::ate);
        long sz = f.tellg();
        f.seekg(0);
        int lines = 0;
        std::string line;
        while (std::getline(f, line)) ++lines;

        std::cout << "\n  Written to: " << CLR_GREEN << html_path << CLR_RESET << "\n";
        info("File size   →", std::to_string(sz) + " bytes");
        info("Lines       →", std::to_string(lines));
        std::cout << "\n";
        ok("Self-contained — no CDN, no internet, open in any browser");
    } else {
        std::cout << CLR_RED << "  Error: " << s2.error << CLR_RESET << "\n";
    }

    pause_for_user();

    step(4, "Show what the embedded JSON looks like");
    std::ifstream f(html_path);
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());

    auto start = content.find("const PROFILE_DATA=");
    auto end   = content.find(";\n", start);
    if (start != std::string::npos && end != std::string::npos) {
        std::string json = content.substr(start, end - start + 1);
        // Print first 800 chars
        std::cout << "\n  Embedded JSON (first 800 chars):\n\n";
        std::string snippet = json.substr(0, std::min((size_t)800, json.size()));
        // Indent each line
        std::istringstream ss(snippet);
        std::string jl;
        while (std::getline(ss, jl))
            std::cout << "    " << CLR_DIM << jl << CLR_RESET << "\n";
        if (json.size() > 800) std::cout << "    " << CLR_DIM << "  ...\n" << CLR_RESET;
        std::cout << "\n";
        ok("This JSON is parsed by the canvas renderer in the browser");
    }

    std::cout << "\n  To view the flamegraph:\n"
              << CLR_CYAN << "    open " << html_path << "  (macOS)\n"
              << "    xdg-open " << html_path << "  (Linux)\n" << CLR_RESET;

    pause_for_user();
}

// ═════════════════════════════════════════════════════════════════════════════
// TEST CASE 5 — Full pipeline walkthrough (no ptrace)
// Simulates what actually happens during a real profiling session
// ═════════════════════════════════════════════════════════════════════════════

void tc_full_pipeline() {
    section("TEST 5 — Full pipeline walkthrough (simulated)");

    std::cout << "This simulates exactly what happens when you run:\n\n"
              << CLR_CYAN
              << "    ./profiler --pid 1234 --rate 99 --duration 5\n\n"
              << CLR_RESET
              << "We skip the actual ptrace calls (needs Linux + root)\n"
              << "but show every other step with real data.\n\n";

    pause_for_user();

    // ── Step 1: Config ────────────────────────────────────────────────────────
    step(1, "Parse CLI args → Config struct");
    profiler::Config cfg;
    cfg.pid        = 1234;
    cfg.rate_hz    = 99;
    cfg.duration_s = 5;
    cfg.max_depth  = 64;
    cfg.output_html = "/tmp/debug_pipeline_out.html";

    info("cfg.pid        →", std::to_string(cfg.pid));
    info("cfg.rate_hz    →", std::to_string(cfg.rate_hz) + " Hz");
    info("cfg.duration_s →", std::to_string(cfg.duration_s) + " seconds");
    info("cfg.max_depth  →", std::to_string(cfg.max_depth) + " frames");
    info("cfg.output_html→", cfg.output_html);
    std::cout << "\n";
    ok("Config is passed by const-ref into every module — never copied");

    pause_for_user();

    // ── Step 2: Interval math ─────────────────────────────────────────────────
    step(2, "Calculate sampling interval");
    double interval_ms = 1000.0 / cfg.rate_hz;
    int expected_samples = cfg.rate_hz * cfg.duration_s;
    std::cout << "\n";
    info("1000ms / 99Hz  →", std::to_string(interval_ms).substr(0,7) + " ms between samples");
    info("99 * 5 seconds →", std::to_string(expected_samples) + " expected samples");
    std::cout << "\n  Each iteration:\n";
    arrow("usleep(10ms)", "PTRACE_INTERRUPT", "freeze target");
    arrow("PTRACE_INTERRUPT", "waitpid()", "wait for stop signal");
    arrow("waitpid()", "PTRACE_GETREGS", "read RIP + RBP registers");
    arrow("PTRACE_GETREGS", "process_vm_readv()", "walk frame chain");
    arrow("process_vm_readv()", "resolve_symbol()", "addr → name");
    arrow("resolve_symbol()", "aggregator.add()", "store sample");
    arrow("aggregator.add()", "PTRACE_CONT", "resume target");
    std::cout << "\n";
    ok("The target is paused for < 1ms out of every 10ms — barely noticeable");

    pause_for_user();

    // ── Step 3: Simulate 50 samples ──────────────────────────────────────────
    step(3, "Simulate 50 sample captures with realistic distribution");
    profiler::Aggregator agg("test_target");

    // Simulate realistic sample distribution based on test_target's workload
    struct FakeStack { std::vector<std::string> frames; int weight; };
    std::vector<FakeStack> stacks = {
        {{"cpu_math",      "level2_math", "workload_loop", "main"}, 27},
        {{"cpu_fibonacci", "level2_fib",  "workload_loop", "main"}, 17},
        {{"cpu_memory",    "level2_mem",  "workload_loop", "main"},  5},
        {{"workload_loop", "main"                                 },  1},
    };

    int total = 0;
    std::cout << "\n  Feeding samples:\n";
    for (auto &fs : stacks) {
        auto s = make_sample("test_target", fs.frames);
        for (int i = 0; i < fs.weight; ++i) agg.add(s);
        total += fs.weight;
        std::cout << "    " << CLR_BLUE << std::setw(3) << fs.weight << CLR_RESET
                  << " x  [" << fs.frames[0] << " → ... → "
                  << fs.frames.back() << "]\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }
    std::cout << "    " << CLR_DIM << "─────────────────────────────\n" << CLR_RESET;
    std::cout << "    " << CLR_BOLD << std::setw(3) << total
              << CLR_RESET << "   total samples\n\n";
    ok("Aggregator has been accumulating a hit-count map the whole time");

    pause_for_user();

    // ── Step 4: Build profile ─────────────────────────────────────────────────
    step(4, "Build Profile from aggregator");
    agg.set_lost(2);
    profiler::Profile profile = agg.build(cfg.pid, cfg);

    std::cout << "\n  Final profile:\n";
    info("total_samples  →", std::to_string(profile.total_samples));
    info("lost_samples   →", std::to_string(profile.lost_samples));
    info("unique stacks  →", std::to_string(profile.counts.size()));
    std::cout << "\n";

    for (auto &[k, v] : profile.counts) {
        double pct = 100.0 * v / profile.total_samples;
        std::string bar(int(pct / 2), '#');
        std::cout << "  " << CLR_BLUE << std::setw(3) << v << CLR_RESET
                  << " " << CLR_DIM << std::fixed << std::setprecision(1)
                  << std::setw(5) << pct << "%" << CLR_RESET
                  << "  " << CLR_GREEN << bar << CLR_RESET << "\n"
                  << "         " << CLR_DIM << k << CLR_RESET << "\n\n";
    }

    pause_for_user();

    // ── Step 5: Write output ──────────────────────────────────────────────────
    step(5, "Write outputs");
    std::string folded = cfg.output_html + ".folded";

    auto r1 = profiler::ReportWriter::write_folded(folded, profile);
    auto r2 = profiler::ReportWriter::write_html(cfg.output_html, profile);

    if (r1.ok()) ok("Folded stacks → " + folded);
    else std::cout << CLR_RED << "  Error: " << r1.error << CLR_RESET << "\n";

    if (r2.ok()) ok("HTML flamegraph → " + cfg.output_html);
    else std::cout << CLR_RED << "  Error: " << r2.error << CLR_RESET << "\n";

    std::cout << "\n  " << CLR_CYAN
              << "open " << cfg.output_html << CLR_RESET
              << "  ← open this in your browser to see the flamegraph\n\n";

    pause_for_user();
}

// ═════════════════════════════════════════════════════════════════════════════
// TEST CASE 6 — Stack unwinding explained step by step
// ═════════════════════════════════════════════════════════════════════════════

void tc_stack_unwinding() {
    section("TEST 6 — Stack unwinding  (frame-pointer walk explained)");

    std::cout << "This is the most important piece of the profiler.\n"
              << "When we freeze the target, we need to reconstruct its call stack.\n"
              << "Here is exactly how the frame-pointer walk works.\n\n";

    pause_for_user();

    step(1, "What is in memory when a function is called");
    std::cout << R"(
  When cpu_math() calls into a loop body, the x86-64 stack looks like this:

  HIGH ADDRESS
  ┌──────────────────────────────────────────────────┐
  │  main() stack frame                              │
  │    saved rbp  ──→ points to workload_loop's rbp  │
  │    return addr    (address in __libc_start_main)  │
  ├──────────────────────────────────────────────────┤
  │  workload_loop() frame                           │
  │    saved rbp  ──→ points to level2_math's rbp    │
  │    return addr    (address in main)               │
  ├──────────────────────────────────────────────────┤
  │  level2_math() frame                             │
  │    saved rbp  ──→ points to cpu_math's rbp       │
  │    return addr    (address in workload_loop)      │
  ├──────────────────────────────────────────────────┤
  │  cpu_math() frame     ← we are HERE              │
  │    local vars (acc, i, n, ...)                   │
  │    saved rbp          ← RBP register points here │
  │    return addr                                   │
  ├──────────────────────────────────────────────────┤
  │  ↓ (stack grows down)                            │
  LOW ADDRESS

  RIP (instruction pointer) = current address inside cpu_math
  RBP (frame pointer)       = address of "saved rbp" in cpu_math's frame
)";

    pause_for_user();

    step(2, "The unwind algorithm");
    std::cout << R"(
  1. Read RIP → frames[0]  (we're at this address right now)

  2. Read memory at RBP:
       [RBP + 0] = saved_rbp   (the caller's frame pointer)
       [RBP + 8] = return_addr (where we go when cpu_math returns)

  3. Push return_addr → frames[1]   (this is inside level2_math)
     Set RBP = saved_rbp             (now looking at level2_math's frame)

  4. Repeat:
       [RBP + 0] = next saved_rbp
       [RBP + 8] = next return_addr  (this is inside workload_loop)
     Push return_addr → frames[2]
     ...

  5. Stop when:
       RBP == 0  (no more frames, hit the bottom of the stack)
       saved_rbp <= current RBP  (stack is going wrong direction — corrupt)
       depth >= max_depth        (safety limit, default 64)
)";

    pause_for_user();

    step(3, "Why process_vm_readv() instead of PTRACE_PEEKDATA");
    std::cout << R"(
  Two ways to read memory from another process:

  PTRACE_PEEKDATA:
    - Reads 8 bytes per syscall
    - For a 10-frame stack you need 10 × 2 = 20 syscall round-trips
    - Each syscall = context switch = ~1-5 microseconds
    - 20 syscalls × 5μs = 100μs just to read the stack

  process_vm_readv():
    - Reads arbitrary bytes in ONE syscall
    - Specify a list of (local_buf, remote_addr, len) pairs
    - We read 16 bytes (saved_rbp + return_addr) per frame
    - Still one syscall per frame, but no PTRACE overhead
    - ~5-10x faster than PTRACE_PEEKDATA

  This matters because we do this 99 times per second.
  100μs × 99 = 9.9ms per second just on stack reading with the slow method.
  With process_vm_readv it's ~1ms per second — much better.
)";

    pause_for_user();

    step(4, "Why -fno-omit-frame-pointer is required");
    std::cout << R"(
  By default, compilers optimise away the frame pointer chain.
  They use RBP as a general-purpose register instead.

  Without -fno-omit-frame-pointer:
    cpu_math() frame:
      [RBP + 0] = some random value (used as loop counter or something)
      [RBP + 8] = also garbage
    The unwinder follows this "chain" into random memory → crash or garbage

  With -fno-omit-frame-pointer:
    The compiler adds 2 instructions at every function entry:
      push rbp       (save caller's frame pointer)
      mov rbp, rsp   (set our frame pointer to current stack top)
    This maintains the linked list of frames that we can walk.

  Production binaries often omit frame pointers (-O2 default on some toolchains).
  For those, you'd need DWARF unwinding (libunwind) which reads the .eh_frame
  section — much more complex. We use frame pointers to keep it simple.
)";

    pause_for_user();
}

// ═════════════════════════════════════════════════════════════════════════════
// Main menu
// ═════════════════════════════════════════════════════════════════════════════

int main() {
    // Clear screen
    std::cout << "\033[2J\033[H";

    std::cout << CLR_BOLD << CLR_CYAN
              << "\n  perf-profiler  —  interactive debug runner\n"
              << CLR_RESET
              << "  Watch every internal step print live in your terminal.\n"
              << "  Press ENTER at each pause to advance.\n\n"
              << CLR_DIM
              << "  ────────────────────────────────────────────────────\n"
              << CLR_RESET;

    struct TestCase { std::string label; std::function<void()> fn; };
    std::vector<TestCase> cases = {
        {"types.hpp          — Result<T> and Status error handling",  tc_types},
        {"aggregator.hpp     — how samples become collapsed stacks",  tc_aggregator},
        {"symbol_resolver    — /proc/maps + ELF .symtab lookup",      tc_resolver},
        {"report_writer.hpp  — Profile → .folded + HTML flamegraph",  tc_report_writer},
        {"Full pipeline      — end-to-end with simulated samples",    tc_full_pipeline},
        {"Stack unwinding    — frame-pointer walk explained",         tc_stack_unwinding},
    };

    while (true) {
        std::cout << "\n  Choose a test case (or 'q' to quit):\n\n";
        for (int i = 0; i < (int)cases.size(); ++i)
            std::cout << "    " << CLR_YELLOW << (i+1) << CLR_RESET
                      << "  " << cases[i].label << "\n";
        std::cout << "\n  > ";

        std::string input;
        std::getline(std::cin, input);

        if (input == "q" || input == "Q") {
            std::cout << "\n  Bye.\n\n";
            break;
        }

        int choice = 0;
        try { choice = std::stoi(input); } catch (...) {}

        if (choice < 1 || choice > (int)cases.size()) {
            std::cout << CLR_RED << "  Invalid choice.\n" << CLR_RESET;
            continue;
        }

        // Clear and run
        std::cout << "\033[2J\033[H";
        cases[choice - 1].fn();

        std::cout << CLR_BOLD << CLR_GREEN
                  << "\n  ✓  Done with test case " << choice << "\n"
                  << CLR_RESET;
    }

    return 0;
}