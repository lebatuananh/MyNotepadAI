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

#ifndef CSVDOCUMENT_H
#define CSVDOCUMENT_H

#include "git/FileEncodingDetector.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QVector>
#include <QHash>

#include <atomic>
#include <functional>
#include <list>

class QFile;
class QTextCodec;

// CsvDocument is the zero-copy core of the CSV/TSV table preview. It memory-maps
// the file (QFile::map), detects encoding/BOM/EOL/delimiter from a ≤64 KB head,
// and builds a SPARSE byte-offset index (one quint64 every 1024 rows) on a
// worker thread. Cells are decoded lazily per slice over the mapped buffer; a
// small parsed-row LRU + a sequential cursor make sequential scrolling
// amortized O(1). It never materializes a per-row container sized to the row
// count. RAII unmap on destruction.
//
// Two coordinate facts the rest of the system relies on:
//  - absolute row indices are immutable (no row insert/delete in v1), so the
//    sparse index and the edit overlay keys stay valid across re-index.
//  - all heavy reads (copy, find, save) use *Uncached / byte-range accessors
//    that touch only the immutable mmap + codec, so they are safe to run on a
//    worker as long as no re-index is in flight.
//
// NOT a QObject — it is a plain data core owned by CsvPreviewWidget, which
// drives the worker threading (QtConcurrent + QFutureWatcher).
class CsvDocument
{
public:
    enum class Eol : std::uint8_t { Lf, CrLf, Cr };

    enum class OpenStatus : std::uint8_t {
        Ok,
        TooLarge,     // > hard cap (2 GB)
        CannotOpen,   // file missing / permission
        Empty,        // zero-byte file
        MapFailed     // QFile::map returned null
    };

    static constexpr quint64 kBlockStride = 1024;          // rows per index entry
    static constexpr qint64  kHardCap = 2LL * 1024 * 1024 * 1024; // 2 GB
    static constexpr qint64  kSoftNotice = 500LL * 1024 * 1024;   // 500 MB
    static constexpr qsizetype kSniffBytes = 64 * 1024;    // head sniff budget

    CsvDocument();
    ~CsvDocument();

    CsvDocument(const CsvDocument &) = delete;
    CsvDocument &operator=(const CsvDocument &) = delete;

    // Map the file and sniff encoding/BOM/EOL/delimiter. Cheap — UI-thread safe.
    // For UTF-16/UTF-32 (BOM) the whole file is transcoded to a held UTF-8
    // buffer here (rare branch) so byte-offset scanning is valid.
    OpenStatus open(const QString &path);

    QString filePath() const { return m_path; }
    qint64 fileSize() const { return m_fileSize; }

    // Heavy O(F) single quote-aware pass. Run on a worker thread. Fills the
    // sparse offset vector, total row count and max field count. Returns false
    // if `cancel` was set. `progress(bytesScanned, totalBytes)` is throttled.
    bool buildIndex(const std::atomic<bool> &cancel,
                    const std::function<void(qint64, qint64)> &progress);

    // --- Delimiter ---
    char delimiter() const { return m_delimiter; }
    void setDelimiter(char d) { m_delimiter = d; } // caller must re-run buildIndex

    // --- Index results (valid after a successful buildIndex) ---
    quint64 totalRows() const { return m_totalRows; }
    int maxFieldCount() const { return m_maxFieldCount; }
    // True when the row count exceeds INT_MAX and the view must clamp.
    bool rowClampActive() const;

    // --- Encoding / EOL info ---
    QByteArray codecDisplayName() const;
    FileEncodingDetector::Bom bom() const { return m_bom; }
    Eol eol() const { return m_eol; }
    bool isTranscoded() const { return m_isTranscoded; }

    // --- Hot path (UI thread only): LRU + sequential cursor ---
    // Returns the decoded fields of `absRow` (COW copy, cheap). Out-of-range
    // rows return empty. Uses the parsed-row LRU.
    QVector<QString> rowFields(quint64 absRow);
    // Single decoded cell; empty for ragged/out-of-range (col >= fields).
    QString cell(quint64 absRow, int col);

    // --- Thread-safe reads (no LRU mutation): for copy/find/save workers ---
    // Safe to call from any thread while no re-index is in flight (they touch
    // only the immutable mmap/transcoded buffer + codec + offset vector).
    QVector<QString> parseRowUncached(quint64 absRow) const; // seek + parse (random access)

