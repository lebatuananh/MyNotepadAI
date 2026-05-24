#include "SendWithGoalDialog.h"

#include "AcpAgentDefinition.h"
#include "AcpAgentRegistry.h"
#include "ApplicationSettings.h"
#include "GoalAgentSettings.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>
#include <QWidgetAction>

static constexpr int kMaxRows = GoalAgentSettings::kMaxCriteriaRows;

SendWithGoalDialog::SendWithGoalDialog(AcpAgentRegistry *registry,
                                       ApplicationSettings *settings,
                                       QWidget *parent)
    : QDialog(parent)
    , m_registry(registry)
    , m_settings(settings)
{
    setWindowTitle(tr("Send with Goal"));
    setMinimumWidth(440);
    buildUi();
    populateAgents();
    populateTemplates();
    populatePresets();
    updateRowCount();
    updateTemplateButtons();
}

SendWithGoalDialog::~SendWithGoalDialog() = default;

void SendWithGoalDialog::buildUi()
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(12);

    // --- Success criteria section ---
    auto *criteriaLabel = new QLabel(tr("Success criteria"), this);
    criteriaLabel->setStyleSheet(QStringLiteral("font-weight: 500; font-size: 12px;"));
    mainLayout->addWidget(criteriaLabel);

    auto *helpLabel = new QLabel(
        tr("Each row is one milestone. The goal-agent advances to the next when "
           "it judges the previous one done. Up to %1 rows.").arg(kMaxRows), this);
    helpLabel->setWordWrap(true);
    helpLabel->setStyleSheet(QStringLiteral("font-size: 11px; color: gray;"));
    mainLayout->addWidget(helpLabel);

    // Preset buttons
    auto *presetLayout = new QHBoxLayout;
    m_loadPresetBtn = new QPushButton(tr("Load preset"), this);
    m_presetMenu = new QMenu(this);
    m_loadPresetBtn->setMenu(m_presetMenu);
    presetLayout->addWidget(m_loadPresetBtn);

    m_savePresetBtn = new QPushButton(tr("Save as preset"), this);
    connect(m_savePresetBtn, &QPushButton::clicked, this, &SendWithGoalDialog::onSavePreset);
    presetLayout->addWidget(m_savePresetBtn);
    presetLayout->addStretch();
    mainLayout->addLayout(presetLayout);

    // Criteria scroll area
    m_criteriaScroll = new QScrollArea(this);
    m_criteriaScroll->setWidgetResizable(true);
    m_criteriaScroll->setFrameShape(QFrame::NoFrame);
    m_criteriaScroll->setMinimumHeight(120);
    m_criteriaScroll->setMaximumHeight(280);

    auto *criteriaHost = new QWidget(m_criteriaScroll);
    m_criteriaLayout = new QVBoxLayout(criteriaHost);
    m_criteriaLayout->setContentsMargins(0, 0, 0, 0);
    m_criteriaLayout->setSpacing(4);
    m_criteriaLayout->addStretch();
    m_criteriaScroll->setWidget(criteriaHost);
    mainLayout->addWidget(m_criteriaScroll);

    // Add first empty criterion
    m_criteriaEdits.append(createCriterionEdit());

    // Add/Remove buttons + row count
    auto *addRemoveLayout = new QHBoxLayout;
    m_addBtn = new QPushButton(tr("+ Add criterion"), this);
    connect(m_addBtn, &QPushButton::clicked, this, &SendWithGoalDialog::onAddCriterion);
    addRemoveLayout->addWidget(m_addBtn);

    m_removeBtn = new QPushButton(tr("Remove"), this);
    connect(m_removeBtn, &QPushButton::clicked, this, &SendWithGoalDialog::onRemoveCriterion);
    addRemoveLayout->addWidget(m_removeBtn);

    addRemoveLayout->addStretch();
    m_rowCountLabel = new QLabel(this);
    m_rowCountLabel->setStyleSheet(QStringLiteral("font-size: 11px; color: gray;"));
    addRemoveLayout->addWidget(m_rowCountLabel);
    mainLayout->addLayout(addRemoveLayout);

    // --- Goal-agent picker ---
    auto *agentLabel = new QLabel(tr("Goal-agent"), this);
    agentLabel->setStyleSheet(QStringLiteral("font-weight: 500; font-size: 12px;"));
    mainLayout->addWidget(agentLabel);

    m_agentCombo = new QComboBox(this);
    mainLayout->addWidget(m_agentCombo);

    // --- Prompt template picker ---
    auto *tplLabel = new QLabel(tr("Prompt template"), this);
    tplLabel->setStyleSheet(QStringLiteral("font-weight: 500; font-size: 12px;"));
    mainLayout->addWidget(tplLabel);

    auto *tplRow = new QHBoxLayout;
    m_templateCombo = new QComboBox(this);
    m_templateCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    tplRow->addWidget(m_templateCombo);

    auto *tplNewBtn = new QPushButton(tr("New"), this);
    connect(tplNewBtn, &QPushButton::clicked, this, &SendWithGoalDialog::onTemplateNew);
    tplRow->addWidget(tplNewBtn);

    m_tplRenameBtn = new QPushButton(tr("Rename"), this);
    connect(m_tplRenameBtn, &QPushButton::clicked, this, &SendWithGoalDialog::onTemplateRename);
    tplRow->addWidget(m_tplRenameBtn);

    m_tplEditBtn = new QPushButton(tr("Edit"), this);
    connect(m_tplEditBtn, &QPushButton::clicked, this, &SendWithGoalDialog::onTemplateEdit);
    tplRow->addWidget(m_tplEditBtn);

    m_tplDeleteBtn = new QPushButton(tr("Delete"), this);
    connect(m_tplDeleteBtn, &QPushButton::clicked, this, &SendWithGoalDialog::onTemplateDelete);
    tplRow->addWidget(m_tplDeleteBtn);

    connect(m_templateCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this]() { updateTemplateButtons(); });

    mainLayout->addLayout(tplRow);

    // --- Max iterations ---
    auto *iterLabel = new QLabel(tr("Max iterations"), this);
    iterLabel->setStyleSheet(QStringLiteral("font-weight: 500; font-size: 12px;"));
    mainLayout->addWidget(iterLabel);

    m_maxIterSpin = new QSpinBox(this);
    m_maxIterSpin->setRange(GoalAgentSettings::kMaxIterationsMin,
                            GoalAgentSettings::kMaxIterationsMax);

    // Load default from settings
    const QString settingsJson = m_settings->get("Ai/GoalAgentSettings", QString());
    GoalAgentSettings goalSettings;
    if (!settingsJson.isEmpty()) {
        goalSettings = GoalAgentSettings::fromJson(
            QJsonDocument::fromJson(settingsJson.toUtf8()).object());
    }
    m_maxIterSpin->setValue(goalSettings.defaultMaxIterations);
    mainLayout->addWidget(m_maxIterSpin);

    // --- Error label ---
    m_errorLabel = new QLabel(this);
    m_errorLabel->setStyleSheet(QStringLiteral("color: red; font-size: 12px;"));
    m_errorLabel->hide();
    mainLayout->addWidget(m_errorLabel);

    // --- Footer buttons ---
    auto *footerLayout = new QHBoxLayout;
    footerLayout->addStretch();
    m_cancelBtn = new QPushButton(tr("Cancel"), this);
    connect(m_cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    footerLayout->addWidget(m_cancelBtn);

    m_startBtn = new QPushButton(tr("Start Goal"), this);
    m_startBtn->setDefault(true);
    m_startBtn->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return));
    connect(m_startBtn, &QPushButton::clicked, this, &SendWithGoalDialog::onStart);
    footerLayout->addWidget(m_startBtn);
    mainLayout->addLayout(footerLayout);
}

