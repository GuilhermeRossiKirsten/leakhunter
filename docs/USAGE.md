# Usage

## Synopsis

```
leakhunter [options] <program> [program-args...]
leakhunter [options] -- <program> [program-args...]
```

The first token that does not start with `-` ends LeakHunter's own options. Everything from there
on belongs to the monitored program:

```console
$ leakhunter ./app --verbose        # --verbose goes to ./app
$ leakhunter --verbose ./app        # --verbose goes to leakhunter
$ leakhunter --verbose -- ./app --verbose   # one each, unambiguous
```

## Options

### Output

| Option | Default | Effect |
|---|---|---|
| `-o, --output <dir>` | `leakhunter-report` | Directory for the reports; created if missing |
| `--report-name <t>` | `{target}-{timestamp}` | Stem for the generated files; `{target}` and `{timestamp}` expand |
| `--html` | — | Generate `report.html` only |
| `--json` | — | Generate `report.json` only |
| *(neither)* | ✔ | Generate both |

Passing both `--html` and `--json` is the same as passing neither.

**Reports accumulate; they do not overwrite.** The default name carries the binary and the local
time, so running several targets into one directory leaves one report each:

```console
$ ls leakhunter-report/
docindex-20260730-104221.html    pipeline23-20260730-104226.html
docindex-20260730-104221.json    pipeline23-20260730-104226.json
```

Pass a fixed stem when you want a stable path — a CI job publishing one artifact, or a script that
reads the result:

```console
$ leakhunter --report-name report --json ./app     # always leakhunter-report/report.json
```

The timestamp has one-second resolution, so two runs of the same binary inside the same second do
collide. Use `--output` per run if that matters.

Nothing is deleted. The one file that *is* removed on exit is the intermediate binary trace, which
is a working artifact rather than a result; `--keep-trace` keeps it.

### Capture

| Option | Default | Effect |
|---|---|---|
| `--max-frames <n>` | `32` | Stack frames captured per allocation (max 128) |

This is the main performance dial. Unwinding dominates the cost of tracing, and the trace file
grows with it too (~65 bytes per allocation at 8 frames). Measured on 1M allocations:

| `--max-frames` | Time | Note |
|---:|---:|---|
| *(untraced)* | 0.01 s | |
| 4 | 4.8 s | libunwind build; too shallow to reach the caller in deep code |
| 8 | 1.24 s | usually enough to identify the responsible function |
| 32 | 1.53 s | the default; deep context for template-heavy code |

Lower is not always better: if the limit cuts the stack before it reaches your code, the leak gets
blamed on a library frame. `summary.truncatedTraces` in the JSON tells you when that is happening.

### Filtering

| Option | Default | Effect |
|---|---|---|
| `--min-leak-size <n>` | `0` | Omit leaks below `<n>` bytes from the listing |
| `--include-runtime` | off | Also list blocks the C runtime never frees |

Filtered leaks stay in the totals — the summary always reflects reality, only the listing shrinks.

**About `--include-runtime`:** glibc allocates buffers it deliberately never releases. A single
`printf` costs a 4 KiB stdio buffer that is live at exit in every program ever written. LeakHunter
classifies those by the module that requested them and counts them separately. Turn this on when
you are debugging the runtime itself, or when you want the raw "everything live at exit" view.

### Symbolisation

| Option | Default | Effect |
|---|---|---|
| `--no-source` | off | Skip the DWARF pass entirely (implies `--no-source-snippets`) |
| `--no-source-snippets` | snippets on | Do not read source files at all |
| `--snippet-context <n>` | `4` | Lines of source context on each side (0–32) |
| `--source-root <dir>` | — | Where to look for sources whose recorded path is not valid here. Repeatable |

By default LeakHunter looks for `llvm-symbolizer` (then `addr2line`) on `PATH` and uses it to
recover function names and `file:line`. Without it you still get names for exported symbols, but
`static` functions — most application code — will show as `<unknown>` with a raw address.

Override the tool with `LEAKHUNTER_SYMBOLIZER=/path/to/llvm-symbolizer`.

`--no-source` is worth using on very large traces where the extra process launches cost more than
the information is worth.

### Diagnostics

| Option | Effect |
|---|---|
| `-v, --verbose` | Per-stage detail on stderr, plus an agent banner from inside the target |
| `-q, --quiet` | Errors only; suppresses the stdout summary |
| `--keep-trace` | Keep the intermediate binary trace instead of deleting it |
| `--trace-file <path>` | Write the trace to an explicit path (implies `--keep-trace`) |
| `--agent <path>` | Use a specific `libleakhunter_agent.so` |
| `--no-mismatch-check` | Do not report blocks released through the wrong entry point |
| `--suppressions <file>` | Ignore leaks matching the rules in `<file>`. Repeatable — see below |
| `--strict-suppressions` | Exit 2 if any suppression rule matched nothing |
| `-h, --help` / `-V, --version` | Print and exit |

