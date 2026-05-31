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

#include "CsvEditCommands.h"

// ---------------------------------------------------------------------------
// ClearCellsCommand
// ---------------------------------------------------------------------------

ClearCellsCommand::ClearCellsCommand(CsvTableModel *model, quint64 dataRowLo,
                                     quint64 dataRowHi, int colLo, int colHi,
                                     QUndoCommand *parent)
    : QUndoCommand(parent)
    , m_model(model)
    , m_rowLo(dataRowLo), m_rowHi(dataRowHi)
    , m_colLo(colLo), m_colHi(colHi)
{
    setText(QObject::tr("Clear Cells"));
}

void ClearCellsCommand::redo()
{
    if (!m_model) return;
    if (!m_captured) {
        // Snapshot the prior overlay entries inside the rectangle exactly once,
        // so undo restores set-values and removes formerly-unset ones.
        const QHash<quint64, QString> &ov = m_model->overlay();
        const bool header = m_model->headerMode();
        for (quint64 d = m_rowLo; d <= m_rowHi; ++d) {
            const quint64 vf = header ? d + 1 : d;
            for (int c = m_colLo; c <= m_colHi; ++c) {
                const quint64 key = CsvTableModel::overlayKey(vf, c);
                auto it = ov.constFind(key);
                if (it != ov.constEnd())
                    m_priorOverlay.insert(key, it.value());
            }
        }
        m_captured = true;
    }
    m_model->clearCellsInRange(m_rowLo, m_rowHi, m_colLo, m_colHi);
}

void ClearCellsCommand::undo()
{
    if (!m_model) return;
    const bool header = m_model->headerMode();
    for (quint64 d = m_rowLo; d <= m_rowHi; ++d) {
        const quint64 vf = header ? d + 1 : d;
        for (int c = m_colLo; c <= m_colHi; ++c) {
            const quint64 key = CsvTableModel::overlayKey(vf, c);
            auto it = m_priorOverlay.constFind(key);
            if (it != m_priorOverlay.constEnd())
                m_model->setOverlayCell(vf, c, it.value()); // restore prior value
            else
                m_model->removeOverlayCell(vf, c);          // was unset → remove
        }
    }
    // Repaint the restored rectangle (no mutation).
    m_model->emitCellsChanged(m_rowLo, m_rowHi, m_colLo, m_colHi);
}

// ---------------------------------------------------------------------------
// DeleteRowsCommand
// ---------------------------------------------------------------------------

DeleteRowsCommand::DeleteRowsCommand(CsvTableModel *model, quint64 dataRowLo,
                                     quint64 dataRowHi, QUndoCommand *parent)
    : QUndoCommand(parent)
    , m_model(model)
    , m_rowLo(dataRowLo), m_rowHi(dataRowHi)
{
    setText(QObject::tr("Delete Row(s)"));
}

void DeleteRowsCommand::redo()
{
    if (!m_model) return;
    if (!m_captured) {
        m_before = m_model->captureSnapshot();
        m_captured = true;
    }
    m_model->deleteDataRows(m_rowLo, m_rowHi);
}

void DeleteRowsCommand::undo()
{
    if (!m_model) return;
    m_model->restoreSnapshot(m_before);
}

// ---------------------------------------------------------------------------
// DeleteColumnsCommand
// ---------------------------------------------------------------------------

DeleteColumnsCommand::DeleteColumnsCommand(CsvTableModel *model, int colLo, int colHi,
                                           QUndoCommand *parent)
    : QUndoCommand(parent)
    , m_model(model)
    , m_colLo(colLo), m_colHi(colHi)
{
    setText(QObject::tr("Delete Column(s)"));
}

void DeleteColumnsCommand::redo()
{
    if (!m_model) return;
    if (!m_captured) {
        m_before = m_model->captureSnapshot();
        m_captured = true;
    }
    m_model->deleteColumns(m_colLo, m_colHi);
}

void DeleteColumnsCommand::undo()
{
    if (!m_model) return;
    m_model->restoreSnapshot(m_before);
}
