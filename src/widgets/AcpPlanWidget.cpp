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

#include "AcpPlanWidget.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

QString glyphFor(const QString &status)
{
    if (status == QLatin1String("completed"))   return QStringLiteral("✓");
    if (status == QLatin1String("in_progress")) return QStringLiteral("▶");
    return QStringLiteral("•");
}

} // namespace

AcpPlanWidget::AcpPlanWidget(QWidget *parent)
    : QFrame(parent)
{
    setFrameShape(QFrame::StyledPanel);
    setStyleSheet(QStringLiteral("AcpPlanWidget { background: palette(alternate-base); border-radius: 4px; }"));
    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(8, 6, 8, 6);
    m_layout->setSpacing(2);

    // Header row: "Tasks" label + badge + resume button
    auto *headerRow = new QWidget(this);
    auto *headerLayout = new QHBoxLayout(headerRow);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(6);

    auto *header = new QLabel(tr("Tasks"), headerRow);
    header->setStyleSheet(QStringLiteral("QLabel { font-weight: bold; }"));
    headerLayout->addWidget(header);

    m_badge = new QLabel(headerRow);
    m_badge->setStyleSheet(QStringLiteral("QLabel { color: palette(mid); font-size: 11px; }"));
    headerLayout->addWidget(m_badge);

    headerLayout->addStretch();

    m_resumeBtn = new QPushButton(tr("Resume"), headerRow);
    m_resumeBtn->setFlat(true);
    m_resumeBtn->setStyleSheet(QStringLiteral(
        "QPushButton { color: palette(link); font-size: 11px; padding: 0 4px; }"
        "QPushButton:hover { text-decoration: underline; }"));
    m_resumeBtn->setVisible(false);
    connect(m_resumeBtn, &QPushButton::clicked, this, [this]() {
        QStringList lines;
        for (const auto &e : m_entries) {
            if (e.status != QLatin1String("completed"))
                lines.append(QStringLiteral("- ") + e.text);
        }
        if (!lines.isEmpty()) {
            emit resumeRequested(
                tr("Please continue completing the following tasks:\n") + lines.join(QLatin1Char('\n')));
        }
    });
    headerLayout->addWidget(m_resumeBtn);

    m_layout->addWidget(headerRow);
}

void AcpPlanWidget::clearRows()
{
    while (m_layout->count() > 1) {
        QLayoutItem *li = m_layout->takeAt(1);
        if (li->widget()) li->widget()->deleteLater();
        delete li;
    }
}

void AcpPlanWidget::setEntries(const QList<AcpProtocol::AcpPlanEntry> &entries)
{
    m_entries = entries;
    clearRows();
    for (const auto &entry : entries) {
        auto *row = new QWidget(this);
        auto *rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        rl->setSpacing(6);

        auto *icon = new QLabel(glyphFor(entry.status), row);
        icon->setFixedWidth(14);
        auto *text = new QLabel(entry.text, row);
        text->setWordWrap(true);
        text->setTextInteractionFlags(Qt::TextSelectableByMouse);
        if (entry.status == QLatin1String("completed")) {
            QFont f = text->font();
            f.setStrikeOut(true);
            text->setFont(f);
            text->setStyleSheet(QStringLiteral("color: palette(mid);"));
        }
        rl->addWidget(icon);
        rl->addWidget(text, 1);
        m_layout->addWidget(row);
    }
    updateBadge();
    updateResumeButton();
}

void AcpPlanWidget::setAgentIdle(bool idle)
{
    m_agentIdle = idle;
    updateResumeButton();
}

void AcpPlanWidget::updateBadge()
{
    if (m_entries.isEmpty()) {
        m_badge->clear();
        return;
    }
    int completed = 0;
    for (const auto &e : m_entries) {
        if (e.status == QLatin1String("completed"))
            ++completed;
    }
    m_badge->setText(QStringLiteral("%1/%2").arg(completed).arg(m_entries.size()));
}

void AcpPlanWidget::updateResumeButton()
{
    if (!m_agentIdle || m_entries.isEmpty()) {
        m_resumeBtn->setVisible(false);
        return;
    }
    bool hasIncomplete = false;
    for (const auto &e : m_entries) {
        if (e.status != QLatin1String("completed")) {
            hasIncomplete = true;
            break;
        }
    }
    m_resumeBtn->setVisible(hasIncomplete);
}
