# LeakHunter

**Lightweight memory leak detection for C++ applications — no recompilation, no source changes.**

```console
$ leakhunter ./app
```

LeakHunter injects a small tracing agent into your program with `LD_PRELOAD`, records every
allocation and free, and reports whatever is still outstanding when the program exits — with the
function, file and line responsible.

It is deliberately **not** a Valgrind or AddressSanitizer replacement. It does one thing: find
leaks, attribute them to a function, and produce a report you can read or feed to CI. It also
reports blocks released through the wrong entry point (`new[]` freed with `delete`), because it
already has the evidence and the check costs nothing.

---

## Example

```console
$ leakhunter --output reports ./build/examples/malloc_free
[leakhunter info] running: ./build/examples/malloc_free
leaked 0x5615c2c04c40, 0x5615c2c074c0, 0x5615c2c09520

  LeakHunter summary
  ------------------------------------------------------------
  total allocations           2008  (672.03 KiB)
  total freed                 2004  (655.53 KiB)
  peak live memory       16.50 KiB
  memory leaked          12.50 KiB
  leaks                          3  in 3 distinct site(s)
  runtime blocks                 1  (4.00 KiB, not listed; --include-runtime)

  top leak sites
      8.00 KiB  x1      (anonymous namespace)::leakRealloc()
                        at examples/malloc_free.cpp:20
      4.00 KiB  x1      (anonymous namespace)::leakAligned()
                        at examples/malloc_free.cpp:25
         512 B  x1      (anonymous namespace)::leakCalloc()
                        at examples/malloc_free.cpp:14

  report: reports/report.json
  report: reports/report.html
```

Exit code is `0` when clean and `1` when leaks or mismatched frees were found, so it drops
straight into a CI gate.

A run can leak nothing and still fail. This program returns every block it allocates — through the
wrong door:

```console
$ leakhunter --output reports ./build/examples/mismatched_free
[leakhunter info] running: ./build/examples/mismatched_free
every block was released -- through the wrong door
[leakhunter warning] 4 block(s) were released through the wrong entry point -- this is undefined behaviour, see the report

  LeakHunter summary
  ------------------------------------------------------------
  total allocations              8  (6.30 KiB)
  total freed                    7  (2.30 KiB)
  peak live memory        4.00 KiB
  memory leaked                0 B
  leaks                          0  in 0 distinct site(s)
  runtime blocks                 1  (4.00 KiB, not listed; --include-runtime)
  mismatched frees               4  (undefined behaviour)

  mismatched frees
    16 B allocated with new, released with free()
                        allocated at (anonymous namespace)::newThenFree() at examples/mismatched_free.cpp:32
    1.00 KiB allocated with new[], released with delete
                        allocated at (anonymous namespace)::newArrayThenScalarDelete() at examples/mismatched_free.cpp:41
    1.00 KiB allocated with new[], released with free()
                        allocated at (anonymous namespace)::newArrayThenFree() at examples/mismatched_free.cpp:48
    ... and 1 more (see the report)

  report: reports/report.json
  report: reports/report.html
```

---

## Features

- **Zero instrumentation.** No recompilation, no linker flags, no `#include`. Works on release
  binaries (debug info improves the reports, it is not required).
- **Full allocator coverage.** `malloc`, `calloc`, `realloc`, `free`, `aligned_alloc`,
  `posix_memalign`, and every form of `operator new` / `operator delete`, including the sized,
  aligned and `nothrow` overloads.
- **Real stack traces.** DWARF CFI unwinding via the compiler runtime, with libunwind as an
  opt-in alternative. Symbols come from `dladdr` plus a DWARF pass through `llvm-symbolizer`,
  which is what recovers the names of `static` functions that never reach the dynamic symbol table.
- **Attribution, not just addresses.** Allocator frames are skipped so the blame lands on *your*
  function, and leaks are grouped by that function.
- **Signal from noise.** Blocks the C runtime never frees by design (stdio buffers, locale
  tables) are counted separately instead of drowning out real leaks.
- **Suppressions that stay honest.** `--suppressions leaks.supp` silences known leaks by function,
  module, source path or whole stack — and reports what it silenced, per rule, plus any rule that
  has rotted into matching nothing. Quiet output always means something.
- **Mismatched frees.** `new[]` released with `delete`, `malloc` released with `delete`, and the
  rest of that family. Undefined behaviour that usually keeps working right up until it doesn't.
  Free at run time: the allocating entry point is already recorded, so the check is a comparison.
- **Thread-aware.** Every allocation carries the kernel thread id that made it.
- **Two reports.** A self-contained HTML page (no CDN, no assets directory) and a versioned JSON
  document for tooling.

## Requirements

| | |
|---|---|
| OS | Linux (x86-64, aarch64). See [ROADMAP](docs/ROADMAP.md) for Windows and macOS. |
| Compiler | C++20 — GCC 11+ or Clang 14+ |
| Build | CMake 3.20+ |
| Required deps | fmt, spdlog, nlohmann/json — fetched automatically if not installed |
| Optional deps | `llvm` (`llvm-symbolizer`, for file:line), `libunwind-dev` (only with `-DLEAKHUNTER_WITH_LIBUNWIND=ON`) |

## Build

```console
$ cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
$ cmake --build build -j
$ ctest --test-dir build --output-on-failure
```

