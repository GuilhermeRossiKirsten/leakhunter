# A target that never exits on its own must still produce a report when stopped.
#
# This is the case that used to fail completely: Ctrl-C killed the host, left the
# target orphaned and running, and wrote nothing. It needs a live process and a
# signal, so it drives the CLI through a shell rather than execute_process.
#
# Required: LEAKHUNTER, TARGET_BIN, OUTPUT
#   RUN_SECONDS       how long to let the target run before stopping it
#   STOP_SIGNAL       INT or TERM
#   EXPECT_FUNCTION   a function name that must be blamed

if(NOT LEAKHUNTER OR NOT TARGET_BIN OR NOT OUTPUT)
    message(FATAL_ERROR "run_longrunning.cmake: LEAKHUNTER, TARGET_BIN and OUTPUT are required")
endif()

if(NOT UNIX)
    message(STATUS "signals are POSIX-only; nothing to assert here")
    return()
endif()

find_program(shell_program sh)
if(NOT shell_program)
    message(STATUS "no sh(1); nothing to assert")
    return()
endif()

file(REMOVE_RECURSE "${OUTPUT}")
if(NOT DEFINED RUN_SECONDS)
    set(RUN_SECONDS 2)
endif()
if(NOT DEFINED STOP_SIGNAL)
    set(STOP_SIGNAL INT)
endif()

# Start it, let it run, stop it, wait. The bounded wait matters: a regression
# that leaves the host blocked must fail the test rather than hang the suite.
set(script "
'${LEAKHUNTER}' --json --report-name report --output '${OUTPUT}' '${TARGET_BIN}' > '${OUTPUT}.log' 2>&1 &
pid=$!
sleep ${RUN_SECONDS}
kill -${STOP_SIGNAL} $pid 2>/dev/null
waited=0
while kill -0 $pid 2>/dev/null && [ $waited -lt 100 ]; do
    sleep 0.1
    waited=$((waited + 1))
done
if kill -0 $pid 2>/dev/null; then
    echo 'STILL-RUNNING'
    kill -9 $pid 2>/dev/null
    exit 90
fi
wait $pid
echo \"exit:$?\"
")

execute_process(COMMAND "${shell_program}" -c "${script}"
                RESULT_VARIABLE shellResult
                OUTPUT_VARIABLE shellOutput
                ERROR_VARIABLE shellError)

message(STATUS "driver said: ${shellOutput}")
if(shellResult EQUAL 90)
    message(FATAL_ERROR "leakhunter did not exit after ${STOP_SIGNAL}; it would have hung a "
                        "terminal waiting for a target that never stops")
endif()

if(NOT EXISTS "${OUTPUT}/report.json")
    file(READ "${OUTPUT}.log" runLog)
    message(FATAL_ERROR "stopping a long-running target produced no report.\n${runLog}")
endif()

file(READ "${OUTPUT}/report.json" report)
string(JSON leakCount GET "${report}" summary leakCount)
string(JSON stopped GET "${report}" run stoppedByRequest)
string(JSON signal GET "${report}" run terminatingSignal)
string(JSON groupCount LENGTH "${report}" groups)
message(STATUS "stopped target: ${leakCount} leaks, ${groupCount} site(s), "
               "signal ${signal}, stoppedByRequest=${stopped}")

# The whole point: a live process still yields findings.
if(leakCount EQUAL 0)
    message(FATAL_ERROR "no leaks reported from a target that leaks on every tick")
endif()

# And the report must say the run was ended deliberately, or an incomplete trace
# reads as a malfunction.
if(NOT stopped)
    message(FATAL_ERROR "run.stoppedByRequest is false after we sent ${STOP_SIGNAL}")
endif()
if(signal EQUAL 0)
    message(FATAL_ERROR "run.terminatingSignal is 0 after we signalled the target")
endif()

if(DEFINED EXPECT_FUNCTION)
    set(found FALSE)
    math(EXPR lastIndex "${groupCount} - 1")
    foreach(index RANGE ${lastIndex})
        string(JSON name GET "${report}" groups ${index} function)
        if(name MATCHES "${EXPECT_FUNCTION}")
            set(found TRUE)
        endif()
    endforeach()
    if(NOT found)
        message(FATAL_ERROR "expected a leak blamed on '${EXPECT_FUNCTION}'")
    endif()
endif()

message(STATUS "PASS")