## Suppression files

`--suppressions <file>` silences leaks you have looked at and decided not to fix — a vendored
library with intentional one-time allocations, a cache you keep on purpose. The flag is repeatable
and the files are applied in order.

Rules apply to **mismatched frees as well as leaks**: vendored code with undefined behaviour you
cannot fix has to be acceptable, or the tool blocks every build and gets removed from CI.

**Suppressed findings are counted and listed separately, never dropped silently.** They leave
`leakCount`, `leakedBytes` and `mismatchedFreeCount` (that is the point — they stop counting against
you), and they appear in `summary.suppressedByRules`, in
`summary.mismatchesSuppressedByRules`, in `suppressions.applied` with a per-rule breakdown, in the
terminal summary, and in a notice at the top of the HTML report. A leak detector that can be made
quiet without saying so is one nobody should trust.

### Format

One rule per line, `<scope>:<pattern>`. Blank lines and `#` comments are ignored.

```
# Anything allocated inside the vendored parser, however deep.
stack:*/third_party/parser/*

# One specific function. The `*`s are needed because the demangled name carries
# the namespace and the parameter list.
function:*Cache::warmUp*

# Everything from one shared object.
module:*/libthirdparty.so*

# Everything from one source tree.
file:*/generated/*
```

| Scope | Matched against |
|---|---|
| `function` | the **blamed** frame's function name |
| `module` | the **blamed** frame's object file |
| `file` | the **blamed** frame's source path |
| `stack` | the function, module **or** source path of **any** frame in the stack |

`*` matches any sequence of characters **including `/`**, and `?` matches exactly one. There are no
character classes and no `**`; matching is case-sensitive. The first rule that matches wins, and
that rule gets the hit.

### `function:` versus `stack:`

`function:`, `module:` and `file:` look only at the frame the report blames, so a rule written for
one function cannot silently swallow its callers. `stack:` reaches every frame, which is what
actually solves "I do not care about anything allocated under this library" — and it is easy to
over-apply:

> `stack:*myapp*` matching your own binary's module means *anything whose stack passes through your
> binary*, which is every allocation the program made, including ones libc made on its behalf.
> Prefer `function:` when you can name the site.

### Rules that have rotted

A rule that matches nothing is worse than no rule: it looks like coverage that is not there. The
function was renamed, the library was dropped, the pattern had a typo. LeakHunter warns about each
one and lists them in `suppressions.unused`:

```console
[leakhunter warning] suppression rule matched nothing: leaks.supp:6: function:*oldName*
```

`--strict-suppressions` promotes that to **exit code 2**, so a project that has decided to keep its
suppression file honest can enforce it in CI.

### Malformed rules are fatal

A missing colon, an unknown scope or an empty pattern stops the run **before the target is
launched**, with the file and line:

```console
$ leakhunter --suppressions leaks.supp ./app
leakhunter: leaks.supp:2: unknown scope 'functoin'. Known scopes: function, module, file, stack.
```

A typo in a suppression file silently changes what the tool reports. Finding that out from a CI run
that passed for the wrong reason is far worse than a startup failure.

## Source snippets

By default the reports show the **blamed line itself**, read out of your source tree:

```
  200.00 KiB  x100    poc::(anonymous namespace)::indexBatch(unsigned long)
                      at poc/src/IndexWorker.cpp:27
                      27 |     auto* scratch = static_cast<unsigned char*>(std::malloc(kScratchBytes));
                         |                                                            ^
```

The HTML report shows a window around it with that line highlighted; expand a row to see it.

The caret sits on the exact column when `llvm-symbolizer` provided one. `addr2line` does not report
columns, so there the whole line is underlined instead — accurate about how much it knows.

> **The HTML report will contain excerpts of your source code.**
>
> That is usually what you want, and it is why this is on by default. But it changes what the
> artifact is: attaching `report.html` to a ticket, or publishing it as a CI artifact, now shares
> code. `--no-source-snippets` turns it off entirely.
>
> There is a second, narrower consideration. Source paths come from the traced binary's debug info,
> so a binary built from a forged DWARF path could name a file you did not intend to embed. If you
> are tracing something you did not build, `--source-root <dir>` restricts what can be found, and
> `--no-source-snippets` removes the question.

### When the source is somewhere else

A report generated on a different machine from the build has recorded paths that do not exist. Point
`--source-root` at the tree and LeakHunter matches progressively shorter suffixes of the recorded
path against it, the way `gdb set substitute-path` does:

