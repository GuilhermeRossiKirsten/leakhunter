# ---------------------------------------------------------------------------
# pipeline23 -- the same program as poc4/, in C++23.
#
# The pair exists to show that LeakHunter's findings do not depend on the
# dialect: interception happens below the language, at malloc and operator new.
# Both leak the same 50 blocks of 256 bytes, and an integration test asserts
# that the two reports agree.
#
#   cmake -S poc3 -B poc3/build -DCMAKE_BUILD_TYPE=RelWithDebInfo
#   cmake --build poc3/build
#   leakhunter ./poc3/build/pipeline23
# ---------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(pipeline23 CXX)
    set(poc3_standalone TRUE)
else()
    set(poc3_standalone FALSE)
endif()

# <expected> is not simply a matter of "is your compiler new enough".
#
# libstdc++ 13 gates <expected> on `__cpp_concepts >= 202002L`, and Clang 18
# reports 201907L -- it has not claimed full C++20 concepts conformance. So
# Clang 18 with libstdc++ 13 cannot compile this file even though it is newer
# than the GCC that can. Detect it rather than fail the build of the whole
# project over a demonstration.
include(CheckCXXSourceCompiles)

# Set the standard, do not pass `-std=` by hand: CMake appends the project's own
# `-std=c++20` *after* CMAKE_REQUIRED_FLAGS, so a raw flag loses and the check
# reports "no <expected>" on a toolchain that has it.
set(poc3_saved_standard "${CMAKE_CXX_STANDARD}")
set(CMAKE_CXX_STANDARD 23)
check_cxx_source_compiles("
    #include <expected>
    int main() { std::expected<int, int> e{1}; return e.value_or(0) - 1; }
" LEAKHUNTER_HAS_STD_EXPECTED)
set(CMAKE_CXX_STANDARD "${poc3_saved_standard}")

if(NOT LEAKHUNTER_HAS_STD_EXPECTED)
    message(STATUS
        "poc3 skipped: this toolchain has no usable <expected>. "
        "libstdc++ gates it on __cpp_concepts >= 202002L, which Clang does not yet report. "
        "poc4 (C++98) still builds, and every other test is unaffected.")
    return()
endif()

add_executable(pipeline23 src/pipeline.cpp)

# Set explicitly rather than via cxx_std_23: the whole point of this target is
# the standard it is compiled as, so it must not inherit one.
set_target_properties(pipeline23 PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)

target_compile_options(pipeline23 PRIVATE -g -O1 -fno-omit-frame-pointer)

if(NOT poc3_standalone)
    set_target_properties(pipeline23 PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/poc3")
endif()
