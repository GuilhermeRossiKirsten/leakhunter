# Integration test driver.
#
# Runs the real CLI against a real program and asserts on the generated
# report.json. Written as a CMake script so the suite needs no scripting
# runtime beyond what is already required to build.
#
# Required:
#   LEAKHUNTER   path to the leakhunter executable
#   TARGET       path to the program to monitor
#   OUTPUT       directory for the reports
# Optional expectations:
#   EXPECT_LEAK_COUNT     exact number of leaks
#   EXPECT_MIN_LEAKS      minimum number of leaks (for non-deterministic runs)
#   EXPECT_LEAKED_BYTES   exact number of leaked bytes
#   EXPECT_SIGNAL         signal the target must have died from
#   EXPECT_TARGET_EXIT    the target's own exit code
#   EXPECT_PARTIAL        1 if the trace must be flagged incomplete
#   EXPECT_MIN_GROUPS     minimum number of distinct leak sites
#   EXPECT_FUNCTION       a function name that must appear among the groups
#   EXPECT_ABSENT         a function name that must NOT appear among the groups
#   EXPECT_MIN_THREADS    minimum threadCount on some group
#   EXPECT_MISMATCHES     exact number of mismatched frees
#   EXPECT_MISMATCH_FUNCTION
#                         a function name that must appear among the mismatches
#   EXPECT_EXIT           the exit code leakhunter itself must return
#   EXPECT_SNIPPET_CONTAINS
#                         text that must appear on the blamed line of some group's
#                         source snippet
#   EXPECT_NO_SNIPPETS    1 if no group may carry a snippet
#   EXPECT_SUPPRESSED     exact number of leaks suppressed by rules
#   EXPECT_UNUSED_RULES   exact number of suppression rules that matched nothing
#   EXTRA_ARGS            additional leakhunter flags (semicolon separated)

if(NOT LEAKHUNTER OR NOT TARGET_BIN OR NOT OUTPUT)
    message(FATAL_ERROR "run_case.cmake: LEAKHUNTER, TARGET_BIN and OUTPUT are required")
endif()

file(REMOVE_RECURSE "${OUTPUT}")

set(command "${LEAKHUNTER}" --json --output "${OUTPUT}")
if(EXTRA_ARGS)
    list(APPEND command ${EXTRA_ARGS})
endif()
list(APPEND command "${TARGET_BIN}")

message(STATUS "running: ${command}")

execute_process(
    COMMAND ${command}
    RESULT_VARIABLE exitCode
    OUTPUT_VARIABLE standardOutput
    ERROR_VARIABLE standardError)

message(STATUS "exit code: ${exitCode}")
message(STATUS "stdout:\n${standardOutput}")
if(standardError)
    message(STATUS "stderr:\n${standardError}")
endif()

if(DEFINED EXPECT_EXIT)
    if(NOT exitCode EQUAL EXPECT_EXIT)
        message(FATAL_ERROR "expected leakhunter to exit ${EXPECT_EXIT}, got ${exitCode}")
    endif()
elseif(NOT exitCode EQUAL 0 AND NOT exitCode EQUAL 1)
    # 0 = clean, 1 = defects found. Anything else means the tool itself failed,
    # unless the case is explicitly asserting on that code.
    message(FATAL_ERROR "leakhunter failed with exit code ${exitCode}")
endif()

set(reportPath "${OUTPUT}/report.json")
if(NOT EXISTS "${reportPath}")
    message(FATAL_ERROR "no report was generated at ${reportPath}")
endif()

file(READ "${reportPath}" report)

string(JSON leakCount GET "${report}" summary leakCount)
string(JSON leakedBytes GET "${report}" summary leakedBytes)
string(JSON totalAllocations GET "${report}" summary totalAllocations)
string(JSON groupCount LENGTH "${report}" groups)

message(STATUS "report: ${totalAllocations} allocations, ${leakCount} leaks, "
               "${leakedBytes} bytes, ${groupCount} groups")

if(totalAllocations EQUAL 0)
    message(FATAL_ERROR "no allocations were intercepted at all -- LD_PRELOAD injection failed")
endif()

