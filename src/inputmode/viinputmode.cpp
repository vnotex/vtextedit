#include "viinputmode.h"

#include <QDebug>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QStyleHints>

#include "inputmodeeditorinterface.h"

#include <katevi/emulatedcommandbar.h>
#include <katevi/inputmodemanager.h>
#include <katevi/interface/kateviconfig.h>

using namespace vte;

ViStatusBar::ViStatusBar(KateVi::EmulatedCommandBar *p_commandBar) : m_commandBar(p_commandBar) {
  auto mainLayout = new QHBoxLayout(this);
  mainLayout->setContentsMargins(0, 0, 0, 0);

  m_commandBar->hide();
  mainLayout->addWidget(m_commandBar);
  connect(m_commandBar, &KateVi::EmulatedCommandBar::hideMe, this, [this]() {
    m_commandBar->closed();
    m_commandBar->hide();
    emit commandBarHidden();
  });
  connect(m_commandBar, &KateVi::EmulatedCommandBar::showMe, this, [this]() {
    m_commandBar->show();
    emit commandBarShown();
  });

  m_keyStrokeIndicator = new QLabel("", this);
  mainLayout->addWidget(m_keyStrokeIndicator);
}

void ViStatusBar::setKeyStroke(const QString &p_keys) { m_keyStrokeIndicator->setText(p_keys); }

ViStatusWidget::ViStatusWidget(const QSharedPointer<ViStatusBar> &p_widget)
    : InputModeStatusWidget(nullptr), m_statusBar(p_widget) {
  connect(m_statusBar.data(), &ViStatusBar::commandBarHidden, this,
          &InputModeStatusWidget::focusOut);
  connect(m_statusBar.data(), &ViStatusBar::commandBarShown, this, &InputModeStatusWidget::focusIn);
}

QSharedPointer<QWidget> ViStatusWidget::widget() { return m_statusBar; }

ViInputMode::ViInputMode(InputModeEditorInterface *p_interface,
                         const QSharedPointer<KateVi::GlobalState> &p_viGlobal,
                         const QSharedPointer<KateViI::KateViConfig> &p_viConfig)
    : AbstractInputMode(p_interface), m_viGlobal(p_viGlobal), m_viConfig(p_viConfig),
      c_cursorFlashTime(QGuiApplication::styleHints()->cursorFlashTime()) {
  m_viModeManager.reset(new KateVi::InputModeManager(this, p_interface));
}

ViInputMode::~ViInputMode() {
  restoreCursorBlinking();

  if (m_viStatusBar) {
    Q_ASSERT(m_viEmulatedCommandBar && m_viEmulatedCommandBar->parent());
    Q_ASSERT(!m_viStatusBar->parent());
  }
}

QString ViInputMode::name() const { return QStringLiteral("vi"); }

InputMode ViInputMode::mode() const { return InputMode::ViMode; }

void ViInputMode::activate() {
  Q_ASSERT(!m_active);
  m_active = true;
  m_interface->clearSelection();
  m_interface->setCaretStyle(CaretStyle::Block);
  updateCursorBlinking();
}

void ViInputMode::deactivate() {
  Q_ASSERT(m_active);
  m_active = false;
  m_interface->setUndoMergeAllEdits(false);
  m_interface->setCaretStyle(CaretStyle::Line);
  restoreCursorBlinking();
}

bool ViInputMode::handleKeyPress(QKeyEvent *p_event) {
  if (m_nextKeyPressIsOverriddenShortcut) {
    // Ignore it since it has been handled via stealShortcut().
    m_nextKeyPressIsOverriddenShortcut = false;
    return true;
  }

  qDebug() << "ViInputMode handleKeyPress" << p_event;

  if (m_viModeManager->handleKeyPress(p_event)) {
    m_interface->notifyEditorModeChanged(editorMode());
    return true;
  }

  return false;
}

bool ViInputMode::stealShortcut(QKeyEvent *p_event) {
  if (!m_viConfig->stealShortcut()) {
    return false;
  }

  const bool isStolen = handleKeyPress(p_event);
  if (isStolen) {
    m_nextKeyPressIsOverriddenShortcut = true;
  }

  return isStolen;
}

KateVi::EmulatedCommandBar *ViInputMode::viModeEmulatedCommandBar() {
  if (!m_viEmulatedCommandBar) {
    m_viEmulatedCommandBar = new KateVi::EmulatedCommandBar(this, m_viModeManager.data());
    m_viEmulatedCommandBar->hide();
  }

  return m_viEmulatedCommandBar;
}

void ViInputMode::showViModeEmulatedCommandBar() { emit m_viEmulatedCommandBar->showMe(); }

KateVi::InputModeManager *ViInputMode::viInputModeManager() const { return m_viModeManager.data(); }

EditorMode ViInputMode::editorMode() const {
  return static_cast<EditorMode>(m_viModeManager->getCurrentViewMode());
}

