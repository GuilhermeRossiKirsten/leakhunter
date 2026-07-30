# FindLibunwind.cmake
#
# Locates libunwind (https://github.com/libunwind/libunwind).
#
# Defines:
#   Libunwind_FOUND
#   Libunwind::Libunwind   imported target (headers + generic + arch libraries)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PC_LIBUNWIND QUIET libunwind)
endif()

find_path(Libunwind_INCLUDE_DIR
    NAMES libunwind.h
    HINTS ${PC_LIBUNWIND_INCLUDE_DIRS})

find_library(Libunwind_LIBRARY
    NAMES unwind
    HINTS ${PC_LIBUNWIND_LIBRARY_DIRS})

# libunwind ships a per-architecture companion library holding the actual
# unwinding tables (libunwind-x86_64, libunwind-aarch64, ...).
set(_lh_unwind_arch "")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    set(_lh_unwind_arch x86_64)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(_lh_unwind_arch aarch64)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm.*)$")
    set(_lh_unwind_arch arm)
endif()

if(_lh_unwind_arch)
    find_library(Libunwind_ARCH_LIBRARY
        NAMES unwind-${_lh_unwind_arch}
        HINTS ${PC_LIBUNWIND_LIBRARY_DIRS})
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Libunwind
    REQUIRED_VARS Libunwind_LIBRARY Libunwind_INCLUDE_DIR)

if(Libunwind_FOUND AND NOT TARGET Libunwind::Libunwind)
    add_library(Libunwind::Libunwind UNKNOWN IMPORTED)
    set_target_properties(Libunwind::Libunwind PROPERTIES
        IMPORTED_LOCATION "${Libunwind_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${Libunwind_INCLUDE_DIR}")
    if(Libunwind_ARCH_LIBRARY)
        set_property(TARGET Libunwind::Libunwind APPEND PROPERTY
            INTERFACE_LINK_LIBRARIES "${Libunwind_ARCH_LIBRARY}")
    endif()
endif()

mark_as_advanced(Libunwind_INCLUDE_DIR Libunwind_LIBRARY Libunwind_ARCH_LIBRARY)
