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

#include "FileEncodingDetector.h"

#include "uchardet.h"

#include <QTextCodec>

#include <cstdint>

FileEncodingDetector::Bom FileEncodingDetector::detectBom(const QByteArray &raw)
{
    const auto *p = reinterpret_cast<const unsigned char *>(raw.constData());
    const qsizetype n = raw.size();
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) return Bom::Utf8;
    if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE)                 return Bom::Utf16LE;
    if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF)                 return Bom::Utf16BE;
    return Bom::None;
}

int FileEncodingDetector::bomByteCount(Bom bom)
{
    switch (bom) {
    case Bom::Utf8:    return 3;
    case Bom::Utf16LE:
    case Bom::Utf16BE: return 2;
    case Bom::Utf32LE:
    case Bom::Utf32BE: return 4;
    case Bom::None:    break;
    }
    return 0;
}

bool FileEncodingDetector::isValidUtf8(const char *data, qsizetype size)
{
    // Hot loop: walk once, branch-light. Reject overlongs and surrogates.
    const auto *p = reinterpret_cast<const unsigned char *>(data);
    const unsigned char *end = p + size;
    while (p < end) {
        const unsigned char c = *p;
        if (c < 0x80) { ++p; continue; }

        int need;
        std::uint32_t cp;
        if      ((c & 0xE0) == 0xC0) { need = 1; cp = c & 0x1F; if (c < 0xC2) return false; }
        else if ((c & 0xF0) == 0xE0) { need = 2; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { need = 3; cp = c & 0x07; if (c > 0xF4) return false; }
        else return false;

        if (end - p <= need) return false;
        for (int i = 1; i <= need; ++i) {
            const unsigned char cc = p[i];
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        // Overlong / surrogate / out-of-range
        if (need == 1 && cp < 0x80)            return false;
        if (need == 2 && cp < 0x800)           return false;
        if (need == 3 && cp < 0x10000)         return false;
        if (cp >= 0xD800 && cp <= 0xDFFF)      return false;
        if (cp > 0x10FFFF)                      return false;
        p += need + 1;
    }
    return true;
}

bool FileEncodingDetector::decode(const QByteArray &raw, QString &out, Bom *bomOut)
{
    const Bom bom = detectBom(raw);
    if (bomOut) *bomOut = bom;

    switch (bom) {
    case Bom::Utf8:
        out = QString::fromUtf8(raw.constData() + 3, raw.size() - 3);
        return true;
    case Bom::Utf16LE: {
        QTextCodec *codec = QTextCodec::codecForName("UTF-16LE");
        out = codec ? codec->toUnicode(raw.constData() + 2, raw.size() - 2)
                    : QString::fromUtf16(reinterpret_cast<const char16_t *>(raw.constData() + 2),
                                         (raw.size() - 2) / 2);
        return true;
    }
    case Bom::Utf16BE: {
        QTextCodec *codec = QTextCodec::codecForName("UTF-16BE");
        if (codec) {
            out = codec->toUnicode(raw.constData() + 2, raw.size() - 2);
            return true;
        }
        return false;
    }
    case Bom::Utf32LE:
    case Bom::Utf32BE:
        // detectBom() never returns a UTF-32 kind, so this path is unreachable
        // from decode(); listed only to keep the switch exhaustive. Fall through
        // to the no-BOM detection ladder if ever hit.
    case Bom::None:
        break;
    }

    // No BOM. Fast path: try UTF-8 strict validation. Covers ~95% of source.
    if (isValidUtf8(raw.constData(), raw.size())) {
        out = QString::fromUtf8(raw);
        return true;
    }

    // Fallback: uchardet on the head (sniff ~64 KiB for budget control).
    uchardet_t ud = uchardet_new();
    const qsizetype sniff = qMin<qsizetype>(raw.size(), 64 * 1024);
    if (uchardet_handle_data(ud, raw.constData(), static_cast<size_t>(sniff)) != 0) {
        uchardet_delete(ud);
        out = QString::fromLatin1(raw);
        return false;
    }
    uchardet_data_end(ud);
    const char *enc = uchardet_get_charset(ud);
    QString result;
    bool ok = false;
    if (enc && enc[0] != '\0') {
        if (QTextCodec *codec = QTextCodec::codecForName(enc)) {
            QTextCodec::ConverterState st;
            result = codec->toUnicode(raw.constData(), raw.size(), &st);
            ok = (st.invalidChars == 0);
        }
    }
    uchardet_delete(ud);
    if (ok) {
        out = result;
        return true;
    }
    out = QString::fromLatin1(raw);
    return false;
}

FileEncodingDetector::SniffResult FileEncodingDetector::sniff(const QByteArray &head)
{
    SniffResult result;

    const auto *p = reinterpret_cast<const unsigned char *>(head.constData());
    const qsizetype n = head.size();

    // BOM fast path. UTF-32 must be checked before UTF-16 because a UTF-32LE BOM
    // (FF FE 00 00) starts with the UTF-16LE BOM (FF FE).
    if (n >= 4 && p[0] == 0x00 && p[1] == 0x00 && p[2] == 0xFE && p[3] == 0xFF) {
        result.bom = Bom::Utf32BE;
        result.codecName = QByteArrayLiteral("UTF-32BE");
        return result;
    }
    if (n >= 4 && p[0] == 0xFF && p[1] == 0xFE && p[2] == 0x00 && p[3] == 0x00) {
        result.bom = Bom::Utf32LE;
        result.codecName = QByteArrayLiteral("UTF-32LE");
        return result;
    }
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) {
        result.bom = Bom::Utf8;
        // UTF-8 is the implicit default — leave codecName empty.
        return result;
    }
    if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) {
        result.bom = Bom::Utf16LE;
        result.codecName = QByteArrayLiteral("UTF-16LE");
        return result;
    }
    if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) {
        result.bom = Bom::Utf16BE;
        result.codecName = QByteArrayLiteral("UTF-16BE");
        return result;
    }

    // No BOM. The head is already capped by the caller (≤64 KB). Strict UTF-8
    // validity over the head covers the common case; treat a clean head as
    // UTF-8 (codecName empty). A multibyte sequence may straddle the head's tail
    // boundary, so tolerate a truncated trailing sequence by trimming back to
    // the last lead byte before validating.
    qsizetype validLen = n;
    while (validLen > 0 && (static_cast<unsigned char>(head[validLen - 1]) & 0xC0) == 0x80)
        --validLen; // back over UTF-8 continuation bytes
    if (validLen > 0 && (static_cast<unsigned char>(head[validLen - 1]) & 0x80) != 0)
        --validLen; // back over the (possibly incomplete) lead byte
    if (isValidUtf8(head.constData(), validLen)) {
        result.codecName = QByteArray(); // UTF-8
        return result;
    }

    // Fallback: uchardet on the head (already ≤64 KB).
    uchardet_t ud = uchardet_new();
    if (uchardet_handle_data(ud, head.constData(), static_cast<size_t>(n)) == 0) {
        uchardet_data_end(ud);
        const char *enc = uchardet_get_charset(ud);
        if (enc && enc[0] != '\0') {
            // Only accept names QTextCodec can actually resolve so the caller's
            // lazy decode never silently falls through to Latin-1.
            if (QTextCodec::codecForName(enc))
                result.codecName = QByteArray(enc);
        }
    }
    uchardet_delete(ud);
    // Empty codecName == decode as UTF-8 (best effort), matching decode()'s
    // final fallback intent without materializing the whole string here.
    return result;
}
