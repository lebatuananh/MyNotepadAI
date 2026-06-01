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

#include <QFontDatabase>
#include <QLabel>
#include <QHBoxLayout>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTimer>
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
    resize(480, 320);

    auto *root = new QVBoxLayout(this);
    m_stack = new QStackedWidget(this);
    root->addWidget(m_stack, 1);

    // --- Progress page ---
    auto *progressPage = new QWidget(this);
    auto *pl = new QVBoxLayout(progressPage);
    m_stageLabel = new QLabel(tr("Connecting…"), progressPage);
    m_progress = new QProgressBar(progressPage);
    m_progress->setRange(0, 0); // indeterminate
    m_logOutput = new QPlainTextEdit(progressPage);
    m_logOutput->setReadOnly(true);
    m_logOutput->setMaximumBlockCount(500);
    QFont logFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    logFont.setPointSize(9);
    m_logOutput->setFont(logFont);
    m_logOutput->setFrameShape(QFrame::StyledPanel);
    m_cancelBtn = new QPushButton(tr("Cancel"), progressPage);
    auto *pBtns = new QHBoxLayout();
    pBtns->addStretch(1);
    pBtns->addWidget(m_cancelBtn);
    pl->addWidget(m_stageLabel);
    pl->addWidget(m_progress);
    pl->addWidget(m_logOutput, 1);
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
        m_overallTimer->start();
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

    // Defense-in-depth UI timeout (35s) — slightly longer than the worker's 30s
    // connect-phase deadline so the worker fires first and propagates cleanly via
    // connectionLost. This catches the edge case where the worker's timer somehow
    // doesn't fire (e.g. thread stall).
    m_overallTimer = new QTimer(this);
    m_overallTimer->setSingleShot(true);
    m_overallTimer->setInterval(35000);
    connect(m_overallTimer, &QTimer::timeout, this, &SshConnectDialog::onOverallTimeout);
    m_overallTimer->start();

    // Log the initial connection target.
    if (m_connection) {
        const auto &profile = m_connection->profile();
        appendLog(tr("Resolving host %1…").arg(profile.host));
    }

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
                appendLog(tr("WARNING: Host key has changed!"));
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
                const auto &profile = m_connection->profile();
                switch (s) {
                case SshConnection::State::ConnectingSocket:
                    setStage(tr("Connecting…"));
                    appendLog(tr("TCP connection to %1:%2 in progress…")
                                  .arg(profile.host).arg(profile.port));
                    break;
                case SshConnection::State::Handshaking:
                    setStage(tr("Connecting…"));
                    appendLog(tr("TCP connection established on port %1").arg(profile.port));
                    appendLog(tr("SSH handshake in progress…"));
                    break;
                case SshConnection::State::AwaitingHostKey:
                    appendLog(tr("Verifying host key…"));
                    break;
                case SshConnection::State::Authenticating:
                    setStage(tr("Authenticating…"));
                    appendLog(tr("Authenticating as %1…").arg(profile.username));
                    break;
                case SshConnection::State::Ready:
                    appendLog(tr("Connection established."));
                    break;
                case SshConnection::State::Disconnected:
                case SshConnection::State::Failed:
                    break;
                default:
                    break;
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

void SshConnectDialog::appendLog(const QString &message)
{
    m_logOutput->appendPlainText(message);
    // Auto-scroll to the latest entry.
    m_logOutput->verticalScrollBar()->setValue(m_logOutput->verticalScrollBar()->maximum());
}

void SshConnectDialog::onHostKey(const QString &fingerprint, const QByteArray &key)
{
    Q_UNUSED(key);
    if (m_hostKeyHandled) {
        return;
    }
    m_hostKeyHandled = true;
    m_overallTimer->stop();
    m_fingerprint->setPlainText(fingerprint);
    m_acceptBtn->setDefault(true);
    m_acceptBtn->setFocus();
    showPage(Page::HostKey);
}

void SshConnectDialog::onConnected()
{
    m_overallTimer->stop();
    accept();
}

void SshConnectDialog::onConnectionLost(const QString &reason)
{
    m_overallTimer->stop();
    const QString msg = reason.isEmpty() ? tr("The connection was lost.") : reason;
    appendLog(tr("Error: %1").arg(msg));
    m_errorText->setText(msg);
    m_retryBtn->setDefault(true);
    m_retryBtn->setFocus();
    showPage(Page::Error);
}

void SshConnectDialog::onAuthFailed(const QString &reason)
{
    m_overallTimer->stop();
    const QString msg = reason.isEmpty()
                            ? tr("Authentication failed — check your credentials.")
                            : reason;
    appendLog(tr("Error: %1").arg(msg));
    m_errorText->setText(msg);
    m_retryBtn->setDefault(true);
    m_retryBtn->setFocus();
    showPage(Page::Error);
}

void SshConnectDialog::onCancel()
{
    m_overallTimer->stop();
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
    appendLog(tr("Retrying connection…"));
    m_overallTimer->start();
    if (m_connection) {
        m_connection->connectToHost();
    }
}

void SshConnectDialog::onOverallTimeout()
{
    appendLog(tr("Error: Connection timed out — no response from server."));
    if (m_connection) {
        m_connection->disconnectFromHost();
    }
    m_errorText->setText(tr("Connection timed out — the server did not respond."));
    m_retryBtn->setDefault(true);
    m_retryBtn->setFocus();
    showPage(Page::Error);
}
