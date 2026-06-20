/*
 * main.cpp — perf-profiler entry point
 *
 * Wires together:
 *   Config → SymbolResolver → StackUnwinder → PerfSampler → Aggregator → ReportWriter
 *
 * All heavy logic lives in the headers; main() is just orchestration + CLI.
 */

#include "types.hpp"
#include "symbol_resolver.hpp"
#include "stack_unwinder.hpp"
#include "perf_sampler.hpp"
#include "aggregator.hpp"
#include "report_writer.hpp"

#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

// ── Global sampler pointer so SIGINT can call stop() cleanly ─────────────────
static profiler::PerfSampler *g_sampler = nullptr;
static void on_signal(int) { if (g_sampler) g_sampler->stop(); }

// ── CLI helpers ───────────────────────────────────────────────────────────────
static void print_usage(const char *prog) {
    std::cout <<
R"(Usage: )" << prog << R"( --pid <PID> [OPTIONS]

Attach to a running process and sample its CPU call stacks.

Required:
  --pid,      -p <PID>       Target process PID

Sampling options:
  --rate,     -r <Hz>        Samples per second          (default: 99)
  --duration, -d <seconds>   How long to profile         (default: 30)
  --depth,    -D <frames>    Max call stack depth        (default: 64)

Output options:
  --output,   -o <file>      HTML flamegraph path        (default: flamegraph.html)
  --folded,   -f <file>      Also write folded stacks    (default: <output>.folded)
  --verbose,  -v             Print progress every second

Diagnostics:
  --help,     -h             Show this message
  --version                  Print version and exit

Examples:
  sudo ./profiler --pid 1234 --rate 99 --duration 30
  ./profiler -p $(pgrep my_server) -r 200 -d 60 -o report.html
  ./profiler -p 5678 --verbose

Troubleshooting:
  perf_event_open fails  →  echo 1 | sudo tee /proc/sys/kernel/perf_event_paranoid
  ptrace fails           →  run with sudo, or: sudo setcap cap_sys_ptrace+eip ./profiler
  Flat flamegraph        →  recompile target with -fno-omit-frame-pointer
)";
}

static void print_version() {
    std::cout << "perf-profiler 1.0.0\n"
              << "Built " << __DATE__ << " " << __TIME__ << "\n"
              << "Kernel: Linux (perf_event_open + ptrace)\n";
}

// ── Validate /proc/<pid>/status exists ────────────────────────────────────────
static bool pid_exists(pid_t pid) {
    std::ifstream f("/proc/" + std::to_string(pid) + "/status");
    return f.good();
}

// ── Read /proc/<pid>/comm ─────────────────────────────────────────────────────
static std::string read_comm(pid_t pid) {
    std::ifstream f("/proc/" + std::to_string(pid) + "/comm");
    std::string s;
    std::getline(f, s);
    return s.empty() ? std::to_string(pid) : s;
}

