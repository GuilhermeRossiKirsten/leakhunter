# Sample reports

Real output, with paths rewritten to `/home/dev/myapp` so the files are reproducible.

| File | Generated from | Shows |
|---|---|---|
| `report.html` / `report.json` | [`multiple_leaks.cpp`](../multiple_leaks.cpp) | 111 leaks across 3 sites |
| `report-mismatched-free.html` / `.json` | [`mismatched_free.cpp`](../mismatched_free.cpp) | 0 leaks, 4 mismatched frees |

```console
$ leakhunter --output sample-report ./build/multiple_leaks
$ leakhunter --output sample-report ./build/mismatched_free
```

The HTML files are single-file and self-contained — no CDN, no assets directory. They work
offline, from an email attachment, or as a CI artifact. The JSON schema is documented in
[`docs/REPORT_FORMAT.md`](../../docs/REPORT_FORMAT.md).

The program leaks from three sites and also allocates 5000 blocks it frees correctly. The report
shows exactly the three:

| Bytes leaked | Count | Function |
|---:|---:|---|
| 1.00 MiB | 1 | `leakOne()` at `multiple_leaks.cpp:31` |
| 40.00 KiB | 10 | `leakMedium()` at `multiple_leaks.cpp:26` |
| 6.25 KiB | 100 | `leakSmall()` at `multiple_leaks.cpp:20` |

`allocateAndFree()` does not appear — that is the assertion the corresponding integration test
makes, and it is the property that matters most in a leak detector.

In the HTML report, clicking a row expands its stack trace; the highlighted frame is the one
blamed for the allocation. The table sorts by any column and filters by function, module or file.

## The mismatched-free report

`report-mismatched-free.*` comes from a program that **leaks nothing** — every block is returned —
and still fails, with exit code 1:

| Allocated with | Released with | At |
|---|---|---|
| `new` | `free()` | `mismatched_free.cpp:32` |
| `new[]` | `delete` | `mismatched_free.cpp:41` |
| `new[]` | `free()` | `mismatched_free.cpp:48` |
| `malloc()` | `delete` | `mismatched_free.cpp:55` |

The stack shown for each is the **allocation** site, which is what identifies the object whose
`delete` is wrong. `summary.mismatchDetection` reads `active`, meaning the check actually ran —
worth reading, because it is automatically suppressed for targets that keep their own global
`operator new`/`delete`.
