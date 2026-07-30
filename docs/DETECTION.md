# Detecting memory leaks: the landscape, and where this tool sits

Written to answer two questions honestly. **What else exists**, and **when should you use something
other than LeakHunter?** A tool's own documentation is the worst place to look for the second
answer, which is why the comparison below is measured rather than asserted.

---

## 1. Five strategies

The strategy decides what a tool *can* see. Everything else is detail.

| Strategy | Tools | Sees | Cannot see | Needs |
|---|---|---|---|---|
| **Compile-time instrumentation** | AddressSanitizer / LeakSanitizer, HWASan | leaks, overflows, use-after-free | anything in code you did not rebuild | a rebuild |
| **Dynamic binary translation** | Valgrind Memcheck, Dr. Memory | all of the above, plus uninitialised reads | — | nothing; runs stock binaries |
| **Allocator interposition** | **LeakHunter**, heaptrack | everything live at exit, with stacks | reachability, overflows, use-after-free | nothing; `LD_PRELOAD` |
| **Sampling in production** | GWP-ASan, jemalloc/tcmalloc profiling | a fraction of bugs, on real traffic | whatever was not sampled | a build flag or an env var |
| **Static analysis** | GCC `-fanalyzer`, clang-analyzer, Coverity | leaks on error paths, before you run | anything crossing a translation unit | source |

Two more worth knowing about: **ARM MTE** tags allocations in hardware and catches use-after-free and
overflows deterministically at near-zero cost — Android 12 shipped it — and **`-fanalyzer`** catches
the single most common leak shape (allocate, fail, `return` without freeing) at compile time, which
is cheaper than any of the above because nothing has to run.

## 2. Reachability, and why "still reachable" exists

This is the deepest difference between tool families, and the one that decides which to use.

**LeakSanitizer** does a flood fill. It scans the roots — stacks, registers, globals — for anything
that looks like a heap pointer, marks what it finds, follows those blocks for more pointers, and
repeats. Whatever is never reached is a leak. Blocks reachable from a leaked block are reported
separately as *indirect* leaks.

**Valgrind** does the same and splits the result four ways: *definitely lost*, *indirectly lost*,
*possibly lost* (a pointer to the middle of a block), and *still reachable*.

**LeakHunter does none of this.** It reports everything live at exit. A cache you deliberately never
free, a singleton, a lazily built table — all of them are "leaks" here and "still reachable" there.

That is a real disadvantage and it is the reason `--suppressions` and the runtime-block
classification exist: they are the manual version of what reachability gives you for free. The
trade is that flood fill needs to stop the world and know where every root is, which is why the
tools that do it either recompile your program or emulate it.

---

## 3. Measured, on this machine

Same source, same day, GCC 13.3.0 on Ubuntu 24.04 (x86-64), on two targets from `poc/` and `poc5/`.

### `poc/docindex` — four planted defects

| | LeakHunter | ASan + LSan |
|---|---|---|
| Leaked bytes | **319,450** | **319,450** |
| Leaked blocks | **700** | **700** |
| Grouping | 3 sites | 4 direct + 1 indirect |
| Mismatched frees | 8 | 8 (`alloc-dealloc-mismatch`) |
| Wall time | 0.49 s total, **0.04 s** tracing only | 0.08 s |
| Rebuild needed | no | yes |

**They agree byte for byte.** Where they differ is in how the 700 are grouped, and that difference
is instructive:

```
LSan                                          LeakHunter
  Direct   204800 in 100  (indexBatch)          100 × 204800  indexBatch
  Direct    11200 in 200  (Document objects)    400 × 113600  buildDocument
  Indirect 102400 in 200  (their payloads)  ┘
  Direct      550 in 100  (field names)         200 ×   1050  duplicateSpan
  Direct      500 in 100  (field values)    ┘
```

LSan's **indirect** category is the thing LeakHunter cannot express: those 200 payload buffers are
unreachable *because* the 200 `Document`s that pointed at them leaked. Fix `buildDocument` and both
go. LSan tells you the causal structure; LeakHunter tells you the one function to open. Neither
ordering is wrong, and if you are chasing a large leak through a graph of objects, LSan's is more
informative.

### `poc5/clean_app` — the negative control

| | LeakHunter | ASan + LSan |
|---|---|---|
| Findings | **none** | **none** |
| Wall time | 0.43 s | 0.03 s |

~12,000 allocations through `unique_ptr` with a custom deleter, a `shared_ptr`/`weak_ptr` graph, 500
exceptions unwound by RAII, and a `pmr` arena. Both tools stay quiet. A negative control that only
one tool passes would mean one of them has a false positive.

### About those timings

LeakHunter's 0.49 s is **not** tracing cost. Tracing alone — `--no-source`, no report — is
**0.04 s** against an 0.01 s baseline, measured three times. The rest is one `llvm-symbolizer`
subprocess and writing two reports, paid once per run regardless of how long the target ran.

ASan's 0.08 s excludes a rebuild that took seconds, and the binary it produced is not the one you
ship. Comparing the two numbers directly would be comparing different things.

### Valgrind: could not be run here, and why

Valgrind 3.22 refuses to start on this machine:

