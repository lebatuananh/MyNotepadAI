# Vendored libssh2

This directory is the wrapper for **libssh2**, the SSH-2 protocol library that
powers NotepadADE's remote-execution foundation (Phase 1: SSH transport +
remote terminal). It is the same library git and curl use.

It is built with the vendored **mbedTLS** (`../mbedtls/`) as its crypto backend,
so there is no dependency on a system OpenSSL on any platform.

## Pinned version

- **Version:** `1.11.1`
- **License:** BSD-3-Clause
- **Release:** https://github.com/libssh2/libssh2/releases/tag/libssh2-1.11.1
- **Tarball:** https://github.com/libssh2/libssh2/releases/download/libssh2-1.11.1/libssh2-1.11.1.tar.gz

## Why libssh2 (not libssh / system ssh)

Phase 1 multiplexes many logical channels (a PTY now; git/exec/SFTP later) over
**one** `LIBSSH2_SESSION` on a single TCP connection, in **non-blocking** mode,
driven by a `QSocketNotifier` pump on a dedicated worker thread (0% idle CPU).
That control over channel multiplexing and the socket fd is exactly what a low
-level protocol library gives us and a higher-level wrapper / shelling out to
the system `ssh` binary does not. See `openspec/changes/add-ssh-remote-foundation/design.md`
decisions D1 and D2.

## How to vendor the sources

The upstream source tree is intentionally **not** committed here. Drop the
extracted release tree so this layout exists:

```
thirdparty/libssh2/
  CMakeLists.txt          (this wrapper — committed)
  README.md               (this file — committed)
  libssh2-1.11.1/         (extracted release tarball — NOT committed)
    include/libssh2.h
    src/
    CMakeLists.txt        (upstream's own CMake — enumerates its own sources)
```

From the repo root:

```bash
cd NotepadADE/thirdparty/libssh2
curl -L -o libssh2-1.11.1.tar.gz \
  https://github.com/libssh2/libssh2/releases/download/libssh2-1.11.1/libssh2-1.11.1.tar.gz
tar xzf libssh2-1.11.1.tar.gz          # -> libssh2-1.11.1/
rm libssh2-1.11.1.tar.gz
```

Vendor mbedTLS first (`../mbedtls/README.md`) — the libssh2 wrapper reads
`NN_MBEDTLS_INCLUDE_DIR` published by the mbedTLS wrapper and links the
`mbedcrypto` target, so `thirdparty/CMakeLists.txt` must `add_subdirectory(mbedtls)`
**before** `add_subdirectory(libssh2)` (it already does).

## What the wrapper does

- `add_subdirectory()`s `libssh2-1.11.1/` with `CRYPTO_BACKEND=mbedTLS` pointed
  at the vendored mbedTLS (no system crypto).
- Forces static-only (`BUILD_SHARED_LIBS OFF`, `BUILD_STATIC_LIBS ON`), disables
  examples/tests and zlib compression.
- Normalizes the upstream static target (`libssh2_static`) to a stable
  link name **`libssh2`** (via an INTERFACE alias) so `src/CMakeLists.txt`
  and `tests/CMakeLists.txt` can `target_link_libraries(... libssh2)`
  regardless of the upstream target spelling.
- Silences upstream warnings (`/W0` MSVC, `-w` otherwise) and keeps PIC on.
- Neutralizes libssh2's `install()`/`export()` rules (shadowed with empty
  functions, scoped to the libssh2 subtree) — we consume it as an in-tree static
  lib and never `make install`/package it. Without this, libssh2's
  `install(EXPORT "libssh2-targets")` pulls our PUBLIC `mbedcrypto` link into an
  export set it isn't part of, failing the CMake generate step. This lives in
  the wrapper, so it survives re-vendoring.

## ⚠️ One required patch INSIDE the extracted tree (re-apply on re-vendor)

The wrapper cannot fix everything from outside. After extracting
`libssh2-1.11.1/`, guard the **`include(CPack)`** at the bottom of
`libssh2-1.11.1/CMakeLists.txt` so it only runs when libssh2 is the top-level
project:

```cmake
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(CPACK_PACKAGE_VERSION_MAJOR ${LIBSSH2_VERSION_MAJOR})
  set(CPACK_PACKAGE_VERSION_MINOR ${LIBSSH2_VERSION_MINOR})
  set(CPACK_PACKAGE_VERSION_PATCH ${LIBSSH2_VERSION_PATCH})
  set(CPACK_PACKAGE_VERSION ${LIBSSH2_VERSION})
  include(CPack)
endif()
```

As a vendored subdirectory, an unconditional `include(CPack)` globally reserves
the `package` target name and clashes with the host app's own packaging target
(`NotepadADE/cmake/PackagingWindows.cmake`). Note: once that bad `include(CPack)`
has run even once, it writes `CPackConfig.cmake` into the build dir, which keeps
poisoning every reconfigure — `rm -rf build-debug` after applying the guard.
- Does **not** disable `/guard:cf` (MSVC) or Linux stack-protector/RELRO — those
  are inherited from the toolchain by design.

## Windows / ssh-agent note

Agent authentication uses the Pageant named pipe on Windows via
`libssh2_agent_*`; no extra build dependency is required (it is part of libssh2).

## If you must pin a different version

Update the version in **three** places and keep them in sync with this README:
the `NN_LIBSSH2_DIR` path, the `FATAL_ERROR` hint, and the alias logic in
`CMakeLists.txt`. libssh2 1.11.x is verified against mbedTLS 3.6.x.
