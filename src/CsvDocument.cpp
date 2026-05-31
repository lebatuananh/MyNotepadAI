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

#include "CsvDocument.h"

#include <QFile>
#include <QFileInfo>
#include <QTextCodec>

#include <climits>

CsvDocument::CsvDocument() = default;

CsvDocument::~CsvDocument()
{
    unmap();
}

CsvDocument::OpenStatus CsvDocument::open(const QString &path)
{
    unmap();
    m_path = path;
    m_offsets.clear();
    m_totalRows = 0;
    m_maxFieldCount = 1;
    clearCache();

    QFileInfo fi(path);
    if (!fi.exists() || !fi.isFile())
        return OpenStatus::CannotOpen;

    m_fileSize = fi.size();
    if (m_fileSize > kHardCap)
        return OpenStatus::TooLarge;
    if (m_fileSize == 0)
        return OpenStatus::Empty;

    m_file = new QFile(path);
    if (!m_file->open(QIODevice::ReadOnly)) {
        delete m_file;
        m_file = nullptr;
        return OpenStatus::CannotOpen;
    }

    m_map = m_file->map(0, m_fileSize);
    if (!m_map) {
        m_file->close();
        delete m_file;
        m_file = nullptr;
        return OpenStatus::MapFailed;
    }

    // Sniff encoding from the head only (≤64 KB) — no whole-file decode.
    const qsizetype sniffLen = static_cast<qsizetype>(qMin<qint64>(m_fileSize, kSniffBytes));
    const QByteArray head = QByteArray::fromRawData(reinterpret_cast<const char *>(m_map), sniffLen);
    const FileEncodingDetector::SniffResult sniff = FileEncodingDetector::sniff(head);
    m_bom = sniff.bom;
    m_codecName = sniff.codecName;

    using Bom = FileEncodingDetector::Bom;
    if (m_bom == Bom::Utf16LE || m_bom == Bom::Utf16BE ||
        m_bom == Bom::Utf32LE || m_bom == Bom::Utf32BE) {
        // UTF-16/UTF-32: raw byte-offset newline scanning is unsafe (0x0A
        // appears inside code units). The WHOLE-file transcode to UTF-8 is heavy
        // and is therefore DEFERRED to buildIndex() (worker thread). Here we only
        // transcode the head to sniff the delimiter/EOL — cheap, UI-thread safe.
        const int bomLen = FileEncodingDetector::bomByteCount(m_bom);
        QTextCodec *src = QTextCodec::codecForName(sniff.codecName);
        m_pendingTranscode = true;
        m_isTranscoded = true;        // backing store WILL be the transcoded UTF-8
        m_codec = nullptr;            // UTF-8 once transcoded
        m_saveCodec = src;            // re-encode in the original codec on save
        m_dataStart = 0;
        // Head transcode (≤64 KB of original bytes) for delimiter/EOL sniff only.
        QByteArray uhead;
        if (src) {
            const int headBytes = static_cast<int>(qMin<qint64>(m_fileSize - bomLen, kSniffBytes));
            uhead = src->toUnicode(reinterpret_cast<const char *>(m_map) + bomLen, headBytes).toUtf8();
        }
        detectEol(uhead);
        sniffDelimiter(uhead, path);
        // m_map stays valid until buildIndex() consumes it for the full transcode.
        return OpenStatus::Ok;
    }

    // UTF-8 / SBCS / DBCS: index the mmap directly. Skip a leading UTF-8 BOM.
    m_isTranscoded = false;
    m_pendingTranscode = false;
    m_dataStart = FileEncodingDetector::bomByteCount(m_bom); // 0 or 3 (Utf8)
    m_data = reinterpret_cast<const char *>(m_map);
    m_dataSize = m_fileSize;
    m_codec = sniff.codecName.isEmpty() ? nullptr : QTextCodec::codecForName(sniff.codecName);
    m_saveCodec = m_codec;                       // same codec round-trips on save
    detectEol(head);
    sniffDelimiter(head, path);
    return OpenStatus::Ok;
}

bool CsvDocument::rowClampActive() const
{
    return m_totalRows > static_cast<quint64>(INT_MAX);
}

