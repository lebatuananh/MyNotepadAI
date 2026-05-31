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

// Pure coordinate-math + overlay re-key tests for CsvTableModel structural
// inserts. The save serialization lives in CsvPreviewWidget (a QWidget); here we
// prove the model layer the save walk reads from: virtual↔physical translation,
// the overlay re-key trap (an edited cell below an insert must keep its value),
// and dimension growth. A small on-disk file is mmap'd + indexed synchronously.

#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QUndoStack>
#include <atomic>

#include "CsvTableModel.h"
#include "CsvDocument.h"
#include "CsvEditCommands.h"

class TestCsvTableModelInsert : public QObject
{
    Q_OBJECT

private slots:
    void rowInsert_shiftsOverlayBelow();
    void rowInsert_midFile_translation();
    void colInsert_shiftsOverlayRight();
    void colInsert_growsColumnCount();
    void emptyDoc_insertColumn_noCrash();
    void saveWalk_midRowBlank_roundTrips();
    void saveWalk_appendTrailingBlank_roundTrips();
    void absoluteCell_translatesUnderInserts();
    void workerReadPath_translatesUnderInserts();
    void menuPositionMapping_aboveBelowBeforeAfter();
    void colInsert_headerLabelsTrackVisualPosition();
    void saveWalk_colInsert_roundTrips();
    void emptyDoc_insertAndSave_roundTrips();
    void emptyDoc_headerMode_appendOnceGivesOneDataRow();
    void setHeaderText_updatesLabelAndPersists();
    void clearCells_thenUndo();
    void deleteRows_thenUndo();
    void deleteColumns_thenUndo();
    void deleteRows_headerRowGuardedInHeaderMode();
    void sequentialDeletes_undoRedoStayInSync();

private:
    // Build a doc from `bytes`, index it synchronously, return an owned model.
    // headerMode defaults OFF so data row r == file row r (simpler assertions).
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

    // Mirror CsvPreviewWidget::saveToDisk's virtual walk EXACTLY, INCLUDING the
    // verbatim byte-slice fast path and its `!hasColInserts` / per-row-overlay
    // guard — so the bytes produced are what the widget would write AND so a
    // verbatim-bypass bug (e.g. failing to disable the fast path under column
    // inserts) would surface here as a missing delimiter. Non-transcoded path
    // only (ASCII test inputs). base/dataSize come from the ORIGINAL document.
    static QByteArray serializeLikeWidget(CsvDocument *doc, CsvTableModel &model)
    {
        const char delim = doc->delimiter();
        const char eol = '\n';
        const char *base = doc->data();
        const qint64 dataSize = doc->dataSize();
        const quint64 vRows = model.virtualTotalFileRows();
        const int vCols = model.virtualColumnCount();
        const bool hasColInserts = model.hasInsertedColumns();
        const QHash<quint64, QString> &overlay = model.overlay();
        QByteArray out;
        qint64 offset = doc->firstRowOffset();
        QVector<QString> fields;
        for (quint64 vr = 0; vr < vRows; ++vr) {
            const qint64 rowStart = offset;
            const qint64 physRow = model.virtualFileRowToPhysical(vr);
            const bool blankRow = (physRow < 0);

            // Same verbatim gate as the widget: physical row, no column inserts,
            // no overlay edit on this row.
            bool verbatim = !blankRow && !hasColInserts;
            if (verbatim) {
                for (int c = 0; c < vCols; ++c)
                    if (overlay.contains(CsvTableModel::overlayKey(vr, c))) { verbatim = false; break; }
            }

            if (verbatim) {
                qint64 rowEnd = doc->rowEnd(rowStart);
                qint64 contentEnd = rowEnd;
                while (contentEnd > rowStart &&
                       (base[contentEnd - 1] == '\n' || base[contentEnd - 1] == '\r'))
                    --contentEnd;
                out.append(base + rowStart, static_cast<int>(contentEnd - rowStart));
                offset = rowEnd;
            } else {
                // Re-serialize by VIRTUAL FILE ROW exactly as the widget does
                // (overlay-by-file-row, then physical field) — NOT via
                // absoluteCell, which re-applies the header shift and would read
                // the wrong file row in header mode.
                QVector<QString> physFields;
                if (!blankRow)
                    physFields = doc->parseRowUncached(static_cast<quint64>(physRow));
                QString line;
                for (int vc = 0; vc < vCols; ++vc) {
                    auto it = overlay.constFind(CsvTableModel::overlayKey(vr, vc));
                    QString value;
                    if (it != overlay.constEnd()) {
                        value = it.value();
                    } else if (!blankRow) {
                        const qint64 pc = CsvTableModel::physicalForVirtual(model.insertedColsSnapshot(), vc);
                        if (pc >= 0 && pc < physFields.size())
                            value = physFields.at(static_cast<int>(pc));
                    }
                    line += CsvDocument::serializeField(value, delim);
                    if (vc < vCols - 1) line += QLatin1Char(delim);
                }
                out += line.toUtf8();
                if (!blankRow)
                    offset = doc->parseRowSequential(rowStart, fields); // advance cursor
            }

            // Terminator policy copied verbatim from the widget.
            const bool lastRow = (vr + 1 == vRows);
            if (!lastRow) {
                out += eol;
            } else if (blankRow) {
                out += eol;
            } else {
                if (dataSize > 0 && (base[dataSize - 1] == '\n' || base[dataSize - 1] == '\r'))
                    out += eol;
            }
        }
        return out;
    }
};