void SendWithGoalDialog::populateAgents()
{
    m_agentCombo->clear();
    const auto agents = m_registry->agents();
    const QString settingsJson = m_settings->get("Ai/GoalAgentSettings", QString());
    GoalAgentSettings goalSettings;
    if (!settingsJson.isEmpty()) {
        goalSettings = GoalAgentSettings::fromJson(
            QJsonDocument::fromJson(settingsJson.toUtf8()).object());
    }

    int selectedIdx = 0;
    for (int i = 0; i < agents.size(); ++i) {
        const auto &a = agents[i];
        m_agentCombo->addItem(a.name.isEmpty() ? a.id : a.name, a.id);
        if (a.id == goalSettings.agentId)
            selectedIdx = i;
    }
    if (m_agentCombo->count() > 0)
        m_agentCombo->setCurrentIndex(selectedIdx);
}

void SendWithGoalDialog::populateTemplates()
{
    m_templateCombo->clear();
    const QString settingsJson = m_settings->get("Ai/GoalAgentSettings", QString());
    GoalAgentSettings goalSettings;
    if (!settingsJson.isEmpty()) {
        goalSettings = GoalAgentSettings::fromJson(
            QJsonDocument::fromJson(settingsJson.toUtf8()).object());
    }

    for (const auto &t : goalSettings.promptTemplates) {
        QString label = t.name;
        if (t.id == QLatin1String(GoalAgentSettings::kDefaultTemplateId))
            label += QStringLiteral(" (default)");
        m_templateCombo->addItem(label, t.id);
    }
}

