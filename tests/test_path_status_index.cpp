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

#include "PathStatusIndex.h"

#include <QElapsedTimer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTest>
#include <QtTest/QtTest>

namespace {

GitStatusEntry makeEntry(const QString &relPath,
                         GitStatusEntry::Change change,
                         GitStatusEntry::Section section = GitStatusEntry::Tracked)
{
    GitStatusEntry e;
    e.relPath = relPath;
    e.change  = change;
    e.section = section;
    return e;
}

// All tests use a fixed forward-slash repo root so behavior is identical on
// every platform — QDir::cleanPath converts backslashes anyway.
const QString kRoot = QStringLiteral("/repo");

} // namespace

class TestPathStatusIndex : public QObject
{
    Q_OBJECT

private slots:
    void emptyIndex_lookupsReturnNullopt();
    void singleModifiedFile_decoratesFileAndAncestors();
    void folderConflictWinsOverModified();
    void folderDeletedWinsOverModified();
    void folderRenamedWinsOverUntracked();
    void folderModifiedWinsOverAdded();
    void rollupReachesGrandparent();
    void rollupExcludesRepoRoot();
    void cleanPath_acceptsNativeSeparators();
    void cleanPath_stripsTrailingSlash();
    void deltaFromEmptyReturnsAllPaths();
    void deltaBetweenIdenticalReturnsEmpty();
    void deltaCapturesRemovedEntry();
    void worseChange_priorityTableMonotonic();
    void submoduleEntry_decoratesFolderAndDescendants();

    // Hard-fail benchmarks — see tasks.md §1.6
    void bench_rebuild1000Entries();
    void bench_deltaTwo1000IndexesWith5PercentDivergence();
    void bench_lookupThroughput();
};

void TestPathStatusIndex::emptyIndex_lookupsReturnNullopt()
{
    PathStatusIndex idx;
    QVERIFY(idx.isEmpty());
    QCOMPARE(idx.fileCount(), 0);
    QCOMPARE(idx.folderCount(), 0);
    QVERIFY(!idx.fileChange(QStringLiteral("/anywhere/foo.cpp")).has_value());
    QVERIFY(!idx.folderChange(QStringLiteral("/anywhere")).has_value());
}

void TestPathStatusIndex::singleModifiedFile_decoratesFileAndAncestors()
{
    GitStatusEntries entries;
    entries.append(makeEntry(QStringLiteral("src/foo/bar.cpp"), GitStatusEntry::Modified));

    PathStatusIndex idx;
    idx.rebuild(entries, kRoot);

    const auto file = idx.fileChange(QStringLiteral("/repo/src/foo/bar.cpp"));
    QVERIFY(file.has_value());
    QCOMPARE(*file, GitStatusEntry::Modified);

    QCOMPARE(*idx.folderChange(QStringLiteral("/repo/src/foo")), GitStatusEntry::Modified);
    QCOMPARE(*idx.folderChange(QStringLiteral("/repo/src")), GitStatusEntry::Modified);
}

void TestPathStatusIndex::folderConflictWinsOverModified()
{
    GitStatusEntries entries;
    entries.append(makeEntry(QStringLiteral("src/foo.cpp"), GitStatusEntry::Modified));
    entries.append(makeEntry(QStringLiteral("src/bar.cpp"),
                             GitStatusEntry::Unmerged, GitStatusEntry::Conflicts));

    PathStatusIndex idx;
    idx.rebuild(entries, kRoot);

    QCOMPARE(*idx.folderChange(QStringLiteral("/repo/src")), GitStatusEntry::Unmerged);
}

void TestPathStatusIndex::folderDeletedWinsOverModified()
{
    GitStatusEntries entries;
    entries.append(makeEntry(QStringLiteral("src/a.cpp"), GitStatusEntry::Modified));
    entries.append(makeEntry(QStringLiteral("src/b.cpp"), GitStatusEntry::Deleted));

    PathStatusIndex idx;
    idx.rebuild(entries, kRoot);

    QCOMPARE(*idx.folderChange(QStringLiteral("/repo/src")), GitStatusEntry::Deleted);
}