if(DEFINED EXPECT_LEAK_COUNT AND NOT leakCount EQUAL EXPECT_LEAK_COUNT)
    message(FATAL_ERROR "expected ${EXPECT_LEAK_COUNT} leaks, got ${leakCount}")
endif()

if(DEFINED EXPECT_MIN_LEAKS AND leakCount LESS EXPECT_MIN_LEAKS)
    message(FATAL_ERROR "expected at least ${EXPECT_MIN_LEAKS} leaks, got ${leakCount}")
endif()

if(DEFINED EXPECT_SIGNAL)
    string(JSON actualSignal GET "${report}" run terminatingSignal)
    if(NOT actualSignal EQUAL EXPECT_SIGNAL)
        message(FATAL_ERROR "expected the target to die from signal ${EXPECT_SIGNAL}, "
                            "got ${actualSignal}")
    endif()
endif()

if(DEFINED EXPECT_TARGET_EXIT)
    string(JSON actualExit GET "${report}" run exitCode)
    if(NOT actualExit EQUAL EXPECT_TARGET_EXIT)
        message(FATAL_ERROR "expected the target to exit ${EXPECT_TARGET_EXIT}, got ${actualExit}")
    endif()
endif()

# A trace with no end marker must be flagged, otherwise partial results would
# silently read as complete ones.
if(DEFINED EXPECT_PARTIAL)
    string(JSON dropped GET "${report}" summary droppedRecords)
    if(dropped EQUAL 0)
        message(FATAL_ERROR "expected the trace to be flagged incomplete, droppedRecords is 0")
    endif()
endif()

if(DEFINED EXPECT_LEAKED_BYTES AND NOT leakedBytes EQUAL EXPECT_LEAKED_BYTES)
    message(FATAL_ERROR "expected ${EXPECT_LEAKED_BYTES} leaked bytes, got ${leakedBytes}")
endif()

if(DEFINED EXPECT_MIN_GROUPS AND groupCount LESS EXPECT_MIN_GROUPS)
    message(FATAL_ERROR "expected at least ${EXPECT_MIN_GROUPS} leak sites, got ${groupCount}")
endif()

# Collect the group function names once; the remaining checks all use them.
set(functions "")
set(maxThreads 0)
if(groupCount GREATER 0)
    math(EXPR lastIndex "${groupCount} - 1")
    foreach(index RANGE ${lastIndex})
        string(JSON name GET "${report}" groups ${index} function)
        string(JSON threads GET "${report}" groups ${index} threadCount)
        list(APPEND functions "${name}")
        if(threads GREATER maxThreads)
            set(maxThreads ${threads})
        endif()
    endforeach()
endif()

message(STATUS "leak sites: ${functions}")

if(DEFINED EXPECT_FUNCTION)
    set(found FALSE)
    foreach(name IN LISTS functions)
        if(name MATCHES "${EXPECT_FUNCTION}")
            set(found TRUE)
        endif()
    endforeach()
    if(NOT found)
        message(FATAL_ERROR "expected a leak blamed on '${EXPECT_FUNCTION}', got: ${functions}")
    endif()
endif()

if(DEFINED EXPECT_ABSENT)
    foreach(name IN LISTS functions)
        if(name MATCHES "${EXPECT_ABSENT}")
            message(FATAL_ERROR "'${EXPECT_ABSENT}' must not be reported, but matched '${name}'")
        endif()
    endforeach()
endif()

if(DEFINED EXPECT_MIN_THREADS AND maxThreads LESS EXPECT_MIN_THREADS)
    message(FATAL_ERROR "expected a group spanning at least ${EXPECT_MIN_THREADS} threads, "
                        "the widest had ${maxThreads}")
endif()

# --- mismatched frees ------------------------------------------------------

string(JSON mismatchCount GET "${report}" summary mismatchedFreeCount)
string(JSON mismatchListed LENGTH "${report}" mismatchedFrees)
message(STATUS "mismatched frees: ${mismatchCount} (${mismatchListed} listed)")