// ── main ──────────────────────────────────────────────────────────────────────
int main(int argc, char **argv) {
    profiler::Config cfg;
    bool explicit_folded = false;

    // ── Argument parsing ────────────────────────────────────────────────────
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need_arg = [&](const std::string &flag) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "Error: " << flag << " requires an argument\n";
                std::exit(1);
            }
            return argv[++i];
        };

        if      (a == "--pid"      || a == "-p") cfg.pid        = std::stoi(need_arg(a));
        else if (a == "--rate"     || a == "-r") cfg.rate_hz    = std::stoi(need_arg(a));
        else if (a == "--duration" || a == "-d") cfg.duration_s = std::stoi(need_arg(a));
        else if (a == "--depth"    || a == "-D") cfg.max_depth  = std::stoi(need_arg(a));
        else if (a == "--output"   || a == "-o") cfg.output_html= need_arg(a);
        else if (a == "--folded"   || a == "-f") { cfg.output_folded = need_arg(a); explicit_folded = true; }
        else if (a == "--verbose"  || a == "-v") cfg.verbose    = true;
        else if (a == "--help"     || a == "-h") { print_usage(argv[0]); return 0; }
        else if (a == "--version")               { print_version();      return 0; }
        else { std::cerr << "Unknown option: " << a << "  (try --help)\n"; return 1; }
    }

    // ── Validate ────────────────────────────────────────────────────────────
    if (cfg.pid <= 0) {
        std::cerr << "Error: --pid is required\n\n";
        print_usage(argv[0]);
        return 1;
    }
    if (!pid_exists(cfg.pid)) {
        std::cerr << "Error: PID " << cfg.pid << " does not exist\n";
        return 1;
    }
    if (cfg.rate_hz < 1 || cfg.rate_hz > 10000) {
        std::cerr << "Error: --rate must be 1..10000\n"; return 1;
    }
    if (cfg.duration_s < 1 || cfg.duration_s > 3600) {
        std::cerr << "Error: --duration must be 1..3600\n"; return 1;
    }
    if (!explicit_folded)
        cfg.output_folded = cfg.output_html + ".folded";

    // ── Banner ──────────────────────────────────────────────────────────────
    std::string comm = read_comm(cfg.pid);
    std::cout << "\n"
              << "  perf-profiler\n"
              << "  ─────────────────────────────────────────\n"
              << "  Target  : " << comm << "  (PID " << cfg.pid << ")\n"
              << "  Rate    : " << cfg.rate_hz    << " Hz\n"
              << "  Duration: " << cfg.duration_s << " s\n"
              << "  Output  : " << cfg.output_html << "\n"
              << "  ─────────────────────────────────────────\n\n";

    // ── Wire up components ──────────────────────────────────────────────────
    profiler::SymbolResolver resolver(cfg.pid);
    profiler::StackUnwinder  unwinder(cfg.max_depth);
    profiler::Aggregator     aggregator(comm);
    profiler::PerfSampler    sampler(cfg, unwinder, resolver);

    g_sampler = &sampler;
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    // ── Run ─────────────────────────────────────────────────────────────────
    auto t_start = std::chrono::steady_clock::now();

    auto progress = cfg.verbose
        ? profiler::ProgressCallback([&](uint64_t n, int64_t ms) {
              std::cout << "  [" << std::setw(4) << ms/1000 << "s]  "
                        << n << " samples\n";
          })
        : profiler::ProgressCallback([&](uint64_t n, int64_t ms) {
              // Minimal progress bar even in non-verbose mode
              int pct = (int)(100LL * ms / (cfg.duration_s * 1000LL));
              pct = std::min(pct, 100);
              std::cout << "\r  Profiling... " << std::setw(3) << pct << "%  "
                        << n << " samples  " << std::flush;
          });

    auto status = sampler.run(
        [&](profiler::Sample &&s) { aggregator.add(s); },
        progress
    );

    std::cout << "\n";

    if (!status) {
        std::cerr << "Error: " << status.error << "\n";
        return 1;
    }

    auto t_end = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        t_end - t_start).count();

    // ── Aggregate & report ──────────────────────────────────────────────────
    aggregator.set_lost(sampler.lost_samples());
    profiler::Profile profile = aggregator.build(cfg.pid, cfg);

    std::cout << "\n"
              << "  ─────────────────────────────────────────\n"
              << "  Collected : " << profile.total_samples << " samples\n"
              << "  Lost      : " << profile.lost_samples  << " samples\n"
              << "  Elapsed   : " << elapsed_ms            << " ms\n"
              << "  Unique stacks: " << profile.counts.size() << "\n"
              << "  ─────────────────────────────────────────\n\n";

    // Write folded stacks
    {
        auto s = profiler::ReportWriter::write_folded(cfg.output_folded, profile);
        if (!s) std::cerr << "Warning: " << s.error << "\n";
        else    std::cout << "  Folded stacks → " << cfg.output_folded << "\n";
    }

    // Write HTML flamegraph
    {
        auto s = profiler::ReportWriter::write_html(cfg.output_html, profile);
        if (!s) { std::cerr << "Error: " << s.error << "\n"; return 1; }
        std::cout << "  Flamegraph    → " << cfg.output_html << "\n\n";
    }

    std::cout << "  Open in browser:  xdg-open " << cfg.output_html << "\n\n";

    return 0;
}
