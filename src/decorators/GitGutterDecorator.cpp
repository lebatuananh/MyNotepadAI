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

#include "GitGutterDecorator.h"

#include "../git/BufferDiffEngine.h"
#include "../git/CatFileBlobFetcher.h"
#include "../git/GitBaseBlobCache.h"
#include "../git/GitDiffPalette.h"
#include "../git/GitProcessRunner.h"
#include "../NotepadNextApplication.h"
#include "BookMarkDecorator.h"
#include "GitGutterMarkers.h"

#include <QApplication>
#include <QColor>
#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QTimer>

namespace {

constexpr int kMarkAdded    = GitGutterMarkerIds::Added;
constexpr int kMarkModified = GitGutterMarkerIds::Modified;
constexpr int kMarkDeleted  = GitGutterMarkerIds::Deleted;

constexpr int kMargin = 1;
constexpr int kDebounceMs = 120;

constexpr int kAnnotStyleOffset    = 512;
constexpr int kAnnotStyleDelRelIdx = 0;
constexpr int kAnnotStyleDelAbs    = kAnnotStyleOffset + kAnnotStyleDelRelIdx;

inline int sciColor(const QColor &c)
{
    return (c.blue() << 16) | (c.green() << 8) | c.red();
}

QVector<QByteArray> sliceLines(const QByteArray &blob, qint32 startLine, qint32 count)
{
    QVector<QByteArray> out;
    if (count <= 0 || blob.isEmpty()) return out;
    out.reserve(count);

    qint32 line = 0;
    qsizetype p = 0;
    const qsizetype n = blob.size();
    while (p < n && line < startLine) {
        if (blob.at(p) == '\n') ++line;
        ++p;
    }
    while (p < n && count > 0) {
        const qsizetype lineStart = p;
        while (p < n && blob.at(p) != '\n') ++p;
        out.append(blob.mid(lineStart, p - lineStart));
        if (p < n) ++p;
        --count;
    }
    return out;
}

} // namespace

GitGutterDecorator::GitGutterDecorator(ScintillaNext *editor)
    : EditorDecorator(editor),
      m_debounce(new QTimer(this))
{
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(kDebounceMs);
    connect(m_debounce, &QTimer::timeout, this, &GitGutterDecorator::onRediffDebounced);

    setupMargin();

    if (auto *app = qobject_cast<NotepadNextApplication *>(qApp)) {
        connect(app, &NotepadNextApplication::effectiveThemeChanged,
                this, &GitGutterDecorator::onThemeChanged);
    }
}

GitGutterDecorator::~GitGutterDecorator()
{
    if (m_topProc) {
        m_topProc->disconnect(this);
        if (m_topProc->state() != QProcess::NotRunning) m_topProc->kill();
    }
    if (m_blobFetcher) {
        m_blobFetcher->disconnect(this);
    }
}

void GitGutterDecorator::setupMargin()
{
    editor->markerDefine(kMarkAdded,    SC_MARK_LEFTRECT);
    editor->markerDefine(kMarkModified, SC_MARK_LEFTRECT);
    editor->markerDefine(kMarkDeleted,  SC_MARK_LEFTRECT);

    const int mask = editor->marginMaskN(kMargin);
    editor->setMarginMaskN(kMargin,
                           mask
                           | (1 << kMarkAdded)
                           | (1 << kMarkModified)
                           | (1 << kMarkDeleted));

    applyPalette();
}

void GitGutterDecorator::applyPalette()
{
    bool isDark = false;
    if (auto *app = qobject_cast<NotepadNextApplication *>(qApp)) {
        isDark = app->isEffectiveThemeDark();
    }
    const GitDiffPalette &p = GitDiffPalette::current(isDark);

    const int addedColor    = sciColor(p.fgAdded);
    const int modifiedColor = sciColor(p.fgModified);
    const int deletedColor  = sciColor(p.fgDeleted);

    editor->markerSetFore(kMarkAdded,    addedColor);
    editor->markerSetBack(kMarkAdded,    addedColor);
    editor->markerSetFore(kMarkModified, modifiedColor);
    editor->markerSetBack(kMarkModified, modifiedColor);
    editor->markerSetFore(kMarkDeleted,  deletedColor);
    editor->markerSetBack(kMarkDeleted,  deletedColor);
}

void GitGutterDecorator::clearInlineHunk()
{
    if (m_annotatedHunkStart < 0 || editor == nullptr) return;
    editor->annotationClearAll();
    m_annotatedHunkStart = -1;
}

void GitGutterDecorator::applyAnnotationPalette()
{
    if (editor == nullptr) return;
    bool isDark = false;
    if (auto *app = qobject_cast<NotepadNextApplication *>(qApp)) {
        isDark = app->isEffectiveThemeDark();
    }
    const GitDiffPalette &p = GitDiffPalette::current(isDark);

    editor->styleSetFore(kAnnotStyleDelAbs, sciColor(p.fgDeleted));
    editor->styleSetBack(kAnnotStyleDelAbs, sciColor(p.bgDelLine));
}

