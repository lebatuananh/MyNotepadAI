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

#pragma once

#include "EditorDecorator.h"

#include "../git/GitBlameParser.h"

#include <QString>

class GitBlameFetcher;
class QProcess;
class QTimer;

// Renders an inline "<Author>, <when> • <summary>" annotation at the END of
// the caret's current line — same affordance as JetBrains IDEs / VS Code
// Git Lens. Uses Scintilla EOL annotations so it appears AFTER the code
// without nudging columns or eating an editor row.
//
// Lifecycle:
//   * File path resolved → discover repo top via `git rev-parse` (reuses the
//     same async pattern as GitGutterDecorator).
//   * Blame is fetched lazily by GitBlameFetcher and cached for the file.
//   * Caret move → debounce 300 ms → repaint the annotation on the new line.
//   * Buffer modified → clear annotation (blame is HEAD-tied; live buffer
//     state would render the wrong author for shifted lines). Re-fetch on
//     SavePointReached.
//
// One decorator per ScintillaNext. Spawned by EditorManager::setupEditor when
// the InlineBlameEnabled setting is on.
class InlineBlameDecorator : public EditorDecorator
{
    Q_OBJECT

public:
    explicit InlineBlameDecorator(ScintillaNext *editor);
    ~InlineBlameDecorator() override;

    // Force a re-fetch ignoring the debounce. Used by EditorManager when the
    // settings toggle flips back on.
    void refresh();

public slots:
    void notify(const Scintilla::NotificationData *pscn) override;

private slots:
    void onTopLevelFinished();
    void onBlameReady(const QString &repoTop, const QString &relPath,
                      const GitBlameParser::Result &result);
    void onBlameFailed(const QString &repoTop, const QString &relPath,
                       const QString &message);
    void onCaretDebounced();
    void onThemeChanged();

private:
    void setupEolAnnotationOnce();
    void applyEolPalette();
    void clearAnnotation();
    void renderAtCaretLine();
    void scheduleCaretRepaint();
    void startTopLevelDiscovery();
    void startBlameFetch();

    QTimer *m_caretDebounce = nullptr;
    QProcess *m_topProc = nullptr;
    GitBlameFetcher *m_blameFetcher = nullptr;

    // Cached blame for the current file. Cleared on file change or buffer
    // modify; rebuilt on SavePointReached / refresh().
    GitBlameParser::Result m_blame;
    bool m_blameValid = false;

    qint32 m_annotatedLine = -1;
    bool   m_annotStyleReady = false;

    QString m_lastResolvedFile;
    QString m_repoToplevel;
    bool    m_topLevelChecked = false;
};