void SendWithGoalDialog::populatePresets()
{
    m_presetMenu->clear();
    const QString settingsJson = m_settings->get("Ai/GoalAgentSettings", QString());
    GoalAgentSettings goalSettings;
    if (!settingsJson.isEmpty()) {
        goalSettings = GoalAgentSettings::fromJson(
            QJsonDocument::fromJson(settingsJson.toUtf8()).object());
    }

    if (goalSettings.criteriaPresets.isEmpty()) {
        m_presetMenu->addAction(tr("No saved presets"))->setEnabled(false);
        return;
    }

    for (const auto &preset : goalSettings.criteriaPresets) {
        auto *wa = new QWidgetAction(m_presetMenu);
        auto *row = new QWidget;
        auto *hl = new QHBoxLayout(row);
        hl->setContentsMargins(6, 2, 4, 2);
        hl->setSpacing(4);

        auto *loadBtn = new QPushButton(
            QStringLiteral("%1 (%2)").arg(preset.name).arg(preset.criteria.size()), row);
        loadBtn->setFlat(true);
        loadBtn->setCursor(Qt::PointingHandCursor);
        loadBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        loadBtn->setStyleSheet(QStringLiteral("QPushButton { text-align: left; padding: 2px 4px; }"));
        hl->addWidget(loadBtn);

        auto *delBtn = new QToolButton(row);
        delBtn->setIcon(QIcon(QStringLiteral(":/icons/bin_closed.png")));
        delBtn->setAutoRaise(true);
        delBtn->setIconSize(QSize(14, 14));
        delBtn->setFixedSize(20, 20);
        delBtn->setToolTip(tr("Delete"));
        hl->addWidget(delBtn);

        wa->setDefaultWidget(row);
        m_presetMenu->addAction(wa);

        connect(loadBtn, &QPushButton::clicked, this, [this, preset]() {
            m_presetMenu->close();
            for (auto *edit : m_criteriaEdits)
                edit->deleteLater();
            m_criteriaEdits.clear();
            for (const auto &c : preset.criteria) {
                m_criteriaEdits.append(createCriterionEdit(c));
            }
            if (m_criteriaEdits.isEmpty()) {
                m_criteriaEdits.append(createCriterionEdit());
            }
            updateRowCount();
        });

        connect(delBtn, &QToolButton::clicked, this, [this, id = preset.id, name = preset.name]() {
            m_presetMenu->close();
            auto answer = QMessageBox::question(
                this, tr("Delete preset"),
                tr("Delete preset \"%1\"?").arg(name));
            if (answer != QMessageBox::Yes)
                return;

            const QString json = m_settings->get("Ai/GoalAgentSettings", QString());
            if (json.isEmpty())
                return;
            QJsonParseError parseErr;
            auto doc = QJsonDocument::fromJson(json.toUtf8(), &parseErr);
            if (parseErr.error != QJsonParseError::NoError)
                return;
            GoalAgentSettings gs = GoalAgentSettings::fromJson(doc.object());
            gs.criteriaPresets.removeIf([&id](const GoalCriteriaPreset &p) { return p.id == id; });
            m_settings->setValue(QStringLiteral("Ai/GoalAgentSettings"),
                                QString::fromUtf8(QJsonDocument(gs.toJson()).toJson(QJsonDocument::Compact)));
            populatePresets();
        });
    }
}

