#include "normalinputmode.h"

#include <QKeyEvent>

#include "inputmodeeditorinterface.h"

using namespace vte;

NormalInputMode::NormalInputMode(InputModeEditorInterface *p_interface)
    : AbstractInputMode(p_interface) {}

QString NormalInputMode::name() const { return QStringLiteral("normal"); }

InputMode NormalInputMode::mode() const { return InputMode::NormalMode; }

void NormalInputMode::activate() {}

void NormalInputMode::deactivate() {}

bool NormalInputMode::handleKeyPress(QKeyEvent *p_event) {
  if (p_event->modifiers() == Qt::NoModifier) {
    switch (p_event->key()) {
    case Qt::Key_Insert:
      if (m_mode == EditorMode::NormalModeInsert) {
        enterOverwriteMode();
      } else {
        Q_ASSERT(m_mode == EditorMode::NormalModeOverwrite);
        enterInsertMode();
      }
      m_interface->notifyEditorModeChanged(editorMode());
      return true;

    case Qt::Key_Enter:
      Q_FALLTHROUGH();
    case Qt::Key_Return:
      if (m_interface->isCompletionActive()) {
        m_interface->completionExecute();
        return true;
      }
      break;

    default:
      break;
    }
  } else if (p_event->modifiers() == Qt::ControlModifier) {
    switch (p_event->key()) {
    case Qt::Key_Space:
      Q_FALLTHROUGH();
    case Qt::Key_N:
      commandCompleteNext();
      return true;

    case Qt::Key_P:
      commandCompletePrevious();
      return true;

    case Qt::Key_J:
      m_interface->scrollUp();
      return true;

    case Qt::Key_K:
      m_interface->scrollDown();
      return true;

    default:
      break;
    }
  }

  return false;
}

bool NormalInputMode::stealShortcut(QKeyEvent *p_event) {
  Q_UNUSED(p_event);
  return false;
}

EditorMode NormalInputMode::editorMode() const { return m_mode; }

void NormalInputMode::focusIn() {}

void NormalInputMode::focusOut() {}

void NormalInputMode::preKeyPressDefaultHandle(QKeyEvent *p_event) { Q_UNUSED(p_event); }

void NormalInputMode::postKeyPressDefaultHandle(QKeyEvent *p_event) { Q_UNUSED(p_event); }

void NormalInputMode::enterInsertMode() {
  m_interface->setCaretStyle(CaretStyle::Line);
  m_interface->update();
  m_interface->setOverwriteMode(false);
  m_mode = EditorMode::NormalModeInsert;
}

void NormalInputMode::enterOverwriteMode() {
  m_interface->setCaretStyle(CaretStyle::Half);
  m_interface->update();
  m_interface->setOverwriteMode(true);
  m_mode = EditorMode::NormalModeOverwrite;
}

void NormalInputMode::commandCompleteNext() {
  if (m_interface->isCompletionActive()) {
    m_interface->completionNext(false);
  } else {
    m_interface->userInvokedCompletion(false);
  }
}

void NormalInputMode::commandCompletePrevious() {
  if (m_interface->isCompletionActive()) {
    m_interface->completionNext(true);
  } else {
    m_interface->userInvokedCompletion(true);
  }
}

QSharedPointer<InputModeStatusWidget> NormalInputMode::statusWidget() { return nullptr; }
