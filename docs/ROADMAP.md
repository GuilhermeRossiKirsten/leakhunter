# Roadmap

Ordered by value per unit of complexity. Everything here is deliberately *not* in 0.1 — the goal
of the first release is a tool that does one thing correctly, not one that does many things
approximately.

The architecture was shaped with these in mind: the agent/host split, the versioned wire format
and the interface-based host modules exist so that most of this is additive rather than a rewrite.

**Shipped since this list was written:**

- *Mismatched-free detection.* The cheapest item on the list, because the allocating entry point was
  already in every record — the work was a spare byte on the deallocation record, a comparison
  table, and a gate against the one configuration that produces false positives (see `README.md`,
  Known limits).
- *Suppression files.* `--suppressions`, with `function:`/`module:`/`file:`/`stack:` scopes, per-rule
  hit accounting, and detection of rules that have rotted into matching nothing. The design
  constraint was that suppressed leaks must be *reported* rather than dropped: a leak detector that
  can be made quiet without saying so is one nobody should trust.

---

## 0.2 — Sharper results

### Allocation-site deduplication in the agent
Today every allocation writes its full stack. Interning them in the agent (hash the frame array,
emit each distinct stack once, then reference it by id) would shrink the trace.

**This entry originally claimed "an order of magnitude". Measured, it is less.** Projecting from real
traces at `--max-frames 32`:

| Workload | Allocations | Distinct stacks | Mean depth | Frame payload | Projected saving |
|---|---:|---:|---:|---:|---:|
| synthetic, shallow stacks | 160,002 | 6 | 4.0 | 31% of trace | **1.3x** |
| `cmake` configuring a project | 178,442 | 22,339 | 30.4 | 77% of trace | **2.9x** |
| `g++` driver | 301 | 231 | 11.4 | 47% of trace | 1.0x |

The saving tracks stack *depth*, not the dedup ratio: the synthetic case dedups 26,667:1 and still
only saves 1.3x, because its stacks are four frames deep and the fixed 40 bytes of header and record
dominate. Deep C++ stacks are where it pays, and 2.9x is the honest expectation there.

That is a real win but not a free one — it needs a wire format change, a fixed-size intern table and
frame arena in the agent's BSS (no allocation is permitted there), and a fallback path for when the
table fills. Worth doing, worth doing carefully, and not worth overselling.
*Touches:* `TraceFormat` (new record type), `Agent`, `FileTraceSource`.

### Growth mode
`--growth` samples live memory periodically and reports allocation sites whose live bytes grow
monotonically. Answers "what is leaking in this long-running service", which the exit-time snapshot
cannot: a server that never exits never produces a report today.
*Touches:* agent periodic snapshots, new `analysis/GrowthAnalyzer`.

### Baseline comparison
`--baseline before.json` to report only what is new or worse. This is what makes the tool usable
on a codebase that already has leaks, instead of only on clean ones.

---

## 0.3 — Reach

### Multi-process tracing
Only the process LeakHunter launched is traced. Anything it `fork()`s is stopped by the atfork
handler, and anything it `exec()`s recognises itself as a child via `LEAKHUNTER_PID` and stays
passive.

That second half was a bug fix, not a design: before it, an exec'd child opened the same trace path
with `O_TRUNC`, destroyed the parent's flushed records, and the host went on to report *the child's*
allocations as the target's own — with `droppedRecords` at zero. On `cmake` configuring a project
that meant 1,356 allocations reported out of 178,442.

Giving each process its own `trace.<pid>` file and merging them on the host is what actually lifts
the limitation, and covers the very common fork+exec build-tool and server shapes. The identity
plumbing it needs already exists.
*Touches:* `Agent::initialize` (write `trace.<pid>`), new `tracker/MergingTraceSource`.

### Streaming instead of a file
A reader thread in the host consuming a pipe while the target runs. Removes the disk-space cost of
long runs and enables live progress output. The file path stays as the fallback, because it is what
survives a `SIGKILL`.
*Touches:* new `tracker/PipeTraceSource`, `ProcessRunner`.

### Attach to a running process
`leakhunter --pid 1234` via `ptrace` + injected `dlopen`. Significantly more invasive than
`LD_PRELOAD` and platform-specific, but it is the difference between "I can reproduce it" and "it
is happening in production right now".

---

## 0.4 — Platforms

The host code is already portable; only `ProcessRunner` and the agent are not.

### Windows
`LD_PRELOAD` has no equivalent. The plan is IAT/inline hooking of `HeapAlloc`, `HeapFree`,
`malloc` and `operator new` in a DLL injected with `CreateRemoteThread`, with `CaptureStackBackTrace`
for unwinding and DbgHelp/PDB for symbols.
*Touches:* new `src/agent/win32/`, new `process/WindowsProcessRunner`. `TraceFormat` and every host
module are unchanged — that is the point of the split.

### macOS
`DYLD_INSERT_LIBRARIES` is the direct analogue, and `malloc_zone` interposition is cleaner than
symbol interposition. Blocked in practice by System Integrity Protection for system binaries, which
must be documented rather than worked around.

---

## Later

| Item | Notes |
|---|---|
| **Reachability analysis** | Distinguish "lost" from "still reachable" by scanning globals, stacks and registers. This is the big one — it is what Valgrind and LSan do, and it is a large amount of platform-specific work. It would also change LeakHunter's identity from "simple" to "comprehensive"; worth doing only if the demand is clearly there. |
| **`mmap`/`munmap` tracking** | Catches custom allocators and large blocks that bypass libc. |
| **Sampling mode** | Record 1 in N allocations to make production tracing affordable. |
| **Flame graph output** | The grouped data is already a tree; rendering it as one is mostly presentation. |
| **Double-free detection** | Harder than the mismatched-free check that shipped, and for one specific reason: a second free of a retired address is indistinguishable from a free of something allocated before tracing began. Both land in `summary.untrackedFrees` today. Telling them apart needs a tombstone set of retired addresses — bounded memory, so it would have to be a ring buffer with an honest "beyond this horizon we cannot tell" caveat. |
| **`compile_commands.json`-aware source snippets** | Show the leaking line inline in the HTML report. |
| **CI action** | A published GitHub Action wrapping the JSON gate. |

---

## Explicit non-goals

Saying no is what keeps the tool small:

- **Being a general-purpose profiler.** No CPU sampling, no cache analysis, no allocation
  latency histograms. Those are different tools.
- **Detecting use-after-free or buffer overflows.** That is AddressSanitizer's job and it does it
  far better, with compiler support LeakHunter does not have.
- **Zero overhead.** Capturing a stack per allocation costs what it costs. `--max-frames` is the
  dial; sampling is the future answer.
- **Replacing the allocator.** LeakHunter observes; it never changes allocation behaviour, so the
  program under test behaves the way it does in production.
