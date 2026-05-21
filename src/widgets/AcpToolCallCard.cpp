/*
 * This file is part of Notepad Next.
 * Copyright 2026 NotepadADE contributors
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

#include "AcpToolCallCard.h"

#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QResizeEvent>
#include <QTextBrowser>
#include <QTextDocument>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

AcpToolCallCard::AcpToolCallCard(const AcpProtocol::AcpToolCall &initial, QWidget *parent)
    : QFrame(parent)
    , m_id(initial.id)
    , m_title(initial.title)
    , m_status(initial.status)
    , m_groupId(initial.groupId)
    , m_content(initial.content)
{
    setFrameShape(QFrame::StyledPanel);
    setStyleSheet(QStringLiteral("AcpToolCallCard { background: palette(base); border: 1px solid palette(mid); border-radius: 4px; }"));

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(6, 4, 6, 4);
    outer->setSpacing(2);
    m_outer = outer;

    auto *header = new QHBoxLayout();
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(6);

    m_statusIcon = new QLabel(this);
    m_titleLabel = new QLabel(this);
    m_titleLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_expandBtn = new QToolButton(this);
    m_expandBtn->setText(QStringLiteral("▸"));
    m_expandBtn->setCheckable(true);
    m_expandBtn->setChecked(false);
    m_expandBtn->setAutoRaise(true);

    header->addWidget(m_statusIcon);
    header->addWidget(m_titleLabel, 1);
    header->addWidget(m_expandBtn);

    outer->addLayout(header);

    m_body = new QTextBrowser(this);
    m_body->setStyleSheet(QStringLiteral("QTextBrowser { background: transparent; border: none; }"));
    m_body->setOpenExternalLinks(true);
    // Cards size to content. Internal scroll on a card produces a nested
    // scrollbar inside the transcript's own scroll area — bad UX. The expand
    // button collapses verbose output instead.
    m_body->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_body->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_body->setFrameShape(QFrame::NoFrame);
    m_body->document()->setDocumentMargin(0);
    outer->addWidget(m_body);

    connect(m_expandBtn, &QToolButton::toggled, this, [this](bool checked) {
        setCollapsed(!checked);
    });

    refreshHeader();
    rerenderBody();
    // Cards land collapsed by default — the title alone is enough to scan
    // a turn; click the chevron to read the body. Keeps long transcripts
    // skimmable without forcing the user to collapse each card by hand.
    setCollapsed(true);
}

void AcpToolCallCard::apply(const AcpProtocol::AcpToolCallUpdate &update)
{
    if (update.status.has_value()) {
        m_status = *update.status;
    }
    if (update.content.has_value()) {
        m_content = *update.content;
    }
    refreshHeader();
    rerenderBody();
}

QString AcpToolCallCard::statusGlyph() const
{
    if (m_status == QLatin1String("completed")) return QStringLiteral("✓");
    if (m_status == QLatin1String("failed"))    return QStringLiteral("✗");
    if (m_status == QLatin1String("running"))   return QStringLiteral("⚙");
    return QStringLiteral("⏳");
}

void AcpToolCallCard::refreshHeader()
{
    m_statusIcon->setText(statusGlyph());
    m_titleLabel->setText(m_title.isEmpty() ? tr("Tool call") : m_title);
}

void AcpToolCallCard::rerenderBody()
{
    if (!m_body) return;
    QString text;
    bool decodedSomething = false;
    for (const auto &v : m_content) {
        if (!v.isObject()) continue;
        const QJsonObject obj = v.toObject();
        const QString type = obj.value(QStringLiteral("type")).toString();
        if (type == QLatin1String("text")) {
            text += obj.value(QStringLiteral("text")).toString();
            text += QLatin1Char('\n');
            decodedSomething = true;
        } else if (type == QLatin1String("image")) {
            text += QStringLiteral("[image]\n");
            decodedSomething = true;
        }
    }
    if (!decodedSomething && !m_content.isEmpty()) {
        // Fall back to pretty JSON.
        const QJsonDocument doc(m_content);
        text = QString::fromUtf8(doc.toJson(QJsonDocument::Indented));
    }
    m_body->document()->setPlainText(text);
    refitBodyHeight();
}

void AcpToolCallCard::refitBodyHeight()
{
    if (!m_body) return;
    // Width from our own already-set geometry, not the child viewport — the
    // child has not been laid out yet inside our resizeEvent.
    int marginL = 0, marginT = 0, marginR = 0, marginB = 0;
    if (m_outer) {
        m_outer->getContentsMargins(&marginL, &marginT, &marginR, &marginB);
    }
    const int w = width() - marginL - marginR;
    if (w <= 0) return;
    QTextDocument *doc = m_body->document();
    doc->setTextWidth(w);
    const int bodyH = qMax(0, static_cast<int>(std::ceil(doc->size().height())));
    m_body->setFixedHeight(bodyH);

    // Pin the card's own height. setFixedHeight on the inner browser only
    // clamps the browser; the card's QFrame sizeHint still cascades through
    // QAbstractScrollArea's font-derived default and inflates the card.
    int headerH = 0;
    if (m_outer && m_outer->count() > 0) {
        if (auto *headerItem = m_outer->itemAt(0)) {
            headerH = headerItem->sizeHint().height();
        }
    }
    int cardH = marginT + marginB + headerH;
    if (m_body->isVisible()) {
        cardH += (m_outer ? m_outer->spacing() : 0) + bodyH;
    }
    setFixedHeight(cardH);
}

void AcpToolCallCard::resizeEvent(QResizeEvent *event)
{
    QFrame::resizeEvent(event);
    refitBodyHeight();
}

void AcpToolCallCard::setCollapsed(bool collapsed)
{
    m_collapsed = collapsed;
    if (m_body) m_body->setVisible(!collapsed);
    if (m_expandBtn) {
        m_expandBtn->blockSignals(true);
        m_expandBtn->setChecked(!collapsed);
        m_expandBtn->setText(collapsed ? QStringLiteral("▸") : QStringLiteral("▾"));
        m_expandBtn->blockSignals(false);
    }
    // Re-pin the card so it shrinks to header-only when collapsed (and
    // grows back when re-expanded).
    refitBodyHeight();
}
