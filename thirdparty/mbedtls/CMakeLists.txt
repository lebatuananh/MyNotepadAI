# Vendored mbedTLS — crypto backend for the vendored libssh2 SSH transport.
# License: Apache-2.0 (dual Apache-2.0 / GPL-2.0; we use Apache-2.0).
#
# PINNED VERSION: v3.6.2 (LTS)
# Source:  https://github.com/Mbed-TLS/mbedtls/releases/tag/mbedtls-3.6.2
#
# The upstream source tree is NOT committed in this wrapper directory. Drop the
# extracted release tree into:
#     thirdparty/mbedtls/mbedtls-3.6.2/
# (the directory must contain include/ and library/). See README.md here for
# the exact download/extract commands and checksum.
#
# We reuse mbedTLS's own CMake build — it enumerates its own source files, so
# this honors the repo's no-glob rule — but force static-only, disable
# tests/programs, and silence upstream warnings to match the other thirdparty
# targets (libvterm, libptyqt, xdiff, supertinycron).

set(NN_MBEDTLS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/mbedtls-3.6.2")

if(NOT EXISTS "${NN_MBEDTLS_DIR}/include/mbedtls/build_info.h")
    message(FATAL_ERROR
        "Vendored mbedTLS sources not found at:\n"
        "    ${NN_MBEDTLS_DIR}\n"
        "Download mbedTLS v3.6.2 and extract it there before building the SSH\n"
        "remote support. See thirdparty/mbedtls/README.md for the exact steps.")
endif()

# Static-only, no tests/programs/fuzzers. Use the committed generated files so a
# Python/Perl toolchain is not required at configure time.
set(ENABLE_TESTING             OFF CACHE BOOL "" FORCE)
set(ENABLE_PROGRAMS            OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS     OFF CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY ON  CACHE BOOL "" FORCE)
set(GEN_FILES                  OFF CACHE BOOL "" FORCE)

# Toolchain-detection fix for clang targeting the MSVC ABI with the *GNU*
# command-line frontend (our Windows toolchain: CMAKE_C_COMPILER_ID=Clang,
# CMAKE_C_SIMULATE_ID=MSVC, CMAKE_C_COMPILER_FRONTEND_VARIANT=GNU).
#
# mbedTLS does NOT consult CMake's own MSVC variable. It derives its private
# CMAKE_COMPILER_IS_MSVC from CMAKE_C_SIMULATE_ID (= "MSVC" here) and then
# appends MSVC-only flags "/W3 /utf-8" to CMAKE_C_FLAGS for its whole subtree
# (mbedcrypto + the bundled everest / p256-m targets). The GNU-frontend clang
# driver treats "/W3" and "/utf-8" as input filenames and errors out
# ("no such file or directory: '/W3'"). CMake's global MSVC var is FALSE in this
# configuration (the app uses GNU-style flags throughout), so only mbedTLS trips.
#
# Clearing the SIMULATE_ID vars in this directory scope makes mbedTLS fall back
# to CMAKE_C_COMPILER_ID = "Clang" and take its Clang branch (GNU-style -Wall…
# the driver accepts). The override is scoped to this directory and the
# add_subdirectory() children below (mbedtls sources + everest + p256-m); it does
# NOT affect libssh2 (a sibling add_subdirectory) or the rest of the build. Lives
# in the wrapper so it survives re-vendoring.
if(CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "GNU"
   AND CMAKE_C_SIMULATE_ID STREQUAL "MSVC")
    set(CMAKE_C_SIMULATE_ID "")
    set(CMAKE_CXX_SIMULATE_ID "")
endif()

add_subdirectory("${NN_MBEDTLS_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/mbedtls-build" EXCLUDE_FROM_ALL)

# Silence upstream warnings + keep PIC on the three static libs. We deliberately
# do NOT touch /guard:cf (MSVC) or stack-protector/RELRO (Linux): those come from
# the toolchain/just flags and inheriting them on these targets is intended.
foreach(_mbedtls_target mbedcrypto mbedx509 mbedtls)
    if(TARGET ${_mbedtls_target})
        set_target_properties(${_mbedtls_target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
        if(MSVC)
            target_compile_options(${_mbedtls_target} PRIVATE /W0)
        else()
            target_compile_options(${_mbedtls_target} PRIVATE -w)
        endif()
    endif()
endforeach()

# Expose the include dir as a cache var so libssh2's mbedTLS detection can find
# it without a system install.
set(NN_MBEDTLS_INCLUDE_DIR "${NN_MBEDTLS_DIR}/include"
    CACHE PATH "Vendored mbedTLS include directory" FORCE)
