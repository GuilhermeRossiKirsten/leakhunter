# Validation: checking the numbers without trusting the tool

This document exists so you can decide for yourself whether LeakHunter reports **correct** data,
rather than taking its word for it.

## The method, and why it is arranged this way

A tool that validates itself proves nothing. So every leak in `poc6` … `poc10` is fixed by named
constants at the top of the file, and the expected result is **arithmetic you can do on paper**:

```
poc7:  30 x 256  +  20 x 512  +  15 x 256  +  10 x 128  =  23,040 bytes in 75 blocks
       aligned_alloc  posix_memalign  new(align_val_t)  new[]
```

Three independent checks have to agree before a row below is called correct:

1. **Arithmetic** — worked out from the constants, and pinned in the source by `static_assert`, so
   the program refuses to compile if the document and the code ever disagree.
2. **LeakHunter** — what this tool reports.
3. **AddressSanitizer + LeakSanitizer** — an unrelated implementation, using a completely different
   strategy (compile-time instrumentation and reachability analysis, not `LD_PRELOAD` interposition).

If (2) and (3) agree with (1), the number is right. If (2) and (3) disagree, at least one tool is
wrong and the document says so rather than picking a winner.

Every figure below was measured on **GCC 13.3.0, Ubuntu 24.04, x86-64, libstdc++**, at `-O1
-fno-omit-frame-pointer -fno-builtin`. The same expectations also pass under **Clang 18**, with both
the compiler's own unwinder and libunwind — four configurations, 35 integration tests, no
disagreement.

> **Why `-O1 -fno-builtin`, and how I found out it was necessary.**
>
> The standard permits a compiler to delete an allocation whose result is unobservable (C++ N3664).
> Under Clang 18 this was not theoretical: it removed **25 of poc6's `calloc` leaks and every one of
> poc8's 90 blocks**, so `exception_paths` reported *zero leaks* and passed. The report was accurate
> — about a binary that no longer did what its source said.
>
> Writing through the block does not stop it; the compiler folds the write-then-read back to a
> constant. Storing the pointer in a `volatile` does not stop it either. `-fno-builtin` does: it
> tells the compiler to treat `malloc`, `calloc`, `realloc` and `operator new` as opaque calls.
>
> This is worth knowing beyond this repository. **If you benchmark or validate a leak detector with
> an optimising compiler and no such flag, you may be measuring a program that no longer allocates.**
> The failure is silent and looks like a passing test.

---

## Allocator coverage

`malloc` and `realloc` are C; `new`/`delete` are C++. LeakHunter interposes **both families**, and
a validation set that only exercised one would leave half the interceptors unverified. C++ programs
reach the C family constantly anyway — every `std::string` and `std::vector` allocation ends up in
`malloc` beneath `operator new`.

So the coverage is deliberate:

| Entry point | Family | Exercised by |
|---|---|---|
| `malloc` | C | poc6, poc10 |
| `calloc` (`nmemb * size`) | C | poc6 |
| `realloc` (grow, shrink, from `NULL`) | C | poc6 |
| `free` | C | poc6, poc10 |
| `aligned_alloc` | C11 | poc7 |
| `posix_memalign` (returns through a parameter) | POSIX | poc7 |
| `operator new` / `delete` | C++ | poc8, poc10 |
| `operator new[]` / `delete[]` | C++ | poc6, poc7, poc8, poc9 |
| `operator new(size_t, align_val_t)` | C++17 | poc7 |
| `std::make_unique`, `std::make_shared` | C++ | poc8, poc9, poc10 |

`realloc` earns its own file because it is **the only entry point that frees and allocates in one
call**. A tracker that treats it as an allocation double-counts; one that treats it as a free loses
the block. Neither mistake is visible on a program that only calls `malloc` and `free`. `poc6` runs
the same growth twice — once through `realloc`, once the C++ way (`new[]`, copy, `delete[]` the old)
— so both paths can be compared in one report.

---

## Results

### poc6 — `realloc_chain`

