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

#include "CsvTableModel.h"
#include "CsvDocument.h"

#include <algorithm>
#include <climits>

// The overlay is keyed by ABSOLUTE FILE ROW (not data row): file row 0 in header
// mode is the column header (never editable), data rows are file rows shifted by
// one. Keying by file row keeps save (which walks file rows) and absolute reads
// consistent with no remapping across re-base.

CsvTableModel::CsvTableModel(CsvDocument *doc, QObject *parent)
    : QAbstractTableModel(parent)
    , m_doc(doc)
{
}

quint64 CsvTableModel::totalRows() const
{
    if (!m_doc) return 0;
    // VIRTUAL total: physical document rows, plus inserted blanks, minus deleted
    // document rows. All row math (dataRowCount/clampBase/window) flows from here,
    // so every structural edit is accounted for at once. Clamped to INT_MAX.
    const quint64 phys = m_doc->totalRows();
    const qint64 t = static_cast<qint64>(phys)
                   + m_insertedFileRows.size()
                   - m_deletedFileRows.size();
    const quint64 tu = t < 0 ? 0 : static_cast<quint64>(t);
    return tu > static_cast<quint64>(INT_MAX) ? static_cast<quint64>(INT_MAX) : tu;
}

quint64 CsvTableModel::virtualTotalFileRows() const
{
    return totalRows();
}

int CsvTableModel::virtualColumnCount() const
{
    const int phys = m_doc ? m_doc->maxFieldCount() : 0;
    return phys + m_insertedCols.size() - m_deletedCols.size();
}

qint64 CsvTableModel::virtualFileRowToPhysical(quint64 vFileRow) const
{
    return physicalComposed(m_insertedFileRows, m_deletedFileRows, vFileRow);
}

int CsvTableModel::virtualColToPhysical(int vCol) const
{
    return static_cast<int>(physicalComposed(m_insertedCols, m_deletedCols, vCol));
}

quint64 CsvTableModel::dataRowCount() const
{
    const quint64 t = totalRows();
    if (m_headerMode && t > 0)
        return t - 1;
    return t;
}

quint64 CsvTableModel::fileRowForDataRow(quint64 dataRow) const
{
    return m_headerMode ? dataRow + 1 : dataRow;
}

int CsvTableModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid() || m_reindexing) return 0;
    // A sort/filter view replaces the row set wholesale; its size is the displayed
    // count. windowBase is always 0 in this regime (canSortFilter ⇒ single window).
    if (m_viewActive) return m_viewOrder.size();
    const quint64 dr = dataRowCount();
    if (dr <= m_windowBase) return 0;
    return static_cast<int>(qMin<quint64>(dr - m_windowBase, kWindowSize));
}

int CsvTableModel::columnCount(const QModelIndex &parent) const
{
    if (parent.isValid() || m_reindexing) return 0;
    if (!m_doc) return 0;
    // VIRTUAL column count: physical fields plus inserted blank columns.
    return virtualColumnCount();
}

QVariant CsvTableModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || !m_doc) return {};
    if (role == Qt::FontRole)
        return m_hasCellFont ? QVariant(m_cellFont) : QVariant();
    if (role != Qt::DisplayRole && role != Qt::EditRole && role != Qt::ToolTipRole)
        return {};

    // Row translation: m_windowBase + proxyRow is the view row; viewRowToDataRow
    // maps it to the data row (identity when no sort/filter view is active, where
    // windowBase may be large; in the view regime windowBase==0 so the sum is the
    // proxy row). The sum is < dataRowCount() ≤ INT_MAX, so the int narrowing is
    // safe.
    const quint64 viewRow = m_windowBase + static_cast<quint64>(index.row());
    const quint64 absDataRow = viewRowToDataRow(static_cast<int>(viewRow));
    const int vCol = index.column();
    const quint64 vFileRow = fileRowForDataRow(absDataRow);
    // Overlay first (no mmap touch). Overlay is keyed by VIRTUAL (fileRow,col),
    // so an inserted blank's edits and a document cell's edits both resolve here.
    auto it = m_overlay.constFind(overlayKey(vFileRow, vCol));
    if (it != m_overlay.constEnd())
        return it.value();
    // Translate virtual → physical. An inserted blank row or column has no
    // document backing → empty (it only ever carries overlay values).
    const qint64 physFileRow = virtualFileRowToPhysical(vFileRow);
    const int physCol = virtualColToPhysical(vCol);
    if (physFileRow < 0 || physCol < 0)
        return QString();
    // DISPLAY hot path uses the document's LRU (sequential scroll amortized
    // O(1)). m_doc->cell mutates the LRU; legal here because the const applies to
    // the model, not the pointee, and data() is only ever called on the UI thread.
    return m_doc->cell(static_cast<quint64>(physFileRow), physCol);
}

