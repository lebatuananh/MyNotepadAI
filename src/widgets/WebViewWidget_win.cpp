/*
 * This file is part of NotepadAI.
 * Copyright 2024 NotepadAI contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "WebViewWidget.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QWindow>

#include <atomic>
#include <memory>

#include <windows.h>
#include <WebView2.h>

// Load CreateCoreWebView2EnvironmentWithOptions dynamically so we don't
// depend on WebView2LoaderStatic.lib (MSVC-only). The function lives in
// WebView2Loader.dll which ships with the WebView2 Runtime.
using CreateEnvironmentFn = HRESULT(STDAPICALLTYPE *)(
    PCWSTR, PCWSTR, IUnknown *,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *);

static CreateEnvironmentFn resolveCreateEnvironment()
{
    static CreateEnvironmentFn fn = []() -> CreateEnvironmentFn {
        HMODULE mod = LoadLibraryW(L"WebView2Loader.dll");
        if (!mod) return nullptr;
        return reinterpret_cast<CreateEnvironmentFn>(
            GetProcAddress(mod, "CreateCoreWebView2EnvironmentWithOptions"));
    }();
    return fn;
}

// WebView2 Windows implementation using the COM SDK directly.
// Async initialization: HWND → Environment → Controller → WebView → Navigate.
class WebViewWidgetWin : public WebViewWidget
{

public:
    WebViewWidgetWin(const QString &appId, const QUrl &url, int debugPort, QWidget *parent)
        : WebViewWidget(appId, url, parent)
        , m_debugPort(debugPort)
    {
        m_hostWidget = new QWidget(this);
        m_hostWidget->setAttribute(Qt::WA_NativeWindow);
        m_hostWidget->setAttribute(Qt::WA_DontCreateNativeAncestors, false);
        mainLayout()->addWidget(m_hostWidget, 1);
    }

    ~WebViewWidgetWin() override
    {
        destroy();
    }

    void initialize() override
    {
        if (m_initStarted) return;
        m_initStarted = true;
        m_hwnd = reinterpret_cast<HWND>(m_hostWidget->winId());
        m_dbgInitCalled = true;
        initWebView2();
    }

    QString debugInfo() const override
    {
        QString info;
        info += QStringLiteral("--- WebView2 Win Debug ---\n");
        info += QStringLiteral("initCalled: %1\n").arg(m_dbgInitCalled ? "true" : "false");
        info += QStringLiteral("HWND: 0x%1\n").arg(reinterpret_cast<quintptr>(m_hwnd), 0, 16);
        info += QStringLiteral("hostWidget visible: %1\n").arg(m_hostWidget ? (m_hostWidget->isVisible() ? "true" : "false") : "null");
        info += QStringLiteral("hostWidget size: %1x%2\n").arg(m_hostWidget ? m_hostWidget->width() : 0).arg(m_hostWidget ? m_hostWidget->height() : 0);
        info += QStringLiteral("loaderResolved: %1\n").arg(m_dbgLoaderResolved ? "true" : "false");
        info += QStringLiteral("createEnvHR: 0x%1\n").arg(static_cast<unsigned>(m_dbgCreateEnvHr), 8, 16, QLatin1Char('0'));
        info += QStringLiteral("envCreated: %1\n").arg(m_environment ? "true" : "false");
        info += QStringLiteral("envCallbackHR: 0x%1\n").arg(static_cast<unsigned>(m_dbgEnvCallbackHr), 8, 16, QLatin1Char('0'));
        info += QStringLiteral("controllerCreated: %1\n").arg(m_controller ? "true" : "false");
        info += QStringLiteral("ctrlCallbackHR: 0x%1\n").arg(static_cast<unsigned>(m_dbgCtrlCallbackHr), 8, 16, QLatin1Char('0'));
        info += QStringLiteral("webViewReady: %1\n").arg(m_webView ? "true" : "false");
        info += QStringLiteral("navigateCalled: %1\n").arg(m_dbgNavigateCalled ? "true" : "false");
        info += QStringLiteral("navCompleted: %1\n").arg(m_dbgNavCompleted ? "true" : "false");
        info += QStringLiteral("userDataFolder: %1\n").arg(m_dbgUserDataFolder);
        return info;
    }

    void navigate(const QUrl &url) override
    {
        if (!m_webView) return;
        setLoading(true);
        m_webView->Navigate(url.toString().toStdWString().c_str());
    }

    void reload() override
    {
        if (!m_webView) return;
        setLoading(true);
        m_webView->Reload();
    }

    void stop() override
    {
        if (!m_webView) return;
        m_webView->Stop();
        setLoading(false);
    }

    void destroy() override
    {
        m_alive->store(false, std::memory_order_release);

        if (m_cdpPollTimer) {
            m_cdpPollTimer->stop();
            m_cdpPollTimer->deleteLater();
            m_cdpPollTimer = nullptr;
        }
        if (m_cdpNam) {
            m_cdpNam->deleteLater();
            m_cdpNam = nullptr;
        }

        if (m_controller) {
            m_controller->Close();
            m_controller->Release();
            m_controller = nullptr;
        }
        if (m_webView) {
            m_webView->Release();
            m_webView = nullptr;
        }
        if (m_environment) {
            m_environment->Release();
            m_environment = nullptr;
        }
        hideCdpUrl();
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        if (m_controller && m_hostWidget) {
            RECT bounds;
            bounds.left = 0;
            bounds.top = 0;
            bounds.right = m_hostWidget->width();
            bounds.bottom = m_hostWidget->height();
            m_controller->put_Bounds(bounds);
        }
    }

private:
    void initWebView2()
    {
        const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        m_dbgUserDataFolder = QDir::toNativeSeparators(
            appData + QStringLiteral("/MiniApps/") + appId());

        auto createEnv = resolveCreateEnvironment();
        m_dbgLoaderResolved = (createEnv != nullptr);
        if (!createEnv) {
            emit navigationCompleted(false, tr("WebView2Loader.dll not found. Please install Microsoft Edge WebView2 Runtime."));
            return;
        }

        // Pass debug port via environment variable — more reliable than COM options
        // object across different WebView2 runtime versions. The runtime reads this
        // env var synchronously during CreateCoreWebView2EnvironmentWithOptions.
        QByteArray prevEnv;
        bool hadEnv = false;
        if (m_debugPort > 0) {
            const QByteArray envName = "WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS";
            prevEnv = qgetenv(envName.constData());
            hadEnv = !prevEnv.isNull();
            qputenv(envName.constData(),
                    QStringLiteral("--remote-debugging-port=%1").arg(m_debugPort).toUtf8());
        }

        HRESULT hr = createEnv(
            nullptr,
            m_dbgUserDataFolder.toStdWString().c_str(),
            nullptr,
            new EnvironmentCompletedHandler(this));

        // Restore environment immediately — the runtime has already read it
        if (m_debugPort > 0) {
            const QByteArray envName = "WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS";
            if (hadEnv)
                qputenv(envName.constData(), prevEnv);
            else
                qunsetenv(envName.constData());
        }

        m_dbgCreateEnvHr = hr;
        if (FAILED(hr)) {
            emit navigationCompleted(false, tr("Failed to create WebView2 environment (0x%1)")
                                                .arg(static_cast<unsigned>(hr), 8, 16, QLatin1Char('0')));
        }
    }

    void onEnvironmentCreated(HRESULT hr, ICoreWebView2Environment *env)
    {
        m_dbgEnvCallbackHr = hr;
        if (FAILED(hr) || !env) {
            emit navigationCompleted(false, tr("WebView2 Runtime not available (0x%1)")
                                                .arg(static_cast<unsigned>(hr), 8, 16, QLatin1Char('0')));
            return;
        }
        m_environment = env;
        m_environment->AddRef();

        // Start CDP discovery polling if debug port is configured
        if (m_debugPort > 0)
            startCdpDiscovery();

        env->CreateCoreWebView2Controller(
            m_hwnd,
            new ControllerCompletedHandler(this));
    }

    void onControllerCreated(HRESULT hr, ICoreWebView2Controller *controller)
    {
        m_dbgCtrlCallbackHr = hr;
        if (FAILED(hr) || !controller) {
            emit navigationCompleted(false, tr("Failed to create WebView2 controller (0x%1)")
                                                .arg(static_cast<unsigned>(hr), 8, 16, QLatin1Char('0')));
            return;
        }
        m_controller = controller;
        m_controller->AddRef();

        m_controller->get_CoreWebView2(&m_webView);
        if (!m_webView) {
            emit navigationCompleted(false, tr("Failed to get WebView2 core"));
            return;
        }

        // Configure settings
        ICoreWebView2Settings *settings = nullptr;
        m_webView->get_Settings(&settings);
        if (settings) {
            settings->put_IsStatusBarEnabled(FALSE);
            settings->put_AreDefaultContextMenusEnabled(TRUE);
            settings->put_AreDevToolsEnabled(TRUE);
            settings->Release();
        }

        // Set initial bounds
        RECT bounds;
        bounds.left = 0;
        bounds.top = 0;
        bounds.right = m_hostWidget->width();
        bounds.bottom = m_hostWidget->height();
        m_controller->put_Bounds(bounds);

        // Subscribe to NavigationCompleted
        EventRegistrationToken navToken;
        m_webView->add_NavigationCompleted(
            new NavigationCompletedHandler(this), &navToken);

        // Subscribe to ProcessFailed
        EventRegistrationToken procToken;
        m_webView->add_ProcessFailed(
            new ProcessFailedHandler(this), &procToken);

        // Navigate to initial URL
        setLoading(true);
        m_dbgNavigateCalled = true;
        m_webView->Navigate(initialUrl().toString().toStdWString().c_str());
    }

    // --- Lightweight COM callback implementations (prevent DLL ref-counting) ---

    // EnvironmentCompletedHandler: called when CreateCoreWebView2EnvironmentWithOptions finishes.
    struct EnvironmentCompletedHandler : ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler {
        WebViewWidgetWin *owner;
        std::shared_ptr<std::atomic<bool>> alive;
        ULONG refCount = 1;
        EnvironmentCompletedHandler(WebViewWidgetWin *o) : owner(o), alive(o->m_alive) {}
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
            if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler)) {
                *ppv = this; AddRef(); return S_OK;
            }
            *ppv = nullptr; return E_NOINTERFACE;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return ++refCount; }
        ULONG STDMETHODCALLTYPE Release() override { if (--refCount == 0) { delete this; return 0; } return refCount; }
        HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, ICoreWebView2Environment *env) override {
            if (!alive->load(std::memory_order_acquire)) return S_OK;
            owner->onEnvironmentCreated(hr, env);
            return S_OK;
        }
    };

    // ControllerCompletedHandler: called when CreateCoreWebView2Controller finishes.
    struct ControllerCompletedHandler : ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
        WebViewWidgetWin *owner;
        std::shared_ptr<std::atomic<bool>> alive;
        ULONG refCount = 1;
        ControllerCompletedHandler(WebViewWidgetWin *o) : owner(o), alive(o->m_alive) {}
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
            if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler)) {
                *ppv = this; AddRef(); return S_OK;
            }
            *ppv = nullptr; return E_NOINTERFACE;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return ++refCount; }
        ULONG STDMETHODCALLTYPE Release() override { if (--refCount == 0) { delete this; return 0; } return refCount; }
        HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, ICoreWebView2Controller *ctrl) override {
            if (!alive->load(std::memory_order_acquire)) return S_OK;
            owner->onControllerCreated(hr, ctrl);
            return S_OK;
        }
    };

    // NavigationCompletedHandler: fires when a navigation finishes.
    struct NavigationCompletedHandler : ICoreWebView2NavigationCompletedEventHandler {
        WebViewWidgetWin *owner;
        std::shared_ptr<std::atomic<bool>> alive;
        ULONG refCount = 1;
        NavigationCompletedHandler(WebViewWidgetWin *o) : owner(o), alive(o->m_alive) {}
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
            if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ICoreWebView2NavigationCompletedEventHandler)) {
                *ppv = this; AddRef(); return S_OK;
            }
            *ppv = nullptr; return E_NOINTERFACE;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return ++refCount; }
        ULONG STDMETHODCALLTYPE Release() override { if (--refCount == 0) { delete this; return 0; } return refCount; }
        HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *, ICoreWebView2NavigationCompletedEventArgs *args) override {
            if (!alive->load(std::memory_order_acquire)) return S_OK;
            BOOL success = FALSE;
            if (args) args->get_IsSuccess(&success);
            owner->m_dbgNavCompleted = true;
            owner->setLoading(false);
            emit owner->navigationCompleted(success, success ? QString() : QStringLiteral("Navigation failed"));
            return S_OK;
        }
    };

    // ProcessFailedHandler: fires when the renderer crashes.
    struct ProcessFailedHandler : ICoreWebView2ProcessFailedEventHandler {
        WebViewWidgetWin *owner;
        std::shared_ptr<std::atomic<bool>> alive;
        ULONG refCount = 1;
        ProcessFailedHandler(WebViewWidgetWin *o) : owner(o), alive(o->m_alive) {}
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
            if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ICoreWebView2ProcessFailedEventHandler)) {
                *ppv = this; AddRef(); return S_OK;
            }
            *ppv = nullptr; return E_NOINTERFACE;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return ++refCount; }
        ULONG STDMETHODCALLTYPE Release() override { if (--refCount == 0) { delete this; return 0; } return refCount; }
        HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *, ICoreWebView2ProcessFailedEventArgs *args) override {
            if (!alive->load(std::memory_order_acquire)) return S_OK;
            COREWEBVIEW2_PROCESS_FAILED_KIND kind = COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED;
            if (args) args->get_ProcessFailedKind(&kind);
            QString desc;
            switch (kind) {
            case COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED:
                desc = QStringLiteral("Browser process exited"); break;
            case COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED:
                desc = QStringLiteral("Renderer process exited"); break;
            case COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE:
                desc = QStringLiteral("Renderer process unresponsive"); break;
            default:
                desc = QStringLiteral("Process failed"); break;
            }
            emit owner->processFailed(desc);
            return S_OK;
        }
    };

    // --- CDP Discovery ---

    void startCdpDiscovery()
    {
        m_cdpNam = new QNetworkAccessManager(this);
        m_cdpPollCount = 0;
        m_cdpPollTimer = new QTimer(this);
        m_cdpPollTimer->setInterval(100);
        connect(m_cdpPollTimer, &QTimer::timeout, this, [this]() { onCdpPollTick(); });
        m_cdpPollTimer->start();
    }

    void onCdpPollTick()
    {
        if (++m_cdpPollCount > 50) {
            // Timeout: 50 * 100ms = 5 seconds
            m_cdpPollTimer->stop();
            qWarning("WebViewWidgetWin: CDP discovery timed out on port %d", m_debugPort);
            return;
        }

        const QString url = QStringLiteral("http://127.0.0.1:%1/json/version").arg(m_debugPort);
        QNetworkRequest req{QUrl(url)};
        req.setTransferTimeout(500);
        QNetworkReply *reply = m_cdpNam->get(req);
        connect(reply, &QNetworkReply::finished, this, [this, reply]() {
            reply->deleteLater();
            if (!m_cdpPollTimer || !m_cdpPollTimer->isActive())
                return;

            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (status == 200) {
                m_cdpPollTimer->stop();
                const QByteArray body = reply->readAll();
                const QJsonDocument doc = QJsonDocument::fromJson(body);
                const QString wsUrl = doc.object().value(QStringLiteral("webSocketDebuggerUrl")).toString();
                const QString httpUrl = QStringLiteral("http://127.0.0.1:%1").arg(m_debugPort);
                showCdpUrl(httpUrl);
                emit cdpReady(httpUrl, wsUrl);
            }
            // else: keep polling on next timer tick
        });
    }

    QWidget *m_hostWidget = nullptr;
    HWND m_hwnd = nullptr;
    bool m_initStarted = false;
    ICoreWebView2Environment *m_environment = nullptr;
    ICoreWebView2Controller *m_controller = nullptr;
    ICoreWebView2 *m_webView = nullptr;

    // Shared flag for COM callback safety: handlers hold a copy and check
    // before dereferencing `owner`. Set to false in destroy() before releasing
    // COM objects so late-firing callbacks see the widget as gone.
    std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);

    // Debug state
    bool m_dbgInitCalled = false;
    bool m_dbgLoaderResolved = false;
    bool m_dbgNavigateCalled = false;
    bool m_dbgNavCompleted = false;
    HRESULT m_dbgCreateEnvHr = S_OK;
    HRESULT m_dbgEnvCallbackHr = S_OK;
    HRESULT m_dbgCtrlCallbackHr = S_OK;
    QString m_dbgUserDataFolder;

    // CDP debug port
    int m_debugPort = 0;
    QNetworkAccessManager *m_cdpNam = nullptr;
    QTimer *m_cdpPollTimer = nullptr;
    int m_cdpPollCount = 0;
};

// Factory: Windows implementation
WebViewWidget *WebViewWidget::create(const QString &appId, const QUrl &url, int debugPort, QWidget *parent)
{
    return new WebViewWidgetWin(appId, url, debugPort, parent);
}
