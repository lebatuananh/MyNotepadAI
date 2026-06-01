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
#include "../git/GitRunnerFactory.h"
#include "../NotepadNextApplication.h"
#include "../remote/SshProfile.h"

#include <QApplication>
#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QMouseEvent>
#include <QString>
#include <QTimer>
#include <QToolTip>

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

    editor->installEventFilter(this);

    if (auto *app = qobject_cast<NotepadNextApplication *>(qApp)) {
        connect(app, &NotepadNextApplication::effectiveThemeChanged,
                this, &InlineBlameDecorator::onThemeChanged);
        connect(app, &NotepadNextApplication::gitHeadChanged,
                this, &InlineBlameDecorator::onHeadChanged);
    }
}

InlineBlameDecorator::~InlineBlameDecorator()
{
    if (m_topRunner) {
        m_topRunner->cancelAsync();
    }
    if (m_blameFetcher) m_blameFetcher->disconnect(this);
}

void InlineBlameDecorator::setupEolAnnotationOnce()
{
    if (m_annotStyleReady || editor == nullptr) return;
    editor->eOLAnnotationSetVisible(1); // EOLANNOTATION_STANDARD
    editor->eOLAnnotationSetStyleOffset(kEolStyleOffset);
    editor->setMouseDwellTime(500);
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
    case Notification::DwellStart: {
        const QPoint local(static_cast<int>(pscn->x), static_cast<int>(pscn->y));
        if (isPointOnAnnotation(local)) {
            const QPoint global = editor->mapToGlobal(local);
            showBlameTooltip(global);
        }
        break;
    }
    case Notification::DwellEnd:
        QToolTip::hideText();
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

void InlineBlameDecorator::onHeadChanged()
{
    m_blame = {};
    m_blameValid = false;
    clearAnnotation();
    if (isEnabled() && editor && editor->isFile() && !editor->modify())
        startBlameFetch();
}

bool InlineBlameDecorator::isPointOnAnnotation(const QPoint &localPos) const
{
    if (m_annotatedLine < 0 || !m_blameValid || editor == nullptr) return false;

    const auto pos = editor->positionFromPointClose(localPos.x(), localPos.y());
    if (pos != -1) return false;

    const int clickLine = static_cast<int>(editor->lineFromPosition(
        editor->positionFromPoint(localPos.x(), localPos.y())));
    if (clickLine != m_annotatedLine) return false;

    const int lineEndPos = static_cast<int>(editor->lineEndPosition(clickLine));
    const int lineEndX = static_cast<int>(editor->pointXFromPosition(lineEndPos));
    return localPos.x() > lineEndX;
}

bool InlineBlameDecorator::eventFilter(QObject *obj, QEvent *event)
{
    Q_UNUSED(obj)
    if (!isEnabled() || editor == nullptr) return false;

    if (event->type() == QEvent::MouseButtonRelease) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton && isPointOnAnnotation(me->pos())) {
            handleAnnotationClick();
            return true;
        }
    }
    return false;
}

void InlineBlameDecorator::handleAnnotationClick()
{
    if (m_annotatedLine < 0 || !m_blameValid) return;

    const GitBlameParser::Line *match = nullptr;
    for (const auto &ln : m_blame.lines) {
        if (ln.lineIdx == m_annotatedLine) { match = &ln; break; }
    }
    if (!match) return;
    const auto &rec = m_blame.records.at(match->recordIdx);
    if (rec.sha.isEmpty()) return;

    emit commitClicked(rec.sha);
}