QString CsvTableModel::absoluteCell(quint64 absDataRow, int col) const
{
    if (!m_doc || col < 0) return {};
    const quint64 vFileRow = fileRowForDataRow(absDataRow);
    // Overlay first (no mmap touch). Virtual-keyed, same as data().
    auto it = m_overlay.constFind(overlayKey(vFileRow, col));
    if (it != m_overlay.constEnd())
        return it.value();
    // Translate virtual → physical; inserted blank ⇒ empty.
    const qint64 physFileRow = virtualFileRowToPhysical(vFileRow);
    const int physCol = virtualColToPhysical(col);
    if (physFileRow < 0 || physCol < 0)
        return QString();
    // parseRowUncached is window-independent and does not mutate the LRU, so it is
    // safe for selection/copy/search reads that may run on a worker thread.
    const QVector<QString> fields = m_doc->parseRowUncached(static_cast<quint64>(physFileRow));
    if (physCol < fields.size())
        return fields.at(physCol);
    return {};
}

QString CsvTableModel::columnLabel(int col) const
{
    if (col < 0) return {};
    if (m_headerMode && m_doc && totalRows() > 0) {
        // Real header from VIRTUAL file row 0, with overlay precedence. col is a
        // virtual column; the overlay is virtual-keyed.
        auto it = m_overlay.constFind(overlayKey(0, col));
        if (it != m_overlay.constEnd() && !it.value().isEmpty())
            return it.value();
        // Document fallback only for a physical (non-inserted) column.
        const int physCol = virtualColToPhysical(col);
        if (physCol >= 0) {
            const QVector<QString> fields = m_doc->parseRowUncached(0);
            if (physCol < fields.size() && !fields.at(physCol).isEmpty())
                return fields.at(physCol);
        }
        // Fall through to A/B/C for empty / inserted header cells.
    }
    // A/B/C... spreadsheet-style column names.
    QString label;
    int n = col;
    do {
        label.prepend(QChar('A' + (n % 26)));
        n = n / 26 - 1;
    } while (n >= 0);
    return label;
}

QVariant CsvTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Horizontal) {
        if (role == Qt::DisplayRole || role == Qt::ToolTipRole)
            return columnLabel(section);
        return {};
    }
    // Vertical header: ORIGINAL (1-based) data row number. Under a sort/filter
    // view this is the source file row, not the view position — so the user sees
    // which file row each displayed row is. Identity when no view is active.
    if (role == Qt::DisplayRole) {
        const quint64 viewRow = m_windowBase + static_cast<quint64>(section);
        const quint64 absDataRow = viewRowToDataRow(static_cast<int>(viewRow));
        return QString::number(absDataRow + 1);
    }
    if (role == Qt::FontRole)
        return m_hasCellFont ? QVariant(m_cellFont) : QVariant();
    return {};
}

