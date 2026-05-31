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

#ifndef FILE_ENCODING_DETECTOR_H
#define FILE_ENCODING_DETECTOR_H

#include <QByteArray>
#include <QString>

// Tiny BOM-first encoding detector for the diff viewer: untracked files we need
// to display inline are usually source code, so BOM + UTF-8 validity covers
// 99% of cases. Falls back to uchardet only when bytes are clearly not UTF-8.
//
// Kept separate from ScintillaNext::readFromDisk to avoid touching the editor's
// load path — the diff view never goes through ScintillaNext file IO.
class FileEncodingDetector
{
public:
    // Utf32LE/Utf32BE are additive: detectBom()/decode() never produce them
    // (their behavior is unchanged); only the new sniff() path reports them, for
    // callers that lazily decode per-slice and need to take the UTF-16/32
    // transcode branch (e.g. the CSV table preview).
    enum class Bom : std::uint8_t { None, Utf8, Utf16LE, Utf16BE, Utf32LE, Utf32BE };

    // Decodes raw bytes to QString. Strips BOM. Returns false if decoding falls
    // back to lossy replacement (i.e. encoding could not be determined cleanly).
    static bool decode(const QByteArray &raw, QString &out, Bom *bomOut = nullptr);

    // Result of sniffing a file head without decoding the whole file.
    struct SniffResult {
        QByteArray codecName;   // name for QTextCodec::codecForName; empty == UTF-8
        Bom bom = Bom::None;    // detected BOM (may be a UTF-32 kind)
    };

    // Sniff-only encoding detection: inspect at most the first ~64 KB (the head
    // the caller supplies) and return the detected codec name + BOM WITHOUT
    // decoding the whole file. Mirrors decode()'s detection ladder (BOM fast
    // path → strict UTF-8 validity → uchardet on the head) but never builds a
    // QString of the content. Intended for lazy per-slice decoders.
    static SniffResult sniff(const QByteArray &head);

    // Number of BOM bytes at the start of a file for a given BOM kind
    // (0/3/2/2/4/4). Lets callers skip and later re-emit the BOM verbatim.
    static int bomByteCount(Bom bom);

private:
    static Bom detectBom(const QByteArray &raw);
    static bool isValidUtf8(const char *data, qsizetype size);
};

#endif // FILE_ENCODING_DETECTOR_H
