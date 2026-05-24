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

#include "GitBlameParser.h"

#include <QByteArray>

namespace {

inline bool isHexDigit(char c)
{
    return (c >= '0' && c <= '9')
        || (c >= 'a' && c <= 'f')
        || (c >= 'A' && c <= 'F');
}

// Header line shape: "<40 hex> <orig-line> <final-line> [<count>]"
bool isHeaderLine(QByteArrayView line)
{
    if (line.size() < 41) return false;
    for (int i = 0; i < 40; ++i)
        if (!isHexDigit(line.at(i))) return false;
    return line.at(40) == ' ';
}

// Parse a long decimal starting at `data + pos`, advancing `pos` past it.
// Returns 0 on parse failure — the caller treats 0 as an invalid line, which
// is fine because git uses 1-based line numbers.
qint64 parseLong(QByteArrayView line, qsizetype &pos)
{
    qint64 v = 0;
    bool any = false;
    while (pos < line.size()) {
        const char c = line.at(pos);
        if (c < '0' || c > '9') break;
        v = v * 10 + (c - '0');
        ++pos;
        any = true;
    }
    return any ? v : 0;
}

void skipSpaces(QByteArrayView line, qsizetype &pos)
{
    while (pos < line.size() && line.at(pos) == ' ') ++pos;
}

} // namespace

GitBlameParser::Result GitBlameParser::parse(QByteArrayView porcelain)
{
    Result out;
    if (porcelain.isEmpty()) return out;

    // Reserve based on a rough porcelain-bytes-per-line heuristic. The
    // typical line is ~120 bytes of porcelain (header + content); for files
    // with many lines this saves a few reallocs.
    const qsizetype estLines = porcelain.size() / 120 + 16;
    out.lines.reserve(estLines);
    out.records.reserve(estLines / 4 + 4);

    // Map sha → record index, so subsequent (non-first) header lines for
    // the same commit reuse the existing record.
    QHash<QByteArray, qint32> shaIndex;

    QByteArray currentSha;
    qint32 currentRecord = -1;
    qint64 currentFinalLine = 0;
    qint64 currentCount = 1;
    bool inMetadata = false;

    // Walk the porcelain stream line by line. We don't allocate per line —
    // QByteArrayView slices into the input buffer with no copy.
    qsizetype p = 0;
    const qsizetype n = porcelain.size();
    while (p < n) {
        qsizetype lineEnd = p;
        while (lineEnd < n && porcelain.at(lineEnd) != '\n') ++lineEnd;
        QByteArrayView line = porcelain.sliced(p, lineEnd - p);
        const qsizetype next = lineEnd < n ? lineEnd + 1 : lineEnd;

        if (!line.isEmpty() && line.at(0) == '\t') {
            // Content line — emit a Line entry for each line covered by the
            // current header (count >= 1 for first occurrence, 1 for repeats).
            if (currentRecord >= 0 && currentFinalLine > 0) {
                out.lines.append({ static_cast<qint32>(currentFinalLine - 1),
                                   currentRecord });
                // Multi-line headers: subsequent content lines belong to the
                // next final-line of the same commit. The porcelain spec says
                // each subsequent line in the group is its own header+content
                // pair, so we just advance and let the next header reset us.
            }
            inMetadata = false;
        } else if (isHeaderLine(line)) {
            currentSha = QByteArray(line.constData(), 40);
            qsizetype pos = 41;
            (void)parseLong(line, pos); // orig-line — unused for our purpose
            skipSpaces(line, pos);
            currentFinalLine = parseLong(line, pos);
            skipSpaces(line, pos);
            currentCount = pos < line.size() ? parseLong(line, pos) : 1;
            if (currentCount <= 0) currentCount = 1;

            const auto it = shaIndex.constFind(currentSha);
            if (it != shaIndex.constEnd()) {
                currentRecord = it.value();
                inMetadata = false;
            } else {
                Record r;
                r.sha = currentSha;
                out.records.append(r);
                currentRecord = static_cast<qint32>(out.records.size() - 1);
                shaIndex.insert(currentSha, currentRecord);
                inMetadata = true;
            }
        } else if (inMetadata && currentRecord >= 0) {
            // Metadata key/value lines.
            auto &rec = out.records[currentRecord];
            if (line.startsWith("author ")) {
                rec.author = QString::fromUtf8(line.constData() + 7,
                                               line.size() - 7);
            } else if (line.startsWith("author-time ")) {
                qsizetype pos = 12;
                rec.authorTime = parseLong(line, pos);
            } else if (line.startsWith("summary ")) {
                rec.summary = QString::fromUtf8(line.constData() + 8,
                                                line.size() - 8);
            } else if (line == QByteArrayView("boundary")) {
                rec.boundary = true;
            }
            // Other fields (author-mail, author-tz, committer*, previous,
            // filename) are ignored — keep parser fast.
        }

        p = next;
    }
    return out;
}
