# A single interface target carrying the project-wide warning set.
# Consumers link `leakhunter::warnings` (PRIVATE) instead of touching flags.

add_library(leakhunter_warnings INTERFACE)
add_library(leakhunter::warnings ALIAS leakhunter_warnings)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(leakhunter_warnings INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wnon-virtual-dtor
        -Wcast-align
        -Wunused
        -Woverloaded-virtual
        -Wnull-dereference
        -Wdouble-promotion
        -Wformat=2)
    if(LEAKHUNTER_WERROR)
        target_compile_options(leakhunter_warnings INTERFACE -Werror)
    endif()
elseif(MSVC)
    target_compile_options(leakhunter_warnings INTERFACE /W4 /permissive-)
    if(LEAKHUNTER_WERROR)
        target_compile_options(leakhunter_warnings INTERFACE /WX)
    endif()
endif()
