# cpu-profiler

Sampling profiler for Linux. Attaches to a running process by PID and tells you where it's spending CPU time — no recompile, no restart, no code changes.

```bash
sudo ./profiler --pid 1234 --rate 99 --duration 30
```

Output is a flamegraph HTML file you open in a browser. The x-axis is time, each row is a stack frame. Wider = more CPU.

## How it works

Every 1/rate seconds:
1. `PTRACE_INTERRUPT` — freeze the target mid-execution
2. `PTRACE_GETREGS` — read the instruction pointer and frame pointer
3. `process_vm_readv` — walk the frame pointer chain to get the full call stack
4. `/proc/PID/maps` + ELF `.symtab` — turn raw addresses into function names
5. `PTRACE_CONT` — let it keep running

Do this 99 times a second for 30 seconds, count which call stacks show up most often, and you have your answer. The process is paused for maybe 1ms out of every 10ms — not enough to affect most workloads.

`perf_event_open` keeps a CPU clock counter running on the target so the kernel tracks its usage correctly while we're attached.

## Project layout

```
include/
  types.hpp           - Frame, Sample, Profile, Config, Result<T>
  symbol_resolver.hpp - /proc/PID/maps + ELF symtab parsing
  stack_unwinder.hpp  - frame pointer walk via ptrace + process_vm_readv
                        also supports DWARF unwinding via libunwind (make USE_LIBUNWIND=1)
  perf_sampler.hpp    - main sampling loop, handles all threads
  thread_enumerator.hpp - finds all threads from /proc/PID/task/
  aggregator.hpp      - samples -> collapsed stack counts
  report_writer.hpp   - outputs .folded stacks, HTML flamegraph, pprof format

src/
  main.cpp            - CLI, wires everything together

tests/
  run_tests.cpp       - unit + integration tests, no dependencies
  test_target.cpp     - CPU burn target used to validate the profiler
  debug_runner.cpp    - interactive terminal walkthrough of each module
```

## Building

```bash
make          # default build
make all      # profiler + test binaries
make check    # run unit tests (no root needed)
make debug    # interactive module walkthrough
```

For binaries compiled without `-fno-omit-frame-pointer` (most production builds), use DWARF unwinding instead:

```bash
apt install libunwind-dev
make USE_LIBUNWIND=1
```

## Running

```bash
# allow perf_event_open for non-root (one time)
echo 1 | sudo tee /proc/sys/kernel/perf_event_paranoid

# build and run the test target
make all
./tests/test_target &

# profile it
sudo ./profiler --pid $! --rate 99 --duration 10
xdg-open flamegraph.html
```

## Options

```
--pid,      -p  target PID (required)
--rate,     -r  samples per second (default: 99)
--duration, -d  how long to run in seconds (default: 30)
--depth,    -D  max stack depth (default: 64)
--kernel,   -k  include kernel frames (default: off)
--output,   -o  HTML output path (default: flamegraph.html)
--folded,   -f  folded stacks output (default: <output>.folded)
--pprof,    -P  pprof format output (default: skip)
--verbose,  -v  print sample count every second
```

## Output

**flamegraph.html** — self-contained, no CDN. Click to zoom into a subtree, escape to reset, hover for exact percentages.

**.folded** — one line per unique call path with count. Works with [flamegraph.pl](https://github.com/brendangregg/FlameGraph) and [speedscope](https://speedscope.app).

**.pprof.txt** — compatible with `go tool pprof` and Grafana Pyroscope.

## Testing

```bash
make check                    # 32 unit tests, works on mac and linux
./test_all.sh                 # full system check (49 assertions)
./test_all.sh --integration   # live ptrace tests, needs root
```

## Known issues

- Frame pointer unwinding breaks on binaries compiled with `-O2` on some toolchains that omit frame pointers by default. Use `make USE_LIBUNWIND=1` or recompile the target with `-fno-omit-frame-pointer`.
- At very high sample rates (>500 Hz) the ptrace overhead becomes noticeable. 99 Hz is a good default.
- Kernel frames require `perf_event_paranoid <= 0` in addition to ptrace permission.

## Requirements

- Linux x86-64 or AArch64, kernel >= 3.10
- g++ with C++17
- `sudo` or `CAP_SYS_PTRACE` + `perf_event_paranoid <= 1`
- No other dependencies

## License

MIT