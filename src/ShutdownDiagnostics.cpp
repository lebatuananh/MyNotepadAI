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

/*
 * Shutdown diagnostics for the debug build.
 *
 * On clean shutdown (aboutToQuit), writes shutdown_report.txt containing:
 *   - Header (timestamp, app version, pid, uptime)
 *   - Memory stats (working set / private bytes / peak working set on Windows;
 *     RSS / peak RSS on POSIX)
 *   - QObject census (per-class instance counts, candidate leak rows)
 *   - Slow-event log (top events whose dispatch exceeded the threshold)
 *   - PROFILE_SCOPE aggregate table (per-tag totals)
 *
 * Path policy: same as CrashHandler — primary is cwd, fallback is
 * %APPDATA%/NotepadAI on Windows or $XDG_DATA_HOME/NotepadAI on POSIX.
 * This is intentionally a duplicate of CrashHandler's resolver (Qt API here
 * vs raw OS APIs there) because the crash handler is signal-safety-constrained
 * while we are free to use QStandardPaths/QDir. Keep both sites in sync if
 * the policy changes.
 *
 * Gating: g_collectionEnabled flips off when the user toggles
 * "Help -> Debug -> Shutdown Diagnostics" off; the hot path (notify() override)
 * checks this atomic before any work.
 *
 * Compile-out: the whole module is wrapped in #ifndef NDEBUG; Release builds
 * see only empty inline stubs (see header).
 */

#ifndef NDEBUG

#include "ShutdownDiagnostics.h"
#include "ProfileScope.h"

#include <atomic>
#include <chrono>

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMetaEnum>
#include <QMetaObject>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTemporaryFile>
#include <QTextStream>
#include <QVector>
#include <QWidget>
#include <QWindow>

#include "DataPaths.h"

#ifdef Q_OS_WIN
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <psapi.h>
#  pragma comment(lib, "psapi.lib")
#else
#  include <sys/resource.h>
#  include <unistd.h>
#endif