    // Sequential streaming primitives for O(F) bulk passes (copy/find/save):
    // seek ONCE to a row, then walk with parseRowSequential which returns the
    // next row's start offset. Avoids the O(block) forward-scan per row.
    qint64 firstRowOffset() const { return m_dataStart; }
    qint64 seekOffset(quint64 absRow) const { return seekRowStart(absRow); }
    qint64 parseRowSequential(qint64 startOffset, QVector<QString> &out) const
    { return parseRowAt(startOffset, out); }
    // Byte offset just past this row's terminator (== next row start / dataSize).
    qint64 rowEnd(qint64 startOffset) const { return skipRow(startOffset); }

    // --- Raw backing buffer (mmap or transcoded UTF-8) ---
    const char *data() const { return m_data; }
    qint64 dataSize() const { return m_dataSize; }

    // --- Save helpers (encode in the ORIGINAL codec/EOL/BOM) ---
    QByteArray encodeForSave(const QString &s) const;
    QByteArray eolBytes() const;
    QByteArray bomBytes() const;
    // RFC 4180 quote a single field value (does not encode).
    static QString serializeField(const QString &value, char delimiter);

    // --- Lifecycle around an atomic save (Windows file-lock handling) ---
    void unmap();          // release the mapping + close the handle
    bool remap();          // re-map the same path (after a save/rename)
    void clearCache();     // drop the LRU + sequential cursor

private:
    // Decode a field's raw bytes (with surrounding quotes if `quoted`) into a
    // QString, unescaping RFC 4180 "" and decoding with the backing codec.
    QString decodeField(const char *p, qsizetype len, bool quoted) const;

    // Parse one row beginning at byte offset `start`. Fills `out` with decoded
    // fields (cleared first). Returns the next row start (past the terminator)
    // or dataSize at EOF.
    qint64 parseRowAt(qint64 start, QVector<QString> &out) const;
    // Boundary-only quote-aware scan: advance past exactly one row. Returns next
    // row start. Used for the bounded forward-scan in seekRowStart.
    qint64 skipRow(qint64 start) const;
    // Byte offset of the first byte of `absRow` (nearest block + forward-scan).
    qint64 seekRowStart(quint64 absRow) const;

    void sniffDelimiter(const QByteArray &head, const QString &path);
    void detectEol(const QByteArray &head);

    QString m_path;
    QFile *m_file = nullptr;          // owns the OS handle while mapped
    uchar *m_map = nullptr;           // QFile::map result (null when transcoded)
    qint64 m_fileSize = 0;            // on-disk size

    QByteArray m_transcoded;          // UTF-8 backing store for UTF-16/32 files
    const char *m_data = nullptr;     // backing buffer (mmap or m_transcoded)
    qint64 m_dataSize = 0;            // size of the backing buffer
    qint64 m_dataStart = 0;           // offset of first row (past BOM in mmap)

    char m_delimiter = ',';
    Eol m_eol = Eol::Lf;
    FileEncodingDetector::Bom m_bom = FileEncodingDetector::Bom::None;
    bool m_isTranscoded = false;
    bool m_pendingTranscode = false;  // UTF-16/32: whole-file transcode deferred to buildIndex (worker)

    QTextCodec *m_codec = nullptr;     // decode the backing buffer (UTF-8 if null)
    QTextCodec *m_saveCodec = nullptr; // encode on save (original codec)
    QByteArray m_codecName;            // detected name (for display)

    // Sparse index
    QVector<quint64> m_offsets;        // one byte offset every kBlockStride rows
    quint64 m_totalRows = 0;
    int m_maxFieldCount = 1;

    // Parsed-row LRU (UI thread only). True O(1) promote/evict: an intrusive
    // doubly-linked list (front = most recently used, back = LRU victim) keyed
    // by a side QHash from absRow → node iterator. A cache hit splices its node
    // to the front in O(1); an insert past capacity pops the back in O(1) — no
    // O(capacity) scan on the data() hot path (CLAUDE.md performance contract).
    static constexpr int kLruCapacity = 1024;
    struct LruEntry {
        quint64 row;
        QVector<QString> fields;
    };
    using LruList = std::list<LruEntry>;
    LruList m_lruList;                              // front = MRU, back = LRU victim
    QHash<quint64, LruList::iterator> m_lruIndex;   // absRow → node
    // Sequential cursor: parsing absRow == m_cursorRow continues from
    // m_cursorOffset, avoiding a block re-scan on consecutive access.
    quint64 m_cursorRow = 0;
    qint64 m_cursorOffset = -1;        // -1 == invalid
};

#endif // CSVDOCUMENT_H