// The trap: edit a cell, insert a blank row ABOVE it, the edit must follow.
void TestCsvTableModelInsert::rowInsert_shiftsOverlayBelow()
{
    QTemporaryDir dir;
    CsvDocument *doc = openDoc(dir, "a,b\nc,d\ne,f\n");
    CsvTableModel model(doc);
    model.setHeaderMode(false);

    // Edit data row 2 col 0 ("e" → "EDITED"). File row == data row (no header).
    QVERIFY(model.setData(model.index(2, 0), QStringLiteral("EDITED"), Qt::EditRole));
    QCOMPARE(model.data(model.index(2, 0)).toString(), QStringLiteral("EDITED"));

    // Insert a blank row at data row 1 (above the edited row).
    model.insertBlankDataRow(1);

    // Row count grew by one.
    QCOMPARE(model.dataRowCount(), quint64(4));
    // The inserted row is blank.
    QCOMPARE(model.data(model.index(1, 0)).toString(), QString());
    // Row 1's original content ("c,d") shifted down to row 2.
    QCOMPARE(model.data(model.index(2, 0)).toString(), QStringLiteral("c"));
    // THE TRAP: the edited cell that was at row 2 must now be at row 3 with its
    // value intact — not stranded at the old physical index.
    QCOMPARE(model.data(model.index(3, 0)).toString(), QStringLiteral("EDITED"));
    QCOMPARE(model.data(model.index(3, 1)).toString(), QStringLiteral("f"));

    delete doc;
}

void TestCsvTableModelInsert::rowInsert_midFile_translation()
{
    QTemporaryDir dir;
    CsvDocument *doc = openDoc(dir, "r0\nr1\nr2\n");
    CsvTableModel model(doc);
    model.setHeaderMode(false);

    model.insertBlankDataRow(1); // virtual: [r0, blank, r1, r2]

    QCOMPARE(model.virtualFileRowToPhysical(0), qint64(0)); // r0
    QCOMPARE(model.virtualFileRowToPhysical(1), qint64(-1)); // inserted blank
    QCOMPARE(model.virtualFileRowToPhysical(2), qint64(1)); // r1
    QCOMPARE(model.virtualFileRowToPhysical(3), qint64(2)); // r2
    QCOMPARE(model.virtualTotalFileRows(), quint64(4));

    delete doc;
}

void TestCsvTableModelInsert::colInsert_shiftsOverlayRight()
{
    QTemporaryDir dir;
    CsvDocument *doc = openDoc(dir, "a,b,c\nd,e,f\n");
    CsvTableModel model(doc);
    model.setHeaderMode(false);

    // Edit col 2 ("c" at row 0).
    QVERIFY(model.setData(model.index(0, 2), QStringLiteral("COL2"), Qt::EditRole));

    // Insert a blank column at col 1.
    model.insertBlankColumn(1);

    QCOMPARE(model.columnCount(), 4);
    QCOMPARE(model.data(model.index(0, 0)).toString(), QStringLiteral("a"));
    QCOMPARE(model.data(model.index(0, 1)).toString(), QString());       // inserted blank
    QCOMPARE(model.data(model.index(0, 2)).toString(), QStringLiteral("b"));
    // THE TRAP (column edition): edited col 2 shifted to col 3, value intact.
    QCOMPARE(model.data(model.index(0, 3)).toString(), QStringLiteral("COL2"));

    delete doc;
}

void TestCsvTableModelInsert::colInsert_growsColumnCount()
{
    QTemporaryDir dir;
    CsvDocument *doc = openDoc(dir, "a,b\nc,d\n");
    CsvTableModel model(doc);
    model.setHeaderMode(false);
    const int before = model.columnCount();
    model.insertBlankColumn(before); // append
    QCOMPARE(model.columnCount(), before + 1);
    QCOMPARE(model.virtualColToPhysical(before), -1); // appended col is blank
    delete doc;
}

void TestCsvTableModelInsert::emptyDoc_insertColumn_noCrash()
{
    QTemporaryDir dir;
    // Header-only file: one row, zero data rows in header mode.
    CsvDocument *doc = openDoc(dir, "h0,h1\n");
    CsvTableModel model(doc);
    // header mode default ON → dataRowCount 0.
    QCOMPARE(model.dataRowCount(), quint64(0));
    model.insertBlankColumn(model.virtualColumnCount()); // append with zero data rows
    QCOMPARE(model.columnCount(), 3);
    // Insert a data row when there are none — must not crash, count becomes 1.
    model.insertBlankDataRow(0);
    QCOMPARE(model.dataRowCount(), quint64(1));
    delete doc;
}

