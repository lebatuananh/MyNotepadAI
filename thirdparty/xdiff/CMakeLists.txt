# Vendored xdiff from git v2.45.0 (xdiff/ subtree). LGPL-2.1.
#
# We build a STATIC library so the editor process links the diff routines
# in-process — calling out to `git diff` is a process-spawn per file edit and
# would blow our gutter-update budget. xdl_diff lets us diff a HEAD blob
# against the live editor buffer at memcpy speed.
#
# The directory contains the original xdiff sources verbatim plus a minimal
# git-compat-util.h shim (the upstream file is huge and pulls in git's full
# runtime — we replace it with just the bits xdiff actually uses).

add_library(xdiff STATIC
    xdiffi.c
    xemit.c
    xhistogram.c
    xmerge.c
    xpatience.c
    xprepare.c
    xutils.c
)

target_include_directories(xdiff
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)

# xdiff is C89-with-a-touch-of-C99; let the compiler pick a relaxed standard.
set_target_properties(xdiff PROPERTIES
    C_STANDARD 99
    C_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

# Hush warnings from upstream code — xdiff is well-tested and we keep it
# verbatim. Re-enable warnings if we ever take a local patch.
if(MSVC)
    target_compile_options(xdiff PRIVATE /W0)
else()
    target_compile_options(xdiff PRIVATE -w)
endif()
