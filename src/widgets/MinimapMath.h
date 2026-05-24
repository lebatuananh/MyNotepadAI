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

#pragma once

#include <QtGlobal>

#include <algorithm>
#include <cstdint>

// Pure integer math for the minimap's y ↔ line mapping. Lives in its own
// header so unit tests don't have to drag in ScintillaNext + every widget
// dependency of EditorMinimap. Two static-inline functions, no runtime
// state.
//
// Both are deliberately branch-light and use 64-bit intermediate products
// to handle huge files (>2 billion lines × widget height before overflow).
namespace MinimapMath {

inline qint32 lineFor(int y, int widgetHeight, qint32 lineCount)
{
    if (lineCount <= 0 || widgetHeight <= 0) return 0;
    const int clamped = std::clamp(y, 0, widgetHeight - 1);
    const qint64 line = (static_cast<qint64>(clamped) * lineCount)
                        / std::max(1, widgetHeight);
    return static_cast<qint32>(std::clamp<qint64>(line, 0, lineCount - 1));
}

inline int yFor(qint32 line, int widgetHeight, qint32 lineCount)
{
    if (lineCount <= 0) return 0;
    const qint32 clamped = std::clamp<qint32>(line, 0, lineCount - 1);
    return static_cast<int>((static_cast<qint64>(clamped) * widgetHeight) / lineCount);
}

} // namespace MinimapMath