// Mirror the widget's save serialization at the model level: walk virtual rows,
// emit overlay-or-physical fields, and assert a mid-file blank lands correctly.
void TestCsvTableModelInsert::saveWalk_midRowBlank_roundTrips()
{
    QTemporaryDir dir;
    CsvDocument *doc = openDoc(dir, "a,b\nc,d\n");
    CsvTableModel model(doc);
    model.setHeaderMode(false);
    model.insertBlankDataRow(1); // virtual data rows: [a,b], [blank], [c,d]

    // Reproduce the save walk in virtual coordinates (the widget's loop).
    const char delim = doc->delimiter();
    const quint64 vRows = model.virtualTotalFileRows();
    const int vCols = model.virtualColumnCount();
    QStringList lines;
    for (quint64 vr = 0; vr < vRows; ++vr) {
        QString line;
        for (int vc = 0; vc < vCols; ++vc) {
            // absoluteCell is the model's window-independent read (overlay→phys).
            const QString value = model.absoluteCell(vr, vc);
            line += CsvDocument::serializeField(value, delim);
            if (vc < vCols - 1) line += QLatin1Char(delim);
        }
        lines << line;
    }
    QCOMPARE(lines.size(), 3);
    QCOMPARE(lines.at(0), QStringLiteral("a,b"));
    QCOMPARE(lines.at(1), QStringLiteral(","));   // blank row, 2 empty fields
    QCOMPARE(lines.at(2), QStringLiteral("c,d"));

    delete doc;
}

