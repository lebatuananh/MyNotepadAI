#include "QuickFileOpenDialog.h"

#include <QVBoxLayout>
#include <QDirIterator>
#include <QDir>
#include <QKeyEvent>
#include <QShowEvent>
#include <QStringView>
#include <QtConcurrent>
#include <QApplication>
#include <algorithm>

QuickFileOpenDialog::QuickFileOpenDialog(const QString &rootPath, QWidget *parent)
    : QDialog(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
    , m_rootPath(rootPath)
{
    setMinimumWidth(500);
    setMaximumHeight(400);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(2);

    m_lineEdit = new QLineEdit(this);
    m_lineEdit->setPlaceholderText(tr("Indexing files..."));
    m_lineEdit->setClearButtonEnabled(true);
    layout->addWidget(m_lineEdit);

    m_listView = new QListView(this);
    m_listView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_listView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_model = new QStringListModel(this);
    m_listView->setModel(m_model);
    layout->addWidget(m_listView);

    m_lineEdit->installEventFilter(this);
    m_listView->installEventFilter(this);
    qApp->installEventFilter(this);

    connect(m_lineEdit, &QLineEdit::textChanged, this, &QuickFileOpenDialog::onTextChanged);
    connect(m_listView, &QListView::activated, this, &QuickFileOpenDialog::onItemActivated);
    connect(m_listView, &QListView::clicked, this, &QuickFileOpenDialog::onItemActivated);

    m_watcher = new QFutureWatcher<QStringList>(this);
    connect(m_watcher, &QFutureWatcher<QStringList>::finished,
            this, &QuickFileOpenDialog::onIndexReady);
    m_watcher->setFuture(QtConcurrent::run(&QuickFileOpenDialog::buildFileIndex, m_rootPath));

    m_lineEdit->setFocus();
}

QuickFileOpenDialog::~QuickFileOpenDialog()
{
    qApp->removeEventFilter(this);
    if (m_watcher->isRunning()) {
        m_watcher->waitForFinished();
    }
}

QString QuickFileOpenDialog::selectedFilePath() const
{
    return m_selectedFile;
}

void QuickFileOpenDialog::onIndexReady()
{
    m_allFiles = m_watcher->result();
    m_indexReady = true;
    m_lineEdit->setPlaceholderText(tr("Type to search files..."));
    applyFilter(m_lineEdit->text());
}

void QuickFileOpenDialog::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        reject();
        return;
    }
    QDialog::keyPressEvent(event);
}

void QuickFileOpenDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    activateWindow();
    m_lineEdit->setFocus();
}

bool QuickFileOpenDialog::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonPress) {
        auto *target = qobject_cast<QWidget *>(obj);
        if (target && !isAncestorOf(target) && target != this) {
            reject();
            return false;
        }
    }

    if (event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        if (ke->key() == Qt::Key_Escape) {
            reject();
            return true;
        }
        if (obj == m_lineEdit) {
            switch (ke->key()) {
            case Qt::Key_Down: {
                int row = m_listView->currentIndex().row() + 1;
                if (row < m_model->rowCount())
                    m_listView->setCurrentIndex(m_model->index(row));
                return true;
            }
            case Qt::Key_Up: {
                int row = m_listView->currentIndex().row() - 1;
                if (row >= 0)
                    m_listView->setCurrentIndex(m_model->index(row));
                return true;
            }
            case Qt::Key_Return:
            case Qt::Key_Enter:
                onItemActivated(m_listView->currentIndex());
                return true;
            default:
                break;
            }
        } else if (obj == m_listView) {
            switch (ke->key()) {
            case Qt::Key_Return:
            case Qt::Key_Enter:
                onItemActivated(m_listView->currentIndex());
                return true;
            default:
                m_lineEdit->setFocus();
                m_lineEdit->event(event);
                return true;
            }
        }
    }
    return QDialog::eventFilter(obj, event);
}

