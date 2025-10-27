/*  This file is part of the KDE libraries and the Kate part.
 *
 *  Copyright (C) 2008-2011 Erlend Hamberg <ehamberg@gmail.com>
 *  Copyright (C) 2011 Svyatoslav Kuzmich <svatoslav1@gmail.com>
 *  Copyright (C) 2012 - 2013 Simon St James <kdedevel@etotheipiplusone.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 *  You should have received a copy of the GNU Library General Public License
 *  along with this library; see the file COPYING.LIB.  If not, write to
 *  the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301, USA.
 */

#include <completionrecorder.h>
#include <completionreplayer.h>
#include <katevi/global.h>
#include <katevi/inputmodemanager.h>
#include <katevi/interface/kateviconfig.h>
#include <katevi/interface/katevieditorinterface.h>
#include <katevi/interface/kateviinputmode.h>
#include <keyparser.h>
#include <lastchangerecorder.h>
#include <macrorecorder.h>
#include <marks.h>
#include <modes/insertvimode.h>
#include <viutils.h>

#include <QRegularExpression>

using namespace KateVi;

InsertViMode::InsertViMode(InputModeManager *viInputModeManager,
                           KateViI::KateViEditorInterface *editorInterface)
    : ModeBase() {
  m_interface = editorInterface;
  m_viInputModeManager = viInputModeManager;

  m_interface->connectTextInserted([this](const KateViI::Range &p_range) {
    if (!m_viInputModeManager->inputAdapter()->isActive()) {
      return;
    }
    textInserted(p_range);
  });
}

InsertViMode::~InsertViMode() {}

bool InsertViMode::commandInsertFromAbove() {
  KateViI::Cursor c(m_interface->cursorPosition());
  if (c.line() <= 0) {
    return false;
  }

  QString line = m_interface->line(c.line() - 1);
  const int tabWidth = m_viInputModeManager->inputAdapter()->kateViConfig()->tabWidth();
  const int virtualCursorColumn = m_interface->toVirtualColumn(c.line(), c.column(), tabWidth);
  QChar ch = getCharAtVirtualColumn(line, virtualCursorColumn, tabWidth);

  if (ch == QChar::Null) {
    return false;
  }

  return m_interface->insertText(c, ch);
}

bool InsertViMode::commandInsertFromBelow() {
  KateViI::Cursor c(m_interface->cursorPosition());
  if (c.line() >= m_interface->lines() - 1) {
    return false;
  }

  QString line = m_interface->line(c.line() + 1);
  const int tabWidth = m_viInputModeManager->inputAdapter()->kateViConfig()->tabWidth();
  const int virtualCursorColumn = m_interface->toVirtualColumn(c.line(), c.column(), tabWidth);
  QChar ch = getCharAtVirtualColumn(line, virtualCursorColumn, tabWidth);

  if (ch == QChar::Null) {
    return false;
  }

  return m_interface->insertText(c, ch);
}

bool InsertViMode::commandDeleteWord() {
  KateViI::Cursor c1(m_interface->cursorPosition());
  KateViI::Cursor c2;

  c2 = findPrevWordStart(c1.line(), c1.column());

  if (c2.line() != c1.line()) {
    if (c1.column() == 0) {
      c2.setColumn(m_interface->line(c2.line()).length());
    } else {
      c2.setColumn(0);
      c2.setLine(c2.line() + 1);
    }
  }

  Range r(c2, c1, ExclusiveMotion);
  return deleteRange(r, CharWise, false);
}

bool InsertViMode::commandDeleteLine() {
  KateViI::Cursor c(m_interface->cursorPosition());
  Range r(c.line(), 0, c.line(), c.column(), ExclusiveMotion);

  if (c.column() == 0) {
    // Try to move the current line to the end of the previous line.
    if (c.line() == 0) {
      return true;
    } else {
      r.startColumn = m_interface->line(c.line() - 1).length();
      r.startLine--;
    }
  } else {
    /*
     * Remove backwards until the first non-space character. If no
     * non-space was found, remove backwards to the first column.
     */
    QRegularExpression nonSpace(QLatin1String("\\S"));
    r.startColumn = getLine().indexOf(nonSpace);
    if (r.startColumn == -1 || r.startColumn >= c.column()) {
      r.startColumn = 0;
    }
  }
  return deleteRange(r, CharWise, false);
}

