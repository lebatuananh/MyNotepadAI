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

#include "RecentWorkspaceOpenDialog.h"

#include "QuickFileOpenDialog.h"
#include "widgets/FuzzyHighlightDelegate.h"

#include <QAbstractListModel>
#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListView>
#include <QShowEvent>
#include <QStringList>
#include <QVBoxLayout>

// =========================================================================
//  RecentWorkspaceModel — virtualized list over the current result set.
//  A result is (entry index, match positions). Empty query yields one result
//  per entry in MRU order with no positions; a query yields the fuzzy top-K.
// =========================================================================
class RecentWorkspaceModel : public QAbstractListModel
{
public:
    explicit RecentWorkspaceModel(const QVector<RecentWorkspaceOpenDialog::Entry> *entries,
                                  QObject *parent)
        : QAbstractListModel(parent), m_entries(entries)
    {
    }

    void setResults(QVector<QuickFileOpenCandidate> &&results)
    {
        beginResetModel();
        m_results = std::move(results);
        endResetModel();
    }

    int entryIndexAt(int row) const
    {
        if (row < 0 || row >= m_results.size()) return -1;
        return m_results.at(row).index;
    }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : static_cast<int>(m_results.size());
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || !m_entries) return {};
        const int row = index.row();
        if (row < 0 || row >= m_results.size()) return {};
        const QuickFileOpenCandidate &cand = m_results.at(row);
        if (cand.index < 0 || cand.index >= m_entries->size()) return {};
        const RecentWorkspaceOpenDialog::Entry &e = m_entries->at(cand.index);

        switch (role) {
        case Qt::DisplayRole:
            return e.matchText;
        case FuzzyHighlightDelegate::LeadingIconRole:
            return e.icon;
        case FuzzyHighlightDelegate::TrailingTextRole:
            return e.statusText;
        case FuzzyHighlightDelegate::MatchPositionsRole: {
            QVector<int> v;
            v.reserve(cand.positions.size());
            for (qint32 p : cand.positions) v.append(static_cast<int>(p));
            return QVariant::fromValue(v);
        }
        default:
            return {};
        }
    }

private:
    const QVector<RecentWorkspaceOpenDialog::Entry> *m_entries;
    QVector<QuickFileOpenCandidate> m_results;
};

// =========================================================================
//  RecentWorkspaceOpenDialog
// =========================================================================

RecentWorkspaceOpenDialog::RecentWorkspaceOpenDialog(QVector<Entry> entries, QWidget *parent)
    : QDialog(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
    , m_entries(std::move(entries))
{
    setMinimumWidth(560);
    setMaximumHeight(420);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(2);

    m_lineEdit = new QLineEdit(this);
    m_lineEdit->setPlaceholderText(tr("Type to search recent workspaces..."));
    m_lineEdit->setClearButtonEnabled(true);
    layout->addWidget(m_lineEdit);

    m_listView = new QListView(this);
    m_listView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_listView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_listView->setUniformItemSizes(true);
    m_model = new RecentWorkspaceModel(&m_entries, this);
    m_listView->setModel(m_model);
    m_listView->setItemDelegate(new FuzzyHighlightDelegate(m_listView));
    layout->addWidget(m_listView);

    // Build the fuzzy index once from the match strings (index-aligned with
    // m_entries). Reused for every keystroke; entries are immutable here.
    QStringList matchStrings;
    matchStrings.reserve(m_entries.size());
    for (const Entry &e : m_entries)
        matchStrings.append(e.matchText);
    m_index = std::make_shared<const FileIndexCache>(
        FileIndexCache::build(matchStrings, /*isGitRepo=*/false));

    m_lineEdit->installEventFilter(this);
    m_listView->installEventFilter(this);
    qApp->installEventFilter(this);

    connect(m_lineEdit, &QLineEdit::textChanged, this, &RecentWorkspaceOpenDialog::onTextChanged);
    connect(m_listView, &QListView::activated, this, &RecentWorkspaceOpenDialog::onItemActivated);
    connect(m_listView, &QListView::clicked, this, &RecentWorkspaceOpenDialog::onItemActivated);

    applyFilter(QString());   // initial: full MRU list
    m_lineEdit->setFocus();
}

RecentWorkspaceOpenDialog::~RecentWorkspaceOpenDialog()
{
    qApp->removeEventFilter(this);
}

void RecentWorkspaceOpenDialog::applyFilter(const QString &pattern)
{
    QVector<QuickFileOpenCandidate> results;

    if (pattern.isEmpty()) {
        // Empty query → every entry in its given (MRU) order, no scoring, no
        // positions. Clearing the query always rebuilds this full list, so the
        // view never gets stuck on a stale filtered result set.
        results.reserve(m_entries.size());
        for (int i = 0; i < m_entries.size(); ++i) {
            QuickFileOpenCandidate cand;
            cand.index = i;
            results.append(cand);
        }
    } else if (m_index) {
        // Non-empty query → reuse the shared fuzzy core. computeMatches returns
        // survivors best-first with match positions backtraced; index maps back
        // into m_entries. No match → empty vector → empty model.
        results = QuickFileOpenDialog::computeMatches(*m_index, pattern,
                                                      QuickFileOpenDialog::kMaxResults);
    }

    const bool hadResults = !results.isEmpty();
    m_model->setResults(std::move(results));
    if (hadResults)
        m_listView->setCurrentIndex(m_model->index(0, 0));
}

void RecentWorkspaceOpenDialog::onTextChanged(const QString &text)
{
    applyFilter(text);
}

void RecentWorkspaceOpenDialog::onItemActivated(const QModelIndex &index)
{
    if (!index.isValid()) return;
    const int entryIdx = m_model->entryIndexAt(index.row());
    if (entryIdx < 0 || entryIdx >= m_entries.size()) return;
    m_selectedUri = m_entries.at(entryIdx).openUri;
    if (m_selectedUri.isEmpty()) return;
    accept();
}

void RecentWorkspaceOpenDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    activateWindow();
    m_lineEdit->setFocus();
}

bool RecentWorkspaceOpenDialog::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonPress) {
        auto *target = qobject_cast<QWidget *>(obj);
        if (target && !isAncestorOf(target) && target != this) {
            reject();
            return false;
        }
    }

    if (event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        if (ke->key() == Qt::Key_Escape) {
            reject();
            return true;
        }
        if (obj == m_lineEdit) {
            switch (ke->key()) {
            case Qt::Key_Down: {
                int row = m_listView->currentIndex().row() + 1;
                if (row < m_model->rowCount())
                    m_listView->setCurrentIndex(m_model->index(row, 0));
                return true;
            }
            case Qt::Key_Up: {
                int row = m_listView->currentIndex().row() - 1;
                if (row >= 0)
                    m_listView->setCurrentIndex(m_model->index(row, 0));
                return true;
            }
            case Qt::Key_Return:
            case Qt::Key_Enter:
                onItemActivated(m_listView->currentIndex());
                return true;
            default:
                break;
            }
        } else if (obj == m_listView) {
            switch (ke->key()) {
            case Qt::Key_Return:
            case Qt::Key_Enter:
                onItemActivated(m_listView->currentIndex());
                return true;
            default:
                m_lineEdit->setFocus();
                m_lineEdit->event(event);
                return true;
            }
        }
    }
    return QDialog::eventFilter(obj, event);
}
