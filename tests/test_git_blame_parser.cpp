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
#include <QElapsedTimer>
#include <QString>
#include <QTest>
#include <QtTest/QtTest>

#include <limits>

class TestGitBlameParser : public QObject
{
    Q_OBJECT
private slots:
    void emptyInput_emitsZeroLines();
    void singleLine_extractsMetadataAndContent();
    void multipleLinesSameCommit_secondHeaderShortForm();
    void twoCommitsInterleaved_recordsDedupedBySha();
    void boundaryFlag_setsOnRootCommit();
    void truncatedInput_partialResult();
    void benchmark_5kLines_under_60ms();
};

void TestGitBlameParser::emptyInput_emitsZeroLines()
{
    const auto r = GitBlameParser::parse({});
    QVERIFY(r.records.isEmpty());
    QVERIFY(r.lines.isEmpty());
}

void TestGitBlameParser::singleLine_extractsMetadataAndContent()
{
    const QByteArray in = QByteArrayLiteral(
        "fe5a4f70b8c5c4a83eb6c6f57b3b3b9d8e2a3c4d 1 1 1\n"
        "author Alice Example\n"
        "author-mail <alice@example.com>\n"
        "author-time 1700000000\n"
        "author-tz -0500\n"
        "committer Alice Example\n"
        "committer-mail <alice@example.com>\n"
        "committer-time 1700000000\n"
        "committer-tz -0500\n"
        "summary Initial scaffold\n"
        "filename src/foo.cpp\n"
        "\t#include \"foo.h\"\n");
    const auto r = GitBlameParser::parse(in);
    QCOMPARE(r.records.size(), 1);
    QCOMPARE(r.records[0].author, QStringLiteral("Alice Example"));
    QCOMPARE(r.records[0].authorTime, qint64(1700000000));
    QCOMPARE(r.records[0].summary, QStringLiteral("Initial scaffold"));
    QVERIFY(!r.records[0].boundary);

    QCOMPARE(r.lines.size(), 1);
    QCOMPARE(r.lines[0].lineIdx, 0);
    QCOMPARE(r.lines[0].recordIdx, 0);
}

void TestGitBlameParser::multipleLinesSameCommit_secondHeaderShortForm()
{
    const QByteArray in = QByteArrayLiteral(
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa 1 1 2\n"
        "author Bob\n"
        "author-time 1\n"
        "summary first commit\n"
        "filename src/x.cpp\n"
        "\tline one\n"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa 2 2\n"
        "\tline two\n");
    const auto r = GitBlameParser::parse(in);
    QCOMPARE(r.records.size(), 1);
    QCOMPARE(r.lines.size(), 2);
    QCOMPARE(r.lines[0].lineIdx, 0);
    QCOMPARE(r.lines[1].lineIdx, 1);
    QCOMPARE(r.lines[0].recordIdx, 0);
    QCOMPARE(r.lines[1].recordIdx, 0);
}

void TestGitBlameParser::twoCommitsInterleaved_recordsDedupedBySha()
{
    const QByteArray in = QByteArrayLiteral(
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa 1 1 1\n"
        "author Alice\n"
        "author-time 100\n"
        "summary A\n"
        "filename src/y.cpp\n"
        "\tA line\n"
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb 2 2 1\n"
        "author Bob\n"
        "author-time 200\n"
        "summary B\n"
        "filename src/y.cpp\n"
        "\tB line\n"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa 3 3\n"
        "\tA line again\n");
    const auto r = GitBlameParser::parse(in);
    QCOMPARE(r.records.size(), 2);
    QCOMPARE(r.lines.size(), 3);
    QCOMPARE(r.lines[0].recordIdx, 0);
    QCOMPARE(r.lines[1].recordIdx, 1);
    QCOMPARE(r.lines[2].recordIdx, 0); // back to first sha
    QCOMPARE(r.records[0].author, QStringLiteral("Alice"));
    QCOMPARE(r.records[1].author, QStringLiteral("Bob"));
}

void TestGitBlameParser::boundaryFlag_setsOnRootCommit()
{
    const QByteArray in = QByteArrayLiteral(
        "1111111111111111111111111111111111111111 1 1 1\n"
        "author Root\n"
        "author-time 1\n"
        "summary Initial commit\n"
        "boundary\n"
        "filename src/r.cpp\n"
        "\tcontent\n");
    const auto r = GitBlameParser::parse(in);
    QCOMPARE(r.records.size(), 1);
    QVERIFY(r.records[0].boundary);
}

void TestGitBlameParser::truncatedInput_partialResult()
{
    // Truncated mid-metadata: parser should keep what it has so far.
    const QByteArray in = QByteArrayLiteral(
        "1111111111111111111111111111111111111111 1 1 1\n"
        "author Trunc\n"
        "author-time 42\n");
    const auto r = GitBlameParser::parse(in);
    QCOMPARE(r.records.size(), 1);
    QCOMPARE(r.records[0].author, QStringLiteral("Trunc"));
    // No content line → no Line entry.
    QVERIFY(r.lines.isEmpty());
}

void TestGitBlameParser::benchmark_5kLines_under_60ms()
{
    // 5000 lines spread across ~50 commits — realistic for a long-lived
    // source file. Each commit costs one metadata block + many short-form
    // header repeats.
    QByteArray buf;
    buf.reserve(5000 * 80);
    for (int commit = 0; commit < 50; ++commit) {
        // 40-char hex SHA — zero-pad the commit number so each is unique.
        const QByteArray sha = QByteArray::number(commit, 16).rightJustified(40, '0');
        // 100 lines per commit.
        for (int li = 0; li < 100; ++li) {
            const int finalLine = commit * 100 + li + 1;
            buf.append(sha);
            buf.append(' ');
            buf.append(QByteArray::number(finalLine));
            buf.append(' ');
            buf.append(QByteArray::number(finalLine));
            if (li == 0) {
                buf.append(" 100\n");
                buf.append("author Author").append(QByteArray::number(commit)).append('\n');
                buf.append("author-time ").append(QByteArray::number(1700000000 + commit)).append('\n');
                buf.append("summary commit ").append(QByteArray::number(commit)).append('\n');
                buf.append("filename src/foo.cpp\n");
            } else {
                buf.append('\n');
            }
            buf.append("\tsome line content here\n");
        }
    }

    (void)GitBlameParser::parse(buf); // warm-up

    QElapsedTimer t;
    qint64 best = std::numeric_limits<qint64>::max();
    GitBlameParser::Result last;
    for (int run = 0; run < 3; ++run) {
        t.start();
        last = GitBlameParser::parse(buf);
        best = std::min(best, t.nsecsElapsed());
    }
    QCOMPARE(last.records.size(), 50);
    QCOMPARE(last.lines.size(), 5000);

    const double ms = best / 1.0e6;
    qInfo() << "GitBlameParser 5k-line best:" << ms << "ms";
    QVERIFY2(ms < 60.0,
             qPrintable(QString("Blame parser too slow: %1ms (>60ms)").arg(ms)));
}

QTEST_GUILESS_MAIN(TestGitBlameParser)
#include "test_git_blame_parser.moc"