```console
# The binary was built at /build/agent/work/1/s, the source is here.
$ leakhunter --source-root ~/projects/myapp --json ./myapp
```

Repeat the flag for a monorepo with several trees. If nothing matches, the site simply has no
snippet and LeakHunter says how many files it could not find.

## Diagnostics for your editor and for CI

`--diagnostics` writes compiler-style lines to **stderr**, which is what an editor's error parser
reads:

```console
$ leakhunter --diagnostics ./build/poc/docindex
poc/src/IndexWorker.cpp:27:60: warning: leak: 100 block(s) leaked here, 200.00 KiB in total, by ... [leakhunter:leak]
poc/src/DocumentCache.cpp:67:49: warning: mismatched-free: 8 block(s) allocated with new[] here, released with free() -- undefined behaviour (4.00 KiB affected) [leakhunter:mismatched-free]
```

In vim that is `:cfile`-able; in VS Code it populates the Problems pane. Findings at the same
location are collapsed into one line with a count, the way a compiler would.

Inside GitHub Actions (`$GITHUB_ACTIONS` set) the format switches automatically to workflow
commands, which annotate the pull-request diff on the offending line:

```yaml
- run: leakhunter --diagnostics --json --output artifacts ./build/tests
```

```
::warning file=src/IndexWorker.cpp,line=27,col=60,title=leakhunter leak::100 block(s) leaked here...
```

No flag needed for that: the useful choice in CI is never the one you remembered to pass.

## Long-running targets

A service does not exit, so **you** end the run. `Ctrl-C` (or `kill` on the leakhunter process)
stops the target and produces the report for everything it did up to that moment:

```console
$ leakhunter ./my-daemon
  ... runs ...
^C
[leakhunter info] stopped the target with signal 2 (Interrupt); reporting what it did up to that point
```

The signal is forwarded to the target so its agent can flush; the normal reporting path then runs.
**Press it twice to force-quit** — the second signal restores the default handler, so a wedged
target cannot trap you.

The report marks this: `run.stoppedByRequest` is `true`, and the summary says so in plain language.
`summary.droppedRecords` will be 1, because the trace genuinely has no end marker — read the two
together before treating the run as incomplete.

**This is a snapshot, not a trend.** It says what was live when you stopped it, not what is
*growing*. For that, take two runs of different lengths and compare `summary.leakedBytes`; a site
that scales with uptime is leaking, one that stays flat is a fixed cost. Sampled growth analysis is
on the [roadmap](ROADMAP.md).

## Multi-process targets

**Only the process LeakHunter launches is traced.** A child it `fork()`s is stopped by an atfork
handler; a child it `exec()`s sees `LEAKHUNTER_PID` in its environment, notices that its own pid
differs, and stays passive.

That matters more than it sounds, because it decides what you should point the tool at:

```console
$ leakhunter ./run-tests.sh     # traces the SHELL. Almost certainly not what you want.
$ leakhunter ./build/my_tests   # traces the test binary.
$ leakhunter g++ -c foo.cpp     # traces the g++ DRIVER; cc1plus does the real work, untraced.
```

If the target is a wrapper, find the binary it ends up running and trace that instead. With
`--verbose` the agent says so from inside each child it declines to trace:

```console
[leakhunter agent] pid 4123 is a child of the traced process 4120; not tracing
```

Tracing every process in a tree is on the [roadmap](ROADMAP.md); it needs a trace file per process
and a merging reader on the host.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | Target ran, nothing found |
| 1 | Target ran, leaks and/or mismatched frees found |
| 2 | Invalid arguments, an unreadable/malformed suppression file, or a rotted rule under `--strict-suppressions` |
| 3 | The target could not be started |
| 4 | Internal error (tracing or report generation failed) |

The target's own exit code is preserved in the report (`run.exitCode`), not in LeakHunter's — the
two answer different questions.

## Recipes

### Fail a CI build on any leak

```yaml
- run: leakhunter --json --output artifacts ./build/tests
```

Exit code 1 fails the step, and `artifacts/report.json` is your build artifact. Note that a
mismatched free also fails the step, even when nothing leaked — it is undefined behaviour, and a
gate that lets it through is not much of a gate.

### Fail only on leaks above a threshold

```console
$ leakhunter --json -o reports ./app
$ jq -e '.summary.leakedBytes < 1048576' reports/report.json
```

### Gate on undefined behaviour but tolerate leaks

Useful while adopting the tool on a codebase that already leaks: mismatched frees are almost always
worth fixing immediately, whereas a leak backlog takes time.