QByteArray CsvDocument::codecDisplayName() const
{
    if (m_isTranscoded)
        return m_codecName.isEmpty() ? QByteArrayLiteral("UTF-16/32") : m_codecName;
    return m_codecName.isEmpty() ? QByteArrayLiteral("UTF-8") : m_codecName;
}

void CsvDocument::unmap()
{
    if (m_file) {
        if (m_map) {
            m_file->unmap(m_map);
            m_map = nullptr;
        }
        m_file->close();
        delete m_file;
        m_file = nullptr;
    }
    m_map = nullptr;
    if (!m_isTranscoded) {
        m_data = nullptr;
        m_dataSize = 0;
    }
}

bool CsvDocument::remap()
{
    if (m_isTranscoded) {
        // Re-open + re-transcode the (possibly rewritten) file from scratch.
        return open(m_path) == OpenStatus::Ok;
    }
    delete m_file;
    m_file = new QFile(m_path);
    if (!m_file->open(QIODevice::ReadOnly)) {
        delete m_file;
        m_file = nullptr;
        return false;
    }
    QFileInfo fi(m_path);
    m_fileSize = fi.size();
    m_map = m_file->map(0, m_fileSize);
    if (!m_map) {
        m_file->close();
        delete m_file;
        m_file = nullptr;
        return false;
    }
    m_data = reinterpret_cast<const char *>(m_map);
    m_dataSize = m_fileSize;
    clearCache();
    return true;
}

void CsvDocument::clearCache()
{
    m_lruList.clear();
    m_lruIndex.clear();
    m_cursorRow = 0;
    m_cursorOffset = -1;
}

void CsvDocument::detectEol(const QByteArray &head)
{
    // First terminator wins. Default LF.
    const char *p = head.constData();
    const qsizetype n = head.size();
    for (qsizetype i = 0; i < n; ++i) {
        if (p[i] == '\r') {
            m_eol = (i + 1 < n && p[i + 1] == '\n') ? Eol::CrLf : Eol::Cr;
            return;
        }
        if (p[i] == '\n') {
            m_eol = Eol::Lf;
            return;
        }
    }
    m_eol = Eol::Lf;
}

void CsvDocument::sniffDelimiter(const QByteArray &head, const QString &path)
{
    // Candidate set. Default by extension first, then confirm by multi-line
    // consistency (lowest variance of per-line occurrence counts).
    const QString ext = QFileInfo(path).suffix().toLower();
    if (ext == QLatin1String("tsv") || ext == QLatin1String("tab"))
        m_delimiter = '\t';
    else
        m_delimiter = ',';

    static const char kCandidates[] = {',', ';', '\t', '|'};

    // Split the head into up to ~50 lines (ignore the last partial line). Count
    // each candidate per line OUTSIDE quotes, then score by mean-normalized
    // variance; prefer the most consistent candidate that actually appears.
    const char *p = head.constData();
    const qsizetype n = head.size();

    struct Stat { int lines = 0; long long sum = 0; long long sumSq = 0; };
    Stat stats[4];

    qsizetype i = 0;
    int lineCount = 0;
    constexpr int kMaxLines = 50;
    while (i < n && lineCount < kMaxLines) {
        int counts[4] = {0, 0, 0, 0};
        bool inQuote = false;
        qsizetype lineStart = i;
        for (; i < n; ++i) {
            const char c = p[i];
            if (c == '"') {
                if (inQuote && i + 1 < n && p[i + 1] == '"') { ++i; continue; }
                inQuote = !inQuote;
                continue;
            }
            if (!inQuote && (c == '\n' || c == '\r')) break;
            if (!inQuote) {
                for (int k = 0; k < 4; ++k)
                    if (c == kCandidates[k]) ++counts[k];
            }
        }
        // Skip the terminator(s).
        if (i < n && p[i] == '\r') ++i;
        if (i < n && p[i] == '\n') ++i;

        if (i > lineStart) {
            ++lineCount;
            for (int k = 0; k < 4; ++k) {
                stats[k].lines++;
                stats[k].sum += counts[k];
                stats[k].sumSq += static_cast<long long>(counts[k]) * counts[k];
            }
        }
    }

    if (lineCount == 0)
        return; // keep extension default

    int bestIdx = -1;
    double bestScore = 0.0; // higher = better (consistency * presence)
    for (int k = 0; k < 4; ++k) {
        if (stats[k].sum == 0) continue;
        const double mean = static_cast<double>(stats[k].sum) / stats[k].lines;
        const double var = static_cast<double>(stats[k].sumSq) / stats[k].lines - mean * mean;
        // Lower variance and higher mean are both good. Score = mean / (1+var).
        const double score = mean / (1.0 + var);
        if (score > bestScore) {
            bestScore = score;
            bestIdx = k;
        }
    }

    if (bestIdx >= 0)
        m_delimiter = kCandidates[bestIdx];
}

