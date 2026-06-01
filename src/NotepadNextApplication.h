/*
 * This file is part of Notepad Next.
 * Copyright 2019 Justin Dailey
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


#ifndef NOTEPADNEXTAPPLICATION_H
#define NOTEPADNEXTAPPLICATION_H

#include "ApplicationSettings.h"

#include "SingleApplication"

#include <QCommandLineParser>
#include <QPointer>
#include <QTimer>


class MainWindow;
class LuaState;
class EditorManager;
class PreviewTabManager;
class RecentFilesListManager;
class ScintillaNext;
class SessionManager;
class TranslationManager;
class AcpAgentManager;
class ScheduledTaskRunner;
namespace ai { class CommitMessageGenerator; class CredentialStore; }
namespace remote { class ExecutionContextRegistry; class SshProfileRegistry; }


class NotepadNextApplication : public SingleApplication
{
    Q_OBJECT

public:
    NotepadNextApplication(int &argc, char **argv, const QString &userData = QString());

    bool init();

    RecentFilesListManager *getRecentFilesListManager() const { return recentFilesListManager; }
    RecentFilesListManager *getRecentWorkspacesListManager() const { return recentWorkspacesListManager; }
    EditorManager *getEditorManager() const { return editorManager; }
    PreviewTabManager *getPreviewTabManager() const { return previewTabManager; }
    SessionManager *getSessionManager() const;
    TranslationManager *getTranslationManager() const { return translationManager; };
    AcpAgentManager *getAiAgentManager() const { return aiAgentManager_; }
    ScheduledTaskRunner *getScheduledTaskRunner() const { return scheduledTaskRunner_; }
    ai::CommitMessageGenerator *getCommitMessageGenerator() const { return commitMessageGenerator_; }
    ai::CredentialStore *getCredentialStore() const { return credentialStore_; }
    remote::ExecutionContextRegistry *getExecutionContextRegistry() const { return executionContextRegistry_; }
    remote::SshProfileRegistry *getSshProfileRegistry() const { return sshProfileRegistry_; }

    LuaState *getLuaState() const { return luaState; }
    QString getFileDialogFilter() const;
    QString getFileDialogFilterForLanguage(const QString &language) const;
    ApplicationSettings *getSettings() const { return settings; }

    QStringList getLanguages() const;
    void setEditorLanguage(ScintillaNext *editor, const QString &languageName) const;

    QString detectLanguage(ScintillaNext *editor) const;
    QString detectLanguageFromExtension(const QString &extension, const QString &fileName = QString()) const;
    QString detectLanguageFromContents(ScintillaNext *editor) const;

    // Editor-free language detection from a (possibly relative) file path.
    // Used by GitDiffSyntaxMapper which has no ScintillaNext to attach to.
    QString detectLanguageFromPath(const QString &relPath) const;

    // Returns the Lexilla lexer name for the given language (e.g. "cpp",
    // "python"). Empty when language is unknown or has no lexer mapping.
    QString resolveLexerName(const QString &languageName) const;

    void sendInfoToPrimaryInstance();

    bool isRunningAsAdmin() const;

    QStringList debugInfo() const;

    bool isEffectiveThemeDark() const;

signals:
    void effectiveThemeChanged();
    void gitHeadChanged();
    void gitWorkingTreeDirtied(const QString &path);

protected:
    bool event(QEvent *event) override;
#ifndef NDEBUG
    bool notify(QObject *receiver, QEvent *event) override;
#endif

private slots:
    void saveSettings();
    void receiveInfoFromSecondaryInstance(quint32 instanceId, QByteArray message);
    void saveSession();

private:
    void openFiles(const QStringList &files);

    void loadSettings();

    void applyTheme();

    EditorManager *editorManager;
    PreviewTabManager *previewTabManager = nullptr;
    RecentFilesListManager *recentFilesListManager;
    RecentFilesListManager *recentWorkspacesListManager;
    ApplicationSettings *settings;
    SessionManager *sessionManager;
    TranslationManager *translationManager;
    AcpAgentManager *aiAgentManager_ = nullptr;
    ScheduledTaskRunner *scheduledTaskRunner_ = nullptr;
    ai::CommitMessageGenerator *commitMessageGenerator_ = nullptr;
    ai::CredentialStore *credentialStore_ = nullptr;
    remote::SshProfileRegistry *sshProfileRegistry_ = nullptr;
    remote::ExecutionContextRegistry *executionContextRegistry_ = nullptr;

    LuaState *luaState = Q_NULLPTR;

    MainWindow *window = Q_NULLPTR;
    QPointer<QWidget> currentlyFocusedWidget; // Keep a weak pointer to the QWidget since we don't own it

    MainWindow *createNewWindow();

    QCommandLineParser parser;

    QTimer autoSaveTimer; //save automatically the session
};

#endif // NOTEPADNEXTAPPLICATION_H
