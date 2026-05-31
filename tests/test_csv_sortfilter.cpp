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

// Pure compute-core tests for the CSV sort/filter view layer. CsvSortFilter is
// Qt-widget-free and reads cells through a CellReader callback, so the sort,
// filter, and numeric-detection logic are exercised against an in-memory 2D
// grid with no model/widget. The final case proves the model's
// viewRowToDataRow() is the IDENTITY when no view is active (the central safety
// property) over a small mmap'd temp file.

#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QStringList>
#include <QVector>
#include <atomic>

#include "CsvSortFilter.h"
#include "CsvTableModel.h"
#include "CsvDocument.h"

class TestCsvSortFilter : public QObject
{
    Q_OBJECT

private slots:
    void numericDetection_allNumeric();
    void numericDetection_leadingZeroIsText();
    void numericDetection_mixedIsText();
    void numericDetection_emptyAndRaggedIgnored();
    void sort_numericAscDesc();
    void sort_lexicographicCaseInsensitive();
    void sort_stablePreservesOriginalOrder();
    void sort_emptyAndRaggedLast();
    void noSortNoFilter_isIdentityOrder();
    void filter_singleColumnSubstring();
    void filter_multiColumnAnded();
    void filter_combinedWithSort();
    void viewRowToDataRow_identityWhenInactive();

private:
    // A CellReader over a row-major grid; ragged/out-of-range cells return empty
    // (the same contract the model's worker reader gives the compute core).
    static CsvSortFilter::CellReader gridReader(const QVector<QStringList> &grid)
    {
        return [&grid](quint64 dataRow, int col) -> QString {
            if (dataRow >= static_cast<quint64>(grid.size())) return QString();
            const QStringList &row = grid.at(static_cast<qsizetype>(dataRow));
            return (col >= 0 && col < row.size()) ? row.at(col) : QString();
        };
    }

    static CsvDocument *openDoc(QTemporaryDir &dir, const QByteArray &bytes,
                                const QString &name = QStringLiteral("t.csv"))
    {
        const QString path = dir.filePath(name);
        QFile f(path);
        f.open(QIODevice::WriteOnly);
        f.write(bytes);
        f.close();
        auto *doc = new CsvDocument();
        const auto st = doc->open(path);
        Q_ASSERT(st == CsvDocument::OpenStatus::Ok);
        std::atomic<bool> cancel{false};
        doc->buildIndex(cancel, nullptr);
        return doc;
    }
};

// A column whose sampled non-empty cells all parse as numbers (no leading zero)
// is numeric.
void TestCsvSortFilter::numericDetection_allNumeric()
{
    const QVector<QStringList> grid = {{"9"}, {"10"}, {"-3"}, {"2.5"}, {"0"}};
    QVERIFY(CsvSortFilter::isNumericColumn(gridReader(grid), grid.size(), 0));
}

// A leading-zero value (^-?0[0-9]) forces the column to TEXT even though it
// would otherwise parse as a number (ZIP / phone). "0" and "0.5" stay numeric.
void TestCsvSortFilter::numericDetection_leadingZeroIsText()
{
    const QVector<QStringList> zip = {{"01234"}, {"00501"}, {"12345"}};
    QVERIFY(!CsvSortFilter::isNumericColumn(gridReader(zip), zip.size(), 0));

    // Plain zero and sub-one decimals are NOT leading-zero exceptions.
    const QVector<QStringList> nums = {{"0"}, {"0.5"}, {"3"}};
    QVERIFY(CsvSortFilter::isNumericColumn(gridReader(nums), nums.size(), 0));
}

// Any non-number among the sampled cells makes the column text.
void TestCsvSortFilter::numericDetection_mixedIsText()
{
    const QVector<QStringList> grid = {{"1"}, {"two"}, {"3"}};
    QVERIFY(!CsvSortFilter::isNumericColumn(gridReader(grid), grid.size(), 0));
}

