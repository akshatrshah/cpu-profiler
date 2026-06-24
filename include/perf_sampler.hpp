#pragma once
/*
 * perf_sampler.hpp
 *
 * The main sampling loop. Opens a perf_event_open counter on the target,
 * discovers all its threads, and every 1/rate seconds:
 *   - interrupts each thread with PTRACE_INTERRUPT
 *   - unwinds its stack
 *   - resumes it
 *   - calls the sample callback with the result
 *
 * Rescans /proc/<pid>/task/ periodically to pick up threads that spawn
 * after we start (common in thread pools).
 *
 * The --kernel flag maps to exclude_kernel in the perf_event_attr,
 * so kernel frames only show up when explicitly requested.
 */

#include "types.hpp"
#include "stack_unwinder.hpp"
#include "symbol_resolver.hpp"
#include "thread_enumerator.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>

#ifdef __linux__
#  include <cerrno>
#  include <cstring>
#  include <fstream>
#  include <sys/ioctl.h>
#  include <sys/mman.h>
#  include <sys/ptrace.h>
#  include <sys/syscall.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
#  include <linux/perf_event.h>
#endif

namespace profiler {

using SampleCallback   = std::function<void(Sample&&)>;
using ProgressCallback = std::function<void(uint64_t /*samples*/, int64_t /*elapsed_ms*/)>;

class PerfSampler {
public:
    PerfSampler(const Config &cfg, StackUnwinder &unwinder, SymbolResolver &resolver)
        : cfg_(cfg), unwinder_(unwinder), resolver_(resolver) {}

    ~PerfSampler() { cleanup(); }

    PerfSampler(const PerfSampler&)            = delete;
    PerfSampler& operator=(const PerfSampler&) = delete;

    Status run(SampleCallback on_sample, ProgressCallback on_progress = nullptr) {
#ifdef __linux__
        // ── 1. Open perf counter on the main PID ─────────────────────────────
        perf_fd_ = open_perf_event(cfg_.pid, cfg_.include_kernel);
        if (perf_fd_ < 0)
            return Status::fail(
                std::string("perf_event_open: ") + strerror(errno) +
                "\n  → Try: echo 1 | sudo tee /proc/sys/kernel/perf_event_paranoid");

        static constexpr size_t PAGES   = 8;
        static constexpr size_t MMAP_SZ = (PAGES + 1) * 4096;
        mmap_base_ = mmap(nullptr, MMAP_SZ, PROT_READ|PROT_WRITE,
                          MAP_SHARED, perf_fd_, 0);
        if (mmap_base_ == MAP_FAILED) {
            mmap_base_ = nullptr;
            return Status::fail(std::string("mmap perf ring: ") + strerror(errno));
        }
        mmap_size_ = MMAP_SZ;

        // ── 2. Discover and attach to all threads ─────────────────────────────
        threads_ = ThreadEnumerator::seize_all(cfg_.pid);
        if (threads_.empty())
            return Status::fail("Could not attach to any thread of PID " +
                                std::to_string(cfg_.pid) +
                                "\n  → Try sudo, or: sudo setcap cap_sys_ptrace+eip ./profiler");

        int attached_count = 0;
        for (auto &t : threads_) if (t.attached) ++attached_count;
        if (cfg_.verbose)
            std::cout << "  Attached to " << attached_count
                      << " thread(s)\n";

        ioctl(perf_fd_, PERF_EVENT_IOC_RESET,  0);
        ioctl(perf_fd_, PERF_EVENT_IOC_ENABLE, 0);

        // ── 3. Sampling loop ──────────────────────────────────────────────────
        const auto interval = std::chrono::microseconds(
            static_cast<long>(1'000'000.0 / cfg_.rate_hz));
        const auto t_start = std::chrono::steady_clock::now();
        const auto t_end   = t_start + std::chrono::seconds(cfg_.duration_s);
        auto t_last_prog   = t_start;

        uint64_t n_samples = 0, n_lost = 0;
        running_ = true;

        while (running_ && std::chrono::steady_clock::now() < t_end) {
            auto t_iter = std::chrono::steady_clock::now();

            // Sample every attached thread in this iteration
            for (auto &thread : threads_) {
                if (!thread.attached) continue;

                // Interrupt this thread
                if (ptrace(PTRACE_INTERRUPT, thread.tid, nullptr, nullptr) != 0) {
                    if (errno == ESRCH) {
                        // Thread exited — mark it detached
                        thread.attached = false;
                        continue;
                    }
                    ++n_lost;
                    continue;
                }

                // Wait for the stop to land on THIS thread
                int ws = 0;
                pid_t w = waitpid(thread.tid, &ws, __WALL);
                if (w < 0 || !WIFSTOPPED(ws)) {
                    ptrace(PTRACE_CONT, thread.tid, nullptr, nullptr);
                    ++n_lost;
                    continue;
                }

                // Unwind its stack
                auto uw = unwinder_.unwind(thread.tid);
                if (uw.ok() && !uw.value.empty()) {
                    Sample s;
                    s.timestamp = std::chrono::steady_clock::now();
                    s.tid       = thread.tid;
                    for (uint64_t a : uw.value)
                        s.frames.push_back(resolver_.resolve(a));
                    on_sample(std::move(s));
                    ++n_samples;
                } else {
                    ++n_lost;
                }

                ptrace(PTRACE_CONT, thread.tid, nullptr, nullptr);
            }

            // Re-scan for new threads (handles thread pool growth mid-profile)
            maybe_rescan_threads();

            // Progress callback
            if (on_progress) {
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - t_last_prog).count() >= 1000) {
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - t_start).count();
                    on_progress(n_samples, elapsed);
                    t_last_prog = now;
                }
            }

