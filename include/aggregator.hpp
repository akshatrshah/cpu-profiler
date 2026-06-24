#pragma once

#include "types.hpp"
#include <string>
#include <set>

namespace profiler {

// Takes raw samples and accumulates hit counts per unique call path.
//
// Key format: "comm[thread];outermost_frame;...;innermost_frame"
// Thread name is included only when it differs from the process name,
// so single-threaded profiles stay clean.
class Aggregator {
public:
    explicit Aggregator(std::string comm) : comm_(std::move(comm)) {}

    void add(const Sample &s) {
        std::string key = comm_;

        if (!s.thread_name.empty() && s.thread_name != comm_) {
            key += '[';
            key += s.thread_name;
            key += ']';
        }

        if (!s.thread_name.empty())
            seen_threads_.insert(s.thread_name);

        // outermost frame first — that's what flamegraph.pl expects
        for (int i = (int)s.frames.size() - 1; i >= 0; --i) {
            key += ';';
            const auto &f = s.frames[i];
            key += f.symbol.empty() ? f.binary : f.symbol;
        }

        counts_[key]++;
        ++total_;
    }

    void set_lost(uint64_t n) { lost_ = n; }

    Profile build(pid_t pid, const Config &cfg, int thread_count = 0) const {
        Profile p;
        p.counts        = counts_;
        p.total_samples = total_;
        p.lost_samples  = lost_;
        p.target_pid    = pid;
        p.target_comm   = comm_;
        p.rate_hz       = cfg.rate_hz;
        p.duration_s    = cfg.duration_s;
        p.thread_count  = thread_count > 0
                            ? thread_count
                            : (int)seen_threads_.size();
        return p;
    }

    uint64_t total() const { return total_; }

private:
    std::string                  comm_;
    std::map<StackKey, uint64_t> counts_;
    std::set<std::string>        seen_threads_;
    uint64_t                     total_ = 0;
    uint64_t                     lost_  = 0;
};

} // namespace profiler