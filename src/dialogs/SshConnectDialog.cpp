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

#include "SshConnectDialog.h"

#include "remote/RemoteExecutionContext.h"
#include "remote/SshConnection.h"

#include <QLabel>
#include <QHBoxLayout>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QWidget>

using remote::SshConnection;

SshConnectDialog::SshConnectDialog(remote::RemoteExecutionContext *context, QWidget *parent)
    : QDialog(parent)
    , m_context(context)
    , m_connection(context ? context->connection() : nullptr)
{
    setWindowTitle(tr("Connect"));
    setModal(true);
    resize(440, 240);

    auto *root = new QVBoxLayout(this);
    m_stack = new QStackedWidget(this);
    root->addWidget(m_stack, 1);

    // --- Progress page ---
    auto *progressPage = new QWidget(this);
    auto *pl = new QVBoxLayout(progressPage);
    m_stageLabel = new QLabel(tr("Connecting…"), progressPage);
    m_progress = new QProgressBar(progressPage);
    m_progress->setRange(0, 0); // indeterminate
    m_cancelBtn = new QPushButton(tr("Cancel"), progressPage);
    auto *pBtns = new QHBoxLayout();
    pBtns->addStretch(1);
    pBtns->addWidget(m_cancelBtn);
    pl->addWidget(m_stageLabel);
    pl->addWidget(m_progress);
    pl->addStretch(1);
    pl->addLayout(pBtns);
    m_stack->addWidget(progressPage);

    // --- Host-key page ---
    auto *hostKeyPage = new QWidget(this);
    auto *hl = new QVBoxLayout(hostKeyPage);
    m_hostKeyText = new QLabel(
        tr("The authenticity of this host can't be established. Verify the "
           "fingerprint before continuing."),
        hostKeyPage);
    m_hostKeyText->setWordWrap(true);
    m_fingerprint = new QPlainTextEdit(hostKeyPage);
    m_fingerprint->setReadOnly(true);
    m_fingerprint->setMaximumHeight(48);
    m_acceptBtn = new QPushButton(tr("Accept"), hostKeyPage);
    m_rejectBtn = new QPushButton(tr("Reject"), hostKeyPage);
    auto *hBtns = new QHBoxLayout();
    hBtns->addStretch(1);
    hBtns->addWidget(m_rejectBtn);
    hBtns->addWidget(m_acceptBtn);
    hl->addWidget(m_hostKeyText);
    hl->addWidget(m_fingerprint);
    hl->addStretch(1);
    hl->addLayout(hBtns);
    m_stack->addWidget(hostKeyPage);

    // --- Error page ---
    auto *errorPage = new QWidget(this);
    auto *el = new QVBoxLayout(errorPage);
    m_errorText = new QLabel(errorPage);
    m_errorText->setWordWrap(true);
    m_retryBtn = new QPushButton(tr("Retry"), errorPage);
    m_closeBtn = new QPushButton(tr("Close"), errorPage);
    auto *eBtns = new QHBoxLayout();
    eBtns->addStretch(1);
    eBtns->addWidget(m_closeBtn);
    eBtns->addWidget(m_retryBtn);
    el->addWidget(m_errorText);
    el->addStretch(1);
    el->addLayout(eBtns);
    m_stack->addWidget(errorPage);

    connect(m_cancelBtn, &QPushButton::clicked, this, &SshConnectDialog::onCancel);
    connect(m_acceptBtn, &QPushButton::clicked, this, [this]() {
        if (m_connection) m_connection->acceptHostKey();
        showPage(Page::Progress);
        setStage(tr("Authenticating…"));
    });
    connect(m_rejectBtn, &QPushButton::clicked, this, [this]() {
        if (m_connection) m_connection->rejectHostKey();
        reject();
    });
    connect(m_retryBtn, &QPushButton::clicked, this, &SshConnectDialog::onRetry);
    connect(m_closeBtn, &QPushButton::clicked, this, &QDialog::reject);

    showPage(Page::Progress);
    setStage(tr("Connecting…"));
    wireConnection();

    // If the connection is already past Ready (re-shown), reflect it.
    if (m_context && m_context->state() == remote::ExecutionContext::State::Connected) {
        onConnected();
    }
}

SshConnectDialog::~SshConnectDialog() = default;

void SshConnectDialog::wireConnection()
{
    if (!m_connection) {
        return;
    }
    connect(m_connection, &SshConnection::hostKeyReceived, this,
            &SshConnectDialog::onHostKey, Qt::UniqueConnection);
    connect(m_connection, &SshConnection::hostKeyChanged, this,
            [this](const QString &fingerprint, const QByteArray &key) {
                // Loud changed-key warning (MITM guard) — pair the colored state
                // with explicit text per ui-dna; color is never the sole carrier.
                m_hostKeyText->setText(
                    tr("WARNING: the host key for this server has CHANGED since "
                       "you last connected. This may indicate a man-in-the-middle "
                       "attack. Only accept if you know the key was rotated."));
                onHostKey(fingerprint, key);
            }, Qt::UniqueConnection);
    connect(m_connection, &SshConnection::connected, this,
            &SshConnectDialog::onConnected, Qt::UniqueConnection);
    connect(m_connection, &SshConnection::connectionLost, this,
            &SshConnectDialog::onConnectionLost, Qt::UniqueConnection);
    connect(m_connection, &SshConnection::authFailed, this,
            &SshConnectDialog::onAuthFailed, Qt::UniqueConnection);
    connect(m_connection, &SshConnection::stateChanged, this,
            [this](SshConnection::State s) {
                if (s == SshConnection::State::Handshaking) {
                    setStage(tr("Connecting…"));
                } else if (s == SshConnection::State::Authenticating) {
                    setStage(tr("Authenticating…"));
                }
            }, Qt::UniqueConnection);
}

void SshConnectDialog::showPage(Page page)
{
    m_stack->setCurrentIndex(static_cast<int>(page));
}

void SshConnectDialog::setStage(const QString &text)
{
    m_stageLabel->setText(text);
}

void SshConnectDialog::onHostKey(const QString &fingerprint, const QByteArray &key)
{
    Q_UNUSED(key);
    if (m_hostKeyHandled) {
        return;
    }
    m_hostKeyHandled = true;
    m_fingerprint->setPlainText(fingerprint);
    m_acceptBtn->setDefault(true);
    m_acceptBtn->setFocus();
    showPage(Page::HostKey);
}

void SshConnectDialog::onConnected()
{
    accept();
}

void SshConnectDialog::onConnectionLost(const QString &reason)
{
    m_errorText->setText(reason.isEmpty() ? tr("The connection was lost.") : reason);
    m_retryBtn->setDefault(true);
    m_retryBtn->setFocus();
    showPage(Page::Error);
}

void SshConnectDialog::onAuthFailed(const QString &reason)
{
    m_errorText->setText(reason.isEmpty()
                             ? tr("Authentication failed — check your credentials.")
                             : reason);
    m_retryBtn->setDefault(true);
    m_retryBtn->setFocus();
    showPage(Page::Error);
}

void SshConnectDialog::onCancel()
{
    if (m_connection) {
        m_connection->disconnectFromHost();
    }
    reject();
}

void SshConnectDialog::onRetry()
{
    m_hostKeyHandled = false;
    showPage(Page::Progress);
    setStage(tr("Connecting…"));
    if (m_connection) {
        m_connection->connectToHost();
    }
}
