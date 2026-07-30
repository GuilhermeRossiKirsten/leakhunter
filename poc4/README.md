# `pipeline98` — the same bug, in C++98

The identical program to [`poc3/`](../poc3/), written the way it would have been in 1998: no
`auto`, no range-for, no lambdas, no `nullptr`, no smart pointers, no move semantics. Failure comes
back as a `bool` with two out-parameters, because there is nothing else to return it with.

```console
$ leakhunter ./build/poc4/pipeline98
pipeline starting (__cplusplus = 199711L)
  250 accepted, 50 rejected, histogram total 200, 1 tag(s)
pipeline finished cleanly

  leaks                         50  in 1 distinct site(s)

  top leak sites
     12.50 KiB  x50     poc4::parseSample(std::string const&, poc4::Sample&, poc4::ParseError&)
                        at poc4/src/pipeline.cpp:53
                        53 |     out.payload = static_cast<char*>(std::malloc(kSampleBytes));
                           |                                                 ^
```

**50 leaks, 12800 bytes — the same numbers the C++23 twin produces.** The comparison table is in
[`poc3/README.md`](../poc3/README.md).

---

## What the two versions share, and what they do not

The leak is in the same place for the same reason: a buffer is allocated, a later validation fails,
and the early return takes the error code but not the buffer.

```cpp
out.payload = static_cast<char*>(std::malloc(kSampleBytes));
...
if (reading.empty()) {
    error = ParseEmptyChannel;
    return false;                 /* out.payload is still owned */
}
```

What C++23 changes is that the *failure* becomes hard to ignore — `std::expected` will not let the
caller use a value that is not there. What it does not change is the *resource*: nothing about
`std::unexpected` releases what was allocated before it. The 1998 version is more obviously
dangerous and no more broken.

For LeakHunter the two are indistinguishable. It interposes `malloc` and `operator new`; a call from
a `std::expected` error path and a call from a `bool`-returning out-parameter function are the same
call.

## Why this one is worth having

Old C++ is not a museum piece. Code compiled as C++98 or C++03 is still in production, and it is
exactly the code most likely to leak — no RAII by default, manual ownership everywhere, error paths
written before anyone had `unique_ptr` to reach for. A leak detector that only worked on modern
translation units would be aimed away from the problem.

Setting `CXX_STANDARD 98` on the target rather than inheriting the project's is deliberate: the
standard it compiles as is the point.

## Building it on its own

```console
$ cmake -S poc4 -B poc4/build -DCMAKE_BUILD_TYPE=RelWithDebInfo
$ cmake --build poc4/build
$ leakhunter ./poc4/build/pipeline98
```

`parseSample` is `noinline` in both twins so they name the same function; see
[`poc3/README.md`](../poc3/README.md).