100 chains of `malloc(64) → realloc(256) → realloc(1024)`, 60 released and 40 kept; 25 `calloc`
leaks; 50 C++ grow chains of which 20 are kept.

```
  40 x 1024  =  40,960     realloc chains kept
  25 x  128  =   3,200     calloc (8 x 16)
  20 x 1024  =  20,480     C++ grow chains kept
  ---------------------
  85 blocks     64,640 bytes
```

| | Blocks | Bytes | Sites |
|---|---|---|---|
| **Expected** (arithmetic) | **85** | **64,640** | — |
| LeakHunter | 85 | 64,640 | 2 |
| ASan + LSan | 85 | 64,640 | 3 records |

**✅ Agree.** The point of this one: 100 chains issued **300** allocation calls and 200 implicit
releases, yet only 40 blocks are live. `realloc` was accounted for exactly once, in both directions.
The intermediate 64 B and 256 B blocks appear nowhere.

### poc7 — `aligned_family`

The allocators that are easy to leave un-interposed, plus 200 matched pairs through each that must
**not** be reported.

| | Blocks | Bytes | Sites |
|---|---|---|---|
| **Expected** (arithmetic) | **75** | **23,040** | — |
| LeakHunter | 75 | 23,040 | 4 |
| ASan + LSan | 75 | 23,040 | 4 records |

**✅ Agree.** Four sites, one per family — so `aligned_alloc`, `posix_memalign` and the over-aligned
`operator new(size_t, align_val_t)` are all intercepted. A missing hook here would show as a smaller
number, silently. Turnover 9.49× confirms the 800 matched pairs were seen *and* correctly excluded.

The four leak functions are marked `[[gnu::noinline]]`, and that is load-bearing: at `-O1` the
compiler inlines all four into `main`, and the report then shows **one** site named `main`. That is a
correct answer to "which function do I open" — it is just not the one this file exists to
demonstrate. Getting this wrong is how a validation document ends up asserting something that is an
artefact of its own build flags.

### poc8 — `exception_paths`

50 raw buffers abandoned by a `throw`, 50 identical buffers held by `unique_ptr` through the *same*
throw, and 20 nested double-allocations abandoned together.

| | Blocks | Bytes | Sites |
|---|---|---|---|
| **Expected** (arithmetic) | **90** | **13,840** | — |
| LeakHunter | 90 | 13,840 | 3 |
| ASan + LSan | 90 | 13,840 | 3 records |

**✅ Agree.** The number that matters is what is *absent*: 90, not 140. The 50 `unique_ptr` buffers
released during unwinding are correctly not reported. Reporting 140 would mean RAII is not
understood; reporting 40 would mean the throwing path is not seen.

### poc9 — `thread_storm`

8 threads × 125 leaked blocks of 128 B, interleaved with 8 × 500 recycled blocks of 256 B.

| | Blocks | Bytes | Sites | Threads |
|---|---|---|---|---|
| **Expected** (arithmetic) | **1,000** | **128,000** | — | **8** |
| LeakHunter | 1,000 | 128,000 | 1 | 8 |
| ASan + LSan | 1,000 | 128,000 | 1 record | — |

**✅ Agree.** Exact under concurrency — no drift from races, and the site is correctly attributed to
all 8 threads. Total allocated 1,166,848 B against a peak of ~136 KB: the 1,024,000 B of churn was
tracked and excluded from the leak total, and shows up as 8.58× turnover instead.

### poc10 — `ownership_zoo`

25 `shared_ptr` cycles (leak) beside 25 `weak_ptr` cycles (must not), 30 `unique_ptr::release()`
calls, and 20 abandoned C handles owning a buffer each.

```
  50 x 256   =  12,800     cycle payloads
  30 x 512   =  15,360     released out of unique_ptr
  20 x (32 + 128) = 3,200  handle + its buffer
  ----------------------
  120 blocks    31,360 bytes   <- exact
  + 50 blocks   ? bytes        <- make_shared, size defined by libstdc++
```

