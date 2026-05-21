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


// Regression test for: Tab/Backtab were being consumed by QWidget::event()
// focus-traversal (focusNextPrevChild) before reaching TerminalWidget::keyPressEvent,
// so they never made it to the PTY. TerminalWidget::event() now intercepts those
// key events and forwards them to keyPressEvent.
//
// To deterministically exercise the bug *without* requiring sibling widgets and
// an active window, the test subclass forces focusNextPrevChild() to return true
// (simulating "a sibling took the Tab focus"). With the production event()
// override in place, Tab/Backtab still reach keyPressEvent. Without the override,
// QWidget::event() would route them through focusNextPrevChild and skip
// keyPressEvent — the test then fails (tabHits stays at 0), which is exactly the
// regression we want to guard against.


#include <QtTest>
#include <QCoreApplication>
#include <QKeyEvent>

#include "TerminalWidget.h"


class TestableTerminalWidget : public TerminalWidget
{
public:
    using TerminalWidget::TerminalWidget;

    int tabHits = 0;
    int backtabHits = 0;
    int otherHits = 0;
    Qt::KeyboardModifiers lastBacktabMods = Qt::NoModifier;

protected:
    // Replaces the production keyPressEvent so the test does not depend on
    // libvterm runtime state (m_vt / m_pty are null, so the production
    // keyPressEvent would short-circuit to the base class).
    void keyPressEvent(QKeyEvent *e) override
    {
        switch (e->key()) {
        case Qt::Key_Tab:
            ++tabHits;
            break;
        case Qt::Key_Backtab:
            ++backtabHits;
            lastBacktabMods = e->modifiers();
            break;
        default:
            ++otherHits;
            break;
        }
        e->accept();
    }

    // Pretend a focusable sibling exists so the QWidget::event() Tab path
    // would normally consume the key. The production event() override must
    // intercept first; otherwise this returns true and keyPressEvent is skipped.
    bool focusNextPrevChild(bool next) override
    {
        Q_UNUSED(next);
        return true;
    }
};


class TestTerminalKeyRouting : public QObject
{
    Q_OBJECT

private slots:
    void tabKeyPressReachesKeyPressEvent();
    void backtabKeyPressReachesKeyPressEvent();
    void shiftBacktabPreservesShiftModifier();
    void regularKeyStillReachesKeyPressEvent();
};


void TestTerminalKeyRouting::tabKeyPressReachesKeyPressEvent()
{
    TestableTerminalWidget w;
    QKeyEvent ev(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
    QCoreApplication::sendEvent(&w, &ev);
    QCOMPARE(w.tabHits, 1);
    QCOMPARE(w.backtabHits, 0);
}

void TestTerminalKeyRouting::backtabKeyPressReachesKeyPressEvent()
{
    TestableTerminalWidget w;
    // Qt translates Shift+Tab to Qt::Key_Backtab; the modifier flag stays set.
    QKeyEvent ev(QEvent::KeyPress, Qt::Key_Backtab, Qt::ShiftModifier);
    QCoreApplication::sendEvent(&w, &ev);
    QCOMPARE(w.backtabHits, 1);
    QCOMPARE(w.tabHits, 0);
}

void TestTerminalKeyRouting::shiftBacktabPreservesShiftModifier()
{
    TestableTerminalWidget w;
    QKeyEvent ev(QEvent::KeyPress, Qt::Key_Backtab, Qt::ShiftModifier);
    QCoreApplication::sendEvent(&w, &ev);
    QVERIFY(w.lastBacktabMods.testFlag(Qt::ShiftModifier));
}

void TestTerminalKeyRouting::regularKeyStillReachesKeyPressEvent()
{
    TestableTerminalWidget w;
    QKeyEvent ev(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, QStringLiteral("a"));
    QCoreApplication::sendEvent(&w, &ev);
    QCOMPARE(w.otherHits, 1);
}


QTEST_MAIN(TestTerminalKeyRouting)
#include "test_terminal_key_routing.moc"