Qt::ItemFlags CsvTableModel::flags(const QModelIndex &index) const
{
    Qt::ItemFlags f = QAbstractTableModel::flags(index);
    if (index.isValid() && !m_readOnly)
        f |= Qt::ItemIsEditable;
    else
        f &= ~Qt::ItemIsEditable;
    return f;
}

bool CsvTableModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (!index.isValid() || role != Qt::EditRole || m_readOnly)
        return false;
    // proxy → viewRow → dataRow → vFileRow. The overlay key is unchanged (still
    // virtual file row), so an edit through a sorted/filtered view sticks to its
    // logical row and rides along the next recompute. Identity when no view.
    const quint64 viewRow = m_windowBase + static_cast<quint64>(index.row());
    const quint64 absDataRow = viewRowToDataRow(static_cast<int>(viewRow));
    const quint64 fileRow = fileRowForDataRow(absDataRow);
    setOverlayCell(fileRow, index.column(), value.toString());
    emit dataChanged(index, index, {Qt::DisplayRole, Qt::EditRole});
    return true;
}

void CsvTableModel::setOverlayCell(quint64 fileRow, int col, const QString &value)
{
    // "dirty" == overlay non-empty OR a structural insert pending. Only emit the
    // empty→dirty transition when BOTH were previously empty.
    const bool wasClean = m_overlay.isEmpty() && !hasStructuralEdits();
    m_overlay.insert(overlayKey(fileRow, col), value);
    if (wasClean)
        emit overlayDirtyChanged(true);
}

bool CsvTableModel::setHeaderText(int col, const QString &value)
{
    if (!m_headerMode || m_readOnly || col < 0 || col >= columnCount())
        return false;
    // The header is virtual file row 0; the overlay is virtual-keyed, and
    // columnLabel() consults overlayKey(0,col) first, so this updates the label.
    setOverlayCell(0, col, value);
    emit headerDataChanged(Qt::Horizontal, col, col);
    return true;
}

void CsvTableModel::clearOverlay()
{
    // Single lifecycle hook for discarding ALL pending edits — overlay AND the
    // structural virtual layer. Called post-save (after the re-index re-reads the
    // now-on-disk inserted rows) and on documentReloaded(); inserts must drop here
    // or they would double-count against the freshly indexed document.
    if (m_overlay.isEmpty() && !hasStructuralEdits()) return;
    m_overlay.clear();
    m_insertedFileRows.clear();
    m_insertedCols.clear();
    m_deletedFileRows.clear();
    m_deletedCols.clear();
    emit overlayDirtyChanged(false);
    // Visible cells must repaint from the document now.
    const int rc = rowCount();
    const int cc = columnCount();
    if (rc > 0 && cc > 0)
        emit dataChanged(index(0, 0), index(rc - 1, cc - 1), {Qt::DisplayRole, Qt::EditRole});
}

void CsvTableModel::rekeyOverlayForRowInsert(quint64 atFileRow)
{
    // Every overlay entry whose VIRTUAL file row is >= the insert point shifts
    // down by one so an edited cell keeps its value across the insert. Packing
    // matches overlayKey(): key = (fileRow<<32) | quint32(col).
    if (m_overlay.isEmpty()) return;
    QHash<quint64, QString> shifted;
    shifted.reserve(m_overlay.size());
    for (auto it = m_overlay.cbegin(); it != m_overlay.cend(); ++it) {
        const quint64 vf = it.key() >> 32;
        const int c = static_cast<int>(it.key() & 0xffffffffu);
        shifted.insert(overlayKey(vf >= atFileRow ? vf + 1 : vf, c), it.value());
    }
    m_overlay.swap(shifted);
}

void CsvTableModel::rekeyOverlayForColInsert(int atCol)
{
    // Symmetric to the row re-key, on the low 32 bits (the column).
    if (m_overlay.isEmpty()) return;
    QHash<quint64, QString> shifted;
    shifted.reserve(m_overlay.size());
    for (auto it = m_overlay.cbegin(); it != m_overlay.cend(); ++it) {
        const quint64 vf = it.key() >> 32;
        const int c = static_cast<int>(it.key() & 0xffffffffu);
        shifted.insert(overlayKey(vf, c >= atCol ? c + 1 : c), it.value());
    }
    m_overlay.swap(shifted);
}

