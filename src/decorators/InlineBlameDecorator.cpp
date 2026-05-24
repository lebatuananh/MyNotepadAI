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

#include "InlineBlameDecorator.h"

#include "../git/GitBlameFetcher.h"
#include "../git/GitDiffPalette.h"
#include "../git/GitProcessRunner.h"
#include "../NotepadNextApplication.h"

#include <QApplication>
#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QString>
#include <QTimer>

namespace {

// Caret-move debounce. 300 ms strikes a balance: the user can scrub through
// lines with arrow keys without the annotation flashing on every step, but
// holding still feels instant.
constexpr int kCaretDebounceMs = 300;

// EOL annotation style lives well above any lexer style index, same scheme
// the gutter inline-diff uses. Single style — blame uses a uniform colour
// across the line.
constexpr int kEolStyleOffset = 768;
constexpr int kEolStyleRelIdx = 0;
constexpr int kEolStyleAbs    = kEolStyleOffset + kEolStyleRelIdx;

constexpr int kMaxSummaryChars = 50;

inline int sciColor(const QColor &c)
{
    return (c.blue() << 16) | (c.green() << 8) | c.red();
}

QString humanizeRelative(qint64 ts)
{
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const qint64 d = now - ts;
    if (d < 60)       return QObject::tr("just now");
    if (d < 3600)     return QObject::tr("%1 minutes ago").arg(d / 60);
    if (d < 86400)    return QObject::tr("%1 hours ago").arg(d / 3600);
    if (d < 7*86400)  return QObject::tr("%1 days ago").arg(d / 86400);
    if (d < 30*86400) return QObject::tr("%1 weeks ago").arg(d / (7 * 86400));
    if (d < 365*86400) return QObject::tr("%1 months ago").arg(d / (30 * 86400));
    return QObject::tr("%1 years ago").arg(d / (365 * 86400));
}

QString truncate(const QString &s, int max)
{
    if (s.size() <= max) return s;
    return s.left(max - 1) + QChar(0x2026); // ellipsis
}

// Pick a faded foreground that reads as "metadata, not code". Roughly half-
// blended toward background so blame doesn't compete with the cursor line.
QColor mutedForeground(bool isDark)
{
    return isDark ? QColor(150, 150, 150) : QColor(120, 120, 120);
}

} // namespace

InlineBlameDecorator::InlineBlameDecorator(ScintillaNext *editor)
    : EditorDecorator(editor),
      m_caretDebounce(new QTimer(this))
{
    m_caretDebounce->setSingleShot(true);
    m_caretDebounce->setInterval(kCaretDebounceMs);
    connect(m_caretDebounce, &QTimer::timeout,
            this, &InlineBlameDecorator::onCaretDebounced);

    if (auto *app = qobject_cast<NotepadNextApplication *>(qApp)) {
        connect(app, &NotepadNextApplication::effectiveThemeChanged,
                this, &InlineBlameDecorator::onThemeChanged);
    }
}

InlineBlameDecorator::~InlineBlameDecorator()
{
    if (m_topProc) {
        m_topProc->disconnect(this);
        if (m_topProc->state() != QProcess::NotRunning) m_topProc->kill();
    }
    if (m_blameFetcher) m_blameFetcher->disconnect(this);
}

void InlineBlameDecorator::setupEolAnnotationOnce()
{
    if (m_annotStyleReady || editor == nullptr) return;
    editor->eOLAnnotationSetVisible(1); // EOLANNOTATION_STANDARD
    editor->eOLAnnotationSetStyleOffset(kEolStyleOffset);
    applyEolPalette();
    m_annotStyleReady = true;
}

void InlineBlameDecorator::applyEolPalette()
{
    if (editor == nullptr) return;
    bool isDark = false;
    if (auto *app = qobject_cast<NotepadNextApplication *>(qApp)) {
        isDark = app->isEffectiveThemeDark();
    }
    editor->styleSetFore(kEolStyleAbs, sciColor(mutedForeground(isDark)));
    editor->styleSetItalic(kEolStyleAbs, true);
}

void InlineBlameDecorator::clearAnnotation()
{
    if (m_annotatedLine < 0 || editor == nullptr) return;
    editor->eOLAnnotationSetText(m_annotatedLine, "");
    m_annotatedLine = -1;
}

void InlineBlameDecorator::notify(const Scintilla::NotificationData *pscn)
{
    if (!isEnabled() || pscn == nullptr) return;

    using Scintilla::Notification;
    switch (pscn->nmhdr.code) {
    case Notification::UpdateUI:
        // Selection-only update means the caret may have moved. Repaint the
        // annotation; debounced to skip rapid keystroke-driven moves.
        if (FlagSet(pscn->updated, Scintilla::Update::Selection)) {
            scheduleCaretRepaint();
        }
        break;
    case Notification::Modified: {
        const auto mt = pscn->modificationType;
        using MT = Scintilla::ModificationFlags;
        if (FlagSet(mt, MT::InsertText) || FlagSet(mt, MT::DeleteText)) {
            // Buffer dirtied: blame is now tied to a stale tree. Hide the
            // annotation; it reappears on save.
            m_blameValid = false;
            clearAnnotation();
        }
        break;
    }
    case Notification::SavePointReached:
        // Disk now matches buffer (saved). Blame can be re-fetched.
        startBlameFetch();
        break;
    default:
        break;
    }
}

void InlineBlameDecorator::scheduleCaretRepaint()
{
    m_caretDebounce->start();
}

