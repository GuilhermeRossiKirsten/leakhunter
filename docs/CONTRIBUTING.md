# Contributing

## Getting set up

```console
$ sudo apt install cmake g++ llvm libunwind-dev     # Debian/Ubuntu
$ cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLEAKHUNTER_WERROR=ON
$ cmake --build build -j
$ ctest --test-dir build --output-on-failure
```

`libunwind-dev` and `llvm` are optional — the build falls back to `_Unwind_Backtrace` and to
dladdr-only symbols — but you want both when working on the agent or the symbol resolver, since
that is what most users will have.

`-DLEAKHUNTER_WERROR=ON` is what CI uses. Turn it on locally so you find warnings before CI does.

## Layout

`include/leakhunter/<module>/` and `src/<module>/` mirror each other. One class per file, named
after the class.

`src/agent/` is different and its headers stay private: that code runs inside somebody else's
process. Read [ARCHITECTURE.md](ARCHITECTURE.md) before touching it.

## Rules for `src/agent/`

These are not style preferences. Breaking any of them produces crashes in *the user's* program
that are extremely hard to attribute back to us.

1. **Never allocate on a hot path.** No `std::vector`, `std::string`, `std::function`, iostreams,
   `printf`. Fixed buffers and raw syscalls only.
2. **Guard every entry point** with `HookGuard` and check `engaged()` before recording.
3. **No static objects with constructors or destructors.** State goes in BSS and is initialised
   explicitly; `Agent` is `constinit` and that is enforced by the compiler.
4. **Assume every function you call may re-enter you.** `dlsym`, `dladdr` and libunwind all can.
5. **Degrade, never abort.** A failure counts a dropped record and carries on. Killing the user's
   process because our trace file is full is not acceptable.

If a change to the agent cannot be made within these rules, it probably belongs on the host side.

## Rules for the host

Ordinary modern C++20. RAII, smart pointers, interfaces for anything with more than one
implementation or anything a test needs to fake.

- Recoverable failures return `Result<T>` / `Status`. Exceptions are for genuinely exceptional
  conditions and are caught in `main`.
- Modules depend downward only. If `AllocationRegistry` needs to know about symbols, the design is
  wrong.
- Anything a test needs to substitute gets an interface (`IProcessRunner`, `ISymbolResolver`,
  `ITraceSource`, `IReportGenerator`).

## The wire format

`include/leakhunter/ipc/TraceFormat.hpp` is the contract between agent and host.

- Changing a struct layout is a **breaking change**: bump `kFormatVersion`.
- Adding a new `RecordType` is **not** breaking — readers skip unknown types using
  `RecordHeader::payloadBytes`. Prefer this.
- Only `<cstdint>` may be included there. Nothing else.
- The `static_assert`s at the bottom exist to catch accidental layout changes. If one fires, that
  is the point.

## Tests

Every change needs one. Unit tests for host logic, integration tests for anything that touches
interception.

Adding an integration case is two steps:

```cpp
// examples/my_case.cpp  -- document the expected answer in a comment
```
```cmake
# examples/CMakeLists.txt
leakhunter_add_example(my_case)

# tests/CMakeLists.txt
leakhunter_add_integration_test(integration_my_case my_case
    EXPECT "EXPECT_LEAK_COUNT=2"
           "EXPECT_FUNCTION=leakingFunction"
           "EXPECT_ABSENT=cleanFunction")
```

**Write the absence assertions.** `EXPECT_ABSENT` is what catches false positives, and a leak
detector with false positives gets switched off.

### Make the allocations observable

This one has bitten twice, in the demo and in a benchmark harness, and it is easy to miss:

```cpp
for (int i = 0; i < 1000; ++i) { (void)std::malloc(64); }   // may allocate NOTHING
```

C++ permits eliding allocations, and both GCC and Clang do it. A test program whose allocations are
not observable can be compiled down to nothing, and the "failure" then looks like a bug in
LeakHunter rather than in the test. Clang at `-O1` removed 500 allocations from `poc/` this way,
turning 700 leaks into 600.

Store the pointer somewhere observable — a container, a struct that outlives the call, or a
`volatile` sink:

```cpp
void* volatile g_sink = nullptr;
g_sink = std::malloc(64);
```

`-Wunused-result` is the compiler telling you in advance.

### Both compilers

Before a release, build with each. They disagree in ways that matter: different inlining changes
which enclosing function is blamed, and one of them may accept a warning flag the other rejects
under `-Werror`.

```console
$ cmake -B build-gcc   -DLEAKHUNTER_WERROR=ON
$ cmake -B build-clang -DLEAKHUNTER_WERROR=ON -DCMAKE_CXX_COMPILER=clang++
```

## Style

`.clang-format` is authoritative — run it before committing.

- 4 spaces, 100 columns, `PascalCase` types, `camelCase` functions and variables, `kCamelCase`
  constants, `trailing_` underscore on private members.
- Comment **why**, not what. `// increment the counter` is noise; `// dlsym allocates, so the
  first call re-enters us` is the reason the next person does not delete the line.
- Every non-obvious design decision belongs in a comment at the point of the decision, and in
  ARCHITECTURE.md if it shapes more than one file.

## Pull requests

1. `ctest --test-dir build --output-on-failure` passes.
2. `-DLEAKHUNTER_WERROR=ON` builds clean.
3. New behaviour has a test; changed behaviour has an updated test.
4. Docs updated if you changed the CLI, the JSON schema, or the architecture.
5. The commit message explains *why*.

Bug reports are most useful with the output of `leakhunter --verbose --keep-trace`, your compiler
and distro version, and whether `libunwind` and `llvm-symbolizer` were present.
