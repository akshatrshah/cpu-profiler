/*
 * main.cpp — perf-profiler entry point
 *
 * Wires together:
 *   Config → SymbolResolver → StackUnwinder → PerfSampler
 *          → Aggregator → ReportWriter
 */

#include "types.hpp"
#include "symbol_resolver.hpp"
#include "stack_unwinder.hpp"
#include "thread_enumerator.hpp"
#include "perf_sampler.hpp"
#include "aggregator.hpp"
#include "report_writer.hpp"

#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <unistd.h>

static profiler::PerfSampler *g_sampler = nullptr;
static void on_signal(int) { if (g_sampler) g_sampler->stop(); }

static void print_usage(const char *prog) {
    std::cout <<
"Usage: " << prog << " --pid <PID> [OPTIONS]\n"
"\n"
"Attach to a running process and sample its CPU call stacks.\n"
"\n"
"Required:\n"
"  --pid,      -p <PID>       Target process PID\n"
"\n"
"Sampling:\n"
"  --rate,     -r <Hz>        Samples per second          (default: 99)\n"
"  --duration, -d <seconds>   How long to profile         (default: 30)\n"
"  --depth,    -D <frames>    Max call stack depth        (default: 64)\n"
"  --kernel,   -k             Include kernel frames       (default: off)\n"
"\n"
"Output:\n"
"  --output,   -o <file>      HTML flamegraph path        (default: flamegraph.html)\n"
"  --folded,   -f <file>      Folded stacks path          (default: <output>.folded)\n"
"  --pprof,    -P <file>      pprof text format path      (default: skip)\n"
"  --verbose,  -v             Print progress every second\n"
"\n"
"Other:\n"
"  --help,     -h             Show this message\n"
"  --version                  Print version and exit\n"
"\n"
"Examples:\n"
"  sudo ./profiler --pid 1234 --rate 99 --duration 30\n"
"  sudo ./profiler --pid 1234 --kernel --pprof profile.pprof.txt\n"
"  ./profiler -p $(pgrep nginx) -r 49 -d 60 --verbose\n"
"\n"
"Unwind method: " << profiler::unwind_method() << "\n"
"\n"
"Troubleshooting:\n"
"  perf_event_open fails  →  echo 1 | sudo tee /proc/sys/kernel/perf_event_paranoid\n"
"  ptrace fails           →  run with sudo, or: sudo setcap cap_sys_ptrace+eip ./profiler\n"
"  Flat flamegraph        →  recompile target with -fno-omit-frame-pointer\n"
"                            or rebuild profiler with: make USE_LIBUNWIND=1\n";
}

static void print_version() {
    std::cout << "perf-profiler 2.0.0\n"
              << "Built " << __DATE__ << " " << __TIME__ << "\n"
              << "Unwind: " << profiler::unwind_method() << "\n";
}

static bool pid_exists(pid_t pid) {
    std::ifstream f("/proc/" + std::to_string(pid) + "/status");
    return f.good();
}

static std::string read_comm(pid_t pid) {
    std::ifstream f("/proc/" + std::to_string(pid) + "/comm");
    std::string s;
    std::getline(f, s);
    return s.empty() ? std::to_string(pid) : s;
}

int main(int argc, char **argv) {
    profiler::Config cfg;
    bool explicit_folded = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const std::string &flag) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "Error: " << flag << " requires an argument\n";
                std::exit(1);
            }
            return argv[++i];
        };

        if      (a == "--pid"      || a == "-p") cfg.pid            = std::stoi(need(a));
        else if (a == "--rate"     || a == "-r") cfg.rate_hz        = std::stoi(need(a));
        else if (a == "--duration" || a == "-d") cfg.duration_s     = std::stoi(need(a));
        else if (a == "--depth"    || a == "-D") cfg.max_depth      = std::stoi(need(a));
        else if (a == "--output"   || a == "-o") cfg.output_html    = need(a);
        else if (a == "--folded"   || a == "-f") { cfg.output_folded = need(a); explicit_folded = true; }
        else if (a == "--pprof"    || a == "-P") cfg.output_pprof   = need(a);
        else if (a == "--kernel"   || a == "-k") cfg.include_kernel = true;
        else if (a == "--verbose"  || a == "-v") cfg.verbose        = true;
        else if (a == "--help"     || a == "-h") { print_usage(argv[0]); return 0; }
        else if (a == "--version")               { print_version();       return 0; }
        else { std::cerr << "Unknown option: " << a << "  (try --help)\n"; return 1; }
    }

    if (cfg.pid <= 0) {
        std::cerr << "Error: --pid is required\n\n";
        print_usage(argv[0]);
        return 1;
    }
    if (!pid_exists(cfg.pid)) {
        std::cerr << "Error: PID " << cfg.pid << " does not exist\n"; return 1;
    }
    if (cfg.rate_hz < 1 || cfg.rate_hz > 10000) {
        std::cerr << "Error: --rate must be 1..10000\n"; return 1;
    }
    if (cfg.duration_s < 1 || cfg.duration_s > 3600) {
        std::cerr << "Error: --duration must be 1..3600\n"; return 1;
    }
    if (!explicit_folded)
        cfg.output_folded = cfg.output_html + ".folded";

    std::string comm = read_comm(cfg.pid);

    // Count threads upfront so we can show it in the banner
    int n_threads = 1;
