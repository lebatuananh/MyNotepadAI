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

#include "MinimapMath.h"

#include <QElapsedTimer>
#include <QTest>
#include <QtTest/QtTest>

#include <limits>

class TestEditorMinimap : public QObject
{
    Q_OBJECT
private slots:
    void zeroLines_zeroHeight_returnsZero();
    void lineFor_topPixel_returnsLineZero();
    void lineFor_bottomPixel_returnsLastLine();
    void lineFor_outOfRangeY_clamps();
    void yFor_firstLine_returnsZero();
    void yFor_lastLine_lessThanHeight();
    void lineFor_yFor_roundTrip_inMiddle();
    void hugeFile_lineFor_stillCorrect();
    void benchmark_lineFor_yFor_subMicrosecond();
};

void TestEditorMinimap::zeroLines_zeroHeight_returnsZero()
{
    QCOMPARE(MinimapMath::lineFor(0,   0, 0),  0);
    QCOMPARE(MinimapMath::lineFor(50, 100, 0), 0);
    QCOMPARE(MinimapMath::lineFor(0,   0, 100), 0);
    QCOMPARE(MinimapMath::yFor(5,   100, 0), 0);
}

void TestEditorMinimap::lineFor_topPixel_returnsLineZero()
{
    QCOMPARE(MinimapMath::lineFor(0, 400, 100), 0);
}

void TestEditorMinimap::lineFor_bottomPixel_returnsLastLine()
{
    QCOMPARE(MinimapMath::lineFor(399, 400, 100), 99);
}

void TestEditorMinimap::lineFor_outOfRangeY_clamps()
{
    QCOMPARE(MinimapMath::lineFor(-50, 400, 100), 0);
    QCOMPARE(MinimapMath::lineFor(9999, 400, 100), 99);
}

void TestEditorMinimap::yFor_firstLine_returnsZero()
{
    QCOMPARE(MinimapMath::yFor(0, 400, 100), 0);
}

void TestEditorMinimap::yFor_lastLine_lessThanHeight()
{
    QCOMPARE(MinimapMath::yFor(99, 400, 100), 396);
}

void TestEditorMinimap::lineFor_yFor_roundTrip_inMiddle()
{
    const int H = 1000;
    const qint32 N = 200;
    for (qint32 line = 0; line < N; ++line) {
        const int y = MinimapMath::yFor(line, H, N);
        const qint32 round = MinimapMath::lineFor(y, H, N);
        QCOMPARE(round, line);
    }
}

void TestEditorMinimap::hugeFile_lineFor_stillCorrect()
{
    const qint32 N = 1'000'000;
    QCOMPARE(MinimapMath::lineFor(0,   500, N), 0);
    QCOMPARE(MinimapMath::lineFor(499, 500, N), qint32(499 * 2000));
    QCOMPARE(MinimapMath::lineFor(250, 500, N), qint32(500000));
}

void TestEditorMinimap::benchmark_lineFor_yFor_subMicrosecond()
{
    const qint32 N = 100'000;
    QElapsedTimer t;
    t.start();
    volatile qint32 sink = 0;
    for (int y = 0; y < 1000; ++y) {
        sink ^= MinimapMath::lineFor(y, 1000, N);
        sink ^= MinimapMath::yFor(static_cast<qint32>(y * 100), 1000, N);
    }
    const qint64 ns = t.nsecsElapsed();
    const double perCallNs = ns / 2000.0;
    qInfo() << "MinimapMath per-call:" << perCallNs << "ns";
    QVERIFY2(perCallNs < 1000.0,
             qPrintable(QString("Math too slow: %1 ns/call").arg(perCallNs)));
}

QTEST_GUILESS_MAIN(TestEditorMinimap)
#include "test_editor_minimap.moc"
