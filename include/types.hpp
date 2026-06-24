#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <chrono>

namespace profiler {

// one resolved frame from the target's call stack
struct Frame {
    uint64_t    address = 0;    // virtual address in target process
    std::string symbol;         // function name, or hex if we couldn't resolve
    std::string binary;         // which DSO/exe this address lives in
    uint64_t    offset  = 0;    // file offset within that binary
};

// one sample = one call stack snapshot
// frames[0] is where the CPU is right now, frames.back() is main()
struct Sample {
    std::vector<Frame> frames;
    std::chrono::steady_clock::time_point timestamp;
    pid_t       tid         = 0;
    std::string thread_name;
};

// collapsed stack key: "comm[thread];outermost;...;innermost"
// this is the format flamegraph.pl uses
using StackKey = std::string;

struct Profile {
    std::map<StackKey, uint64_t> counts;
    uint64_t    total_samples = 0;
    uint64_t    lost_samples  = 0;  // samples we missed due to ptrace latency
    pid_t       target_pid    = 0;
    std::string target_comm;
    int         rate_hz       = 0;
    int         duration_s    = 0;
    int         thread_count  = 0;
};

struct Config {
    pid_t       pid            = -1;
    int         rate_hz        = 99;
    int         duration_s     = 30;
    int         max_depth      = 64;
    std::string output_html    = "flamegraph.html";
    std::string output_folded;      // defaults to <output>.folded
    std::string output_pprof;       // empty = skip
    bool        include_kernel = false;
    bool        verbose        = false;
};

// used instead of exceptions throughout — kernel-facing code shouldn't throw
template<typename T>
struct Result {
    T           value{};
    std::string error;
    bool ok() const { return error.empty(); }

    static Result<T> ok_val(T v)          { return {std::move(v), ""}; }
    static Result<T> err(std::string msg) { return {{}, std::move(msg)}; }
};

// Result for functions that don't return a value
struct Status {
    std::string error;
    bool ok() const { return error.empty(); }
    explicit operator bool() const { return ok(); }

    static Status success()             { return {""}; }
    static Status fail(std::string msg) { return {std::move(msg)}; }
};

} // namespace profiler