void CsvTableModel::insertBlankDataRow(quint64 dataRow)
{
    if (!m_doc || m_readOnly) return;
    // dataRow is a DATA-grid row; convert to the virtual FILE row the insert
    // occupies (header mode shifts by one). Append == dataRowCount().
    const quint64 dr = dataRowCount();
    if (dataRow > dr) dataRow = dr;
    const quint64 atFileRow = fileRowForDataRow(dataRow);

    // Header-mode seed: when the document has NO file rows at all, file row 0
    // (the header) does not exist yet. Inserting only the data row would let it
    // be consumed as the header (dataRowCount stays 0 — the "must click twice"
    // bug). So also insert a blank header file row 0 in that one case.
    const bool seedHeader = m_headerMode && totalRows() == 0;

    // The widget guarantees dataRow is within the current window before calling
    // (it rebases first), so the proxy row is valid for begin/endInsertRows.
    const int proxy = static_cast<int>(dataRow - m_windowBase);
    const bool wasClean = m_overlay.isEmpty() && !hasStructuralEdits();

    beginInsertRows(QModelIndex(), proxy, proxy);
    // Shift existing inserted-row marks at/after the insert point up by one.
    for (quint64 &r : m_insertedFileRows)
        if (r >= atFileRow) ++r;
    rekeyOverlayForRowInsert(atFileRow);
    m_insertedFileRows.insert(
        std::upper_bound(m_insertedFileRows.begin(), m_insertedFileRows.end(), atFileRow),
        atFileRow);
    if (seedHeader) {
        // file row 0 is the blank header; atFileRow was 1, so 0 sorts before it.
        m_insertedFileRows.insert(
            std::upper_bound(m_insertedFileRows.begin(), m_insertedFileRows.end(), quint64(0)),
            quint64(0));
    }
    endInsertRows();

    if (wasClean) emit overlayDirtyChanged(true);
}

void CsvTableModel::insertBlankColumn(int col)
{
    if (!m_doc || m_readOnly) return;
    const int vcc = virtualColumnCount();
    if (col < 0) col = 0;
    if (col > vcc) col = vcc;  // append
    const bool wasClean = m_overlay.isEmpty() && !hasStructuralEdits();

    beginInsertColumns(QModelIndex(), col, col);
    for (int &c : m_insertedCols)
        if (c >= col) ++c;
    rekeyOverlayForColInsert(col);
    m_insertedCols.insert(
        std::upper_bound(m_insertedCols.begin(), m_insertedCols.end(), col),
        col);
    endInsertColumns();

    if (wasClean) emit overlayDirtyChanged(true);
}

void CsvTableModel::removeOverlayCell(quint64 fileRow, int col)
{
    if (m_overlay.remove(overlayKey(fileRow, col)) > 0 && m_overlay.isEmpty()
        && !hasStructuralEdits())
        emit overlayDirtyChanged(false);
}

void CsvTableModel::clearCellsInRange(quint64 dataRowLo, quint64 dataRowHi, int colLo, int colHi)
{
    if (!m_doc || m_readOnly) return;
    const quint64 dr = dataRowCount();
    if (dr == 0) return;
    dataRowHi = qMin(dataRowHi, dr - 1);
    colHi = qMin(colHi, virtualColumnCount() - 1);
    if (dataRowLo > dataRowHi || colLo > colHi) return;

    // Clearing = write empty strings into the overlay at every (virtual file row,
    // virtual col) in the rectangle. No structural change → no reset, just one
    // dataChanged spanning the in-window portion of the rect.
    for (quint64 d = dataRowLo; d <= dataRowHi; ++d) {
        const quint64 vFileRow = fileRowForDataRow(d);
        for (int c = colLo; c <= colHi; ++c)
            setOverlayCell(vFileRow, c, QString());
    }
    const int rLo = toProxyRow(dataRowLo);
    const int rHi = toProxyRow(dataRowHi);
    if (rLo >= 0 && rHi >= 0)
        emit dataChanged(index(rLo, colLo), index(rHi, colHi), {Qt::DisplayRole, Qt::EditRole});
}

