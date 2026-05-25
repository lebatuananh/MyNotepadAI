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

#ifndef ACP_PLAN_WIDGET_H
#define ACP_PLAN_WIDGET_H

#include <QFrame>
#include <QList>

#include "AcpProtocol.h"

class QLabel;
class QPushButton;
class QVBoxLayout;

class AcpPlanWidget : public QFrame
{
    Q_OBJECT

public:
    explicit AcpPlanWidget(QWidget *parent = nullptr);

    void setEntries(const QList<AcpProtocol::AcpPlanEntry> &entries);
    void setAgentIdle(bool idle);

signals:
    void resumeRequested(const QString &prompt);

private:
    void clearRows();
    void updateBadge();
    void updateResumeButton();

    QVBoxLayout *m_layout = nullptr;
    QLabel *m_badge = nullptr;
    QPushButton *m_resumeBtn = nullptr;
    QList<AcpProtocol::AcpPlanEntry> m_entries;
    bool m_agentIdle = false;
};

#endif // ACP_PLAN_WIDGET_H