void SendWithGoalDialog::updateRowCount()
{
    m_rowCountLabel->setText(
        QStringLiteral("%1 / %2 rows").arg(m_criteriaEdits.size()).arg(kMaxRows));
    m_addBtn->setEnabled(m_criteriaEdits.size() < kMaxRows);
    m_removeBtn->setEnabled(m_criteriaEdits.size() > 1);
}

void SendWithGoalDialog::updateTemplateButtons()
{
    const bool isBuiltin = m_templateCombo->currentData().toString()
                           == QLatin1String(GoalAgentSettings::kDefaultTemplateId);
    m_tplRenameBtn->setEnabled(!isBuiltin);
    m_tplEditBtn->setEnabled(!isBuiltin);
    m_tplDeleteBtn->setEnabled(!isBuiltin);
}

void SendWithGoalDialog::onAddCriterion()
{
    if (m_criteriaEdits.size() >= kMaxRows)
        return;
    auto *edit = createCriterionEdit();
    m_criteriaEdits.append(edit);
    edit->setFocus();
    updateRowCount();
}

void SendWithGoalDialog::onRemoveCriterion()
{
    if (m_criteriaEdits.size() <= 1)
        return;

    // Remove the last one (or the focused one if identifiable)
    QPlainTextEdit *target = nullptr;
    for (auto *edit : m_criteriaEdits) {
        if (edit->hasFocus()) {
            target = edit;
            break;
        }
    }
    if (!target)
        target = m_criteriaEdits.last();

    m_criteriaEdits.removeOne(target);
    target->deleteLater();
    updateRowCount();
}

void SendWithGoalDialog::onSavePreset()
{
    QStringList criteria;
    for (auto *edit : m_criteriaEdits) {
        const QString text = edit->toPlainText().trimmed();
        if (!text.isEmpty())
            criteria.append(text);
    }
    if (criteria.isEmpty()) {
        m_errorLabel->setText(tr("Add at least one criterion to save as a preset."));
        m_errorLabel->show();
        return;
    }

    bool ok = false;
    QString name;
    // Simple input dialog with inline validation
    auto *dlg = new QDialog(this);
    dlg->setWindowTitle(tr("Save criteria preset"));
    auto *layout = new QVBoxLayout(dlg);
    layout->addWidget(new QLabel(tr("Preset name:"), dlg));
    auto *nameEdit = new QLineEdit(dlg);
    nameEdit->setMaxLength(100);
    layout->addWidget(nameEdit);
    auto *nameError = new QLabel(dlg);
    nameError->setStyleSheet(QStringLiteral("color: red; font-size: 11px;"));
    nameError->hide();
    layout->addWidget(nameError);
    auto *btnBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dlg);
    layout->addWidget(btnBox);
    connect(btnBox, &QDialogButtonBox::rejected, dlg, &QDialog::reject);
    connect(btnBox, &QDialogButtonBox::accepted, dlg, [dlg, nameEdit, nameError]() {
        if (nameEdit->text().trimmed().isEmpty()) {
            nameError->setText(QObject::tr("Name is required."));
            nameError->show();
            return;
        }
        dlg->accept();
    });

    if (dlg->exec() == QDialog::Accepted) {
        name = nameEdit->text().trimmed();
        ok = !name.isEmpty();
    }
    delete dlg;

    if (!ok)
        return;

    // Save to settings
    const QString settingsJson = m_settings->get("Ai/GoalAgentSettings", QString());
    GoalAgentSettings goalSettings;
    if (!settingsJson.isEmpty()) {
        goalSettings = GoalAgentSettings::fromJson(
            QJsonDocument::fromJson(settingsJson.toUtf8()).object());
    }

    GoalCriteriaPreset preset;
    preset.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    preset.name = name;
    preset.criteria = criteria;

    // Check for name collision — overwrite in place.
    bool replaced = false;
    for (int i = 0; i < goalSettings.criteriaPresets.size(); ++i) {
        if (goalSettings.criteriaPresets[i].name.trimmed() == name) {
            goalSettings.criteriaPresets[i].criteria = criteria;
            replaced = true;
            break;
        }
    }
    if (!replaced)
        goalSettings.criteriaPresets.append(preset);

    m_settings->setValue(QStringLiteral("Ai/GoalAgentSettings"),
                        QString::fromUtf8(QJsonDocument(goalSettings.toJson()).toJson(QJsonDocument::Compact)));
    populatePresets();
    m_errorLabel->hide();
}


