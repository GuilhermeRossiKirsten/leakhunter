# JSON report format

`report.json` is the machine-readable output. It is versioned: `schemaVersion` is bumped on any
breaking change, and new fields may be added within a version without one.

**Current version: 2**

## Top level

```jsonc
{
  "schemaVersion": 2,
  "tool":    { "name": "leakhunter", "version": "0.1.0" },
  "run":     { /* what was executed */ },
  "summary": { /* aggregate counters */ },
  "groups":  [ /* leaks grouped by responsible function */ ],
  "leaks":   [ /* individual leaked blocks */ ],
  "mismatchedFrees": [ /* blocks released through the wrong entry point */ ],
  "suppressions": { /* what --suppressions rules did */ }
}
```

## `run`

| Field | Type | Meaning |
|---|---|---|
| `command` | string | The monitored command line |
| `pid` | number | PID of the traced process |
| `generatedAt` | string | ISO-8601 UTC timestamp |
| `exitCode` | number | The **target's** exit code, not LeakHunter's |
| `terminatingSignal` | number | Signal that killed the target, 0 if it exited normally |
| `durationMs` | number | Wall-clock runtime of the target |

## `summary`

| Field | Type | Meaning |
|---|---|---|
| `totalAllocations` | number | Allocations intercepted |
| `totalDeallocations` | number | Frees intercepted |
| `totalBytesAllocated` | number | Sum of all requested sizes |
| `totalBytesFreed` | number | Sum of sizes for frees matched to a known allocation |
| `leakedBytes` | number | **Headline:** bytes leaked by application code |
| `leakCount` | number | **Headline:** number of leaked blocks |
| `leakGroups` | number | Distinct leak sites (== `groups.length`) |
| `runtimeLeakedBytes` | number | Bytes the C runtime never frees by design |
| `runtimeLeakCount` | number | Blocks the C runtime never frees by design |
| `peakLiveBytes` | number | High-water mark of live memory |
| `untrackedFrees` | number | Frees of pointers allocated before tracing began |
| `droppedRecords` | number | **Non-zero means the data is incomplete** |
| `truncatedTraces` | number | Stacks that hit the `--max-frames` limit |
| `suppressedLeaks` | number | Leaks excluded from the listing by size/detail limits, **still in the totals** |
| `suppressedBytes` | number | Their total size |
| `suppressedByRules` | number | Leaks matched by a `--suppressions` rule, **excluded from the totals** |
| `suppressedByRulesBytes` | number | Their total size |
| `unusedSuppressionRules` | number | Rules that matched nothing (== `suppressions.unused.length`) |
| `mismatchedFreeCount` | number | Blocks released through the wrong entry point |
| `suppressedMismatches` | number | Mismatches counted but not listed (beyond the detail cap) |
| `mismatchesSuppressedByRules` | number | Mismatches matched by a `--suppressions` rule, **excluded from the count** |
| `mismatchDetection` | string | `"active"`, `"suppressed"` or `"disabled"` — see below |
| `clean` | boolean | `leakCount == 0 && mismatchedFreeCount == 0` |

`leakedBytes` counts application leaks only unless `--include-runtime` was passed, in which case
runtime blocks are folded in. `runtimeLeakedBytes` is always reported separately as well, so a
consumer can reconstruct either view.

A non-zero `droppedRecords` means every count is a **lower bound**. Gate on it in CI:

```console
$ jq -e '.summary.droppedRecords == 0' report.json || echo "incomplete trace"
```

## `groups`

Leaks sharing a responsible function, sorted by `totalBytes` descending.

| Field | Type | Meaning |
|---|---|---|
| `function` | string | Demangled name blamed for the leaks, or `<unknown>` |
| `module` | string | Object file containing it |
| `location` | string | `file:line`, empty when no debug info was available |
| `totalBytes` | number | Sum of the group's leaks |
| `count` | number | Number of leaks in the group |
| `threadCount` | number | Distinct threads that hit this site |
| `blamedFrame` | number | Index into `stackTrace` of the blamed frame |
| `stackTrace` | array | Representative stack (see below) |
| `leakIndices` | array | Indices into the top-level `leaks` array |

