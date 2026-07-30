# Report files must be named for the target and the moment, and must accumulate.
#
# Two runs of the same binary into the same directory have to leave two reports,
# not one overwritten twice -- which is what a fixed "report.json" does, and the
# reason the default template exists.
#
# It also checks that the summary prints the path it actually wrote. Printing a
# path that does not exist is the first symptom of the naming rule being
# implemented twice.
#
# Required: LEAKHUNTER, TARGET_BIN, OUTPUT

if(NOT LEAKHUNTER OR NOT TARGET_BIN OR NOT OUTPUT)
    message(FATAL_ERROR "run_naming.cmake: LEAKHUNTER, TARGET_BIN and OUTPUT are required")
endif()

file(REMOVE_RECURSE "${OUTPUT}")
get_filename_component(targetName "${TARGET_BIN}" NAME)

# --- default naming: <target>-<timestamp> ----------------------------------

execute_process(COMMAND "${LEAKHUNTER}" --output "${OUTPUT}" "${TARGET_BIN}"
                OUTPUT_VARIABLE firstOutput ERROR_VARIABLE firstError)

file(GLOB produced "${OUTPUT}/*.json" "${OUTPUT}/*.html")
list(LENGTH produced producedCount)
message(STATUS "first run produced ${producedCount} file(s)")
foreach(file IN LISTS produced)
    get_filename_component(name "${file}" NAME)
    message(STATUS "  ${name}")

    if(NOT name MATCHES "^${targetName}-[0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]-[0-9][0-9][0-9][0-9][0-9][0-9]\\.(json|html)$")
        message(FATAL_ERROR "'${name}' is not <target>-<YYYYMMDD-HHMMSS>.<ext>")
    endif()
endforeach()

if(NOT producedCount EQUAL 2)
    message(FATAL_ERROR "expected a .json and a .html, got ${producedCount} file(s)")
endif()

# The summary must name the files that exist. A stale hard-coded "report.json"
# here would send the reader to a path that was never written.
foreach(file IN LISTS produced)
    get_filename_component(name "${file}" NAME)
    string(FIND "${firstOutput}" "${name}" mentioned)
    if(mentioned EQUAL -1)
        message(FATAL_ERROR "the summary never mentions '${name}':\n${firstOutput}")
    endif()
endforeach()
if(firstOutput MATCHES "report\\.json")
    message(FATAL_ERROR "the summary still advertises a hard-coded report.json")
endif()

# --- a second run must not overwrite the first ------------------------------
#
# The timestamp has one-second resolution, so wait before running again. Without
# this the test would be flaky rather than wrong, which is worse.
execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1.1)
execute_process(COMMAND "${LEAKHUNTER}" --output "${OUTPUT}" "${TARGET_BIN}"
                OUTPUT_QUIET ERROR_QUIET)

file(GLOB afterSecond "${OUTPUT}/*.json" "${OUTPUT}/*.html")
list(LENGTH afterSecond afterCount)
message(STATUS "after a second run: ${afterCount} file(s)")
if(NOT afterCount EQUAL 4)
    message(FATAL_ERROR "a second run should add two more files, not replace them; "
                        "got ${afterCount}")
endif()

# --- an explicit name is honoured, and IS overwritten ----------------------

execute_process(COMMAND "${LEAKHUNTER}" --json --report-name pinned --output "${OUTPUT}"
                        "${TARGET_BIN}"
                OUTPUT_QUIET ERROR_QUIET)
if(NOT EXISTS "${OUTPUT}/pinned.json")
    message(FATAL_ERROR "--report-name pinned did not produce pinned.json")
endif()

message(STATUS "PASS")