// The disputed case: append a blank row at the very end, serialize EXACTLY as
// the widget would (content + terminator branches), write the bytes, reopen
// with a fresh CsvDocument, and assert the REAL reloaded row count + cells.
// Whatever this asserts is the truth that the showNotice decision must match.
void TestCsvTableModelInsert::saveWalk_appendTrailingBlank_roundTrips()
{
    QTemporaryDir dir;
    CsvDocument *doc = openDoc(dir, "A\nB"); // 2 rows, B unterminated
    CsvTableModel model(doc);
    model.setHeaderMode(false);
    QCOMPARE(model.dataRowCount(), quint64(2));

    model.insertBlankDataRow(model.dataRowCount()); // append → [A, B, blank]
    QCOMPARE(model.virtualTotalFileRows(), quint64(3));

    const QByteArray bytes = serializeLikeWidget(doc, model);
    // Document the exact bytes the serializer produced.
    qInfo() << "serialized bytes:" << bytes.toHex(' ') << "literal:" << bytes;

    // Write to a new file and reopen — the real post-save reindex.
    const QString outPath = dir.filePath(QStringLiteral("out.csv"));
    {
        QFile f(outPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(bytes);
        f.close();
    }
    auto *reDoc = new CsvDocument();
    QCOMPARE(reDoc->open(outPath), CsvDocument::OpenStatus::Ok);
    std::atomic<bool> cancel{false};
    reDoc->buildIndex(cancel, nullptr);

    CsvTableModel reModel(reDoc);
    reModel.setHeaderMode(false);

    // THE TRUTH: how many rows does the reloaded file actually have?
    qInfo() << "reloaded totalRows:" << reDoc->totalRows()
            << "reModel.dataRowCount:" << reModel.dataRowCount();

    // Assert the trailing blank survived as a real 3rd row.
    QCOMPARE(reDoc->totalRows(), quint64(3));
    QCOMPARE(reModel.dataRowCount(), quint64(3));
    QCOMPARE(reModel.data(reModel.index(0, 0)).toString(), QStringLiteral("A"));
    QCOMPARE(reModel.data(reModel.index(1, 0)).toString(), QStringLiteral("B"));
    QCOMPARE(reModel.data(reModel.index(2, 0)).toString(), QString()); // blank row

    delete reDoc;
    delete doc;
}

// absoluteCell is the single translated reader that the copy + find paths use
// (the async workers, which read the document in PHYSICAL coords, are gated off
// when inserts are pending). Prove absoluteCell returns the right data through
// both a row and a column insert — this is the read the blast-radius fix relies
// on. Mirrors the trap test but via absoluteCell (worker-equivalent), not data().
void TestCsvTableModelInsert::absoluteCell_translatesUnderInserts()
{
    QTemporaryDir dir;
    CsvDocument *doc = openDoc(dir, "a,b,c\nd,e,f\ng,h,i\n");
    CsvTableModel model(doc);
    model.setHeaderMode(false);

    // Edit (row 2,col 2) "i" → "EDIT", then insert a blank row at 1 AND a blank
    // column at 1. The edited cell should move from (2,2) to (3,3).
    QVERIFY(model.setData(model.index(2, 2), QStringLiteral("EDIT"), Qt::EditRole));
    model.insertBlankDataRow(1);
    model.insertBlankColumn(1);

    // absoluteCell(dataRow, col) — virtual coordinates, worker-equivalent read.
    QCOMPARE(model.absoluteCell(0, 0), QStringLiteral("a"));
    QCOMPARE(model.absoluteCell(0, 1), QString());            // inserted blank col
    QCOMPARE(model.absoluteCell(0, 2), QStringLiteral("b"));
    QCOMPARE(model.absoluteCell(1, 0), QString());            // inserted blank row
    QCOMPARE(model.absoluteCell(2, 0), QStringLiteral("d"));  // original row 1 shifted
    QCOMPARE(model.absoluteCell(3, 3), QStringLiteral("EDIT")); // edited cell shifted +1/+1

    delete doc;
}

// The copy + find COUNT workers run off-thread and read the document in
// PHYSICAL coordinates, so they translate via CsvTableModel::physicalForVirtual
// over snapshots of the inserted-row/col lists (the SAME primitive the model
// uses — no drift, and the worker stays async, so large multi-window files are
// not regressed to a sync UI-thread scan). This reproduces that exact worker
// read path on a multi-row doc and asserts every cell, including an inserted
// blank row, an inserted blank column, and an overlay edit that shifted.
void TestCsvTableModelInsert::workerReadPath_translatesUnderInserts()
{
    QTemporaryDir dir;
    CsvDocument *doc = openDoc(dir, "a,b\nc,d\ne,f\n");
    CsvTableModel model(doc);
    model.setHeaderMode(false);

    QVERIFY(model.setData(model.index(2, 1), QStringLiteral("Z"), Qt::EditRole)); // f→Z
    model.insertBlankDataRow(1);  // [a,b],[blank],[c,d],[e,Z]
    model.insertBlankColumn(0);   // prepend a blank column

    // Snapshot exactly as the worker does.
    const QList<quint64> insRows = model.insertedFileRowsSnapshot();
    const QList<int> insCols = model.insertedColsSnapshot();
    const QHash<quint64, QString> overlay = model.overlay();
    const bool headerMode = model.headerMode();

    // Worker-equivalent read: virtual (dataRow,c) → overlay (virtual-keyed) or
    // physical document via physicalForVirtual. Byte-for-byte the copy worker's
    // cellAt lambda.
    auto workerCell = [&](quint64 dataRow, int c) -> QString {
        const quint64 vFileRow = headerMode ? dataRow + 1 : dataRow;
        auto it = overlay.constFind(CsvTableModel::overlayKey(vFileRow, c));
        if (it != overlay.constEnd()) return it.value();
        const qint64 physFileRow = CsvTableModel::physicalForVirtual(insRows, vFileRow);
        const qint64 physCol = CsvTableModel::physicalForVirtual(insCols, c);
        if (physFileRow < 0 || physCol < 0) return QString();
        const QVector<QString> fields = doc->parseRowUncached(static_cast<quint64>(physFileRow));
        return physCol < fields.size() ? fields.at(static_cast<int>(physCol)) : QString();
    };

    // Grid is now 4 data rows × 3 cols: col 0 blank, cols 1-2 = original a/b.
    QCOMPARE(model.dataRowCount(), quint64(4));
    QCOMPARE(model.columnCount(), 3);

    // Worker read must agree with the model's own data() at every cell.
    for (quint64 r = 0; r < 4; ++r)
        for (int c = 0; c < 3; ++c)
            QCOMPARE(workerCell(r, c), model.data(model.index(static_cast<int>(r), c)).toString());

    // Spot-check the meaningful cells.
    QCOMPARE(workerCell(0, 0), QString());            // inserted blank col
    QCOMPARE(workerCell(0, 1), QStringLiteral("a"));
    QCOMPARE(workerCell(0, 2), QStringLiteral("b"));
    QCOMPARE(workerCell(1, 1), QString());            // inserted blank row
    QCOMPARE(workerCell(2, 1), QStringLiteral("c"));  // original row 1 shifted down
    QCOMPARE(workerCell(3, 2), QStringLiteral("Z"));  // edited cell, shifted by both inserts

    delete doc;
}

// Menu → position mapping. The header handlers compute the target row/col from
// the clicked section exactly as:
//   above  → toAbsRow(section)        below  → toAbsRow(section)+1
//   before → section                  after  → section+1
// This guards the off-by-one that would make "below" insert above. Window base
// is 0 here (small doc; clampBase pins it), so toAbsRow(section)==section — the
// +m_windowBase add-back itself is covered by rowInsert_midFile_translation and
// toAbsRow's definition. We assert by checking WHERE the blank landed.
void TestCsvTableModelInsert::menuPositionMapping_aboveBelowBeforeAfter()
{
    auto freshRowDoc = [this](QTemporaryDir &d) {
        CsvDocument *doc = openDoc(d, "r0\nr1\nr2\n");
        return doc;
    };

    // "Insert Row Above" clicked on section 1 → insertRowAt(toAbsRow(1)=1).
    {
        QTemporaryDir d; CsvDocument *doc = freshRowDoc(d);
        CsvTableModel m(doc); m.setHeaderMode(false);
        const int section = 1;
        m.insertBlankDataRow(m.toAbsRow(section)); // "above"
        QCOMPARE(m.data(m.index(1, 0)).toString(), QString());        // blank at 1
        QCOMPARE(m.data(m.index(2, 0)).toString(), QStringLiteral("r1")); // r1 pushed down
        delete doc;
    }
    // "Insert Row Below" clicked on section 1 → insertRowAt(toAbsRow(1)+1=2).
    {
        QTemporaryDir d; CsvDocument *doc = freshRowDoc(d);
        CsvTableModel m(doc); m.setHeaderMode(false);
        const int section = 1;
        m.insertBlankDataRow(m.toAbsRow(section) + 1); // "below"
        QCOMPARE(m.data(m.index(1, 0)).toString(), QStringLiteral("r1")); // r1 stays
        QCOMPARE(m.data(m.index(2, 0)).toString(), QString());        // blank below it
        delete doc;
    }
    // "Insert Column Before" on section 1 → insertColumnAt(1).
    {
        QTemporaryDir d; CsvDocument *doc = openDoc(d, "a,b,c\n");
        CsvTableModel m(doc); m.setHeaderMode(false);
        m.insertBlankColumn(1); // "before" col 1
        QCOMPARE(m.data(m.index(0, 1)).toString(), QString());        // blank at 1
        QCOMPARE(m.data(m.index(0, 2)).toString(), QStringLiteral("b")); // b pushed right
        delete doc;
    }
    // "Insert Column After" on section 1 → insertColumnAt(2).
    {
        QTemporaryDir d; CsvDocument *doc = openDoc(d, "a,b,c\n");
        CsvTableModel m(doc); m.setHeaderMode(false);
        m.insertBlankColumn(1 + 1); // "after" col 1
        QCOMPARE(m.data(m.index(0, 1)).toString(), QStringLiteral("b")); // b stays
        QCOMPARE(m.data(m.index(0, 2)).toString(), QString());        // blank after it
        delete doc;
    }
}

// Column header labels must track VISUAL position after an insert, not the
// underlying physical column — otherwise the header is wrong even when the data
// is right. Two regimes:
//  - header mode: real first-row headers follow their data through the shift;
//    the inserted blank gets a spreadsheet-letter placeholder.
//  - non-header (A/B/C) mode: every label is purely the view-column letter.
void TestCsvTableModelInsert::colInsert_headerLabelsTrackVisualPosition()
{
    // Header mode: first row is the header. Insert a blank column at index 1.
    {
        QTemporaryDir dir;
        CsvDocument *doc = openDoc(dir, "Name,Age,City\nAlice,30,NYC\n");
        CsvTableModel model(doc);
        // header mode default ON.
        QVERIFY(model.headerMode());
        model.insertBlankColumn(1);

        QCOMPARE(model.columnCount(), 4);
        // Real headers track their data across the shift; blank gets a letter.
        QCOMPARE(model.headerData(0, Qt::Horizontal, Qt::DisplayRole).toString(), QStringLiteral("Name"));
        QCOMPARE(model.headerData(1, Qt::Horizontal, Qt::DisplayRole).toString(), QStringLiteral("B")); // inserted blank
        QCOMPARE(model.headerData(2, Qt::Horizontal, Qt::DisplayRole).toString(), QStringLiteral("Age"));  // shifted right
        QCOMPARE(model.headerData(3, Qt::Horizontal, Qt::DisplayRole).toString(), QStringLiteral("City")); // shifted right
        delete doc;
    }
    // Non-header mode: labels are purely the visual A/B/C/D position.
    {
        QTemporaryDir dir;
        CsvDocument *doc = openDoc(dir, "a,b,c\nd,e,f\n");
        CsvTableModel model(doc);
        model.setHeaderMode(false);
        model.insertBlankColumn(1);

        QCOMPARE(model.columnCount(), 4);
        QCOMPARE(model.headerData(0, Qt::Horizontal, Qt::DisplayRole).toString(), QStringLiteral("A"));
        QCOMPARE(model.headerData(1, Qt::Horizontal, Qt::DisplayRole).toString(), QStringLiteral("B")); // inserted blank
        QCOMPARE(model.headerData(2, Qt::Horizontal, Qt::DisplayRole).toString(), QStringLiteral("C"));
        QCOMPARE(model.headerData(3, Qt::Horizontal, Qt::DisplayRole).toString(), QStringLiteral("D"));
        delete doc;
    }
}

// The hardest serialization path: a column insert forces EVERY row to gain a
// delimiter at the right position, and the verbatim byte-slice fast path MUST be
// disabled (hasColInserts) or the original row bytes would be written with no
// new delimiter and the column would silently vanish. Build a 2-col doc, insert
// a blank column in the middle, serialize via the widget-faithful helper, write,
// reopen, and assert the raw bytes, reloaded maxFieldCount, and cells.
void TestCsvTableModelInsert::saveWalk_colInsert_roundTrips()
{
    QTemporaryDir dir;
    CsvDocument *doc = openDoc(dir, "a,b\nc,d"); // 2 cols, last row unterminated
    CsvTableModel model(doc);
    model.setHeaderMode(false);
    QCOMPARE(model.columnCount(), 2);

    model.insertBlankColumn(1); // [a,"",b] / [c,"",d]
    QVERIFY(model.hasInsertedColumns());
    QCOMPARE(model.columnCount(), 3);

    const QByteArray bytes = serializeLikeWidget(doc, model);
    qInfo() << "col-insert serialized bytes:" << bytes.toHex(' ') << "literal:" << bytes;

    // (3) Raw bytes: blank field between the two originals, no spurious trailing
    // delimiter, original file had no trailing EOL so none is added.
    QCOMPARE(bytes, QByteArray("a,,b\nc,,d"));

    // Reopen and reindex — the real post-save round-trip.
    const QString outPath = dir.filePath(QStringLiteral("out.csv"));
    {
        QFile f(outPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(bytes);
        f.close();
    }
    auto *reDoc = new CsvDocument();
    QCOMPARE(reDoc->open(outPath), CsvDocument::OpenStatus::Ok);
    std::atomic<bool> cancel{false};
    reDoc->buildIndex(cancel, nullptr);
    CsvTableModel reModel(reDoc);
    reModel.setHeaderMode(false);

    // (1) maxFieldCount grew to 3.
    QCOMPARE(reDoc->maxFieldCount(), 3);
    QCOMPARE(reModel.columnCount(), 3);
    QCOMPARE(reModel.dataRowCount(), quint64(2));

    // (2) Reloaded cells: blank in the middle, originals shifted right.
    QCOMPARE(reModel.data(reModel.index(0, 0)).toString(), QStringLiteral("a"));
    QCOMPARE(reModel.data(reModel.index(0, 1)).toString(), QString());
    QCOMPARE(reModel.data(reModel.index(0, 2)).toString(), QStringLiteral("b"));
    QCOMPARE(reModel.data(reModel.index(1, 0)).toString(), QStringLiteral("c"));
    QCOMPARE(reModel.data(reModel.index(1, 1)).toString(), QString());
    QCOMPARE(reModel.data(reModel.index(1, 2)).toString(), QStringLiteral("d"));

    delete reDoc;
    delete doc;
}

// Regression: inserting into an EMPTY (zero-byte) CSV. open() returns
// OpenStatus::Empty and the document never maps a file (m_data == nullptr), so
// this is the crash-risk path. The widget now builds a 0-row model for empty
// files; here we drive the model directly: build it on an empty doc, append a
// row and a column, edit a cell, serialize + reopen, and assert the round-trip.
void TestCsvTableModelInsert::emptyDoc_insertAndSave_roundTrips()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("empty.csv"));
    { QFile f(path); QVERIFY(f.open(QIODevice::WriteOnly)); f.close(); } // zero bytes

    auto *doc = new CsvDocument();
    QCOMPARE(doc->open(path), CsvDocument::OpenStatus::Empty);
    std::atomic<bool> cancel{false};
    QVERIFY(doc->buildIndex(cancel, nullptr)); // no-op, must not crash
    QCOMPARE(doc->totalRows(), quint64(0));

    CsvTableModel model(doc);
    model.setHeaderMode(false);
    QCOMPARE(model.dataRowCount(), quint64(0));
    QCOMPARE(model.columnCount(), 1); // empty doc presents as 1 col, 0 rows

    // Append a row, then a column → a 1×2 editable grid, then type a cell.
    model.insertBlankDataRow(model.dataRowCount());
    QCOMPARE(model.dataRowCount(), quint64(1));
    model.insertBlankColumn(model.virtualColumnCount());
    QCOMPARE(model.columnCount(), 2);
    QVERIFY(model.setData(model.index(0, 0), QStringLiteral("hi"), Qt::EditRole));

    // Serialize via the widget-faithful path (m_data is nullptr here — must not
    // deref) and round-trip. The appended row is an INSERTED BLANK row, so the
    // terminator rule emits a trailing EOL ("hi,\n"); it reloads as one 2-field
    // row regardless (the post-newline emptiness is not a counted row).
    const QByteArray bytes = serializeLikeWidget(doc, model);
    qInfo() << "empty-doc serialized bytes:" << bytes.toHex(' ') << "literal:" << bytes;
    QCOMPARE(bytes, QByteArray("hi,\n"));

    const QString outPath = dir.filePath(QStringLiteral("out.csv"));
    { QFile f(outPath); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(bytes); f.close(); }
    auto *reDoc = new CsvDocument();
    QCOMPARE(reDoc->open(outPath), CsvDocument::OpenStatus::Ok);
    QVERIFY(reDoc->buildIndex(cancel, nullptr));
    CsvTableModel reModel(reDoc);
    reModel.setHeaderMode(false);
    QCOMPARE(reModel.dataRowCount(), quint64(1));
    QCOMPARE(reModel.columnCount(), 2);
    QCOMPARE(reModel.data(reModel.index(0, 0)).toString(), QStringLiteral("hi"));
    QCOMPARE(reModel.data(reModel.index(0, 1)).toString(), QString());

    delete reDoc;
    delete doc;
}

