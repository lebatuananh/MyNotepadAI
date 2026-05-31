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

#ifndef CSVTABLEMODEL_H
#define CSVTABLEMODEL_H

#include <QAbstractTableModel>
#include <QHash>
#include <QList>
#include <QString>
#include <QVector>
#include <QFont>

#include <algorithm>

class CsvDocument;

// Windowed table model over a CsvDocument. It exposes AT MOST `windowSize`
// (256K) rows starting at `windowBase` to defeat QHeaderView's O(N) per-row
// SectionItem allocation on the Qt 6.5 floor. Two coordinate spaces:
//   - absolute (absRow ∈ [0, totalRows)): the source of truth for selection,
//     search, overlay and save; read straight from CsvDocument + overlay,
//     independent of the window.
//   - proxy (proxyRow ∈ [0, windowSize)): display only; absRow = windowBase + proxyRow.
//
// Re-base = beginResetModel() → set windowBase → endResetModel() (one repaint).
// The edit overlay is keyed by ABSOLUTE coordinates so edits survive re-base.
class CsvTableModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    static constexpr quint64 kWindowSize = 256 * 1024;

    explicit CsvTableModel(CsvDocument *doc, QObject *parent = nullptr);

    // --- QAbstractTableModel ---
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole) override;

    // --- Structural edits (blank row/column insertion) ---
    // The CsvDocument is immutable by design (CsvDocument.h:45). Inserts are held
    // as a VIRTUAL layer in the model: sorted lists of virtual file-row / column
    // positions that are blank. Virtual coordinates are what the view, selection,
    // overlay and save all speak; physical (document) coordinates are recovered by
    // subtracting the inserts that precede a position. The overlay is re-keyed to
    // VIRTUAL coords so an inserted blank's edits and a document cell's edits share
    // one store. Undo is out of scope (no QUndoStack in this layer).
    //
    // dataRow is the data-grid row (header-mode shifts it by one file row).
    void insertBlankDataRow(quint64 dataRow);   // dataRow == dataRowCount() ⇒ append
    void insertBlankColumn(int col);             // col == virtualColumnCount() ⇒ append
    bool hasStructuralEdits() const { return !m_insertedFileRows.isEmpty() || !m_insertedCols.isEmpty()
                                          || !m_deletedFileRows.isEmpty() || !m_deletedCols.isEmpty(); }
    bool hasInsertedColumns() const { return !m_insertedCols.isEmpty(); }
    bool hasDeletedColumns() const { return !m_deletedCols.isEmpty(); }

    // --- Deletion / clearing (all integrate with the undo stack via snapshots) ---
    // Clear cell CONTENT over an inclusive VIRTUAL data-row / column rectangle:
    // writes empty strings into the overlay (one dataChanged, no reset).
    void clearCellsInRange(quint64 dataRowLo, quint64 dataRowHi, int colLo, int colHi);
    // Emit dataChanged over the in-window portion of a virtual data-row/column
    // rectangle without mutating anything (used by undo to repaint restored cells).
    void emitCellsChanged(quint64 dataRowLo, quint64 dataRowHi, int colLo, int colHi);
    // Delete the inclusive VIRTUAL data-row range. In header mode the header file
    // row 0 is never touched (the widget already excludes it). beginRemoveRows —
    // no full reset. Inserted-blank rows in the range drop from the inserted set;
    // physical rows add to the deleted-physical set; survivors' overlay/insert
    // positions shift down. Returns the number of data rows removed.
    int deleteDataRows(quint64 dataRowLo, quint64 dataRowHi);
    // Delete the inclusive VIRTUAL column range. beginRemoveColumns — no reset.
    int deleteColumns(int colLo, int colHi);

    // --- Structural snapshot (the undo substrate) ---
    // Captures/restores the FULL edit state (overlay + all four sparse sets).
    // restore is bracketed in beginResetModel/endResetModel — used by undo, where
    // re-adding scattered previously-deleted rows can't be a contiguous insert.
    struct StructuralSnapshot {
        QHash<quint64, QString> overlay;
        QList<quint64> insertedFileRows;
        QList<int> insertedCols;
        QList<quint64> deletedFileRows;
        QList<int> deletedCols;
    };
    StructuralSnapshot captureSnapshot() const;
    void restoreSnapshot(const StructuralSnapshot &s);

    // Virtual ↔ physical (file-row / column). Inserted positions return -1.
    quint64 virtualTotalFileRows() const;        // doc rows + inserted rows (clamped)
    int virtualColumnCount() const;              // maxFieldCount + inserted columns
    qint64 virtualFileRowToPhysical(quint64 vFileRow) const; // -1 == inserted blank
    int virtualColToPhysical(int vCol) const;                // -1 == inserted blank

    // The ONE translation primitive, shared by the model and the off-thread
    // copy/find workers so there is a single source of coordinate truth (no
    // drift). `inserted` MUST be sorted ascending. Returns the physical index, or
    // -1 when `v` is itself an inserted (blank) position. T is quint64 (rows) or
    // int (columns). O(log n).
    template <typename T>
    static qint64 physicalForVirtual(const QList<T> &inserted, T v)
    {
        if (inserted.isEmpty())
            return static_cast<qint64>(v);
        if (std::binary_search(inserted.cbegin(), inserted.cend(), v))
            return -1;
        const auto before = std::lower_bound(inserted.cbegin(), inserted.cend(), v) - inserted.cbegin();
        return static_cast<qint64>(v) - static_cast<qint64>(before);
    }

    // Composed virtual→physical mapping that accounts for BOTH inserted blanks
    // (sorted VIRTUAL positions) and deleted document rows/cols (sorted PHYSICAL
    // positions). Both sets are SPARSE — memory is O(#edits), never O(#rows), so
    // the architecture's "no per-row container" mandate holds. Returns the
    // physical index, or -1 if `v` is an inserted blank. Steps:
    //   1. v ∈ inserted            → -1 (blank, no document backing)
    //   2. k = v − (#inserted < v) → the k-th SURVIVING physical index
    //   3. walk deleted (sorted)   → the k-th index skipping deleted ones
    // O(#inserted-before + #deleted) — both tiny for interactive editing.
    template <typename T>
    static qint64 physicalComposed(const QList<T> &inserted, const QList<T> &deleted, T v)
    {
        if (!inserted.isEmpty() && std::binary_search(inserted.cbegin(), inserted.cend(), v))
            return -1;
        const auto insBefore = inserted.isEmpty() ? 0
            : std::lower_bound(inserted.cbegin(), inserted.cend(), v) - inserted.cbegin();
        qint64 k = static_cast<qint64>(v) - static_cast<qint64>(insBefore);
        if (deleted.isEmpty())
            return k;
        // Find the k-th surviving physical index: advance past each deleted slot
        // at or below the running physical position.
        qint64 p = k;
        for (const T d : deleted) {
            if (static_cast<qint64>(d) <= p) ++p;
            else break;
        }
        return p;
    }

    // Immutable snapshots for worker threads (copied cheaply; the worker reads a
    // frozen view while the UI thread is barred from re-indexing mid-copy/find).
    QList<quint64> insertedFileRowsSnapshot() const { return m_insertedFileRows; }
    QList<int> insertedColsSnapshot() const { return m_insertedCols; }
    QList<quint64> deletedFileRowsSnapshot() const { return m_deletedFileRows; }
    QList<int> deletedColsSnapshot() const { return m_deletedCols; }

    // --- Windowing ---
    quint64 windowBase() const { return m_windowBase; }
    quint64 windowSize() const { return kWindowSize; }
    quint64 totalRows() const;                 // clamped to INT_MAX
    bool setWindowBase(quint64 newBase);       // re-base; returns true if it moved
    // Clamp a desired base so [base, base+window) stays within totalRows.
    quint64 clampBase(quint64 desiredBase) const;

    // absolute ↔ proxy. Returns -1 when the absolute row is outside the window.
    int toProxyRow(quint64 absRow) const;
    quint64 toAbsRow(int proxyRow) const { return m_windowBase + static_cast<quint64>(proxyRow); }

    // --- Sort / filter view-order layer ---
    // A view-only reordering/narrowing of the data rows backed by ONE bounded
    // index array `m_viewOrder` (≤ kWindowSize quint32 = ≤ 1 MiB), set whenever a
    // sort or any filter is active. The no-view path is provably unchanged:
    // `viewRowToDataRow` is the IDENTITY when inactive (one branch returning its
    // argument, never touching m_viewOrder), and `rowCount()` falls back to the
    // windowed count. Available only in the single-window regime (no re-base) —
    // see canSortFilter(); above it the widget hides the affordances.
    bool viewActive() const { return m_viewActive; }
    bool canSortFilter() const { return dataRowCount() <= kWindowSize; }
    int sortColumn() const { return m_sortColumn; }
    Qt::SortOrder sortOrder() const { return m_sortOrder; }

    // The ONE coordinate primitive. Identity fast-path when no view is active:
    // returns its argument without reading the order array. Active ⇒ index into
    // m_viewOrder (windowBase is always 0 in the sort/filter regime).
    quint64 viewRowToDataRow(int viewRow) const
    {
        if (!m_viewActive) return static_cast<quint64>(viewRow);   // identity fast-path
        return m_viewOrder[viewRow];
    }

    // Install / drop the view order. Both bracket beginResetModel/endResetModel
    // (the displayed row set changes wholesale — one repaint, matching the
    // windowing idiom). setViewOrder takes ownership of `order` (moved in);
    // `sortCol`/`order` record the sort that produced it (for the header
    // indicator), independent of whether the order is sort- or filter-driven.
    void setViewOrder(QVector<quint32> &&order, int sortCol, Qt::SortOrder ord);
    void clearViewOrder();

    // Immutable copy of the view order for worker threads. Empty ⇒ no view active
    // (the worker then treats view row == data row, the identity). The UI thread
    // is barred from recomputing/re-indexing until the worker is cancelled+joined,
    // so this frozen snapshot is safe to read off-thread (same contract as the
    // structural-insert snapshots above).
    QVector<quint32> viewOrderSnapshot() const { return m_viewOrder; }

    // --- Absolute cell reads (window-independent) ---
    // The single source of truth for selection/copy/search/save. Consults the
    // overlay first, else CsvDocument. `headerMode` shifts data rows by one.
    QString absoluteCell(quint64 absRow, int col) const;
    // Header text for a data column (real first-row value in header mode, else A/B/C).
    QString columnLabel(int col) const;

    // --- Header-row mode ---
    bool headerMode() const { return m_headerMode; }
    void setHeaderMode(bool on);
    // Number of DATA rows (totalRows minus 1 when header mode is on).
    quint64 dataRowCount() const;

    // --- Edit overlay (absolute FILE-row keyed) ---
    // Keyed by absolute FILE row (file row 0 == the column-header row in header
    // mode), so keys are stable across re-base and header-mode toggles and map
    // 1:1 to the rows the save path walks.
    static quint64 overlayKey(quint64 fileRow, int col) { return (fileRow << 32) | static_cast<quint32>(col); }
    void setOverlayCell(quint64 fileRow, int col, const QString &value);
    void removeOverlayCell(quint64 fileRow, int col); // drop an overlay entry (clear/undo)
    // Set the header-row text for a column (header mode). Writes the overlay at
    // virtual file row 0 and repaints the horizontal header. No-op if header mode
    // is off (there is no header row to edit). Returns true if applied.
    bool setHeaderText(int col, const QString &value);
    bool hasOverlay() const { return !m_overlay.isEmpty(); }
    const QHash<quint64, QString> &overlay() const { return m_overlay; }
    void clearOverlay();

    // --- Read-only (conflict guard) ---
    void setReadOnly(bool ro);
    bool isReadOnly() const { return m_readOnly; }

    // --- Theming ---
    void setCellFont(const QFont &f);

    CsvDocument *document() const { return m_doc; }

    // Re-read the document after a re-index (delimiter change / external reload /
    // post-save): full reset, keep windowBase clamped.
    void documentReloaded();

    // During an async re-index the worker mutates the document's offsets/data
    // out from under the view. Setting this true makes rowCount/columnCount
    // report 0 (inside a model reset) so the view stops requesting cells until
    // documentReloaded() re-attaches. Initial indexing doesn't need this (the
    // model is set on the view only after the first pass completes).
    void setReindexing(bool on);

