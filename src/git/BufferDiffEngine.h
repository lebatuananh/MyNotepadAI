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

#pragma once

#include <QByteArrayView>
#include <QVector>

#include <cstdint>

// In-process buffer-vs-blob diff. Wraps the vendored xdiff library so callers
// stay in C++ and never spawn `git diff`. Designed for the gutter hot path:
// every keystroke can reasonably trigger a re-diff, so the wrapper is
// zero-copy on the input side (xdl_diff reads from `mmfile_t` which is just a
// (ptr, size) pair pointing into the caller's buffers).
//
// The output is a flat hunk vector — no string copies, no Scintilla coupling.
// Higher-level code (e.g. GitGutterDecorator) maps hunks to marker lines.
namespace BufferDiffEngine {

struct Hunk {
    // All values are 0-based line indices coming straight out of xdl_diff.
    qint32 oldStart;  // first line of the change in the base (HEAD) blob
    qint32 oldCount;  // number of base lines covered (0 => pure insertion)
    qint32 newStart;  // first line of the change in the buffer
    qint32 newCount;  // number of buffer lines covered (0 => pure deletion)
};

using Hunks = QVector<Hunk>;

enum class Algorithm : quint8 {
    Histogram,  // default — fastest for typical source code
    Myers,      // xdiff's classic algorithm; useful when we want exact upstream parity
    Patience,
};

// Run xdl_diff against `base` (HEAD blob) and `buf` (live buffer). Both views
// must outlive the call; xdiff treats them as read-only memory regions and
// performs no copies of the input. Returns an empty vector when the two views
// are identical (fast path: byte-level equality short-circuits xdiff entirely).
//
// Thread-safety: pure function, no globals. Safe to invoke from any thread.
Hunks diff(QByteArrayView base, QByteArrayView buf,
           Algorithm algo = Algorithm::Histogram);

} // namespace BufferDiffEngine