// Empty / ragged cells don't decide the type; an all-empty column is text.
void TestCsvSortFilter::numericDetection_emptyAndRaggedIgnored()
{
    // col 1 is ragged/empty on some rows but every PRESENT value is numeric.
    const QVector<QStringList> grid = {{"a", "1"}, {"b"}, {"c", ""}, {"d", "2"}};
    QVERIFY(CsvSortFilter::isNumericColumn(gridReader(grid), grid.size(), 1));

    const QVector<QStringList> allEmpty = {{"a", ""}, {"b", ""}};
    QVERIFY(!CsvSortFilter::isNumericColumn(gridReader(allEmpty), allEmpty.size(), 1));
}

// Numeric column sorts by VALUE (9 before 10), both directions.
void TestCsvSortFilter::sort_numericAscDesc()
{
    const QVector<QStringList> grid = {{"10"}, {"9"}, {"100"}, {"2"}};
    std::atomic<bool> cancel{false};
    const QStringList noFilters;

    const QVector<quint32> asc = CsvSortFilter::computeViewOrder(
        gridReader(grid), grid.size(), 1, noFilters, 0, Qt::AscendingOrder, cancel);
    QCOMPARE(asc, (QVector<quint32>{3, 1, 0, 2}));   // 2, 9, 10, 100

    const QVector<quint32> desc = CsvSortFilter::computeViewOrder(
        gridReader(grid), grid.size(), 1, noFilters, 0, Qt::DescendingOrder, cancel);
    QCOMPARE(desc, (QVector<quint32>{2, 0, 1, 3}));  // 100, 10, 9, 2
}

// Text column sorts lexicographically, case-insensitively.
void TestCsvSortFilter::sort_lexicographicCaseInsensitive()
{
    const QVector<QStringList> grid = {{"banana"}, {"Apple"}, {"cherry"}};
    std::atomic<bool> cancel{false};
    const QVector<quint32> asc = CsvSortFilter::computeViewOrder(
        gridReader(grid), grid.size(), 1, QStringList(), 0, Qt::AscendingOrder, cancel);
    QCOMPARE(asc, (QVector<quint32>{1, 0, 2}));      // Apple, banana, cherry
}

// Equal keys keep their original relative order (stable).
void TestCsvSortFilter::sort_stablePreservesOriginalOrder()
{
    // col 0 is the key (all "x"); col 1 is an id to observe the tie order.
    const QVector<QStringList> grid = {{"x", "0"}, {"x", "1"}, {"x", "2"}, {"x", "3"}};
    std::atomic<bool> cancel{false};
    const QVector<quint32> asc = CsvSortFilter::computeViewOrder(
        gridReader(grid), grid.size(), 2, QStringList(), 0, Qt::AscendingOrder, cancel);
    QCOMPARE(asc, (QVector<quint32>{0, 1, 2, 3}));   // unchanged: all keys equal

    const QVector<quint32> desc = CsvSortFilter::computeViewOrder(
        gridReader(grid), grid.size(), 2, QStringList(), 0, Qt::DescendingOrder, cancel);
    QCOMPARE(desc, (QVector<quint32>{0, 1, 2, 3}));  // still original order
}

// Empty / missing (ragged) cells sort LAST in both directions, stably.
void TestCsvSortFilter::sort_emptyAndRaggedLast()
{
    // Rows 1 and 3 have an empty / missing key column; rows 0,2,4 have values.
    const QVector<QStringList> grid = {
        {"b"}, {""}, {"a"}, {/* ragged: no col 0 value */}, {"c"}};
    std::atomic<bool> cancel{false};

    const QVector<quint32> asc = CsvSortFilter::computeViewOrder(
        gridReader(grid), grid.size(), 1, QStringList(), 0, Qt::AscendingOrder, cancel);
    // a, b, c first (sorted), then the two empties LAST in original order (1, 3).
    QCOMPARE(asc, (QVector<quint32>{2, 0, 4, 1, 3}));

    const QVector<quint32> desc = CsvSortFilter::computeViewOrder(
        gridReader(grid), grid.size(), 1, QStringList(), 0, Qt::DescendingOrder, cancel);
    // c, b, a (reverse), then empties STILL last in original order (1, 3).
    QCOMPARE(desc, (QVector<quint32>{4, 0, 2, 1, 3}));
}