void CsvTableModel::emitCellsChanged(quint64 dataRowLo, quint64 dataRowHi, int colLo, int colHi)
{
    const quint64 dr = dataRowCount();
    if (dr == 0) return;
    dataRowHi = qMin(dataRowHi, dr - 1);
    colHi = qMin(colHi, virtualColumnCount() - 1);
    if (dataRowLo > dataRowHi || colLo > colHi) return;
    const int rLo = toProxyRow(dataRowLo);
    const int rHi = toProxyRow(dataRowHi);
    if (rLo >= 0 && rHi >= 0)
        emit dataChanged(index(rLo, colLo), index(rHi, colHi), {Qt::DisplayRole, Qt::EditRole});
}

CsvTableModel::StructuralSnapshot CsvTableModel::captureSnapshot() const
{
    return StructuralSnapshot{ m_overlay, m_insertedFileRows, m_insertedCols,
                               m_deletedFileRows, m_deletedCols };
}

void CsvTableModel::restoreSnapshot(const StructuralSnapshot &s)
{
    // Undo/redo of structural edits: re-adding scattered previously-deleted rows
    // (or removing earlier inserts) is not a contiguous begin/endInsert range, so
    // this is the one path that uses a full reset. Selection is re-projected by
    // the widget afterwards.
    const bool wasDirty = !m_overlay.isEmpty() || hasStructuralEdits();
    beginResetModel();
    m_overlay = s.overlay;
    m_insertedFileRows = s.insertedFileRows;
    m_insertedCols = s.insertedCols;
    m_deletedFileRows = s.deletedFileRows;
    m_deletedCols = s.deletedCols;
    m_windowBase = clampBase(m_windowBase);
    endResetModel();
    const bool nowDirty = !m_overlay.isEmpty() || hasStructuralEdits();
    if (wasDirty != nowDirty)
        emit overlayDirtyChanged(nowDirty);
}