namespace ShutdownDiagnostics {

namespace {

// Collection toggle. Default = true. Gates the notify() hot path.
std::atomic<bool> g_collectionEnabled{true};

// Threshold for "slow event". Default = 16 ms (one 60 Hz frame).
std::atomic<uint64_t> g_slowEventThresholdNs{16ULL * 1000ULL * 1000ULL};

// Process startup time.
std::chrono::steady_clock::time_point g_startupTime;

// Slow event log. Pre-aggregated by event-type to bound memory at < 1 KB.
// 23 whitelisted event types -> 23 rows max.
struct EventStat {
    int type = 0;
    uint64_t totalCount = 0;
    uint64_t slowCount = 0;
    uint64_t totalNs = 0;
    uint64_t maxNs = 0;
};

// Indexed by event type id (small int, max value bounded by Qt's enum range).
// Use QHash<int, EventStat> for sparse storage.
QHash<int, EventStat> &eventStats()
{
    static QHash<int, EventStat> s;
    return s;
}

QMutex &eventStatsMutex()
{
    static QMutex m;
    return m;
}

// Per-receiver slow-paint aggregation. Key = className + '#' + objectName so
// e.g. two ScintillaEdit instances with different objectName roll up separately.
// Bounded by ~widget count of the running app, typically <200 entries.
struct SlowPaintStat {
    QString className;
    QString objectName;
    uint64_t count = 0;
    uint64_t totalNs = 0;
    uint64_t maxNs = 0;
};

QHash<QString, SlowPaintStat> &slowPaintStats()
{
    static QHash<QString, SlowPaintStat> s;
    return s;
}

QMutex &slowPaintStatsMutex()
{
    static QMutex m;
    return m;
}

// Threshold for "slow paint" capture in the per-widget table. Independent of
// the event-row slowCount threshold so the paint table stays focused on
// genuinely expensive paints regardless of how the user tunes the event one.
constexpr uint64_t kSlowPaintThresholdNs = 50ULL * 1000ULL * 1000ULL; // 50 ms

// Whitelist of event types we record. Anything else is fast-path skipped.
const QSet<int> &whitelistedTypes()
{
    static const QSet<int> s {
        QEvent::Timer,
        QEvent::MouseButtonPress,
        QEvent::MouseButtonRelease,
        QEvent::MouseButtonDblClick,
        QEvent::MouseMove,
        QEvent::KeyPress,
        QEvent::KeyRelease,
        QEvent::FocusIn,
        QEvent::FocusOut,
        QEvent::Enter,
        QEvent::Leave,
        QEvent::Paint,
        QEvent::Resize,
        QEvent::Show,
        QEvent::Hide,
        QEvent::Close,
        QEvent::Wheel,
        QEvent::ContextMenu,
        QEvent::DeferredDelete,
        QEvent::ChildAdded,
        QEvent::ChildRemoved,
        QEvent::MetaCall,
        QEvent::UpdateRequest,
    };
    return s;
}

QString eventTypeName(int type)
{
    static const QMetaEnum meta = QMetaEnum::fromType<QEvent::Type>();
    const char *key = meta.valueToKey(type);
    if (key) return QString::fromLatin1(key);
    return QStringLiteral("Type(%1)").arg(type);
}

// Path resolver (Qt-based mirror of CrashHandler's raw-OS resolver).
QString primaryPath()
{
    return QDir::current().absoluteFilePath(QStringLiteral("shutdown_report.txt"));
}

QString fallbackPath()
{
    const QString &base = DataPaths::appDataLocation();
    QDir().mkpath(base);
    return QDir(base).absoluteFilePath(QStringLiteral("shutdown_report.txt"));
}

// Try to open a file for writing; uses QTemporaryFile probe to dodge the
// Windows ACL false-negatives in QFileInfo::isWritable().
bool isPathWritable(const QString &filePath)
{
    const QString dir = QFileInfo(filePath).absolutePath();
    if (!QDir(dir).exists()) {
        return false;
    }
    QTemporaryFile probe(dir + QStringLiteral("/.shutdown_probe_XXXXXX"));
    if (!probe.open()) {
        return false;
    }
    probe.remove();
    return true;
}

QString resolveReportPath()
{
    QString primary = primaryPath();
    if (isPathWritable(primary)) {
        return primary;
    }
    return fallbackPath();
}

} // namespace

namespace Detail {

void recordEvent(QObject *receiver, QEvent *event, qint64 ns)
{
    if (!g_collectionEnabled.load(std::memory_order_relaxed)) {
        return;
    }

    const int type = static_cast<int>(event->type());
    if (!whitelistedTypes().contains(type)) {
        return;
    }

    const uint64_t threshold = g_slowEventThresholdNs.load(std::memory_order_relaxed);
    const uint64_t uns = static_cast<uint64_t>(ns < 0 ? 0 : ns);

    {
        QMutexLocker lock(&eventStatsMutex());
        EventStat &s = eventStats()[type];
        s.type = type;
        s.totalCount++;
        s.totalNs += uns;
        if (uns > s.maxNs) s.maxNs = uns;
        if (uns >= threshold) s.slowCount++;
    }

    // Per-receiver paint attribution. Only Paint events qualify here, and only
    // when the call exceeded a hard 50 ms threshold — otherwise the table would
    // grow huge for trivial paints. receiver is rarely null in practice, but we
    // still guard since some Qt internals send events to deleted observers.
    if (receiver && type == QEvent::Paint && uns >= kSlowPaintThresholdNs) {
        const QMetaObject *mo = receiver->metaObject();
        const QString cls = mo ? QString::fromLatin1(mo->className()) : QStringLiteral("(null)");
        const QString name = receiver->objectName();
        const QString key = cls + QLatin1Char('#') + name;

        QMutexLocker lock(&slowPaintStatsMutex());
        SlowPaintStat &p = slowPaintStats()[key];
        if (p.count == 0) {
            p.className = cls;
            p.objectName = name;
        }
        p.count++;
        p.totalNs += uns;
        if (uns > p.maxNs) p.maxNs = uns;
    }
}

} // namespace Detail

void install()
{
    g_startupTime = std::chrono::steady_clock::now();
}

void setCollectionEnabled(bool enabled)
{
    g_collectionEnabled.store(enabled, std::memory_order_relaxed);
}

bool isCollectionEnabled()
{
    return g_collectionEnabled.load(std::memory_order_relaxed);
}

void setSlowEventThresholdMs(int ms)
{
    if (ms < 1) ms = 1;
    g_slowEventThresholdNs.store(static_cast<uint64_t>(ms) * 1000ULL * 1000ULL,
                                  std::memory_order_relaxed);
}

namespace {

// Census walker: dedupe via QSet, traverse from top-level widgets/windows and
// qApp children. Returns per-class counts. Limitation: only finds QObjects
// reachable from those three roots — orphaned QObjects (parented to nothing)
// will be missed. This is documented and accepted.
QHash<QString, int> objectCensus()
{
    QHash<QString, int> counts;
    QSet<QObject *> seen;

    auto walk = [&](QObject *root) {
        if (!root || seen.contains(root)) return;
        seen.insert(root);
        const QMetaObject *mo = root->metaObject();
        counts[QString::fromLatin1(mo->className())]++;
        const QList<QObject *> kids = root->findChildren<QObject *>();
        for (QObject *kid : kids) {
            if (seen.contains(kid)) continue;
            seen.insert(kid);
            counts[QString::fromLatin1(kid->metaObject()->className())]++;
        }
    };

    if (QApplication *app = qApp) {
        walk(app);
        const QWidgetList tops = QApplication::topLevelWidgets();
        for (QWidget *w : tops) walk(w);
        const QList<QWindow *> wins = QApplication::topLevelWindows();
        for (QWindow *w : wins) walk(w);
    }

    return counts;
}

MemoryStats sampleMemoryStats()
{
    MemoryStats m{};
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS_EX pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                              reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc),
                              sizeof(pmc))) {
        m.workingSetBytes = pmc.WorkingSetSize;
        m.peakWorkingSetBytes = pmc.PeakWorkingSetSize;
        m.privateBytes = pmc.PrivateUsage;
    }