// Regression: in HEADER mode, one "Append Row" on an empty file must yield one
// DATA row — not zero (the "must click twice" bug, where the single inserted
// row was consumed as the header). insertBlankDataRow seeds a blank header row 0
// when the document has no file rows yet.
void TestCsvTableModelInsert::emptyDoc_headerMode_appendOnceGivesOneDataRow()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("empty.csv"));
    { QFile f(path); QVERIFY(f.open(QIODevice::WriteOnly)); f.close(); }

    auto *doc = new CsvDocument();
    QCOMPARE(doc->open(path), CsvDocument::OpenStatus::Empty);
    std::atomic<bool> cancel{false};
    QVERIFY(doc->buildIndex(cancel, nullptr));

    CsvTableModel model(doc);
    // header mode default ON.
    QVERIFY(model.headerMode());
    QCOMPARE(model.dataRowCount(), quint64(0));

    model.insertBlankDataRow(model.dataRowCount()); // ONE append
    QCOMPARE(model.dataRowCount(), quint64(1));      // ...gives ONE data row
    QCOMPARE(model.virtualTotalFileRows(), quint64(2)); // header row 0 + data row 1

    delete doc;
}

// Regression: the column header text must be editable in header mode, the label
// must update, and the edit must survive a save round-trip (header is file row 0).
void TestCsvTableModelInsert::setHeaderText_updatesLabelAndPersists()
{
    QTemporaryDir dir;
    CsvDocument *doc = openDoc(dir, "a,b\nc,d\n");
    CsvTableModel model(doc); // header mode ON by default

    QVERIFY(model.headerMode());
    // Initially the header labels come from row 0 ("a","b").
    QCOMPARE(model.columnLabel(0), QStringLiteral("a"));
    // Rename column 0's header.
    QVERIFY(model.setHeaderText(0, QStringLiteral("Name")));
    QCOMPARE(model.columnLabel(0), QStringLiteral("Name"));
    QVERIFY(model.hasOverlay());

    // Save round-trip: header row is rewritten with the new value.
    const QByteArray bytes = serializeLikeWidget(doc, model);
    QCOMPARE(bytes, QByteArray("Name,b\nc,d\n"));

    // setHeaderText is a no-op when header mode is off.
    model.setHeaderMode(false);
    QVERIFY(!model.setHeaderText(0, QStringLiteral("X")));

    delete doc;
}

