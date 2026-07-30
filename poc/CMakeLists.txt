# ---------------------------------------------------------------------------
# docindex -- the demonstration target.
#
# Buildable two ways:
#
#   * as part of LeakHunter (it is, by default -- see LEAKHUNTER_BUILD_POC);
#   * standalone, which is how you would build any other project:
#
#       cmake -S poc -B poc/build -DCMAKE_BUILD_TYPE=RelWithDebInfo
#       cmake --build poc/build
#       leakhunter ./poc/build/docindex
#
# Built with -g and frame pointers, which is all LeakHunter needs to report
# file:line. It does NOT need instrumentation, sanitizers or a special build
# type -- that is the point of the demonstration.
# ---------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.20)

# Standalone or subproject? The answer only changes where the binary lands.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(docindex CXX)
    set(poc_standalone TRUE)
else()
    set(poc_standalone FALSE)
endif()

add_executable(docindex
    src/main.cpp
    src/RecordParser.cpp
    src/DocumentCache.cpp
    src/ReportBuilder.cpp
    src/IndexWorker.cpp)

target_include_directories(docindex PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(docindex PRIVATE cxx_std_20)

# -O1 rather than -O2: enough that the code is realistic, little enough that the
# stacks stay readable. -fno-omit-frame-pointer is not required (LeakHunter
# unwinds with DWARF CFI) but it makes the demo's traces tidier.
target_compile_options(docindex PRIVATE -g -O1 -fno-omit-frame-pointer)

find_package(Threads REQUIRED)
target_link_libraries(docindex PRIVATE Threads::Threads)

# Inside the LeakHunter build, keep it out of build/bin next to the real tools.
# Standalone, leave it at the top of its own build directory, which is where
# anyone would look for it.
if(NOT poc_standalone)
    set_target_properties(docindex PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/poc")
endif()

# Deliberately no install() rule: this is a demonstration, not a deliverable.
