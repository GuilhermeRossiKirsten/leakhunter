# `service` — a target that never exits

A daemon-shaped program: one loop, one leak per tick, and **no way out except stopping it**.

Where [`poc/`](../poc/) shows attribution across a realistic codebase, this one shows a single
thing: LeakHunter can watch a process that is still alive, and still report when you end it.

```console
$ ./poc2/run_demo.sh
```

Or by hand — start it, watch it grow, stop it when you have seen enough:

```console
$ leakhunter ./build/poc2/service
service up (pid 1022) -- stop it with Ctrl-C, or: kill 1022
  tick 5 -- 2 KiB leaked so far
  tick 10 -- 5 KiB leaked so far
  tick 15 -- 7 KiB leaked so far
^C
[leakhunter info] stopped the target with signal 2 (Interrupt); reporting what it did up to that point

  LeakHunter summary
  ------------------------------------------------------------
  total allocations             61  (34.00 KiB)
  total freed                   45  (22.50 KiB)
  peak live memory       12.00 KiB
  memory leaked           7.50 KiB
  leaks                         15  in 1 distinct site(s)
                        (still running when you stopped it; this is everything up to that moment)
  runtime blocks                 1  (4.00 KiB, not listed; --include-runtime)

  top leak sites
      7.50 KiB  x15     (anonymous namespace)::handleRequest(unsigned long)
                        at poc2/src/service.cpp:40
                        40 |     auto* request = static_cast<char*>(std::malloc(kRequestBytes));
                           |                                                   ^
```

15 ticks, 512 bytes each, 7.5 KiB — and the 45 correctly-freed health checks do not appear.

---

## What stopping it actually does

This did not work before `poc2` existed, and finding that out is why it does now. Ctrl-C killed
LeakHunter, left the target **orphaned and still running**, and produced no report at all — the one
case a leak detector is most obviously wanted for was the one it could not serve.

Now `SIGINT` and `SIGTERM` are handled while the target runs:

```
  Ctrl-C ──▶ leakhunter's handler ──▶ kill(target, SIGINT)
                                        │
                                        ▼
                              the agent's own handler flushes the trace, re-raises
                                        │
                                        ▼
                       waitpid returns ──▶ read trace ──▶ analyse ──▶ report
```

The handler does only what is async-signal-safe: record the signal, pass it on. Everything after
that is the ordinary reporting path, which is why nothing about the analysis had to learn that a
run can end this way.

**Press it twice and you get out.** The second signal restores the default handler and re-raises, so
a wedged target can never trap you.

## Two things the report says differently

A run you ended is not a run that went wrong, and the report distinguishes them:

- **`run.stoppedByRequest: true`** in the JSON, and a plain-language line in the terminal and the
  HTML report. Without it, the reader sees an incomplete trace and reasonably concludes something
  broke.
- **No "did not shut down cleanly" warning.** A trace with no end marker normally means the target
  crashed or called `_exit()`. When *we* stopped it, that is the designed outcome, and warning about
  it would be describing your own Ctrl-C as a malfunction.

`summary.droppedRecords` is still 1, because the trace genuinely has no end marker and a consumer
gating on completeness should still see that. Read it together with `run.stoppedByRequest`.

## What it cannot tell you

**This is a snapshot, not a trend.** It reports what was live at the moment you stopped the process.
For a service the question is usually "what is *growing*", which needs periodic sampling and a
comparison over time — that is `--growth` on the [roadmap](../docs/ROADMAP.md), and it is not here.

What you can do today is take two snapshots and compare them by hand:

```console
$ leakhunter --json -o after-1min  ./build/poc2/service    # stop after a minute
$ leakhunter --json -o after-5min  ./build/poc2/service    # stop after five
$ jq -s '.[1].summary.leakedBytes - .[0].summary.leakedBytes' \
     after-1min/report.json after-5min/report.json
```

A site whose bytes scale with uptime is leaking; one that stays flat is a fixed cost.

## Building it on its own

```console
$ cmake -S poc2 -B poc2/build -DCMAKE_BUILD_TYPE=RelWithDebInfo
$ cmake --build poc2/build
$ leakhunter ./poc2/build/service
```

`handleRequest` is marked `noinline` for the demonstration: called once from a loop this small the
compiler folds it into `main`, and the report then blames `main` at the right file and line —
correct, but it reads badly. Real request handlers are not three lines long. The `volatile` store of
each buffer is load-bearing for a different reason: C++ permits eliding allocations, and without it
a compiler may delete the bug. See [docs/CONTRIBUTING.md](../docs/CONTRIBUTING.md).
