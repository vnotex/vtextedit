#include "vscodeinputmode.h"

#include <QInputDialog>
#include <QKeyEvent>

#include "inputmodeeditorinterface.h"

using namespace vte;

VscodeInputMode::VscodeInputMode(InputModeEditorInterface *p_interface)
    : AbstractInputMode(p_interface) {}

QString VscodeInputMode::name() const { return QStringLiteral("vscode"); }

InputMode VscodeInputMode::mode() const { return InputMode::VscodeMode; }

void VscodeInputMode::activate() {}

void VscodeInputMode::deactivate() {}

bool VscodeInputMode::handleKeyPress(QKeyEvent *p_event) {
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

    case Qt::Key_C:
      if (!m_interface->selection()) {
        copyCurrentLine(false);
        return true;
      }
      break;

    case Qt::Key_X:
      if (!m_interface->selection()) {
        copyCurrentLine(true);
        return true;
      }
      break;

    case Qt::Key_L:
      selectCurrentLine();
      return true;

    default:
      break;
    }
  } else if (p_event->modifiers() == (Qt::ShiftModifier | Qt::ControlModifier)) {
    switch (p_event->key()) {
    case Qt::Key_G:
      gotoLine();
      return true;

    case Qt::Key_K:
      deleteCurrentLine();
      return true;

    default:
      break;
    }
  } else if (p_event->modifiers() == Qt::AltModifier) {
    switch (p_event->key()) {
    case Qt::Key_Up:
      moveLineUp();
      return true;

    case Qt::Key_Down:
      moveLineDown();
      return true;

    default:
      break;
    }
  } else if (p_event->modifiers() == (Qt::ShiftModifier | Qt::AltModifier)) {
    switch (p_event->key()) {
    case Qt::Key_Up:
      duplicateLineUp();
      return true;

    case Qt::Key_Down:
      duplicateLineDown();
      return true;

    default:
      break;
    }
  }

  return false;
}

bool VscodeInputMode::stealShortcut(QKeyEvent *p_event) {
  Q_UNUSED(p_event);
  return false;
}

EditorMode VscodeInputMode::editorMode() const { return m_mode; }

void VscodeInputMode::focusIn() {}

void VscodeInputMode::focusOut() {}

void VscodeInputMode::preKeyPressDefaultHandle(QKeyEvent *p_event) { Q_UNUSED(p_event); }

void VscodeInputMode::postKeyPressDefaultHandle(QKeyEvent *p_event) { Q_UNUSED(p_event); }

void VscodeInputMode::enterInsertMode() {
  m_interface->setCaretStyle(CaretStyle::Line);
  m_interface->update();
  m_interface->setOverwriteMode(false);
  m_mode = EditorMode::NormalModeInsert;
}

void VscodeInputMode::enterOverwriteMode() {
  m_interface->setCaretStyle(CaretStyle::Half);
  m_interface->update();
  m_interface->setOverwriteMode(true);
  m_mode = EditorMode::NormalModeOverwrite;
}

void VscodeInputMode::commandCompleteNext() {
  if (m_interface->isCompletionActive()) {
    m_interface->completionNext(false);
  } else {
    m_interface->userInvokedCompletion(false);
  }
}

void VscodeInputMode::commandCompletePrevious() {
  if (m_interface->isCompletionActive()) {
    m_interface->completionNext(true);
  } else {
    m_interface->userInvokedCompletion(true);
  }
}

QSharedPointer<InputModeStatusWidget> VscodeInputMode::statusWidget() { return nullptr; }

void VscodeInputMode::gotoLine() {
  int currentLine = m_interface->cursorPosition().line() + 1;
  int maxLine = m_interface->lastLine() + 1;

  bool ok = false;
  int line = QInputDialog::getInt(nullptr, QWidget::tr("Go to Line"),
                                  QWidget::tr("Line number (1-%1):").arg(maxLine), currentLine, 1,
                                  maxLine, 1, &ok);
  if (ok) {
    m_interface->updateCursor(line - 1, 0);
  }
}

void VscodeInputMode::copyCurrentLine(bool p_cut) {
  int line = m_interface->cursorPosition().line();
  QString text = m_interface->line(line);
  m_interface->copyToClipboard(text + '\n');
  if (p_cut) {
    m_interface->removeLine(line);
  }
}

void VscodeInputMode::selectCurrentLine() {
  int line = m_interface->cursorPosition().line();
  int lastLine = m_interface->lastLine();

  if (line < lastLine) {
    m_interface->setSelection(line, 0, line + 1, 0);
  } else {
    int length = m_interface->lineLength(line);
    m_interface->setSelection(line, 0, line, length);
  }
}

void VscodeInputMode::moveLineUp() {
  int currentLine = m_interface->cursorPosition().line();
  int currentColumn = m_interface->cursorPosition().column();
  if (currentLine > 0) {
    QString currentLineText = m_interface->line(currentLine);
    QString aboveLineText = m_interface->line(currentLine - 1);

    m_interface->removeLine(currentLine);
    m_interface->removeLine(currentLine - 1);

    m_interface->insertLine(currentLine - 1, currentLineText);
    m_interface->insertLine(currentLine, aboveLineText);

    m_interface->updateCursor(currentLine - 1, currentColumn);
  }
}

void VscodeInputMode::moveLineDown() {
  int currentLine = m_interface->cursorPosition().line();
  int currentColumn = m_interface->cursorPosition().column();
  if (currentLine < m_interface->lastLine()) {
    QString currentLineText = m_interface->line(currentLine);
    QString belowLineText = m_interface->line(currentLine + 1);

    m_interface->removeLine(currentLine + 1);
    m_interface->removeLine(currentLine);

    m_interface->insertLine(currentLine, belowLineText);
    m_interface->insertLine(currentLine + 1, currentLineText);

    m_interface->updateCursor(currentLine + 1, currentColumn);
  }
}

void VscodeInputMode::duplicateLineUp() {
  int currentLine = m_interface->cursorPosition().line();
  int currentColumn = m_interface->cursorPosition().column();

  QString lineText = m_interface->line(currentLine);

  m_interface->insertLine(currentLine, lineText);

  m_interface->updateCursor(currentLine, currentColumn);
}

void VscodeInputMode::duplicateLineDown() {
  int currentLine = m_interface->cursorPosition().line();
  int currentColumn = m_interface->cursorPosition().column();

  QString lineText = m_interface->line(currentLine);

  m_interface->insertLine(currentLine + 1, lineText);

  m_interface->updateCursor(currentLine + 1, currentColumn);
}

void VscodeInputMode::deleteCurrentLine() {
  int currentLine = m_interface->cursorPosition().line();

  m_interface->removeLine(currentLine);

  if (currentLine > m_interface->lastLine()) {
    m_interface->updateCursor(m_interface->lastLine(), 0);
  } else {
    m_interface->updateCursor(currentLine, 0);
  }
}