## `leaks`

Individual blocks, sorted by size descending. Capped at 5000 entries; `groups` always accounts for
every leak regardless.

| Field | Type | Meaning |
|---|---|---|
| `address` | string | Hex address in the target's address space |
| `size` | number | Requested size in bytes |
| `kind` | string | `malloc`, `calloc`, `realloc`, `aligned_alloc`, `operator new`, `operator new[]` |
| `origin` | string | `application` or `runtime` |
| `threadId` | number | Kernel thread id that allocated it |
| `timestampNs` | number | Nanoseconds since the agent attached |
| `responsibleFunction` | string | Convenience copy of the blamed frame's name |
| `responsibleFrame` | number | Index into `stackTrace` |
| `stackTrace` | array | Full captured stack |

## Stack frames

| Field | Type | Meaning |
|---|---|---|
| `address` | string | Hex program counter (the **call site**, see note) |
| `function` | string | Demangled name; empty when unresolvable |
| `displayName` | string | How a report should show it — see below |
| `module` | string | Path of the containing object file |
| `moduleOffset` | string | Hex offset within the module — what a symbolizer wants |
| `symbolOffset` | string | Hex distance from the start of the named symbol, `0x0` if unknown |
| `preciseName` | boolean | **True only when the name came from debug info** |
| `resolved` | boolean | False means only the raw address is known |
| `file` | string | *(optional)* Source path, present only with debug info |
| `line` | number | *(optional)* Source line |
| `column` | number | *(optional)* 1-based source column. `llvm-symbolizer` reports it; `addr2line` does not, and then the key is absent rather than `0` |

> **`preciseName` is the field to check before trusting `function`.**
>
> With debug info, the name is exact and `preciseName` is true. Without it, the name is whatever
> the symbol table came closest to — and in a stripped binary that can be an exported function some
> distance away, presented with misleading confidence. `_obstack_memory_used` may really be a
> static function 400 bytes later.
>
> `displayName` encodes this so a report cannot mislead by accident: exact names appear bare,
> approximate ones carry their offset (`someSymbol+0x34`), and nameless frames become
> `<unknown>+0x9049`. When `preciseName` is false, `groups[].location` gives `module+0xoffset`
> instead of `file:line` — feed that to `addr2line` or a disassembler to find the real call site.

> **Note on addresses.** Frames store the return address minus one, so symbolisation attributes
> them to the *call* instruction rather than to whatever follows it. This matters when a call is
> the last instruction of a function or a loop body. The stored value is therefore one byte before
> the real return address — standard practice for stack symbolisation.

## Example

