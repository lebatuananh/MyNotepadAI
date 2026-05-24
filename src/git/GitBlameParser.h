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

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QVector>

#include <cstdint>

// Decoder for `git blame --porcelain` output. The porcelain format is
// machine-stable across git versions and is the only mode that yields
// per-line metadata without ambiguous whitespace parsing.
//
// The format is naturally denormalised: the first occurrence of a commit in
// the stream carries the full author/summary block; subsequent occurrences
// only repeat the header. We normalise on the way in — author/summary live
// in `records`, lines reference them by index — so a file with 10k lines
// authored by 5 commits costs 5 * (Author + Summary) + 10k * (qint32, qint32).
class GitBlameParser
{
public:
    struct Record {
        QByteArray sha;        // 40 hex chars
        QString    author;
        qint64     authorTime = 0;
        QString    summary;
        bool       boundary   = false; // root commit (no parent)
    };

    struct Line {
        qint32 lineIdx;   // 0-indexed line in the file
        qint32 recordIdx; // index into Result::records
    };

    struct Result {
        QVector<Record> records;
        QVector<Line>   lines;
    };

    // Pure function. Tolerant of truncated input (returns what could be
    // parsed so the UI degrades gracefully).
    static Result parse(QByteArrayView porcelain);
};
