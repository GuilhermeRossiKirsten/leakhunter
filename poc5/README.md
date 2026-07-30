# `clean_app` — the negative control

Every other demonstration here is planted with a defect. **This one is not**, and that makes it the
most important of them.

A leak detector that reports something on clean code is worse than useless: people switch it off,
and then it protects nothing. So the interesting claim is not "it found the bug" — any tool that
reports everything finds every bug. It is "it found the bug **and stayed quiet about 12,000
correct allocations**".

```console
$ leakhunter ./build/poc5/clean_app
clean-app starting
  scene: 259 nodes, root parent is empty (as it should be)
  unwound 500 exceptions with live owners on the stack
  arena summarised 100890 label bytes
  3000 owned strings, 121890 bytes total
clean-app finished; every allocation was released

  LeakHunter summary
  ------------------------------------------------------------
  verdict                   PASSED  (no leaks, no mismatched frees)

  total allocations          11974  (1.88 MiB)
  total freed                11973  (1.87 MiB)
  peak live memory      244.18 KiB
  memory leaked                0 B
  leaks                          0  in 0 distinct site(s)
  runtime blocks                 1  (4.00 KiB, not listed; --include-runtime)
```

**Exit code 0.** The HTML report shows a green `PASSED — no leaks, no mismatched frees`, and
`summary.clean` is `true` in the JSON.

---

## It is not clean by being simple

11,974 allocations, through the four paths that most often go wrong.

### 1. A raw `malloc` buffer, released by a custom deleter

```cpp
struct MallocDeleter {
    void operator()(void* pointer) const noexcept { std::free(pointer); }
};
using RawBuffer = std::unique_ptr<unsigned char, MallocDeleter>;
```

The resource came from `malloc`, so it has to go back through `free`. Pairing those by hand at every
call site is exactly how the `new[]`/`free` mistake in [`poc/`](../poc/) happens. Stating the pairing
once, in the type, makes it impossible to get wrong anywhere.

### 2. A `shared_ptr` graph whose back-references are `weak_ptr`

**Smart pointers do not prevent this leak.** Two `shared_ptr`s pointing at each other keep each other
alive for ever, and LeakHunter reports that exactly like a forgotten `free` — the memory is live at
exit, which is all it claims to measure.

```cpp
class Node {
    std::vector<std::shared_ptr<Node>> children_;
    std::weak_ptr<Node> parent_;   // NOT shared_ptr
};
```

259 nodes across four levels. The asymmetry is the design: parents own, children observe. Drop the
root and the whole graph goes.

### 3. Exceptions unwound with live owners on the stack

```cpp
poc5::RawBuffer scratch = poc5::makeRawBuffer(bytes);
auto node = poc5::Node::create("transient", bytes);
throw std::runtime_error("simulated failure half way through");
```

Done 500 times. The pre-RAII version of this is the classic leak — allocate, allocate, throw, and the
first buffer is gone. Here both destructors run during unwinding.

### 4. A `std::pmr` arena over a stack buffer

`monotonic_buffer_resource` takes one upstream block and hands slices out, releasing everything at
once when it goes out of scope. 2,000 `pmr::string`s long enough to defeat the small-string
optimisation, so they really do reach the allocator.

Plus ownership moved out of a factory, through a container, and read back through a non-owning
`std::span`.

---

## What "passed" rests on

Worth reading the report rather than just the exit code:

- **`summary.mismatchDetection` is `active`.** Zero mismatched frees only means the program is clean
  when the check actually ran; it suppresses itself for targets that keep their own global
  `operator new`/`delete`. Here it ran.
- **`untrackedFrees` is 0.** Nothing was released that LeakHunter had not seen allocated, so its
  bookkeeping matched the program's from start to finish.
- **One runtime block, not listed.** glibc's stdio buffer, which it never frees by design. Counted
  separately and correctly kept out of the verdict — see [`docs/USAGE.md`](../docs/USAGE.md).

An integration test pins all of this: 0 leaks, 0 bytes, 0 mismatches, no snippets, exit 0. If a
future change makes LeakHunter fire on this program, that test fails before anyone ships it.

## Building it on its own

```console
$ cmake -S poc5 -B poc5/build -DCMAKE_BUILD_TYPE=RelWithDebInfo
$ cmake --build poc5/build
$ leakhunter ./poc5/build/clean_app        # exit 0
```

C++20, for `std::span` and `std::pmr` as used here.
