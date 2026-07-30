cmake_minimum_required(VERSION 3.20)

project(LeakHunter
    VERSION 0.1.0
    DESCRIPTION "Lightweight memory leak detector for C++ applications"
    HOMEPAGE_URL "https://github.com/leakhunter/leakhunter"
    LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# Everything lands in a predictable place: the CLI resolves the agent library
# relative to its own executable path (see core/AgentLocator).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(LEAKHUNTER_AGENT_SUPPORTED ON)
else()
    set(LEAKHUNTER_AGENT_SUPPORTED OFF)
endif()

option(LEAKHUNTER_BUILD_AGENT    "Build the LD_PRELOAD interception agent" ${LEAKHUNTER_AGENT_SUPPORTED})
option(LEAKHUNTER_BUILD_TESTS    "Build the test suite"                    ON)
option(LEAKHUNTER_BUILD_EXAMPLES "Build the example programs"              ON)
option(LEAKHUNTER_BUILD_POC      "Build the poc*/ demonstrations"           ON)
option(LEAKHUNTER_WERROR         "Treat warnings as errors"                OFF)

include(GNUInstallDirs)
include(LeakHunterWarnings)
include(LeakHunterDependencies)

set(LEAKHUNTER_AGENT_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/leakhunter")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildConfig.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/leakhunter/core/BuildConfig.hpp"
    @ONLY)

add_subdirectory(src)

if(LEAKHUNTER_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# On by default so it cannot silently rot: a demonstration nobody builds is a
# demonstration that stops working. An integration test pins its numbers.
if(LEAKHUNTER_BUILD_POC)
    add_subdirectory(poc)
    add_subdirectory(poc2)
    add_subdirectory(poc3)
    add_subdirectory(poc4)
endif()

if(LEAKHUNTER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

install(TARGETS leakhunter RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(TARGET leakhunter_agent)
    install(TARGETS leakhunter_agent LIBRARY DESTINATION ${LEAKHUNTER_AGENT_INSTALL_DIR})
endif()
install(FILES README.md LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR})

message(STATUS "")
message(STATUS "LeakHunter ${PROJECT_VERSION}")
message(STATUS "  build type ......... ${CMAKE_BUILD_TYPE}")
message(STATUS "  compiler ........... ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  agent (LD_PRELOAD) . ${LEAKHUNTER_BUILD_AGENT}")
message(STATUS "  unwinder ........... ${LEAKHUNTER_UNWINDER}")
message(STATUS "  tests .............. ${LEAKHUNTER_BUILD_TESTS}")
message(STATUS "  examples ........... ${LEAKHUNTER_BUILD_EXAMPLES}")
message(STATUS "  poc demo ........... ${LEAKHUNTER_BUILD_POC}")
message(STATUS "")
