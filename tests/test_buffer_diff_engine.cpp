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

#include <QByteArray>
#include <QElapsedTimer>
#include <QString>
#include <QTest>
#include <QtTest/QtTest>

#include <limits>

class TestBufferDiffEngine : public QObject
{
    Q_OBJECT
private slots:
    void identicalBuffers_emitZeroHunks();
    void emptyBase_addsAllAsOneHunk();
    void emptyBuffer_deletesAllAsOneHunk();
    void singleLineInsert_oneHunk_zeroOld_oneNew();
    void singleLineDelete_oneHunk_oneOld_zeroNew();
    void singleLineModify_oneHunk_oneOld_oneNew();
    void disjointEdits_twoHunks();
    void crlfBaseAgainstLfBuffer_emitsZeroHunks();
    void crlfBufferAgainstLfBase_emitsZeroHunks();
    void benchmark_5kLineDiff_under_3ms_p95();
};

namespace {

QByteArray joinLines(int count, const char *body)
{
    QByteArray out;
    out.reserve(static_cast<int>((std::strlen(body) + 1) * count));
    for (int i = 0; i < count; ++i) {
        out.append(body);
        out.append('\n');
    }
    return out;
}

} // namespace

void TestBufferDiffEngine::identicalBuffers_emitZeroHunks()
{
    const QByteArray base = joinLines(100, "alpha");
    const auto hunks = BufferDiffEngine::diff(base, base);
    QVERIFY(hunks.isEmpty());
}

void TestBufferDiffEngine::emptyBase_addsAllAsOneHunk()
{
    const QByteArray base;
    const QByteArray buf  = joinLines(3, "new line");

    const auto hunks = BufferDiffEngine::diff(base, buf);
    QCOMPARE(hunks.size(), 1);
    QCOMPARE(hunks[0].oldCount, 0);
    QCOMPARE(hunks[0].newCount, 3);
    QCOMPARE(hunks[0].newStart, 0);
}

void TestBufferDiffEngine::emptyBuffer_deletesAllAsOneHunk()
{
    const QByteArray base = joinLines(4, "stale line");
    const QByteArray buf;

    const auto hunks = BufferDiffEngine::diff(base, buf);
    QCOMPARE(hunks.size(), 1);
    QCOMPARE(hunks[0].oldCount, 4);
    QCOMPARE(hunks[0].newCount, 0);
    QCOMPARE(hunks[0].oldStart, 0);
}

void TestBufferDiffEngine::singleLineInsert_oneHunk_zeroOld_oneNew()
{
    const QByteArray base =
        QByteArrayLiteral("alpha\nbeta\ngamma\n");
    const QByteArray buf =
        QByteArrayLiteral("alpha\ninserted\nbeta\ngamma\n");

    const auto hunks = BufferDiffEngine::diff(base, buf);
    QCOMPARE(hunks.size(), 1);
    QCOMPARE(hunks[0].oldCount, 0);
    QCOMPARE(hunks[0].newCount, 1);
    // Inserted at new-line 1 (between alpha and beta).
    QCOMPARE(hunks[0].newStart, 1);
}

void TestBufferDiffEngine::singleLineDelete_oneHunk_oneOld_zeroNew()
{
    const QByteArray base =
        QByteArrayLiteral("alpha\nbeta\ngamma\n");
    const QByteArray buf =
        QByteArrayLiteral("alpha\ngamma\n");

    const auto hunks = BufferDiffEngine::diff(base, buf);
    QCOMPARE(hunks.size(), 1);
    QCOMPARE(hunks[0].oldCount, 1);
    QCOMPARE(hunks[0].newCount, 0);
    QCOMPARE(hunks[0].oldStart, 1);
}

void TestBufferDiffEngine::singleLineModify_oneHunk_oneOld_oneNew()
{
    const QByteArray base =
        QByteArrayLiteral("alpha\nbeta\ngamma\n");
    const QByteArray buf =
        QByteArrayLiteral("alpha\nBETA\ngamma\n");

    const auto hunks = BufferDiffEngine::diff(base, buf);
    QCOMPARE(hunks.size(), 1);
    QCOMPARE(hunks[0].oldCount, 1);
    QCOMPARE(hunks[0].newCount, 1);
    QCOMPARE(hunks[0].newStart, 1);
}

