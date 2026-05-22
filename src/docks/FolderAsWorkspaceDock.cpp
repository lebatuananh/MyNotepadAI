/*
 * This file is part of Notepad Next.
 * Copyright 2022 Justin Dailey
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


#include "FolderAsWorkspaceDock.h"
#include "ApplicationSettings.h"
#include "GitController.h"
#include "GitDiffViewController.h"
#include "GitTabWidget.h"
#include "NotepadNextApplication.h"
#include "ui_FolderAsWorkspaceDock.h"

#include <QApplication>
#include <QCursor>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QHelpEvent>
#include <QStyle>
#include <QTabWidget>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>

ApplicationSetting<QString> rootPathSetting{"FolderAsWorkspace/RootPath"};

namespace {

class FolderAsWorkspaceFsModel : public QFileSystemModel
{
public:
    using QFileSystemModel::QFileSystemModel;

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (role == Qt::ToolTipRole && index.isValid()) {
            return QDir::toNativeSeparators(filePath(index));
        }
        return QFileSystemModel::data(index, role);
    }
};

} // namespace

FolderAsWorkspaceDock::FolderAsWorkspaceDock(QWidget *parent) :
    QDockWidget(parent),
    ui(new Ui::FolderAsWorkspaceDock),
    model(new FolderAsWorkspaceFsModel(this)),
    tooltipTimer(new QTimer(this))
{
    ui->setupUi(this);

    ui->treeView->setModel(model);
    ui->treeView->header()->hideSection(1);
    ui->treeView->header()->hideSection(2);
    ui->treeView->header()->hideSection(3);

    connect(ui->treeView, &QTreeView::doubleClicked, this, [=](const QModelIndex &index) {
        if (!model->isDir(index)) {
            emit fileDoubleClicked(model->filePath(index));
        }
    });

    const int wakeUpDelay = QApplication::style()->styleHint(QStyle::SH_ToolTip_WakeUpDelay);
    tooltipTimer->setSingleShot(true);
    tooltipTimer->setInterval(wakeUpDelay > 0 ? wakeUpDelay : 700);
    connect(tooltipTimer, &QTimer::timeout, this, [this]() {
        QWidget *viewport = ui->treeView->viewport();
        const QPoint localPos = viewport->mapFromGlobal(QCursor::pos());
        const QModelIndex index = ui->treeView->indexAt(localPos);
        if (!index.isValid() || QPersistentModelIndex(index) != pendingTooltipIndex) {
            return;
        }
        const QString text = model->data(index, Qt::ToolTipRole).toString();
        if (text.isEmpty()) {
            return;
        }
        QToolTip::showText(QCursor::pos(), text, viewport, ui->treeView->visualRect(index));
    });

    ui->treeView->viewport()->installEventFilter(this);

    connect(ui->tabs, &QTabWidget::currentChanged, this, &FolderAsWorkspaceDock::onTabChanged);

    ApplicationSettings settings;
    setRootPath(settings.get(rootPathSetting));
}

FolderAsWorkspaceDock::FolderAsWorkspaceDock(const QString &initialPath, QWidget *parent) :
    QDockWidget(parent),
    ui(new Ui::FolderAsWorkspaceDock),
    model(new FolderAsWorkspaceFsModel(this)),
    tooltipTimer(new QTimer(this))
{
    ui->setupUi(this);

    ui->treeView->setModel(model);
    ui->treeView->header()->hideSection(1);
    ui->treeView->header()->hideSection(2);
    ui->treeView->header()->hideSection(3);

    connect(ui->treeView, &QTreeView::doubleClicked, this, [=](const QModelIndex &index) {
        if (!model->isDir(index)) {
            emit fileDoubleClicked(model->filePath(index));
        }
    });

    const int wakeUpDelay = QApplication::style()->styleHint(QStyle::SH_ToolTip_WakeUpDelay);
    tooltipTimer->setSingleShot(true);
    tooltipTimer->setInterval(wakeUpDelay > 0 ? wakeUpDelay : 700);
    connect(tooltipTimer, &QTimer::timeout, this, [this]() {
        QWidget *viewport = ui->treeView->viewport();
        const QPoint localPos = viewport->mapFromGlobal(QCursor::pos());
        const QModelIndex index = ui->treeView->indexAt(localPos);
        if (!index.isValid() || QPersistentModelIndex(index) != pendingTooltipIndex) {
            return;
        }
        const QString text = model->data(index, Qt::ToolTipRole).toString();
        if (text.isEmpty()) {
            return;
        }
        QToolTip::showText(QCursor::pos(), text, viewport, ui->treeView->visualRect(index));
    });

    ui->treeView->viewport()->installEventFilter(this);

    connect(ui->tabs, &QTabWidget::currentChanged, this, &FolderAsWorkspaceDock::onTabChanged);

    // Explicit-path ctor: skip the saved-setting load so additional workspaces
    // don't briefly flash the previous global root before showing their own.
    setRootPath(initialPath);
}

FolderAsWorkspaceDock::~FolderAsWorkspaceDock()
{
    delete ui;
}

void FolderAsWorkspaceDock::setRootPath(const QString dir)
{
    ApplicationSettings settings;
    settings.set(rootPathSetting, dir);

    model->setRootPath(dir);
    ui->treeView->setRootIndex(model->index(dir));

    if (gitTab) {
        gitTab->setWorkspaceRoot(dir);
    }

    // Window title doubles as the tab label when several workspaces are tabified
    // alongside each other, so make it the folder basename rather than the static
    // .ui label.
    if (dir.isEmpty()) {
        setWindowTitle(tr("Folder as Workspace"));
    } else {
        QString basename = QFileInfo(QDir::cleanPath(dir)).fileName();
        if (basename.isEmpty()) basename = dir;
        setWindowTitle(basename);
    }
}

QString FolderAsWorkspaceDock::rootPath() const
{
    return model->rootPath();
}

bool FolderAsWorkspaceDock::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == ui->treeView->viewport()) {
        switch (event->type()) {
        case QEvent::ToolTip: {
            auto *helpEvent = static_cast<QHelpEvent *>(event);
            const QModelIndex index = ui->treeView->indexAt(helpEvent->pos());

            if (!index.isValid()) {
                tooltipTimer->stop();
                pendingTooltipIndex = QPersistentModelIndex();
                QToolTip::hideText();
                return true;
            }

            const QPersistentModelIndex pIndex(index);

            // Same item we're already tracking (timer running or tooltip shown) — leave it alone.
            if (pIndex == pendingTooltipIndex) {
                return true;
            }

            pendingTooltipIndex = pIndex;

            if (!QToolTip::isVisible() && !tooltipTimer->isActive()) {
                // No tooltip in flight — Qt has already waited the standard delay,
                // so show the first one immediately.
                const QString text = model->data(index, Qt::ToolTipRole).toString();
                if (!text.isEmpty()) {
                    QToolTip::showText(helpEvent->globalPos(), text, ui->treeView->viewport(),
                                       ui->treeView->visualRect(index));
                }
            } else {
                // Switching from one file's tooltip to another — hide and force a fresh wait.
                QToolTip::hideText();
                tooltipTimer->start();
            }
            return true;
        }
        case QEvent::Leave:
            tooltipTimer->stop();
            pendingTooltipIndex = QPersistentModelIndex();
            QToolTip::hideText();
            break;
        default:
            break;
        }
    }
    return QDockWidget::eventFilter(watched, event);
}

void FolderAsWorkspaceDock::onTabChanged(int index)
{
    // Lazy-create the Git tab the first time the user looks at it,
    // so docks that never need it don't spawn a GitController.
    if (index < 0) return;
    QWidget *page = ui->tabs->widget(index);
    if (page == ui->gitTab) {
        ensureGitTab();
    }
}

void FolderAsWorkspaceDock::ensureGitTab()
{
    if (gitTab) {
        gitTab->initializeIfNeeded();
        return;
    }
    gitTab = new GitTabWidget(rootPath(), this);
    auto *layout = qobject_cast<QVBoxLayout *>(ui->gitTab->layout());
    if (layout) {
        layout->addWidget(gitTab);
    } else {
        // Defensive: .ui should always provide a layout, but fall back if not.
        auto *fallback = new QVBoxLayout(ui->gitTab);
        fallback->setContentsMargins(0, 0, 0, 0);
        fallback->setSpacing(0);
        fallback->addWidget(gitTab);
    }
    connect(gitTab, &GitTabWidget::fileActivated,
            this, &FolderAsWorkspaceDock::fileDoubleClicked);
    connect(gitTab, &GitTabWidget::diffRequested,
            this, &FolderAsWorkspaceDock::gitDiffRequested);
    connect(gitTab, &GitTabWidget::openSubmoduleRequested,
            this, &FolderAsWorkspaceDock::gitOpenSubmoduleRequested);
    gitTab->initializeIfNeeded();
}

GitDiffViewController *FolderAsWorkspaceDock::ensureGitDiffViewController()
{
    if (gitDiffViewController) return gitDiffViewController;

    ensureGitTab();
    if (!gitTab || !gitTab->controller()) return nullptr;

    auto *app = qobject_cast<NotepadNextApplication*>(qApp);
    if (!app) return nullptr;

    gitDiffViewController = new GitDiffViewController(gitTab->controller(),
                                                      app->getEditorManager(),
                                                      this);
    gitDiffViewController->setDarkPalette(app->isEffectiveThemeDark());

    connect(gitDiffViewController, &GitDiffViewController::diffRendered,
            this, &FolderAsWorkspaceDock::gitDiffPreviewRendered);
    connect(gitDiffViewController, &GitDiffViewController::diffFailed,
            this, &FolderAsWorkspaceDock::gitDiffPreviewFailed);

    connect(app, &NotepadNextApplication::effectiveThemeChanged,
            gitDiffViewController, [this, app]() {
                gitDiffViewController->setDarkPalette(app->isEffectiveThemeDark());
            });

    return gitDiffViewController;
}

void FolderAsWorkspaceDock::showGitDiffPreview(const GitStatusEntry &entry)
{
    if (auto *c = ensureGitDiffViewController()) {
        c->showDiffFor(entry);
    }
}

void FolderAsWorkspaceDock::showGitTab()
{
    // Setting currentWidget triggers QTabWidget::currentChanged → onTabChanged,
    // which lazy-constructs the GitTabWidget. No need to call ensureGitTab here.
    if (ui->tabs->currentWidget() != ui->gitTab) {
        ui->tabs->setCurrentWidget(ui->gitTab);
    } else {
        // Already on the Git tab — make sure the underlying widget exists
        // (no currentChanged would fire on a no-op switch).
        ensureGitTab();
    }
}
