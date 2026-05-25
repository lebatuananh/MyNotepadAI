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

#include <QtTest>
#include <QCoreApplication>
#include <QSettings>
#include <QTemporaryDir>

#include "ApplicationSettings.h"
#include "TerminalTaskRegistry.h"

class TestTerminalTaskRegistry : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    void emptyWorkspace_returnsEmpty();
    void addTask_roundTrip_singleTask();
    void addTask_roundTrip_multipleTasks_sameWorkspace();
    void addTask_multipleWorkspaces_isolated();
    void addTask_emptyName_fallsBackToCommand();
    void addTask_emptyCommand_ignored();
    void normalization_trailingSlash_collidesWithClean();
#ifdef Q_OS_WIN
    void normalization_caseVariants_collide();
    void normalization_backslash_collidesWithForwardSlash();
#endif

private:
    QTemporaryDir tempDir;
};

void TestTerminalTaskRegistry::initTestCase()
{
    QVERIFY(tempDir.isValid());
    QCoreApplication::setOrganizationName("NotepadNextTest");
    QCoreApplication::setApplicationName("NotepadNextTestTaskRegistry");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, tempDir.path());
}

void TestTerminalTaskRegistry::init()
{
    ApplicationSettings s;
    s.clear();
    s.sync();
}

void TestTerminalTaskRegistry::emptyWorkspace_returnsEmpty()
{
    ApplicationSettings s;
    TerminalTaskRegistry r(&s);
    QVERIFY(r.tasksForWorkspace(QStringLiteral("/some/path")).isEmpty());
}

void TestTerminalTaskRegistry::addTask_roundTrip_singleTask()
{
    ApplicationSettings s;
    TerminalTaskRegistry r(&s);
    r.addTask(QStringLiteral("/ws/foo"), {QStringLiteral("Build"), QStringLiteral("cmake --build .")});
    const auto tasks = r.tasksForWorkspace(QStringLiteral("/ws/foo"));
    QCOMPARE(tasks.size(), 1);
    QCOMPARE(tasks[0].name,    QStringLiteral("Build"));
    QCOMPARE(tasks[0].command, QStringLiteral("cmake --build ."));
}

void TestTerminalTaskRegistry::addTask_roundTrip_multipleTasks_sameWorkspace()
{
    ApplicationSettings s;
    TerminalTaskRegistry r(&s);
    r.addTask(QStringLiteral("/ws/foo"), {QStringLiteral("Build"), QStringLiteral("cmake --build .")});
    r.addTask(QStringLiteral("/ws/foo"), {QStringLiteral("Test"),  QStringLiteral("ctest")});
    const auto tasks = r.tasksForWorkspace(QStringLiteral("/ws/foo"));
    QCOMPARE(tasks.size(), 2);
    QCOMPARE(tasks[0].name, QStringLiteral("Build"));
    QCOMPARE(tasks[1].name, QStringLiteral("Test"));
}

void TestTerminalTaskRegistry::addTask_multipleWorkspaces_isolated()
{
    ApplicationSettings s;
    TerminalTaskRegistry r(&s);
    r.addTask(QStringLiteral("/ws/alpha"), {QStringLiteral("A"), QStringLiteral("echo a")});
    r.addTask(QStringLiteral("/ws/beta"),  {QStringLiteral("B"), QStringLiteral("echo b")});

    const auto alpha = r.tasksForWorkspace(QStringLiteral("/ws/alpha"));
    const auto beta  = r.tasksForWorkspace(QStringLiteral("/ws/beta"));
    QCOMPARE(alpha.size(), 1);
    QCOMPARE(beta.size(),  1);
    QCOMPARE(alpha[0].name, QStringLiteral("A"));
    QCOMPARE(beta[0].name,  QStringLiteral("B"));
}

void TestTerminalTaskRegistry::addTask_emptyName_fallsBackToCommand()
{
    ApplicationSettings s;
    TerminalTaskRegistry r(&s);
    r.addTask(QStringLiteral("/ws/foo"), {QString(), QStringLiteral("make")});
    const auto tasks = r.tasksForWorkspace(QStringLiteral("/ws/foo"));
    QCOMPARE(tasks.size(), 1);
    QCOMPARE(tasks[0].name, QStringLiteral("make"));
}

void TestTerminalTaskRegistry::addTask_emptyCommand_ignored()
{
    ApplicationSettings s;
    TerminalTaskRegistry r(&s);
    r.addTask(QStringLiteral("/ws/foo"), {QStringLiteral("Bad"), QString()});
    QVERIFY(r.tasksForWorkspace(QStringLiteral("/ws/foo")).isEmpty());
}

void TestTerminalTaskRegistry::normalization_trailingSlash_collidesWithClean()
{
    ApplicationSettings s;
    TerminalTaskRegistry r(&s);
    r.addTask(QStringLiteral("/ws/foo/"), {QStringLiteral("T"), QStringLiteral("cmd")});
    QCOMPARE(r.tasksForWorkspace(QStringLiteral("/ws/foo")).size(), 1);
}

#ifdef Q_OS_WIN
void TestTerminalTaskRegistry::normalization_caseVariants_collide()
{
    ApplicationSettings s;
    TerminalTaskRegistry r(&s);
    r.addTask(QStringLiteral("D:/projects/foo"), {QStringLiteral("T"), QStringLiteral("cmd")});
    QCOMPARE(r.tasksForWorkspace(QStringLiteral("d:/projects/foo")).size(), 1);
    QCOMPARE(r.tasksForWorkspace(QStringLiteral("D:/projects/foo")).size(), 1);
}

void TestTerminalTaskRegistry::normalization_backslash_collidesWithForwardSlash()
{
    ApplicationSettings s;
    TerminalTaskRegistry r(&s);
    r.addTask(QStringLiteral("D:\\projects\\foo"), {QStringLiteral("T"), QStringLiteral("cmd")});
    QCOMPARE(r.tasksForWorkspace(QStringLiteral("D:/projects/foo")).size(), 1);
}
#endif

QTEST_APPLESS_MAIN(TestTerminalTaskRegistry)

#include "test_terminal_task_registry.moc"
