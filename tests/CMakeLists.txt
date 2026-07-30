# ---------------------------------------------------------------------------
# Unit tests -- pure host-side logic, no process spawning.
# ---------------------------------------------------------------------------
add_executable(leakhunter_unit_tests
    unit/main.cpp
    unit/AllocationRegistryTest.cpp
    unit/CommandLineParserTest.cpp
    unit/DiagnosticsTest.cpp
    unit/GlobTest.cpp
    unit/LeakAnalyzerTest.cpp
    unit/ReportGeneratorTest.cpp
    unit/SourceSnippetTest.cpp
    unit/SuppressionSetTest.cpp
    unit/TracedPidTest.cpp
    unit/TraceRoundTripTest.cpp)

target_include_directories(leakhunter_unit_tests
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/support")

target_link_libraries(leakhunter_unit_tests
    PRIVATE leakhunter::core leakhunter::warnings)

add_test(NAME unit COMMAND leakhunter_unit_tests)
set_tests_properties(unit PROPERTIES LABELS "unit")

# ---------------------------------------------------------------------------
# Integration tests -- the real CLI, the real agent, real programs.
#
# Each case asserts on report.json, so a regression in interception, analysis
# or serialisation fails the build.
# ---------------------------------------------------------------------------
if(LEAKHUNTER_BUILD_AGENT AND LEAKHUNTER_BUILD_EXAMPLES)
    set(runner "${CMAKE_CURRENT_SOURCE_DIR}/integration/run_case.cmake")

    function(leakhunter_add_integration_test name target)
        cmake_parse_arguments(ARG "" "" "EXPECT;EXTRA_ARGS" ${ARGN})

        set(defines "")
        foreach(expectation IN LISTS ARG_EXPECT)
            list(APPEND defines "-D${expectation}")
        endforeach()

        if(ARG_EXTRA_ARGS)
            # Semicolon-joined so it survives as a CMake list inside the script.
            string(REPLACE ";" "\\;" forwarded "${ARG_EXTRA_ARGS}")
            list(APPEND defines "-DEXTRA_ARGS=${forwarded}")
        endif()

        add_test(NAME ${name}
            COMMAND "${CMAKE_COMMAND}"
                    "-DLEAKHUNTER=$<TARGET_FILE:leakhunter>"
                    "-DTARGET_BIN=$<TARGET_FILE:${target}>"
                    "-DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/reports/${name}"
                    ${defines}
                    -P "${runner}")

        set_tests_properties(${name} PROPERTIES
            LABELS "integration"
            TIMEOUT 120)
    endfunction()

    # A single unfreed malloc, blamed on the function that made it.
    leakhunter_add_integration_test(integration_simple_leak simple_leak
        EXPECT "EXPECT_LEAK_COUNT=1"
               "EXPECT_LEAKED_BYTES=1024"
               "EXPECT_FUNCTION=allocateBuffer")

    # Several sites, grouped; the balanced one must not show up.
    leakhunter_add_integration_test(integration_multiple_leaks multiple_leaks
        EXPECT "EXPECT_MIN_GROUPS=3"
               "EXPECT_FUNCTION=leakSmall"
               "EXPECT_ABSENT=allocateAndFree")

    # Concurrency: 4 threads leaking simultaneously.
    leakhunter_add_integration_test(integration_threaded_leak threaded_leak
        EXPECT "EXPECT_LEAK_COUNT=100"
               "EXPECT_LEAKED_BYTES=51200"
               "EXPECT_MIN_THREADS=4")

    # operator new / operator new[], with the matched pairs excluded.
    leakhunter_add_integration_test(integration_new_delete new_delete
        EXPECT "EXPECT_LEAK_COUNT=2"
               "EXPECT_FUNCTION=leak"
               "EXPECT_ABSENT=balancedNewDelete")

    # The full C allocator surface, including a realloc chain.
    leakhunter_add_integration_test(integration_malloc_free malloc_free
        EXPECT "EXPECT_LEAK_COUNT=3"
               "EXPECT_ABSENT=balancedMallocFree")

    # The negative control: no leaks means no findings.
    leakhunter_add_integration_test(integration_clean_program clean_program
        EXPECT "EXPECT_LEAK_COUNT=0"
               "EXPECT_MISMATCHES=0"
               "EXPECT_EXIT=0")

    # Nothing leaks and the run still has to fail: every block is returned
    # through the wrong entry point.
    leakhunter_add_integration_test(integration_mismatched_free mismatched_free
        EXPECT "EXPECT_LEAK_COUNT=0"
               "EXPECT_MISMATCHES=4"
               "EXPECT_MISMATCH_FUNCTION=new"
               "EXPECT_EXIT=1")

    # ... and the opt-out has to reach the exit code, not just the listing.
    leakhunter_add_integration_test(integration_mismatch_opt_out mismatched_free
        EXTRA_ARGS "--no-mismatch-check"
        EXPECT "EXPECT_LEAK_COUNT=0"
               "EXPECT_MISMATCHES=0"
               "EXPECT_EXIT=0")

    # A suppression rule must remove its leaks from the headline count and leave
    # the unnamed sites alone. multiple_leaks leaks 111 blocks across 3 sites;
    # the rule covers leakSmall's 100.
    leakhunter_add_integration_test(integration_suppressions multiple_leaks
        EXTRA_ARGS "--suppressions;${CMAKE_SOURCE_DIR}/examples/multiple_leaks.supp"
        EXPECT "EXPECT_LEAK_COUNT=11"
               "EXPECT_SUPPRESSED=100"
               "EXPECT_UNUSED_RULES=0"
               "EXPECT_MIN_GROUPS=2"
               "EXPECT_FUNCTION=leakMedium"
               "EXPECT_ABSENT=leakSmall"
               "EXPECT_EXIT=1")

    # Suppressing every site makes an otherwise-failing run pass. This is the
    # property the whole feature exists for, so it gets its own case.
    #
    # 112, not 111: a `stack:` rule matching the target's module also covers the
    # one runtime block (a stdio buffer), because that allocation's stack passes
    # through main() in the same binary. That is correct -- a user rule outranks
    # our runtime classification -- and it is exactly how broad `stack:` rules
    # are, which is why the number is pinned here rather than left approximate.
    leakhunter_add_integration_test(integration_suppress_everything multiple_leaks
        EXTRA_ARGS "--suppressions;${CMAKE_SOURCE_DIR}/tests/integration/suppress_all.supp"
        EXPECT "EXPECT_LEAK_COUNT=0"
               "EXPECT_SUPPRESSED=112"
               "EXPECT_EXIT=0")

    # Suppressions must cover mismatched frees too, or a user with unfixable
    # vendored UB writes rules, still gets exit 1, and has nothing telling them why.
    leakhunter_add_integration_test(integration_suppress_mismatches mismatched_free
        EXTRA_ARGS "--suppressions;${CMAKE_SOURCE_DIR}/examples/mismatched_free.supp"
        EXPECT "EXPECT_LEAK_COUNT=0"
               "EXPECT_MISMATCHES=0"
               "EXPECT_UNUSED_RULES=0"
               "EXPECT_EXIT=0")

    # A target that fork+exec's a child used to have its trace destroyed by that
    # child, and the host reported the CHILD's leaks as the target's -- with
    # droppedRecords still zero. This asserts the parent's leaks are all present
    # and the child's are absent.
    leakhunter_add_integration_test(integration_spawns_child spawns_child
        EXPECT "EXPECT_LEAK_COUNT=20100"
               "EXPECT_FUNCTION=leak"
               "EXPECT_ABSENT=runAsChild"
               "EXPECT_EXIT=1")

    # Two regressions in one target, both of which silently lost data:
    #   * fork() without exec: the child inherited a copy of the parent's buffer
    #     and shared its file offset, so its flush corrupted the parent's trace.
    #     Reported 50 leaks out of 100.
    #   * the daemonisation idiom closed the agent's trace descriptor, producing
    #     an empty trace the host blamed on static linking.
    leakhunter_add_integration_test(integration_forks_and_closes_fds forks_and_closes_fds
        EXPECT "EXPECT_LEAK_COUNT=400"
               "EXPECT_LEAKED_BYTES=51200"
               "EXPECT_FUNCTION=leakBlocks"
               "EXPECT_EXIT=1")

    # A rule that matches nothing is a warning by default and fatal on request.
    leakhunter_add_integration_test(integration_strict_suppressions multiple_leaks
        EXTRA_ARGS "--strict-suppressions;--suppressions;${CMAKE_SOURCE_DIR}/tests/integration/rotted.supp"
        EXPECT "EXPECT_UNUSED_RULES=1"
               "EXPECT_EXIT=2")

    # The poc/ demonstration. Its README quotes these numbers, so pinning them
    # here is what stops the demo drifting away from its own documentation --
    # and a demonstration nobody tests is one that breaks quietly.
    if(TARGET docindex)
        leakhunter_add_integration_test(integration_poc_docindex docindex
            EXPECT "EXPECT_LEAK_COUNT=700"
                   "EXPECT_MIN_GROUPS=3"
                   "EXPECT_MISMATCHES=8"
                   "EXPECT_MISMATCH_FUNCTION=copyPayload"
                   "EXPECT_MIN_THREADS=4"
                   "EXPECT_FUNCTION=indexBatch"
                   "EXPECT_EXIT=1")

        # The blamed line must come back out of the source file. Reaching this
        # exercises the whole chain: interception, unwinding, DWARF, file read.
        leakhunter_add_integration_test(integration_poc_snippets docindex
            EXPECT "EXPECT_LEAK_COUNT=700"
                   "EXPECT_SNIPPET_CONTAINS=malloc(kScratchBytes)"
                   "EXPECT_EXIT=1")

        # ...and the opt-out has to actually stop reading source.
        leakhunter_add_integration_test(integration_poc_no_snippets docindex
            EXTRA_ARGS "--no-source-snippets"
            EXPECT "EXPECT_LEAK_COUNT=700"
                   "EXPECT_NO_SNIPPETS=1"
                   "EXPECT_EXIT=1")

        # --no-source implies it too: with no file:line there is nothing to read.
        leakhunter_add_integration_test(integration_poc_no_source docindex
            EXTRA_ARGS "--no-source"
            EXPECT "EXPECT_NO_SNIPPETS=1"
                   "EXPECT_EXIT=1")

        # And the worked suppression file must keep doing what its comments claim.
        leakhunter_add_integration_test(integration_poc_suppressed docindex
            EXTRA_ARGS "--suppressions;${CMAKE_SOURCE_DIR}/poc/docindex.supp"
            EXPECT "EXPECT_LEAK_COUNT=600"
                   "EXPECT_SUPPRESSED=100"
                   "EXPECT_UNUSED_RULES=0"
                   "EXPECT_ABSENT=indexBatch"
                   "EXPECT_EXIT=1")
    endif()

    # A target killed mid-run never runs the agent's destructor. Without the
    # fatal-signal flush the entire buffered trace is lost, so this guards the
    # case a leak hunter is most often reached for.
    leakhunter_add_integration_test(integration_crash_after_leak crash_after_leak
        EXPECT "EXPECT_MIN_LEAKS=400"
               "EXPECT_SIGNAL=11"
               "EXPECT_PARTIAL=1"
               "EXPECT_FUNCTION=leakBeforeDying")

    # _exit() skips destructors and atexit handlers; the interposed _exit is
    # what keeps the trace.
    leakhunter_add_integration_test(integration_abrupt_exit abrupt_exit
        EXPECT "EXPECT_MIN_LEAKS=250"
               "EXPECT_TARGET_EXIT=3"
               "EXPECT_PARTIAL=1"
               "EXPECT_FUNCTION=leakThenBailOut")
endif()