// --- Deletion / clearing + undo (the chore's required regression coverage) ---

// Clear a 2x2 cell range, assert emptied, undo, assert original values restored.
void TestCsvTableModelInsert::clearCells_thenUndo()
{
    QTemporaryDir dir;
    CsvDocument *doc = openDoc(dir, "a,b,c\nd,e,f\ng,h,i\n");
    CsvTableModel model(doc);
    model.setHeaderMode(false);
    QUndoStack stack;

    // Clear data rows 0..1, cols 0..1 → a,b,d,e become empty; c,f,g,h,i intact.
    stack.push(new ClearCellsCommand(&model, 0, 1, 0, 1));
    QCOMPARE(model.data(model.index(0, 0)).toString(), QString());
    QCOMPARE(model.data(model.index(1, 1)).toString(), QString());
    QCOMPARE(model.data(model.index(0, 2)).toString(), QStringLiteral("c")); // outside range
    QCOMPARE(model.data(model.index(2, 0)).toString(), QStringLiteral("g"));

    stack.undo();
    QCOMPARE(model.data(model.index(0, 0)).toString(), QStringLiteral("a"));
    QCOMPARE(model.data(model.index(0, 1)).toString(), QStringLiteral("b"));
    QCOMPARE(model.data(model.index(1, 0)).toString(), QStringLiteral("d"));
    QCOMPARE(model.data(model.index(1, 1)).toString(), QStringLiteral("e"));

    // Redo re-applies the clear.
    stack.redo();
    QCOMPARE(model.data(model.index(0, 0)).toString(), QString());
    QCOMPARE(model.data(model.index(1, 1)).toString(), QString());
    QCOMPARE(model.data(model.index(0, 2)).toString(), QStringLiteral("c")); // still intact
    delete doc;
}

