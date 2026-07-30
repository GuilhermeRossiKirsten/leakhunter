# ---------------------------------------------------------------------------
# service -- the long-running demonstration target.
#
# A program that never exits on its own, so that stopping it is part of the
# demonstration rather than an accident.
#
#   cmake -S poc2 -B poc2/build -DCMAKE_BUILD_TYPE=RelWithDebInfo
#   cmake --build poc2/build
#   leakhunter ./poc2/build/service        # then Ctrl-C
# ---------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(service CXX)
    set(poc2_standalone TRUE)
else()
    set(poc2_standalone FALSE)
endif()

add_executable(service src/service.cpp)
target_compile_features(service PRIVATE cxx_std_20)
target_compile_options(service PRIVATE -g -O1 -fno-omit-frame-pointer)

find_package(Threads REQUIRED)
target_link_libraries(service PRIVATE Threads::Threads)

if(NOT poc2_standalone)
    set_target_properties(service PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/poc2")
endif()
