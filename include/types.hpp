#pragma once
/*
 * types.hpp — Shared data structures for perf-profiler
 *
 * Keeps all cross-module types in one place so every translation unit agrees
 * on the same layout.  No system headers leak out of here — only stdint /
 * stddef / string / vector.
 */

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <chrono>

namespace profiler {

// ---------------------------------------------------------------------------
// A single resolved stack frame
// ---------------------------------------------------------------------------
struct Frame {
    uint64_t    address  = 0;       // raw virtual address in the target process
    std::string symbol;             // e.g. "heavy_computation" or "0x4012ab"
    std::string binary;             // basename of the mapped DSO / exe
    uint64_t    offset   = 0;       // address - mapping_start + file_offset
};

// ---------------------------------------------------------------------------
// One sample: a snapshot of the call stack at a point in time
// ---------------------------------------------------------------------------
struct Sample {
    std::vector<Frame> frames;  // [0] = innermost (current PC), last = outermost
    std::chrono::steady_clock::time_point timestamp;
    pid_t       tid         = 0;
    std::string thread_name;    // from /proc/<pid>/task/<tid>/comm
};

// ---------------------------------------------------------------------------
// Aggregated profile: collapsed stacks → hit counts
// ---------------------------------------------------------------------------
//   Key format (Brendan Gregg stackcollapse):
//     "comm[thread_name];outer_frame;...;inner_frame"
using StackKey = std::string;

struct Profile {
    std::map<StackKey, uint64_t> counts;  // sorted so output is deterministic
    uint64_t total_samples = 0;
    uint64_t lost_samples  = 0;           // ptrace failures / process-gone events
    pid_t    target_pid    = 0;
    std::string target_comm;
    int      rate_hz       = 0;
    int      duration_s    = 0;
    int      thread_count  = 0;           // number of threads profiled
};

// ---------------------------------------------------------------------------
// Configuration (populated by CLI, passed into the engine)
// ---------------------------------------------------------------------------
struct Config {
    pid_t       pid            = -1;
    int         rate_hz        = 99;
    int         duration_s     = 30;
    int         max_depth      = 64;
    std::string output_html    = "flamegraph.html";
    std::string output_folded;           // empty = auto (<html>.folded)
    std::string output_pprof;            // empty = skip pprof output
    bool        include_kernel = false;  // include kernel frames
    bool        verbose        = false;
};

// ---------------------------------------------------------------------------
// Return type used throughout (avoids exceptions in kernel-facing code)
// ---------------------------------------------------------------------------
template<typename T>
struct Result {
    T           value{};
    std::string error;            // non-empty means failure
    bool ok() const { return error.empty(); }

    static Result<T> ok_val(T v)              { return {std::move(v), ""}; }
    static Result<T> err(std::string msg)     { return {{}, std::move(msg)}; }
};

// Specialisation for void-like results
struct Status {
    std::string error;
    bool ok()  const { return error.empty(); }
    explicit operator bool() const { return ok(); }

    static Status success()               { return {""}; }
    static Status fail(std::string msg)   { return {std::move(msg)}; }
};

} // namespace profiler