// Delete a middle row, assert rows collapsed, undo, assert row reappears in place.
void TestCsvTableModelInsert::deleteRows_thenUndo()
{
    QTemporaryDir dir;
    CsvDocument *doc = openDoc(dir, "r0\nr1\nr2\nr3\n");
    CsvTableModel model(doc);
    model.setHeaderMode(false);
    QCOMPARE(model.dataRowCount(), quint64(4));
    QUndoStack stack;

    stack.push(new DeleteRowsCommand(&model, 1, 2)); // remove r1, r2
    QCOMPARE(model.dataRowCount(), quint64(2));
    QCOMPARE(model.data(model.index(0, 0)).toString(), QStringLiteral("r0"));
    QCOMPARE(model.data(model.index(1, 0)).toString(), QStringLiteral("r3"));

    stack.undo();
    QCOMPARE(model.dataRowCount(), quint64(4));
    QCOMPARE(model.data(model.index(0, 0)).toString(), QStringLiteral("r0"));
    QCOMPARE(model.data(model.index(1, 0)).toString(), QStringLiteral("r1"));
    QCOMPARE(model.data(model.index(2, 0)).toString(), QStringLiteral("r2"));
    QCOMPARE(model.data(model.index(3, 0)).toString(), QStringLiteral("r3"));

    stack.redo();
    QCOMPARE(model.dataRowCount(), quint64(2));
    QCOMPARE(model.data(model.index(1, 0)).toString(), QStringLiteral("r3"));
    delete doc;
}

// Delete a middle column, assert columns collapsed, undo, assert column restored.
void TestCsvTableModelInsert::deleteColumns_thenUndo()
{
    QTemporaryDir dir;
    CsvDocument *doc = openDoc(dir, "a,b,c\nd,e,f\n");
    CsvTableModel model(doc);
    model.setHeaderMode(false);
    QCOMPARE(model.columnCount(), 3);
    QUndoStack stack;

    stack.push(new DeleteColumnsCommand(&model, 1, 1)); // remove middle col
    QCOMPARE(model.columnCount(), 2);
    QCOMPARE(model.data(model.index(0, 0)).toString(), QStringLiteral("a"));
    QCOMPARE(model.data(model.index(0, 1)).toString(), QStringLiteral("c"));
    QCOMPARE(model.data(model.index(1, 1)).toString(), QStringLiteral("f"));

    stack.undo();
    QCOMPARE(model.columnCount(), 3);
    QCOMPARE(model.data(model.index(0, 1)).toString(), QStringLiteral("b"));
    QCOMPARE(model.data(model.index(1, 1)).toString(), QStringLiteral("e"));
    QCOMPARE(model.data(model.index(0, 2)).toString(), QStringLiteral("c"));

    // Redo re-deletes the middle column.
    stack.redo();
    QCOMPARE(model.columnCount(), 2);
    QCOMPARE(model.data(model.index(0, 1)).toString(), QStringLiteral("c"));
    QCOMPARE(model.data(model.index(1, 1)).toString(), QStringLiteral("f"));
    delete doc;
}

