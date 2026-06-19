#pragma once
/*
 * stack_unwinder.hpp — Reads the call stack of a stopped process
 *
 * Linux only: uses PTRACE_GETREGS + process_vm_readv (frame-pointer walk).
 * On non-Linux platforms the class compiles as a stub so the test suite
 * (which never calls unwind() on macOS) still builds and runs.
 */

#include "types.hpp"

#include <cerrno>
#include <cstring>
#include <unistd.h>

#ifdef __linux__
#  include <sys/ptrace.h>
#  include <sys/uio.h>
#  include <sys/user.h>
#endif

namespace profiler {

class StackUnwinder {
public:
    explicit StackUnwinder(int max_depth = 64) : max_depth_(max_depth) {}

    Result<std::vector<uint64_t>> unwind(pid_t pid) const {
#ifdef __linux__
        std::vector<uint64_t> addrs;

#  if defined(__x86_64__)
        struct user_regs_struct regs{};
        if (ptrace(PTRACE_GETREGS, pid, nullptr, &regs) != 0)
            return Result<std::vector<uint64_t>>::err(
                std::string("PTRACE_GETREGS: ") + strerror(errno));
        uint64_t ip = regs.rip;
        uint64_t bp = regs.rbp;

#  elif defined(__aarch64__)
        struct user_pt_regs regs{};
        struct iovec iov { &regs, sizeof(regs) };
        if (ptrace(PTRACE_GETREGSET, pid, (void*)NT_PRSTATUS, &iov) != 0)
            return Result<std::vector<uint64_t>>::err(
                std::string("PTRACE_GETREGSET: ") + strerror(errno));
        uint64_t ip = regs.pc;
        uint64_t bp = regs.regs[29];

#  else
#    error "perf-profiler: unsupported architecture"
#  endif

        addrs.reserve(max_depth_);
        addrs.push_back(ip);

        uint64_t prev_bp = 0;
        for (int depth = 1; depth < max_depth_ && bp != 0 && bp != prev_bp; ++depth) {
            uint64_t buf[2] = {0, 0};
            struct iovec local_iov  { buf,                         sizeof(buf) };
            struct iovec remote_iov { reinterpret_cast<void*>(bp), sizeof(buf) };

            ssize_t n = process_vm_readv(pid, &local_iov, 1, &remote_iov, 1, 0);
            if (n != (ssize_t)sizeof(buf)) break;

            uint64_t saved_bp = buf[0];
            uint64_t ret_addr = buf[1];
            if (ret_addr == 0) break;

            addrs.push_back(ret_addr);
            prev_bp = bp;
            bp      = saved_bp;
            if (saved_bp != 0 && saved_bp <= prev_bp) break;
        }

        return Result<std::vector<uint64_t>>::ok_val(std::move(addrs));

#else   // !__linux__
        (void)pid;
        return Result<std::vector<uint64_t>>::err(
            "StackUnwinder: ptrace is Linux-only");
#endif
    }

    int  max_depth() const   { return max_depth_; }
    void set_max_depth(int d){ max_depth_ = d; }

private:
    int max_depth_;
};

} // namespace profiler