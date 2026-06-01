# Vendored mbedTLS

This directory is the wrapper for the **mbedTLS** crypto library, used as the
crypto backend for the vendored `libssh2` SSH transport (see
`../libssh2/`). mbedTLS is chosen over OpenSSL/WinCNG because it builds
identically on every platform with no dependency on a system OpenSSL — matching
the repo's vendored-C-lib convention (Scintilla, libvterm, xdiff, lua, uchardet
are all vendored).

## Pinned version

- **Version:** `v3.6.2` (LTS)
- **License:** Apache-2.0 (upstream is dual Apache-2.0 / GPL-2.0; we use Apache-2.0)
- **Release:** https://github.com/Mbed-TLS/mbedtls/releases/tag/mbedtls-3.6.2
- **Tarball (with submodules/generated files):**
  https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.2/mbedtls-3.6.2.tar.bz2

> Use the **release tarball**, not a bare `git archive` of the tag — the release
> tarball ships the pre-generated source files (`error.c`, the ASN.1/OID tables,
> etc.) so a Python/Perl toolchain is **not** required at configure time. This is
> why `CMakeLists.txt` sets `GEN_FILES OFF`.

## How to vendor the sources

The upstream source tree is intentionally **not** committed here. Drop the
extracted release tree so this layout exists:

```
thirdparty/mbedtls/
  CMakeLists.txt          (this wrapper — committed)
  README.md               (this file — committed)
  mbedtls-3.6.2/          (extracted release tarball — NOT committed)
    include/mbedtls/build_info.h
    library/
    CMakeLists.txt        (upstream's own CMake — enumerates its own sources)
```

From the repo root:

```bash
cd NotepadADE/thirdparty/mbedtls
curl -L -o mbedtls-3.6.2.tar.bz2 \
  https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.2/mbedtls-3.6.2.tar.bz2
tar xjf mbedtls-3.6.2.tar.bz2          # -> mbedtls-3.6.2/
rm mbedtls-3.6.2.tar.bz2
```

The wrapper `CMakeLists.txt` `add_subdirectory()`s `mbedtls-3.6.2/`, forces
static-only (`mbedcrypto`, `mbedx509`, `mbedtls`), disables tests/programs,
silences upstream warnings (`/W0` MSVC, `-w` otherwise), and keeps
position-independent code on. It does **not** disable `/guard:cf` (MSVC) or the
Linux stack-protector/RELRO flags — those are inherited from the toolchain by
design.

## Windows clang (GNU frontend) toolchain fix

The wrapper clears `CMAKE_C_SIMULATE_ID`/`CMAKE_CXX_SIMULATE_ID` (scoped to the
mbedTLS subtree only) when the compiler is **clang with the GNU command-line
frontend targeting the MSVC ABI** — which is exactly this repo's Windows
toolchain (`CMAKE_C_COMPILER_ID=Clang`, `CMAKE_C_SIMULATE_ID=MSVC`,
`CMAKE_C_COMPILER_FRONTEND_VARIANT=GNU`).

mbedTLS derives its own `CMAKE_COMPILER_IS_MSVC` purely from `CMAKE_C_SIMULATE_ID`
and then appends MSVC-only flags **`/W3 /utf-8`** to `CMAKE_C_FLAGS` for its whole
subtree (`mbedcrypto` + the bundled `everest` / `p256-m` targets). The GNU-frontend
clang driver treats `/W3` and `/utf-8` as input filenames and dies with
`clang: error: no such file or directory: '/W3'`. Clearing the simulate id makes
mbedTLS take its Clang branch (GNU-style `-Wall …`, which the driver accepts).
The fix is in the wrapper and only fires for this specific toolchain combo, so a
real MSVC (`cl.exe`) or `clang-cl` build is unaffected and it survives re-vendoring.

## If you must pin a different version

Update the version in **three** places and keep them in sync:
1. `NN_MBEDTLS_DIR` / the `mbedtls-<ver>` path in `CMakeLists.txt`.
2. The `FATAL_ERROR` hint and `NN_MBEDTLS_INCLUDE_DIR` in `CMakeLists.txt`.
3. This README.

libssh2 1.11.x is verified against mbedTLS 3.6.x; do not pair it with a 2.x
mbedTLS branch.