if(DEFINED EXPECT_MISMATCHES AND NOT mismatchCount EQUAL EXPECT_MISMATCHES)
    set(descriptions "")
    if(mismatchListed GREATER 0)
        math(EXPR lastMismatch "${mismatchListed} - 1")
        foreach(index RANGE ${lastMismatch})
            string(JSON text GET "${report}" mismatchedFrees ${index} description)
            list(APPEND descriptions "${text}")
        endforeach()
    endif()
    message(FATAL_ERROR "expected ${EXPECT_MISMATCHES} mismatched frees, got ${mismatchCount}"
                        " (${descriptions})")
endif()

# --- source snippets -------------------------------------------------------
#
# Collect the blamed line of every group's snippet. Reaching this at all means
# the whole chain worked: interception, unwinding, DWARF symbolisation, and
# reading the file back off disk.

# Checked inside the loop rather than collected into a list first: source lines
# are full of semicolons, and CMake would split every one of them into separate
# list elements.

set(snippetCount 0)
set(snippetMatchFound FALSE)
if(groupCount GREATER 0)
    math(EXPR lastIndex "${groupCount} - 1")
    foreach(index RANGE ${lastIndex})
        string(JSON snippet ERROR_VARIABLE noSnippet GET "${report}" groups ${index} snippet)
        if(NOT noSnippet STREQUAL "NOTFOUND")
            continue()
        endif()

        math(EXPR snippetCount "${snippetCount} + 1")
        string(JSON firstLine GET "${snippet}" firstLine)
        string(JSON blamedLine GET "${snippet}" blamedLine)
        math(EXPR offset "${blamedLine} - ${firstLine}")
        string(JSON blamedText GET "${snippet}" lines ${offset})

        message(STATUS "  snippet blames line ${blamedLine}: ${blamedText}")

        if(DEFINED EXPECT_SNIPPET_CONTAINS)
            string(FIND "${blamedText}" "${EXPECT_SNIPPET_CONTAINS}" position)
            if(NOT position EQUAL -1)
                set(snippetMatchFound TRUE)
            endif()
        endif()
    endforeach()
endif()

message(STATUS "snippets: ${snippetCount} of ${groupCount} group(s)")

if(DEFINED EXPECT_SNIPPET_CONTAINS AND NOT snippetMatchFound)
    message(FATAL_ERROR "no snippet's blamed line contains '${EXPECT_SNIPPET_CONTAINS}'")
endif()

if(DEFINED EXPECT_NO_SNIPPETS AND NOT snippetCount EQUAL 0)
    message(FATAL_ERROR "expected no snippets, got ${snippetCount}")
endif()

# --- suppressions ----------------------------------------------------------

string(JSON suppressedByRules GET "${report}" summary suppressedByRules)
string(JSON unusedRuleCount GET "${report}" summary unusedSuppressionRules)
string(JSON appliedRuleCount LENGTH "${report}" suppressions applied)
message(STATUS "suppressed by rules: ${suppressedByRules} via ${appliedRuleCount} rule(s), "
               "${unusedRuleCount} unused")

if(DEFINED EXPECT_SUPPRESSED AND NOT suppressedByRules EQUAL EXPECT_SUPPRESSED)
    message(FATAL_ERROR "expected ${EXPECT_SUPPRESSED} suppressed leaks, got ${suppressedByRules}")
endif()

if(DEFINED EXPECT_UNUSED_RULES AND NOT unusedRuleCount EQUAL EXPECT_UNUSED_RULES)
    message(FATAL_ERROR "expected ${EXPECT_UNUSED_RULES} unused rules, got ${unusedRuleCount}")
endif()

if(DEFINED EXPECT_MISMATCH_FUNCTION)
    set(found FALSE)
    if(mismatchListed GREATER 0)
        math(EXPR lastMismatch "${mismatchListed} - 1")
        foreach(index RANGE ${lastMismatch})
            string(JSON name GET "${report}" mismatchedFrees ${index} responsibleFunction)
            if(name MATCHES "${EXPECT_MISMATCH_FUNCTION}")
                set(found TRUE)
            endif()
        endforeach()
    endif()
    if(NOT found)
        message(FATAL_ERROR "expected a mismatch blamed on '${EXPECT_MISMATCH_FUNCTION}'")
    endif()
endif()

message(STATUS "PASS")
