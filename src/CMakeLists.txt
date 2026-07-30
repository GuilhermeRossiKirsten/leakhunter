# ---------------------------------------------------------------------------
# leakhunter_core -- everything the CLI does once the target has exited.
#
# Kept as a library rather than folded into the executable so the tests can
# exercise the modules directly, without spawning processes.
# ---------------------------------------------------------------------------
add_library(leakhunter_core STATIC
    analysis/LeakAnalyzer.cpp
    analysis/LeakTriage.cpp
    analysis/SuppressionSet.cpp
    cli/CommandLineParser.cpp
    core/AgentLocator.cpp
    core/Glob.cpp
    core/Logger.cpp
    core/ScopedTempFile.cpp
    core/Types.cpp
    process/PosixProcessRunner.cpp
    registry/AllocationRegistry.cpp
    report/DiagnosticsWriter.cpp
    report/HtmlReportGenerator.cpp
    report/JsonReportGenerator.cpp
    source/SourceSnippetReader.cpp
    symbols/SourceLineResolver.cpp
    symbols/SymbolResolver.cpp
    tracker/FileTraceSource.cpp
    tracker/MemoryTracker.cpp
    app/Application.cpp)

add_library(leakhunter::core ALIAS leakhunter_core)

target_include_directories(leakhunter_core
    PUBLIC
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_BINARY_DIR}/generated")

target_link_libraries(leakhunter_core
    PUBLIC
        fmt::fmt
        spdlog::spdlog
        nlohmann_json::nlohmann_json
    PRIVATE
        leakhunter::warnings)

set_target_properties(leakhunter_core PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

# ---------------------------------------------------------------------------
# leakhunter -- the CLI
# ---------------------------------------------------------------------------
add_executable(leakhunter main.cpp)

target_link_libraries(leakhunter
    PRIVATE
        leakhunter::core
        leakhunter::warnings)

# ---------------------------------------------------------------------------
# leakhunter_agent -- the injected LD_PRELOAD library
#
# Intentionally isolated from leakhunter_core: this code runs inside somebody
# else's process, underneath their allocator. It must not pull in fmt, spdlog,
# nlohmann/json, or anything else that allocates on a hot path.
# ---------------------------------------------------------------------------
if(LEAKHUNTER_BUILD_AGENT)
    add_library(leakhunter_agent SHARED
        agent/Agent.cpp
        agent/BootstrapAllocator.cpp
        agent/Interceptors.cpp
        agent/PcRegistry.cpp
        agent/RealFunctions.cpp
        agent/StackTraceCollector.cpp
        agent/TraceWriter.cpp)

    add_library(leakhunter::agent ALIAS leakhunter_agent)

    target_include_directories(leakhunter_agent
        PRIVATE
            "${CMAKE_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/agent")

    target_link_libraries(leakhunter_agent
        PRIVATE
            leakhunter::warnings
            ${CMAKE_DL_LIBS}
            pthread)

    if(LEAKHUNTER_WITH_LIBUNWIND)
        target_link_libraries(leakhunter_agent PRIVATE Libunwind::Libunwind)
        target_compile_definitions(leakhunter_agent PRIVATE LEAKHUNTER_USE_LIBUNWIND=1)
    else()
        target_compile_definitions(leakhunter_agent PRIVATE LEAKHUNTER_USE_LIBUNWIND=0)
    endif()

    set_target_properties(leakhunter_agent PROPERTIES
        # Only the interposed entry points are visible; everything else stays
        # private so agent internals can never collide with target symbols.
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        # The CLI looks the library up by this exact name.
        OUTPUT_NAME leakhunter_agent
        PREFIX "lib")

    target_compile_options(leakhunter_agent PRIVATE
        # Frame pointers make the fallback unwinder usable and cost little.
        -fno-omit-frame-pointer)

    # The interceptors deliberately redefine libc symbols, which GCC warns about.
    # The flag must be guarded: Clang has no such warning, and passing an unknown
    # -Wno-* to it is an *error* under -Werror (-Wunknown-warning-option). That
    # made -DLEAKHUNTER_WERROR=ON fail outright with Clang, in spite of the README
    # advertising Clang 14+.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(leakhunter_agent PRIVATE -Wno-builtin-declaration-mismatch)
    endif()

    # A shared library tolerates undefined symbols by default, which turns a
    # missing source file into a crash inside the *target* process at the first
    # allocation. Fail at link time instead.
    target_link_options(leakhunter_agent PRIVATE -Wl,--no-undefined)
endif()
