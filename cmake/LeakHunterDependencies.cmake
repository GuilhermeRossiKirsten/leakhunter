# Dependency resolution for LeakHunter.
#
# Policy: prefer system packages, fall back to FetchContent so a fresh clone
# builds with a single `cmake -B build && cmake --build build`.
# Set LEAKHUNTER_FORCE_FETCH=ON to always fetch (useful in CI reproducibility).

include(FetchContent)

option(LEAKHUNTER_FORCE_FETCH "Ignore system packages and fetch dependencies" OFF)

set(FETCHCONTENT_QUIET OFF)

function(_leakhunter_require_package name version gitUrl gitTag)
    if(NOT LEAKHUNTER_FORCE_FETCH)
        find_package(${name} ${version} QUIET)
        if(${name}_FOUND)
            message(STATUS "LeakHunter: using system ${name}")
            return()
        endif()
    endif()

    message(STATUS "LeakHunter: fetching ${name} (${gitTag})")
    FetchContent_Declare(${name}
        GIT_REPOSITORY ${gitUrl}
        GIT_TAG        ${gitTag}
        GIT_SHALLOW    TRUE
        EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(${name})
endfunction()

# ---------------------------------------------------------------------------
# nlohmann/json -- report serialisation (host side only)
# ---------------------------------------------------------------------------
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
_leakhunter_require_package(nlohmann_json 3.11
    "https://github.com/nlohmann/json.git" "v3.11.3")

# ---------------------------------------------------------------------------
# fmt -- string formatting (host side only)
# ---------------------------------------------------------------------------
set(FMT_INSTALL OFF CACHE INTERNAL "")
set(FMT_TEST OFF CACHE INTERNAL "")
_leakhunter_require_package(fmt 9.0
    "https://github.com/fmtlib/fmt.git" "10.2.1")

# ---------------------------------------------------------------------------
# spdlog -- diagnostics logging (host side only), always on top of external fmt
# ---------------------------------------------------------------------------
set(SPDLOG_FMT_EXTERNAL ON CACHE INTERNAL "")
set(SPDLOG_INSTALL OFF CACHE INTERNAL "")
set(SPDLOG_BUILD_EXAMPLE OFF CACHE INTERNAL "")
set(SPDLOG_BUILD_TESTS OFF CACHE INTERNAL "")
_leakhunter_require_package(spdlog 1.11
    "https://github.com/gabime/spdlog.git" "v1.13.0")

# ---------------------------------------------------------------------------
# Stack unwinding inside the injected agent -- a compile-time choice.
#
#   * _Unwind_Backtrace (default) - from libgcc/compiler-rt, always present,
#     DWARF CFI based, no extra runtime dependency in the target process.
#   * libunwind (opt-in) - same DWARF data, richer API, handles cases the
#     compiler runtime does not (signal frames, JIT-registered unwind info).
#
# The default is NOT the "fallback": measured on this project's benchmark
# (glibc 2.39, libunwind 1.6.2, x86-64), both produce byte-identical
# attribution, but _Unwind_Backtrace is substantially faster:
#
#     1M allocations, single thread,  8 frames:  1.24 s  vs  7.16 s
#     2M allocations, 8 threads,      8 frames:  2.45 s  vs  8.54 s
#
# Since tracing cost is dominated by unwinding, that decides the default.
# Turn LEAKHUNTER_WITH_LIBUNWIND on if you need what libunwind handles better.
# ---------------------------------------------------------------------------
option(LEAKHUNTER_WITH_LIBUNWIND
       "Unwind with libunwind instead of the compiler runtime (slower, more capable)" OFF)

if(NOT LEAKHUNTER_BUILD_AGENT)
    set(LEAKHUNTER_UNWINDER "n/a (agent disabled)" CACHE INTERNAL "")
elseif(LEAKHUNTER_WITH_LIBUNWIND)
    find_package(Libunwind REQUIRED)
    set(LEAKHUNTER_UNWINDER "libunwind (requested)" CACHE INTERNAL "")
else()
    set(LEAKHUNTER_UNWINDER "_Unwind_Backtrace" CACHE INTERNAL "")
endif()