bool CsvDocument::buildIndex(const std::atomic<bool> &cancel,
                             const std::function<void(qint64, qint64)> &progress)
{
    // Deferred UTF-16/32 whole-file transcode (heavy — runs here on the worker).
    if (m_pendingTranscode) {
        const int bomLen = FileEncodingDetector::bomByteCount(m_bom);
        if (m_saveCodec && m_map) {
            const QString unified = m_saveCodec->toUnicode(
                reinterpret_cast<const char *>(m_map) + bomLen,
                static_cast<int>(qMin<qint64>(m_fileSize - bomLen, INT_MAX)));
            m_transcoded = unified.toUtf8();
        } else {
            m_transcoded = QByteArray();
        }
        // The original mapping is no longer needed; the UTF-8 buffer backs us now.
        if (m_file && m_map) {
            m_file->unmap(m_map);
            m_map = nullptr;
        }
        m_data = m_transcoded.constData();
        m_dataSize = m_transcoded.size();
        m_dataStart = 0;
        m_pendingTranscode = false;
        if (cancel.load(std::memory_order_relaxed)) return false;
    }

    m_offsets.clear();
    m_totalRows = 0;
    m_maxFieldCount = 1;
    clearCache();

    const char *p = m_data;
    const qint64 size = m_dataSize;
    qint64 i = m_dataStart;
    if (i >= size)
        return true; // nothing to index (empty after BOM)

    // Reserve from an average-line-length estimate (sample first 64 KB).
    {
        qint64 sampleRows = 0;
        const qint64 sampleEnd = qMin<qint64>(size, m_dataStart + 64 * 1024);
        for (qint64 s = m_dataStart; s < sampleEnd; ++s)
            if (p[s] == '\n') ++sampleRows;
        const double avgLen = sampleRows > 0
            ? static_cast<double>(sampleEnd - m_dataStart) / sampleRows
            : 64.0;
        const quint64 estRows = static_cast<quint64>(size / qMax(1.0, avgLen)) + 1;
        m_offsets.reserve(static_cast<int>(qMin<quint64>(estRows / kBlockStride + 2,
                                                         64 * 1024 * 1024)));
    }

    const char delim = m_delimiter;
    quint64 rowIndex = 0;
    int curFields = 1;          // field count of the row currently being scanned
    bool inQuote = false;
    qint64 rowStart = i;

    // Record the offset of row 0.
    m_offsets.push_back(static_cast<quint64>(rowStart));

    qint64 lastProgress = i;
    constexpr qint64 kProgressStep = 8 * 1024 * 1024;

    while (i < size) {
        const char c = p[i];
        if (inQuote) {
            if (c == '"') {
                if (i + 1 < size && p[i + 1] == '"') { i += 2; continue; } // literal ""
                inQuote = false;
            }
            ++i;
            continue;
        }
        if (c == '"') {
            inQuote = true;
            ++i;
            continue;
        }
        if (c == delim) {
            ++curFields;
            ++i;
            continue;
        }
        if (c == '\n' || c == '\r') {
            // Row terminator. Account the row, advance past CRLF/CR/LF.
            if (curFields > m_maxFieldCount) m_maxFieldCount = curFields;
            ++rowIndex;
            if (c == '\r' && i + 1 < size && p[i + 1] == '\n') i += 2;
            else ++i;
            // Reset for next row.
            curFields = 1;
            rowStart = i;
            // Record a sparse offset every kBlockStride rows (skip if at EOF).
            if (i < size && (rowIndex % kBlockStride) == 0)
                m_offsets.push_back(static_cast<quint64>(rowStart));

            if (i - lastProgress >= kProgressStep) {
                lastProgress = i;
                if (progress) progress(i, size);
                if (cancel.load(std::memory_order_relaxed)) return false;
            }
            continue;
        }
        ++i;
    }

    // Trailing row without a final terminator: if there is any content past the
    // last recorded row start, it is one more (unterminated) row.
    if (rowStart < size) {
        if (curFields > m_maxFieldCount) m_maxFieldCount = curFields;
        ++rowIndex;
    }

    m_totalRows = rowIndex;
    if (progress) progress(size, size);
    return true;
}

