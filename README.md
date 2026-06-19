# perf-profiler

A Linux CPU sampling profiler that attaches to **any running process** without restart, recompile, or code modification.

```
./profiler --pid 1234 --rate 99 --duration 30
```

Produces an interactive flamegraph — click any frame to zoom, hover for exact percentages.

---

## How it works

```
perf_event_open(2)         Opens a CPU-clock counter on the target PID
   └── PTRACE_SEIZE        Non-intrusive attach (target keeps running)
         └── sampling loop (every 1/rate seconds):
               ├── PTRACE_INTERRUPT   Pause the target
               ├── PTRACE_GETREGS     Read instruction pointer + frame pointer
               ├── process_vm_readv   Walk the frame-pointer chain in one syscall
               ├── /proc/PID/maps     Resolve addresses → DSO path + file offset
               ├── ELF .symtab        Resolve offset → function name
               └── PTRACE_CONT        Resume the target
```

All logic lives in **5 header-only C++ modules** with no external dependencies beyond the Linux kernel headers that ship with any distro.

---

## Architecture

```
include/
  types.hpp           — Core data structures (Frame, Sample, Profile, Config, Result<T>)
  symbol_resolver.hpp — /proc/PID/maps + ELF .symtab → human-readable symbols
  stack_unwinder.hpp  — ptrace register read + process_vm_readv frame-pointer walk
  perf_sampler.hpp    — perf_event_open counter + PTRACE_INTERRUPT sampling loop
  aggregator.hpp      — Raw samples → collapsed-stack hit counts
  report_writer.hpp   — Collapsed stacks → .folded text + self-contained HTML flamegraph

src/
  main.cpp            — CLI parsing, component wiring, progress output

tests/
  run_tests.cpp       — Unit + integration test suite (zero external dependencies)
  test_target.cpp     — CPU-burn workload for profiler validation
```

---

## Requirements

| | |
|---|---|
| **OS** | Linux (any kernel ≥ 3.10) |
| **Arch** | x86-64 or AArch64 |
| **Compiler** | g++ 8+ or clang++ 7+ with C++17 |
| **Permissions** | `sudo`, or `CAP_SYS_PTRACE` + `perf_event_paranoid ≤ 1` |
| **Dependencies** | None (stdlib + Linux kernel headers only) |

---

## Quick start

```bash
# 1. One-time: relax kernel perf restriction
echo 1 | sudo tee /proc/sys/kernel/perf_event_paranoid

# 2. Build
make

# 3. Launch a test target in the background
make test_target
./tests/test_target &
TARGET_PID=$!

# 4. Profile it
sudo ./profiler --pid $TARGET_PID --rate 99 --duration 10

# 5. Open the flamegraph
xdg-open flamegraph.html
```

---

## Usage

```
./profiler --pid <PID> [OPTIONS]

Required:
  --pid,      -p <PID>       Target process PID

Sampling:
  --rate,     -r <Hz>        Samples per second        (default: 99)
  --duration, -d <seconds>   Profiling duration        (default: 30)
  --depth,    -D <frames>    Max call stack depth      (default: 64)

Output:
  --output,   -o <file>      HTML flamegraph path      (default: flamegraph.html)
  --folded,   -f <file>      Folded stacks path        (default: <output>.folded)
  --verbose,  -v             Print sample count every second

Other:
  --help,     -h             Show usage
  --version                  Print version
```

---

## Output formats

### Interactive HTML flamegraph (`flamegraph.html`)

A self-contained single HTML file (no CDN, no internet required):

- **Click** a frame to zoom into its subtree
- **Double-click** or press **Escape** to reset zoom
- **Backspace** to zoom out one level
- **Hover** for exact sample count and percentage

### Folded stacks (`.folded`)

Compatible with [Brendan Gregg's flamegraph.pl](https://github.com/brendangregg/FlameGraph) and [Speedscope](https://speedscope.app):

```
myapp;main;workload_loop;cpu_math 183
myapp;main;workload_loop;cpu_fibonacci 112
myapp;main;workload_loop;cpu_memory 35
```

```bash
# Use with flamegraph.pl
./FlameGraph/flamegraph.pl flamegraph.html.folded > fg.svg
```

---

## Running tests

```bash
# Build everything
make all

# Unit tests only (no root needed, ~1s)
make check

# Full test suite including integration tests (needs ptrace permission)
make integration

# Or run the binary directly with a filter
./tests/run_tests aggregator
./tests/run_tests report_writer
./tests/run_tests integration
```

Test output looks like:

```
  perf-profiler test suite
  ──────────────────────────────────────────────────

  ✓  result_ok_carries_value               (0ms)
  ✓  result_err_carries_message            (0ms)
  ✓  aggregator_single_sample             (0ms)
  ✓  aggregator_identical_stacks_accumulate (0ms)
  ✓  report_writer_html_is_valid_html5    (2ms)
  ✓  integration_profiler_produces_html   (7843ms)
  ...

  25 passed  0 failed
```

---

## Troubleshooting

| Symptom | Fix |
|---|---|
| `perf_event_open: Permission denied` | `echo 1 \| sudo tee /proc/sys/kernel/perf_event_paranoid` |
| `ptrace SEIZE failed: Operation not permitted` | Run with `sudo`, or: `sudo setcap cap_sys_ptrace+eip ./profiler` |
| Flamegraph is flat — only 1 frame deep | Target was compiled without frame pointers. Recompile with `-fno-omit-frame-pointer` |
| All symbols show as `0x...` | Binary is stripped. Either use debug build, or add `-fno-omit-frame-pointer -g` |
| 0 samples collected | Process may be I/O-bound or sleeping — it genuinely isn't consuming CPU |
| Address already in use on perf_fd | Another profiler/perf tool may be attached — detach it first |

---

## Building with debug sanitisers

```bash
make DEBUG=1 all
make DEBUG=1 check
```

Enables AddressSanitizer + UndefinedBehaviourSanitizer.

---

## Technical notes

### Why frame pointers?

Frame-pointer walking requires the target to be compiled with `-fno-omit-frame-pointer` (the default on most distros for system packages, but often stripped by compiler optimisation flags like `-O2` in some toolchains). This avoids a dependency on `libunwind` or `libdw` (DWARF unwinding), keeping the profiler self-contained.

### Sampling jitter

The actual sample rate will be slightly lower than the requested rate because:
- `ptrace(PTRACE_INTERRUPT)` → `waitpid` has non-deterministic latency
- The sleep accounts for iteration time, but `process_vm_readv` and symbol resolution add overhead

At 99 Hz you can expect ~80–95 actual samples/second. For statistical profiling this is sufficient.

### perf_event_open vs pure ptrace

`perf_event_open` is opened even though we don't use its overflow interrupts. Its role is to keep a CPU-clock counter live on the target PID so the kernel accounts for its CPU usage correctly. The sampling interrupt itself is driven by our `PTRACE_INTERRUPT` + sleep loop, which is more portable and easier to reason about than `SIGIO`-based perf overflow handlers.

---

## License

MIT — see `LICENSE`.
