#pragma once
/*
 * thread_enumerator.hpp — Discovers all threads of a process
 *
 * Linux exposes every thread (task) of a process under:
 *   /proc/<pid>/task/<tid>/
 *
 * We read that directory to get all TIDs, then PTRACE_SEIZE each one
 * independently so we can sample them all.
 *
 * Why this matters:
 *   A process with 8 worker threads spending 12% each in the same function
 *   is actually spending 96% of its total CPU there. Without multi-thread
 *   support you'd see 12% and think it's fine.
 */

#include "types.hpp"

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
    pid_t tid  = 0;
    bool  attached = false;
};

class ThreadEnumerator {
public:
    // Discover all TIDs for the given PID from /proc/<pid>/task/
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
            // Every numeric entry is a TID
            bool all_digits = true;
            for (char *p = ent->d_name; *p; ++p)
                if (!isdigit((unsigned char)*p)) { all_digits = false; break; }
            if (!all_digits) continue;
            tids.push_back((pid_t)std::stoi(ent->d_name));
        }
        closedir(d);

        if (tids.empty())
            return Result<std::vector<pid_t>>::err("No threads found for PID " +
                                                    std::to_string(pid));
        return Result<std::vector<pid_t>>::ok_val(std::move(tids));
#else
        (void)pid;
        return Result<std::vector<pid_t>>::err("thread enumeration is Linux-only");
#endif
    }

    // Attach to all threads of a process.
    // Returns list of ThreadInfo with attached=true for each that succeeded.
    static std::vector<ThreadInfo> seize_all(pid_t pid) {
        std::vector<ThreadInfo> threads;

#ifdef __linux__
        auto r = list_tids(pid);
        if (!r.ok()) return threads;

        for (pid_t tid : r.value) {
            ThreadInfo t;
            t.tid = tid;
            // PTRACE_SEIZE: non-intrusive attach, doesn't stop the thread
            if (ptrace(PTRACE_SEIZE, tid, nullptr,
                       (void*)(uintptr_t)PTRACE_O_TRACECLONE) == 0) {
                t.attached = true;
            }
            threads.push_back(t);
        }
#endif
        return threads;
    }

    // Detach from all threads
    static void detach_all(const std::vector<ThreadInfo> &threads) {
#ifdef __linux__
        for (auto &t : threads)
            if (t.attached)
                ptrace(PTRACE_DETACH, t.tid, nullptr, nullptr);
#else
        (void)threads;
#endif
    }

    // Read the thread name from /proc/<pid>/task/<tid>/comm
    static std::string thread_name(pid_t pid, pid_t tid) {
        std::ifstream f("/proc/" + std::to_string(pid) +
                        "/task/" + std::to_string(tid) + "/comm");
        std::string name;
        std::getline(f, name);
        return name.empty() ? std::to_string(tid) : name;
    }
};

} // namespace profiler
