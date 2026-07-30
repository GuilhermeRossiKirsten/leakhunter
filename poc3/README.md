# `pipeline23` — the same bug, in C++23

A telemetry pipeline written with `std::expected`, ranges, `static operator()`, multidimensional
`operator[]`, `std::to_underlying`, `if consteval`, `auto(x)` and `uz` literals.

Its twin, [`poc4/`](../poc4/), is the identical program in **C++98**. Together they answer one
question: does LeakHunter care what standard your code is written in?

```console
$ leakhunter ./build/poc3/pipeline23
$ leakhunter ./build/poc4/pipeline98
```

| | C++23 | C++98 |
|---|---:|---:|
| `__cplusplus` | `202100L` | `199711L` |
| Total allocations | 303 | 305 |
| **Leaks** | **50** | **50** |
| **Bytes leaked** | **12800** | **12800** |
| Blamed on | `parseSample` | `parseSample` |

**No, it does not care.** Interception happens at `malloc` and `operator new`, below anything the
language does. The allocation counts differ by two because the two standard libraries do slightly
different work around the same program; the *leak* is identical, because it is the same leak.

---

## The bug, and why modern syntax does not prevent it

`parseSample` returns `std::expected<Sample, ParseError>`. The failure is explicit, typed and
impossible to ignore — the caller cannot use the value without checking. And it still leaks:

```cpp
sample.payload = static_cast<char*>(std::malloc(kSampleBytes));
...
if (text.substr(colon + 1).empty()) {
    return std::unexpected(ParseError::EmptyChannel);   // payload is still owned
}
```

`std::unexpected` returns as cleanly as anything and takes nothing with it. The caller receives an
error code and has no idea a buffer was allocated, let alone a handle to free it.

[`poc4/src/pipeline.cpp`](../poc4/src/pipeline.cpp) has the same line with a `bool` return and an
out-parameter. Twenty-five years of language design between them, and the ownership mistake is
untouched — because it was never a syntax problem.

The fix in both is the same: a destructor. `std::unique_ptr<char[]>`, or `Sample` owning its own
cleanup. What `std::expected` buys is that the *error* is hard to ignore; it makes no claim about
the *resource*, and it is worth being clear-eyed about which problem a tool solves.

## Which C++23 features are used

Newer things (`std::print`, `std::mdspan`, `std::generator`, deducing `this`) need GCC 14+ and are
deliberately absent rather than optimistically included.

| Feature | Where |
|---|---|
| `std::expected` / `std::unexpected` | `parseSample` — the error path that leaks |
| `static operator()` | `ChannelIsInteresting` |
| Multidimensional `operator[]` | `Histogram::operator[](row, column)` |
| `std::to_underlying` | `codeOf` |
| `if consteval` | `payloadBudget` |
| `auto(x)` decay-copy | the predicate call in `main` |
| `uz` literal suffix | the histogram indices |
| `std::string::contains` | the tag filter |
| Ranges views | the tag filter |

## This target does not build with Clang 18, and that is not a version problem

Building the pair across compilers turned up something worth stating plainly, because the obvious
explanation is wrong.

`libstdc++` 13 gates `<expected>` on:

```cpp
#if __cplusplus > 202002L && __cpp_concepts >= 202002L
```

GCC 13.3 reports `__cpp_concepts = 202002L`. **Clang 18.1 reports `201907L`** — it has not claimed
full C++20 concepts conformance — so `std::expected` is invisible to it, even though Clang 18 is
newer than GCC 13 and implements more of C++23 in the compiler itself. It is a library
conformance gate, not "your compiler is too old".

Rather than fail the whole project's build over a demonstration, `poc3/CMakeLists.txt` probes for a
usable `<expected>` at configure time and skips the target with an explanation:

```
-- poc3 skipped: this toolchain has no usable <expected>. libstdc++ gates it on
   __cpp_concepts >= 202002L, which Clang does not yet report. poc4 (C++98) still
   builds, and every other test is unaffected.
```

The `poc4` twin and the whole test suite are unaffected — which is the useful behaviour, since the
thing being demonstrated is LeakHunter, not `<expected>`.

> A detail worth knowing if you write such a probe yourself: passing `-std=c++23` through
> `CMAKE_REQUIRED_FLAGS` does **not** work when the enclosing project sets `CMAKE_CXX_STANDARD`.
> CMake appends its own `-std=` *after* yours, so the project's standard wins and the check reports
> a false negative. Set `CMAKE_CXX_STANDARD` around the check instead. That mistake cost a
> configuration here before it was spotted.

## Building it on its own

```console
$ cmake -S poc3 -B poc3/build -DCMAKE_BUILD_TYPE=RelWithDebInfo
$ cmake --build poc3/build
$ leakhunter ./poc3/build/pipeline23
```

`CXX_STANDARD 23` is set on the target explicitly rather than inherited: the standard it compiles as
*is* the point of this target.

`parseSample` is `noinline` in both twins so they name the same function. Without it the compiler
folds it into `main` and the report blames `main` at the right line — correct, and useless for a
side-by-side comparison.
