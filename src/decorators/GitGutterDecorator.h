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
#include "GitGutterMarkers.h"

#include "../git/BufferDiffEngine.h"

#include <QByteArray>
#include <QPointer>
#include <QString>

#include "../git/GitProcessRunner.h"

class CatFileBlobFetcher;
class QTimer;

class GitGutterDecorator : public EditorDecorator
{
    Q_OBJECT

public:
    explicit GitGutterDecorator(ScintillaNext *editor);
    ~GitGutterDecorator() override;

    void refresh();
    void invalidateAndRefresh();

public slots:
    void notify(const Scintilla::NotificationData *pscn) override;

private slots:
    void onBlobReady(const QString &repoTop, const QString &relPath, const QByteArray &blob);
    void onBlobFailed(const QString &repoTop, const QString &relPath, const QString &message);
    void onRediffDebounced();
    void onThemeChanged();

private:
    void setupMargin();
    void applyPalette();
    void clearMarkers();
    void startTopLevelDiscovery();
    void ensureBaseBlobAndDiff();
    void runDiff();
    void scheduleRediff();

    QTimer *m_debounce = nullptr;
    IGitProcessRunner *m_topRunner = nullptr;
    quint64 m_topGeneration = 0;
    CatFileBlobFetcher *m_blobFetcher = nullptr;

    BufferDiffEngine::Hunks m_hunks;
    GitGutterMarkers::Lines m_lastLines;
    QByteArray m_baseBlob;
    bool m_baseBlobReady = false;

    qint32 m_annotatedHunkStart = -1;
    bool m_annotStylesReady = false;

    QString m_lastResolvedFile;
    QString m_runnerScope;
    QString m_lastResolvedPosixPath;
    QString m_repoToplevel;
    QString m_cacheRepoKey;
    bool m_topLevelChecked = false;

    void handleMarginClick(int positionInDoc);
    void showHunkInline(int hunkIdx, int clickedLine);
    void clearInlineHunk();
    void setupAnnotationStylesIfNeeded();
    void applyAnnotationPalette();
};