            // Sleep for the remainder of the interval
            auto elapsed_iter = std::chrono::steady_clock::now() - t_iter;
            if (elapsed_iter < interval) {
                auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                    interval - elapsed_iter).count();
                usleep((useconds_t)us);
            }
        }

        lost_samples_    = n_lost;
        thread_count_    = (int)threads_.size();

        ioctl(perf_fd_, PERF_EVENT_IOC_DISABLE, 0);
        cleanup();

        if (n_samples == 0)
            return Status::fail("No samples collected — was the process running?");
        return Status::success();

#else
        (void)on_sample; (void)on_progress;
        return Status::fail("PerfSampler: perf_event_open is Linux-only");
#endif
    }

    void     stop()          { running_ = false; }
    uint64_t lost_samples()  const { return lost_samples_; }
    int      thread_count()  const { return thread_count_; }

private:
    const Config      &cfg_;
    StackUnwinder     &unwinder_;
    SymbolResolver    &resolver_;

    int    perf_fd_       = -1;
    void  *mmap_base_     = nullptr;
    size_t mmap_size_     = 0;
    std::vector<ThreadInfo> threads_;
    std::atomic<bool> running_{false};
    uint64_t lost_samples_ = 0;
    int      thread_count_ = 0;

    // Rescan /proc/<pid>/task/ every ~2 seconds to pick up new threads
    int rescan_counter_ = 0;
    void maybe_rescan_threads() {
#ifdef __linux__
        if (++rescan_counter_ < (cfg_.rate_hz * 2)) return;
        rescan_counter_ = 0;

        auto r = ThreadEnumerator::list_tids(cfg_.pid);
        if (!r.ok()) return;

        // Find TIDs we haven't seen yet
        for (pid_t tid : r.value) {
            bool known = false;
            for (auto &t : threads_) if (t.tid == tid) { known = true; break; }
            if (known) continue;

            ThreadInfo t;
            t.tid = tid;
            if (ptrace(PTRACE_SEIZE, tid, nullptr,
                       (void*)(uintptr_t)PTRACE_O_TRACECLONE) == 0) {
                t.attached = true;
                if (cfg_.verbose)
                    std::cout << "  [new thread] TID " << tid << "\n";
            }
            threads_.push_back(t);
        }
#endif
    }

    void cleanup() {
#ifdef __linux__
        ThreadEnumerator::detach_all(threads_);
        threads_.clear();
        if (mmap_base_ && mmap_base_ != MAP_FAILED) {
            munmap(mmap_base_, mmap_size_);
            mmap_base_ = nullptr;
        }
        if (perf_fd_ >= 0) { close(perf_fd_); perf_fd_ = -1; }
#endif
    }

#ifdef __linux__
    static int open_perf_event(pid_t pid, bool include_kernel) {
        struct perf_event_attr pea{};
        pea.type           = PERF_TYPE_SOFTWARE;
        pea.config         = PERF_COUNT_SW_CPU_CLOCK;
        pea.size           = sizeof(pea);
        pea.sample_period  = 1'000'000;
        pea.sample_type    = PERF_SAMPLE_IP | PERF_SAMPLE_TID;
        pea.disabled       = 1;
        pea.exclude_kernel = include_kernel ? 0 : 1;
        pea.exclude_hv     = 1;
        pea.wakeup_events  = 1;
        return (int)syscall(SYS_perf_event_open, &pea, pid, -1, -1, 0);
    }
#endif
};

} // namespace profiler