int CsvTableModel::deleteDataRows(quint64 dataRowLo, quint64 dataRowHi)
{
    if (!m_doc || m_readOnly) return 0;
    const quint64 dr = dataRowCount();
    if (dr == 0) return 0;
    dataRowHi = qMin(dataRowHi, dr - 1);
    if (dataRowLo > dataRowHi) return 0;

    // Convert the data-row range to a VIRTUAL FILE-row range (header shift). The
    // header file row 0 is never in a data-row range, so it is implicitly safe.
    const quint64 vLo = fileRowForDataRow(dataRowLo);
    const quint64 vHi = fileRowForDataRow(dataRowHi);
    const quint64 count = vHi - vLo + 1;

    // Proxy range for the removal signal (the widget keeps the range in-window).
    const int proxyLo = static_cast<int>(dataRowLo - m_windowBase);
    const int proxyHi = static_cast<int>(dataRowHi - m_windowBase);

    const bool wasDirty = !m_overlay.isEmpty() || hasStructuralEdits();
    beginRemoveRows(QModelIndex(), proxyLo, proxyHi);

    // Partition each virtual file row in [vLo, vHi]: an inserted blank drops from
    // the inserted set; a physical row adds its PHYSICAL index to the deleted set.
    // Physical indices are computed against the ORIGINAL sets FIRST (before any
    // mutation), else each insert into m_deletedFileRows would skew the next
    // physicalComposed() result.
    QList<quint64> newlyDeletedPhys;
    for (quint64 vf = vLo; vf <= vHi; ++vf) {
        const qint64 phys = physicalComposed(m_insertedFileRows, m_deletedFileRows, vf);
        if (phys >= 0) newlyDeletedPhys.append(static_cast<quint64>(phys));
    }
    QList<quint64> newInserted;
    for (const quint64 vf : m_insertedFileRows) {
        if (vf < vLo) { newInserted.append(vf); }
        else if (vf > vHi) { newInserted.append(vf - count); } // shift survivors down
        // vf in [vLo,vHi] → inserted blank removed (dropped)
    }
    m_insertedFileRows = std::move(newInserted);
    // Merge the new physical deletions into the sorted deleted set.
    for (const quint64 p : newlyDeletedPhys)
        m_deletedFileRows.insert(
            std::upper_bound(m_deletedFileRows.begin(), m_deletedFileRows.end(), p), p);

    // Re-key the overlay: drop entries in the removed range, shift those above
    // down by `count`.
    if (!m_overlay.isEmpty()) {
        QHash<quint64, QString> shifted;
        shifted.reserve(m_overlay.size());
        for (auto it = m_overlay.cbegin(); it != m_overlay.cend(); ++it) {
            const quint64 vf = it.key() >> 32;
            const int c = static_cast<int>(it.key() & 0xffffffffu);
            if (vf < vLo) shifted.insert(it.key(), it.value());
            else if (vf > vHi) shifted.insert(overlayKey(vf - count, c), it.value());
            // vf in range → dropped
        }
        m_overlay.swap(shifted);
    }
    endRemoveRows();

    const bool nowDirty = !m_overlay.isEmpty() || hasStructuralEdits();
    if (wasDirty != nowDirty)
        emit overlayDirtyChanged(nowDirty);
    return static_cast<int>(dataRowHi - dataRowLo + 1);
}

int CsvTableModel::deleteColumns(int colLo, int colHi)
{
    if (!m_doc || m_readOnly) return 0;
    const int vcc = virtualColumnCount();
    if (vcc == 0) return 0;
    colLo = qMax(0, colLo);
    colHi = qMin(colHi, vcc - 1);
    if (colLo > colHi) return 0;
    const int count = colHi - colLo + 1;

    const bool wasDirty = !m_overlay.isEmpty() || hasStructuralEdits();
    beginRemoveColumns(QModelIndex(), colLo, colHi);

    QList<int> newlyDeletedPhys;
    for (int c = colLo; c <= colHi; ++c) {
        const qint64 phys = physicalComposed(m_insertedCols, m_deletedCols, c);
        if (phys >= 0) newlyDeletedPhys.append(static_cast<int>(phys));
    }
    QList<int> newInserted;
    for (const int c : m_insertedCols) {
        if (c < colLo) newInserted.append(c);
        else if (c > colHi) newInserted.append(c - count);
    }
    m_insertedCols = std::move(newInserted);
    for (const int pc : newlyDeletedPhys)
        m_deletedCols.insert(
            std::upper_bound(m_deletedCols.begin(), m_deletedCols.end(), pc), pc);

    if (!m_overlay.isEmpty()) {
        QHash<quint64, QString> shifted;
        shifted.reserve(m_overlay.size());
        for (auto it = m_overlay.cbegin(); it != m_overlay.cend(); ++it) {
            const quint64 vf = it.key() >> 32;
            const int c = static_cast<int>(it.key() & 0xffffffffu);
            if (c < colLo) shifted.insert(it.key(), it.value());
            else if (c > colHi) shifted.insert(overlayKey(vf, c - count), it.value());
        }
        m_overlay.swap(shifted);
    }
    endRemoveColumns();

    const bool nowDirty = !m_overlay.isEmpty() || hasStructuralEdits();
    if (wasDirty != nowDirty)
        emit overlayDirtyChanged(nowDirty);
    return count;
}