void GitGutterDecorator::setupAnnotationStylesIfNeeded()
{
    if (m_annotStylesReady || editor == nullptr) return;
    editor->annotationSetVisible(1); // ANNOTATION_STANDARD
    editor->annotationSetStyleOffset(kAnnotStyleOffset);
    applyAnnotationPalette();
    m_annotStylesReady = true;
}

void GitGutterDecorator::clearMarkers()
{
    editor->markerDeleteAll(kMarkAdded);
    editor->markerDeleteAll(kMarkModified);
    editor->markerDeleteAll(kMarkDeleted);
    clearInlineHunk();
}

void GitGutterDecorator::notify(const Scintilla::NotificationData *pscn)
{
    if (!isEnabled() || pscn == nullptr) return;

    using Scintilla::Notification;
    switch (pscn->nmhdr.code) {
    case Notification::SavePointReached:
        if (!m_repoToplevel.isEmpty() && !m_lastResolvedFile.isEmpty()) {
            const QString rel = QDir(m_repoToplevel).relativeFilePath(m_lastResolvedFile);
            GitBaseBlobCache::instance().invalidate(m_repoToplevel, rel);
            m_baseBlobReady = false;
            m_baseBlob.clear();
        }
        scheduleRediff();
        break;
    case Notification::Modified: {
        const auto mt = pscn->modificationType;
        using MT = Scintilla::ModificationFlags;
        if (FlagSet(mt, MT::InsertText) || FlagSet(mt, MT::DeleteText)) {
            scheduleRediff();
        }
        break;
    }
    case Notification::MarginClick:
        if (pscn->margin == kMargin) {
            handleMarginClick(static_cast<int>(pscn->position));
        }
        break;
    default:
        break;
    }
}

void GitGutterDecorator::handleMarginClick(int positionInDoc)
{
    if (m_lastLines.lineToHunkIdx.isEmpty()) return;

    const int line = editor->lineFromPosition(positionInDoc);
    const auto it = m_lastLines.lineToHunkIdx.constFind(line);
    if (it == m_lastLines.lineToHunkIdx.constEnd()) return;

    showHunkInline(it.value(), line);

    QPointer<ScintillaNext> ed(editor);
    QTimer::singleShot(0, this, [ed, line]() {
        if (!ed) return;
        if (auto *bm = ed->findChild<BookMarkDecorator *>(QString(), Qt::FindDirectChildrenOnly)) {
            if (bm->isBookmarkSet(line)) bm->removeBookmark(line);
        }
    });
}

void GitGutterDecorator::showHunkInline(int hunkIdx, int clickedLine)
{
    Q_UNUSED(clickedLine);
    if (hunkIdx < 0 || hunkIdx >= m_hunks.size() || editor == nullptr) return;

    const auto &h = m_hunks.at(hunkIdx);

    if (h.oldCount == 0) return;
    if (m_baseBlob.isEmpty()) return;

    // Annotation placement: line BEFORE the hunk's first new-side line.
    // This puts the deleted content directly above the changed/new lines,
    // matching Zed's inline diff placement where the old content appears
    // above the current content.
    const qint32 annotLine = h.newStart > 0 ? h.newStart - 1 : 0;

    if (m_annotatedHunkStart == h.newStart) {
        clearInlineHunk();
        return;
    }

    const QVector<QByteArray> deleted = sliceLines(m_baseBlob, h.oldStart, h.oldCount);
    if (deleted.isEmpty()) return;

    QByteArray text;
    QByteArray styles;
    text.reserve(256);
    styles.reserve(256);

    for (int i = 0; i < deleted.size(); ++i) {
        if (i > 0) {
            text.append('\n');
            styles.append(static_cast<char>(kAnnotStyleDelRelIdx));
        }
        const QByteArray &ln = deleted.at(i);
        text.append(ln);
        styles.append(QByteArray(ln.size(), static_cast<char>(kAnnotStyleDelRelIdx)));
    }

    setupAnnotationStylesIfNeeded();
    clearInlineHunk();

    editor->annotationSetText(annotLine, text.constData());
    editor->annotationSetStyles(annotLine, styles.constData());
    m_annotatedHunkStart = h.newStart;
}

void GitGutterDecorator::scheduleRediff()
{
    m_debounce->start();
}

void GitGutterDecorator::onRediffDebounced()
{
    refresh();
}

void GitGutterDecorator::onThemeChanged()
{
    applyPalette();
    if (m_annotStylesReady) applyAnnotationPalette();
}

void GitGutterDecorator::refresh()
{
    if (!isEnabled() || editor == nullptr) {
        clearMarkers();
        return;
    }
    if (!editor->isFile()) {
        clearMarkers();
        return;
    }

    const QString currentFile = editor->getFilePath();
    if (currentFile.isEmpty()) {
        clearMarkers();
        return;
    }

    if (currentFile != m_lastResolvedFile) {
        m_lastResolvedFile = currentFile;
        m_topLevelChecked = false;
        m_repoToplevel.clear();
        m_baseBlob.clear();
        m_baseBlobReady = false;
    }

    if (!m_topLevelChecked) {
        startTopLevelDiscovery();
        return;
    }
    if (m_repoToplevel.isEmpty()) {
        clearMarkers();
        return;
    }
    ensureBaseBlobAndDiff();
}