| | Blocks | Bytes | Sites |
|---|---|---|---|
| **Expected** (arithmetic) | **170** | 31,360 exact **+ 50 library-sized** | — |
| LeakHunter | 170 | 33,360 | 4 |
| ASan + LSan | 170 | 33,360 | 7 records |

**✅ Agree on the numbers that matter.** Note the grouping differs — 4 sites against 7 LSan records
— for the reason in the section below; the block and byte totals, which are what "correct data"
means here, are identical.

This is the one POC whose byte total is *not* fully arithmetic, and the document says so
rather than hiding it: a `make_shared` block holds the object and the control block together, and
the control block's size belongs to the standard library, not to this program.

The residual closes exactly. `33,360 − 31,360 = 2,000` over 50 nodes = **40 bytes each**, which is
`sizeof(Node)` (a `char*` at 8 + a `shared_ptr` at 16 = 24) plus libstdc++'s 16-byte
`_Sp_counted_ptr_inplace`. Two independent tools measured 40; the layout predicts 40.

The 25 `weak_ptr` cycles are byte-for-byte identical apart from one word and leak nothing — so the
50 leaked nodes are the cycle, not the shape.

---

## Valgrind

Valgrind Memcheck now runs here — `libc6-dbg` matching this glibc build was installed, which is what
the earlier editions of this document said was missing. It is a **fourth** implementation and the most
different of all: dynamic binary translation, no recompilation, no `LD_PRELOAD`.

Memcheck splits its findings four ways. LeakHunter has one category, so the comparison is
`definitely + indirectly` against LeakHunter's total:

| Target | Expected | LeakHunter | VG definite | VG indirect | VG total | |
|---|---|---|---|---|---|---|
| `realloc_chain` | 85 / 64,640 | 85 / 64,640 | 85 / 64,640 | — | **85 / 64,640** | ✅ |
| `aligned_family` | 75 / 23,040 | 75 / 23,040 | 75 / 23,040 | — | **75 / 23,040** | ✅ |
| `exception_paths` | 90 / 13,840 | 90 / 13,840 | 90 / 13,840 | — | **90 / 13,840** | ✅ |
| `thread_storm` | 1,000 / 128,000 | 1,000 / 128,000 | 1,000 / 128,000 | — | **1,000 / 128,000** | ✅ |
| `ownership_zoo` | 170 blocks | 170 / 33,360 | 75 / 17,000 | 95 / 16,360 | **170 / 33,360** | ✅ |
| `clean_app` | nothing | nothing | — | — | **nothing** | ✅ |
| `docindex` | — | 700 / 319,450 | 500 / 217,050 | 200 / 102,400 | **700 / 319,450** | ✅ |

**Every total matches, and the splits add up exactly.** `ownership_zoo`: 75 + 95 = 170 blocks,
17,000 + 16,360 = 33,360 bytes. `docindex`: 500 + 200 = 700, 217,050 + 102,400 = 319,450.

Where Memcheck says *indirectly lost* it is describing structure LeakHunter cannot express — the 95
blocks in `ownership_zoo` are the cycle payloads and handle buffers, unreachable *because* the thing
owning them leaked. Same memory, same totals, more explanation. `still reachable` is **0** everywhere,
including `clean_app`.

### Mismatched frees

```
LeakHunter:   8 mismatched frees, grouped into 1 site
Valgrind:     ERROR SUMMARY: 8 errors from 1 contexts
```

Two independent tools, the same 8 occurrences collapsed to the same 1 context — arrived at
separately. That is a useful confirmation of the grouping added for exactly this case.

### Valgrind settles the one disagreement

Clang's LSan reported 9 of the ten `new char[128]` blocks in `aligned_family`; the arithmetic,
LeakHunter and GCC's LSan all said 10. Memcheck:

```
1,280 bytes in 10 blocks are definitely lost in loss record 1 of 4
still reachable: 0 bytes in 0 blocks
```

