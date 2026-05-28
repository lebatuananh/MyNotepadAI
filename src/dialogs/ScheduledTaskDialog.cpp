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

#include "ScheduledTaskDialog.h"
#include "EditScheduledTaskDialog.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include "AcpAgentRegistry.h"
#include "ApplicationSettings.h"
#include "CronExpression.h"
#include "ScheduledTaskDefinition.h"
#include "ScheduledTaskRegistry.h"
#include "ScheduledTaskRunner.h"

ScheduledTaskDialog::ScheduledTaskDialog(ScheduledTaskRegistry *registry,
                                         ScheduledTaskRunner *runner,
                                         AcpAgentRegistry *agentRegistry,
                                         ApplicationSettings *settings,
                                         QWidget *parent)
    : QDialog(parent)
    , m_registry(registry)
    , m_runner(runner)
    , m_agentRegistry(agentRegistry)
    , m_settings(settings)
{
    setWindowTitle(tr("Scheduled Tasks"));
    resize(650, 400);

    auto *layout = new QVBoxLayout(this);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(4);
    m_table->setHorizontalHeaderLabels({tr("Enabled"), tr("Name"), tr("Cron"), tr("Next Fire")});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->setVisible(false);
    layout->addWidget(m_table);

    auto *buttonLayout = new QHBoxLayout();
    auto *addBtn = new QPushButton(tr("Add"), this);
    auto *runNowBtn = new QPushButton(tr("Run Now"), this);
    auto *editBtn = new QPushButton(tr("Edit"), this);
    auto *removeBtn = new QPushButton(tr("Remove"), this);
    auto *closeBtn = new QPushButton(tr("Close"), this);

    buttonLayout->addWidget(addBtn);
    buttonLayout->addWidget(runNowBtn);
    buttonLayout->addWidget(editBtn);
    buttonLayout->addWidget(removeBtn);
    buttonLayout->addStretch();
    buttonLayout->addWidget(closeBtn);
    layout->addLayout(buttonLayout);

    connect(addBtn, &QPushButton::clicked, this, &ScheduledTaskDialog::onAdd);
    connect(runNowBtn, &QPushButton::clicked, this, &ScheduledTaskDialog::onRunNow);
    connect(editBtn, &QPushButton::clicked, this, &ScheduledTaskDialog::onEdit);
    connect(removeBtn, &QPushButton::clicked, this, &ScheduledTaskDialog::onRemove);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_table, &QTableWidget::cellDoubleClicked, this, &ScheduledTaskDialog::onEdit);
    connect(m_registry, &ScheduledTaskRegistry::changed, this, &ScheduledTaskDialog::refreshTable);

    refreshTable();
}

void ScheduledTaskDialog::setDefaultWorkspace(const QString &cwd)
{
    m_defaultWorkspace = cwd;
}

void ScheduledTaskDialog::setRecentWorkspaces(const QStringList &paths)
{
    m_recentWorkspaces = paths;
}

void ScheduledTaskDialog::onAdd()
{
    ScheduledTaskDefinition def;
    def.cwd = m_defaultWorkspace;
    if (EditScheduledTaskDialog::editTask(def, m_agentRegistry, m_settings, this, m_recentWorkspaces)) {
        m_registry->addTask(def);
    }
}

void ScheduledTaskDialog::onEdit()
{
    const int row = m_table->currentRow();
    if (row < 0) {
        return;
    }

    const auto tasks = m_registry->tasks();
    if (row >= tasks.size()) {
        return;
    }

    ScheduledTaskDefinition def = tasks[row];
    if (EditScheduledTaskDialog::editTask(def, m_agentRegistry, m_settings, this, m_recentWorkspaces)) {
        m_registry->updateTask(def);
    }
}

void ScheduledTaskDialog::onRemove()
{
    const int row = m_table->currentRow();
    if (row < 0) {
        return;
    }

    const auto tasks = m_registry->tasks();
    if (row >= tasks.size()) {
        return;
    }

    const auto &task = tasks[row];
    const auto answer = QMessageBox::question(
        this, tr("Remove Task"),
        tr("Remove scheduled task \"%1\"?").arg(task.name),
        QMessageBox::Yes | QMessageBox::No);

    if (answer == QMessageBox::Yes) {
        m_registry->removeTask(task.id);
    }
}

void ScheduledTaskDialog::onRunNow()
{
    const int row = m_table->currentRow();
    if (row < 0) {
        return;
    }

    const auto tasks = m_registry->tasks();
    if (row >= tasks.size()) {
        return;
    }

    m_runner->manualTrigger(tasks[row].id);
}

void ScheduledTaskDialog::refreshTable()
{
    const auto tasks = m_registry->tasks();
    m_table->setRowCount(tasks.size());
    const QDateTime now = QDateTime::currentDateTime();

    for (int i = 0; i < tasks.size(); ++i) {
        const auto &task = tasks[i];

        // Enabled checkbox
        auto *checkBox = new QCheckBox(this);
        checkBox->setChecked(task.enabled);
        const QString taskId = task.id;
        connect(checkBox, &QCheckBox::toggled, this, [this, taskId](bool checked) {
            m_registry->setEnabled(taskId, checked);
        });
        m_table->setCellWidget(i, 0, checkBox);

        // Name
        m_table->setItem(i, 1, new QTableWidgetItem(task.name));

        // Cron
        m_table->setItem(i, 2, new QTableWidgetItem(task.cron));

        // Next fire time
        QString nextStr;
        if (task.enabled) {
            auto parsed = CronExpression::parse(task.cron);
            if (parsed && parsed->isValid()) {
                const QDateTime next = parsed->nextFireTime(now);
                if (next.isValid()) {
                    nextStr = next.toString(QStringLiteral("yyyy-MM-dd hh:mm"));
                }
            }
        }
        m_table->setItem(i, 3, new QTableWidgetItem(nextStr));
    }
}
