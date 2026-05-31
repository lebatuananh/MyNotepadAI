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

#ifndef CSVEDITCOMMANDS_H
#define CSVEDITCOMMANDS_H

#include "CsvTableModel.h"

#include <QUndoCommand>
#include <QHash>
#include <QString>

// Undo/redo commands for the CSV table preview. Each owns a CsvTableModel* (not
// owning the model — the model outlives the stack) and drives the model's
// mutators on redo, restoring captured state on undo. One command == one undo
// step regardless of how many cells/rows/columns it spans.

// Clear the CONTENT of an inclusive virtual data-row / column rectangle. Undo
// restores exactly the overlay entries that existed before (targeted, NO model
// reset — the clear path stays incremental).
class ClearCellsCommand : public QUndoCommand
{
public:
    ClearCellsCommand(CsvTableModel *model, quint64 dataRowLo, quint64 dataRowHi,
                      int colLo, int colHi, QUndoCommand *parent = nullptr);
    void redo() override;
    void undo() override;

private:
    CsvTableModel *m_model;
    quint64 m_rowLo, m_rowHi;
    int m_colLo, m_colHi;
    // Overlay entries (by virtual file-row key) that existed before the clear, so
    // undo can put back the exact prior values; keys absent here were unset and
    // are removed on undo.
    QHash<quint64, QString> m_priorOverlay;
    bool m_captured = false;
};

// Delete the virtual data rows intersecting [rowLo, rowHi]. Undo restores the
// full structural snapshot (re-adding scattered rows is not a contiguous insert,
// so undo uses a model reset — redo uses beginRemoveRows, no reset).
class DeleteRowsCommand : public QUndoCommand
{
public:
    DeleteRowsCommand(CsvTableModel *model, quint64 dataRowLo, quint64 dataRowHi,
                      QUndoCommand *parent = nullptr);
    void redo() override;
    void undo() override;

private:
    CsvTableModel *m_model;
    quint64 m_rowLo, m_rowHi;
    CsvTableModel::StructuralSnapshot m_before;
    bool m_captured = false;
};

// Delete the virtual columns intersecting [colLo, colHi]. Same undo strategy.
class DeleteColumnsCommand : public QUndoCommand
{
public:
    DeleteColumnsCommand(CsvTableModel *model, int colLo, int colHi,
                         QUndoCommand *parent = nullptr);
    void redo() override;
    void undo() override;

private:
    CsvTableModel *m_model;
    int m_colLo, m_colHi;
    CsvTableModel::StructuralSnapshot m_before;
    bool m_captured = false;
};

#endif // CSVEDITCOMMANDS_H