#else
    struct rusage ru{};
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
        // ru_maxrss is in KB on Linux, bytes on macOS. Treat as KB for parity.
#  ifdef __APPLE__
        m.peakWorkingSetBytes = static_cast<uint64_t>(ru.ru_maxrss);
#  else
        m.peakWorkingSetBytes = static_cast<uint64_t>(ru.ru_maxrss) * 1024ULL;
#  endif
        m.workingSetBytes = m.peakWorkingSetBytes;
    }
#endif
    return m;
}

QString humanBytes(uint64_t b)
{
    if (b == 0) return QStringLiteral("0 B");
    const double kb = static_cast<double>(b) / 1024.0;
    if (kb < 1024.0) return QStringLiteral("%1 KB").arg(kb, 0, 'f', 1);
    const double mb = kb / 1024.0;
    if (mb < 1024.0) return QStringLiteral("%1 MB").arg(mb, 0, 'f', 1);
    const double gb = mb / 1024.0;
    return QStringLiteral("%1 GB").arg(gb, 0, 'f', 2);
}

QString humanNs(uint64_t ns)
{
    if (ns < 1000ULL) return QStringLiteral("%1 ns").arg(ns);
    const double us = static_cast<double>(ns) / 1000.0;
    if (us < 1000.0) return QStringLiteral("%1 us").arg(us, 0, 'f', 1);
    const double ms = us / 1000.0;
    if (ms < 1000.0) return QStringLiteral("%1 ms").arg(ms, 0, 'f', 2);
    const double s = ms / 1000.0;
    return QStringLiteral("%1 s").arg(s, 0, 'f', 3);
}

} // namespace

DiagnosticReportData collect()
{
    DiagnosticReportData data;
    data.timestamp = QDateTime::currentDateTime();
#ifdef APP_VERSION
    data.appVersion = QStringLiteral(APP_VERSION);
#else
    data.appVersion = QStringLiteral("unknown");
#endif
#ifdef Q_OS_WIN
    data.pid = static_cast<qint64>(GetCurrentProcessId());
#else
    data.pid = static_cast<qint64>(::getpid());
#endif
    const auto now = std::chrono::steady_clock::now();
    data.uptimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - g_startupTime).count();

    data.memory = sampleMemoryStats();

    // QObject census -> sort desc by count.
    const QHash<QString, int> raw = objectCensus();
    data.objectCensus.reserve(raw.size());
    for (auto it = raw.constBegin(); it != raw.constEnd(); ++it) {
        data.objectCensus.push_back({it.key(), it.value()});
    }
    std::sort(data.objectCensus.begin(), data.objectCensus.end(),
              [](const ObjectCount &a, const ObjectCount &b) {
                  return a.count > b.count;
              });

    // Slow events -> filter rows with slowCount > 0, sort desc by maxNs.
    {
        QMutexLocker lock(&eventStatsMutex());
        for (auto it = eventStats().constBegin(); it != eventStats().constEnd(); ++it) {
            const EventStat &s = it.value();
            if (s.totalCount == 0) continue;
            EventRow row;
            row.typeName = eventTypeName(s.type);
            row.totalCount = s.totalCount;
            row.slowCount = s.slowCount;
            row.totalNs = s.totalNs;
            row.maxNs = s.maxNs;
            data.eventRows.push_back(std::move(row));
        }
    }
    std::sort(data.eventRows.begin(), data.eventRows.end(),
              [](const EventRow &a, const EventRow &b) {
                  return a.maxNs > b.maxNs;
              });

    // PROFILE_SCOPE aggregates.
    const auto samples = ProfileScope::snapshot();
    data.profileRows.reserve(samples.size());
    for (const auto &s : samples) {
        ProfileRow row;
        row.tag = s.tag;
        row.callCount = s.callCount;
        row.totalNs = s.totalNs;
        row.maxNs = s.maxNs;
        data.profileRows.push_back(std::move(row));
    }

    // Slow paints -> sort desc by maxNs.
    {
        QMutexLocker lock(&slowPaintStatsMutex());
        data.slowPaints.reserve(slowPaintStats().size());
        for (auto it = slowPaintStats().constBegin(); it != slowPaintStats().constEnd(); ++it) {
            const SlowPaintStat &p = it.value();
            SlowPaintRow row;
            row.className = p.className;
            row.objectName = p.objectName;
            row.count = p.count;
            row.totalNs = p.totalNs;
            row.maxNs = p.maxNs;
            data.slowPaints.push_back(std::move(row));
        }
    }
    std::sort(data.slowPaints.begin(), data.slowPaints.end(),
              [](const SlowPaintRow &a, const SlowPaintRow &b) {
                  return a.maxNs > b.maxNs;
              });

    return data;
}