Dependencies are resolved with `find_package` first and fetched from source only when missing, so
a distro-packaged fmt/spdlog/json is used when you have one.

```console
$ sudo cmake --install build          # optional
```

## Usage

```console
leakhunter [options] <program> [program-args...]
leakhunter [options] -- <program> [program-args...]
```

The first non-option token ends LeakHunter's own options — `leakhunter ./app --verbose` passes
`--verbose` to `./app`, not to LeakHunter. Use `--` when you want to be explicit.

| Option | Effect |
|---|---|
| `-o, --output <dir>` | Report directory (default `leakhunter-report`) |
| `--html` / `--json` | Emit only that format (default: both) |
| `--max-frames <n>` | Frames captured per allocation (default 32, max 128) |
| `--min-leak-size <n>` | Omit leaks below `<n>` bytes from the listing |
| `--include-runtime` | Also list blocks the C runtime never frees |
| `--no-source` | Skip the `llvm-symbolizer` pass |
| `--no-mismatch-check` | Don't report blocks released through the wrong entry point |
| `--suppressions <file>` | Ignore leaks matching rules in `<file>` (repeatable) |
| `--keep-trace` | Keep the intermediate binary trace |
| `-v, --verbose` / `-q, --quiet` | Diagnostics level |

Full reference: [docs/USAGE.md](docs/USAGE.md).

## Exit codes

| Code | Meaning |
|---|---|
| 0 | Target ran, nothing found |
| 1 | Target ran, leaks and/or mismatched frees found |
| 2 | Invalid arguments, or a bad suppression file |
| 3 | The target could not be started |
| 4 | Internal error (tracing or report generation failed) |

## How it works

```
  leakhunter (host process)                  ./app (target process)
  ─────────────────────────                  ──────────────────────
  1. locate the agent                        LD_PRELOAD=libleakhunter_agent.so
  2. fork + exec with LD_PRELOAD  ────────▶  malloc / new intercepted
                                             stack captured per allocation
                                             records appended to a trace file
  3. wait for exit                 ◀────────  dladdr() symbol table + end marker
  4. replay the trace
  5. allocations − frees = leaks
  6. symbolise, blame, group
  7. report.html + report.json
```

The split is the core design decision: the injected half stays tiny and never allocates, and all
the analysis happens in a normal C++ process afterwards. See
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Overhead

Tracing cost is dominated by walking the stack once per allocation. Measured on this project's
benchmark (glibc 2.39, x86-64, GCC 13, `--no-source`):

| Workload | Untraced | `--max-frames 4` | `--max-frames 8` | `--max-frames 32` |
|---|---:|---:|---:|---:|
| 1M allocations, 1 thread | 0.01 s | — | 1.24 s | 1.53 s |
| 2M allocations, 8 threads | 0.01 s | — | 2.45 s | — |

`--max-frames` is the dial. The trace file costs roughly 65 bytes per allocation at 8 frames —
130 MB for the 1M-allocation run — which is why `--max-frames` matters for long runs and why
stack interning is high on the [roadmap](docs/ROADMAP.md).

These numbers are a worst case: the benchmark does nothing but allocate. A program that does real
work between allocations sees proportionally far less.

## Known limits

These are deliberate, and documented rather than hidden:

- **Every block live at exit is reported.** There is no reachability analysis — a cache you
  intentionally never free is reported as a leak. That is the trade that keeps LeakHunter simple.
- **Statically linked targets cannot be traced.** `LD_PRELOAD` has nothing to interpose on.
- **Only the process you launched is traced.** Children it `fork()`s or `exec()`s are recognised and
  stay passive, so `leakhunter ./build.sh` traces the *shell*, not the compiler. Point it at the
  binary you care about. Multi-process tracing is on the [ROADMAP](docs/ROADMAP.md).
- **A killed target yields partial data.** Records buffered at the moment of death are flushed
  from a fatal-signal handler, and `_exit()` is interposed for the same reason, so a crashing
  program still produces a usable report — but anything after the last flush is gone, and the
  report is flagged incomplete (`summary.droppedRecords > 0`).
- **Custom allocators are invisible** if they call `mmap` directly instead of going through libc.
- **Mismatched-free detection needs both halves of the C++ pair interposed.** A target that
  defines its own global `operator new` or `operator delete` — a static libstdc++, say — keeps its
  own, because the dynamic linker searches the executable before any `LD_PRELOAD` object. When
  LeakHunter detects that it sees only one half, it suppresses the whole check rather than
  reporting the artefacts, and says so in the report (`summary.mismatchDetection`). Leak detection
  is unaffected.
- **Double frees are not detected.** A second free of an address we have already retired looks
  the same as a free of something allocated before tracing began; both are counted in
  `summary.untrackedFrees`.

## Documentation

| | |
|---|---|
| [ARCHITECTURE.md](docs/ARCHITECTURE.md) | Modules, diagrams, execution flow, design rationale |
| [USAGE.md](docs/USAGE.md) | Complete CLI reference and recipes |
| [REPORT_FORMAT.md](docs/REPORT_FORMAT.md) | JSON schema |
| [ROADMAP.md](docs/ROADMAP.md) | What comes next, and why it is not here yet |
| [CONTRIBUTING.md](docs/CONTRIBUTING.md) | Development workflow |
| [examples/sample-report/](examples/sample-report/) | A generated `report.html` / `report.json` you can open right now |

## License

MIT — see [LICENSE](LICENSE).