bool InsertViMode::commandDeleteCharBackward() {
  KateViI::Cursor c(m_interface->cursorPosition());

  Range r(c.line(), c.column() - getCount(), c.line(), c.column(), ExclusiveMotion);

  if (c.column() == 0) {
    if (c.line() == 0) {
      return true;
    } else {
      r.startColumn = m_interface->line(c.line() - 1).length();
      r.startLine--;
    }
  }

  return deleteRange(r, CharWise);
}

bool InsertViMode::commandNewLine() {
  m_interface->newLine();
  return true;
}

bool InsertViMode::commandIndent() {
  KateViI::Cursor c(m_interface->cursorPosition());
  m_interface->indent(KateViI::Range(c.line(), 0, c.line(), 0), 1);
  return true;
}

bool InsertViMode::commandUnindent() {
  KateViI::Cursor c(m_interface->cursorPosition());
  m_interface->indent(KateViI::Range(c.line(), 0, c.line(), 0), -1);
  return true;
}

bool InsertViMode::commandToFirstCharacterInFile() {
  KateViI::Cursor c(0, 0);
  updateCursor(c);
  return true;
}

bool InsertViMode::commandToLastCharacterInFile() {
  int lines = m_interface->lines() - 1;
  KateViI::Cursor c(lines, m_interface->lineLength(lines));
  updateCursor(c);
  return true;
}

bool InsertViMode::commandMoveOneWordLeft() {
  KateViI::Cursor c(m_interface->cursorPosition());
  c = findPrevWordStart(c.line(), c.column());

  if (!c.isValid()) {
    c = KateViI::Cursor(0, 0);
  }

  updateCursor(c);
  return true;
}

bool InsertViMode::commandMoveOneWordRight() {
  KateViI::Cursor c(m_interface->cursorPosition());
  c = findNextWordStart(c.line(), c.column());

  if (!c.isValid()) {
    c = m_interface->documentEnd();
  }

  updateCursor(c);
  return true;
}

bool InsertViMode::commandCompleteNext() {
  if (m_interface->isCompletionActive()) {
    m_interface->completionNext(false);
  } else {
    m_interface->userInvokedCompletion(false);
  }
  return true;
}

bool InsertViMode::commandCompletePrevious() {
  if (m_interface->isCompletionActive()) {
    m_interface->completionNext(true);
  } else {
    m_interface->userInvokedCompletion(true);
  }
  return true;
}

bool InsertViMode::commandInsertContentOfRegister() {
  KateViI::Cursor c(m_interface->cursorPosition());
  KateViI::Cursor cAfter = c;
  QChar reg = getChosenRegister(m_register);

  OperationMode m = getRegisterFlag(reg);
  QString textToInsert = getRegisterContent(reg);

  if (textToInsert.isNull()) {
    error(tr("Nothing in register %1").arg(reg));
    return false;
  }

  if (m == LineWise) {
    textToInsert.chop(1);                           // remove the last \n
    c.setColumn(m_interface->lineLength(c.line())); // paste after the current line and ...
    textToInsert.prepend(QLatin1Char('\n')); // ... prepend a \n, so the text starts on a new line

    cAfter.setLine(cAfter.line() + 1);
    cAfter.setColumn(0);
  } else {
    cAfter.setColumn(cAfter.column() + textToInsert.length());
  }

  m_interface->insertText(c, textToInsert, m == Block);

  updateCursor(cAfter);

  return true;
}

// Start Normal mode just for one command and return to Insert mode
bool InsertViMode::commandSwitchToNormalModeForJustOneCommand() {
  m_viInputModeManager->setTemporaryNormalMode(true);
  m_viInputModeManager->changeViMode(ViMode::NormalMode);
  const KateViI::Cursor cursorPos = m_interface->cursorPosition();
  // If we're at end of the line, move the cursor back one step, as in Vim.
  if (m_interface->line(cursorPos.line()).length() == cursorPos.column()) {
    m_interface->setCursorPosition(KateViI::Cursor(cursorPos.line(), cursorPos.column() - 1));
  }
  m_viInputModeManager->inputAdapter()->setCaretStyle(KateViI::CaretStyle::Block);
  m_viInputModeManager->inputAdapter()->setCursorBlinkingEnabled(false);
  m_interface->notifyViewModeChanged(m_interface->viewMode());
  m_interface->update();
  return true;
}

/**
 * checks if the key is a valid command
 * @return true if a command was completed and executed, false otherwise
 */