qint64 CsvDocument::skipRow(qint64 start) const
{
    const char *p = m_data;
    const qint64 size = m_dataSize;
    qint64 i = start;
    bool inQuote = false;
    while (i < size) {
        const char c = p[i];
        if (inQuote) {
            if (c == '"') {
                if (i + 1 < size && p[i + 1] == '"') { i += 2; continue; }
                inQuote = false;
            }
            ++i;
            continue;
        }
        if (c == '"') { inQuote = true; ++i; continue; }
        if (c == '\n') return i + 1;
        if (c == '\r') return (i + 1 < size && p[i + 1] == '\n') ? i + 2 : i + 1;
        ++i;
    }
    return size; // EOF (no terminator)
}

qint64 CsvDocument::seekRowStart(quint64 absRow) const
{
    if (absRow == 0)
        return m_dataStart;
    const quint64 block = absRow / kBlockStride;
    const int blockIdx = static_cast<int>(qMin<quint64>(block, static_cast<quint64>(m_offsets.size() - 1)));
    qint64 off = static_cast<qint64>(m_offsets.at(blockIdx));
    quint64 row = static_cast<quint64>(blockIdx) * kBlockStride;
    // Bounded forward-scan: at most kBlockStride rows.
    while (row < absRow && off < m_dataSize) {
        off = skipRow(off);
        ++row;
    }
    return off;
}

QString CsvDocument::decodeField(const char *p, qsizetype len, bool quoted) const
{
    if (!quoted) {
        if (len <= 0) return QString();
        return m_codec ? m_codec->toUnicode(p, static_cast<int>(len))
                       : QString::fromUtf8(p, static_cast<int>(len));
    }
    // Quoted: strip the surrounding quotes and unescape "" → ". `p`/`len` cover
    // the bytes INCLUDING the opening/closing quote.
    const char *inner = p + 1;
    qsizetype innerLen = len - 2;
    if (innerLen < 0) innerLen = 0;
    // Fast path: no doubled quote.
    bool hasDouble = false;
    for (qsizetype i = 0; i < innerLen; ++i)
        if (inner[i] == '"') { hasDouble = true; break; }
    if (!hasDouble) {
        return m_codec ? m_codec->toUnicode(inner, static_cast<int>(innerLen))
                       : QString::fromUtf8(inner, static_cast<int>(innerLen));
    }
    QByteArray unescaped;
    unescaped.reserve(static_cast<int>(innerLen));
    for (qsizetype i = 0; i < innerLen; ++i) {
        unescaped.append(inner[i]);
        if (inner[i] == '"' && i + 1 < innerLen && inner[i + 1] == '"')
            ++i; // skip the second quote of a "" pair
    }
    return m_codec ? m_codec->toUnicode(unescaped.constData(), unescaped.size())
                   : QString::fromUtf8(unescaped);
}

qint64 CsvDocument::parseRowAt(qint64 start, QVector<QString> &out) const
{
    out.clear();
    const char *p = m_data;
    const qint64 size = m_dataSize;
    const char delim = m_delimiter;
    qint64 i = start;
    qint64 fieldStart = start;
    bool inQuote = false;
    bool fieldQuoted = false;

    auto emitField = [&](qint64 end) {
        out.push_back(decodeField(p + fieldStart, end - fieldStart, fieldQuoted));
        fieldQuoted = false;
    };

    while (i < size) {
        const char c = p[i];
        if (inQuote) {
            if (c == '"') {
                if (i + 1 < size && p[i + 1] == '"') { i += 2; continue; }
                inQuote = false;
            }
            ++i;
            continue;
        }
        if (c == '"') {
            if (i == fieldStart) fieldQuoted = true; // opening quote of the field
            inQuote = true;
            ++i;
            continue;
        }
        if (c == delim) {
            emitField(i);
            ++i;
            fieldStart = i;
            continue;
        }
        if (c == '\n' || c == '\r') {
            emitField(i);
            if (c == '\r' && i + 1 < size && p[i + 1] == '\n') return i + 2;
            return i + 1;
        }
        ++i;
    }
    // EOF: emit the trailing field if the row had any content.
    emitField(size);
    return size;
}

