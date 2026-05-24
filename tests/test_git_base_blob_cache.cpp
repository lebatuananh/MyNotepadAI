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

#include "GitBaseBlobCache.h"

#include <QByteArray>
#include <QString>
#include <QTest>
#include <QtTest/QtTest>

class TestGitBaseBlobCache : public QObject
{
    Q_OBJECT
private slots:
    void init();              // fresh-cache guarantee per test
    void putGet_roundTrips();
    void miss_returnsEmpty();
    void invalidate_removesEntry();
    void invalidateRepo_removesAllForThatRepo_keepsOthers();
    void lruEviction_dropsLeastRecentlyUsed();
    void getPromotesToMostRecentlyUsed();
    void shrinkBudget_evictsImmediately();
    void replaceSamePath_updatesBytesAccount();
};

namespace {

QByteArray bigBlob(int sizeKiB, char fill)
{
    return QByteArray(sizeKiB * 1024, fill);
}

} // namespace

void TestGitBaseBlobCache::init()
{
    GitBaseBlobCache::instance().clear();
    GitBaseBlobCache::instance().setMaxBytes(16 * 1024 * 1024);
}

void TestGitBaseBlobCache::putGet_roundTrips()
{
    auto &c = GitBaseBlobCache::instance();
    c.put(QStringLiteral("/repo"), QStringLiteral("a.cpp"),
          QByteArrayLiteral("hello world"));

    QCOMPARE(c.get(QStringLiteral("/repo"), QStringLiteral("a.cpp")),
             QByteArrayLiteral("hello world"));
    QCOMPARE(c.byteCount(), qsizetype(11));
}

void TestGitBaseBlobCache::miss_returnsEmpty()
{
    auto &c = GitBaseBlobCache::instance();
    QVERIFY(c.get(QStringLiteral("/nope"), QStringLiteral("x.cpp")).isEmpty());
}

void TestGitBaseBlobCache::invalidate_removesEntry()
{
    auto &c = GitBaseBlobCache::instance();
    c.put(QStringLiteral("/repo"), QStringLiteral("a.cpp"), QByteArrayLiteral("body"));
    c.invalidate(QStringLiteral("/repo"), QStringLiteral("a.cpp"));
    QVERIFY(c.get(QStringLiteral("/repo"), QStringLiteral("a.cpp")).isEmpty());
    QCOMPARE(c.byteCount(), qsizetype(0));
}

void TestGitBaseBlobCache::invalidateRepo_removesAllForThatRepo_keepsOthers()
{
    auto &c = GitBaseBlobCache::instance();
    c.put(QStringLiteral("/r1"), QStringLiteral("a.cpp"), QByteArrayLiteral("A"));
    c.put(QStringLiteral("/r1"), QStringLiteral("b.cpp"), QByteArrayLiteral("B"));
    c.put(QStringLiteral("/r2"), QStringLiteral("c.cpp"), QByteArrayLiteral("C"));

    c.invalidateRepo(QStringLiteral("/r1"));

    QVERIFY(c.get(QStringLiteral("/r1"), QStringLiteral("a.cpp")).isEmpty());
    QVERIFY(c.get(QStringLiteral("/r1"), QStringLiteral("b.cpp")).isEmpty());
    QCOMPARE(c.get(QStringLiteral("/r2"), QStringLiteral("c.cpp")),
             QByteArrayLiteral("C"));
    QCOMPARE(c.byteCount(), qsizetype(1));
}

void TestGitBaseBlobCache::lruEviction_dropsLeastRecentlyUsed()
{
    auto &c = GitBaseBlobCache::instance();
    // 4 entries × 4 KiB; budget set to 12 KiB so inserting the fourth must
    // evict the first.
    c.setMaxBytes(12 * 1024);

    c.put(QStringLiteral("/r"), QStringLiteral("1"), bigBlob(4, '1'));
    c.put(QStringLiteral("/r"), QStringLiteral("2"), bigBlob(4, '2'));
    c.put(QStringLiteral("/r"), QStringLiteral("3"), bigBlob(4, '3'));
    // Insert #4 — total would be 16 KiB; LRU (= #1) must go.
    c.put(QStringLiteral("/r"), QStringLiteral("4"), bigBlob(4, '4'));

    QVERIFY(c.get(QStringLiteral("/r"), QStringLiteral("1")).isEmpty());
    QCOMPARE(c.get(QStringLiteral("/r"), QStringLiteral("2")).size(), qsizetype(4096));
    QCOMPARE(c.get(QStringLiteral("/r"), QStringLiteral("3")).size(), qsizetype(4096));
    QCOMPARE(c.get(QStringLiteral("/r"), QStringLiteral("4")).size(), qsizetype(4096));
    QCOMPARE(c.byteCount(), qsizetype(12 * 1024));
}

void TestGitBaseBlobCache::getPromotesToMostRecentlyUsed()
{
    auto &c = GitBaseBlobCache::instance();
    c.setMaxBytes(12 * 1024);

    c.put(QStringLiteral("/r"), QStringLiteral("1"), bigBlob(4, '1'));
    c.put(QStringLiteral("/r"), QStringLiteral("2"), bigBlob(4, '2'));
    c.put(QStringLiteral("/r"), QStringLiteral("3"), bigBlob(4, '3'));
    // Touch #1 — it now becomes MRU; #2 is the new LRU.
    QCOMPARE(c.get(QStringLiteral("/r"), QStringLiteral("1")).size(), qsizetype(4096));
    c.put(QStringLiteral("/r"), QStringLiteral("4"), bigBlob(4, '4'));

    QCOMPARE(c.get(QStringLiteral("/r"), QStringLiteral("1")).size(), qsizetype(4096));
    QVERIFY(c.get(QStringLiteral("/r"), QStringLiteral("2")).isEmpty());
}

void TestGitBaseBlobCache::shrinkBudget_evictsImmediately()
{
    auto &c = GitBaseBlobCache::instance();
    c.setMaxBytes(16 * 1024);
    c.put(QStringLiteral("/r"), QStringLiteral("1"), bigBlob(4, '1'));
    c.put(QStringLiteral("/r"), QStringLiteral("2"), bigBlob(4, '2'));
    c.put(QStringLiteral("/r"), QStringLiteral("3"), bigBlob(4, '3'));
    c.put(QStringLiteral("/r"), QStringLiteral("4"), bigBlob(4, '4'));
    QCOMPARE(c.byteCount(), qsizetype(16 * 1024));

    c.setMaxBytes(8 * 1024);
    QCOMPARE(c.byteCount(), qsizetype(8 * 1024));
    // Oldest two must be gone; newest two survive.
    QVERIFY(c.get(QStringLiteral("/r"), QStringLiteral("1")).isEmpty());
    QVERIFY(c.get(QStringLiteral("/r"), QStringLiteral("2")).isEmpty());
}

void TestGitBaseBlobCache::replaceSamePath_updatesBytesAccount()
{
    auto &c = GitBaseBlobCache::instance();
    c.put(QStringLiteral("/r"), QStringLiteral("a"), bigBlob(4, 'a'));
    QCOMPARE(c.byteCount(), qsizetype(4096));
    c.put(QStringLiteral("/r"), QStringLiteral("a"), bigBlob(2, 'b'));
    QCOMPARE(c.byteCount(), qsizetype(2048));
    QCOMPARE(c.get(QStringLiteral("/r"), QStringLiteral("a")).at(0), 'b');
}

QTEST_GUILESS_MAIN(TestGitBaseBlobCache)
#include "test_git_base_blob_cache.moc"