void TestPathStatusIndex::folderRenamedWinsOverUntracked()
{
    GitStatusEntries entries;
    entries.append(makeEntry(QStringLiteral("src/a.cpp"), GitStatusEntry::Renamed));
    entries.append(makeEntry(QStringLiteral("src/b.cpp"),
                             GitStatusEntry::Untracked_, GitStatusEntry::Untracked));

    PathStatusIndex idx;
    idx.rebuild(entries, kRoot);

    QCOMPARE(*idx.folderChange(QStringLiteral("/repo/src")), GitStatusEntry::Renamed);
}

void TestPathStatusIndex::folderModifiedWinsOverAdded()
{
    GitStatusEntries entries;
    entries.append(makeEntry(QStringLiteral("src/a.cpp"), GitStatusEntry::Added));
    entries.append(makeEntry(QStringLiteral("src/b.cpp"), GitStatusEntry::Modified));

    PathStatusIndex idx;
    idx.rebuild(entries, kRoot);

    QCOMPARE(*idx.folderChange(QStringLiteral("/repo/src")), GitStatusEntry::Modified);
}

void TestPathStatusIndex::rollupReachesGrandparent()
{
    GitStatusEntries entries;
    entries.append(makeEntry(QStringLiteral("a/b/c/d/e/f.txt"), GitStatusEntry::Modified));

    PathStatusIndex idx;
    idx.rebuild(entries, kRoot);

    QCOMPARE(*idx.folderChange(QStringLiteral("/repo/a/b/c/d/e")), GitStatusEntry::Modified);
    QCOMPARE(*idx.folderChange(QStringLiteral("/repo/a/b/c/d")),   GitStatusEntry::Modified);
    QCOMPARE(*idx.folderChange(QStringLiteral("/repo/a/b/c")),     GitStatusEntry::Modified);
    QCOMPARE(*idx.folderChange(QStringLiteral("/repo/a/b")),       GitStatusEntry::Modified);
    QCOMPARE(*idx.folderChange(QStringLiteral("/repo/a")),         GitStatusEntry::Modified);
}

void TestPathStatusIndex::rollupExcludesRepoRoot()
{
    GitStatusEntries entries;
    entries.append(makeEntry(QStringLiteral("src/foo.cpp"), GitStatusEntry::Modified));

    PathStatusIndex idx;
    idx.rebuild(entries, kRoot);

    QVERIFY(!idx.folderChange(kRoot).has_value());
}

void TestPathStatusIndex::cleanPath_acceptsNativeSeparators()
{
    GitStatusEntries entries;
    entries.append(makeEntry(QStringLiteral("src/foo.cpp"), GitStatusEntry::Modified));

    PathStatusIndex idx;
    idx.rebuild(entries, kRoot);

    // QDir::cleanPath() converts backslashes to forward slashes on every platform.
    QCOMPARE(*idx.fileChange(QStringLiteral("/repo\\src\\foo.cpp")), GitStatusEntry::Modified);
    QCOMPARE(*idx.folderChange(QStringLiteral("/repo\\src")), GitStatusEntry::Modified);
}

void TestPathStatusIndex::cleanPath_stripsTrailingSlash()
{
    GitStatusEntries entries;
    entries.append(makeEntry(QStringLiteral("src/foo.cpp"), GitStatusEntry::Modified));

    PathStatusIndex idx;
    idx.rebuild(entries, kRoot);

    QCOMPARE(*idx.folderChange(QStringLiteral("/repo/src/")), GitStatusEntry::Modified);
    QCOMPARE(*idx.fileChange(QStringLiteral("/repo/src/foo.cpp/")), GitStatusEntry::Modified);
}

