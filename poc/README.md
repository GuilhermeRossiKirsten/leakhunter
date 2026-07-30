# `docindex` — a proof of concept

A small document indexer with four planted defects, for demonstrating LeakHunter on something that
looks like application code rather than a `malloc` with no `free`.

**The program works.** It processes its input, prints a plausible summary and exits `0`. Nothing
crashes, nothing misbehaves, and a test suite would pass it. That is the point: these are the
defects that ship.

```console
$ ./poc/run_demo.sh
```

---

## The four defects

| # | Where | Kind | What it looks like in review |
|---|---|---|---|
| 1 | [`RecordParser.cpp`](src/RecordParser.cpp) | Leak on an error path | The happy path frees correctly. The `return false` doesn't. |
| 2 | [`DocumentCache.cpp`](src/DocumentCache.cpp) | `clear()` on a container of raw pointers | Reads exactly like cleanup. Releases the vector, not the objects. |
| 3 | [`ReportBuilder.cpp`](src/ReportBuilder.cpp) | `new char[]` released with `free()` | Undefined behaviour that happens to work. **No compiler warns** — the allocation and the release are in different translation units. |
| 4 | [`IndexWorker.cpp`](src/IndexWorker.cpp) | 2 KiB leaked per batch, on worker threads | Too small to notice. At a hundred batches a second it is 17 GiB a day. |

Each is surrounded by *correct* code doing the same thing — the parser's success path frees its
fields, the indexer runs four balanced batches for every leaked one, the cache's destructor is
right. A report that cannot separate those from the defects is not worth reading.

---

## What LeakHunter reports

Verified output, not an illustration:

```
  total allocations           4829  (1.16 MiB)
  total freed                 4124  (866.78 KiB)
  peak live memory      321.33 KiB
  memory leaked         311.96 KiB
  leaks                        700  in 3 distinct site(s)
  runtime blocks                 5  (5.25 KiB, not listed; --include-runtime)
  mismatched frees               8  (undefined behaviour)
```

| Site | Leaks | Bytes | Threads | Located at |
|---|---:|---:|---:|---|
| `indexBatch` | 100 | 200.00 KiB | **4** | `IndexWorker.cpp:27` |
| `buildDocument` | 400 | 110.94 KiB | 1 | `DocumentCache.cpp:18` |
| `duplicateSpan` | 200 | 1.03 KiB | 1 | `RecordParser.cpp:18` |

Plus 8 mismatched frees, all `allocated with new[], released with free()`, blamed on
`DocumentCache::copyPayload` at `DocumentCache.cpp:67`.

**Exit code 1.** It drops straight into a CI gate.

### Reading the numbers

A few of them deserve a note, because they are the parts people query:

- **700 leaks from 4829 allocations.** The other 4129 were freed correctly and do not appear. The
  4 balanced batches per leaked one, the parser's success path, the payload copies — all absent.
- **400 leaks at `buildDocument`, from 200 documents.** Each document is two allocations: the
  `Document` and its payload buffer. Leaks are grouped by the *function* responsible, so both land
  in one group and the location shown is the representative one.
- **`duplicateSpan` is a `static` function in an anonymous namespace.** It never reaches the dynamic
  symbol table, so `dladdr` cannot see it — only the DWARF pass through `llvm-symbolizer` recovers
  the name. Most application code looks like this, which is why that pass is on by default.
- **`threads=4` on the indexer site.** Every allocation carries the kernel thread id that made it,
  so one leaking function spread across a pool shows up as one site, not four unrelated ones.
- **8 mismatched frees, 0 bytes leaked from them.** Those blocks *were* returned. They are undefined
  behaviour, not lost memory, which is why they are counted separately and never folded into
  `leakedBytes`.
- **5 runtime blocks, not listed.** glibc's stdio buffers and locale tables, which it never frees by
  design. Counted, kept out of the way. `--include-runtime` shows them.

---

## Things worth trying

```console
# Silence the indexer leak, keep failing on the rest.
$ leakhunter --suppressions poc/docindex.supp build/poc/docindex

# Everything live at exit, including what libc never frees.
$ leakhunter --include-runtime build/poc/docindex

# Only the big ones.
$ leakhunter --min-leak-size 1024 build/poc/docindex

# Machine-readable, for a gate.
$ leakhunter --json build/poc/docindex
$ jq '.groups[] | {function, count, totalBytes, location}' leakhunter-report/report.json

# What it costs: 4829 allocations, so the overhead is invisible here. Turn the
# frame count down and watch the trace shrink.
$ leakhunter --max-frames 8 --keep-trace build/poc/docindex
```

[`docindex.supp`](docindex.supp) is a worked example of a suppression file: the scenario is that
bug #4 belongs to another team and is already ticketed, so CI should stop failing on it. The report
still states what was hidden and by which rule — a suppression makes a finding stop counting, it
never makes it disappear.

---

## Building it on its own

The demo is part of the LeakHunter build by default (`-DLEAKHUNTER_BUILD_POC=OFF` opts out, and an
integration test pins the numbers above so it cannot silently rot). It also builds standalone, which
is closer to how you would use the tool on your own project:

```console
$ cmake -S poc -B poc/build -DCMAKE_BUILD_TYPE=RelWithDebInfo
$ cmake --build poc/build
$ leakhunter ./poc/build/docindex
```

Note what is **not** in [`CMakeLists.txt`](CMakeLists.txt): no sanitizer flags, no instrumentation,
no LeakHunter headers, no link against anything of ours. Just `-g` so the reports can name a line.
The demo is an ordinary program, and that is the whole claim.

### A caveat the demo makes visible

`indexBatch` is marked `noinline`. Without it the compiler folds that one-line helper into the worker
lambda, the frame stops existing, and the report blames
`std::thread::_State_impl<...>::_M_run()` — at the correct file and line, but with a name nobody
wants to read. The attribution is still right; it just reads badly. Real indexing code is called
from several places and far too large to inline, so the attribute keeps the demo representative of
that rather than of a one-line helper. Worth knowing that heavily inlined code will name the
enclosing function.