void InlineBlameDecorator::onCaretDebounced()
{
    renderAtCaretLine();
}

void InlineBlameDecorator::onThemeChanged()
{
    if (m_annotStyleReady) applyEolPalette();
}

void InlineBlameDecorator::refresh()
{
    if (!isEnabled() || editor == nullptr) {
        clearAnnotation();
        return;
    }
    if (!editor->isFile()) {
        clearAnnotation();
        return;
    }

    const QString currentFile = editor->getFilePath();
    if (currentFile.isEmpty()) {
        clearAnnotation();
        return;
    }

    if (currentFile != m_lastResolvedFile) {
        m_lastResolvedFile = currentFile;
        m_topLevelChecked = false;
        m_repoToplevel.clear();
        m_blame = {};
        m_blameValid = false;
        clearAnnotation();
    }

    if (!m_topLevelChecked) {
        startTopLevelDiscovery();
        return;
    }
    if (m_repoToplevel.isEmpty()) {
        clearAnnotation();
        return;
    }
    startBlameFetch();
}

void InlineBlameDecorator::startTopLevelDiscovery()
{
    if (GitProcessRunner::gitExecutable().isEmpty()) {
        m_topLevelChecked = true;
        m_repoToplevel.clear();
        clearAnnotation();
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
            this, &InlineBlameDecorator::onTopLevelFinished);
    connect(m_topProc, &QProcess::errorOccurred, this, [this]() {
        m_topLevelChecked = true;
        m_repoToplevel.clear();
        clearAnnotation();
        if (m_topProc) { m_topProc->deleteLater(); m_topProc = nullptr; }
    });
    m_topProc->start();
}

void InlineBlameDecorator::onTopLevelFinished()
{
    if (!m_topProc) return;

    const int exitCode = m_topProc->exitCode();
    const QByteArray out = m_topProc->readAllStandardOutput();
    m_topProc->deleteLater();
    m_topProc = nullptr;

    m_topLevelChecked = true;
    if (exitCode != 0) {
        m_repoToplevel.clear();
        clearAnnotation();
        return;
    }

    const QString top = QString::fromUtf8(out).trimmed();
    if (top.isEmpty()) {
        m_repoToplevel.clear();
        clearAnnotation();
        return;
    }
    m_repoToplevel = QDir::cleanPath(top);
    startBlameFetch();
}

void InlineBlameDecorator::startBlameFetch()
{
    if (m_repoToplevel.isEmpty() || m_lastResolvedFile.isEmpty()) return;
    if (!m_blameFetcher) {
        m_blameFetcher = new GitBlameFetcher(this);
        connect(m_blameFetcher, &GitBlameFetcher::blameReady,
                this, &InlineBlameDecorator::onBlameReady);
        connect(m_blameFetcher, &GitBlameFetcher::blameFailed,
                this, &InlineBlameDecorator::onBlameFailed);
    }
    const QString rel = QDir(m_repoToplevel).relativeFilePath(m_lastResolvedFile);
    m_blameFetcher->fetch(m_repoToplevel, rel);
}

void InlineBlameDecorator::onBlameReady(const QString &repoTop, const QString &relPath,
                                        const GitBlameParser::Result &result)
{
    Q_UNUSED(repoTop);
    Q_UNUSED(relPath);
    m_blame = result;
    m_blameValid = true;
    renderAtCaretLine();
}

void InlineBlameDecorator::onBlameFailed(const QString &repoTop, const QString &relPath,
                                         const QString &message)
{
    Q_UNUSED(repoTop);
    Q_UNUSED(relPath);
    Q_UNUSED(message);
    m_blame = {};
    m_blameValid = false;
    clearAnnotation();
}

void InlineBlameDecorator::renderAtCaretLine()
{
    if (!isEnabled() || editor == nullptr) return;
    if (!m_blameValid) {
        clearAnnotation();
        return;
    }
    // Hide blame while the buffer is dirty — blame line numbers no longer
    // match the editor view, so showing them would be misinformation.
    if (editor->modify()) {
        clearAnnotation();
        return;
    }

    const int pos = editor->currentPos();
    const int caretLine = editor->lineFromPosition(pos);

    // Linear scan of the blame.lines vector is O(n). For huge files this is
    // wasteful — but the typical case is "find caretLine entry once per
    // caret move" which is one pass per debounce tick. If profiling shows
    // it as a hot spot, swap to a QHash<lineIdx, recordIdx>.
    const GitBlameParser::Line *match = nullptr;
    for (const auto &ln : m_blame.lines) {
        if (ln.lineIdx == caretLine) { match = &ln; break; }
    }
    if (!match) {
        clearAnnotation();
        return;
    }
    const auto &rec = m_blame.records.at(match->recordIdx);

    QString label = QStringLiteral("    %1, %2 • %3")
        .arg(rec.author.isEmpty() ? QStringLiteral("?") : rec.author,
             humanizeRelative(rec.authorTime),
             truncate(rec.summary, kMaxSummaryChars));

    setupEolAnnotationOnce();

    // Different line than last → clear the old EOL annotation first so we
    // don't leave a stale label trailing a previous caret position.
    if (m_annotatedLine >= 0 && m_annotatedLine != caretLine) {
        editor->eOLAnnotationSetText(m_annotatedLine, "");
    }
    const QByteArray text = label.toUtf8();
    editor->eOLAnnotationSetText(caretLine, text.constData());
    editor->eOLAnnotationSetStyle(caretLine, kEolStyleRelIdx);
    m_annotatedLine = caretLine;
}
