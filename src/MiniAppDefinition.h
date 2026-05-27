/*
 * This file is part of NotepadAI.
 * Copyright 2024 NotepadAI contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QString>

struct MiniAppDefinition
{
    QString id;               // UUID string
    QString name;             // Display name (required)
    QString url;              // http/https URL (required)
    QString command;          // Shell command to spawn (optional)
    QString env;              // KEY=VALUE lines (optional)
    QString cwd;              // Working directory (optional)
    QString icon;             // Reserved for future custom icon path
    QString healthCheckUrl;   // Health poll URL (defaults to url if empty)
    int healthTimeoutMs = 30000; // Timeout in ms (range 5000-300000)
    int debugPort = 0;           // CDP debug port (0 = disabled, 1-65535 = enabled)
    bool autoKillOnClose = true; // Kill process on tab close

    bool isValid() const { return !name.isEmpty() && !url.isEmpty(); }

    QString effectiveHealthUrl() const
    {
        return healthCheckUrl.isEmpty() ? url : healthCheckUrl;
    }
};