#ifdef __linux__
    auto tids = profiler::ThreadEnumerator::list_tids(cfg.pid);
    if (tids.ok()) n_threads = (int)tids.value.size();
#endif

    std::cout << "\n"
              << "  perf-profiler v2\n"
              << "  ─────────────────────────────────────────\n"
              << "  Target   : " << comm << "  (PID " << cfg.pid << ")\n"
              << "  Threads  : " << n_threads << "\n"
              << "  Rate     : " << cfg.rate_hz    << " Hz\n"
              << "  Duration : " << cfg.duration_s << " s\n"
              << "  Unwind   : " << profiler::unwind_method() << "\n"
              << "  Kernel   : " << (cfg.include_kernel ? "yes" : "no") << "\n"
              << "  Output   : " << cfg.output_html << "\n"
              << "  ─────────────────────────────────────────\n\n";

    profiler::SymbolResolver resolver(cfg.pid);
    profiler::StackUnwinder  unwinder(cfg.max_depth);
    profiler::Aggregator     aggregator(comm);
    profiler::PerfSampler    sampler(cfg, unwinder, resolver);

    g_sampler = &sampler;
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    auto t_start = std::chrono::steady_clock::now();

    auto progress = profiler::ProgressCallback(
        [&](uint64_t n, int64_t ms) {
            if (cfg.verbose) {
                std::cout << "  [" << std::setw(4) << ms/1000 << "s]  "
                          << n << " samples\n";
            } else {
                int pct = std::min((int)(100LL * ms / (cfg.duration_s * 1000LL)), 100);
                std::cout << "\r  Profiling... " << std::setw(3) << pct << "%  "
                          << n << " samples  " << std::flush;
            }
        });

    // Wire thread name into each sample via the aggregator
    auto status = sampler.run(
        [&](profiler::Sample &&s) {
#ifdef __linux__
            s.thread_name = profiler::ThreadEnumerator::thread_name(cfg.pid, s.tid);
#endif
            aggregator.add(s);
        },
        progress
    );

    std::cout << "\n";
    if (!status) { std::cerr << "Error: " << status.error << "\n"; return 1; }

    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t_start).count();

    aggregator.set_lost(sampler.lost_samples());
    profiler::Profile profile = aggregator.build(cfg.pid, cfg, sampler.thread_count());

    std::cout << "\n"
              << "  ─────────────────────────────────────────\n"
              << "  Collected    : " << profile.total_samples << " samples\n"
              << "  Lost         : " << profile.lost_samples  << " samples\n"
              << "  Threads seen : " << profile.thread_count  << "\n"
              << "  Elapsed      : " << elapsed_ms            << " ms\n"
              << "  Unique stacks: " << profile.counts.size() << "\n"
              << "  ─────────────────────────────────────────\n\n";

    // Write folded stacks
    {
        auto s = profiler::ReportWriter::write_folded(cfg.output_folded, profile);
        if (!s) std::cerr << "Warning: " << s.error << "\n";
        else    std::cout << "  Folded stacks  → " << cfg.output_folded << "\n";
    }

    // Write pprof (optional)
    if (!cfg.output_pprof.empty()) {
        auto s = profiler::ReportWriter::write_pprof(cfg.output_pprof, profile);
        if (!s) std::cerr << "Warning: " << s.error << "\n";
        else    std::cout << "  pprof profile  → " << cfg.output_pprof << "\n";
    }

    // Write HTML flamegraph
    {
        auto s = profiler::ReportWriter::write_html(cfg.output_html, profile);
        if (!s) { std::cerr << "Error: " << s.error << "\n"; return 1; }
        std::cout << "  Flamegraph     → " << cfg.output_html << "\n\n";
    }

    std::cout << "  Open: xdg-open " << cfg.output_html << "\n\n";
    return 0;
}