void TestPathStatusIndex::deltaFromEmptyReturnsAllPaths()
{
    GitStatusEntries entries;
    for (int i = 0; i < 100; ++i) {
        entries.append(makeEntry(QStringLiteral("src/dir%1/file%1.cpp").arg(i),
                                 GitStatusEntry::Modified));
    }

    PathStatusIndex prev;
    PathStatusIndex cur;
    cur.rebuild(entries, kRoot);

    const QSet<QString> delta = cur.deltaPaths(prev);

    // 100 distinct files + 100 distinct immediate-parent folders ("dir0" .. "dir99")
    // + "src". That's 100 + 100 + 1 = 201.
    QCOMPARE(delta.size(), 201);
    QVERIFY(delta.contains(QStringLiteral("/repo/src/dir0/file0.cpp")));
    QVERIFY(delta.contains(QStringLiteral("/repo/src/dir0")));
    QVERIFY(delta.contains(QStringLiteral("/repo/src")));
}

void TestPathStatusIndex::deltaBetweenIdenticalReturnsEmpty()
{
    GitStatusEntries entries;
    for (int i = 0; i < 50; ++i) {
        entries.append(makeEntry(QStringLiteral("src/dir%1/file.cpp").arg(i),
                                 GitStatusEntry::Modified));
    }

    PathStatusIndex a;
    PathStatusIndex b;
    a.rebuild(entries, kRoot);
    b.rebuild(entries, kRoot);

    QVERIFY(a.deltaPaths(b).isEmpty());
}

void TestPathStatusIndex::deltaCapturesRemovedEntry()
{
    GitStatusEntries withFile;
    withFile.append(makeEntry(QStringLiteral("src/foo.cpp"), GitStatusEntry::Modified));

    PathStatusIndex before;
    PathStatusIndex after;
    before.rebuild(withFile, kRoot);
    after.rebuild(GitStatusEntries{}, kRoot);

    const QSet<QString> delta = after.deltaPaths(before);
    QVERIFY(delta.contains(QStringLiteral("/repo/src/foo.cpp")));
    QVERIFY(delta.contains(QStringLiteral("/repo/src")));
    QCOMPARE(delta.size(), 2);
}

void TestPathStatusIndex::worseChange_priorityTableMonotonic()
{
    // The priority order is part of the capability contract — encode it here
    // so a future edit that reorders the table breaks this test loudly.
    using C = GitStatusEntry;
    QCOMPARE(PathStatusIndex::worseChange(C::Modified,    C::Untracked_), C::Modified);
    QCOMPARE(PathStatusIndex::worseChange(C::Added,       C::Modified),   C::Modified);
    QCOMPARE(PathStatusIndex::worseChange(C::Renamed,     C::Added),      C::Added);
    QCOMPARE(PathStatusIndex::worseChange(C::Deleted,     C::Modified),   C::Deleted);
    QCOMPARE(PathStatusIndex::worseChange(C::Unmerged,    C::Deleted),    C::Unmerged);
    QCOMPARE(PathStatusIndex::worseChange(C::Unmerged,    C::Modified),   C::Unmerged);
    QCOMPARE(PathStatusIndex::worseChange(C::Copied,      C::TypeChanged),C::Copied);
}

