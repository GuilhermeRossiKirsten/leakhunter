# ---------------------------------------------------------------------------
# clean-app -- the negative control.
#
# Every other demonstration is planted with a defect. This one is not, which
# makes it the most important: a leak detector that reports something on clean
# code gets switched off, and then it protects nothing.
#
#   cmake -S poc5 -B poc5/build -DCMAKE_BUILD_TYPE=RelWithDebInfo
#   cmake --build poc5/build
#   leakhunter ./poc5/build/clean_app        # expect exit 0
# ---------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(clean_app CXX)
    set(poc5_standalone TRUE)
else()
    set(poc5_standalone FALSE)
endif()

add_executable(clean_app
    src/main.cpp
    src/Scene.cpp)

target_include_directories(clean_app PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")

# C++20 for std::span and std::pmr as used here.
set_target_properties(clean_app PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)

target_compile_options(clean_app PRIVATE -g -O1 -fno-omit-frame-pointer)

if(NOT poc5_standalone)
    set_target_properties(clean_app PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/poc5")
endif()
