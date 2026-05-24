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
#include "GitGutterMarkers.h"

#include <QTest>
#include <QtTest/QtTest>

class TestGitGutterMarkers : public QObject
{
    Q_OBJECT
private slots:
    void emptyHunks_emitsNoMarkers();
    void pureAddedHunk_emitsAddedLines();
    void pureDeletedHunk_emitsDeletedAnchorOnPrevLine();
    void modifiedHunk_emitsModifiedOnNewSideLines();
    void deletionAtTop_emitsDeletedAtLineZero();
    void multipleHunks_emitsDisjointSets();
    void hunkMapping_pointsClickedLineAtItsHunk();
};

void TestGitGutterMarkers::emptyHunks_emitsNoMarkers()
{
    const BufferDiffEngine::Hunks hunks;
    const auto out = GitGutterMarkers::linesFromHunks(hunks);
    QVERIFY(out.added.isEmpty());
    QVERIFY(out.modified.isEmpty());
    QVERIFY(out.deletedAt.isEmpty());
}

void TestGitGutterMarkers::pureAddedHunk_emitsAddedLines()
{
    // Insert two new lines starting at buffer line 4 (0-indexed).
    BufferDiffEngine::Hunks hunks {
        BufferDiffEngine::Hunk{ /*oldStart*/ 4, /*oldCount*/ 0,
                                 /*newStart*/ 4, /*newCount*/ 2 },
    };
    const auto out = GitGutterMarkers::linesFromHunks(hunks);
    QCOMPARE(out.added, (QVector<qint32>{4, 5}));
    QVERIFY(out.modified.isEmpty());
    QVERIFY(out.deletedAt.isEmpty());
}

void TestGitGutterMarkers::pureDeletedHunk_emitsDeletedAnchorOnPrevLine()
{
    // Delete one old line that lived after a context line at buffer-line 3.
    // xdl_diff reports newStart at the position where the deletion would
    // sit in the buffer (one past the context), so newStart=3.
    BufferDiffEngine::Hunks hunks {
        BufferDiffEngine::Hunk{ 3, 1, 3, 0 },
    };
    const auto out = GitGutterMarkers::linesFromHunks(hunks);
    QVERIFY(out.added.isEmpty());
    QVERIFY(out.modified.isEmpty());
    QCOMPARE(out.deletedAt, (QVector<qint32>{2}));
}

void TestGitGutterMarkers::modifiedHunk_emitsModifiedOnNewSideLines()
{
    // Replace two old lines at line 6 with two new ones.
    BufferDiffEngine::Hunks hunks {
        BufferDiffEngine::Hunk{ 6, 2, 6, 2 },
    };
    const auto out = GitGutterMarkers::linesFromHunks(hunks);
    QVERIFY(out.added.isEmpty());
    QVERIFY(out.deletedAt.isEmpty());
    QCOMPARE(out.modified, (QVector<qint32>{6, 7}));
}

void TestGitGutterMarkers::deletionAtTop_emitsDeletedAtLineZero()
{
    // First line of the file deleted — no preceding context, so anchor
    // falls back to line 0.
    BufferDiffEngine::Hunks hunks {
        BufferDiffEngine::Hunk{ 0, 1, 0, 0 },
    };
    const auto out = GitGutterMarkers::linesFromHunks(hunks);
    QCOMPARE(out.deletedAt, (QVector<qint32>{0}));
}

void TestGitGutterMarkers::multipleHunks_emitsDisjointSets()
{
    BufferDiffEngine::Hunks hunks {
        BufferDiffEngine::Hunk{ 0,  0, 0,  1 }, // added at top
        BufferDiffEngine::Hunk{ 10, 2, 11, 2 }, // modified
        BufferDiffEngine::Hunk{ 30, 1, 30, 0 }, // deleted after ctx 30
    };
    const auto out = GitGutterMarkers::linesFromHunks(hunks);
    QCOMPARE(out.added,     (QVector<qint32>{0}));
    QCOMPARE(out.modified,  (QVector<qint32>{11, 12}));
    QCOMPARE(out.deletedAt, (QVector<qint32>{29}));
}

void TestGitGutterMarkers::hunkMapping_pointsClickedLineAtItsHunk()
{
    BufferDiffEngine::Hunks hunks {
        BufferDiffEngine::Hunk{ 0,  0, 0,  1 },
        BufferDiffEngine::Hunk{ 10, 2, 11, 2 },
        BufferDiffEngine::Hunk{ 30, 1, 30, 0 },
    };
    const auto out = GitGutterMarkers::linesFromHunks(hunks);

    const auto idxTop  = out.lineToHunkIdx.value(0,  -1);
    const auto idxMidA = out.lineToHunkIdx.value(11, -1);
    const auto idxMidB = out.lineToHunkIdx.value(12, -1);
    const auto idxBot  = out.lineToHunkIdx.value(29, -1);

    QCOMPARE(idxTop,  0);
    QCOMPARE(idxMidA, 1);
    QCOMPARE(idxMidB, 1);
    QCOMPARE(idxBot,  2);
}

QTEST_GUILESS_MAIN(TestGitGutterMarkers)
#include "test_git_gutter_markers.moc"