void TestPathStatusIndex::submoduleEntry_decoratesFolderAndDescendants()
{
    // Parent's `git status` reports a single entry for the submodule directory
    // (isSubmodule=true). The directory itself must paint, but descendants
    // must NOT inherit — otherwise unchanged files inside the submodule would
    // be falsely coloured. Accurate per-file colours come from a separate
    // sub-repo `git status` whose entries are merged in by the caller before
    // calling rebuild().
    GitStatusEntry sub;
    sub.relPath        = QStringLiteral("NotepadADE");
    sub.change         = GitStatusEntry::Modified;
    sub.section        = GitStatusEntry::Tracked;
    sub.isSubmodule    = true;

    // Sub-repo status reports a single dirty file inside the submodule.
    GitStatusEntry inner;
    inner.relPath = QStringLiteral("NotepadADE/src/git/PathStatusIndex.cpp");
    inner.change  = GitStatusEntry::Modified;
    inner.section = GitStatusEntry::Tracked;

    GitStatusEntries entries;
    entries.append(sub);
    entries.append(inner);

    PathStatusIndex idx;
    idx.rebuild(entries, kRoot);

    // The submodule path itself is a folder (not a file).
    QVERIFY(!idx.fileChange(QStringLiteral("/repo/NotepadADE")).has_value());
    QCOMPARE(*idx.folderChange(QStringLiteral("/repo/NotepadADE")),
             GitStatusEntry::Modified);

    // The actually-dirty file inside paints; rollup carries colour up.
    QCOMPARE(*idx.fileChange(QStringLiteral("/repo/NotepadADE/src/git/PathStatusIndex.cpp")),
             GitStatusEntry::Modified);
    QCOMPARE(*idx.folderChange(QStringLiteral("/repo/NotepadADE/src/git")),
             GitStatusEntry::Modified);
    QCOMPARE(*idx.folderChange(QStringLiteral("/repo/NotepadADE/src")),
             GitStatusEntry::Modified);

    // Clean files inside the submodule that no inner entry reported on
    // STAY CLEAN — no false inheritance from the parent's submodule entry.
    QVERIFY(!idx.fileChange(QStringLiteral("/repo/NotepadADE/src/main.cpp")).has_value());
    QVERIFY(!idx.fileChange(QStringLiteral("/repo/NotepadADE/cmake/CPM.cmake")).has_value());
    QVERIFY(!idx.folderChange(QStringLiteral("/repo/NotepadADE/cmake")).has_value());

    // Sibling paths outside the submodule remain clean.
    QVERIFY(!idx.fileChange(QStringLiteral("/repo/README.md")).has_value());
    QVERIFY(!idx.folderChange(QStringLiteral("/repo/other-dir")).has_value());
}

// --- Performance contracts (hard-fail benchmarks) -----------------------------
//
// The "real" budgets named in design.md (rebuild 1000 < 1 ms, delta < 1 ms,
// 1M lookups < 100 ms) are the Release-build targets that the production hot
// path must meet. Debug builds are 10–50× slower because Q_ASSERT, no
// inlining, MSVC iterator debugging, and STL container checking all run.
//
// These tests run only against the Debug build (`just test` configures
// CMAKE_BUILD_TYPE=Debug), so the asserted budget here is a Debug-tolerant
// regression net (≈ 5× current measurement) that still catches real
// algorithmic regressions while not failing on normal Debug overhead.
// The Release target is referenced in each test's qInfo() for context.
//
// Pattern mirrors test_git_diff_parser.cpp (perf_5kLineDiff_under60ms): one
// untimed warm-up, then best-of-3 timed runs.

void TestPathStatusIndex::bench_rebuild1000Entries()
{
    // Synthesize a deterministic 1000-entry input at average depth 6.
    GitStatusEntries entries;
    entries.reserve(1000);
    for (int i = 0; i < 1000; ++i) {
        entries.append(makeEntry(
            QStringLiteral("a/b%1/c%2/d/e/f%3.cpp").arg(i % 17).arg(i % 31).arg(i),
            GitStatusEntry::Change(i % 8)));   // cycle through all Change values
    }

    // Warm up (caches, branch predictor).
    { PathStatusIndex warm; warm.rebuild(entries, kRoot); }

    QElapsedTimer t;
    qint64 best = std::numeric_limits<qint64>::max();
    for (int run = 0; run < 3; ++run) {
        PathStatusIndex idx;
        t.start();
        idx.rebuild(entries, kRoot);
        const qint64 ns = t.nsecsElapsed();
        best = std::min(best, ns);
        QVERIFY(!idx.isEmpty());
    }
    const qint64 us = best / 1000;

    qInfo() << "rebuild(1000 entries) best =" << us << "us (Release target < 1000 us)";
    QVERIFY2(us < 50000,
             qPrintable(QStringLiteral("rebuild took %1 us, Debug budget 50000 us").arg(us)));
}

