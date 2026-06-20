#pragma once
/*
 * perf_sampler.hpp — Drives the sample-interrupt-unwind-resume loop
 *
 * Linux only: uses perf_event_open(2) + PTRACE_SEIZE/INTERRUPT.
 * On non-Linux platforms this file provides the type aliases used by main.cpp
 * but the PerfSampler class is a compile-time stub that always returns an error,
 * so the project compiles cleanly for unit testing on macOS.
 */

#include "types.hpp"
#include "stack_unwinder.hpp"
#include "symbol_resolver.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>

#ifdef __linux__
#  include <cerrno>
#  include <csignal>
#  include <cstring>
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
using ProgressCallback = std::function<void(uint64_t, int64_t)>;

class PerfSampler {
public:
    PerfSampler(const Config &cfg, StackUnwinder &unwinder, SymbolResolver &resolver)
        : cfg_(cfg), unwinder_(unwinder), resolver_(resolver) {}

    ~PerfSampler() {
#ifdef __linux__
        cleanup();
#endif
    }

    PerfSampler(const PerfSampler&)            = delete;
    PerfSampler& operator=(const PerfSampler&) = delete;

    Status run(SampleCallback on_sample, ProgressCallback on_progress = nullptr) {
#ifdef __linux__
        // ── 1. Open perf counter ─────────────────────────────────────────────
        perf_fd_ = open_perf_event(cfg_.pid);
        if (perf_fd_ < 0)
            return Status::fail(std::string("perf_event_open: ") + strerror(errno) +
                "\n  → Try: echo 1 | sudo tee /proc/sys/kernel/perf_event_paranoid");

        static constexpr size_t PAGES   = 8;
        static constexpr size_t MMAP_SZ = (PAGES + 1) * 4096;
        mmap_base_ = mmap(nullptr, MMAP_SZ, PROT_READ|PROT_WRITE, MAP_SHARED, perf_fd_, 0);
        if (mmap_base_ == MAP_FAILED) {
            mmap_base_ = nullptr;
            return Status::fail(std::string("mmap perf ring: ") + strerror(errno));
        }
        mmap_size_ = MMAP_SZ;

        // ── 2. Attach ────────────────────────────────────────────────────────
        if (ptrace(PTRACE_SEIZE, cfg_.pid, nullptr,
                   (void*)(uintptr_t)PTRACE_O_TRACECLONE) != 0)
            return Status::fail(std::string("ptrace SEIZE: ") + strerror(errno) +
                "\n  → Try sudo, or: sudo setcap cap_sys_ptrace+eip ./profiler");
        attached_ = true;

        ioctl(perf_fd_, PERF_EVENT_IOC_RESET,  0);
        ioctl(perf_fd_, PERF_EVENT_IOC_ENABLE, 0);

        // ── 3. Sampling loop ─────────────────────────────────────────────────
        const auto interval = std::chrono::microseconds(
            static_cast<long>(1'000'000.0 / cfg_.rate_hz));
        const auto t_start = std::chrono::steady_clock::now();
        const auto t_end   = t_start + std::chrono::seconds(cfg_.duration_s);
        auto t_last_prog   = t_start;

        uint64_t n_samples = 0, n_lost = 0;
        running_ = true;

        while (running_ && std::chrono::steady_clock::now() < t_end) {
            auto t_iter = std::chrono::steady_clock::now();

            if (ptrace(PTRACE_INTERRUPT, cfg_.pid, nullptr, nullptr) != 0) {
                if (errno == ESRCH) break;
                ++n_lost; goto sleep_step;
            }
            {
                int ws = 0;
                pid_t w = waitpid(cfg_.pid, &ws, __WALL);
                if (w < 0 || !WIFSTOPPED(ws)) {
                    ptrace(PTRACE_CONT, cfg_.pid, nullptr, nullptr);
                    ++n_lost; goto sleep_step;
                }

                auto uw = unwinder_.unwind(cfg_.pid);
                if (uw.ok() && !uw.value.empty()) {
                    Sample s;
                    s.timestamp = std::chrono::steady_clock::now();
                    s.tid       = cfg_.pid;
                    for (uint64_t a : uw.value)
                        s.frames.push_back(resolver_.resolve(a));
                    on_sample(std::move(s));
                    ++n_samples;
                } else {
                    ++n_lost;
                }
                ptrace(PTRACE_CONT, cfg_.pid, nullptr, nullptr);
            }

sleep_step:
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

            auto elapsed = std::chrono::steady_clock::now() - t_iter;
            if (elapsed < interval) {
                auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                    interval - elapsed).count();
                usleep((useconds_t)us);
            }
        }

        lost_samples_ = n_lost;
        ioctl(perf_fd_, PERF_EVENT_IOC_DISABLE, 0);
        cleanup();

        if (n_samples == 0)
            return Status::fail("No samples collected — was the process running?");
        return Status::success();

#else  // !__linux__
        (void)on_sample; (void)on_progress;
        return Status::fail("PerfSampler: perf_event_open is Linux-only");
#endif
    }

    void     stop()          { running_ = false; }
    uint64_t lost_samples()  const { return lost_samples_; }

private:
    const Config      &cfg_;
    StackUnwinder     &unwinder_;
    SymbolResolver    &resolver_;

    int    perf_fd_       = -1;
    void  *mmap_base_     = nullptr;
    size_t mmap_size_     = 0;
    bool   attached_      = false;
    std::atomic<bool> running_{false};
    uint64_t lost_samples_ = 0;

    void cleanup() {
#ifdef __linux__
        if (attached_) { ptrace(PTRACE_DETACH, cfg_.pid, nullptr, nullptr); attached_ = false; }
        if (mmap_base_ && mmap_base_ != MAP_FAILED) { munmap(mmap_base_, mmap_size_); mmap_base_ = nullptr; }
        if (perf_fd_ >= 0) { close(perf_fd_); perf_fd_ = -1; }
#endif
    }

#ifdef __linux__
    static int open_perf_event(pid_t pid) {
        struct perf_event_attr pea{};
        pea.type           = PERF_TYPE_SOFTWARE;
        pea.config         = PERF_COUNT_SW_CPU_CLOCK;
        pea.size           = sizeof(pea);
        pea.sample_period  = 1'000'000;
        pea.sample_type    = PERF_SAMPLE_IP | PERF_SAMPLE_TID;
        pea.disabled       = 1;
        pea.exclude_kernel = 1;
        pea.exclude_hv     = 1;
        pea.wakeup_events  = 1;
        return (int)syscall(SYS_perf_event_open, &pea, pid, -1, -1, 0);
    }
#endif
};

} // namespace profiler