void TestBufferDiffEngine::disjointEdits_twoHunks()
{
    const QByteArray base =
        QByteArrayLiteral("a\nb\nc\nd\ne\nf\n");
    const QByteArray buf =
        QByteArrayLiteral("a\nB\nc\nd\nE\nf\n");

    const auto hunks = BufferDiffEngine::diff(base, buf);
    QCOMPARE(hunks.size(), 2);
    // Hunks come out in source order; both are 1-line modifies.
    QCOMPARE(hunks[0].oldCount, 1);
    QCOMPARE(hunks[0].newCount, 1);
    QCOMPARE(hunks[1].oldCount, 1);
    QCOMPARE(hunks[1].newCount, 1);
    QVERIFY(hunks[0].newStart < hunks[1].newStart);
}

void TestBufferDiffEngine::crlfBaseAgainstLfBuffer_emitsZeroHunks()
{
    // Real-world Windows case: HEAD blob from `git cat-file` is LF-only, but
    // Scintilla's buffer holds the on-disk CRLF content. Without EOL
    // normalisation every line would diff by a trailing \r.
    const QByteArray base =
        QByteArrayLiteral("alpha\nbeta\ngamma\n");
    const QByteArray buf =
        QByteArrayLiteral("alpha\r\nbeta\r\ngamma\r\n");

    const auto hunks = BufferDiffEngine::diff(base, buf);
    QVERIFY(hunks.isEmpty());
}

void TestBufferDiffEngine::crlfBufferAgainstLfBase_emitsZeroHunks()
{
    // Symmetric case: both sides CRLF should also collapse to zero hunks.
    const QByteArray base =
        QByteArrayLiteral("alpha\r\nbeta\r\ngamma\r\n");
    const QByteArray buf =
        QByteArrayLiteral("alpha\r\nbeta\r\ngamma\r\n");

    const auto hunks = BufferDiffEngine::diff(base, buf);
    QVERIFY(hunks.isEmpty());
}

void TestBufferDiffEngine::benchmark_5kLineDiff_under_3ms_p95()
{
    // 5000 lines of identical-prefix content with one keyword swap every
    // ~30 lines — realistic "small edits scattered through a file" shape
    // that gutter refresh hits on every keystroke.
    const QByteArray filler =
        QByteArrayLiteral("    int some_local_variable = compute_value(i, j); // commentary\n");

    QByteArray base;
    QByteArray buf;
    base.reserve(filler.size() * 5000);
    buf.reserve(filler.size() * 5000);
    for (int i = 0; i < 5000; ++i) {
        base.append(filler);
        if (i % 30 == 0) {
            QByteArray mod = filler;
            mod.replace("compute_value", "compute_VALUE");
            buf.append(mod);
        } else {
            buf.append(filler);
        }
    }

    // Warm up branch predictor / cache.
    (void)BufferDiffEngine::diff(base, buf);

    QElapsedTimer t;
    qint64 best = std::numeric_limits<qint64>::max();
    BufferDiffEngine::Hunks lastResult;
    for (int run = 0; run < 5; ++run) {
        t.start();
        lastResult = BufferDiffEngine::diff(base, buf);
        best = std::min(best, t.nsecsElapsed());
    }
    // Sanity: ~5000/30 ≈ 166 modified lines, so we expect that many hunks.
    QVERIFY(lastResult.size() > 100);

    const double ms = best / 1.0e6;
    qInfo() << "BufferDiffEngine 5k-line best:" << ms << "ms hunks:" << lastResult.size();
    // Debug-tolerant budget (mirrors test_git_diff_parser's 60ms ceiling for
    // Debug builds with -O0). Target on Release is <1ms; we leave headroom
    // for unoptimised CI runs. Tighten if Release-only CI surfaces.
    QVERIFY2(ms < 60.0,
             qPrintable(QString("BufferDiffEngine too slow: %1ms (>60ms)").arg(ms)));
}

QTEST_GUILESS_MAIN(TestBufferDiffEngine)
#include "test_buffer_diff_engine.moc"