quint64 CsvTableModel::clampBase(quint64 desiredBase) const
{
    const quint64 dr = dataRowCount();
    if (dr <= kWindowSize)
        return 0;
    const quint64 maxBase = dr - kWindowSize;
    return qMin(desiredBase, maxBase);
}

bool CsvTableModel::setWindowBase(quint64 newBase)
{
    const quint64 clamped = clampBase(newBase);
    if (clamped == m_windowBase)
        return false;
    beginResetModel();
    m_windowBase = clamped;
    endResetModel();
    return true;
}

int CsvTableModel::toProxyRow(quint64 absDataRow) const
{
    if (absDataRow < m_windowBase) return -1;
    const quint64 proxy = absDataRow - m_windowBase;
    if (proxy >= static_cast<quint64>(rowCount())) return -1;
    return static_cast<int>(proxy);
}

void CsvTableModel::setViewOrder(QVector<quint32> &&order, int sortCol, Qt::SortOrder ord)
{
    // Wholesale row-set change → a model reset is the correct single-repaint
    // signal (matches the windowing idiom). m_viewActive becomes true here and
    // ONLY here, so the inactive identity fast-path never reads m_viewOrder.
    beginResetModel();
    m_viewOrder = std::move(order);
    m_viewActive = true;
    m_sortColumn = sortCol;
    m_sortOrder = ord;
    endResetModel();
}

void CsvTableModel::clearViewOrder()
{
    if (!m_viewActive) {
        // Even when inactive, keep the recorded sort column reset so a stale
        // indicator value can't leak; cheap and avoids a redundant reset.
        m_sortColumn = -1;
        return;
    }
    beginResetModel();
    m_viewOrder.clear();
    m_viewActive = false;
    m_sortColumn = -1;
    m_sortOrder = Qt::AscendingOrder;
    endResetModel();
}

void CsvTableModel::setHeaderMode(bool on)
{
    if (m_headerMode == on) return;
    beginResetModel();
    m_headerMode = on;
    // Row 0 moves between header and data, invalidating the order and the
    // column-numeric classification — drop the view (inline, this reset covers it).
    m_viewOrder.clear();
    m_viewActive = false;
    m_sortColumn = -1;
    m_sortOrder = Qt::AscendingOrder;
    m_windowBase = clampBase(m_windowBase);
    endResetModel();
}

void CsvTableModel::setReadOnly(bool ro)
{
    if (m_readOnly == ro) return;
    m_readOnly = ro;
    // Flags changed for every cell; a layout-preserving reset is overkill, but
    // editability is read in flags() lazily, so just nudge the header/view.
    emit headerDataChanged(Qt::Horizontal, 0, qMax(0, columnCount() - 1));
}

void CsvTableModel::setCellFont(const QFont &f)
{
    m_cellFont = f;
    m_hasCellFont = true;
    const int rc = rowCount();
    const int cc = columnCount();
    if (rc > 0 && cc > 0)
        emit dataChanged(index(0, 0), index(rc - 1, cc - 1), {Qt::FontRole});
}

void CsvTableModel::documentReloaded()
{
    beginResetModel();
    m_reindexing = false;
    // A reload means the document is the new source of truth (delimiter change,
    // external reload, post-save re-index). Any virtual inserts were either
    // persisted (post-save: they now physically exist) or are being discarded —
    // either way they must drop, mirroring clearOverlay()'s lifecycle.
    m_insertedFileRows.clear();
    m_insertedCols.clear();
    m_deletedFileRows.clear();
    m_deletedCols.clear();
    // A reload re-bases the whole grid; any sort/filter view is invalid now.
    m_viewOrder.clear();
    m_viewActive = false;
    m_sortColumn = -1;
    m_sortOrder = Qt::AscendingOrder;
    m_windowBase = clampBase(m_windowBase);
    endResetModel();
}

void CsvTableModel::setReindexing(bool on)
{
    if (m_reindexing == on) return;
    beginResetModel();
    m_reindexing = on;
    endResetModel();
}