```jsonc
{
  "schemaVersion": 2,
  "tool": {
    "name": "leakhunter",
    "version": "0.1.0"
  },
  "run": {
    "command": "/home/dev/LeakHunter/build/examples/simple_leak",
    "pid": 742,
    "generatedAt": "2026-07-29T23:35:43Z",
    "exitCode": 0,
    "terminatingSignal": 0,
    "durationMs": 4
  },
  "summary": {
    "totalAllocations": 2,
    "totalDeallocations": 0,
    "totalBytesAllocated": 5120,
    "totalBytesFreed": 0,
    "leakedBytes": 1024,
    "leakCount": 1,
    "leakGroups": 1,
    "runtimeLeakedBytes": 4096,
    "runtimeLeakCount": 1,
    "peakLiveBytes": 5120,
    "untrackedFrees": 0,
    "droppedRecords": 0,
    "truncatedTraces": 0,
    "suppressedLeaks": 0,
    "suppressedBytes": 0,
    "suppressedByRules": 0,
    "suppressedByRulesBytes": 0,
    "unusedSuppressionRules": 0,
    "mismatchedFreeCount": 0,
    "suppressedMismatches": 0,
    "mismatchesSuppressedByRules": 0,
    "mismatchDetection": "active",
    "clean": false
  },
  "groups": [
    {
      "function": "(anonymous namespace)::allocateBuffer(unsigned long)",
      "module": "/home/dev/LeakHunter/build/examples/simple_leak",
      "location": "/home/dev/LeakHunter/examples/simple_leak.cpp:11",
      "totalBytes": 1024,
      "count": 1,
      "threadCount": 1,
      "blamedFrame": 0,
      "leakIndices": [0],
      "stackTrace": [
        {
          "address": "0x5d85dd103184",
          "function": "(anonymous namespace)::allocateBuffer(unsigned long)",
          "displayName": "(anonymous namespace)::allocateBuffer(unsigned long)",
          "module": "/home/dev/LeakHunter/build/examples/simple_leak",
          "moduleOffset": "0x1184",
          "symbolOffset": "0x0",
          "preciseName": true,
          "resolved": true,
          "file": "/home/dev/LeakHunter/examples/simple_leak.cpp",
          "line": 11
        },
        {
          "address": "0x5d85dd1031b2",
          "function": "main",
          "displayName": "main",
          "module": "/home/dev/LeakHunter/build/examples/simple_leak",
          "moduleOffset": "0x11b2",
          "symbolOffset": "0x0",
          "preciseName": true,
          "resolved": true,
          "file": "/home/dev/LeakHunter/examples/simple_leak.cpp",
          "line": 21
        },
        {
          "address": "0x751441c2a1c9",
          "function": "__libc_init_first",
          "displayName": "__libc_init_first",
          "module": "/lib/x86_64-linux-gnu/libc.so.6",
          "moduleOffset": "0x2a1c9",
          "symbolOffset": "0x0",
          "preciseName": false,
          "resolved": true
        },
        {
          "address": "0x751441c2a28a",
          "function": "__libc_start_main",
          "displayName": "__libc_start_main+0x8a",
          "module": "/lib/x86_64-linux-gnu/libc.so.6",
          "moduleOffset": "0x2a28a",
          "symbolOffset": "0x8a",
          "preciseName": false,
          "resolved": true
        },
        {
          "address": "0x5d85dd1030a4",
          "function": "_start",
          "displayName": "_start",
          "module": "/home/dev/LeakHunter/build/examples/simple_leak",
          "moduleOffset": "0x10a4",
          "symbolOffset": "0x0",
          "preciseName": false,
          "resolved": true
        }
      ]
    }
  ],
  "leaks": [
    {
      "address": "0x5d85e30ed2b0",
      "size": 1024,
      "kind": "malloc",
      "origin": "application",
      "threadId": 742,
      "timestampNs": 511214,
      "responsibleFunction": "(anonymous namespace)::allocateBuffer(unsigned long)",
      "responsibleFrame": 0,
      "stackTrace": [ /* as above */ ]
    }
  ],
  "mismatchedFrees": [],
  "suppressions": {
    "applied": [],
    "unused": []
  }
}
```
This block is generated from a real run of `examples/simple_leak`, not written by hand, so it
cannot drift from what the tool actually emits.

## `snippet`

Present on a `groups[]` or `mismatchedFrees[]` entry when the blamed line could be read out of the
source tree. **Absent, not null**, when it could not — so `"snippet" in group` is a straight answer.

```jsonc
{
  "file": "/home/dev/app/poc/src/IndexWorker.cpp",
  "firstLine": 23,
  "blamedLine": 27,
  "column": 60,
  "lines": [ "/// called from several places...", "..." ]
}
```

| Field | Type | Meaning |
|---|---|---|
| `file` | string | Path **as resolved on the machine that generated the report** — which may differ from the frame's `file` when `--source-root` was used |
| `firstLine` | number | 1-based line number of `lines[0]` |
| `blamedLine` | number | The line to highlight. Always within `[firstLine, firstLine + lines.length)` |
| `column` | number | *(optional)* 1-based column of the allocation |
| `lines` | array | The source text, tabs already expanded, without line endings |

