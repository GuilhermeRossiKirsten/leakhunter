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

**Everything at once:** [`./scripts/run_all_pocs.sh`](scripts/run_all_pocs.sh) builds LeakHunter,
builds all five demonstrations standalone, runs each under the tool, and leaves one timestamped
report per binary.

**Try it in one command:** [`./poc2/run_demo.sh`](poc2/) starts a service that leaks on every tick,
stops it, and reports — showing that a process which never exits on its own is still something you
can point this at. For attribution across a realistic codebase, [`./poc/run_demo.sh`](poc/) builds a small document indexer with four
planted defects — an error-path leak, a `clear()` on a container of raw pointers, a `new[]` released
with `free()` across translation units, and a 2 KiB-per-batch leak on worker threads — and shows
LeakHunter naming each one with its file and line. The program exits `0` and looks healthy without
it.

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
- **Triage, not just a list.** Each site is classified by when its leaks happened — a fixed start-up
  cost reads differently from one growing 200 MiB/day — with an extrapolated rate, advice keyed to
  the allocator used, and a ready-to-paste suppression rule.
- **Memory over time, not just what was left.** Every report carries a profile of live memory
  across the run: peak, when it happened, how much of it came back, and turnover — how many bytes
  passed through the allocator per byte ever held at once. A program that peaks at 900 MiB and exits
  holding 4 KiB has no leak and a very real problem, and a leak count alone calls that clean.
- **Where the memory went, not only what stayed.** The ten call sites that allocated the most
  bytes, ranked by volume rather than by what leaked — including sites that released every byte
  they took. A function pushing gigabytes through a small working set is a real cost with a real
  fix (reserve, pool, reuse), and a leak report cannot see it at all.
- **Attribution, not just addresses.** Allocator frames are skipped so the blame lands on *your*
  function, and leaks are grouped by that function.
- **The leaking line, shown.** Reports include the source line itself with a caret on the exact
  column, in the terminal and in the HTML. `--diagnostics` emits `file:line:col: warning:` that your
  editor can jump to and that GitHub Actions annotates onto the pull-request diff.
- **Signal from noise.** Blocks the C runtime never frees by design (stdio buffers, locale
  tables) are counted separately instead of drowning out real leaks.
- **Suppressions that stay honest.** `--suppressions leaks.supp` silences known leaks by function,
  module, source path or whole stack — and reports what it silenced, per rule, plus any rule that
  has rotted into matching nothing. Quiet output always means something.
- **Numbers you can check.** Five demonstrations (`poc6`–`poc10`) whose every leak is fixed by named
  constants, so the expected result is arithmetic you can do on paper — cross-checked against
  AddressSanitizer, and pinned in CI. See [VALIDATION.md](docs/VALIDATION.md).
- **Mismatched frees, grouped.** `new[]` released with `delete`, `malloc` released with `delete`, and
  the rest of that family — collapsed by call site *and* pairing, with a count. A loop gets the same
  address back from the allocator every iteration, so eight turns of one line used to render as eight
  identical findings; `distinctAddresses` now says outright whether they are iterations or separate
  sites. Undefined behaviour that usually keeps working right up until it doesn't.
  Free at run time: the allocating entry point is already recorded, so the check is a comparison.
- **Thread-aware.** Every allocation carries the kernel thread id that made it.
- **Two reports.** A self-contained HTML page (no CDN, no assets directory) and a versioned JSON
  document for tooling.

## Requirements

| | |
|---|---|
| OS | Linux (x86-64, aarch64). See [ROADMAP](docs/ROADMAP.md) for Windows and macOS. |
| Compiler | C++20. Built and tested with **GCC 13.3** and **Clang 18.1** (identical results); GCC 11+ / Clang 14+ should work, but are not what CI runs |
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
| `--report-name <t>` | Report file stem (default `{target}-{timestamp}`, so runs accumulate) |
| `--html` / `--json` | Emit only that format (default: both) |
| `--max-frames <n>` | Frames captured per allocation (default 32, max 128) |
| `--min-leak-size <n>` | Omit leaks below `<n>` bytes from the listing |
| `--include-runtime` | Also list blocks the C runtime never frees |
| `--no-source` | Skip the `llvm-symbolizer` pass |
| `--no-source-snippets` | Don't read source files into the reports |
| `--source-root <dir>` | Where to find sources when the recorded path isn't valid here |
| `--diagnostics` | Compiler-style findings on stderr, for editors and CI |
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

> **Note on the HTML report:** it embeds excerpts of your source code, because that is what makes
> it useful. Sharing the artifact therefore shares code. `--no-source-snippets` turns that off; see
> [docs/USAGE.md](docs/USAGE.md#source-snippets).

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
- **A target that closes every descriptor can still take the trace away.** The agent's descriptor is
  moved above 512, which dodges the usual `for (fd = 3; fd < 256; ++fd) close(fd)` idiom, but
  `close_range()` and `closefrom()` reach it anyway. When that happens the agent says so on stderr
  rather than leaving an empty trace to be misdiagnosed.
- **Mismatched-free detection needs both halves of the C++ pair interposed.** A target that
  defines its own global `operator new` or `operator delete` — a static libstdc++, say — keeps its
  own, because the dynamic linker searches the executable before any `LD_PRELOAD` object. When
  LeakHunter detects that it sees only one half, it suppresses the whole check rather than
  reporting the artefacts, and says so in the report (`summary.mismatchDetection`). Leak detection
  is unaffected.
- **Allocation hot spots are capped at 20,000 distinct call sites.** Past that the ranking favours
  sites seen early in the run, and `hotSpotsTruncated` says so.
- **The memory profile is capped at 2,000,000 events.** Past that it covers only the start of the
  run and is marked `truncated` — which every renderer states outright, because a truncated profile
  goes flat and looks exactly like a leak that stopped.
- **Double frees are not detected.** A second free of an address we have already retired looks
  the same as a free of something allocated before tracing began; both are counted in
  `summary.untrackedFrees`.

## Documentation

| | |
|---|---|
| [poc/](poc/) | **Start here.** A working program with four planted defects, and what LeakHunter says about it |
| [poc2/](poc2/) | A long-running service: stop it with Ctrl-C and still get the report |
| [poc3/](poc3/) & [poc4/](poc4/) | The same program in **C++23** and **C++98** — identical findings, because interception happens below the language |
| [poc5/](poc5/) | **The negative control.** 12,000 allocations through smart pointers, RAII unwinding and a pmr arena — reported clean |
| [DETECTION.md](docs/DETECTION.md) | **The landscape.** Five detection strategies, a measured head-to-head against ASan/LSan, and when to use something else |
| [ARCHITECTURE.md](docs/ARCHITECTURE.md) | Modules, diagrams, execution flow, design rationale |
| [USAGE.md](docs/USAGE.md) | Complete CLI reference and recipes |
| [REPORT_FORMAT.md](docs/REPORT_FORMAT.md) | JSON schema |
| [ROADMAP.md](docs/ROADMAP.md) | What comes next, and why it is not here yet |
| [CONTRIBUTING.md](docs/CONTRIBUTING.md) | Development workflow |
| [examples/sample-report/](examples/sample-report/) | A generated `report.html` / `report.json` you can open right now |

## License

MIT — see [LICENSE](LICENSE).
