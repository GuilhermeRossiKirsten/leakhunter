# ---------------------------------------------------------------------------
# pipeline98 -- the same program as poc3/, in C++98.
#
# The pair exists to show that LeakHunter's findings do not depend on the
# dialect: interception happens below the language, at malloc and operator new.
# Both leak the same 50 blocks of 256 bytes, and an integration test asserts
# that the two reports agree.
#
#   cmake -S poc4 -B poc4/build -DCMAKE_BUILD_TYPE=RelWithDebInfo
#   cmake --build poc4/build
#   leakhunter ./poc4/build/pipeline98
# ---------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(pipeline98 CXX)
    set(poc4_standalone TRUE)
else()
    set(poc4_standalone FALSE)
endif()

add_executable(pipeline98 src/pipeline.cpp)

# Set explicitly rather than via cxx_std_98: the whole point of this target is
# the standard it is compiled as, so it must not inherit one.
set_target_properties(pipeline98 PROPERTIES
    CXX_STANDARD 98
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)

target_compile_options(pipeline98 PRIVATE -g -O1 -fno-omit-frame-pointer)

if(NOT poc4_standalone)
    set_target_properties(pipeline98 PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/poc4")
endif()
