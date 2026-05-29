#include "SendWithGoalDialog.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "GoalConfigWidget.h"

SendWithGoalDialog::SendWithGoalDialog(AcpAgentRegistry *registry,
                                       ApplicationSettings *settings,
                                       QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Send with Goal"));
    setMinimumWidth(440);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(12);

    m_goalConfig = new GoalConfigWidget(registry, settings, this);
    mainLayout->addWidget(m_goalConfig);

    m_errorLabel = new QLabel(this);
    m_errorLabel->setStyleSheet(QStringLiteral("color: red; font-size: 12px;"));
    m_errorLabel->hide();
    mainLayout->addWidget(m_errorLabel);

    auto *footerLayout = new QHBoxLayout;
    footerLayout->addStretch();
    auto *cancelBtn = new QPushButton(tr("Cancel"), this);
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    footerLayout->addWidget(cancelBtn);

    m_startBtn = new QPushButton(tr("Start Goal"), this);
    m_startBtn->setDefault(true);
    m_startBtn->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return));
    connect(m_startBtn, &QPushButton::clicked, this, &SendWithGoalDialog::onStart);
    footerLayout->addWidget(m_startBtn);
    mainLayout->addLayout(footerLayout);
}

SendWithGoalDialog::~SendWithGoalDialog() = default;

bool SendWithGoalDialog::validate()
{
    const GoalConfigResult r = m_goalConfig->result();
    if (r.criteriaList.isEmpty()) {
        m_errorLabel->setText(tr("At least one criterion is required."));
        m_errorLabel->show();
        return false;
    }
    if (r.agentId.isEmpty()) {
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

SendWithGoalResult SendWithGoalDialog::goalResult() const
{
    const GoalConfigResult gcr = m_goalConfig->result();
    SendWithGoalResult r;
    r.successCriteriaList = gcr.criteriaList;
    r.agentId = gcr.agentId;
    r.maxIterations = gcr.maxIterations;
    r.promptTemplateId = gcr.promptTemplateId;
    return r;
}
