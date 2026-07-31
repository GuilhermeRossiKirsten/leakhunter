# ---------------------------------------------------------------------------
# thread_storm -- eight threads leaking at once: attribution and arithmetic under load.
#
# Built standalone, like any other project: nothing here links against
# LeakHunter or includes a header of ours.
#
#   cmake -S poc9 -B poc9/build -DCMAKE_BUILD_TYPE=RelWithDebInfo
#   cmake --build poc9/build
#   leakhunter ./poc9/build/thread_storm
#
# The expected numbers are fixed by the constants in src/main.cpp and are
# checked against the report in docs/VALIDATION.md.
# ---------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(thread_storm CXX)
    set(poc9_standalone TRUE)
else()
    set(poc9_standalone FALSE)
endif()

add_executable(thread_storm src/main.cpp)

set_target_properties(thread_storm PROPERTIES
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
target_compile_options(thread_storm PRIVATE -g -O1 -fno-omit-frame-pointer -fno-builtin)

find_package(Threads REQUIRED)
target_link_libraries(thread_storm PRIVATE Threads::Threads)

if(NOT poc9_standalone)
    set_target_properties(thread_storm PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/poc9")
endif()
