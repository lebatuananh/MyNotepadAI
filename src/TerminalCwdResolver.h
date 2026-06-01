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

#ifndef TERMINALCWDRESOLVER_H
#define TERMINALCWDRESOLVER_H

#include <QString>

namespace remote { class ExecutionContext; }

class TerminalCwdResolver
{
public:
    static bool canOpenInWorkspace(const QString &workspaceRoot);
    static bool canOpenInFolder(const QString &activeFilePath, bool activeBufferIsFile, const QString &workspaceRoot);

    static QString resolveWorkspace(const QString &workspaceRoot);
    static QString resolveFolder(const QString &activeFilePath, bool activeBufferIsFile, const QString &workspaceRoot);

    // Context-aware resolution (design D11). When `ctx` is remote: NO local
    // QFileInfo check (the path is on another machine) — require the context to
    // be Connected and `requested` non-empty, then POSIX-normalize. When `ctx`
    // is local or null: delegates to resolveWorkspace() so the existing local
    // behavior is byte-for-byte preserved. Returns empty on a disabled case.
    static QString resolveForContext(remote::ExecutionContext *ctx, const QString &requested);
};

#endif