bool ViInputMode::isActive() const { return m_active; }

KateVi::GlobalState *ViInputMode::globalState() const { return m_viGlobal.data(); }

KateViI::KateViConfig *ViInputMode::kateViConfig() const { return m_viConfig.data(); }

void ViInputMode::updateCursor(const KateViI::Cursor &p_cursor) {
  m_interface->updateCursor(p_cursor.line(), p_cursor.column());
}

void ViInputMode::setCaretStyle(KateViI::CaretStyle p_style) {
  if (m_caret != p_style) {
    m_caret = p_style;

    auto style = kateViCaretStyleToEditorCaretStyle(p_style);
    m_interface->setCaretStyle(style);
  }
}

CaretStyle ViInputMode::kateViCaretStyleToEditorCaretStyle(KateViI::CaretStyle p_style) {
  return static_cast<CaretStyle>(p_style);
}

int ViInputMode::linesDisplayed() { return m_interface->linesDisplayed(); }

void ViInputMode::focusIn() { updateCursorBlinking(); }

void ViInputMode::focusOut() { restoreCursorBlinking(); }

void ViInputMode::updateCursorBlinking() const {
  const int flashTime = c_cursorFlashTime > 0 ? c_cursorFlashTime : 1000;
  QGuiApplication::styleHints()->setCursorFlashTime(m_cursorBlinkingEnabled ? flashTime : 0);
}

void ViInputMode::restoreCursorBlinking() const {
  QGuiApplication::styleHints()->setCursorFlashTime(c_cursorFlashTime);
}

void ViInputMode::setCursorBlinkingEnabled(bool p_enabled) {
  if (m_cursorBlinkingEnabled != p_enabled) {
    m_cursorBlinkingEnabled = p_enabled;
    updateCursorBlinking();
  }
}

void ViInputMode::preKeyPressDefaultHandle(QKeyEvent *p_event) {
  if (needToStartEditSession(p_event)) {
    m_interface->editStart();
  }
}

void ViInputMode::postKeyPressDefaultHandle(QKeyEvent *p_event) {
  if (needToStartEditSession(p_event)) {
    m_interface->editEnd();
  }
}

bool ViInputMode::needToStartEditSession(const QKeyEvent *p_event) const {
  if (!m_interface->isUndoMergeAllEditsEnabled()) {
    return false;
  }

  switch (p_event->key()) {
  case Qt::Key_Left:
    Q_FALLTHROUGH();
  case Qt::Key_Right:
    Q_FALLTHROUGH();
  case Qt::Key_Up:
    Q_FALLTHROUGH();
  case Qt::Key_Down:
    Q_FALLTHROUGH();
  case Qt::Key_Delete:
    Q_FALLTHROUGH();
  case Qt::Key_Home:
    Q_FALLTHROUGH();
  case Qt::Key_End:
    Q_FALLTHROUGH();
  case Qt::Key_PageUp:
    Q_FALLTHROUGH();
  case Qt::Key_Insert:
    Q_FALLTHROUGH();
  case Qt::Key_PageDown:
    Q_FALLTHROUGH();
  case Qt::Key_Control:
    Q_FALLTHROUGH();
  case Qt::Key_Shift:
    Q_FALLTHROUGH();
  case Qt::Key_Meta:
    Q_FALLTHROUGH();
  case Qt::Key_Alt:
    return false;

  default:
    break;
  }

  return true;
}

QSharedPointer<InputModeStatusWidget> ViInputMode::statusWidget() {
  if (!m_statusWidget) {
    // Make sure it is init.
    viModeEmulatedCommandBar();
    if (!m_viStatusBar) {
      m_viStatusBar.reset(new ViStatusBar(m_viEmulatedCommandBar));
      m_viStatusBar->hide();
    }
    m_statusWidget.reset(new ViStatusWidget(m_viStatusBar));
  }
  return m_statusWidget;
}

bool ViInputMode::keyPress(QKeyEvent *p_event) { return handleKeyPress(p_event); }

void ViInputMode::updateKeyStroke() {
  if (!m_viStatusBar) {
    return;
  }

  QString keyStroke = m_viModeManager->getVerbatimKeys();
  if (m_viModeManager->isRecording()) {
    if (!keyStroke.isEmpty()) {
      keyStroke += QLatin1Char(' ');
    }
    keyStroke = QLatin1Char('(') + VTextEditTranslate::tr("Recording", "KateVi") + QLatin1Char(')');
  }

  m_viStatusBar->setKeyStroke(keyStroke);
}

void ViInputMode::changeViInputMode(KateVi::ViMode p_mode) {
  if (m_viModeManager->getCurrentViMode() == p_mode) {
    return;
  }

  switch (p_mode) {
  case KateVi::ViMode::InsertMode:
    m_viModeManager->startInsertMode();
    break;

  default:
    // TODO.
    return;
  }
}