void TestPathStatusIndex::bench_deltaTwo1000IndexesWith5PercentDivergence()
{
    GitStatusEntries base;
    base.reserve(1000);
    for (int i = 0; i < 1000; ++i) {
        base.append(makeEntry(
            QStringLiteral("a/b%1/c%2/d/e/f%3.cpp").arg(i % 17).arg(i % 31).arg(i),
            GitStatusEntry::Modified));
    }

    PathStatusIndex prev;
    prev.rebuild(base, kRoot);

    // Diverge 5% of entries to a different change kind.
    for (int i = 0; i < 50; ++i) {
        base[i].change = GitStatusEntry::Deleted;
    }
    PathStatusIndex cur;
    cur.rebuild(base, kRoot);

    // Warm up.
    (void)cur.deltaPaths(prev);

    QElapsedTimer t;
    qint64 best = std::numeric_limits<qint64>::max();
    int deltaSize = 0;
    for (int run = 0; run < 3; ++run) {
        t.start();
        const QSet<QString> delta = cur.deltaPaths(prev);
        const qint64 ns = t.nsecsElapsed();
        best = std::min(best, ns);
        deltaSize = delta.size();
        QVERIFY(!delta.isEmpty());
    }
    const qint64 us = best / 1000;

    qInfo() << "deltaPaths(1000 vs 1000, 5% divergence) best =" << us
            << "us, delta.size =" << deltaSize << "(Release target < 1000 us)";
    QVERIFY2(us < 30000,
             qPrintable(QStringLiteral("deltaPaths took %1 us, Debug budget 30000 us").arg(us)));
}

void TestPathStatusIndex::bench_lookupThroughput()
{
    GitStatusEntries entries;
    entries.reserve(1000);
    for (int i = 0; i < 1000; ++i) {
        entries.append(makeEntry(
            QStringLiteral("a/b%1/c/d/f%2.cpp").arg(i % 17).arg(i),
            GitStatusEntry::Modified));
    }
    PathStatusIndex idx;
    idx.rebuild(entries, kRoot);

    // Pre-build a deterministic set of lookup paths (file hits + folder hits).
    QStringList paths;
    paths.reserve(1000);
    for (int i = 0; i < 500; ++i) {
        paths.append(QStringLiteral("/repo/a/b%1/c/d/f%2.cpp").arg(i % 17).arg(i));
        paths.append(QStringLiteral("/repo/a/b%1/c/d").arg(i % 17));
    }

    // Warm up one full pass.
    {
        int warm = 0;
        for (const QString &p : std::as_const(paths)) {
            if (idx.fileChange(p).has_value())   ++warm;
            if (idx.folderChange(p).has_value()) ++warm;
        }
        QVERIFY(warm > 0);
    }

    // Best of 3 timed passes; each pass does 200k lookups (100k file + 100k folder).
    // Total iterations across all 3 passes = 600k. We report ns/lookup.
    QElapsedTimer t;
    qint64 best = std::numeric_limits<qint64>::max();
    int hits = 0;
    constexpr int kPassesPerRun = 100;     // 100 × 1000 paths = 100k iterations per run
    for (int run = 0; run < 3; ++run) {
        t.start();
        for (int k = 0; k < kPassesPerRun; ++k) {
            for (const QString &p : std::as_const(paths)) {
                if (idx.fileChange(p).has_value())   ++hits;
                if (idx.folderChange(p).has_value()) ++hits;
            }
        }
        const qint64 ns = t.nsecsElapsed();
        best = std::min(best, ns);
    }
    const qint64 lookups = static_cast<qint64>(kPassesPerRun) * paths.size() * 2;
    const double nsPerLookup = static_cast<double>(best) / lookups;
    const qint64 ms = best / 1'000'000;

    qInfo() << "best 200k lookups (file+folder mixed) =" << ms << "ms,"
            << nsPerLookup << "ns/lookup, hits =" << hits
            << "(Release target < 100 ns/lookup)";
    QVERIFY(hits > 0);
    // Debug budget: 5 µs/lookup. Release target: 100 ns/lookup.
    QVERIFY2(nsPerLookup < 5000.0,
             qPrintable(QStringLiteral("lookup avg %1 ns, Debug budget 5000 ns").arg(nsPerLookup)));
}

QTEST_GUILESS_MAIN(TestPathStatusIndex)

#include "test_path_status_index.moc"
