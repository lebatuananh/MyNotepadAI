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

#include <QtTest>
#include <QLineEdit>
#include <QList>
#include <QPointer>

#include "CsvFilterStrip.h"

// Unit test for the per-column filter-strip reconciliation core extracted from
// CsvPreviewWidget. The production handlers (onFilterColumnsInserted /
// onFilterColumnsRemoved / buildFilterStrip's reconcile) DELEGATE to these same
// functions, so these cases cover the shipping code path — they lock down the
// off-by-one splice/erase arithmetic that a tail-append refactor would regress.
//
// Fields carry their filter text in QLineEdit::text(), and the field at index c
// IS the filter for column c (positional map, exactly as currentFilters() reads
// it). So "text rides to the correct column" == "text lands at the expected list
// index after the splice".
class TestCsvFilterStrip : public QObject
{
    Q_OBJECT

private slots:
    void insertMiddle_textRidesToNewIndex_notTail();
    void removeMiddle_sizeMatchesAndWidgetDestroyed();
    void resetRebuild_reconcilesToNewCountWithTextDiscarded();

private:
    // A factory that mints an empty parentless QLineEdit (no widget chrome needed
    // for arithmetic tests). Caller owns lifetime via the list + QPointer probes.
    static QLineEdit *makeField() { return new QLineEdit(); }
    static QString textAt(const QList<QLineEdit *> &fields, int i)
    {
        return (i >= 0 && i < fields.size()) ? fields.at(i)->text() : QString();
    }
    static void destroyAll(QList<QLineEdit *> &fields)
    {
        qDeleteAll(fields);
        fields.clear();
    }
};

// (1) Insert a column in the MIDDLE while a right-of-insert field holds text →
//     the text must ride to its shifted index, NOT stay put (tail-append bug).
void TestCsvFilterStrip::insertMiddle_textRidesToNewIndex_notTail()
{
    // Columns [A, B, C]; "abc" typed into B (index 1).
    QList<QLineEdit *> fields{makeField(), makeField(), makeField()};
    fields.at(1)->setText(QStringLiteral("abc"));
    QPointer<QLineEdit> bField = fields.at(1);   // track B's identity across the splice

    // Insert one column AT index 1 (before B). Model would emit columnsInserted(1,1).
    CsvFilterStrip::spliceInserted(fields, 1, 1, &TestCsvFilterStrip::makeField);

    // Now 4 fields: [A="", NEW="", B="abc", C=""]. B's text rode 1 -> 2.
    QCOMPARE(fields.size(), 4);
    QVERIFY2(!bField.isNull(), "B's field must survive an insert (not destroyed)");
    QCOMPARE(fields.at(2), bField.data());            // same object, shifted right
    QCOMPARE(textAt(fields, 2), QStringLiteral("abc")); // text rode to index 2
    QCOMPARE(textAt(fields, 1), QString());            // the inserted slot is blank
    QVERIFY2(textAt(fields, 1) != QStringLiteral("abc"),
             "tail-append regression: 'abc' must NOT be left on the inserted column");

    destroyAll(fields);
}

// (2) Remove a MIDDLE column → list size drops by one, survivors stay positioned,
//     and the removed QLineEdit is actually destroyed (QPointer nulls → no leak).
void TestCsvFilterStrip::removeMiddle_sizeMatchesAndWidgetDestroyed()
{
    // Columns [A="x", B="y", C="z"]; remove B (index 1).
    QList<QLineEdit *> fields{makeField(), makeField(), makeField()};
    fields.at(0)->setText(QStringLiteral("x"));
    fields.at(1)->setText(QStringLiteral("y"));
    fields.at(2)->setText(QStringLiteral("z"));
    QPointer<QLineEdit> aField = fields.at(0);
    QPointer<QLineEdit> bField = fields.at(1);   // the one being removed
    QPointer<QLineEdit> cField = fields.at(2);

    // Model would emit columnsRemoved(1,1); production passes `delete` as destroy.
    CsvFilterStrip::spliceRemoved(fields, 1, 1, [](QLineEdit *f) { delete f; });

    // Size matches the new column count (3 -> 2); survivors keep their text, with
    // C collapsing from index 2 to index 1.
    QCOMPARE(fields.size(), 2);
    QVERIFY2(bField.isNull(), "removed QLineEdit must be destroyed (no widget leak)");
    QVERIFY2(!aField.isNull() && !cField.isNull(), "survivors must not be destroyed");
    QCOMPARE(fields.at(0), aField.data());
    QCOMPARE(fields.at(1), cField.data());
    QCOMPARE(textAt(fields, 0), QStringLiteral("x"));
    QCOMPARE(textAt(fields, 1), QStringLiteral("z")); // C rode 2 -> 1, text intact

    destroyAll(fields);
}

// (3) Reset/rebuild with a count change (e.g. undo-of-delete restoring a column at
//     its middle index → modelReset). The reset path discards filter text first,
//     then reconciles the count. Assert: fields reconcile to the new count and no
//     stale text survives (so a pending recompute can't filter the wrong column).
void TestCsvFilterStrip::resetRebuild_reconcilesToNewCountWithTextDiscarded()
{
    // Post-delete state: 2 fields [A="", C="z"] for columns [A, C].
    QList<QLineEdit *> fields{makeField(), makeField()};
    fields.at(1)->setText(QStringLiteral("z"));

    // The reset handler (rebuildFilterStripIfCountChanged) first wipes text via
    // resetSortFilterState, THEN buildFilterStrip -> reconcileCount to the restored
    // count (3: [A, B, C]). Model the text-discard:
    for (QLineEdit *f : fields)
        f->clear();
    // Reconcile up to the restored column count of 3.
    CsvFilterStrip::reconcileCount(
        fields, 3, &TestCsvFilterStrip::makeField, [](QLineEdit *f) { delete f; });

    QCOMPARE(fields.size(), 3);                 // aligned to restored columnCount()
    for (int c = 0; c < fields.size(); ++c)
        QCOMPARE(textAt(fields, c), QString()); // every field empty: no misaligned "z"

    // And reconcile DOWN (shrink) destroys from the tail without leaking.
    QPointer<QLineEdit> tail = fields.at(2);
    CsvFilterStrip::reconcileCount(
        fields, 2, &TestCsvFilterStrip::makeField, [](QLineEdit *f) { delete f; });
    QCOMPARE(fields.size(), 2);
    QVERIFY2(tail.isNull(), "shrink must destroy the surplus tail field (no leak)");

    destroyAll(fields);
}

QTEST_MAIN(TestCsvFilterStrip)
#include "test_csv_filter_strip.moc"
