#pragma once
/*
 * aggregator.hpp — Converts raw Sample events into a Profile
 *
 * Converts each Sample's frame list into a semicolon-separated "collapsed
 * stack" key (Brendan Gregg stackcollapse format) and increments a hit
 * counter for that key.  At the end, Profile::counts is a deterministic
 * sorted map that can be serialised directly.
 *
 * Thread-safety: NOT thread-safe.  The sampling loop and aggregator run
 * on the same thread (callback-based), so no locking is needed.
 */

#include "types.hpp"
#include <string>

namespace profiler {

class Aggregator {
public:
    explicit Aggregator(std::string comm) : comm_(std::move(comm)) {}

    // Accept one sample (called from PerfSampler's callback).
    // frames[0] = innermost (current PC), frames.back() = outermost.
    void add(const Sample &s) {
        // Build collapsed key: comm;outermost;...;innermost
        std::string key = comm_;
        for (int i = (int)s.frames.size() - 1; i >= 0; --i) {
            key += ';';
            key += s.frames[i].symbol.empty() ? s.frames[i].binary : s.frames[i].symbol;
        }
        counts_[key]++;
        ++total_;
    }

    void set_lost(uint64_t n) { lost_ = n; }

    // Produce the final Profile.
    Profile build(pid_t pid, const Config &cfg) const {
        Profile p;
        p.counts         = counts_;
        p.total_samples  = total_;
        p.lost_samples   = lost_;
        p.target_pid     = pid;
        p.target_comm    = comm_;
        p.rate_hz        = cfg.rate_hz;
        p.duration_s     = cfg.duration_s;
        return p;
    }

    uint64_t total() const { return total_; }

private:
    std::string                  comm_;
    std::map<StackKey, uint64_t> counts_;
    uint64_t                     total_ = 0;
    uint64_t                     lost_  = 0;
};

} // namespace profiler
