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

#include "DockMiddleClickCloser.h"

#include <QCoreApplication>
#include <QDockWidget>
#include <QEvent>
#include <QMainWindow>
#include <QMouseEvent>
#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QStyle>
#include <QStyleOptionDockWidget>
#include <QTabBar>
#include <QVariant>

namespace {

class Filter : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() != QEvent::MouseButtonRelease) {
            return QObject::eventFilter(watched, event);
        }

        auto *dock = qobject_cast<QDockWidget *>(watched);
        if (dock == nullptr) {
            return QObject::eventFilter(watched, event);
        }

        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() != Qt::MiddleButton) {
            return QObject::eventFilter(watched, event);
        }

        // The default QDockWidget title bar is drawn into the top strip of
        // PM_TitleBarHeight pixels of the dock itself (not a child widget),
        // so middle-button releases there reach this filter directly. We do
        // not currently support QDockWidget::setTitleBarWidget(); no dock
        // in the project uses it. If a custom title bar widget is ever set,
        // mouse events on it would target that child and never reach here.
        QStyleOptionDockWidget opt;
        opt.initFrom(dock);
        const int titleHeight = dock->style()->pixelMetric(QStyle::PM_TitleBarHeight, &opt, dock);
        const QPoint pos = me->position().toPoint();
        if (pos.y() < 0 || pos.y() > titleHeight) {
            return QObject::eventFilter(watched, event);
        }

        dock->close();
        return true;
    }
};

Filter *sharedFilter()
{
    // Singleton parented to qApp so it is destroyed during normal application
    // teardown after all QDockWidgets are gone — avoiding both a process-exit
    // leak and a dangling-filter dispatch on still-alive docks.
    static QPointer<Filter> instance;
    if (instance.isNull()) {
        instance = new Filter(QCoreApplication::instance());
    }
    return instance.data();
}

QDockWidget *dockForTab(QTabBar *tabBar, int index)
{
    if (index < 0)
        return nullptr;

    // Qt stores the QDockWidget pointer (as quintptr) in each tab's data.
    // This is the only unambiguous mapping when multiple docks share a title.
    const QVariant data = tabBar->tabData(index);
    if (data.isValid()) {
        auto *widget = reinterpret_cast<QWidget *>(qvariant_cast<quintptr>(data));
        if (auto *dock = qobject_cast<QDockWidget *>(widget))
            return dock;
    }

    // Fallback: title match (ambiguous with duplicate titles, but covers the
    // case where tabData is not populated).
    const QString tabText = tabBar->tabText(index);
    if (tabText.isEmpty())
        return nullptr;
    QWidget *w = tabBar->parentWidget();
    QMainWindow *mw = nullptr;
    while (w) {
        mw = qobject_cast<QMainWindow *>(w);
        if (mw)
            break;
        w = w->parentWidget();
    }
    if (!mw)
        return nullptr;
    const auto docks = mw->findChildren<QDockWidget *>();
    for (auto *dock : docks) {
        if (dock->windowTitle() == tabText)
            return dock;
    }
    return nullptr;
}

class TabBarFilter : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() != QEvent::MouseButtonRelease)
            return QObject::eventFilter(watched, event);

        auto *tabBar = qobject_cast<QTabBar *>(watched);
        if (!tabBar)
            return QObject::eventFilter(watched, event);

        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() != Qt::MiddleButton)
            return QObject::eventFilter(watched, event);

        const int idx = tabBar->tabAt(me->position().toPoint());
        QDockWidget *dock = dockForTab(tabBar, idx);
        if (!dock)
            return QObject::eventFilter(watched, event);

        dock->close();
        return true;
    }
};

TabBarFilter *sharedTabBarFilter()
{
    static QPointer<TabBarFilter> instance;
    if (instance.isNull()) {
        instance = new TabBarFilter(QCoreApplication::instance());
    }
    return instance.data();
}

} // namespace

namespace DockMiddleClickCloser {

void install(QDockWidget *dock)
{
    if (dock == nullptr) {
        return;
    }
    dock->installEventFilter(sharedFilter());
}

void installTabBarFilter(QMainWindow *mainWindow)
{
    Q_UNUSED(mainWindow);
    // Install on qApp so we catch middle-clicks on any QTabBar in the process.
    // The dockForTab() lookup ensures we only act when the tab bar actually
    // belongs to a tabified dock group — other QTabBars (dialogs, etc.) are
    // left alone because no sibling QDockWidget will match.
    QCoreApplication::instance()->installEventFilter(sharedTabBarFilter());
}

} // namespace DockMiddleClickCloser