void InlineBlameDecorator::showBlameTooltip(const QPoint &globalPos)
{
    if (m_annotatedLine < 0 || !m_blameValid) return;

    const GitBlameParser::Line *match = nullptr;
    for (const auto &ln : m_blame.lines) {
        if (ln.lineIdx == m_annotatedLine) { match = &ln; break; }
    }
    if (!match) return;
    const auto &rec = m_blame.records.at(match->recordIdx);
    if (rec.sha.isEmpty()) return;

    const QString dateStr = QDateTime::fromSecsSinceEpoch(rec.authorTime)
                                .toString(QStringLiteral("yyyy-MM-dd HH:mm"));
    const QString shortSha = QString::fromLatin1(rec.sha.left(8));
    const QString authorDisplay = rec.author.isEmpty()
        ? QStringLiteral("?") : rec.author.toHtmlEscaped();
    const QString emailDisplay = rec.authorMail.toHtmlEscaped();
    const QString summaryDisplay = rec.summary.toHtmlEscaped();

    QString html = QStringLiteral(
        "<p style=\"margin:0;font-weight:bold;\">%1 %2</p>"
        "<p style=\"margin:4px 0 0 0;\">%3</p>"
        "<p style=\"margin:8px 0 0 0;color:#888;font-size:small;\">%4 · %5</p>")
        .arg(authorDisplay, emailDisplay, summaryDisplay, dateStr, shortSha);

    QToolTip::showText(globalPos, html, editor);
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
        m_runnerScope = currentFile;
        if (remote::isSshUri(currentFile)) {
            m_lastResolvedPosixPath = remote::parseSshUri(currentFile).remotePath;
        } else {
            m_lastResolvedPosixPath = currentFile;
        }
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
    ++m_topGeneration;

    if (m_topRunner) {
        m_topRunner->cancelAsync();
        m_topRunner->asQObject()->deleteLater();
        m_topRunner = nullptr;
    }

    const bool isRemote = GitRunnerFactory::isRemotePath(m_runnerScope);
    if (!isRemote && GitProcessRunner::gitExecutable().isEmpty()) {
        m_topLevelChecked = true;
        m_repoToplevel.clear();
        clearAnnotation();
        return;
    }

    const QString dir = isRemote
        ? QFileInfo(m_lastResolvedPosixPath).absolutePath()
        : QFileInfo(m_lastResolvedFile).absolutePath();
    if (dir.isEmpty() || (!isRemote && !QDir(dir).exists())) {
        m_topLevelChecked = true;
        m_repoToplevel.clear();
        clearAnnotation();
        return;
    }

    m_topRunner = GitRunnerFactory::createForRepo(m_runnerScope, this);
    const QStringList argv = {
        QStringLiteral("-C"), dir,
        QStringLiteral("rev-parse"), QStringLiteral("--show-toplevel")
    };
    const quint64 gen = m_topGeneration;
    m_topRunner->run(QString(), argv, QByteArray(), 5000, false,
                     [this, gen](int exit, const QByteArray &out, const QByteArray &) {
        if (gen != m_topGeneration) return;
        if (m_topRunner) {
            m_topRunner->asQObject()->deleteLater();
            m_topRunner = nullptr;
        }
        m_topLevelChecked = true;
        if (exit != 0) {
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
    });
}

void InlineBlameDecorator::startBlameFetch()
{
    if (m_repoToplevel.isEmpty() || m_lastResolvedPosixPath.isEmpty()) return;
    if (!m_blameFetcher) {
        m_blameFetcher = new GitBlameFetcher(this);
        connect(m_blameFetcher, &GitBlameFetcher::blameReady,
                this, &InlineBlameDecorator::onBlameReady);
        connect(m_blameFetcher, &GitBlameFetcher::blameFailed,
                this, &InlineBlameDecorator::onBlameFailed);
    }
    const QString rel = QDir(m_repoToplevel).relativeFilePath(m_lastResolvedPosixPath);
    m_blameFetcher->fetch(m_runnerScope, m_repoToplevel, rel);
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

    QString label = QStringLiteral("    %1, %2 • %3  ↗ %4")
        .arg(rec.author.isEmpty() ? QStringLiteral("?") : rec.author,
             humanizeRelative(rec.authorTime),
             truncate(rec.summary, kMaxSummaryChars),
             QString::fromLatin1(rec.sha.left(8)));

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