bool InsertViMode::handleKeyPress(const QKeyEvent *e) {
  if (m_keys.isEmpty() && !m_waitingRegister) {
    if (e->modifiers() == Qt::NoModifier) {
      switch (e->key()) {
      case Qt::Key_Escape:
        leaveInsertMode();
        return true;
      case Qt::Key_Insert:
        startReplaceMode();
        return true;
#if 0
            // Just let Qt handle these keys.
            case Qt::Key_Left:
                m_view->cursorLeft();
                return true;
            case Qt::Key_Right:
                m_view->cursorRight();
                return true;
            case Qt::Key_Up:
                m_view->up();
                return true;
            case Qt::Key_Down:
                m_view->down();
                return true;
            case Qt::Key_Delete:
                m_view->keyDelete();
                return true;
            case Qt::Key_Home:
                m_view->home();
                return true;
            case Qt::Key_End:
                m_view->end();
                return true;
            case Qt::Key_PageUp:
                m_view->pageUp();
                return true;
            case Qt::Key_PageDown:
                m_view->pageDown();
                return true;
#endif
      case Qt::Key_Enter:
      case Qt::Key_Return:
        if (m_interface->isCompletionActive() &&
            !m_viInputModeManager->macroRecorder()->isReplaying() &&
            !m_viInputModeManager->lastChangeRecorder()->isReplaying()) {
          // Filter out Enter/ Return's that trigger a completion when recording
          // macros/ last change stuff; they will be replaced with the special
          // code "ctrl-space". (This is why there is a
          // "!m_viInputModeManager->isReplayingMacro()" above.)
          m_viInputModeManager->doNotLogCurrentKeypress();

          m_isExecutingCompletion = true;
          m_textInsertedByCompletion.clear();
          m_interface->completionExecute();
          completionFinished();
          m_isExecutingCompletion = false;
          return true;
        }
        Q_FALLTHROUGH();
      default:
        return false;
      }
    } else if (ViUtils::isControlModifier(e->modifiers())) {
      switch (e->key()) {
      case Qt::Key_BracketLeft:
        leaveInsertMode();
        return true;

      case Qt::Key_Space:
        // We use Ctrl-space as a special code in macros/ last change, which
        // means: if replaying a macro/ last change, fetch and execute the next
        // completion for this macro/ last change ...
        if (!m_viInputModeManager->macroRecorder()->isReplaying() &&
            !m_viInputModeManager->lastChangeRecorder()->isReplaying()) {
          commandCompleteNext();
          // ... therefore, we should not record ctrl-space indiscriminately.
          m_viInputModeManager->doNotLogCurrentKeypress();
        } else {
          m_viInputModeManager->completionReplayer()->replay();
        }
        return true;

      case Qt::Key_C:
        leaveInsertMode(true);
        return true;
      case Qt::Key_D:
        commandUnindent();
        return true;
      case Qt::Key_E:
        if (m_interface->isCompletionActive()) {
          m_interface->abortCompletion();
          // Do not record Ctrl-E here.
          m_viInputModeManager->doNotLogCurrentKeypress();
        } else {
          commandInsertFromBelow();
        }
        return true;
      case Qt::Key_N:
        if (!m_viInputModeManager->macroRecorder()->isReplaying()) {
          commandCompleteNext();
        }
        return true;
      case Qt::Key_P:
        if (!m_viInputModeManager->macroRecorder()->isReplaying()) {
          commandCompletePrevious();
        }
        return true;
      case Qt::Key_T:
        commandIndent();
        return true;
      case Qt::Key_W:
        commandDeleteWord();
        return true;
      case Qt::Key_U:
        return commandDeleteLine();
      case Qt::Key_J:
        commandNewLine();
        return true;
      case Qt::Key_H:
        commandDeleteCharBackward();
        return true;
      case Qt::Key_Y:
        commandInsertFromAbove();
        return true;
      case Qt::Key_O:
        commandSwitchToNormalModeForJustOneCommand();
        return true;
      case Qt::Key_Home:
        commandToFirstCharacterInFile();
        return true;
      case Qt::Key_R:
        m_waitingRegister = true;
        return true;
      case Qt::Key_End:
        commandToLastCharacterInFile();
        return true;
      case Qt::Key_Left:
        commandMoveOneWordLeft();
        return true;
      case Qt::Key_Right:
        commandMoveOneWordRight();
        return true;
      default:
        return false;
      }
    }

    return false;
  } else if (m_waitingRegister) {
    // ignore modifier keys alone
    if (ViUtils::isModifier(e->key())) {
      return false;
    }

    QChar key = KeyParser::self()->KeyEventToQChar(*e);
    key = key.toLower();
    m_waitingRegister = false;

    // is it register ?
    // TODO: add registers such as '/'. See :h <c-r>
    if ((key >= QLatin1Char('0') && key <= QLatin1Char('9')) ||
        (key >= QLatin1Char('a') && key <= QLatin1Char('z')) || key == QLatin1Char('_') ||
        key == QLatin1Char('+') || key == QLatin1Char('*') || key == QLatin1Char('"')) {
      m_register = key;
    } else {
      return false;
    }

    commandInsertContentOfRegister();
    return true;
  }

  return false;
}