The line to highlight is `lines[blamedLine - firstLine]`. Windows are clamped at both ends of the
file, so `firstLine` is not always `blamedLine - context`.

Controlled by `--no-source-snippets`, `--snippet-context` and `--source-root`. Off entirely under
`--no-source`, since with no `file`/`line` there is nothing to read.

## `triage`

Present on a `groups[]` entry when the timings allowed a verdict. Absent, not null, when they did
not — a site with no individually-listed leaks, or a run too short to classify.

```jsonc
{
  "pattern": "steady",
  "description": "spread across the run; grows with uptime",
  "firstSeenNs": 201338,
  "lastSeenNs": 3806751882,
  "sampleIsPartial": false,
  "bytesPerHour": 10232203.35,
  "bytesPerDay": 245572880.5,
  "suppressionRule": "function:*(anonymous namespace)::handleRequest*",
  "advice": ["Grows with uptime: about 9.76 MiB/hour ...", "..."]
}
```

| Field | Type | Meaning |
|---|---|---|
| `pattern` | string | `steady`, `startup`, `burst`, `one-shot` or `unknown` |
| `description` | string | one line of plain language for the pattern |
| `firstSeenNs` / `lastSeenNs` | number | window the site's listed leaks span, from process start |
| `sampleIsPartial` | boolean | timings come from fewer blocks than the site leaked |
| `bytesPerHour` / `bytesPerDay` | number | *(optional)* extrapolated; **only present for `steady`** |
| `suppressionRule` | string | *(optional)* a rule that silences this exact site |
| `advice` | array | what to do, most important first |

`bytesPerHour` is deliberately absent for anything but `steady`: a block allocated once at start-up
has no meaningful rate, and emitting `0.0` invites someone to plot it.

**`unknown` is a real answer.** A run shorter than 250 ms cannot tell "grows with uptime" from
"happened once" — every site looks clustered because the whole run is a cluster — so the pattern
stays `unknown` and `advice` says why.

## `hotSpots`

Top level. The ten call sites that allocated the most bytes, **ranked by bytes allocated rather
than bytes leaked**, and present whether or not anything leaked.

A row here may have released every byte it took. That is the point: a function moving gigabytes
through a small working set is a real cost, and the leak report is structurally blind to it because
the registry forgets a block the moment it is freed.

```jsonc
{
  "function": "poc5::makeRawBuffer(unsigned long)",
  "module": "/home/dev/build/poc5/clean_app",
  "location": "poc5/src/Buffers.cpp:31",
  "totalBytes": 1156608,
  "count": 1259,
  "averageBytes": 918,
  "peakLiveBytes": 134656,
  "turnover": 8.59,
  "liveBytes": 0,
  "liveCount": 0
}
```

| Field | Type | Meaning |
|---|---|---|
| `function`, `module`, `location` | string | attributed exactly as a leak group is |
| `totalBytes` / `count` | number | ever allocated here, across the whole run |
| `averageBytes` | number | `totalBytes / count` |
| `peakLiveBytes` | number | the most this one site held at a single moment |
| `turnover` | number | `totalBytes / peakLiveBytes` — how hard this site recycles |
| `liveBytes` / `liveCount` | number | still outstanding at exit; **`0` is the interesting case** |

`hotSpotsTruncated` sits beside the array: `true` means the program had more distinct call sites
than could be tracked (20,000), so the ranking favours sites seen early.

**Sites are merged by responsible frame, not by call stack.** The registry keys on the whole stack
because it has no symbols to key on anything better; a helper called from three places would
otherwise be three rows with the same name and nothing to distinguish them. `peakLiveBytes` is
**summed** across the merged paths rather than maxed — two call paths can hold their blocks
simultaneously, so the max would report less than the site demonstrably held.