```
valgrind:  Fatal error at startup: a function redirection
valgrind:  which is mandatory for this platform-tool combination cannot be set up.
valgrind:  A must-be-redirected function whose name matches the pattern: strlen
valgrind:  in an object with soname matching: ld-linux-x86-64.so.2
```

Ubuntu ships a stripped `ld.so`, so Valgrind needs `libc6-dbg` matching the exact glibc build. This
system runs glibc 2.39; the Ubuntu pool now carries debug symbols only for 2.41 and later, and debug
symbols must match by build ID. Without root there is no way to install the right one.

**So there are no Valgrind numbers in this document.** Everything above about Valgrind comes from its
documentation and is labelled as such. Publishing estimates next to measurements would make the
measurements worthless.

---

## 4. The thing this exercise actually found

While producing the table above, ASan reported a bug in `poc/` — code written for this project,
reviewed, and run under LeakHunter dozens of times:

```
ERROR: AddressSanitizer: heap-buffer-overflow
READ of size 513 at 0x51500001f900
    #1 poc::summarise(...) poc/src/ReportBuilder.cpp:25
0x51500001f900 is located 0 bytes after 512-byte region
allocated by poc::DocumentCache::copyPayload(long) poc/src/DocumentCache.cpp:67
```

`copyPayload` returns 512 bytes of payload. `summarise` called `strlen` on it. The payload is bytes,
not a string, and nothing NUL-terminates it — so the read ran one byte past a live block, every
time, on every run.

**LeakHunter cannot see this and never will.** It knows which blocks are live and where they came
from; it has no idea what the program reads out of them. Detecting that needs shadow memory or
redzones, which is a different strategy with a different cost.

The fix is in `copyPayload` returning the size. The bug is written up here rather than quietly
patched, because "here is a real bug my own tool is blind to" is worth more than any comparison
table.

---

## 5. When not to use LeakHunter

| If you need | Reach for |
|---|---|
| To distinguish a leak from a deliberate cache | LSan or Valgrind — reachability |
| Use-after-free, buffer overflows, uninitialised reads | ASan, Valgrind, or MTE |
| To find bugs on real production traffic | GWP-ASan, or jemalloc/tcmalloc profiling |
| To catch it before the program runs at all | `-fanalyzer`, clang-analyzer, Coverity |
| To trace every process in a build or a server tree | not yet — see [ROADMAP.md](ROADMAP.md) |
| What is *growing* in a service over days | two LeakHunter snapshots compared, or a heap profiler |

**Where LeakHunter is the right answer:** a binary you cannot or will not rebuild, a run where an
order-of-magnitude slowdown is not acceptable, a CI gate that needs an exit code and a JSON file, or
a leak you need attributed to a function and a line rather than to a stack you have to read.

They are not exclusive. The best use of an afternoon is usually LeakHunter in CI on every build, and
ASan on the same tests weekly.

## 6. Prevention, which beats all of it

Every leak in `poc/`, `poc3/` and `poc4/` disappears under the same change: give the resource an
owner with a destructor.

- `std::unique_ptr` / `std::make_unique`, and `std::vector` for buffers. `unique_ptr` with a custom
  deleter for anything that came from C.
- **`std::shared_ptr` does not save you.** A cycle of two `shared_ptr`s leaks exactly like a
  forgotten `free`, and every detector here reports it as one. `weak_ptr` on the way back is the fix;
  [`poc5/`](../poc5/) is built around demonstrating it.
- `-fanalyzer` in CI, which finds the allocate-then-return-early shape at compile time.
- `-fsanitize=address` on the test suite, which finds what this tool structurally cannot.

`poc3/` makes the point sharpest: `std::expected` makes the *error* impossible to ignore and does
nothing at all about the *resource*. Twenty-five years of language evolution between it and `poc4/`,
and the ownership mistake is untouched — because it was never a syntax problem.

---

## Sources

Strategy descriptions and anything not measured above come from:

- [LeakSanitizer — Clang documentation](https://clang.llvm.org/docs/LeakSanitizer.html)
- [Valgrind Memcheck: different ways to lose your memory — Red Hat Developer](https://developers.redhat.com/blog/2021/04/23/valgrind-memcheck-different-ways-to-lose-your-memory)
- [GWP-ASan: sampling-based detection of memory-safety bugs in production (ICSE 2024)](https://arxiv.org/abs/2311.09394)
- [GWP-ASan — LLVM documentation](https://llvm.org/docs/GwpAsan.html)
- [Detecting memory leaks with jemalloc — Red Hat](https://access.redhat.com/articles/6817071)
- [Arm Memory Tagging Extension — Android NDK](https://developer.android.com/ndk/guides/arm-mte)
- [Memory safety: how Arm MTE addresses this challenge — Arm](https://newsroom.arm.com/blog/memory-safety-arm-memory-tagging-extension)
- [Profiling and memory checking tools — Qt Wiki](https://wiki.qt.io/Profiling_and_Memory_Checking_Tools)

Reproduce the measurements with [`scripts/run_all_pocs.sh`](../scripts/run_all_pocs.sh) for the
LeakHunter side, and `g++ -fsanitize=address` on `poc/src/*.cpp` for the ASan side.