void SendWithGoalDialog::onTemplateNew()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("New Prompt Template"),
        tr("Template name:"), QLineEdit::Normal, QString(), &ok);
    if (!ok || name.trimmed().isEmpty()) return;

    const QString settingsJson = m_settings->get("Ai/GoalAgentSettings", QString());
    GoalAgentSettings goalSettings;
    if (!settingsJson.isEmpty()) {
        goalSettings = GoalAgentSettings::fromJson(
            QJsonDocument::fromJson(settingsJson.toUtf8()).object());
    }

    const QString trimmed = name.trimmed();
    for (const auto &t : goalSettings.promptTemplates) {
        if (t.name.compare(trimmed, Qt::CaseInsensitive) == 0) {
            QMessageBox::information(this, tr("New Prompt Template"),
                tr("A template named \"%1\" already exists.").arg(t.name));
            return;
        }
    }

    GoalPromptTemplate tpl;
    tpl.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    tpl.name = trimmed;
    tpl.content = GoalAgentSettings::builtinPromptContent();
    goalSettings.promptTemplates.append(tpl);

    m_settings->setValue(QStringLiteral("Ai/GoalAgentSettings"),
                         QString::fromUtf8(QJsonDocument(goalSettings.toJson()).toJson(QJsonDocument::Compact)));
    populateTemplates();
    int idx = m_templateCombo->findData(tpl.id);
    if (idx >= 0) m_templateCombo->setCurrentIndex(idx);
}

void SendWithGoalDialog::onTemplateRename()
{
    const QString tplId = m_templateCombo->currentData().toString();
    if (tplId.isEmpty()) return;
    if (tplId == QLatin1String(GoalAgentSettings::kDefaultTemplateId)) return;

    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Rename Prompt Template"),
        tr("New name:"), QLineEdit::Normal, m_templateCombo->currentText(), &ok);
    if (!ok || name.trimmed().isEmpty()) return;

    const QString settingsJson = m_settings->get("Ai/GoalAgentSettings", QString());
    GoalAgentSettings goalSettings;
    if (!settingsJson.isEmpty()) {
        goalSettings = GoalAgentSettings::fromJson(
            QJsonDocument::fromJson(settingsJson.toUtf8()).object());
    }

    const QString trimmed = name.trimmed();
    for (const auto &t : goalSettings.promptTemplates) {
        if (t.id != tplId && t.name.compare(trimmed, Qt::CaseInsensitive) == 0) {
            QMessageBox::information(this, tr("Rename Prompt Template"),
                tr("A template named \"%1\" already exists.").arg(t.name));
            return;
        }
    }

    for (auto &tpl : goalSettings.promptTemplates) {
        if (tpl.id == tplId) {
            tpl.name = trimmed;
            break;
        }
    }

    m_settings->setValue(QStringLiteral("Ai/GoalAgentSettings"),
                         QString::fromUtf8(QJsonDocument(goalSettings.toJson()).toJson(QJsonDocument::Compact)));
    populateTemplates();
    int idx = m_templateCombo->findData(tplId);
    if (idx >= 0) m_templateCombo->setCurrentIndex(idx);
}

