#ifndef PREVIEWTABMANAGER_H
#define PREVIEWTABMANAGER_H

#include <QObject>
#include <QHash>
#include <QPointer>
#include <QTimer>
#include <functional>

#include "PreviewContentWidget.h"
#include "DockWidget.h"

class ScintillaNext;
class NotepadNextApplication;
class DockedEditor;

class PreviewTabManager : public QObject
{
    Q_OBJECT

public:
    struct TypeRegistration {
        QStringList extensions;
        QString iconPath;
        std::function<PreviewContentWidget *(QWidget *parent)> factory;
        // When true, openPreviewFromFile calls PreviewContentWidget::loadFromFile
        // (the widget owns its mmap + size cap) and SKIPS the QByteArray-read +
        // decode + setContent path and the 10 MB cap. Markdown/HTML leave this
        // false and keep the existing decoded-text route.
        bool wantsFilePath = false;
        // Human-readable type label for the toolbar tooltip ("Markdown", "HTML",
        // "CSV", "TSV"). Empty falls back to a generic "Preview" label.
        QString displayName;
        // Scintilla language names (e.g. "Markdown", "HTML") that map to this
        // preview type even when the buffer is UNSAVED and has no file extension.
        // The extension match wins; this is the fallback so an unsaved Markdown
        // buffer with the Markdown lexer stays previewable. wantsFilePath types
        // (CSV/TSV) deliberately leave this empty — they need a real on-disk path.
        QStringList languageNames;
    };

    explicit PreviewTabManager(NotepadNextApplication *app, DockedEditor *dockedEditor, QObject *parent = nullptr);

    void registerType(const QString &typeId, const TypeRegistration &reg);
    bool canPreview(const QString &filePath) const;
    // True when filePath's preview type uses the file-path (mmap) route, which
    // enforces its own multi-GB cap inside loadFromFile — callers must NOT apply
    // the 10 MB decoded-text pre-check to these. False for unknown types too.
    bool previewWantsFilePath(const QString &filePath) const;

    // Editor-aware preview gating, used by the toolbar/menu action. Resolves the
    // active editor to a registered preview type (extension first, Scintilla
    // language name as fallback for unsaved buffers) and exposes the result for
    // the action's enabled-state, dynamic tooltip, and dispatch — so all three
    // read from ONE resolver and can never disagree.
    bool canPreviewEditor(ScintillaNext *editor) const;
    // Human-readable type label for the active editor ("Markdown"/"HTML"/"CSV"/
    // "TSV"), or empty when unknown/unsupported. Drives the dynamic tooltip.
    QString previewTypeName(ScintillaNext *editor) const;

    void openOrFocusPreview(ScintillaNext *sourceEditor);
    void openPreviewFromFile(const QString &filePath);
    void closePreview(ScintillaNext *sourceEditor);
    PreviewContentWidget *previewForEditor(ScintillaNext *sourceEditor) const;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

signals:
    void previewOpened(PreviewContentWidget *preview);
    void previewClosed(PreviewContentWidget *preview);

private:
    struct PreviewEntry {
        QPointer<PreviewContentWidget> widget;
        QPointer<ads::CDockWidget> dockWidget;
        QString iconPath;
        QTimer *debounceTimer = nullptr;
    };

    const TypeRegistration *findRegistration(const QString &filePath) const;
    // Resolves an editor to its preview type: file-extension match first, then
    // Scintilla language-name fallback for unsaved buffers. Returns nullptr for
    // a wantsFilePath type on a buffer with no on-disk path (CSV/TSV need mmap),
    // so the gate, tooltip, and dispatch all reject it consistently.
    const TypeRegistration *registrationForEditor(ScintillaNext *editor) const;
    void scheduleRender(ScintillaNext *sourceEditor);
    void performRender(ScintillaNext *sourceEditor);
    void syncScroll(ScintillaNext *sourceEditor);
    int debounceMs(int docLength) const;
    void pinTransientPreviewTab();

    NotepadNextApplication *m_app;
    DockedEditor *m_dockedEditor;
    QHash<QString, TypeRegistration> m_registry;
    QHash<ScintillaNext *, PreviewEntry> m_previews;
    QHash<QString, QPointer<ads::CDockWidget>> m_previewsByPath;
    QPointer<ads::CDockWidget> m_transientPreviewTab;
};

#endif // PREVIEWTABMANAGER_H
