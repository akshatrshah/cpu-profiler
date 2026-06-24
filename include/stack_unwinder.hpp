#pragma once

#include "types.hpp"

#include <cerrno>
#include <cstring>
#include <unistd.h>

#ifdef __linux__
#  include <sys/ptrace.h>
#  include <sys/uio.h>
#  include <sys/user.h>
#  ifdef USE_LIBUNWIND
#    define UNW_LOCAL_ONLY
#    include <libunwind-ptrace.h>
#  endif
#endif

namespace profiler {

inline const char* unwind_method() {
#ifdef USE_LIBUNWIND
    return "DWARF (libunwind-ptrace)";
#else
    return "frame-pointer";
#endif
}

// Unwinds the call stack of a ptrace-stopped process.
//
// Two strategies:
//   default       — frame pointer walk. Fast, zero deps, but requires the target
//                   was compiled with -fno-omit-frame-pointer.
//   USE_LIBUNWIND — reads .eh_frame via libunwind-ptrace. Works on any binary
//                   including fully stripped ones. Needs: apt install libunwind-dev
//                   Build with: make USE_LIBUNWIND=1
class StackUnwinder {
public:
    explicit StackUnwinder(int max_depth = 64) : max_depth_(max_depth) {}

    Result<std::vector<uint64_t>> unwind(pid_t pid) const {
#ifdef __linux__
#  ifdef USE_LIBUNWIND
        return unwind_dwarf(pid);
#  else
        return unwind_fp(pid);
#  endif
#else
        (void)pid;
        return Result<std::vector<uint64_t>>::err("ptrace is Linux-only");
#endif
    }

    int  max_depth() const    { return max_depth_; }
    void set_max_depth(int d) { max_depth_ = d; }

private:
    int max_depth_;

#ifdef __linux__

#  ifdef USE_LIBUNWIND
    Result<std::vector<uint64_t>> unwind_dwarf(pid_t pid) const {
        std::vector<uint64_t> addrs;

        unw_addr_space_t as = unw_create_addr_space(&_UPT_accessors, __BYTE_ORDER__);
        if (!as)
            return Result<std::vector<uint64_t>>::err("unw_create_addr_space failed");

        void *ui = _UPT_create(pid);
        if (!ui) {
            unw_destroy_addr_space(as);
            return Result<std::vector<uint64_t>>::err("_UPT_create failed");
        }

        unw_cursor_t cursor;
        if (unw_init_remote(&cursor, as, ui) != 0) {
            _UPT_destroy(ui);
            unw_destroy_addr_space(as);
            return Result<std::vector<uint64_t>>::err("unw_init_remote failed");
        }

        addrs.reserve(max_depth_);
        do {
            unw_word_t ip = 0;
            unw_get_reg(&cursor, UNW_REG_IP, &ip);
            if (ip == 0) break;
            addrs.push_back((uint64_t)ip);
            if ((int)addrs.size() >= max_depth_) break;
        } while (unw_step(&cursor) > 0);

        _UPT_destroy(ui);
        unw_destroy_addr_space(as);

        if (addrs.empty())
            return Result<std::vector<uint64_t>>::err("DWARF unwind: no frames");
        return Result<std::vector<uint64_t>>::ok_val(std::move(addrs));
    }
#  endif

    Result<std::vector<uint64_t>> unwind_fp(pid_t pid) const {
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
#    error "unsupported architecture"
#  endif

        addrs.reserve(max_depth_);
        addrs.push_back(ip);

        uint64_t prev_bp = 0;
        for (int depth = 1; depth < max_depth_ && bp != 0 && bp != prev_bp; ++depth) {
            // [bp+0] = saved frame pointer, [bp+8] = return address
            uint64_t buf[2] = {0, 0};
            struct iovec local_iov  { buf,                          sizeof(buf) };
            struct iovec remote_iov { reinterpret_cast<void*>(bp),  sizeof(buf) };

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
    }

#endif
};

} // namespace profiler