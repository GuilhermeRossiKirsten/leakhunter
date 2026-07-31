# ---------------------------------------------------------------------------
# realloc_chain -- realloc chains: the one entry point that both frees and allocates.
#
# Built standalone, like any other project: nothing here links against
# LeakHunter or includes a header of ours.
#
#   cmake -S poc6 -B poc6/build -DCMAKE_BUILD_TYPE=RelWithDebInfo
#   cmake --build poc6/build
#   leakhunter ./poc6/build/realloc_chain
#
# The expected numbers are fixed by the constants in src/main.cpp and are
# checked against the report in docs/VALIDATION.md.
# ---------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(realloc_chain CXX)
    set(poc6_standalone TRUE)
else()
    set(poc6_standalone FALSE)
endif()

add_executable(realloc_chain src/main.cpp)

set_target_properties(realloc_chain PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)

# -O1 with frame pointers: optimised enough to be a realistic target, but the
# stacks stay walkable and the planted defects survive. -O2 lets both compilers
# delete allocations whose results are unobservable, which would quietly change
# the numbers this demonstration exists to pin down.
# -fno-builtin is load-bearing, not decoration.
#
# The standard permits a compiler to remove an allocation whose result is
# unobservable (C++ N3664), and Clang does it aggressively: without this flag it
# deleted 25 of poc6's calloc leaks and *every one* of poc8's, so the binary no
# longer did what its source said and the report was correct about the wrong
# program. Storing the pointer in a volatile is not enough to stop it. This flag
# says: treat malloc/calloc/realloc/operator new as opaque calls.
target_compile_options(realloc_chain PRIVATE -g -O1 -fno-omit-frame-pointer -fno-builtin)

if(NOT poc6_standalone)
    set_target_properties(realloc_chain PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/poc6")
endif()
