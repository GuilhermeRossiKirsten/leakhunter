# Stripping a target removes the names LeakHunter would report. It must still
# find every leak, and must say `<unknown>+0xoffset` rather than inventing a
# name -- the offset is exact and is what a symbolizer or disassembler needs.
#
# Required: LEAKHUNTER, TARGET_BIN, OUTPUT, STRIP_TOOL
#   EXPECT_LEAK_COUNT   leaks that must still be found without symbols

if(NOT LEAKHUNTER OR NOT TARGET_BIN OR NOT OUTPUT)
    message(FATAL_ERROR "run_stripped.cmake: LEAKHUNTER, TARGET_BIN and OUTPUT are required")
endif()

find_program(strip_program NAMES ${STRIP_TOOL} strip)
if(NOT strip_program)
    message(STATUS "no strip(1) available; nothing to assert")
    return()
endif()

file(REMOVE_RECURSE "${OUTPUT}")
file(MAKE_DIRECTORY "${OUTPUT}")

set(stripped "${OUTPUT}/target-stripped")
file(COPY_FILE "${TARGET_BIN}" "${stripped}")
execute_process(COMMAND "${strip_program}" --strip-all "${stripped}" RESULT_VARIABLE stripResult)
if(NOT stripResult EQUAL 0)
    message(STATUS "strip failed on this platform; nothing to assert")
    return()
endif()

execute_process(
    COMMAND "${LEAKHUNTER}" --json --report-name report --output "${OUTPUT}" "${stripped}"
    RESULT_VARIABLE exitCode
    OUTPUT_VARIABLE standardOutput
    ERROR_VARIABLE standardError)

if(NOT EXISTS "${OUTPUT}/report.json")
    message(FATAL_ERROR "no report from a stripped target (exit ${exitCode}):\n${standardError}")
endif()

file(READ "${OUTPUT}/report.json" report)
string(JSON leakCount GET "${report}" summary leakCount)
string(JSON groupCount LENGTH "${report}" groups)
message(STATUS "stripped target: ${leakCount} leaks in ${groupCount} group(s)")

# Detection must not depend on symbols at all: they are two separate stages.
if(DEFINED EXPECT_LEAK_COUNT AND NOT leakCount EQUAL EXPECT_LEAK_COUNT)
    message(FATAL_ERROR "a stripped target must still yield ${EXPECT_LEAK_COUNT} leaks, "
                        "got ${leakCount}")
endif()

if(groupCount EQUAL 0)
    message(FATAL_ERROR "no leak sites at all from a stripped target")
endif()

# Every site must be honest about not knowing the name, and must still carry an
# exact address to act on.
math(EXPR lastIndex "${groupCount} - 1")
foreach(index RANGE ${lastIndex})
    string(JSON name GET "${report}" groups ${index} function)
    string(JSON location GET "${report}" groups ${index} location)
    string(JSON blamed GET "${report}" groups ${index} blamedFrame)
    string(JSON precise GET "${report}" groups ${index} stackTrace ${blamed} preciseName)

    if(precise)
        message(FATAL_ERROR "group ${index} claims a precise name from a stripped binary: ${name}")
    endif()
    if(location STREQUAL "")
        message(FATAL_ERROR "group ${index} has no location at all; "
                            "module+offset should have been used")
    endif()
    string(FIND "${location}" "+0x" offsetMark)
    if(offsetMark EQUAL -1)
        message(FATAL_ERROR "group ${index} location '${location}' carries no module offset")
    endif()
endforeach()

message(STATUS "every site reported module+offset and claimed no precise name")
message(STATUS "PASS")