**10, definitely lost.** So the count stands at arithmetic + LeakHunter + GCC LSan + Valgrind = 10,
against Clang LSan = 9. The cause is understood — the `g_escape` global in the test harness, which
only LLVM's root scanner treats as keeping that block alive — and the conclusion is that **75 is the
correct answer**, which is what LeakHunter reports.

## Three run-time implementations, and the one disagreement

The comparison above uses GCC's `libasan`. Adding **LLVM's `compiler-rt`** — a separate codebase, not
a rebuild of the same one — makes it three independent implementations:

| Target | Expected | LeakHunter | GCC LSan | Clang LSan |
|---|---|---|---|---|
| `realloc_chain` | 85 | 85 | 85 | 85 |
| `aligned_family` | 75 | 75 | 75 | **74** |
| `exception_paths` | 90 | 90 | 90 | 90 |
| `thread_storm` | 1,000 | 1,000 | 1,000 | 1,000 |
| `ownership_zoo` | 170 | 170 | 170 | 170 |

**One disagreement, and it turned out to be the most instructive result here.**

Clang's LSan reported `1152 byte(s) in 9 object(s)` where the arithmetic says ten 128-byte blocks.
Chasing it:

1. **Was it a different binary?** LeakHunter was run on a GCC build *and* a Clang build of the same
   source. Both: **75 blocks, 23,040 bytes, identical breakdown.** So the binary was not the cause.
2. **Was the block still reachable from a register or the stack?** `LSAN_OPTIONS=use_registers=0:use_stacks=0`
   — still 74. Hypothesis wrong.
3. **Globals.** `LSAN_OPTIONS=use_globals=0` → `1280 byte(s) in 10 object(s)`. There it is.

The cause is **my own test harness**. `g_escape` — the `volatile void*` added to stop the compiler
eliding allocations — is a global, and it holds the pointer from the *last* `new char[128]`. LSan's
flood fill reaches that block from a global root and declines to call it a leak. LeakHunter has no
reachability analysis, so it counts it.

**Neither tool is wrong, and that is the point.** LSan says *"something still points at this; it may
be a deliberately retained singleton."* LeakHunter says *"this was allocated and never freed."* For
the question this document asks — how many blocks did the program fail to free — **75 is the correct
answer**, and it is the one the constants specify.

This is the reachability difference from [DETECTION.md](DETECTION.md) §2, which that document
describes in the abstract, caught happening by accident in a five-line harness. It is also a warning
about writing these harnesses: an anti-elision global changes what a reachability-based tool reports.

## What `-fanalyzer` found: nothing, for a documented reason

GCC's static analyser found **0 leaks in all five**. That is not a failure of the POCs — a control
confirms the analyser works:

```c
int f(void){ char* p = malloc(64); if(!p) return 1; return 0; }
   warning: leak of 'p' [CWE-401] [-Wanalyzer-malloc-leak]
```

`-fanalyzer` is a **C** analyser; GCC documents its C++ support as very limited, and every leak here
is through `new`, `make_shared` or across function boundaries. Worth knowing before anyone puts it in
a C++ CI pipeline expecting leak coverage. On C code it is genuinely useful and costs nothing to run.

---

## Why the site counts differ from LSan's

Blocks and bytes agree in all five. **Grouping does not always**, and the reason is worth stating
because it looks like a discrepancy and is not:

| | LeakHunter sites | LSan records |
|---|---|---|
| `realloc_chain` | 2 | 3 |
| `aligned_family` | 4 | 4 |
| `exception_paths` | 3 | 3 |
| `thread_storm` | 1 | 1 |
| `ownership_zoo` | 4 | 7 |

Two causes, both by design:

1. **Inlining.** LeakHunter resolves to the outermost frame (`llvm-symbolizer --inlining=false`), so
   a helper inlined into its caller is reported against the caller. LSan reports the inlined chain.
   LeakHunter answers *"which function do I open"*; LSan answers *"which exact source location"*.
2. **Direct vs indirect.** LSan splits a leaked structure from the buffers it owned — the 20
   abandoned handles in `ownership_zoo` are 20 direct plus 20 indirect records. LeakHunter has no
   reachability analysis and reports both as leaks in one group. See
   [DETECTION.md](DETECTION.md) §2.

