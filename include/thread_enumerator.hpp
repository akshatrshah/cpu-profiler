#pragma once

#include "types.hpp"

#include <fstream>
#include <string>
#include <vector>

#ifdef __linux__
#  include <dirent.h>
#  include <cstring>
#  include <sys/ptrace.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#endif

namespace profiler {

struct ThreadInfo {
    pid_t tid      = 0;
    bool  attached = false;
};

// Linux exposes all threads of a process under /proc/<pid>/task/<tid>/
// We enumerate those, seize each one, and sample them all in the main loop.
//
// Without this you'd miss CPU work happening on non-main threads entirely.
// A server with 8 threads each spending 12% in a hot function looks fine
// per-thread but is actually spending 96% of CPU there.
class ThreadEnumerator {
public:
    static Result<std::vector<pid_t>> list_tids(pid_t pid) {
#ifdef __linux__
        std::string task_dir = "/proc/" + std::to_string(pid) + "/task";
        DIR *d = opendir(task_dir.c_str());
        if (!d)
            return Result<std::vector<pid_t>>::err(
                "opendir " + task_dir + ": " + strerror(errno));

        std::vector<pid_t> tids;
        struct dirent *ent;
        while ((ent = readdir(d)) != nullptr) {
            if (ent->d_name[0] == '.') continue;
            bool numeric = true;
            for (char *p = ent->d_name; *p; ++p)
                if (!isdigit((unsigned char)*p)) { numeric = false; break; }
            if (!numeric) continue;
            tids.push_back((pid_t)std::stoi(ent->d_name));
        }
        closedir(d);

        if (tids.empty())
            return Result<std::vector<pid_t>>::err(
                "no threads found for PID " + std::to_string(pid));
        return Result<std::vector<pid_t>>::ok_val(std::move(tids));
#else
        (void)pid;
        return Result<std::vector<pid_t>>::err("Linux-only");
#endif
    }

    static std::vector<ThreadInfo> seize_all(pid_t pid) {
        std::vector<ThreadInfo> threads;
#ifdef __linux__
        auto r = list_tids(pid);
        if (!r.ok()) return threads;

        for (pid_t tid : r.value) {
            ThreadInfo t;
            t.tid = tid;
            if (ptrace(PTRACE_SEIZE, tid, nullptr,
                       (void*)(uintptr_t)PTRACE_O_TRACECLONE) == 0)
                t.attached = true;
            threads.push_back(t);
        }
#endif
        return threads;
    }

    static void detach_all(const std::vector<ThreadInfo> &threads) {
#ifdef __linux__
        for (auto &t : threads)
            if (t.attached)
                ptrace(PTRACE_DETACH, t.tid, nullptr, nullptr);
#else
        (void)threads;
#endif
    }

    static std::string thread_name(pid_t pid, pid_t tid) {
        std::ifstream f("/proc/" + std::to_string(pid) +
                        "/task/" + std::to_string(tid) + "/comm");
        std::string name;
        std::getline(f, name);
        return name.empty() ? std::to_string(tid) : name;
    }
};

} // namespace profiler