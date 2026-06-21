/*
 * test_target.cpp — Deterministic CPU-burn target for profiler validation
 *
 * Call graph (approximate CPU share):
 *
 *   main()
 *     └── workload_loop()          [orchestrates the three workloads]
 *           ├── cpu_math()         ~55% — floating-point / sqrt / sin
 *           ├── cpu_fibonacci()    ~35% — tight integer loop
 *           └── cpu_memory()       ~10% — cache-pressure loop
 *
 * Compile (frame pointers are REQUIRED for the stack unwinder):
 *   g++ -std=c++17 -O1 -fno-omit-frame-pointer -g -o test_target test_target.cpp
 *
 * Run:
 *   ./test_target [seconds]     # default = run forever until SIGINT
 *
 * The PID is printed to stdout on startup for scripting.
 */

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <unistd.h>

static volatile bool g_running = true;
static void on_sig(int) { g_running = false; }

// ── Workloads ─────────────────────────────────────────────────────────────────

// ~55% of CPU time
static double __attribute__((noinline)) cpu_math(long n) {
    double acc = 1.0;
    for (long i = 1; i <= n; ++i)
        acc += std::sqrt((double)i) * std::sin(acc * 0.00001);
    return acc;
}

// ~35% of CPU time
static long __attribute__((noinline)) cpu_fibonacci(int n) {
    long a = 0, b = 1;
    for (int i = 2; i <= n; ++i) { long t = a + b; a = b; b = t; }
    return b;
}

// ~10% of CPU time
static long __attribute__((noinline)) cpu_memory(int n) {
    volatile long x = 0;
    for (int i = 0; i < n; ++i) x += i ^ (i >> 3);
    return x;
}

// Thin wrappers to give the profiler distinct symbol names
static double __attribute__((noinline)) level2_math()   { return cpu_math(1'500'000L); }
static long   __attribute__((noinline)) level2_fib()    { return cpu_fibonacci(45); }
static long   __attribute__((noinline)) level2_mem()    { return cpu_memory(400'000); }

static void __attribute__((noinline)) workload_loop() {
    while (g_running) {
        (void)level2_math();
        (void)level2_fib();
        (void)level2_mem();
    }
}

// ── main ──────────────────────────────────────────────────────────────────────
int main(int argc, char **argv) {
    signal(SIGINT,  on_sig);
    signal(SIGTERM, on_sig);

    int secs = 0;
    if (argc > 1) secs = std::atoi(argv[1]);

    std::printf("test_target PID=%d\n", (int)getpid());
    std::fflush(stdout);

    if (secs > 0) {
        alarm((unsigned)secs);
        signal(SIGALRM, on_sig);
    }

    workload_loop();
    return 0;
}
