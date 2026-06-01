# Vendored libssh2 — SSH transport for the remote-execution foundation.
# License: BSD-3-Clause.
#
# PINNED VERSION: 1.11.1
# Source:  https://github.com/libssh2/libssh2/releases/tag/libssh2-1.11.1
#
# libssh2 is a pure SSH-2 protocol library (the same one git/curl use). We need
# it — not a higher-level wrapper — because Phase 1 multiplexes many logical
# channels (PTY now; git/exec/SFTP later) over ONE LIBSSH2_SESSION on a single
# TCP connection, in non-blocking mode, driven by a QSocketNotifier pump. That
# level of control is the whole point (see openspec design D1/D2).
#
# Crypto backend = the vendored mbedTLS (../mbedtls/), so there is no dependency
# on a system OpenSSL on any platform.
#
# The upstream source tree is NOT committed in this wrapper directory. Drop the
# extracted release tree into:
#     thirdparty/libssh2/libssh2-1.11.1/
# (the directory must contain include/ and src/). See README.md here for the
# exact download/extract commands and checksum.
#
# We reuse libssh2's own CMake build (it enumerates its own source files, so the
# no-glob rule is honored) but force static-only, mbedTLS crypto, no
# tests/examples, and silence upstream warnings to match the other thirdparty
# targets.

set(NN_LIBSSH2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libssh2-1.11.1")

if(NOT EXISTS "${NN_LIBSSH2_DIR}/include/libssh2.h")
    message(FATAL_ERROR
        "Vendored libssh2 sources not found at:\n"
        "    ${NN_LIBSSH2_DIR}\n"
        "Download libssh2 1.11.1 and extract it there before building the SSH\n"
        "remote support. See thirdparty/libssh2/README.md for the exact steps.")
endif()

# Point libssh2 at the vendored mbedTLS. ../mbedtls/CMakeLists.txt (added first
# in thirdparty/CMakeLists.txt) publishes NN_MBEDTLS_INCLUDE_DIR and the
# mbedcrypto target. libssh2 finds mbedTLS via MBEDTLS_INCLUDE_DIR +
# MBEDCRYPTO_LIBRARY; we feed the in-tree target so no system install is needed.
set(CRYPTO_BACKEND        "mbedTLS"               CACHE STRING "" FORCE)
set(MBEDTLS_INCLUDE_DIR   "${NN_MBEDTLS_INCLUDE_DIR}" CACHE PATH "" FORCE)
set(MBEDCRYPTO_LIBRARY    "mbedcrypto"            CACHE STRING "" FORCE)

set(BUILD_SHARED_LIBS     OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS     ON  CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES        OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING         OFF CACHE BOOL "" FORCE)
set(ENABLE_ZLIB_COMPRESSION OFF CACHE BOOL "" FORCE)
set(LIBSSH2_NO_DEPRECATED  OFF CACHE BOOL "" FORCE)

# Neutralize libssh2's install()/export() rules for the subtree below. We consume
# libssh2 as an in-tree static lib and never `make install`/package it, so its
# install(EXPORT "libssh2-targets") is dead weight — and worse, it pulls our
# PUBLIC mbedcrypto link into the export set, failing the CMake generate step
# ("target mbedcrypto is not in any export set"). Shadowing the built-ins with
# empty functions is scoped to this directory and its add_subdirectory children
# (the vendored libssh2 tree) and does NOT leak to sibling targets like mbedtls.
# Lives in the wrapper so it survives re-vendoring. EXCLUDE_FROM_ALL only drops
# libssh2 from the default build target; it does NOT suppress install/export
# generation, which is why this override is required.
function(install)
endfunction()
function(export)
endfunction()

add_subdirectory("${NN_LIBSSH2_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/libssh2-build" EXCLUDE_FROM_ALL)

# Upstream's static target is "libssh2_static". Normalize to an INTERFACE alias
# "libssh2" so src/CMakeLists.txt can link a stable name regardless of the
# upstream target's spelling across versions, and make sure it pulls mbedcrypto.
if(TARGET libssh2_static)
    set(_nn_ssh2_real libssh2_static)
elseif(TARGET libssh2)
    set(_nn_ssh2_real libssh2)
else()
    message(FATAL_ERROR "libssh2 CMake did not define a static target (libssh2_static).")
endif()

set_target_properties(${_nn_ssh2_real} PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(${_nn_ssh2_real} PRIVATE /W0)
else()
    target_compile_options(${_nn_ssh2_real} PRIVATE -w)
endif()
if(TARGET mbedcrypto)
    target_link_libraries(${_nn_ssh2_real} PUBLIC mbedcrypto)
endif()

# Stable link name for the app + tests. If the real target is already named
# "libssh2" this alias is skipped.
if(NOT TARGET libssh2)
    add_library(nn_libssh2 INTERFACE)
    target_link_libraries(nn_libssh2 INTERFACE ${_nn_ssh2_real})
    target_include_directories(nn_libssh2 INTERFACE "${NN_LIBSSH2_DIR}/include")
    add_library(libssh2 ALIAS nn_libssh2)
endif()