```console
$ leakhunter --json -o reports ./app || true
$ jq -e '.summary.mismatchedFreeCount == 0' reports/report.json
```

### Verify that the mismatch check actually ran

It is suppressed automatically for targets that define their own global `operator new` or
`operator delete`, so a zero count is only meaningful alongside `mismatchDetection`:

```console
$ jq -r '.summary | "\(.mismatchDetection): \(.mismatchedFreeCount)"' reports/report.json
active: 0
```

### Compare two runs

```console
$ leakhunter --json -o before ./app
$ git switch feature && cmake --build build
$ leakhunter --json -o after ./app
$ diff <(jq -S '.groups|map({function,totalBytes})' before/report.json) \
       <(jq -S '.groups|map({function,totalBytes})' after/report.json)
```

### Trace a program that needs arguments and environment

```console
$ CONFIG=prod leakhunter --max-frames 64 -o reports -- ./server --port 8080
```

The environment is inherited; LeakHunter only adds `LD_PRELOAD` and its own `LEAKHUNTER_*`
variables. An existing `LD_PRELOAD` is preserved and appended to, not replaced.

### Look at only the biggest offenders

```console
$ leakhunter --min-leak-size 4096 -o reports ./app
```

### Inspect the raw trace

```console
$ leakhunter --trace-file /tmp/app.lhtrace -o reports ./app
```

The format is documented in `include/leakhunter/ipc/TraceFormat.hpp`.

## Environment variables

| Variable | Read by | Purpose |
|---|---|---|
| `LEAKHUNTER_AGENT` | host | Path to the agent library; overrides discovery |
| `LEAKHUNTER_SYMBOLIZER` | host | Symbolizer to use instead of auto-detection |
| `LEAKHUNTER_TRACE` | agent | Trace output path — **set by the host**, do not set manually |
| `LEAKHUNTER_MAX_FRAMES` | agent | Set by the host from `--max-frames` |
| `LEAKHUNTER_VERBOSE` | agent | Set by the host from `--verbose` |

The agent is inert unless `LEAKHUNTER_TRACE` is set, so leaving it in `LD_PRELOAD` is harmless.

## Troubleshooting

### "trace file is empty — the target produced no allocation data"

The agent never ran, or ran and intercepted nothing.

- **Statically linked target.** `ldd ./app` reporting "not a dynamic executable" means
  `LD_PRELOAD` has nothing to interpose on. LeakHunter cannot help; rebuild dynamically.
- **The target re-execs or is a shell wrapper.** Point LeakHunter at the real binary.
- **A setuid/setgid binary.** The loader ignores `LD_PRELOAD` for those, by design.

### Function names show as `<unknown>+0x9049`

No symbolizer on `PATH`, or the target has no debug info. Install LLVM
(`apt install llvm` / `dnf install llvm`) or build the target with `-g`. Check what was found with
`--verbose`.

The `+0x9049` is the offset within the module named on the line below, and it is exact — you can
hand it straight to `addr2line -e <module> 0x9049` or to a disassembler.

### A name looks wrong — the function it blames makes no sense

You are looking at a stripped binary. Without debug info, symbolisation falls back to the symbol
table and returns the nearest *exported* symbol, which can be some distance from the code that
actually allocated.

LeakHunter marks these: the name is shown with its offset (`_obstack_memory_used+0x34`, or bare
when even that is unknown), the location line reads `module+0xoffset` rather than `file:line`, and
`preciseName` is `false` in the JSON. Treat the name as a hint and the offset as the fact.

### "N records were dropped by the agent"

The trace could not be written completely — usually a full disk. The report is a **lower bound**
on the real leaks. Free space, or use `--trace-file` to point at a bigger volume.

### "trace has no end marker"

The target crashed, was killed, or called `_exit()`, so the agent never ran its clean shutdown.

This is handled, not fatal: a fatal-signal handler flushes the buffer on the way out, `_exit` is
interposed to do the same, and the module map written at start-up means the recovered addresses
still resolve to function names and line numbers. What you lose is anything allocated after the
last flush, plus the `dladdr` symbol table — which the DWARF pass covers for you.

`summary.droppedRecords` is non-zero for these runs, so treat the counts as a **lower bound**. If
the target was `SIGKILL`ed there is nothing to be done: the kernel does not let a process react.

### The program is much slower under LeakHunter

Expected — every allocation now walks the stack. Reduce `--max-frames`, add `--no-source` to skip
the symbolizer pass, or trace a smaller workload.

If you built with `-DLEAKHUNTER_WITH_LIBUNWIND=ON`, try without it: on the benchmark in this
repository libunwind is 3–6x slower than the compiler runtime for identical results.
