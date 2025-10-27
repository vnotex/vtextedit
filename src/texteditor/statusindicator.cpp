#include "statusindicator.h"

#include <QAction>
#include <QActionGroup>
#include <QDebug>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QToolButton>
#include <QVariant>

#include "inputmodestatuswidget.h"
#include <utils/utils.h>

using namespace vte;

const char *StatusIndicator::c_cursorLabelProperty = "CursorLabel.StatusIndicator.vte";

const char *StatusIndicator::c_syntaxLabelProperty = "SyntaxLabel.StatusIndicator.vte";

const char *StatusIndicator::c_modeLabelProperty = "ModeLabel.StatusIndicator.vte";

StatusIndicator::StatusIndicator(QWidget *p_parent) : QWidget(p_parent) { setupUI(); }

StatusIndicator::~StatusIndicator() {
  if (m_inputModeWidget) {
    auto widget = m_inputModeWidget->widget();
    widget->hide();
    widget->setParent(nullptr);
  }
}

void StatusIndicator::setupUI() {
  auto mainLayout = new QHBoxLayout(this);
  auto margins = mainLayout->contentsMargins();
  margins.setTop(0);
  margins.setBottom(0);
  mainLayout->setContentsMargins(margins);

  // Cursor label.
  m_cursorLabel = new QLabel(this);
  m_cursorLabel->setProperty(c_cursorLabelProperty, true);
  m_cursorLabel->setText(generateCursorLabelText(1, 1, 0));
  mainLayout->addWidget(m_cursorLabel);

  // Add a stretch.
  mainLayout->addStretch();

  // Spell check button.
  {
    m_spellCheckBtn = new QToolButton(this);
    m_spellCheckBtn->setPopupMode(QToolButton::InstantPopup);
    Utils::removeMenuIndicator(m_spellCheckBtn);
    mainLayout->addWidget(m_spellCheckBtn);

    auto act = new QAction(tr("Spelling"), m_spellCheckBtn);
    m_spellCheckBtn->setDefaultAction(act);

    auto menu = new QMenu(m_spellCheckBtn);
    m_spellCheckBtn->setMenu(menu);
  }

  // Syntax label.
  m_syntaxLabel = new QLabel(this);
  m_syntaxLabel->setProperty(c_syntaxLabelProperty, true);
  mainLayout->addWidget(m_syntaxLabel);

  // Mode label.
  m_modeLabel = new QLabel(this);
  m_modeLabel->setProperty(c_modeLabelProperty, true);
  mainLayout->addWidget(m_modeLabel);
}

QString StatusIndicator::generateCursorLabelText(int p_lineCount, int p_line, int p_column) {
  QString cursorText = tr("Line: %1 - %2 (%3%)   Col: %4")
                           .arg(p_line)
                           .arg(p_lineCount)
                           .arg((int)(p_line * 1.0 / p_lineCount * 100), 2)
                           .arg(p_column, -3);
  return cursorText;
}

void StatusIndicator::updateCursor(int p_lineCount, int p_line, int p_column) {
  Q_ASSERT(p_lineCount > 0 && p_line <= p_lineCount);
  m_cursorLabel->setText(generateCursorLabelText(p_lineCount, p_line, p_column));
}

void StatusIndicator::updateSyntax(const QString &p_syntax) {
  m_syntaxLabel->setText(p_syntax.toUpper());
}

void StatusIndicator::updateMode(const EditorMode &p_mode) {
  m_modeLabel->setText(editorModeToString(p_mode));
}

void StatusIndicator::updateInputModeStatusWidget(
    const QSharedPointer<InputModeStatusWidget> &p_statusWidget) {
  if (m_inputModeWidget != p_statusWidget) {
    // Detach the old widget.
    if (m_inputModeWidget) {
      disconnect(m_inputModeWidget.data(), 0, this, 0);

      auto widget = m_inputModeWidget->widget();
      widget->hide();
      widget->setParent(nullptr);
    }

    m_inputModeWidget = p_statusWidget;
    if (m_inputModeWidget) {
      // Insert it into the layout.
      auto widget = m_inputModeWidget->widget();
      QHBoxLayout *mainLayout = static_cast<QHBoxLayout *>(layout());
      mainLayout->insertWidget(0, widget.data());
      widget->show();

      connect(m_inputModeWidget.data(), &InputModeStatusWidget::focusIn, this,
              &StatusIndicator::focusIn);
      connect(m_inputModeWidget.data(), &InputModeStatusWidget::focusOut, this,
              &StatusIndicator::focusOut);
    }
  }
}

void StatusIndicator::hideInputModeStatusWidget() {
  Q_ASSERT(m_inputModeWidget);
  m_inputModeWidget->widget()->hide();
}

const QSharedPointer<InputModeStatusWidget> &StatusIndicator::getInputModeStatusWidget() const {
  return m_inputModeWidget;
}

void StatusIndicator::updateSpellCheck(bool p_spellCheckEnabled, bool p_autoDetectLanguageEnabled,
                                       const QString &p_currentLanguage,
                                       const QMap<QString, QString> &p_dictionaries) {
  auto menu = m_spellCheckBtn->menu();
  if (!menu->isEmpty()) {
    qWarning() << "updateSpellCheck() is called twice";
    return;
  }

  {
    m_spellCheckEnabled = p_spellCheckEnabled;
    auto act = menu->addAction(tr("Enable Spell Check"));
    act->setCheckable(true);
    act->setChecked(p_spellCheckEnabled);
    connect(act, &QAction::triggered, this, [this](bool p_checked) {
      m_spellCheckEnabled = p_checked;
      signalSpellCheckChanged();
    });
  }

  {
    m_autoDetectLanguageEnabled = p_autoDetectLanguageEnabled;
    auto act = menu->addAction(tr("Auto Detect Language"));
    act->setCheckable(true);
    act->setChecked(p_autoDetectLanguageEnabled);
    connect(act, &QAction::triggered, this, [this](bool p_checked) {
      m_autoDetectLanguageEnabled = p_checked;
      signalSpellCheckChanged();
    });
  }

  menu->addSeparator();

  m_defaultSpellCheckLanguage = p_currentLanguage;
  if (p_dictionaries.isEmpty()) {
    auto act = menu->addAction(tr("No Dictionary Found"));
    act->setEnabled(false);
  } else {
    auto ag = new QActionGroup(menu);
    for (auto it = p_dictionaries.begin(); it != p_dictionaries.end(); ++it) {
      auto act = menu->addAction(it.key());
      act->setData(it.value());
      act->setCheckable(true);
      ag->addAction(act);
      if (it.value() == p_currentLanguage) {
        act->setChecked(true);
      }
    }

    connect(ag, &QActionGroup::triggered, this, [this](QAction *p_act) {
      m_defaultSpellCheckLanguage = p_act->data().toString();
      signalSpellCheckChanged();
    });
  }
}

void StatusIndicator::signalSpellCheckChanged() {
  emit spellCheckChanged(m_spellCheckEnabled, m_autoDetectLanguageEnabled,
                         m_defaultSpellCheckLanguage);
}