void GitGutterDecorator::startTopLevelDiscovery()
{
    if (GitProcessRunner::gitExecutable().isEmpty()) {
        m_topLevelChecked = true;
        m_repoToplevel.clear();
        clearMarkers();
        return;
    }

    if (m_topProc) {
        m_topProc->disconnect(this);
        if (m_topProc->state() != QProcess::NotRunning) m_topProc->kill();
        m_topProc->deleteLater();
        m_topProc = nullptr;
    }

    const QFileInfo fi(m_lastResolvedFile);
    const QString dir = fi.absolutePath();
    if (dir.isEmpty() || !QDir(dir).exists()) {
        m_topLevelChecked = true;
        m_repoToplevel.clear();
        return;
    }

    m_topProc = new QProcess(this);
    m_topProc->setProgram(GitProcessRunner::gitExecutable());
    m_topProc->setArguments({ QStringLiteral("rev-parse"), QStringLiteral("--show-toplevel") });
    m_topProc->setWorkingDirectory(dir);
    m_topProc->setProcessEnvironment(GitProcessRunner::baseEnv());
    connect(m_topProc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &GitGutterDecorator::onTopLevelFinished);
    connect(m_topProc, &QProcess::errorOccurred, this, [this]() {
        m_topLevelChecked = true;
        m_repoToplevel.clear();
        clearMarkers();
        if (m_topProc) { m_topProc->deleteLater(); m_topProc = nullptr; }
    });
    m_topProc->start();
}

void GitGutterDecorator::onTopLevelFinished()
{
    if (!m_topProc) return;

    const int exitCode = m_topProc->exitCode();
    const QByteArray out = m_topProc->readAllStandardOutput();
    m_topProc->deleteLater();
    m_topProc = nullptr;

    m_topLevelChecked = true;
    if (exitCode != 0) {
        m_repoToplevel.clear();
        clearMarkers();
        return;
    }

    QString top = QString::fromUtf8(out).trimmed();
    if (top.isEmpty()) {
        m_repoToplevel.clear();
        clearMarkers();
        return;
    }
    m_repoToplevel = QDir::cleanPath(top);
    ensureBaseBlobAndDiff();
}

void GitGutterDecorator::ensureBaseBlobAndDiff()
{
    if (m_repoToplevel.isEmpty() || m_lastResolvedFile.isEmpty()) {
        clearMarkers();
        return;
    }

    const QString rel = QDir(m_repoToplevel).relativeFilePath(m_lastResolvedFile);

    if (m_baseBlobReady) {
        runDiff();
        return;
    }
    const QByteArray cached = GitBaseBlobCache::instance().get(m_repoToplevel, rel);
    if (!cached.isNull()) {
        m_baseBlob = cached;
        m_baseBlobReady = true;
        runDiff();
        return;
    }

    if (!m_blobFetcher) {
        m_blobFetcher = new CatFileBlobFetcher(this);
        connect(m_blobFetcher, &CatFileBlobFetcher::blobReady,
                this, &GitGutterDecorator::onBlobReady);
        connect(m_blobFetcher, &CatFileBlobFetcher::blobFailed,
                this, &GitGutterDecorator::onBlobFailed);
    }
    m_blobFetcher->fetch(m_repoToplevel, rel);
}

void GitGutterDecorator::onBlobReady(const QString &repoTop, const QString &relPath,
                                     const QByteArray &blob)
{
    Q_UNUSED(repoTop);
    Q_UNUSED(relPath);
    m_baseBlob = blob;
    m_baseBlobReady = true;
    runDiff();
}

void GitGutterDecorator::onBlobFailed(const QString &repoTop, const QString &relPath,
                                      const QString &message)
{
    Q_UNUSED(repoTop);
    Q_UNUSED(relPath);
    Q_UNUSED(message);
    m_baseBlob.clear();
    m_baseBlobReady = true;
    clearMarkers();
}

void GitGutterDecorator::runDiff()
{
    if (!isEnabled() || editor == nullptr) return;

    const QByteArray buf = editor->getText(editor->textLength() + 1);

    m_hunks = BufferDiffEngine::diff(m_baseBlob, buf);
    m_lastLines = GitGutterMarkers::linesFromHunks(m_hunks);

    clearMarkers();
    for (qint32 ln : m_lastLines.added)     editor->markerAdd(ln, kMarkAdded);
    for (qint32 ln : m_lastLines.modified)  editor->markerAdd(ln, kMarkModified);
    for (qint32 ln : m_lastLines.deletedAt) editor->markerAdd(ln, kMarkDeleted);
}