Neither ordering is wrong, and neither changes a total. If you are chasing a leak through a graph of
objects, LSan's grouping is more informative; if you want the function to open, this one is.

---

## Summary

| POC | Expected | LeakHunter | ASan + LSan | |
|---|---|---|---|---|
| `realloc_chain` | 85 / 64,640 B | 85 / 64,640 B | 85 / 64,640 B | ✅ |
| `aligned_family` | 75 / 23,040 B | 75 / 23,040 B | 75 / 23,040 B | ✅ |
| `exception_paths` | 90 / 13,840 B | 90 / 13,840 B | 90 / 13,840 B | ✅ |
| `thread_storm` | 1,000 / 128,000 B | 1,000 / 128,000 B | 1,000 / 128,000 B | ✅ |
| `ownership_zoo` | 170 blocks | 170 / 33,360 B | 170 / 33,360 B | ✅ |
| `clean_app` (poc5) | **nothing** | nothing | nothing | ✅ |

Valgrind Memcheck agrees with every row above; see the Valgrind section for its four-way split.

**1,420 blocks and 262,880 bytes across five programs, three independent methods, no disagreement.**

The negative control matters as much as the rest: a tool that reports something on `poc5/clean_app`
gets switched off, and then it protects nothing.

---

## Reproducing this

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build -j

# what the program itself claims, from its own constants
./build/poc7/aligned_family

# what LeakHunter measures
./build/bin/leakhunter --json -o /tmp/v -- ./build/poc7/aligned_family

# what an unrelated tool measures
g++ -std=c++17 -g -O1 -fsanitize=address -o /tmp/asan-poc7 poc7/src/main.cpp
/tmp/asan-poc7
```

The expectations are also wired into the test suite, so a regression fails CI rather than waiting to
be noticed:

```bash
ctest --test-dir build -R integration_poc --output-on-failure
```

---

## What this does **not** prove

Being clear about the limits is what makes the rest worth reading.

- **It does not prove the absence of leaks in your program.** These are five programs whose defects
  were planted. Agreement here means the accounting is right, not that every code path is covered.
- **It does not test reachability**, because LeakHunter has none. Every block live at exit is
  reported. A deliberate cache is a "leak" here and *still reachable* under LSan — see
  [DETECTION.md](DETECTION.md) §2.
- **It does not cover memory errors**, only leaks. LeakHunter is structurally blind to
  buffer overflows and use-after-free; ASan found a real heap-buffer-overflow in this repository's
  own `poc/` that LeakHunter cannot see, written up in [DETECTION.md](DETECTION.md) §4.
- **Valgrind now runs**, after `libc6-dbg` matching this glibc was installed with root. Its results
  are in the Valgrind section above and agree with every row. Getting there without root was not
  possible, and the routes that failed are recorded because they are what most CI images will hit:

  | Attempt | Result |
  |---|---|
  | `apt-get download libc6-dbg` | 404 — 2.39-0ubuntu8.7 superseded, index stale |
  | archive + security pool listing | only `8.8` and `8` remain |
  | `libc6-dbg 8.8` against our `ld.so` | build ID `da07864e…` absent — point releases differ |
  | `debuginfod.ubuntu.com` (by build ID) | connection times out; host unreachable here |
  | `sudo apt-get install` | no passwordless sudo |
  | Launchpad API → librarian | API answers, but `launchpadlibrarian.net` is unreachable |

  Without matching symbols Valgrind stops at `a must-be-redirected function whose name matches the
  pattern: strlen in an object with soname matching: ld-linux-x86-64.so.2`. `sudo apt-get install
  valgrind libc6-dbg` fixes it in one step; nothing short of root did.
- **`ownership_zoo`'s byte total is platform-dependent.** The 33,360 figure holds for libstdc++ on
  x86-64. Its **block count of 170 is portable**; the integration test pins blocks, not bytes.
