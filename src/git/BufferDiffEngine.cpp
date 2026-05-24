/*
 * This file is part of Notepad Next.
 * Copyright 2026 NotepadAI contributors
 *
 * Notepad Next is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Notepad Next is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Notepad Next.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "BufferDiffEngine.h"

extern "C" {
// git-compat-util.h provides the `regex_t` opaque typedef that xdiff.h
// references — pull it in first so the public header type-checks in our
// MSVC-ABI build (Windows has no <regex.h>).
#include "git-compat-util.h"
#include "xdiff.h"
}

#include <QByteArray>

#include <cstring>

namespace BufferDiffEngine {

namespace {

// xdl_diff hands hunks to us through a C function pointer with a void* cookie.
// The cookie is a pointer to the destination vector; emit_hunk appends one
// entry per fired hunk.
int emit_hunk(long start_a, long count_a,
              long start_b, long count_b,
              void *cb)
{
    auto *out = static_cast<Hunks *>(cb);
    out->append(Hunk{
        static_cast<qint32>(start_a),
        static_cast<qint32>(count_a),
        static_cast<qint32>(start_b),
        static_cast<qint32>(count_b),
    });
    return 0;
}

unsigned long algorithmFlag(Algorithm a)
{
    switch (a) {
    case Algorithm::Histogram: return XDF_HISTOGRAM_DIFF;
    case Algorithm::Patience:  return XDF_PATIENCE_DIFF;
    case Algorithm::Myers:     return 0;
    }
    return XDF_HISTOGRAM_DIFF;
}

// Strip CR that immediately precedes LF. Only allocates when the input
// actually contains \r — callers fall back to zero-copy when this returns
// an empty buffer. Lone \r (old-Mac line ending) is preserved so it stays
// part of the line content rather than silently merging lines.
QByteArray stripCRLF(QByteArrayView in)
{
    QByteArray out;
    out.reserve(in.size());
    const char *p = in.data();
    const qsizetype n = in.size();
    for (qsizetype i = 0; i < n; ++i) {
        if (p[i] == '\r' && i + 1 < n && p[i + 1] == '\n') continue;
        out.append(p[i]);
    }
    return out;
}

} // namespace

Hunks diff(QByteArrayView base, QByteArrayView buf, Algorithm algo)
{
    // git stores blobs in canonical LF form, but Scintilla's getText returns
    // the buffer with whatever EOL it loaded from disk — on Windows that's
    // usually CRLF. xdl_diff is line-oriented and treats \r as content, so a
    // CRLF/LF mismatch makes every line look modified. Detect via memchr
    // first; if neither side has \r the call stays zero-copy.
    const bool baseHasCR = base.size() > 0
        && std::memchr(base.data(), '\r', static_cast<size_t>(base.size())) != nullptr;
    const bool bufHasCR  = buf.size() > 0
        && std::memchr(buf.data(),  '\r', static_cast<size_t>(buf.size()))  != nullptr;

    QByteArray normBase;
    QByteArray normBuf;
    if (baseHasCR) normBase = stripCRLF(base);
    if (bufHasCR)  normBuf  = stripCRLF(buf);

    const QByteArrayView baseView = baseHasCR ? QByteArrayView(normBase) : base;
    const QByteArrayView bufView  = bufHasCR  ? QByteArrayView(normBuf)  : buf;

    // Byte-level equality is a common case in real workloads (file saved
    // unchanged, autocompletion stamping the same characters): short-circuit
    // here so xdl_diff doesn't have to tokenise both sides just to discover
    // the same fact.
    if (baseView.size() == bufView.size()
        && (baseView.isEmpty()
            || std::memcmp(baseView.data(), bufView.data(),
                           static_cast<size_t>(baseView.size())) == 0)) {
        return {};
    }

    // xdl_diff takes a non-const `mmfile_t*`, but treats both inputs as
    // read-only. Cast away const to satisfy the C ABI — the call site's
    // QByteArrayView already encodes immutability for us.
    mmfile_t mf1 {
        const_cast<char *>(baseView.data()),
        static_cast<long>(baseView.size()),
    };
    mmfile_t mf2 {
        const_cast<char *>(bufView.data()),
        static_cast<long>(bufView.size()),
    };

    xpparam_t xpp {};
    xpp.flags = algorithmFlag(algo);

    xdemitconf_t xecfg {};
    xecfg.hunk_func = emit_hunk;

    Hunks out;
    // Pre-reserve a small power-of-two to skip the first few realloc steps;
    // most editor sessions on a single file produce O(small) hunks.
    out.reserve(16);

    xdemitcb_t ecb {};
    ecb.priv = &out;

    // xdl_diff returns non-zero on internal failure (OOM, bug). On failure we
    // surface an empty vector — callers treat "no hunks" as "no markers", and
    // they will retry on the next refresh tick. Failing loudly here would
    // wedge the gutter for the rest of the session.
    if (xdl_diff(&mf1, &mf2, &xpp, &xecfg, &ecb) != 0) {
        return {};
    }
    return out;
}

} // namespace BufferDiffEngine
