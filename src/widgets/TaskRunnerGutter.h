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

#ifndef TASKRUNNERGUTTER_H
#define TASKRUNNERGUTTER_H

#include <QCache>
#include <QImage>
#include <QObject>
#include <QString>
#include <QVector>

class QTimer;
class ScintillaNext;
class TerminalManager;

namespace Scintilla { struct NotificationData; }

// Marker 20, Margin 3 — documented allocation for task runner gutter.
// See also: GitGutterMarkerIds (16-18), HideLines (21-23).

class TaskRunnerGutter : public QObject
{
    Q_OBJECT

public:
    explicit TaskRunnerGutter(ScintillaNext *editor, TerminalManager *termMgr);
    ~TaskRunnerGutter() override;

    // Re-define the marker icon (e.g. after theme change).
    void defineMarker();

    // Static helpers for file-type detection.
    static bool isTaskFile(const QString &filename);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void onNotify(Scintilla::NotificationData *pscn);
    void reparseAndUpdateMarkers();

private:
    struct TaskEntry {
        int line;           // 0-based Scintilla line
        QString name;       // task/recipe/target name
        QString command;    // full command to execute
        bool hasRequiredParams = false;
    };

    void onMarginClick(int margin, int line);
    void recalcMarginBounds();

    // Parsers
    QVector<TaskEntry> parsePackageJson(const QByteArray &text);
    QVector<TaskEntry> parseJustfile(const QByteArray &text);
    QVector<TaskEntry> parseMakefile(const QByteArray &text);
    QVector<TaskEntry> parseDenoJson(const QByteArray &text);

    // Package manager detection
    QString detectPackageManager(const QString &dir);

    // Icon generation
    static QImage generateRunIcon(int size, const QColor &color);

    ScintillaNext *m_editor;
    TerminalManager *m_termMgr;
    QTimer *m_debounce;
    QVector<TaskEntry> m_tasks;

    // Margin bounds cache (pixel x range for margin 3)
    int m_marginLeft = 0;
    int m_marginRight = 0;
    int m_lastTooltipLine = -1;

    // Package manager cache: dir -> manager command prefix
    static QCache<QString, QString> s_pmCache;
};

#endif // TASKRUNNERGUTTER_H