**This is not a leak list.** Nothing here affects `summary.clean`, the exit code, or any leak count.

## `timeline`

Top level, not per group. Present whenever anything was allocated; absent, not null, otherwise.

Where the rest of the report answers *what was left over*, this answers *what the run cost*. A
program that peaks at 900 MiB and exits holding 4 KiB has no leak and a very real problem, and
without this the report calls it clean and says nothing else.

```jsonc
{
  "peakBytes": 14848,
  "peakBlocks": 22,
  "peakAtNs": 3799182004,
  "turnover": 3.034,
  "endedNearPeak": true,
  "truncated": false,
  "coverage": 1.0,
  "summary": "Live memory climbed to 14.50 KiB and ended at 14.00 KiB: ...",
  "timestampsNs": [33421, 66842, ...],
  "liveBytes":    [4608,  4608,  ...],
  "liveBlocks":   [9,     9,     ...]
}
```

| Field | Type | Meaning |
|---|---|---|
| `peakBytes` / `peakBlocks` | number | high-water mark, and how many blocks made it up |
| `peakAtNs` | number | when the peak happened, from process start |
| `turnover` | number | `totalBytesAllocated / peakBytes` — how hard the program recycles |
| `endedNearPeak` | boolean | ended holding ≥90% of its peak: memory went up and stayed up |
| `truncated` | boolean | **read this before plotting** — see below |
| `coverage` | number | fraction of the run the samples cover; `1.0` unless truncated |
| `summary` | string | one line of plain language about the shape |
| `timestampsNs`, `liveBytes`, `liveBlocks` | array | the samples, always the same length |

**Parallel arrays, not an array of objects.** A timeline is up to 120 samples of three numbers;
repeating the key names 120 times triples the size of the section for nothing a consumer gains.
The three arrays are index-aligned and always equal in length.

**`peakBytes` is tracked per event, not per sample.** A spike that rises and falls between two
samples still sets the peak, so `max(liveBytes)` can be lower than `peakBytes`. That is not a bug:
the samples are what to plot, `peakBytes` is what actually happened.

### `truncated` is not a detail

Events are capped at 2,000,000 (allocation *and* free each count as one). Past that the timeline
covers only the beginning of the run, and `truncated` is `true`.

This matters more than an ordinary cap because of *how* it fails. A truncated series shows memory
rising and then going flat — which is exactly what "the leak stopped" looks like. A consumer that
plots the samples without checking this flag will draw a reassuring chart of a leaking program.

When `truncated` is `true`, `endedNearPeak` is always `false`: the tail is missing, so the claim
would be about samples that do not exist. Use `coverage` to label the axis honestly.

## `mismatchedFrees`

A block released through an entry point that does not pair with the one that allocated it —
`new[]` freed with `delete`, `malloc` freed with `delete`, and so on. These are **not leaks**: the
memory was returned. They are undefined behaviour, and they are reported because the tool is
already holding the evidence needed to prove them.

```jsonc
{
  "address": "0x5599c2a4b2a0",
  "size": 1024,
  "allocatedBy": "operator new[]",
  "releasedBy": "free",
  "description": "allocated with new[], released with free()",
  "allocatedOnThread": 12345,
  "releasedOnThread": 12345,
  "timestampNs": 4102338,
  "responsibleFunction": "(anonymous namespace)::newArrayThenFree()",
  "responsibleFrame": 1,
  "stackTrace": [ /* the ALLOCATION stack, not the free stack */ ]
}
```

The stack is where the block was **allocated**. Capturing a second stack on every free would
roughly double the cost of tracing to answer a question the allocation site already answers: you
know which object it is, so you know which `delete` is wrong.

### `mismatchDetection`

`mismatchedFreeCount == 0` means the program is clean **only when `mismatchDetection` is
`"active"`**. The other two values mean the check did not run:

| Value | Meaning |
|---|---|
| `active` | The check ran. Zero findings means zero mismatches. |
| `suppressed` | The target allocates with `new` but our `operator delete` never ran, so the target defines its own. Every pairing derivable from that gap would be an artefact, so the check was disabled automatically. |
| `disabled` | `--no-mismatch-check` was passed. |

`suppressed` happens for real: the dynamic linker searches the executable itself before any
`LD_PRELOAD` object, so a program that links a static libstdc++ or defines a global
`operator delete` keeps its own. Leak detection is unaffected either way.

Gate on it in CI if the check matters to you:

```console
$ jq -e '.summary.mismatchDetection == "active"' report.json || echo "check did not run"
```

## `suppressions`

What the `--suppressions` rules actually did. Always present, so a consumer can tell "no rules were
given" from "rules were given and hid nothing".

```jsonc
{
  "applied": [
    { "rule": "leaks.supp:12: function:*leakSmall*", "count": 100, "bytes": 6400 }
  ],
  "unused": [
    "leaks.supp:18: function:*renamedLastYear*"
  ]
}
```

`applied` is sorted by `bytes` descending: if one rule is hiding most of the leaked memory, that is
the one worth re-examining. Each `rule` string is `<file>:<line>: <scope>:<pattern>`, so it points
straight at the line to edit.

`unused` lists rules that matched nothing. That is a defect in the suppression file, not in the
program: the function was renamed, the library was dropped, the pattern had a typo — and the rule now
looks like coverage that is not there. `--strict-suppressions` makes it exit 2.

Rules apply to **mismatched frees as well as leaks** — vendored code with undefined behaviour you
cannot fix has to be acceptable, or the tool blocks every build and gets removed from CI.
`summary.mismatchesSuppressedByRules` breaks out how many of the suppressed findings were
mismatches.

A rule's `count` covers both classes; its `bytes` counts **leaked bytes only**. A mismatched block
was returned — it is undefined behaviour, not lost memory — so folding its size into a "bytes
suppressed" figure would overstate the memory impact. A rule that suppressed nothing but mismatches
therefore shows a non-zero `count` and `bytes: 0`, which is accurate.

Findings counted here are **excluded from `leakCount`, `leakedBytes` and `mismatchedFreeCount`**,
which is the whole point of a suppression. They are never dropped silently:

```console
$ jq -r '.suppressions.applied[] | "\(.count)\t\(.bytes)\t\(.rule)"' report.json
100     6400    leaks.supp:12: function:*leakSmall*
```

Note the distinction from `summary.suppressedLeaks`, which is `--min-leak-size` and the detail cap:
those leaks stay in the totals and are only missing from the listing.

## Compatibility

| Version | Change |
|---|---|
| 2 | Added `triage` on groups — additive, `summary.clean` unchanged, so **no version bump**. Added `snippet` on groups and mismatched frees, and `column` on stack frames — both purely additive, so **no version bump**: a consumer that ignores unknown keys is unaffected, and `summary.clean` did not change meaning. Added `suppressions`, `summary.suppressedByRules`, `summary.suppressedByRulesBytes`, `summary.unusedSuppressionRules`, `mismatchedFrees`, `summary.mismatchedFreeCount`, `summary.suppressedMismatches` and `summary.mismatchDetection`. `summary.clean` now also requires `mismatchedFreeCount == 0`, which is why this is a version bump rather than an additive change. |
| 1 | Initial format. |

## Stability guarantees

Within a `schemaVersion`:

- existing fields keep their name, type and meaning;
- new fields may be added — consumers must ignore unknown ones;
- array ordering is stable (`groups` and `leaks` by size descending).

Breaking changes bump `schemaVersion`.

## Binary trace format

The intermediate file written by the agent (`--keep-trace`) is a private format between the agent
and the host, fully specified in `include/leakhunter/ipc/TraceFormat.hpp`. It carries its own magic
number and version and is validated on read. It is **not** a stable public interface — read
`report.json` instead.