QString formatReport(const DiagnosticReportData &d)
{
    QString out;
    QTextStream s(&out);

    s << "===== SHUTDOWN DIAGNOSTICS =====\n";
    s << "Timestamp:    " << d.timestamp.toString(Qt::ISODate) << "\n";
    s << "App version:  " << d.appVersion << "\n";
    s << "PID:          " << d.pid << "\n";
    s << "Uptime:       " << d.uptimeMs << " ms\n";
    s << "\n";

    s << "----- Memory -----\n";
    s << "Working set:        " << humanBytes(d.memory.workingSetBytes) << "\n";
    s << "Peak working set:   " << humanBytes(d.memory.peakWorkingSetBytes) << "\n";
#ifdef Q_OS_WIN
    s << "Private bytes:      " << humanBytes(d.memory.privateBytes) << "\n";
#endif
    s << "\n";

    s << "----- QObject census (top 30 by count) -----\n";
    s << QString::asprintf("%-50s %8s\n", "Class", "Count");
    int shown = 0;
    for (const auto &o : d.objectCensus) {
        if (shown >= 30) break;
        s << QString::asprintf("%-50s %8d\n",
                                o.className.left(50).toUtf8().constData(),
                                o.count);
        ++shown;
    }
    s << "Total classes: " << d.objectCensus.size() << "\n";
    s << "\n";

    s << "----- Slow events (sorted by max latency) -----\n";
    s << QString::asprintf("%-28s %10s %10s %14s %14s\n",
                           "Type", "Count", "Slow", "Total", "Max");
    for (const auto &e : d.eventRows) {
        s << QString::asprintf("%-28s %10llu %10llu %14s %14s\n",
                                e.typeName.toUtf8().constData(),
                                static_cast<unsigned long long>(e.totalCount),
                                static_cast<unsigned long long>(e.slowCount),
                                humanNs(e.totalNs).toUtf8().constData(),
                                humanNs(e.maxNs).toUtf8().constData());
    }
    s << "\n";

    s << "----- PROFILE_SCOPE aggregates -----\n";
    s << QString::asprintf("%-40s %10s %14s %14s %14s\n",
                           "Tag", "Calls", "Total", "Avg", "Max");
    for (const auto &p : d.profileRows) {
        const uint64_t avg = p.callCount > 0 ? p.totalNs / p.callCount : 0;
        s << QString::asprintf("%-40s %10llu %14s %14s %14s\n",
                                p.tag.left(40).toUtf8().constData(),
                                static_cast<unsigned long long>(p.callCount),
                                humanNs(p.totalNs).toUtf8().constData(),
                                humanNs(avg).toUtf8().constData(),
                                humanNs(p.maxNs).toUtf8().constData());
    }
    if (d.profileRows.isEmpty()) {
        s << "(no PROFILE_SCOPE samples)\n";
    }
    s << "\n";

    s << "----- Slow paints (per-receiver, >=50 ms) -----\n";
    s << QString::asprintf("%-38s %-28s %8s %14s %14s\n",
                           "Class", "Object", "Count", "Total", "Max");
    for (const auto &p : d.slowPaints) {
        s << QString::asprintf("%-38s %-28s %8llu %14s %14s\n",
                                p.className.left(38).toUtf8().constData(),
                                p.objectName.left(28).toUtf8().constData(),
                                static_cast<unsigned long long>(p.count),
                                humanNs(p.totalNs).toUtf8().constData(),
                                humanNs(p.maxNs).toUtf8().constData());
    }
    if (d.slowPaints.isEmpty()) {
        s << "(no slow paints captured)\n";
    }
    s << "\n";

    s << "===== END =====\n";

    s.flush();
    return out;
}

bool writeReport()
{
    if (!g_collectionEnabled.load(std::memory_order_relaxed)) {
        return false;
    }

    const DiagnosticReportData data = collect();
    const QString body = formatReport(data);
    const QString path = resolveReportPath();

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return false;
    }
    const QByteArray utf8 = body.toUtf8();
    const qint64 written = f.write(utf8);
    f.close();
    return written == utf8.size();
}

void resetForTesting()
{
    {
        QMutexLocker lock(&eventStatsMutex());
        eventStats().clear();
    }
    {
        QMutexLocker lock(&slowPaintStatsMutex());
        slowPaintStats().clear();
    }
}

} // namespace ShutdownDiagnostics

#endif // NDEBUG