void SendWithGoalDialog::onTemplateEdit()
{
    const QString tplId = m_templateCombo->currentData().toString();
    if (tplId.isEmpty()) return;
    if (tplId == QLatin1String(GoalAgentSettings::kDefaultTemplateId)) return;

    const QString settingsJson = m_settings->get("Ai/GoalAgentSettings", QString());
    if (settingsJson.isEmpty()) return;
    QJsonParseError parseErr;
    auto doc = QJsonDocument::fromJson(settingsJson.toUtf8(), &parseErr);
    if (parseErr.error != QJsonParseError::NoError) return;
    GoalAgentSettings goalSettings = GoalAgentSettings::fromJson(doc.object());

    const GoalPromptTemplate *current = goalSettings.findTemplate(tplId);
    if (!current) return;

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Edit Prompt Template"));
    dlg.resize(640, 480);

    auto *layout = new QVBoxLayout(&dlg);

    auto *help = new QLabel(
        tr("Required placeholders: {{goal}}, {{conversation}}, {{iteration}}, "
           "{{maxIterations}}, {{criterionIndex}}, {{totalCriteria}}"), &dlg);
    help->setWordWrap(true);
    help->setStyleSheet(QStringLiteral("font-size: 11px; color: gray;"));
    layout->addWidget(help);

    auto *editor = new QPlainTextEdit(&dlg);
    editor->setPlainText(current->content);
    layout->addWidget(editor, 1);

    auto *btnBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    layout->addWidget(btnBox);
    connect(btnBox, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(btnBox, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return;

    for (auto &tpl : goalSettings.promptTemplates) {
        if (tpl.id == tplId) {
            tpl.content = editor->toPlainText();
            break;
        }
    }

    m_settings->setValue(QStringLiteral("Ai/GoalAgentSettings"),
                         QString::fromUtf8(QJsonDocument(goalSettings.toJson()).toJson(QJsonDocument::Compact)));
}

void SendWithGoalDialog::onTemplateDelete()
{
    const QString tplId = m_templateCombo->currentData().toString();
    if (tplId.isEmpty()) return;
    if (tplId == QLatin1String(GoalAgentSettings::kDefaultTemplateId)) {
        QMessageBox::information(this, tr("Delete Template"),
            tr("The built-in default template cannot be deleted."));
        return;
    }

    const auto answer = QMessageBox::question(this, tr("Delete Template"),
        tr("Delete template \"%1\"?").arg(m_templateCombo->currentText()),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) return;

    const QString settingsJson = m_settings->get("Ai/GoalAgentSettings", QString());
    GoalAgentSettings goalSettings;
    if (!settingsJson.isEmpty()) {
        goalSettings = GoalAgentSettings::fromJson(
            QJsonDocument::fromJson(settingsJson.toUtf8()).object());
    }

    goalSettings.promptTemplates.removeIf([&](const GoalPromptTemplate &t) {
        return t.id == tplId;
    });

    m_settings->setValue(QStringLiteral("Ai/GoalAgentSettings"),
                         QString::fromUtf8(QJsonDocument(goalSettings.toJson()).toJson(QJsonDocument::Compact)));
    populateTemplates();
}

bool SendWithGoalDialog::validate()
{
    QStringList criteria;
    for (auto *edit : m_criteriaEdits) {
        const QString text = edit->toPlainText().trimmed();
        if (!text.isEmpty())
            criteria.append(text);
    }
    if (criteria.isEmpty()) {
        m_errorLabel->setText(tr("At least one criterion is required."));
        m_errorLabel->show();
        return false;
    }
    if (m_agentCombo->currentData().toString().isEmpty()) {
        m_errorLabel->setText(tr("Select a goal-agent."));
        m_errorLabel->show();
        return false;
    }
    m_errorLabel->hide();
    return true;
}

void SendWithGoalDialog::onStart()
{
    if (!validate())
        return;
    accept();
}

SendWithGoalResult SendWithGoalDialog::result() const
{
    SendWithGoalResult r;
    for (auto *edit : m_criteriaEdits) {
        const QString text = edit->toPlainText().trimmed();
        if (!text.isEmpty())
            r.successCriteriaList.append(text);
    }
    r.agentId = m_agentCombo->currentData().toString();
    r.maxIterations = m_maxIterSpin->value();
    r.promptTemplateId = m_templateCombo->currentData().toString();
    return r;
}

QPlainTextEdit *SendWithGoalDialog::createCriterionEdit(const QString &text)
{
    auto *edit = new QPlainTextEdit(m_criteriaScroll->widget());
    edit->setPlainText(text);
    edit->setPlaceholderText(tr("Describe a success criterion..."));
    edit->setMinimumHeight(56);
    edit->setMaximumHeight(100);
    edit->setTabChangesFocus(true);
    // Insert before the trailing stretch
    const int insertIdx = m_criteriaLayout->count() - 1;
    m_criteriaLayout->insertWidget(insertIdx, edit);
    return edit;
}
