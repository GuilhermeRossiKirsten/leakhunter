# ---------------------------------------------------------------------------
# exception_paths -- leaks on the path an exception takes, next to the RAII version.
#
# Built standalone, like any other project: nothing here links against
# LeakHunter or includes a header of ours.
#
#   cmake -S poc8 -B poc8/build -DCMAKE_BUILD_TYPE=RelWithDebInfo
#   cmake --build poc8/build
#   leakhunter ./poc8/build/exception_paths
#
# The expected numbers are fixed by the constants in src/main.cpp and are
# checked against the report in docs/VALIDATION.md.
# ---------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(exception_paths CXX)
    set(poc8_standalone TRUE)
else()
    set(poc8_standalone FALSE)
endif()

add_executable(exception_paths src/main.cpp)

set_target_properties(exception_paths PROPERTIES
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
target_compile_options(exception_paths PRIVATE -g -O1 -fno-omit-frame-pointer -fno-builtin)

if(NOT poc8_standalone)
    set_target_properties(exception_paths PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/poc8")
endif()