signals:
    // Emitted when the overlay transitions empty↔non-empty (drives the tab dirty
    // indicator). `dirty` true == there is at least one pending edit.
    void overlayDirtyChanged(bool dirty);

private:
    // Map a proxy/absolute DATA row to the absolute row in the file
    // (adds 1 when header mode shifts row 0 to the column header).
    quint64 fileRowForDataRow(quint64 dataRow) const;

    // Re-key the overlay after a structural insert: every entry whose virtual
    // file-row (or column) is at/after the insert point shifts up by one, so an
    // edited cell keeps reading its own value across the insert.
    void rekeyOverlayForRowInsert(quint64 atFileRow);
    void rekeyOverlayForColInsert(int atCol);

    CsvDocument *m_doc = nullptr;
    quint64 m_windowBase = 0;
    bool m_headerMode = true;
    bool m_readOnly = false;
    QHash<quint64, QString> m_overlay;  // key = (virtualFileRow<<32)|virtualCol, value = edited text
    QFont m_cellFont;
    bool m_hasCellFont = false;
    bool m_reindexing = false;          // true while a worker rebuilds the index

    // Sort/filter view-order layer. `m_viewActive` gates the identity fast-path
    // in viewRowToDataRow() — it is the SOLE truth for "is a view active", set
    // true only by setViewOrder and false only by clearViewOrder, so the inactive
    // branch never touches m_viewOrder. `m_viewOrder` holds the displayed data-row
    // indices (≤ kWindowSize entries). m_sortColumn/m_sortOrder record the active
    // sort for the header indicator (-1 == no sort column, filter-only view).
    bool m_viewActive = false;
    QVector<quint32> m_viewOrder;
    int m_sortColumn = -1;
    Qt::SortOrder m_sortOrder = Qt::AscendingOrder;

    // Structural-edit virtual layer (see the Structural edits section above).
    // Both kept SORTED ascending; positions are VIRTUAL (post-insert) coordinates.
    QList<quint64> m_insertedFileRows;  // virtual file-row indices that are blank
    QList<int> m_insertedCols;          // virtual column indices that are blank
    // Deleted DOCUMENT positions, SORTED ascending, in PHYSICAL coordinates
    // (indices into the immutable CsvDocument). Composed with the inserted sets by
    // physicalComposed(). Sparse — O(#deletes), never O(#rows).
    QList<quint64> m_deletedFileRows;   // physical file rows removed from the view
    QList<int> m_deletedCols;           // physical columns removed from the view
};

#endif // CSVTABLEMODEL_H
