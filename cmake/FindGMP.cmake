# Finds the GNU Multiple Precision arithmetic library (GMP) and its C++
# bindings (gmpxx) under the system prefixes, with pkg-config paths and
# $MINGW_PREFIX as hints. Defines the imported target GMP::GMP, whose link
# interface includes libgmpxx, for <gmpxx.h> and mpz_class.
#
# The include directory is SYSTEM (the default for IMPORTED targets), so
# warnings inside gmpxx.h do not trip the strict -Werror flags.

# pkg-config supplies hint paths when available; its absence does not fail the build.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PC_GMP QUIET gmp)
endif()

# In an MSYS2 MinGW shell the MinGW cmake has its prefix (/ucrt64 or /mingw64)
# on CMAKE_SYSTEM_PREFIX_PATH and pkgconf ships gmp.pc, so GMP is found without
# hints. $ENV{MINGW_PREFIX}, which MSYS2 sets in every MinGW shell, is a hint
# for shells without pkgconf or with a non-MinGW cmake. Outside such a shell it
# is empty, and find_* ignores it.
find_path(GMP_INCLUDE_DIR
    NAMES gmp.h
    HINTS ${PC_GMP_INCLUDEDIR} ${PC_GMP_INCLUDE_DIRS} $ENV{MINGW_PREFIX}
    PATH_SUFFIXES include)

find_library(GMP_LIBRARY
    NAMES gmp libgmp
    HINTS ${PC_GMP_LIBDIR} ${PC_GMP_LIBRARY_DIRS} $ENV{MINGW_PREFIX}
    PATH_SUFFIXES lib)

# The C++ bindings are a separate library (libgmpxx). The code uses its
# expression templates and mpz_class arithmetic but not its string or iostream
# operators, whose ABI differs between C++ standard libraries (see
# src/core/oracle.cpp). On MinGW, CMAKE_FIND_LIBRARY_SUFFIXES lets find_library
# resolve the .dll.a import library or the .a static library by the same name.
find_library(GMPXX_LIBRARY
    NAMES gmpxx libgmpxx
    HINTS ${PC_GMP_LIBDIR} ${PC_GMP_LIBRARY_DIRS} $ENV{MINGW_PREFIX}
    PATH_SUFFIXES lib)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(GMP
    REQUIRED_VARS GMP_LIBRARY GMPXX_LIBRARY GMP_INCLUDE_DIR
    REASON_FAILURE_MESSAGE
        "Install GMP incl. its C++ bindings. MSYS2/MinGW: 'pacman -S mingw-w64-ucrt-x86_64-gmp' (UCRT64) or 'pacman -S mingw-w64-x86_64-gmp' (MINGW64) -- ships gmp.h, gmpxx.h and libgmp/libgmpxx (.dll.a).")

if(GMP_FOUND AND NOT TARGET GMP::GMP)
    add_library(GMP::GMP UNKNOWN IMPORTED)
    set_target_properties(GMP::GMP PROPERTIES
        IMPORTED_LOCATION "${GMP_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${GMP_INCLUDE_DIR}")
    # gmpxx must precede gmp on the link line, so it is an interface link library.
    set_property(TARGET GMP::GMP APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES "${GMPXX_LIBRARY}")
endif()

mark_as_advanced(GMP_INCLUDE_DIR GMP_LIBRARY GMPXX_LIBRARY)
