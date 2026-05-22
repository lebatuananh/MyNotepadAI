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

#include "GitDiffViewController.h"

#include "EditorManager.h"
#include "GitController.h"
#include "GitDiffFetcher.h"
#include "GitDiffPainter.h"
#include "GitDiffPalette.h"
#include "ScintillaNext.h"
#include "LineNumbers.h"

GitDiffViewController::GitDiffViewController(GitController *controller,
                                             EditorManager *editorManager,
                                             QObject *parent)
    : QObject(parent), m_controller(controller), m_editorManager(editorManager)
{
    m_fetcher = new GitDiffFetcher(controller, this);
    connect(m_fetcher, &GitDiffFetcher::parsedReady, this, &GitDiffViewController::onParsedReady);
    connect(m_fetcher, &GitDiffFetcher::failed, this, &GitDiffViewController::onFetchFailed);
}

ScintillaNext *GitDiffViewController::previewEditor() const
{
    return m_previewEditor.data();
}

void GitDiffViewController::setDarkPalette(bool dark)
{
    if (m_isDark == dark) return;
    m_isDark = dark;
    if (m_previewEditor) {
        GitDiffPainter::configureEditor(m_previewEditor, GitDiffPalette::current(m_isDark));
    }
}

void GitDiffViewController::showDiffFor(const GitStatusEntry &entry)
{
    if (entry.hasUnstableEncoding) {
        emit diffFailed(entry.relPath,
                        tr("Diff unavailable: filename has unsupported encoding."));
        return;
    }
    m_currentRelPath = entry.relPath;
    m_currentStaged = entry.stagedSide;
    ensurePreviewEditor();
    updatePreviewTitle(entry.relPath, entry.stagedSide);
    m_fetcher->request(entry);
}

ScintillaNext *GitDiffViewController::ensurePreviewEditor()
{
    if (m_previewEditor) return m_previewEditor;
    ScintillaNext *e = m_editorManager->createEditor(tr("[Diff Preview]"));
    m_editorManager->registerAsDiffView(e);

    // The default LineNumbers decorator generates buffer-line numbers
    // (1, 2, 3, ...) which are meaningless for a unified diff. GitDiffPainter
    // populates margin 0 itself with the diff's own old/new file line numbers,
    // so the decorator must stay disabled on this editor.
    if (auto *ln = e->findChild<LineNumbers *>(QString(), Qt::FindDirectChildrenOnly)) {
        ln->setEnabled(false);
    }

    GitDiffPainter::configureEditor(e, GitDiffPalette::current(m_isDark));
    m_previewEditor = e;
    emit newPreviewEditorCreated(e);
    return e;
}

void GitDiffViewController::updatePreviewTitle(const QString &relPath, bool stagedSide)
{
    if (!m_previewEditor) return;
    const QString name = stagedSide
        ? tr("[Diff: %1 (staged)]").arg(relPath)
        : tr("[Diff: %1]").arg(relPath);
    m_previewEditor->setName(name);
}

void GitDiffViewController::onParsedReady(const QString &relPath, bool stagedSide,
                                          const std::shared_ptr<const GitDiffParser::Result> &parsed)
{
    if (relPath != m_currentRelPath || stagedSide != m_currentStaged) return; // stale
    if (!m_previewEditor) return;
    GitDiffPainter::render(m_previewEditor, *parsed);
    emit diffRendered(m_previewEditor);
}

void GitDiffViewController::onFetchFailed(const QString &relPath, bool stagedSide, const QString &message)
{
    if (relPath != m_currentRelPath || stagedSide != m_currentStaged) return;
    emit diffFailed(relPath, message);
}
