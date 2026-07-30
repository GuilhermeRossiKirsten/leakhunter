# Sample targets for LeakHunter. Each one leaks in a specific, documented way
# so the tool's output can be checked against a known answer.
#
# Built with frame pointers and debug info so the reports show real file:line.

function(leakhunter_add_example name)
    add_executable(${name} ${name}.cpp)
    target_compile_options(${name} PRIVATE -g -fno-omit-frame-pointer -O0)
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples")
endfunction()

leakhunter_add_example(simple_leak)
leakhunter_add_example(multiple_leaks)
leakhunter_add_example(threaded_leak)
leakhunter_add_example(new_delete)
leakhunter_add_example(malloc_free)
leakhunter_add_example(clean_program)
leakhunter_add_example(crash_after_leak)
leakhunter_add_example(abrupt_exit)
leakhunter_add_example(mismatched_free)
leakhunter_add_example(spawns_child)

# The mismatches in this one are the point of the example, so the compiler must
# not refuse to build it. GCC's -Wmismatched-new-delete catches the two cases it
# can see locally; the whole value of LeakHunter here is catching the ones that
# cross a translation unit, where no compiler can help.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(mismatched_free PRIVATE -Wno-mismatched-new-delete -Wno-free-nonheap-object)
endif()

target_link_libraries(threaded_leak PRIVATE pthread)