// Header-row guard: in header mode the header (file row 0) is never a data row,
// so deleting data rows never removes it — the header label is preserved.
void TestCsvTableModelInsert::deleteRows_headerRowGuardedInHeaderMode()
{
    QTemporaryDir dir;
    CsvDocument *doc = openDoc(dir, "H0,H1\na,b\nc,d\n");
    CsvTableModel model(doc); // header mode ON by default
    QVERIFY(model.headerMode());
    QCOMPARE(model.dataRowCount(), quint64(2));       // H0,H1 is the header
    QCOMPARE(model.columnLabel(0), QStringLiteral("H0"));
    QUndoStack stack;

    // Delete ALL data rows (0..1). The header must survive.
    stack.push(new DeleteRowsCommand(&model, 0, 1));
    QCOMPARE(model.dataRowCount(), quint64(0));
    QCOMPARE(model.columnLabel(0), QStringLiteral("H0")); // header intact
    QCOMPARE(model.columnLabel(1), QStringLiteral("H1"));

    stack.undo();
    QCOMPARE(model.dataRowCount(), quint64(2));
    QCOMPARE(model.data(model.index(0, 0)).toString(), QStringLiteral("a"));
    QCOMPARE(model.columnLabel(0), QStringLiteral("H0"));

    // Redo re-deletes all data rows; header still survives.
    stack.redo();
    QCOMPARE(model.dataRowCount(), quint64(0));
    QCOMPARE(model.columnLabel(0), QStringLiteral("H0"));
    QCOMPARE(model.columnLabel(1), QStringLiteral("H1"));
    delete doc;
}

// Sequential deletes must stay in sync through undo/redo. Command B's snapshot is
// taken AFTER A (so it already contains A's deleted-set); undoing B must reverse
// only B and leave A applied; undoing A returns to the original; redoing both
// re-applies. This catches a snapshot that omits the deleted-sets.
void TestCsvTableModelInsert::sequentialDeletes_undoRedoStayInSync()
{
    QTemporaryDir dir;
    CsvDocument *doc = openDoc(dir, "r0\nr1\nr2\nr3\nr4\nr5\n");
    CsvTableModel model(doc);
    model.setHeaderMode(false);
    QCOMPARE(model.dataRowCount(), quint64(6));
    QUndoStack stack;

    auto rowAt = [&](int r) { return model.data(model.index(r, 0)).toString(); };

    // Delete A: virtual rows [1,2] (r1,r2) → r0,r3,r4,r5.
    stack.push(new DeleteRowsCommand(&model, 1, 2));
    QCOMPARE(model.dataRowCount(), quint64(4));
    QCOMPARE(rowAt(0), QStringLiteral("r0"));
    QCOMPARE(rowAt(1), QStringLiteral("r3"));
    QCOMPARE(rowAt(2), QStringLiteral("r4"));
    QCOMPARE(rowAt(3), QStringLiteral("r5"));

    // Delete B: virtual rows [2,3] (now r4,r5) → r0,r3.
    stack.push(new DeleteRowsCommand(&model, 2, 3));
    QCOMPARE(model.dataRowCount(), quint64(2));
    QCOMPARE(rowAt(0), QStringLiteral("r0"));
    QCOMPARE(rowAt(1), QStringLiteral("r3"));

    // Undo B only: r4,r5 come back, but A stays applied (r1,r2 still gone).
    stack.undo();
    QCOMPARE(model.dataRowCount(), quint64(4));
    QCOMPARE(rowAt(0), QStringLiteral("r0"));
    QCOMPARE(rowAt(1), QStringLiteral("r3"));
    QCOMPARE(rowAt(2), QStringLiteral("r4"));
    QCOMPARE(rowAt(3), QStringLiteral("r5"));

    // Undo A: original 6 rows restored in order.
    stack.undo();
    QCOMPARE(model.dataRowCount(), quint64(6));
    for (int i = 0; i < 6; ++i)
        QCOMPARE(rowAt(i), QStringLiteral("r%1").arg(i));

    // Redo A then B: back to r0,r3.
    stack.redo();
    QCOMPARE(model.dataRowCount(), quint64(4));
    QCOMPARE(rowAt(1), QStringLiteral("r3"));
    stack.redo();
    QCOMPARE(model.dataRowCount(), quint64(2));
    QCOMPARE(rowAt(0), QStringLiteral("r0"));
    QCOMPARE(rowAt(1), QStringLiteral("r3"));

    delete doc;
}

QTEST_MAIN(TestCsvTableModelInsert)
#include "test_csv_table_model_insert.moc"