// leave insert mode when esc, etc, is pressed. if leaving block
// prepend/append, the inserted text will be added to all block lines. if
// ctrl-c is used to exit insert mode this is not done.
void InsertViMode::leaveInsertMode(bool force) {
  m_interface->abortCompletion();
  if (!force) {
    if (m_blockInsert != None) { // block append/prepend
      KATEVI_NIY;
      /*
      // make sure cursor haven't been moved
      if (m_blockRange.startLine == m_view->cursorPosition().line()) {
          int start, len;
          QString added;
          KTextEditor::Cursor c;

          switch (m_blockInsert) {
          case Append:
          case Prepend:
              if (m_blockInsert == Append) {
                  start = m_blockRange.endColumn + 1;
              } else {
                  start = m_blockRange.startColumn;
              }

              len = m_view->cursorPosition().column() - start;
              added = getLine().mid(start, len);

              c = KTextEditor::Cursor(m_blockRange.startLine, start);
              for (int i = m_blockRange.startLine + 1; i <=
      m_blockRange.endLine; i++) { c.setLine(i); doc()->insertText(c, added);
              }
              break;
          case AppendEOL:
              start = m_eolPos;
              len = m_view->cursorPosition().column() - start;
              added = getLine().mid(start, len);

              c = KTextEditor::Cursor(m_blockRange.startLine, start);
              for (int i = m_blockRange.startLine + 1; i <=
      m_blockRange.endLine; i++) { c.setLine(i);
                  c.setColumn(doc()->lineLength(i));
                  doc()->insertText(c, added);
              }
              break;
          default:
              error(QStringLiteral("not supported"));
          }
      }

      m_blockInsert = None;
      */
    } else {
      const QString added = m_interface->getText(KateViI::Range(
          m_viInputModeManager->marks()->getStartEditYanked(), m_interface->cursorPosition()));

      if (m_count > 1) {
        for (unsigned int i = 0; i < m_count - 1; i++) {
          if (m_countedRepeatsBeginOnNewLine) {
            m_interface->newLine();
          }
          m_interface->insertText(m_interface->cursorPosition(), added);
        }
      }
    }
  }

  m_countedRepeatsBeginOnNewLine = false;
  startNormalMode();
}

void InsertViMode::setBlockPrependMode(Range blockRange) {
  // ignore if not more than one line is selected
  if (blockRange.startLine != blockRange.endLine) {
    m_blockInsert = Prepend;
    m_blockRange = blockRange;
  }
}

void InsertViMode::setBlockAppendMode(Range blockRange, BlockInsert b) {
  Q_ASSERT(b == Append || b == AppendEOL);

  // ignore if not more than one line is selected
  if (blockRange.startLine != blockRange.endLine) {
    m_blockRange = blockRange;
    m_blockInsert = b;
    if (b == AppendEOL) {
      m_eolPos = m_interface->lineLength(m_blockRange.startLine);
    }
  } else {
    qWarning() << "Cursor moved. Ignoring block append/prepend.";
  }
}

void InsertViMode::completionFinished() {
  Completion::CompletionType completionType = Completion::PlainText;
  if (m_interface->cursorPosition() != m_textInsertedByCompletionEndPos) {
    completionType = Completion::FunctionWithArgs;
  } else if (m_textInsertedByCompletion.endsWith(QLatin1String("()")) ||
             m_textInsertedByCompletion.endsWith(QLatin1String("();"))) {
    completionType = Completion::FunctionWithoutArgs;
  }

  const bool removeTail =
      m_viInputModeManager->inputAdapter()->kateViConfig()->wordCompletionRemoveTail();
  m_viInputModeManager->completionRecorder()->logCompletionEvent(
      Completion(m_textInsertedByCompletion, removeTail, completionType));
}

void InsertViMode::textInserted(const KateViI::Range &p_range) {
  if (m_isExecutingCompletion) {
    m_textInsertedByCompletion += m_interface->getText(p_range);
    m_textInsertedByCompletionEndPos = p_range.end();
  }
}