void QuickFileOpenDialog::onTextChanged(const QString &text)
{
    if (m_indexReady)
        applyFilter(text);
}

void QuickFileOpenDialog::onItemActivated(const QModelIndex &index)
{
    if (!index.isValid()) return;
    const QString rel = index.data(Qt::DisplayRole).toString();
    m_selectedFile = m_rootPath + QLatin1Char('/') + rel;
    accept();
}

QStringList QuickFileOpenDialog::buildFileIndex(const QString &rootPath)
{
    QStringList files;
    files.reserve(4096);
    const int rootLen = rootPath.length() + 1;
    QDirIterator it(rootPath, QDir::Files | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QString path = it.filePath();
        const QStringView rel = QStringView(path).mid(rootLen);
        if (rel.startsWith(QLatin1String(".git/")) || rel.contains(QLatin1String("/.git/")))
            continue;
        if (rel.startsWith(QLatin1String("node_modules/")) || rel.contains(QLatin1String("/node_modules/")))
            continue;
        if (rel.startsWith(QLatin1String("build/")) || rel.startsWith(QLatin1String("build-")))
            continue;
        files.append(rel.toString());
    }
    files.sort(Qt::CaseInsensitive);
    return files;
}

void QuickFileOpenDialog::applyFilter(const QString &pattern)
{
    if (pattern.isEmpty()) {
        m_filteredFiles = m_allFiles.mid(0, 200);
    } else {
        struct Scored { int score; int idx; };
        QVector<Scored> scored;
        scored.reserve(m_allFiles.size());
        for (qsizetype i = 0, n = m_allFiles.size(); i < n; ++i) {
            int s = fuzzyScore(pattern, m_allFiles[i]);
            if (s > 0)
                scored.append({s, static_cast<int>(i)});
        }
        std::sort(scored.begin(), scored.end(), [](const Scored &a, const Scored &b) {
            return a.score > b.score;
        });
        m_filteredFiles.clear();
        const int limit = static_cast<int>(qMin(scored.size(), qsizetype(200)));
        m_filteredFiles.reserve(limit);
        for (int i = 0; i < limit; ++i)
            m_filteredFiles.append(m_allFiles[scored[i].idx]);
    }
    m_model->setStringList(m_filteredFiles);
    if (!m_filteredFiles.isEmpty())
        m_listView->setCurrentIndex(m_model->index(0));
}

int QuickFileOpenDialog::fuzzyScore(const QString &pattern, const QString &candidate)
{
    const int pLen = pattern.length();
    const int cLen = candidate.length();
    if (pLen == 0) return 1;
    if (pLen > cLen) return 0;

    int score = 0;
    int pi = 0;
    int prevMatchPos = -2;

    for (int ci = 0; ci < cLen && pi < pLen; ++ci) {
        const QChar pc = pattern[pi].toLower();
        const QChar cc = candidate[ci].toLower();
        if (pc == cc) {
            score += 1;
            if (ci == prevMatchPos + 1)
                score += 2;
            if (ci == 0 || candidate[ci - 1] == QLatin1Char('/') ||
                candidate[ci - 1] == QLatin1Char('\\') ||
                candidate[ci - 1] == QLatin1Char('.') ||
                candidate[ci - 1] == QLatin1Char('_') ||
                candidate[ci - 1] == QLatin1Char('-'))
                score += 3;
            if (candidate[ci].isUpper() && pattern[pi].isUpper())
                score += 2;
            prevMatchPos = ci;
            ++pi;
        }
    }
    if (pi < pLen) return 0;

    int filenameStart = candidate.lastIndexOf(QLatin1Char('/'));
    if (filenameStart < 0) filenameStart = 0; else ++filenameStart;
    const QStringView filename = QStringView(candidate).mid(filenameStart);
    if (filename.contains(pattern, Qt::CaseInsensitive))
        score += 10 + pLen;

    return score;
}