QVector<QString> CsvDocument::parseRowUncached(quint64 absRow) const
{
    QVector<QString> out;
    if (absRow >= m_totalRows) return out;
    const qint64 start = seekRowStart(absRow);
    parseRowAt(start, out);
    return out;
}

QVector<QString> CsvDocument::rowFields(quint64 absRow)
{
    if (absRow >= m_totalRows) return {};

    auto idxIt = m_lruIndex.find(absRow);
    if (idxIt != m_lruIndex.end()) {
        // Hit: splice the node to the front (most-recently-used) in O(1).
        // std::list::splice does not invalidate the node iterator, so the
        // QHash entry stays valid.
        const LruList::iterator &node = idxIt.value();
        m_lruList.splice(m_lruList.begin(), m_lruList, node);
        return node->fields; // COW copy, cheap
    }

    // Sequential cursor fast path: consecutive access continues from the last
    // parsed offset instead of re-seeking the block.
    qint64 start;
    if (m_cursorOffset >= 0 && absRow == m_cursorRow)
        start = m_cursorOffset;
    else
        start = seekRowStart(absRow);

    QVector<QString> fields;
    const qint64 next = parseRowAt(start, fields);
    m_cursorRow = absRow + 1;
    m_cursorOffset = next;

    // Insert at the front (MRU); evict the back (LRU victim) past capacity —
    // both O(1), no scan over the LRU on the hot path.
    m_lruList.push_front(LruEntry{absRow, fields});
    m_lruIndex.insert(absRow, m_lruList.begin());
    if (m_lruIndex.size() > kLruCapacity) {
        const quint64 evict = m_lruList.back().row;
        m_lruList.pop_back();
        m_lruIndex.remove(evict);
    }
    return fields;
}

QString CsvDocument::cell(quint64 absRow, int col)
{
    const QVector<QString> &fields = rowFields(absRow);
    if (col < 0 || col >= fields.size()) return QString();
    return fields.at(col);
}

QByteArray CsvDocument::eolBytes() const
{
    switch (m_eol) {
    case Eol::CrLf: return QByteArrayLiteral("\r\n");
    case Eol::Cr:   return QByteArrayLiteral("\r");
    case Eol::Lf:   break;
    }
    return QByteArrayLiteral("\n");
}

QByteArray CsvDocument::bomBytes() const
{
    using Bom = FileEncodingDetector::Bom;
    switch (m_bom) {
    case Bom::Utf8:    return QByteArray("\xEF\xBB\xBF", 3);
    case Bom::Utf16LE: return QByteArray("\xFF\xFE", 2);
    case Bom::Utf16BE: return QByteArray("\xFE\xFF", 2);
    case Bom::Utf32LE: return QByteArray("\xFF\xFE\x00\x00", 4);
    case Bom::Utf32BE: return QByteArray("\x00\x00\xFE\xFF", 4);
    case Bom::None:    break;
    }
    return QByteArray();
}

QByteArray CsvDocument::encodeForSave(const QString &s) const
{
    if (m_saveCodec)
        return m_saveCodec->fromUnicode(s);
    return s.toUtf8();
}

QString CsvDocument::serializeField(const QString &value, char delimiter)
{
    // RFC 4180: quote if the value contains the delimiter, a quote, CR or LF.
    bool needQuote = false;
    for (const QChar ch : value) {
        const ushort u = ch.unicode();
        if (u == static_cast<ushort>(delimiter) || u == '"' || u == '\n' || u == '\r') {
            needQuote = true;
            break;
        }
    }
    if (!needQuote)
        return value;
    QString out;
    out.reserve(value.size() + 2);
    out.append(QLatin1Char('"'));
    for (const QChar ch : value) {
        if (ch == QLatin1Char('"'))
            out.append(QLatin1Char('"')); // double the quote
        out.append(ch);
    }
    out.append(QLatin1Char('"'));
    return out;
}