// No sort column and no filters ⇒ the identity order [0,1,2,...]. This is the
// compute-level analog of "clear restores the original order".
void TestCsvSortFilter::noSortNoFilter_isIdentityOrder()
{
    const QVector<QStringList> grid = {{"z"}, {"y"}, {"x"}};
    std::atomic<bool> cancel{false};
    const QVector<quint32> order = CsvSortFilter::computeViewOrder(
        gridReader(grid), grid.size(), 1, QStringList(), -1, Qt::AscendingOrder, cancel);
    QCOMPARE(order, (QVector<quint32>{0, 1, 2}));
}

// A single-column substring filter retains only matching rows (case-insensitive),
// in original order (no sort).
void TestCsvSortFilter::filter_singleColumnSubstring()
{
    const QVector<QStringList> grid = {
        {"Alice", "NYC"}, {"Bob", "LA"}, {"alicia", "NYC"}, {"Carol", "SF"}};
    std::atomic<bool> cancel{false};
    QStringList filters; filters << "ali" << "";   // col 0 contains "ali"
    const QVector<quint32> order = CsvSortFilter::computeViewOrder(
        gridReader(grid), grid.size(), 2, filters, -1, Qt::AscendingOrder, cancel);
    QCOMPARE(order, (QVector<quint32>{0, 2}));      // Alice, alicia (case-insensitive)
}

// Multiple active filters are ANDed: a row must match every non-empty filter.
void TestCsvSortFilter::filter_multiColumnAnded()
{
    const QVector<QStringList> grid = {
        {"Alice", "NYC"}, {"Bob", "NYC"}, {"alicia", "LA"}, {"Alan", "NYC"}};
    std::atomic<bool> cancel{false};
    QStringList filters; filters << "al" << "nyc"; // col 0 ~ "al" AND col 1 ~ "nyc"
    const QVector<quint32> order = CsvSortFilter::computeViewOrder(
        gridReader(grid), grid.size(), 2, filters, -1, Qt::AscendingOrder, cancel);
    QCOMPARE(order, (QVector<quint32>{0, 3}));      // Alice/NYC and Alan/NYC; not alicia/LA
}

// One combined pass: filter THEN sort the retained set.
void TestCsvSortFilter::filter_combinedWithSort()
{
    const QVector<QStringList> grid = {
        {"Alice", "30"}, {"Bob", "20"}, {"alicia", "40"}, {"Carol", "25"}};
    std::atomic<bool> cancel{false};
    QStringList filters; filters << "ali" << "";   // keep Alice(30), alicia(40)
    // sort the retained rows by col 1 (numeric) descending → alicia(40), Alice(30).
    const QVector<quint32> order = CsvSortFilter::computeViewOrder(
        gridReader(grid), grid.size(), 2, filters, 1, Qt::DescendingOrder, cancel);
    QCOMPARE(order, (QVector<quint32>{2, 0}));
}

// THE central safety property: with no view active, viewRowToDataRow(r) == r and
// rowCount() == dataRowCount() — the no-view path is byte-for-byte unchanged.
void TestCsvSortFilter::viewRowToDataRow_identityWhenInactive()
{
    QTemporaryDir dir;
    CsvDocument *doc = openDoc(dir, "a,b\nc,d\ne,f\ng,h\n");
    CsvTableModel model(doc);
    model.setHeaderMode(false);

    QVERIFY(!model.viewActive());
    QCOMPARE(static_cast<quint64>(model.rowCount()), model.dataRowCount());
    for (int r = 0; r < model.rowCount(); ++r)
        QCOMPARE(model.viewRowToDataRow(r), static_cast<quint64>(r));

    // Apply a view, then clear it: identity must be restored exactly.
    model.setViewOrder(QVector<quint32>{2, 0, 1}, 0, Qt::AscendingOrder);
    QVERIFY(model.viewActive());
    QCOMPARE(model.rowCount(), 3);
    QCOMPARE(model.viewRowToDataRow(0), quint64(2));
    QCOMPARE(model.viewRowToDataRow(1), quint64(0));

    model.clearViewOrder();
    QVERIFY(!model.viewActive());
    QCOMPARE(static_cast<quint64>(model.rowCount()), model.dataRowCount());
    for (int r = 0; r < model.rowCount(); ++r)
        QCOMPARE(model.viewRowToDataRow(r), static_cast<quint64>(r));

    delete doc;
}

QTEST_MAIN(TestCsvSortFilter)
#include "test_csv_sortfilter.moc"
