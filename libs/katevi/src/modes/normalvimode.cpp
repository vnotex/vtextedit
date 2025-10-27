/*  This file is part of the KDE libraries and the Kate part.
 *
 *  Copyright (C) 2008-2009 Erlend Hamberg <ehamberg@gmail.com>
 *  Copyright (C) 2008 Evgeniy Ivanov <powerfox@kde.ru>
 *  Copyright (C) 2009 Paul Gideon Dann <pdgiddie@gmail.com>
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
#include <command.h>
#include <history.h>
#include <katevi/emulatedcommandbar.h>
#include <katevi/global.h>
#include <katevi/globalstate.h>
#include <katevi/inputmodemanager.h>
#include <katevi/interface/kateviconfig.h>
#include <katevi/interface/katevieditorinterface.h>
#include <katevi/interface/kateviinputmode.h>
#include <keymapper.h>
#include <keyparser.h>
#include <lastchangerecorder.h>
#include <macrorecorder.h>
#include <marks.h>
#include <modes/insertvimode.h>
#include <modes/normalvimode.h>
#include <modes/replacevimode.h>
#include <modes/visualvimode.h>
#include <motion.h>
#include <registers.h>
#include <searcher.h>
#include <viutils.h>

#include <QApplication>
#include <QDebug>
#include <QList>
#include <QRegularExpression>

using namespace KateVi;

#define ADDCMD(STR, FUNC, FLGS)                                                                    \
  m_commands.push_back(new Command(this, QStringLiteral(STR), &NormalViMode::FUNC, FLGS));

#define ADDMOTION(STR, FUNC, FLGS)                                                                 \
  m_motions.push_back(new Motion(this, QStringLiteral(STR), &NormalViMode::FUNC, FLGS));

NormalViMode::NormalViMode(InputModeManager *viInputModeManager,
                           KateViI::KateViEditorInterface *editorInterface)
    : ModeBase() {
  m_interface = editorInterface;
  m_viInputModeManager = viInputModeManager;
  m_stickyColumn = -1;
  m_lastMotionWasVisualLineUpOrDown = false;
  m_currentMotionWasVisualLineUpOrDown = false;

  // FIXME: make configurable
  m_extraWordCharacters = QString();
  m_matchingItems[QStringLiteral("/*")] = QStringLiteral("*/");
  m_matchingItems[QStringLiteral("*/")] = QStringLiteral("-/*");

  m_matchItemRegex = generateMatchingItemRegex();

  initializeCommands();
  resetParser(); // initialise with start configuration

  /*
  connect(doc()->undoManager(), SIGNAL(undoStart(KTextEditor::Document*)),
          this, SLOT(undoBeginning()));
  connect(doc()->undoManager(), SIGNAL(undoEnd(KTextEditor::Document*)),
          this, SLOT(undoEnded()));

  updateYankHighlightAttrib();
  connect(view, SIGNAL(configChanged()),
          this, SLOT(updateYankHighlightAttrib()));
  connect(doc(),
  SIGNAL(aboutToInvalidateMovingInterfaceContent(KTextEditor::Document*)), this,
  SLOT(clearYankHighlight())); connect(doc(),
  SIGNAL(aboutToDeleteMovingInterfaceContent(KTextEditor::Document*)), this,
  SLOT(aboutToDeleteMovingInterfaceContent()));
  */
}

NormalViMode::~NormalViMode() {
  qDeleteAll(m_commands);
  qDeleteAll(m_motions);
}

/**
 * parses a key stroke to check if it's a valid (part of) a command
 * @return true if a command was completed and executed, false otherwise
 */
bool NormalViMode::handleKeyPress(const QKeyEvent *e) {
  const int keyCode = e->key();
  const auto modifiers = e->modifiers();

  // Ignore modifier keys alone.
  if (ViUtils::isModifier(keyCode)) {
    return false;
  }

  // Ignore skipped keys.
  if (m_viInputModeManager->kateViConfig()->shouldSkipKey(keyCode, modifiers)) {
    return false;
  }

  clearYankHighlight();

  auto adapter = m_viInputModeManager->inputAdapter();
  if (keyCode == Qt::Key_Escape ||
      (keyCode == Qt::Key_C && ViUtils::isControlModifier(modifiers)) ||
      (keyCode == Qt::Key_BracketLeft && ViUtils::isControlModifier(modifiers))) {
    adapter->setCaretStyle(KateViI::Block);
    m_pendingResetIsDueToExit = true;
    // Vim in weird as if we e.g. i<ctrl-o><ctrl-c> it claims (in the status
    // bar) to still be in insert mode, but behaves as if it's in normal mode.
    // I'm treating the status bar thing as a bug and just exiting insert mode
    // altogether.
    m_viInputModeManager->setTemporaryNormalMode(false);
    reset();
    return true;
  }

  const QChar key = KeyParser::self()->KeyEventToQChar(*e);
  const QChar lastChar = m_keys.isEmpty() ? QChar::Null : m_keys.at(m_keys.size() - 1);
  const bool isWaitingForRegisterOrChar = waitingForRegisterOrCharToSearch();

  // Use replace caret when reading a character for "r"
  if (key == QLatin1Char('r') && !isWaitingForRegisterOrChar) {
    adapter->setCaretStyle(KateViI::Underline);
  }

  appendToKeysVerbatim(KeyParser::self()->decodeKeySequence(key));

  // Accumulate the count.
  if ((keyCode >= Qt::Key_0 && keyCode <= Qt::Key_9 && lastChar != QLatin1Char('"')) // key 0-9
      && (m_countTemp != 0 || keyCode != Qt::Key_0) // first digit can't be 0
      && (!isWaitingForRegisterOrChar)              // Not in the middle of "find char"
                                                    // motions or replacing char.
      && modifiers == Qt::NoModifier) {

    m_countTemp *= 10;
    m_countTemp += keyCode - Qt::Key_0;
    return true;
  } else if (m_countTemp != 0) {
    m_count = getCount() * m_countTemp;
    m_countTemp = 0;
    m_iscounted = true;
  }

  m_keys.append(key);

  if (m_viInputModeManager->macroRecorder()->isRecording() && key == QLatin1Char('q')) {
    // Need to special case this "finish macro" q, as the "begin macro" q
    // needs a parameter whereas the finish macro does not.
    m_viInputModeManager->macroRecorder()->stop();
    resetParser();
    return true;
  }

  /* TODO
  if ((key == QLatin1Char('/') || key == QLatin1Char('?')) &&
  !isWaitingForRegisterOrChar) {
      // Special case for "/" and "?": these should be motions, but this is
  complicated by
      // the fact that the user must interact with the search bar before the
  range of the
      // motion can be determined.
      // We hack around this by showing the search bar immediately, and, when
  the user has
      // finished interacting with it, have the search bar send a "synthetic"
  keypresses
      // that will either abort everything (if the search was aborted) or
  "complete" the motion
      // otherwise.
      m_positionWhenIncrementalSearchBegan = m_view->cursorPosition();
      if (key == QLatin1Char('/')) {
          commandSearchForward();
      } else {
          commandSearchBackward();
      }
      return true;
  }
  */

  // Special case: "cw" and "cW" work the same as "ce" and "cE" if the cursor is
  // on a non-blank.  This is because Vim interprets "cw" as change-word, and a
  // word does not include the following white space. (:help cw in vim)
  if ((m_keys == QLatin1String("cw") || m_keys == QLatin1String("cW")) &&
      !getCharUnderCursor().isSpace()) {
    rewriteKeysInCWCase();
  }

  if (m_keys[0].toLatin1() == Qt::Key_QuoteDbl) {
    return assignRegisterFromKeys();
  }

  updateMatchingCommands();

  // this indicates where in the command string one should start looking for a
  // motion command
  const int checkFrom =
      m_awaitingMotionOrTextObject.isEmpty() ? 0 : m_awaitingMotionOrTextObject.top();

  // Use operator-pending caret when reading a motion for an operator
  // in normal mode. We need to check that we are indeed in normal mode
  // since visual mode inherits from it.
  if (m_viInputModeManager->getCurrentViMode() == ViMode::NormalMode &&
      !m_awaitingMotionOrTextObject.isEmpty()) {
    adapter->setCaretStyle(KateViI::Half);
  }

  // look for matching motion commands from position 'checkFrom'
  bool motionExecuted = updateMatchingMotions(checkFrom);

  // Check again if we are waiting for register or char to search.
  if (waitingForRegisterOrCharToSearch()) {
    // If we are waiting for a char to search or a new register,
    // don't translate next character; we need the actual character
    // so that e.g. 'ab' is translated to 'fb'
    // if the mappings 'a' -> 'f' and 'b' -> something else exist.
    m_viInputModeManager->keyMapper()->setDoNotMapNextKeyPress();
  }

  if (motionExecuted) {
    return true;
  }

  // If we have only one match, check if it is a perfect match and if so,
  // execute it if it's not waiting for a motion or a text object.
  if (m_matchingCommands.size() == 1) {
    const int cmdIdx = m_matchingCommands.at(0);
    if (m_commands.at(cmdIdx)->matchesExact(m_keys) && !m_commands.at(cmdIdx)->needsMotion()) {
      if (m_viInputModeManager->getCurrentViMode() == ViMode::NormalMode) {
        m_viInputModeManager->inputAdapter()->setCaretStyle(KateViI::Block);
      }

      Command *cmd = m_commands.at(cmdIdx);
      executeCommand(cmd);

      // Check if reset() should be called.
      // Some commands in visual mode should not end visual mode.
      if (cmd->shouldReset()) {
        reset();
        m_interface->setBlockSelection(false);
      }

      resetParser();
      return true;
    }
  } else if (m_matchingCommands.size() == 0 && m_matchingMotions.size() == 0) {
    resetParser();
    // A bit ugly:  we haven't made use of the key event,
    // but don't want "typeable" keypresses (e.g. a, b, 3, etc) to be marked
    // as unused as they will then be added to the document, but we don't
    // want to swallow all keys in case this was a shortcut.
    // So say we made use of it if and only if it was *not* a shortcut.
    return e->type() != QEvent::ShortcutOverride;
  }

  m_matchingMotions.clear();
  return true;
}

/**
 * (re)set to start configuration. This is done when a command is completed
 * executed or when a command is aborted
 */
void NormalViMode::resetParser() {
  m_keys.clear();
  clearKeysVerbatim();
  m_count = 0;
  m_oneTimeCountOverride = -1;
  m_iscounted = false;
  m_countTemp = 0;
  m_register = QChar::Null;
  m_findWaitingForChar = false;
  m_matchingCommands.clear();
  m_matchingMotions.clear();
  m_awaitingMotionOrTextObject.clear();
  m_motionOperatorIndex = 0;

  m_commandWithMotion = false;
  m_linewiseCommand = true;
  m_deleteCommand = false;

  m_commandShouldKeepSelection = false;

  m_currentChangeEndMarker = KateViI::Cursor::invalid();

  auto viMode = m_viInputModeManager->getCurrentViMode();
  if (viMode == ViMode::NormalMode || viMode == ViMode::VisualLineMode ||
      viMode == ViMode::VisualBlockMode || viMode == ViMode::VisualMode) {
    m_viInputModeManager->inputAdapter()->setCaretStyle(KateViI::Block);
    m_viInputModeManager->inputAdapter()->setCursorBlinkingEnabled(false);
  }
}

// Reset the command parser.
void NormalViMode::reset() {
  resetParser();
  m_commandRange.startLine = -1;
  m_commandRange.startColumn = -1;
}

void NormalViMode::beginMonitoringDocumentChanges() {
  // TODO: Do we need to monitor the document change in normal mode?
}

void NormalViMode::executeCommand(const Command *cmd) {
  if (m_interface->isReadOnly()) {
    return;
  }

  const ViMode originalViMode = m_viInputModeManager->getCurrentViMode();

  cmd->execute();

  // if normal mode was started by using Ctrl-O in insert mode,
  // it's time to go back to insert mode.
  if (m_viInputModeManager->getTemporaryNormalMode()) {
    startInsertMode();
    m_interface->update();
  }

  // if the command was a change, and it didn't enter insert mode, store the key
  // presses so that they can be repeated with '.'
  const ViMode newViMode = m_viInputModeManager->getCurrentViMode();
  if (newViMode != ViMode::InsertMode && newViMode != ViMode::ReplaceMode) {
    if (cmd->isChange() && !m_viInputModeManager->lastChangeRecorder()->isReplaying()) {
      m_viInputModeManager->storeLastChangeCommand();
    }

    // when we transition to visual mode, remember the command in the keys
    // history (V, v, ctrl-v, ...) this will later result in buffer filled with
    // something like this "Vjj>" which we can use later with repeat "."
    const bool commandSwitchedToVisualMode =
        (originalViMode == ViMode::NormalMode) && m_viInputModeManager->isAnyVisualMode();
    if (!commandSwitchedToVisualMode) {
      m_viInputModeManager->clearCurrentChangeLog();
    }
  }

  if (newViMode == ViMode::NormalMode) {
    m_viInputModeManager->amendCursorPosition();
  }
}

////////////////////////////////////////////////////////////////////////////////
// COMMANDS AND OPERATORS
////////////////////////////////////////////////////////////////////////////////

/**
 * enter insert mode at the cursor position
 */

bool NormalViMode::commandEnterInsertMode() {
  m_stickyColumn = -1;
  m_viInputModeManager->getViInsertMode()->setCount(getCount());
  return startInsertMode();
}

/**
 * enter insert mode after the current character
 */
bool NormalViMode::commandEnterInsertModeAppend() {
  KateViI::Cursor c(m_interface->cursorPosition());
  c.setColumn(c.column() + 1);

  // if empty line, the cursor should start at column 0
  if (m_interface->lineLength(c.line()) == 0) {
    c.setColumn(0);
  }

  // cursor should never be in a column > number of columns
  if (c.column() > m_interface->lineLength(c.line())) {
    c.setColumn(m_interface->lineLength(c.line()));
  }

  updateCursor(c);

  m_stickyColumn = -1;
  m_viInputModeManager->getViInsertMode()->setCount(getCount());
  return startInsertMode();
}

/**
 * start insert mode after the last character of the line
 */
bool NormalViMode::commandEnterInsertModeAppendEOL() {
  KateViI::Cursor c(m_interface->cursorPosition());
  c.setColumn(m_interface->lineLength(c.line()));
  updateCursor(c);

  m_stickyColumn = -1;
  m_viInputModeManager->getViInsertMode()->setCount(getCount());
  return startInsertMode();
}

bool NormalViMode::commandEnterInsertModeBeforeFirstNonBlankInLine() {
  KateViI::Cursor cursor(m_interface->cursorPosition());
  int c = getFirstNonBlank();

  cursor.setColumn(c);
  updateCursor(cursor);

  m_stickyColumn = -1;
  m_viInputModeManager->getViInsertMode()->setCount(getCount());
  return startInsertMode();
}

/**
 * enter insert mode at the last insert position
 */
bool NormalViMode::commandEnterInsertModeLast() {
  KateViI::Cursor c = m_viInputModeManager->marks()->getInsertStopped();
  if (c.isValid()) {
    updateCursor(c);
  }

  m_stickyColumn = -1;
  return startInsertMode();
}

bool NormalViMode::commandEnterVisualLineMode() {
  if (m_viInputModeManager->getCurrentViMode() == VisualLineMode) {
    reset();
    return true;
  }

  return startVisualLineMode();
}

#if 0
bool NormalViMode::commandEnterVisualBlockMode()
{
    if (m_viInputModeManager->getCurrentViMode() == VisualBlockMode) {
        reset();
        return true;
    }

    return startVisualBlockMode();
}

bool NormalViMode::commandReselectVisual()
{
    // start last visual mode and set start = `< and cursor = `>
    KateViI::Cursor c1 = m_viInputModeManager->marks()->getSelectionStart();
    KateViI::Cursor c2 = m_viInputModeManager->marks()->getSelectionFinish();

    // we should either get two valid cursors or two invalid cursors
    Q_ASSERT(c1.isValid() == c2.isValid());

    if (c1.isValid() && c2.isValid()) {
        m_viInputModeManager->getViVisualMode()->setStart(c1);
        bool returnValue = false;

        switch (m_viInputModeManager->getViVisualMode()->getLastVisualMode()) {
        case ViMode::VisualMode:
            returnValue = commandEnterVisualMode();
            break;
        case ViMode::VisualLineMode:
            returnValue = commandEnterVisualLineMode();
            break;
        case ViMode::VisualBlockMode:
            returnValue = commandEnterVisualBlockMode();
            break;
        default:
            Q_ASSERT("invalid visual mode");
        }
        m_viInputModeManager->getViVisualMode()->goToPos(c2);
        return returnValue;
    } else {
        error(QStringLiteral("No previous visual selection"));
    }

    return false;
}
#endif

bool NormalViMode::commandEnterVisualMode() {
  if (m_viInputModeManager->getCurrentViMode() == ViMode::VisualMode) {
    reset();
    return true;
  }

  return startVisualMode();
}

bool NormalViMode::commandToOtherEnd() {
  if (m_viInputModeManager->isAnyVisualMode()) {
    m_viInputModeManager->getViVisualMode()->switchStartEnd();
    return true;
  }

  return false;
}

bool NormalViMode::commandEnterReplaceMode() {
  m_stickyColumn = -1;
  m_viInputModeManager->getViReplaceMode()->setCount(getCount());
  return startReplaceMode();
}

bool NormalViMode::commandDeleteLine() {
  KateViI::Cursor c(m_interface->cursorPosition());

  Range r;

  r.startLine = c.line();
  r.endLine = c.line() + getCount() - 1;

  int column = c.column();

  bool ret = deleteRange(r, LineWise);

  c = m_interface->cursorPosition();
  if (column > m_interface->lineLength(c.line()) - 1) {
    column = m_interface->lineLength(c.line()) - 1;
  }
  if (column < 0) {
    column = 0;
  }

  if (c.line() > m_interface->lines() - 1) {
    c.setLine(m_interface->lines() - 1);
  }

  c.setColumn(column);
  m_stickyColumn = -1;
  updateCursor(c);

  m_deleteCommand = true;
  return ret;
}

bool NormalViMode::commandDelete() {
  m_deleteCommand = true;
  return deleteRange(m_commandRange, getOperationMode());
}

bool NormalViMode::commandDeleteToEOL() {
  KateViI::Cursor c(m_interface->cursorPosition());
  OperationMode m = CharWise;

  switch (m_viInputModeManager->getCurrentViMode()) {
  case ViMode::NormalMode:
    m_commandRange.startLine = c.line();
    m_commandRange.startColumn = c.column();
    m_commandRange.endLine = c.line() + getCount() - 1;
    break;
  case ViMode::VisualMode:
  case ViMode::VisualLineMode:
    m = LineWise;
    break;
  case ViMode::VisualBlockMode:
    m_commandRange.normalize();
    m = Block;
    break;
  default:
    /* InsertMode and ReplaceMode will never call this method. */
    Q_ASSERT(false);
  }

  // KateVi::EOL.
  m_commandRange.endColumn = m_interface->lineLength(m_commandRange.endLine) - 1;

  bool r = deleteRange(m_commandRange, m);

  switch (m) {
  case CharWise:
    c.setColumn(m_interface->lineLength(c.line()) - 1);
    break;
  case LineWise:
    c.setLine(m_commandRange.startLine);
    c.setColumn(getFirstNonBlank(qMin(m_interface->lastLine(), m_commandRange.startLine)));
    break;
  case Block:
    c.setLine(m_commandRange.startLine);
    c.setColumn(m_commandRange.startColumn - 1);
    break;
  }

  // make sure cursor position is valid after deletion
  if (c.line() < 0) {
    c.setLine(0);
  }
  if (c.line() > m_interface->lastLine()) {
    c.setLine(m_interface->lastLine());
  }
  if (c.column() > m_interface->lineLength(c.line()) - 1) {
    c.setColumn(m_interface->lineLength(c.line()) - 1);
  }
  if (c.column() < 0) {
    c.setColumn(0);
  }

  updateCursor(c);

  m_deleteCommand = true;
  return r;
}

bool NormalViMode::commandMakeLowercase() {
  KateViI::Cursor c = m_interface->cursorPosition();

  OperationMode m = getOperationMode();
  QString text = getRange(m_commandRange, m);
  if (m == LineWise) {
    text.chop(1); // don't need '\n' at the end;
  }
  QString lowerCase = text.toLower();

  m_commandRange.normalize();
  KateViI::Cursor start(m_commandRange.startLine, m_commandRange.startColumn);
  KateViI::Cursor end(m_commandRange.endLine, m_commandRange.endColumn);
  KateViI::Range range(start, end);

  m_interface->replaceText(range, lowerCase, m == Block);

  if (m_viInputModeManager->getCurrentViMode() == ViMode::NormalMode) {
    updateCursor(start);
  } else {
    updateCursor(c);
  }

  return true;
}

bool NormalViMode::commandMakeLowercaseLine() {
  KateViI::Cursor c(m_interface->cursorPosition());

  if (m_interface->lineLength(c.line()) == 0) {
    // Nothing to do.
    return true;
  }

  m_commandRange.startLine = c.line();
  m_commandRange.endLine = c.line() + getCount() - 1;
  m_commandRange.startColumn = 0;
  m_commandRange.endColumn = m_interface->lineLength(c.line()) - 1;

  return commandMakeLowercase();
}

bool NormalViMode::commandMakeUppercase() {
  if (!m_commandRange.valid) {
    return false;
  }
  KateViI::Cursor c = m_interface->cursorPosition();
  OperationMode m = getOperationMode();
  QString text = getRange(m_commandRange, m);
  if (m == LineWise) {
    text.chop(1); // don't need '\n' at the end;
  }
  QString upperCase = text.toUpper();

  m_commandRange.normalize();
  KateViI::Cursor start(m_commandRange.startLine, m_commandRange.startColumn);
  KateViI::Cursor end(m_commandRange.endLine, m_commandRange.endColumn);
  KateViI::Range range(start, end);

  m_interface->replaceText(range, upperCase, m == Block);
  if (m_viInputModeManager->getCurrentViMode() == ViMode::NormalMode) {
    updateCursor(start);
  } else {
    updateCursor(c);
  }

  return true;
}

bool NormalViMode::commandMakeUppercaseLine() {
  KateViI::Cursor c(m_interface->cursorPosition());

  if (m_interface->lineLength(c.line()) == 0) {
    // Nothing to do.
    return true;
  }

  m_commandRange.startLine = c.line();
  m_commandRange.endLine = c.line() + getCount() - 1;
  m_commandRange.startColumn = 0;
  m_commandRange.endColumn = m_interface->lineLength(c.line()) - 1;

  return commandMakeUppercase();
}

bool NormalViMode::commandChangeCase() {
  switchView();
  QString text;
  KateViI::Range range;
  KateViI::Cursor c(m_interface->cursorPosition());

  // in visual mode, the range is from start position to end position...
  if (m_viInputModeManager->getCurrentViMode() == ViMode::VisualMode ||
      m_viInputModeManager->getCurrentViMode() == ViMode::VisualBlockMode) {
    KateViI::Cursor c2 = m_viInputModeManager->getViVisualMode()->getStart();

    if (c2 > c) {
      c2.setColumn(c2.column() + 1);
    } else {
      c.setColumn(c.column() + 1);
    }

    range.setRange(c, c2);
    // ... in visual line mode, the range is from column 0 on the first line to
    // the line length of the last line...
  } else if (m_viInputModeManager->getCurrentViMode() == ViMode::VisualLineMode) {
    KateViI::Cursor c2 = m_viInputModeManager->getViVisualMode()->getStart();

    if (c2 > c) {
      c2.setColumn(m_interface->lineLength(c2.line()));
      c.setColumn(0);
    } else {
      c.setColumn(m_interface->lineLength(c.line()));
      c2.setColumn(0);
    }

    range.setRange(c, c2);
    // ... and in normal mode the range is from the current position to the
    // current position + count
  } else {
    KateViI::Cursor c2 = c;
    c2.setColumn(c.column() + getCount());

    if (c2.column() > m_interface->lineLength(c.line())) {
      c2.setColumn(m_interface->lineLength(c.line()));
    }

    range.setRange(c, c2);
  }

  bool block = m_viInputModeManager->getCurrentViMode() == ViMode::VisualBlockMode;

  // get the text the command should operate on
  text = m_interface->getText(range, block);

  // for every character, switch its case
  for (int i = 0; i < text.length(); i++) {
    if (text.at(i).isUpper()) {
      text[i] = text.at(i).toLower();
    } else if (text.at(i).isLower()) {
      text[i] = text.at(i).toUpper();
    }
  }

  // replace the old text with the modified text
  m_interface->replaceText(range, text, block);

  // in normal mode, move the cursor to the right, in visual mode move the
  // cursor to the start of the selection
  if (m_viInputModeManager->getCurrentViMode() == ViMode::NormalMode) {
    updateCursor(range.end());
  } else {
    updateCursor(range.start());
  }

  return true;
}

bool NormalViMode::commandChangeCaseRange() {
  OperationMode m = getOperationMode();
  QString changedCase = getRange(m_commandRange, m);
  if (m == LineWise) {
    changedCase.chop(1); // don't need '\n' at the end;
  }
  KateViI::Range range = KateViI::Range(m_commandRange.startLine, m_commandRange.startColumn,
                                        m_commandRange.endLine, m_commandRange.endColumn);
  // get the text the command should operate on
  // for every character, switch its case
  for (int i = 0; i < changedCase.length(); i++) {
    if (changedCase.at(i).isUpper()) {
      changedCase[i] = changedCase.at(i).toLower();
    } else if (changedCase.at(i).isLower()) {
      changedCase[i] = changedCase.at(i).toUpper();
    }
  }
  m_interface->replaceText(range, changedCase, m == Block);
  return true;
}

bool NormalViMode::commandChangeCaseLine() {
  KateViI::Cursor c(m_interface->cursorPosition());

  if (m_interface->lineLength(c.line()) == 0) {
    // Nothing to do.
    return true;
  }

  m_commandRange.startLine = c.line();
  m_commandRange.endLine = c.line() + getCount() - 1;
  m_commandRange.startColumn = 0;
  m_commandRange.endColumn = m_interface->lineLength(c.line()) - 1; // -1 is for excluding '\0'

  if (!commandChangeCaseRange()) {
    return false;
  }

  KateViI::Cursor start(m_commandRange.startLine, m_commandRange.startColumn);
  if (getCount() > 1) {
    updateCursor(c);
  } else {
    updateCursor(start);
  }
  return true;
}

bool NormalViMode::commandOpenNewLineUnder() {
  m_interface->setUndoMergeAllEdits(true);

  KateViI::Cursor c(m_interface->cursorPosition());

  c.setColumn(m_interface->lineLength(c.line()));
  updateCursor(c);

  m_interface->newLine();

  m_stickyColumn = -1;
  startInsertMode();
  m_viInputModeManager->getViInsertMode()->setCount(getCount());
  m_viInputModeManager->getViInsertMode()->setCountedRepeatsBeginOnNewLine(true);

  return true;
}

bool NormalViMode::commandOpenNewLineOver() {
  m_interface->setUndoMergeAllEdits(true);

  KateViI::Cursor c(m_interface->cursorPosition());

  if (c.line() == 0) {
    m_interface->insertLine(0, QString());
    c.setColumn(0);
    c.setLine(0);
    updateCursor(c);
  } else {
    c.setLine(c.line() - 1);
    c.setColumn(getLine(c.line()).length());
    updateCursor(c);
    m_interface->newLine();
  }

  m_stickyColumn = -1;
  startInsertMode();
  m_viInputModeManager->getViInsertMode()->setCount(getCount());
  m_viInputModeManager->getViInsertMode()->setCountedRepeatsBeginOnNewLine(true);

  return true;
}

bool NormalViMode::commandJoinLines() {
  KateViI::Cursor c(m_interface->cursorPosition());

  unsigned int from = c.line();
  unsigned int to = c.line() + ((getCount() == 1) ? 1 : getCount() - 1);

  // if we were given a range of lines, this information overrides the previous
  if (m_commandRange.startLine != -1 && m_commandRange.endLine != -1) {
    m_commandRange.normalize();
    c.setLine(m_commandRange.startLine);
    from = m_commandRange.startLine;
    to = m_commandRange.endLine;
  }

  if (to >= (unsigned int)m_interface->lines()) {
    return false;
  }

  const int firstNonWhitespaceOnLastLine = m_interface->firstChar(to);
  QString leftTrimmedLastLine;
  if (firstNonWhitespaceOnLastLine != -1) {
    leftTrimmedLastLine = m_interface->line(to).mid(firstNonWhitespaceOnLastLine);
  }

  joinLines(from, to);

  // Position cursor just before first non-whitesspace character of what was the
  // last line joined.
  c.setColumn(m_interface->lineLength(from) - leftTrimmedLastLine.length() - 1);
  if (c.column() >= 0) {
    updateCursor(c);
  }

  m_deleteCommand = true;
  return true;
}

bool NormalViMode::commandChange() {
  KateViI::Cursor c(m_interface->cursorPosition());

  OperationMode m = getOperationMode();

  m_interface->setUndoMergeAllEdits(true);

  commandDelete();

  if (m == LineWise) {
    // if we deleted several lines, insert an empty line and put the cursor
    // there.
    m_interface->insertLine(m_commandRange.startLine, QString());
    c.setLine(m_commandRange.startLine);
    c.setColumn(0);
  } else if (m == Block) {
    KATEVI_NIY;
    // block substitute can be simulated by first deleting the text
    // (done above) and then starting block prepend.
    return commandPrependToBlock();
  } else {
    if (m_commandRange.startLine < m_commandRange.endLine) {
      c.setLine(m_commandRange.startLine);
    }
    c.setColumn(m_commandRange.startColumn);
  }

  updateCursor(c);
  setCount(0); // The count was for the motion, not the insertion.
  commandEnterInsertMode();

  // correct indentation level
  if (m == LineWise) {
    m_interface->align();
  }

  m_deleteCommand = true;
  return true;
}

bool NormalViMode::commandChangeToEOL() {
  commandDeleteToEOL();

  if (getOperationMode() == Block) {
    return commandPrependToBlock();
  }

  m_deleteCommand = true;
  return commandEnterInsertModeAppend();
}

bool NormalViMode::commandChangeLine() {
  m_deleteCommand = true;
  KateViI::Cursor c(m_interface->cursorPosition());
  c.setColumn(0);
  updateCursor(c);

  m_interface->setUndoMergeAllEdits(true);

  // if count >= 2 start by deleting the whole lines
  if (getCount() >= 2) {
    Range r(c.line(), 0, c.line() + getCount() - 2, 0, InclusiveMotion);
    deleteRange(r);
  }

  // ... then delete the _contents_ of the last line, but keep the line
  Range r(c.line(), c.column(), c.line(), m_interface->lineLength(c.line()) - 1, InclusiveMotion);
  deleteRange(r, CharWise, true);

  // ... then enter insert mode
  if (getOperationMode() == Block) {
    return commandPrependToBlock();
  }
  commandEnterInsertModeAppend();

  // correct indentation level
  m_interface->align();

  return true;
}

bool NormalViMode::commandSubstituteChar() {
  if (commandDeleteChar()) {
    // The count is only used for deletion of chars; the inserted text is not
    // repeated
    setCount(0);
    return commandEnterInsertMode();
  }

  m_deleteCommand = true;
  return false;
}

bool NormalViMode::commandSubstituteLine() {
  m_deleteCommand = true;
  return commandChangeLine();
}

bool NormalViMode::commandYank() {
  bool r = false;
  QString yankedText;

  OperationMode m = getOperationMode();
  yankedText = getRange(m_commandRange, m);

  highlightYank(m_commandRange, m);

  QChar chosen_register = getChosenRegister(ZeroRegister);
  fillRegister(chosen_register, yankedText, m);
  yankToClipBoard(chosen_register, yankedText);

  return r;
}

bool NormalViMode::commandYankLine() {
  KateViI::Cursor c(m_interface->cursorPosition());
  QString lines;
  int linenum = c.line();

  for (int i = 0; i < getCount(); i++) {
    lines.append(getLine(linenum + i) + QLatin1Char('\n'));
  }

  Range yankRange(linenum, 0, linenum + getCount() - 1, getLine(linenum + getCount() - 1).length(),
                  InclusiveMotion);
  highlightYank(yankRange);

  QChar chosen_register = getChosenRegister(ZeroRegister);
  fillRegister(chosen_register, lines, LineWise);
  yankToClipBoard(chosen_register, lines);

  return true;
}

bool NormalViMode::commandYankToEOL() {
  OperationMode m = CharWise;
  KateViI::Cursor c(m_interface->cursorPosition());

  MotionType motion = m_commandRange.motionType;
  m_commandRange.endLine = c.line() + getCount() - 1;
  m_commandRange.endColumn = m_interface->lineLength(m_commandRange.endLine) - 1;
  m_commandRange.motionType = InclusiveMotion;

  switch (m_viInputModeManager->getCurrentViMode()) {
  case ViMode::NormalMode:
    m_commandRange.startLine = c.line();
    m_commandRange.startColumn = c.column();
    break;
  case ViMode::VisualMode:
  case ViMode::VisualLineMode:
    m = LineWise;
    {
      VisualViMode *visual = static_cast<VisualViMode *>(this);
      visual->setStart(KateViI::Cursor(visual->getStart().line(), 0));
    }
    break;
  case ViMode::VisualBlockMode:
    m = Block;
    break;
  default:
    /* InsertMode and ReplaceMode will never call this method. */
    Q_ASSERT(false);
  }

  const QString &yankedText = getRange(m_commandRange, m);
  m_commandRange.motionType = motion;
  highlightYank(m_commandRange);

  QChar chosen_register = getChosenRegister(ZeroRegister);
  fillRegister(chosen_register, yankedText, m);
  yankToClipBoard(chosen_register, yankedText);

  return true;
}

// Insert the text in the given register after the cursor position.
// This is the non-g version of paste, so the cursor will usually
// end up on the last character of the pasted text, unless the text
// was multi-line or linewise in which case it will end up
// on the *first* character of the pasted text(!)
// If linewise, will paste after the current line.
bool NormalViMode::commandPaste() { return paste(AfterCurrentPosition, false, false); }

// As with commandPaste, except that the text is pasted *at* the cursor position
bool NormalViMode::commandPasteBefore() { return paste(AtCurrentPosition, false, false); }

// As with commandPaste, except that the cursor will generally be placed *after*
// the last pasted character (assuming the last pasted character is not at  the
// end of the line). If linewise, cursor will be at the beginning of the line
// *after* the last line of pasted text, unless that line is the last line of
// the document; then it will be placed at the beginning of the last line
// pasted.
bool NormalViMode::commandgPaste() { return paste(AfterCurrentPosition, true, false); }

// As with commandgPaste, except that it pastes *at* the current cursor position
// or, if linewise, at the current line.
bool NormalViMode::commandgPasteBefore() { return paste(AtCurrentPosition, true, false); }

bool NormalViMode::commandIndentedPaste() { return paste(AfterCurrentPosition, false, true); }

bool NormalViMode::commandIndentedPasteBefore() { return paste(AtCurrentPosition, false, true); }

bool NormalViMode::commandDeleteChar() {
  KateViI::Cursor c(m_interface->cursorPosition());
  Range r(c.line(), c.column(), c.line(), c.column() + getCount(), ExclusiveMotion);

  if (m_commandRange.startLine != -1 && m_commandRange.startColumn != -1) {
    r = m_commandRange;
  } else {
    if (r.endColumn > m_interface->lineLength(r.startLine)) {
      r.endColumn = m_interface->lineLength(r.startLine);
    }
  }

  // should delete entire lines if in visual line mode and selection in visual
  // block mode
  OperationMode m = CharWise;
  if (m_viInputModeManager->getCurrentViMode() == VisualLineMode) {
    m = LineWise;
  } else if (m_viInputModeManager->getCurrentViMode() == VisualBlockMode) {
    m = Block;
  }

  m_deleteCommand = true;
  return deleteRange(r, m);
}

bool NormalViMode::commandDeleteCharBackward() {
  KateViI::Cursor c(m_interface->cursorPosition());

  Range r(c.line(), c.column() - getCount(), c.line(), c.column(), ExclusiveMotion);

  if (m_commandRange.startLine != -1 && m_commandRange.startColumn != -1) {
    r = m_commandRange;
  } else {
    if (r.startColumn < 0) {
      r.startColumn = 0;
    }
  }

  // should delete entire lines if in visual line mode and selection in visual
  // block mode
  OperationMode m = CharWise;
  if (m_viInputModeManager->getCurrentViMode() == VisualLineMode) {
    m = LineWise;
  } else if (m_viInputModeManager->getCurrentViMode() == VisualBlockMode) {
    m = Block;
  }

  m_deleteCommand = true;
  return deleteRange(r, m);
}

bool NormalViMode::commandReplaceCharacter() {
  QString key = KeyParser::self()->decodeKeySequence(m_keys.right(1));

  // Filter out some special keys.
  const int keyCode = KeyParser::self()->encoded2qt(m_keys.right(1));
  switch (keyCode) {
  case Qt::Key_Left:
  case Qt::Key_Right:
  case Qt::Key_Up:
  case Qt::Key_Down:
  case Qt::Key_Home:
  case Qt::Key_End:
  case Qt::Key_PageUp:
  case Qt::Key_PageDown:
  case Qt::Key_Delete:
  case Qt::Key_Insert:
  case Qt::Key_Backspace:
  case Qt::Key_CapsLock:
    return true;
  case Qt::Key_Return:
  case Qt::Key_Enter:
    key = QStringLiteral("\n");
  }

  bool r;
  if (m_viInputModeManager->isAnyVisualMode()) {

    OperationMode m = getOperationMode();
    QString text = getRange(m_commandRange, m);

    if (m == LineWise) {
      text.chop(1); // don't need '\n' at the end;
    }

    text.replace(QRegularExpression(QLatin1String("[^\n]")), key);

    m_commandRange.normalize();
    KateViI::Cursor start(m_commandRange.startLine, m_commandRange.startColumn);
    KateViI::Cursor end(m_commandRange.endLine, m_commandRange.endColumn);
    KateViI::Range range(start, end);

    r = m_interface->replaceText(range, text, m == Block);

  } else {
    KateViI::Cursor c1(m_interface->cursorPosition());
    KateViI::Cursor c2(m_interface->cursorPosition());

    c2.setColumn(c2.column() + getCount());

    if (c2.column() > m_interface->lineLength(m_interface->cursorPosition().line())) {
      return false;
    }

    r = m_interface->replaceText(KateViI::Range(c1, c2), key.repeated(getCount()));
    updateCursor(c1);
  }
  return r;
}

bool NormalViMode::commandSwitchToCmdLine() {
  QString initialText;
  if (m_viInputModeManager->isAnyVisualMode()) {
    // if in visual mode, make command range == visual selection
    m_viInputModeManager->getViVisualMode()->saveRangeMarks();
    initialText = QStringLiteral("'<,'>");
  } else if (getCount() != 1) {
    // if a count is given, the range [current line] to [current line] +
    // count should be prepended to the command line
    initialText = QLatin1String(".,.+") + QString::number(getCount() - 1);
  }

  m_viInputModeManager->inputAdapter()->showViModeEmulatedCommandBar();
  m_viInputModeManager->inputAdapter()->viModeEmulatedCommandBar()->init(
      EmulatedCommandBar::Command, initialText);

  m_commandShouldKeepSelection = true;

  return true;
}

#if 0
bool NormalViMode::commandSearchBackward()
{
    /*
    m_viInputModeManager->inputAdapter()->showViModeEmulatedCommandBar();
    m_viInputModeManager->inputAdapter()->viModeEmulatedCommandBar()->init(EmulatedCommandBar::SearchBackward);
    */
    return true;
}

bool NormalViMode::commandSearchForward()
{
    /*
    m_viInputModeManager->inputAdapter()->showViModeEmulatedCommandBar();
    m_viInputModeManager->inputAdapter()->viModeEmulatedCommandBar()->init(EmulatedCommandBar::SearchForward);
    */
    return true;
}
#endif

bool NormalViMode::commandUndo() {
  // See BUG #328277
  m_viInputModeManager->clearCurrentChangeLog();

  if (m_interface->undoCount() > 0) {
    const bool mapped = m_viInputModeManager->keyMapper()->isExecutingMapping();

    if (mapped)
      m_interface->editEnd();
    m_interface->undo();
    if (mapped)
      m_interface->editBegin();
    if (m_viInputModeManager->isAnyVisualMode()) {
      m_viInputModeManager->getViVisualMode()->setStart(KateViI::Cursor(-1, -1));
      m_interface->removeSelection();
      startNormalMode();
    }
    return true;
  }
  return false;
}

bool NormalViMode::commandRedo() {
  if (m_interface->redoCount() > 0) {
    const bool mapped = m_viInputModeManager->keyMapper()->isExecutingMapping();

    if (mapped)
      m_interface->editEnd();
    m_interface->redo();
    if (mapped)
      m_interface->editBegin();
    if (m_viInputModeManager->isAnyVisualMode()) {
      m_viInputModeManager->getViVisualMode()->setStart(KateViI::Cursor(-1, -1));
      m_interface->removeSelection();
      startNormalMode();
    }
    return true;
  }
  return false;
}

bool NormalViMode::commandSetMark() {
  KateViI::Cursor c(m_interface->cursorPosition());

  QChar mark = m_keys.at(m_keys.size() - 1);
  m_viInputModeManager->marks()->setUserMark(mark, c);

  return true;
}

bool NormalViMode::commandIndentLine() {
  KateViI::Cursor c(m_interface->cursorPosition());

  m_interface->indent(KateViI::Range(c.line(), 0, c.line() + getCount(), 0), 1);

  return true;
}

bool NormalViMode::commandUnindentLine() {
  KateViI::Cursor c(m_interface->cursorPosition());

  m_interface->indent(KateViI::Range(c.line(), 0, c.line() + getCount(), 0), -1);

  return true;
}

bool NormalViMode::commandIndentLines() {
  const bool downwards = m_commandRange.startLine < m_commandRange.endLine;

  m_commandRange.normalize();

  int line1 = m_commandRange.startLine;
  int line2 = m_commandRange.endLine;
  int cnt = m_viInputModeManager->getCurrentViMode() == ViMode::NormalMode ? 1 : getCount();
  m_interface->indent(KateViI::Range(line1, 0, line2, m_interface->lineLength(line2)), cnt);

  if (downwards) {
    updateCursor(KateViI::Cursor(m_commandRange.startLine, m_commandRange.startColumn));
  } else {
    updateCursor(KateViI::Cursor(m_commandRange.endLine, m_commandRange.endColumn));
  }
  return true;
}

bool NormalViMode::commandUnindentLines() {
  const bool downwards = m_commandRange.startLine < m_commandRange.endLine;

  m_commandRange.normalize();

  int line1 = m_commandRange.startLine;
  int line2 = m_commandRange.endLine;
  int cnt = m_viInputModeManager->getCurrentViMode() == ViMode::NormalMode ? 1 : getCount();
  m_interface->indent(KateViI::Range(line1, 0, line2, m_interface->lineLength(line2)), -cnt);

  if (downwards) {
    updateCursor(KateViI::Cursor(m_commandRange.startLine, m_commandRange.startColumn));
  } else {
    updateCursor(KateViI::Cursor(m_commandRange.endLine, m_commandRange.endColumn));
  }
  return true;
}

bool NormalViMode::commandScrollPageDown() {
  if (getCount() < m_scrollCountLimit) {

    for (int i = 0; i < getCount(); i++) {
      m_interface->pageDown();
    }
  }
  return true;
}

bool NormalViMode::commandScrollPageUp() {
  if (getCount() < m_scrollCountLimit) {
    for (int i = 0; i < getCount(); i++) {
      m_interface->pageUp();
    }
  }
  return true;
}

bool NormalViMode::commandScrollHalfPageUp() {
  if (getCount() < m_scrollCountLimit) {
    for (int i = 0; i < getCount(); i++) {
      m_interface->pageUp(true);
    }
  }
  return true;
}

bool NormalViMode::commandScrollHalfPageDown() {
  if (getCount() < m_scrollCountLimit) {
    for (int i = 0; i < getCount(); i++) {
      m_interface->pageDown(true);
    }
  }
  return true;
}

bool NormalViMode::commandCenterView(bool onFirst) {
  KateViI::Cursor c(m_interface->cursorPosition());
  m_interface->scrollInPage(c, KateViI::PagePosition::Center);
  if (onFirst) {
    c.setColumn(getFirstNonBlank());
    updateCursor(c);
  }
  return true;
}

bool NormalViMode::commandCenterViewOnNonBlank() { return commandCenterView(true); }

bool NormalViMode::commandCenterViewOnCursor() { return commandCenterView(false); }

bool NormalViMode::commandTopView(bool onFirst) {
  KateViI::Cursor c(m_interface->cursorPosition());
  m_interface->scrollInPage(c, KateViI::PagePosition::Top);
  if (onFirst) {
    c.setColumn(getFirstNonBlank());
    updateCursor(c);
  }
  return true;
}

bool NormalViMode::commandTopViewOnNonBlank() { return commandTopView(true); }

bool NormalViMode::commandTopViewOnCursor() { return commandTopView(false); }

bool NormalViMode::commandBottomView(bool onFirst) {
  KateViI::Cursor c(m_interface->cursorPosition());
  m_interface->scrollInPage(c, KateViI::PagePosition::Bottom);
  if (onFirst) {
    c.setColumn(getFirstNonBlank());
    updateCursor(c);
  }
  return true;
}

bool NormalViMode::commandBottomViewOnNonBlank() { return commandBottomView(true); }

bool NormalViMode::commandBottomViewOnCursor() { return commandBottomView(false); }

bool NormalViMode::commandAbort() {
  m_pendingResetIsDueToExit = true;
  reset();
  return true;
}

#if 0
bool NormalViMode::commandPrintCharacterCode()
{
    QChar ch = getCharUnderCursor();

    if (ch == QChar::Null) {
        message(QStringLiteral("NUL"));
    } else {

        int code = ch.unicode();

        QString dec = QString::number(code);
        QString hex = QString::number(code, 16);
        QString oct = QString::number(code, 8);
        if (oct.length() < 3) {
            oct.prepend(QLatin1Char('0'));
        }
        if (code > 0x80 && code < 0x1000) {
            hex.prepend((code < 0x100 ? QLatin1String("00") : QLatin1String("0")));
        }
        message(i18n("'%1' %2,  Hex %3,  Octal %4", ch, dec, hex, oct));
    }

    return true;
}
#endif

bool NormalViMode::commandRepeatLastChange() {
  const int repeatCount = getCount();
  resetParser();
  if (repeatCount > 1) {
    m_oneTimeCountOverride = repeatCount;
  }
  m_interface->editStart();
  m_viInputModeManager->repeatLastChange();
  m_interface->editEnd();

  return true;
}

bool NormalViMode::commandAlignLine() {
  const int line = m_interface->cursorPosition().line();
  KateViI::Range alignRange(KateViI::Cursor(line, 0), KateViI::Cursor(line, 0));

  m_interface->align(alignRange);

  return true;
}

bool NormalViMode::commandAlignLines() {
  m_commandRange.normalize();

  KateViI::Cursor start(m_commandRange.startLine, 0);
  KateViI::Cursor end(m_commandRange.endLine, 0);

  m_interface->align(KateViI::Range(start, end));

  return true;
}

bool NormalViMode::commandAddToNumber() {
  addToNumberUnderCursor(getCount());

  return true;
}

bool NormalViMode::commandSubtractFromNumber() {
  addToNumberUnderCursor(-getCount());

  return true;
}

bool NormalViMode::commandPrependToBlock() {
  KateViI::Cursor c(m_interface->cursorPosition());

  // move cursor to top left corner of selection
  m_commandRange.normalize();
  c.setColumn(m_commandRange.startColumn);
  c.setLine(m_commandRange.startLine);
  updateCursor(c);

  m_stickyColumn = -1;
  m_viInputModeManager->getViInsertMode()->setBlockPrependMode(m_commandRange);
  return startInsertMode();
}

bool NormalViMode::commandAppendToBlock() {
  KateViI::Cursor c(m_interface->cursorPosition());

  m_commandRange.normalize();
  if (m_stickyColumn == (unsigned int)KateVi::EOL) { // append to EOL
    // move cursor to end of first line
    c.setLine(m_commandRange.startLine);
    c.setColumn(m_interface->lineLength(c.line()));
    updateCursor(c);
    m_viInputModeManager->getViInsertMode()->setBlockAppendMode(m_commandRange, AppendEOL);
  } else {
    m_viInputModeManager->getViInsertMode()->setBlockAppendMode(m_commandRange, Append);
    // move cursor to top right corner of selection
    c.setColumn(m_commandRange.endColumn + 1);
    c.setLine(m_commandRange.startLine);
    updateCursor(c);
  }

  m_stickyColumn = -1;

  return startInsertMode();
}

bool NormalViMode::commandGoToNextJump() {
  KateViI::Cursor c = getNextJump(m_interface->cursorPosition());
  updateCursor(c);

  return true;
}

bool NormalViMode::commandGoToPrevJump() {
  KateViI::Cursor c = getPrevJump(m_interface->cursorPosition());
  updateCursor(c);

  return true;
}

bool NormalViMode::commandSwitchToLeftView() {
  switchView(Left);
  return true;
}

bool NormalViMode::commandSwitchToDownView() {
  switchView(Down);
  return true;
}

bool NormalViMode::commandSwitchToUpView() {
  switchView(Up);
  return true;
}

bool NormalViMode::commandSwitchToRightView() {
  switchView(Right);
  return true;
}

bool NormalViMode::commandSwitchToNextView() {
  switchView(Next);
  return true;
}

bool NormalViMode::commandSplitHoriz() { return executeEditorCommand(QStringLiteral("split")); }

bool NormalViMode::commandSplitVert() { return executeEditorCommand(QStringLiteral("vsplit")); }

bool NormalViMode::commandCloseView() { return executeEditorCommand(QStringLiteral("close")); }

bool NormalViMode::commandSwitchToNextTab() {
  QString command = QStringLiteral("bn");

  if (m_iscounted) {
    command = command + QLatin1Char(' ') + QString::number(getCount());
  }

  return executeEditorCommand(command);
}

bool NormalViMode::commandSwitchToPrevTab() {
  QString command = QStringLiteral("bp");

  if (m_iscounted) {
    command = command + QLatin1Char(' ') + QString::number(getCount());
  }

  return executeEditorCommand(command);
}

#if 0
bool NormalViMode::commandFormatLine()
{
    KateViI::Cursor c(m_view->cursorPosition());

    reformatLines(c.line(), c.line() + getCount() - 1);

    return true;
}

bool NormalViMode::commandFormatLines()
{
    reformatLines(m_commandRange.startLine, m_commandRange.endLine);
    return true;
}

bool NormalViMode::commandCollapseToplevelNodes()
{
#if 0
    //FIXME FOLDING
    doc()->foldingTree()->collapseToplevelNodes();
#endif
    return true;
}
#endif

bool NormalViMode::commandStartRecordingMacro() {
  const QChar reg = m_keys[m_keys.size() - 1];
  m_viInputModeManager->macroRecorder()->start(reg);
  return true;
}

bool NormalViMode::commandReplayMacro() {
  // "@<registername>" will have been added to the log; it needs to be cleared
  // *before* we replay the macro keypresses, else it can cause an infinite loop
  // if the macro contains a "."
  m_viInputModeManager->clearCurrentChangeLog();
  const QChar reg = m_keys[m_keys.size() - 1];
  const unsigned int count = getCount();
  resetParser();
  m_interface->editBegin();
  for (unsigned int i = 0; i < count; i++) {
    m_viInputModeManager->macroRecorder()->replay(reg);
  }
  m_interface->editEnd();
  return true;
}

bool NormalViMode::commandCloseNocheck() { return executeEditorCommand(QStringLiteral("q!")); }

bool NormalViMode::commandCloseWrite() { return executeEditorCommand(QStringLiteral("wq")); }

#if 0
bool NormalViMode::commandCollapseLocal()
{
#if 0
    //FIXME FOLDING
    KTextEditor::Cursor c(m_view->cursorPosition());
    doc()->foldingTree()->collapseOne(c.line(), c.column());
#endif
    return true;
}

bool NormalViMode::commandExpandAll()
{
#if 0
    //FIXME FOLDING
    doc()->foldingTree()->expandAll();
#endif
    return true;
}

bool NormalViMode::commandExpandLocal()
{
#if 0
    //FIXME FOLDING
    KTextEditor::Cursor c(m_view->cursorPosition());
    doc()->foldingTree()->expandOne(c.line() + 1, c.column());
#endif
    return true;
}

bool NormalViMode::commandToggleRegionVisibility()
{
#if 0
    //FIXME FOLDING
    KTextEditor::Cursor c(m_view->cursorPosition());
    doc()->foldingTree()->toggleRegionVisibility(c.line());
#endif
    return true;
}
#endif

////////////////////////////////////////////////////////////////////////////////
// MOTIONS
////////////////////////////////////////////////////////////////////////////////

Range NormalViMode::motionUp() { return goLineUp(); }

Range NormalViMode::motionLeft() {
  qDebug() << "motionLeft";
  KateViI::Cursor cursor(m_interface->cursorPosition());
  m_stickyColumn = -1;
  Range r(cursor, ExclusiveMotion);
  r.endColumn -= getCount();

  if (r.endColumn < 0) {
    r.endColumn = 0;
  }

  return r;
}

Range NormalViMode::motionDown() { return goLineDown(); }

Range NormalViMode::motionRight() {
  KateViI::Cursor cursor(m_interface->cursorPosition());
  m_stickyColumn = -1;
  Range r(cursor, ExclusiveMotion);
  r.endColumn += getCount();

  // make sure end position isn't > line length
  if (r.endColumn > m_interface->lineLength(r.endLine)) {
    r.endColumn = m_interface->lineLength(r.endLine);
  }

  return r;
}

Range NormalViMode::motionPageDown() {
  KateViI::Cursor c(m_interface->cursorPosition());
  Range r(c, InclusiveMotion);
  r.endLine += linesDisplayed();

  if (r.endLine >= m_interface->lines()) {
    r.endLine = m_interface->lines() - 1;
  }
  return r;
}

Range NormalViMode::motionPageUp() {
  KateViI::Cursor c(m_interface->cursorPosition());
  Range r(c, InclusiveMotion);
  r.endLine -= linesDisplayed();

  if (r.endLine < 0) {
    r.endLine = 0;
  }
  return r;
}

Range NormalViMode::motionHalfPageDown() {
  if (commandScrollHalfPageDown()) {
    KateViI::Cursor c = m_interface->cursorPosition();
    m_commandRange.endLine = c.line();
    m_commandRange.endColumn = c.column();
    return m_commandRange;
  }
  return Range::invalid();
}

Range NormalViMode::motionHalfPageUp() {
  if (commandScrollHalfPageUp()) {
    KateViI::Cursor c = m_interface->cursorPosition();
    m_commandRange.endLine = c.line();
    m_commandRange.endColumn = c.column();
    return m_commandRange;
  }
  return Range::invalid();
}

Range NormalViMode::motionDownToFirstNonBlank() {
  Range r = goLineDown();
  r.endColumn = getFirstNonBlank(r.endLine);
  return r;
}

Range NormalViMode::motionUpToFirstNonBlank() {
  Range r = goLineUp();
  r.endColumn = getFirstNonBlank(r.endLine);
  return r;
}

Range NormalViMode::motionWordForward() {
  KateViI::Cursor c(m_interface->cursorPosition());
  Range r(c, ExclusiveMotion);

  m_stickyColumn = -1;

  // Special case: If we're already on the very last character in the document,
  // the motion should be inclusive so the last character gets included
  if (c.line() == m_interface->lines() - 1 && c.column() == m_interface->lineLength(c.line()) - 1) {
    r.motionType = InclusiveMotion;
  } else {
    for (int i = 0; i < getCount(); i++) {
      c = findNextWordStart(c.line(), c.column());
      // stop when at the last char in the document
      if (!c.isValid()) {
        c = m_interface->documentEnd();
        // if we still haven't "used up the count", make the motion inclusive,
        // so that the last char is included
        if (i < getCount()) {
          r.motionType = InclusiveMotion;
        }
        break;
      }
    }
  }

  r.endColumn = c.column();
  r.endLine = c.line();

  return r;
}

Range NormalViMode::motionWordBackward() {
  KateViI::Cursor c(m_interface->cursorPosition());
  Range r(c, ExclusiveMotion);

  m_stickyColumn = -1;

  for (int i = 0; i < getCount(); i++) {
    c = findPrevWordStart(c.line(), c.column());

    if (!c.isValid()) {
      c = KateViI::Cursor(0, 0);
      break;
    }
  }

  r.endColumn = c.column();
  r.endLine = c.line();

  return r;
}

Range NormalViMode::motionWORDForward() {
  KateViI::Cursor c(m_interface->cursorPosition());
  Range r(c, ExclusiveMotion);

  m_stickyColumn = -1;

  for (int i = 0; i < getCount(); i++) {
    c = findNextWORDStart(c.line(), c.column());

    // stop when at the last char in the document
    if (c.line() == m_interface->lines() - 1 &&
        c.column() == m_interface->lineLength(c.line()) - 1) {
      break;
    }
  }

  r.endColumn = c.column();
  r.endLine = c.line();

  return r;
}

Range NormalViMode::motionWORDBackward() {
  KateViI::Cursor c(m_interface->cursorPosition());
  Range r(c, ExclusiveMotion);

  m_stickyColumn = -1;

  for (int i = 0; i < getCount(); i++) {
    c = findPrevWORDStart(c.line(), c.column());

    if (!c.isValid()) {
      c = KateViI::Cursor(0, 0);
    }
  }

  r.endColumn = c.column();
  r.endLine = c.line();

  return r;
}

Range NormalViMode::motionToEndOfWord() {
  KateViI::Cursor c(m_interface->cursorPosition());
  Range r(c, InclusiveMotion);

  m_stickyColumn = -1;

  for (int i = 0; i < getCount(); i++) {
    c = findWordEnd(c.line(), c.column());
  }

  if (!c.isValid()) {
    c = m_interface->documentEnd();
  }

  r.endColumn = c.column();
  r.endLine = c.line();

  return r;
}

Range NormalViMode::motionToEndOfWORD() {
  KateViI::Cursor c(m_interface->cursorPosition());
  Range r(c, InclusiveMotion);

  m_stickyColumn = -1;

  for (int i = 0; i < getCount(); i++) {
    c = findWORDEnd(c.line(), c.column());
  }

  if (!c.isValid()) {
    c = m_interface->documentEnd();
  }

  r.endColumn = c.column();
  r.endLine = c.line();

  return r;
}

Range NormalViMode::motionToEndOfPrevWord() {
  KateViI::Cursor c(m_interface->cursorPosition());
  Range r(c, InclusiveMotion);

  m_stickyColumn = -1;

  for (int i = 0; i < getCount(); i++) {
    c = findPrevWordEnd(c.line(), c.column());

    if (c.isValid()) {
      r.endColumn = c.column();
      r.endLine = c.line();
    } else {
      r.endColumn = 0;
      r.endLine = 0;
      break;
    }
  }

  return r;
}

Range NormalViMode::motionToEndOfPrevWORD() {
  KateViI::Cursor c(m_interface->cursorPosition());
  Range r(c, InclusiveMotion);

  m_stickyColumn = -1;

  for (int i = 0; i < getCount(); i++) {
    c = findPrevWORDEnd(c.line(), c.column());

    if (c.isValid()) {
      r.endColumn = c.column();
      r.endLine = c.line();
    } else {
      r.endColumn = 0;
      r.endLine = 0;
      break;
    }
  }

  return r;
}

Range NormalViMode::motionToEOL() {
  KateViI::Cursor c(m_interface->cursorPosition());

  // set sticky column to a ridiculously high value so that the cursor will
  // stick to EOL, but only if it's a regular motion
  if (m_keys.size() == 1) {
    m_stickyColumn = KateVi::EOL;
  }

  unsigned int line = c.line() + (getCount() - 1);
  Range r(line, m_interface->lineLength(line) - 1, InclusiveMotion);

  return r;
}

Range NormalViMode::motionToColumn0() {
  m_stickyColumn = -1;
  KateViI::Cursor cursor(m_interface->cursorPosition());
  Range r(cursor.line(), 0, ExclusiveMotion);

  return r;
}

Range NormalViMode::motionToFirstCharacterOfLine() {
  m_stickyColumn = -1;

  KateViI::Cursor cursor(m_interface->cursorPosition());
  int c = getFirstNonBlank();

  Range r(cursor.line(), c, ExclusiveMotion);

  return r;
}

Range NormalViMode::motionFindChar() {
  m_lastTFcommand = m_keys;
  KateViI::Cursor cursor(m_interface->cursorPosition());
  QString line = getLine();

  m_stickyColumn = -1;

  int matchColumn = cursor.column();

  for (int i = 0; i < getCount(); i++) {
    matchColumn = line.indexOf(m_keys.right(1), matchColumn + 1);
    if (matchColumn == -1) {
      break;
    }
  }

  Range r;

  if (matchColumn != -1) {
    r.endColumn = matchColumn;
    r.endLine = cursor.line();
  } else {
    return Range::invalid();
  }

  return r;
}

Range NormalViMode::motionFindCharBackward() {
  m_lastTFcommand = m_keys;
  KateViI::Cursor cursor(m_interface->cursorPosition());
  QString line = getLine();

  m_stickyColumn = -1;

  int matchColumn = -1;

  int hits = 0;
  int i = cursor.column() - 1;

  while (hits != getCount() && i >= 0) {
    if (line.at(i) == m_keys.at(m_keys.size() - 1)) {
      hits++;
    }

    if (hits == getCount()) {
      matchColumn = i;
    }

    i--;
  }

  Range r(cursor, ExclusiveMotion);

  if (matchColumn != -1) {
    r.endColumn = matchColumn;
    r.endLine = cursor.line();
  } else {
    return Range::invalid();
  }

  return r;
}

Range NormalViMode::motionToChar() {
  m_lastTFcommand = m_keys;
  KateViI::Cursor cursor(m_interface->cursorPosition());
  QString line = getLine();

  m_stickyColumn = -1;
  Range r;
  r.endColumn = -1;
  r.endLine = -1;

  int matchColumn = cursor.column() + (m_isRepeatedTFcommand ? 2 : 1);
  m_isRepeatedTFcommand = false;

  for (int i = 0; i < getCount(); i++) {
    const int lastColumn = matchColumn;
    matchColumn = line.indexOf(m_keys.right(1), matchColumn + ((i > 0) ? 1 : 0));
    if (matchColumn == -1) {
      return Range::invalid();
    }
  }

  r.endColumn = matchColumn - 1;
  r.endLine = cursor.line();
  return r;
}

Range NormalViMode::motionToCharBackward() {
  m_lastTFcommand = m_keys;
  KateViI::Cursor cursor(m_interface->cursorPosition());
  QString line = getLine();

  const int originalColumn = cursor.column();
  m_stickyColumn = -1;

  int matchColumn = originalColumn - 1;

  int hits = 0;
  int i = cursor.column() - (m_isRepeatedTFcommand ? 2 : 1);
  m_isRepeatedTFcommand = false;

  Range r(cursor, ExclusiveMotion);
  while (hits != getCount() && i >= 0) {
    if (line.at(i) == m_keys.at(m_keys.size() - 1)) {
      hits++;
    }

    if (hits == getCount()) {
      matchColumn = i;
    }

    i--;
  }

  if (hits == getCount()) {
    r.endColumn = matchColumn + 1;
    r.endLine = cursor.line();
  } else {
    r.valid = false;
  }

  return r;
}

Range NormalViMode::motionRepeatlastTF() {
  if (!m_lastTFcommand.isEmpty()) {
    m_isRepeatedTFcommand = true;
    m_keys = m_lastTFcommand;
    if (m_keys.at(0) == QLatin1Char('f')) {
      return motionFindChar();
    } else if (m_keys.at(0) == QLatin1Char('F')) {
      return motionFindCharBackward();
    } else if (m_keys.at(0) == QLatin1Char('t')) {
      return motionToChar();
    } else if (m_keys.at(0) == QLatin1Char('T')) {
      return motionToCharBackward();
    }
  }

  // there was no previous t/f command
  return Range::invalid();
}

Range NormalViMode::motionRepeatlastTFBackward() {
  if (!m_lastTFcommand.isEmpty()) {
    m_isRepeatedTFcommand = true;
    m_keys = m_lastTFcommand;
    if (m_keys.at(0) == QLatin1Char('f')) {
      return motionFindCharBackward();
    } else if (m_keys.at(0) == QLatin1Char('F')) {
      return motionFindChar();
    } else if (m_keys.at(0) == QLatin1Char('t')) {
      return motionToCharBackward();
    } else if (m_keys.at(0) == QLatin1Char('T')) {
      return motionToChar();
    }
  }

  // there was no previous t/f command
  return Range::invalid();
}

Range NormalViMode::motionToLineFirst() {
  Range r(getCount() - 1, 0, InclusiveMotion);
  m_stickyColumn = -1;

  if (r.endLine > m_interface->lines() - 1) {
    r.endLine = m_interface->lines() - 1;
  }
  r.jump = true;
  return r;
}

Range NormalViMode::motionToLineLast() {
  Range r(m_interface->lines() - 1, 0, InclusiveMotion);
  m_stickyColumn = -1;

  // don't use getCount() here, no count and a count of 1 is different here...
  if (m_count != 0) {
    r.endLine = m_count - 1;
  }

  if (r.endLine > m_interface->lines() - 1) {
    r.endLine = m_interface->lines() - 1;
  }
  r.jump = true;
  return r;
}

Range NormalViMode::motionToScreenColumn() {
  m_stickyColumn = -1;

  KateViI::Cursor c(m_interface->cursorPosition());

  int column = getCount() - 1;

  if (m_interface->lineLength(c.line()) - 1 < (int)getCount() - 1) {
    column = m_interface->lineLength(c.line()) - 1;
  }

  return Range(c.line(), column, ExclusiveMotion);
}

#if 0
Range NormalViMode::motionToMark()
{
    Range r;

    m_stickyColumn = -1;

    QChar reg = m_keys.at(m_keys.size() - 1);

    KateViI::Cursor c = m_viInputModeManager->marks()->getMarkPosition(reg);
    if (c.isValid()) {
        r.endLine = c.line();
        r.endColumn = c.column();
    } else {
        error(i18n("Mark not set: %1", m_keys.right(1)));
        r.valid = false;
    }

    r.jump = true;

    return r;
}

Range NormalViMode::motionToMarkLine()
{
    Range r = motionToMark();
    r.endColumn = getFirstNonBlank(r.endLine);
    r.jump = true;
    m_stickyColumn = -1;
    return r;
}
#endif

static QString matchingBracketItem(const QString &p_item) {
  Q_ASSERT(p_item.size() == 1);
  QString matchingItem;
  switch (p_item[0].toLatin1()) {
  case '(':
    matchingItem = QStringLiteral(")");
    break;

  case '[':
    matchingItem = QStringLiteral("]");
    break;

  case '{':
    matchingItem = QStringLiteral("}");
    break;

  case ')':
    matchingItem = QStringLiteral("-(");
    break;

  case ']':
    matchingItem = QStringLiteral("-[");
    break;

  case '}':
    matchingItem = QStringLiteral("-{");
    break;

  default:
    Q_ASSERT(false);
    break;
  }

  return matchingItem;
}

Range NormalViMode::motionToMatchingItem() {
  Range r;
  int lines = m_interface->lines();

  // If counted, then it's not a motion to matching item anymore,
  // but a motion to the N'th percentage of the document
  if (isCounted()) {
    int count = getCount();
    if (count > 100) {
      return r;
    }
    r.endLine = qRound(lines * count / 100.0) - 1;
    r.endColumn = 0;
    return r;
  }

  KateViI::Cursor c(m_interface->cursorPosition());

  QString l = getLine();

  // Search for the item from after current cursor.
  int n1 = l.indexOf(m_matchItemRegex, c.column());

  m_stickyColumn = -1;

  if (n1 < 0) {
    return Range::invalid();
  }

  QString item;
  QString matchingItem;
  if (l.indexOf(QRegularExpression(QLatin1String("[(){}\\[\\]]")), n1) == n1) {
    // Brackets.
    item = l.mid(n1, 1);
    matchingItem = matchingBracketItem(item);
  } else {
    // text item we want to find a matching item for
    int n2 = l.indexOf(QRegularExpression(QLatin1String("\\b|\\s|$")), n1);
    item = l.mid(n1, n2 - n1);
    matchingItem = m_matchingItems[item];
  }

  Q_ASSERT(!item.isEmpty());
  int toFind = 1;
  int line = c.line();
  // Column to start searching.
  int column = n1;
  bool reversed = false;
  if (matchingItem.startsWith(QLatin1Char('-'))) {
    matchingItem.remove(0, 1); // remove the '-'
    reversed = true;
  }

  // make sure we don't hit the text item we started the search from
  const int delta = reversed ? -1 : 1;
  column += delta;
  if (column == -1) {
    Q_ASSERT(reversed);
    // We should skip this line by setting the from index of lastIndexOf()
    // to (-item.length() - 1).
    column -= item.length();
  }
  int itemIdx = -1;
  int matchItemIdx = -1;
  while (toFind > 0) {
    // At each iteration, @column is the column to start searching.
    if (reversed) {
      // Now item is like `}`.
      itemIdx = l.lastIndexOf(item, column);
      matchItemIdx = l.lastIndexOf(matchingItem, column);

      if (itemIdx != -1 && (matchItemIdx == -1 || matchItemIdx < itemIdx)) {
        ++toFind;
      }
    } else {
      itemIdx = l.indexOf(item, column);
      matchItemIdx = l.indexOf(matchingItem, column);

      if (itemIdx != -1 && (matchItemIdx == -1 || itemIdx < matchItemIdx)) {
        // Found a recursive match pair.
        ++toFind;
      }
    }

    if (matchItemIdx != -1 || itemIdx != -1) {
      if (reversed) {
        column = qMax(itemIdx, matchItemIdx);
      } else {
        // unsigned int to handle -1 case.
        column = qMin((unsigned int)itemIdx, (unsigned int)matchItemIdx);
      }
    }

    if (matchItemIdx != -1) { // match on current line
      if (matchItemIdx == column) {
        // We found a matching item which will close an open item.
        --toFind;
        c.setLine(line);
        c.setColumn(column);
      }
      column += delta;
    } else { // no match, advance one line if possible
      line += delta;
      // In reversed case, -1 means search from the end.
      column = reversed ? -1 : 0;

      if ((!reversed && line >= lines) || (reversed && line < 0)) {
        r.valid = false;
        break;
      } else {
        l = getLine(line);
      }
    }
  }

  r.endLine = c.line();
  r.endColumn = c.column();
  r.jump = true;

  return r;
}

Range NormalViMode::motionToNextBraceBlockStart() {
  Range r;

  m_stickyColumn = -1;

  int line = findLineStartingWitchChar(QLatin1Char('{'), getCount());

  if (line == -1) {
    return Range::invalid();
  }

  r.endLine = line;
  r.endColumn = 0;
  r.jump = true;

  if (motionWillBeUsedWithCommand()) {
    // Delete from cursor (inclusive) to the '{' (exclusive).
    // If we are on the first column, then delete the entire current line.
    r.motionType = ExclusiveMotion;
    if (m_interface->cursorPosition().column() != 0) {
      r.endLine--;
      r.endColumn = m_interface->lineLength(r.endLine);
    }
  }

  return r;
}

Range NormalViMode::motionToPreviousBraceBlockStart() {
  Range r;

  m_stickyColumn = -1;

  int line = findLineStartingWitchChar(QLatin1Char('{'), getCount(), false);

  if (line == -1) {
    return Range::invalid();
  }

  r.endLine = line;
  r.endColumn = 0;
  r.jump = true;

  if (motionWillBeUsedWithCommand()) {
    // With a command, do not include the { or the cursor position.
    r.motionType = ExclusiveMotion;
  }

  return r;
}

Range NormalViMode::motionToNextBraceBlockEnd() {
  Range r;

  m_stickyColumn = -1;

  int line = findLineStartingWitchChar(QLatin1Char('}'), getCount());

  if (line == -1) {
    return Range::invalid();
  }

  r.endLine = line;
  r.endColumn = 0;
  r.jump = true;

  if (motionWillBeUsedWithCommand()) {
    // Delete from cursor (inclusive) to the '}' (exclusive).
    // If we are on the first column, then delete the entire current line.
    r.motionType = ExclusiveMotion;
    if (m_interface->cursorPosition().column() != 0) {
      r.endLine--;
      r.endColumn = m_interface->lineLength(r.endLine);
    }
  }

  return r;
}

Range NormalViMode::motionToPreviousBraceBlockEnd() {
  Range r;

  m_stickyColumn = -1;

  int line = findLineStartingWitchChar(QLatin1Char('}'), getCount(), false);

  if (line == -1) {
    return Range::invalid();
  }

  r.endLine = line;
  r.endColumn = 0;
  r.jump = true;

  if (motionWillBeUsedWithCommand()) {
    r.motionType = ExclusiveMotion;
  }

  return r;
}

#if 0
Range NormalViMode::motionToNextOccurrence()
{
    const QString word = getWordUnderCursor();
    const Range match = m_viInputModeManager->searcher()->findWordForMotion(word, false, getWordRangeUnderCursor().start(), getCount());
    return Range(match.startLine, match.startColumn, ExclusiveMotion);
}

Range NormalViMode::motionToPrevOccurrence()
{
    const QString word = getWordUnderCursor();
    const Range match = m_viInputModeManager->searcher()->findWordForMotion(word, true, getWordRangeUnderCursor().start(), getCount());
    return Range(match.startLine, match.startColumn, ExclusiveMotion);
}
#endif

Range NormalViMode::motionToFirstLineOfWindow() {
  int lines_to_go = 0;
  const int ld = linesDisplayed();
  const int endLine = m_interface->endLine();
  if ((unsigned int)ld <= (unsigned int)endLine) {
    lines_to_go = endLine - ld - m_interface->cursorPosition().line() + 1;
  } else {
    lines_to_go = -m_interface->cursorPosition().line();
  }

  Range r = goLineUpDown(lines_to_go);
  r.endColumn = getFirstNonBlank(r.endLine);
  return r;
}

Range NormalViMode::motionToMiddleLineOfWindow() {
  int lines_to_go;
  const int ld = linesDisplayed();
  const int endLine = m_interface->endLine();
  if ((unsigned int)ld <= (unsigned int)endLine) {
    lines_to_go = endLine - ld / 2 - m_interface->cursorPosition().line();
  } else {
    lines_to_go = endLine / 2 - m_interface->cursorPosition().line();
  }

  Range r = goLineUpDown(lines_to_go);
  r.endColumn = getFirstNonBlank(r.endLine);
  return r;
}

Range NormalViMode::motionToLastLineOfWindow() {
  int lines_to_go;
  const int ld = linesDisplayed();
  const int endLine = m_interface->endLine();
  if ((unsigned int)ld <= (unsigned int)endLine) {
    lines_to_go = endLine - m_interface->cursorPosition().line();
  } else {
    lines_to_go = endLine - m_interface->cursorPosition().line();
  }

  Range r = goLineUpDown(lines_to_go);
  r.endColumn = getFirstNonBlank(r.endLine);
  return r;
}

Range NormalViMode::motionToNextVisualLine() { return goVisualLineUpDown(getCount()); }

Range NormalViMode::motionToPrevVisualLine() { return goVisualLineUpDown(-getCount()); }

Range NormalViMode::motionToPreviousSentence() {
  // FIXME: consider getCount().
  KateViI::Cursor c = findSentenceStart();
  int linenum = c.line(), column;
  const bool skipSpaces = m_interface->line(linenum).isEmpty();

  if (skipSpaces) {
    linenum--;
    if (linenum >= 0) {
      column = m_interface->line(linenum).size() - 1;
    }
  } else {
    column = c.column();
  }

  for (int i = linenum; i >= 0; i--) {
    const QString &line = m_interface->line(i);

    if (line.isEmpty() && !skipSpaces) {
      return Range(i, 0, InclusiveMotion);
    }

    if (column < 0 && !line.isEmpty()) {
      column = line.size() - 1;
    }

    for (int j = column; j >= 0; j--) {
      if (skipSpaces || QStringLiteral(".?!").indexOf(line.at(j)) != -1) {
        c.setLine(i);
        c.setColumn(j);
        updateCursor(c);
        c = findSentenceStart();
        return Range(c, InclusiveMotion);
      }
    }
    column = line.size() - 1;
  }
  return Range(0, 0, InclusiveMotion);
}

Range NormalViMode::motionToNextSentence() {
  KateViI::Cursor c = findSentenceEnd();
  int linenum = c.line(), column = c.column() + 1;
  const bool skipSpaces = m_interface->line(linenum).isEmpty();

  for (int i = linenum; i < m_interface->lines(); i++) {
    const QString &line = m_interface->line(i);

    if (line.isEmpty() && !skipSpaces) {
      return Range(i, 0, InclusiveMotion);
    }

    for (int j = column; j < line.size(); j++) {
      if (!line.at(j).isSpace()) {
        return Range(i, j, InclusiveMotion);
      }
    }
    column = 0;
  }

  c = m_interface->documentEnd();
  return Range(c, InclusiveMotion);
}

Range NormalViMode::motionToBeforeParagraph() {
  KateViI::Cursor c(m_interface->cursorPosition());

  int line = c.line();

  m_stickyColumn = -1;

  for (int i = 0; i < getCount(); i++) {
    // advance at least one line, but if there are consecutive blank lines
    // skip them all
    do {
      line--;
    } while (line >= 0 && getLine(line + 1).length() == 0);
    while (line > 0 && getLine(line).length() != 0) {
      line--;
    }
  }

  if (line < 0) {
    line = 0;
  }

  Range r(line, 0, InclusiveMotion);

  return r;
}

Range NormalViMode::motionToAfterParagraph() {
  KateViI::Cursor c(m_interface->cursorPosition());

  int line = c.line();

  m_stickyColumn = -1;

  const int numOfLines = m_interface->lines();
  for (int i = 0; i < getCount(); i++) {
    // advance at least one line, but if there are consecutive blank lines
    // skip them all
    do {
      line++;
    } while (line <= numOfLines - 1 && getLine(line - 1).length() == 0);
    while (line < numOfLines - 1 && getLine(line).length() != 0) {
      line++;
    }
  }

  if (line >= numOfLines) {
    line = numOfLines - 1;
  }

  // if we ended up on the last line, the cursor should be placed on the last
  // column
  int column = (line == numOfLines - 1) ? qMax(getLine(line).length() - 1, 0) : 0;

  return Range(line, column, InclusiveMotion);
}

#if 0
Range NormalViMode::motionToIncrementalSearchMatch()
{
    return Range(m_positionWhenIncrementalSearchBegan.line(),
                       m_positionWhenIncrementalSearchBegan.column(),
                       m_view->cursorPosition().line(),
                       m_view->cursorPosition().column(), ExclusiveMotion);
}
#endif

////////////////////////////////////////////////////////////////////////////////
// TEXT OBJECTS
////////////////////////////////////////////////////////////////////////////////

Range NormalViMode::textObjectAWord() {
  KateViI::Cursor c(m_interface->cursorPosition());

  KateViI::Cursor c1 = c;

  bool startedOnSpace = false;
  if (m_interface->characterAt(c).isSpace()) {
    startedOnSpace = true;
  } else {
    c1 = findPrevWordStart(c.line(), c.column() + 1, true);
    if (!c1.isValid()) {
      c1 = KateViI::Cursor(0, 0);
    }
  }
  KateViI::Cursor c2 = KateViI::Cursor(c.line(), c.column() - 1);
  for (int i = 1; i <= getCount(); i++) {
    c2 = findWordEnd(c2.line(), c2.column());
  }
  if (!c1.isValid() || !c2.isValid()) {
    return Range::invalid();
  }
  // Adhere to some of Vim's bizarre rules of whether to swallow ensuing spaces
  // or not. Don't ask ;)
  const KateViI::Cursor nextWordStart = findNextWordStart(c2.line(), c2.column());
  if (nextWordStart.isValid() && nextWordStart.line() == c2.line()) {
    if (!startedOnSpace) {
      c2 = KateViI::Cursor(nextWordStart.line(), nextWordStart.column() - 1);
    }
  } else {
    c2 = KateViI::Cursor(c2.line(), m_interface->lineLength(c2.line()) - 1);
  }
  bool swallowCarriageReturnAtEndOfLine = false;
  if (c2.line() != c.line() && c2.column() == m_interface->lineLength(c2.line()) - 1) {
    // Greedily descend to the next line, so as to swallow the carriage return
    // on this line.
    c2 = KateViI::Cursor(c2.line() + 1, 0);
    swallowCarriageReturnAtEndOfLine = true;
  }
  const bool swallowPrecedingSpaces = (c2.column() == m_interface->lineLength(c2.line()) - 1 &&
                                       !m_interface->characterAt(c2).isSpace()) ||
                                      startedOnSpace || swallowCarriageReturnAtEndOfLine;
  if (swallowPrecedingSpaces) {
    if (c1.column() != 0) {
      const KateViI::Cursor previousNonSpace = findPrevWordEnd(c.line(), c.column());
      if (previousNonSpace.isValid() && previousNonSpace.line() == c1.line()) {
        c1 = KateViI::Cursor(previousNonSpace.line(), previousNonSpace.column() + 1);
      } else if (startedOnSpace || swallowCarriageReturnAtEndOfLine) {
        c1 = KateViI::Cursor(c1.line(), 0);
      }
    }
  }

  return Range(c1, c2, !swallowCarriageReturnAtEndOfLine ? InclusiveMotion : ExclusiveMotion);
}

Range NormalViMode::textObjectInnerWord() {
  KateViI::Cursor c(m_interface->cursorPosition());

  KateViI::Cursor c1 = findPrevWordStart(c.line(), c.column() + 1, true);
  if (!c1.isValid()) {
    c1 = KateViI::Cursor(0, 0);
  }
  // need to start search in column-1 because it might be a one-character word
  KateViI::Cursor c2(c.line(), c.column() - 1);

  for (int i = 0; i < getCount(); i++) {
    c2 = findWordEnd(c2.line(), c2.column(), true);
  }

  if (!c2.isValid()) {
    c2 = m_interface->documentEnd();
  }

  // sanity check
  if (c1.line() != c2.line() || c1.column() > c2.column()) {
    return Range::invalid();
  }
  return Range(c1, c2, InclusiveMotion);
}

Range NormalViMode::textObjectAWORD() {
  KateViI::Cursor c(m_interface->cursorPosition());

  KateViI::Cursor c1 = c;

  bool startedOnSpace = false;
  if (m_interface->characterAt(c).isSpace()) {
    startedOnSpace = true;
  } else {
    c1 = findPrevWORDStart(c.line(), c.column() + 1, true);
    if (!c1.isValid()) {
      c1 = KateViI::Cursor(0, 0);
    }
  }
  KateViI::Cursor c2 = KateViI::Cursor(c.line(), c.column() - 1);
  for (int i = 1; i <= getCount(); i++) {
    c2 = findWORDEnd(c2.line(), c2.column());
  }
  if (!c1.isValid() || !c2.isValid()) {
    return Range::invalid();
  }
  // Adhere to some of Vim's bizarre rules of whether to swallow ensuing spaces
  // or not. Don't ask ;)
  const KateViI::Cursor nextWordStart = findNextWordStart(c2.line(), c2.column());
  if (nextWordStart.isValid() && nextWordStart.line() == c2.line()) {
    if (!startedOnSpace) {
      c2 = KateViI::Cursor(nextWordStart.line(), nextWordStart.column() - 1);
    }
  } else {
    c2 = KateViI::Cursor(c2.line(), m_interface->lineLength(c2.line()) - 1);
  }
  bool swallowCarriageReturnAtEndOfLine = false;
  if (c2.line() != c.line() && c2.column() == m_interface->lineLength(c2.line()) - 1) {
    // Greedily descend to the next line, so as to swallow the carriage return
    // on this line.
    c2 = KateViI::Cursor(c2.line() + 1, 0);
    swallowCarriageReturnAtEndOfLine = true;
  }
  const bool swallowPrecedingSpaces = (c2.column() == m_interface->lineLength(c2.line()) - 1 &&
                                       !m_interface->characterAt(c2).isSpace()) ||
                                      startedOnSpace || swallowCarriageReturnAtEndOfLine;
  if (swallowPrecedingSpaces) {
    if (c1.column() != 0) {
      const KateViI::Cursor previousNonSpace = findPrevWORDEnd(c.line(), c.column());
      if (previousNonSpace.isValid() && previousNonSpace.line() == c1.line()) {
        c1 = KateViI::Cursor(previousNonSpace.line(), previousNonSpace.column() + 1);
      } else if (startedOnSpace || swallowCarriageReturnAtEndOfLine) {
        c1 = KateViI::Cursor(c1.line(), 0);
      }
    }
  }

  return Range(c1, c2, !swallowCarriageReturnAtEndOfLine ? InclusiveMotion : ExclusiveMotion);
}

Range NormalViMode::textObjectInnerWORD() {
  KateViI::Cursor c(m_interface->cursorPosition());

  KateViI::Cursor c1 = findPrevWORDStart(c.line(), c.column() + 1, true);
  if (!c1.isValid()) {
    c1 = KateViI::Cursor(0, 0);
  }
  KateViI::Cursor c2(c);

  for (int i = 0; i < getCount(); i++) {
    c2 = findWORDEnd(c2.line(), c2.column(), true);
  }

  if (!c2.isValid()) {
    c2 = m_interface->documentEnd();
  }

  // sanity check
  if (c1.line() != c2.line() || c1.column() > c2.column()) {
    return Range::invalid();
  }
  return Range(c1, c2, InclusiveMotion);
}

KateViI::Cursor NormalViMode::findSentenceStart() {
  KateViI::Cursor c(m_interface->cursorPosition());
  int linenum = c.line(), column = c.column();
  int prev = column;

  for (int i = linenum; i >= 0; i--) {
    const QString line = m_interface->line(i);
    // When i == linenum, we still need to check column since it may locate
    // at the end of the line.
    if (i != linenum || column >= line.size()) {
      column = line.size() - 1;
    }

    // An empty line is the end of a paragraph.
    if (line.isEmpty()) {
      return KateViI::Cursor((i != linenum) ? i + 1 : i, prev);
    }

    prev = column;
    for (int j = column; j >= 0; j--) {
      if (line.at(j).isSpace()) {
        int lastSpace = j--;
        for (; j >= 0 && QStringLiteral("\"')]").indexOf(line.at(j)) != -1; j--)
          ;

        if (j >= 0 && QStringLiteral(".!?").indexOf(line.at(j)) != -1) {
          return KateViI::Cursor(i, prev);
        }
        j = lastSpace;
      } else
        prev = j;
    }
  }

  return KateViI::Cursor(0, 0);
}

KateViI::Cursor NormalViMode::findSentenceEnd() {
  KateViI::Cursor c(m_interface->cursorPosition());
  int linenum = c.line(), column = c.column();
  int j = 0, prev = 0;

  for (int i = linenum; i < m_interface->lines(); i++) {
    const QString &line = m_interface->line(i);

    // An empty line is the end of a paragraph.
    if (line.isEmpty()) {
      return KateViI::Cursor(linenum, j);
    }

    // Iterating over the line to reach any '.', '!', '?'
    for (j = column; j < line.size(); j++) {
      if (QStringLiteral(".!?").indexOf(line.at(j)) != -1) {
        prev = j++;
        // Skip possible closing characters.
        for (; j < line.size() && QStringLiteral("\"')]").indexOf(line.at(j)) != -1; j++)
          ;

        if (j >= line.size()) {
          return KateViI::Cursor(i, j - 1);
        }

        // And hopefully we're done...
        if (line.at(j).isSpace()) {
          return KateViI::Cursor(i, j - 1);
        }
        j = prev;
      }
    }
    linenum = i;
    prev = column;
    column = 0;
  }

  return KateViI::Cursor(linenum, j - 1);
}

KateViI::Cursor NormalViMode::findParagraphStart() {
  KateViI::Cursor c(m_interface->cursorPosition());
  const bool firstBlank = m_interface->line(c.line()).isEmpty();
  int prev = c.line();

  for (int i = prev; i >= 0; i--) {
    if (m_interface->line(i).isEmpty()) {
      if (i != prev) {
        prev = i + 1;
      }

      /* Skip consecutive empty lines. */
      if (firstBlank) {
        i--;
        for (; i >= 0 && m_interface->line(i).isEmpty(); i--, prev--)
          ;
      }
      return KateViI::Cursor(prev, 0);
    }
  }
  return KateViI::Cursor(0, 0);
}

KateViI::Cursor NormalViMode::findParagraphEnd() {
  KateViI::Cursor c(m_interface->cursorPosition());
  int prev = c.line(), lines = m_interface->lines();
  const bool firstBlank = m_interface->line(prev).isEmpty();

  for (int i = prev; i < lines; i++) {
    if (m_interface->line(i).isEmpty()) {
      if (i != prev) {
        prev = i - 1;
      }

      /* Skip consecutive empty lines. */
      if (firstBlank) {
        i++;
        for (; i < lines && m_interface->line(i).isEmpty(); i++, prev++)
          ;
      }
      int length = m_interface->lineLength(prev);
      return KateViI::Cursor(prev, (length <= 0) ? 0 : length - 1);
    }
  }
  return m_interface->documentEnd();
}

Range NormalViMode::textObjectInnerSentence() {
  Range r;
  KateViI::Cursor c1 = findSentenceStart();
  KateViI::Cursor c2 = findSentenceEnd();
  updateCursor(c1);

  r.startLine = c1.line();
  r.startColumn = c1.column();
  r.endLine = c2.line();
  r.endColumn = c2.column();
  return r;
}

Range NormalViMode::textObjectASentence() {
  int i;
  Range r = textObjectInnerSentence();
  const QString &line = m_interface->line(r.endLine);

  // Skip whitespaces and tabs.
  for (i = r.endColumn + 1; i < line.size(); i++) {
    if (!line.at(i).isSpace()) {
      break;
    }
  }
  r.endColumn = i - 1;

  // Remove preceding spaces.
  if (r.startColumn != 0) {
    if (r.endColumn == line.size() - 1 && !line.at(r.endColumn).isSpace()) {
      const QString &line = m_interface->line(r.startLine);
      for (i = r.startColumn - 1; i >= 0; i--) {
        if (!line.at(i).isSpace()) {
          break;
        }
      }
      r.startColumn = i + 1;
    }
  }
  return r;
}

Range NormalViMode::textObjectInnerParagraph() {
  Range r;
  KateViI::Cursor c1 = findParagraphStart();
  KateViI::Cursor c2 = findParagraphEnd();
  updateCursor(c1);

  r.startLine = c1.line();
  r.startColumn = c1.column();
  r.endLine = c2.line();
  r.endColumn = c2.column();
  return r;
}

Range NormalViMode::textObjectAParagraph() {
  Range r = textObjectInnerParagraph();
  int lines = m_interface->lines();

  if (r.endLine + 1 < lines) {
    // If the next line is empty, remove all subsequent empty lines.
    // Otherwise we'll grab the next paragraph.
    if (m_interface->line(r.endLine + 1).isEmpty()) {
      for (int i = r.endLine + 1; i < lines && m_interface->line(i).isEmpty(); i++) {
        r.endLine++;
      }
      r.endColumn = 0;
    } else {
      KateViI::Cursor prev = m_interface->cursorPosition();
      KateViI::Cursor c(r.endLine + 1, 0);
      updateCursor(c);
      c = findParagraphEnd();
      updateCursor(prev);
      r.endLine = c.line();
      r.endColumn = c.column();
    }
  } else if (m_interface->lineLength(r.startLine) > 0) {
    // We went too far, but maybe we can grab previous empty lines.
    for (int i = r.startLine - 1; i >= 0 && m_interface->line(i).isEmpty(); i--) {
      r.startLine--;
    }
    r.startColumn = 0;
    updateCursor(KateViI::Cursor(r.startLine, r.startColumn));
  } else {
    // We went too far and we're on empty lines, do nothing.
    return Range::invalid();
  }
  return r;
}

Range NormalViMode::textObjectAQuoteDouble() {
  return findSurroundingQuotes(QLatin1Char('"'), false);
}

Range NormalViMode::textObjectInnerQuoteDouble() {
  return findSurroundingQuotes(QLatin1Char('"'), true);
}

Range NormalViMode::textObjectAQuoteSingle() {
  return findSurroundingQuotes(QLatin1Char('\''), false);
}

Range NormalViMode::textObjectInnerQuoteSingle() {
  return findSurroundingQuotes(QLatin1Char('\''), true);
}

Range NormalViMode::textObjectABackQuote() {
  return findSurroundingQuotes(QLatin1Char('`'), false);
}

Range NormalViMode::textObjectInnerBackQuote() {
  return findSurroundingQuotes(QLatin1Char('`'), true);
}

Range NormalViMode::textObjectAParen() {
  return findSurroundingBrackets(QLatin1Char('('), QLatin1Char(')'), false, QLatin1Char('('),
                                 QLatin1Char(')'));
}

Range NormalViMode::textObjectInnerParen() {
  return findSurroundingBrackets(QLatin1Char('('), QLatin1Char(')'), true, QLatin1Char('('),
                                 QLatin1Char(')'));
}

Range NormalViMode::textObjectABracket() {
  return findSurroundingBrackets(QLatin1Char('['), QLatin1Char(']'), false, QLatin1Char('['),
                                 QLatin1Char(']'));
}

Range NormalViMode::textObjectInnerBracket() {
  return findSurroundingBrackets(QLatin1Char('['), QLatin1Char(']'), true, QLatin1Char('['),
                                 QLatin1Char(']'));
}

Range NormalViMode::textObjectACurlyBracket() {
  return findSurroundingBrackets(QLatin1Char('{'), QLatin1Char('}'), false, QLatin1Char('{'),
                                 QLatin1Char('}'));
}

Range NormalViMode::textObjectInnerCurlyBracket() {
  const Range allBetweenCurlyBrackets = findSurroundingBrackets(
      QLatin1Char('{'), QLatin1Char('}'), true, QLatin1Char('{'), QLatin1Char('}'));
  // Emulate the behaviour of vim, which tries to leave the closing bracket on
  // its own line if it was originally on a line different to that of the
  // opening bracket.
  Range innerCurlyBracket(allBetweenCurlyBrackets);

  if (innerCurlyBracket.startLine != innerCurlyBracket.endLine) {
    const bool openingBraceIsLastCharOnLine =
        innerCurlyBracket.startColumn == m_interface->line(innerCurlyBracket.startLine).length();
    const bool stuffToDeleteIsAllOnEndLine =
        openingBraceIsLastCharOnLine &&
        innerCurlyBracket.endLine == innerCurlyBracket.startLine + 1;
    const QString textLeadingClosingBracket =
        m_interface->line(innerCurlyBracket.endLine).mid(0, innerCurlyBracket.endColumn + 1);
    const bool closingBracketHasLeadingNonWhitespace =
        !textLeadingClosingBracket.trimmed().isEmpty();
    if (stuffToDeleteIsAllOnEndLine) {
      if (!closingBracketHasLeadingNonWhitespace) {
        // Nothing there to select - abort.
        return Range::invalid();
      } else {
        // Shift the beginning of the range to the start of the line containing
        // the closing bracket.
        innerCurlyBracket.startLine++;
        innerCurlyBracket.startColumn = 0;
      }
    } else {
      if (openingBraceIsLastCharOnLine && !closingBracketHasLeadingNonWhitespace) {
        innerCurlyBracket.startLine++;
        innerCurlyBracket.startColumn = 0;
        m_lastMotionWasLinewiseInnerBlock = true;
      }
      {
        // The line containing the end bracket is left alone if the end bracket
        // is preceded by just whitespace, else we need to delete everything
        // (i.e. end up with "{}")
        if (!closingBracketHasLeadingNonWhitespace) {
          // Shrink the endpoint of the range so that it ends at the end of the
          // line above, leaving the closing bracket on its own line.
          innerCurlyBracket.endLine--;
          innerCurlyBracket.endColumn = m_interface->line(innerCurlyBracket.endLine).length();
        }
      }
    }
  }
  return innerCurlyBracket;
}

Range NormalViMode::textObjectAInequalitySign() {

  return findSurroundingBrackets(QLatin1Char('<'), QLatin1Char('>'), false, QLatin1Char('<'),
                                 QLatin1Char('>'));
}

Range NormalViMode::textObjectInnerInequalitySign() {

  return findSurroundingBrackets(QLatin1Char('<'), QLatin1Char('>'), true, QLatin1Char('<'),
                                 QLatin1Char('>'));
}

Range NormalViMode::textObjectAComma() { return textObjectComma(false); }

Range NormalViMode::textObjectInnerComma() { return textObjectComma(true); }

// Add commands.
// When adding commands here, remember to add them to visual mode too (if
// applicable).
void NormalViMode::initializeCommands() {
  ADDCMD("a", commandEnterInsertModeAppend, IS_CHANGE);
  ADDCMD("A", commandEnterInsertModeAppendEOL, IS_CHANGE);
  ADDCMD("i", commandEnterInsertMode, IS_CHANGE);
  ADDCMD("<insert>", commandEnterInsertMode, IS_CHANGE);
  ADDCMD("I", commandEnterInsertModeBeforeFirstNonBlankInLine, IS_CHANGE);
  ADDCMD("gi", commandEnterInsertModeLast, IS_CHANGE);
  ADDCMD("v", commandEnterVisualMode, 0);
  ADDCMD("V", commandEnterVisualLineMode, 0);
  /*
  ADDCMD("<c-v>", commandEnterVisualBlockMode, 0);
  ADDCMD("gv", commandReselectVisual, SHOULD_NOT_RESET);
  */
  ADDCMD("o", commandOpenNewLineUnder, IS_CHANGE);
  ADDCMD("O", commandOpenNewLineOver, IS_CHANGE);
  ADDCMD("J", commandJoinLines, IS_CHANGE);
  ADDCMD("c", commandChange, IS_CHANGE | NEEDS_MOTION);
  ADDCMD("C", commandChangeToEOL, IS_CHANGE);
  ADDCMD("cc", commandChangeLine, IS_CHANGE);
  ADDCMD("s", commandSubstituteChar, IS_CHANGE);
  ADDCMD("S", commandSubstituteLine, IS_CHANGE);
  ADDCMD("dd", commandDeleteLine, IS_CHANGE);
  ADDCMD("d", commandDelete, IS_CHANGE | NEEDS_MOTION);
  ADDCMD("D", commandDeleteToEOL, IS_CHANGE);
  ADDCMD("x", commandDeleteChar, IS_CHANGE);
  ADDCMD("<delete>", commandDeleteChar, IS_CHANGE);
  ADDCMD("X", commandDeleteCharBackward, IS_CHANGE);
  ADDCMD("gu", commandMakeLowercase, IS_CHANGE | NEEDS_MOTION);
  ADDCMD("guu", commandMakeLowercaseLine, IS_CHANGE);
  ADDCMD("gU", commandMakeUppercase, IS_CHANGE | NEEDS_MOTION);
  ADDCMD("gUU", commandMakeUppercaseLine, IS_CHANGE);
  ADDCMD("y", commandYank, NEEDS_MOTION);
  ADDCMD("yy", commandYankLine, 0);
  // TODO: Different from Vi: Y should yank lines instead of yank to the end of
  // the line.
  ADDCMD("Y", commandYankToEOL, 0);
  ADDCMD("p", commandPaste, IS_CHANGE);
  ADDCMD("P", commandPasteBefore, IS_CHANGE);
  ADDCMD("gp", commandgPaste, IS_CHANGE);
  ADDCMD("gP", commandgPasteBefore, IS_CHANGE);
  ADDCMD("]p", commandIndentedPaste, IS_CHANGE);
  ADDCMD("[p", commandIndentedPasteBefore, IS_CHANGE);
  ADDCMD("r.", commandReplaceCharacter, IS_CHANGE | REGEX_PATTERN);
  ADDCMD("R", commandEnterReplaceMode, IS_CHANGE);
  ADDCMD(":", commandSwitchToCmdLine, 0);
  ADDCMD("u", commandUndo, 0);
  ADDCMD("<c-r>", commandRedo, 0);
  // TODO: U in Vi is undo all changes in one line. Do not support it for now.
  // ADDCMD("U", commandRedo, 0);
  ADDCMD("m.", commandSetMark, REGEX_PATTERN);
  ADDCMD(">>", commandIndentLine, IS_CHANGE);
  ADDCMD("<<", commandUnindentLine, IS_CHANGE);
  ADDCMD(">", commandIndentLines, IS_CHANGE | NEEDS_MOTION);
  ADDCMD("<", commandUnindentLines, IS_CHANGE | NEEDS_MOTION);
  ADDCMD("<c-f>", commandScrollPageDown, 0);
  ADDCMD("<pagedown>", commandScrollPageDown, 0);
  ADDCMD("<c-b>", commandScrollPageUp, 0);
  ADDCMD("<pageup>", commandScrollPageUp, 0);
  ADDCMD("<c-u>", commandScrollHalfPageUp, 0);
  ADDCMD("<c-d>", commandScrollHalfPageDown, 0);
  ADDCMD("z.", commandCenterViewOnNonBlank, 0);
  ADDCMD("zz", commandCenterViewOnCursor, 0);
  ADDCMD("z<return>", commandTopViewOnNonBlank, 0);
  ADDCMD("zt", commandTopViewOnCursor, 0);
  ADDCMD("z-", commandBottomViewOnNonBlank, 0);
  ADDCMD("zb", commandBottomViewOnCursor, 0);
  /*
  ADDCMD("ga", commandPrintCharacterCode, SHOULD_NOT_RESET);
  */
  ADDCMD(".", commandRepeatLastChange, 0);
  ADDCMD("==", commandAlignLine, IS_CHANGE);
  ADDCMD("=", commandAlignLines, IS_CHANGE | NEEDS_MOTION);
  ADDCMD("~", commandChangeCase, IS_CHANGE);
  ADDCMD("g~", commandChangeCaseRange, IS_CHANGE | NEEDS_MOTION);
  ADDCMD("g~~", commandChangeCaseLine, IS_CHANGE);
  ADDCMD("<c-a>", commandAddToNumber, IS_CHANGE);
  ADDCMD("<c-x>", commandSubtractFromNumber, IS_CHANGE);
  ADDCMD("<c-o>", commandGoToPrevJump, 0);
  ADDCMD("<c-i>", commandGoToNextJump, 0);

  ADDCMD("<c-w>h", commandSwitchToLeftView, 0);
  ADDCMD("<c-w><c-h>", commandSwitchToLeftView, 0);
  ADDCMD("<c-w><left>", commandSwitchToLeftView, 0);
  ADDCMD("<c-w>j", commandSwitchToDownView, 0);
  ADDCMD("<c-w><c-j>", commandSwitchToDownView, 0);
  ADDCMD("<c-w><down>", commandSwitchToDownView, 0);
  ADDCMD("<c-w>k", commandSwitchToUpView, 0);
  ADDCMD("<c-w><c-k>", commandSwitchToUpView, 0);
  ADDCMD("<c-w><up>", commandSwitchToUpView, 0);
  ADDCMD("<c-w>l", commandSwitchToRightView, 0);
  ADDCMD("<c-w><c-l>", commandSwitchToRightView, 0);
  ADDCMD("<c-w><right>", commandSwitchToRightView, 0);
  ADDCMD("<c-w>w", commandSwitchToNextView, 0);
  ADDCMD("<c-w><c-w>", commandSwitchToNextView, 0);

  ADDCMD("<c-w>s", commandSplitHoriz, 0);
  ADDCMD("<c-w>S", commandSplitHoriz, 0);
  ADDCMD("<c-w><c-s>", commandSplitHoriz, 0);
  ADDCMD("<c-w>v", commandSplitVert, 0);
  ADDCMD("<c-w><c-v>", commandSplitVert, 0);
  ADDCMD("<c-w>c", commandCloseView, 0);

  ADDCMD("gt", commandSwitchToNextTab, 0);
  ADDCMD("gT", commandSwitchToPrevTab, 0);

  /*
  ADDCMD("gqq", commandFormatLine, IS_CHANGE);
  ADDCMD("gq", commandFormatLines, IS_CHANGE | NEEDS_MOTION);

  ADDCMD("zo", commandExpandLocal, 0);
  ADDCMD("zc", commandCollapseLocal, 0);
  ADDCMD("za", commandToggleRegionVisibility, 0);
  ADDCMD("zr", commandExpandAll, 0);
  ADDCMD("zm", commandCollapseToplevelNodes, 0);
  */

  ADDCMD("q.", commandStartRecordingMacro, REGEX_PATTERN);
  ADDCMD("@.", commandReplayMacro, REGEX_PATTERN);

  ADDCMD("ZZ", commandCloseWrite, 0);
  ADDCMD("ZQ", commandCloseNocheck, 0);

  // regular motions
  ADDMOTION("h", motionLeft, 0);
  ADDMOTION("<left>", motionLeft, 0);
  ADDMOTION("<backspace>", motionLeft, 0);
  ADDMOTION("j", motionDown, 0);
  ADDMOTION("<down>", motionDown, 0);
  ADDMOTION("<enter>", motionDownToFirstNonBlank, 0);
  ADDMOTION("<return>", motionDownToFirstNonBlank, 0);
  ADDMOTION("k", motionUp, 0);
  ADDMOTION("<up>", motionUp, 0);
  ADDMOTION("-", motionUpToFirstNonBlank, 0);
  ADDMOTION("l", motionRight, 0);
  ADDMOTION("<right>", motionRight, 0);
  ADDMOTION(" ", motionRight, 0);
  ADDMOTION("$", motionToEOL, 0);
  ADDMOTION("<end>", motionToEOL, 0);
  ADDMOTION("0", motionToColumn0, 0);
  ADDMOTION("<home>", motionToColumn0, 0);
  ADDMOTION("^", motionToFirstCharacterOfLine, 0);
  ADDMOTION("f.", motionFindChar, REGEX_PATTERN);
  ADDMOTION("F.", motionFindCharBackward, REGEX_PATTERN);
  ADDMOTION("t.", motionToChar, REGEX_PATTERN);
  ADDMOTION("T.", motionToCharBackward, REGEX_PATTERN);
  ADDMOTION(";", motionRepeatlastTF, 0);
  ADDMOTION(",", motionRepeatlastTFBackward, 0);
  /*
  ADDMOTION("n", motionFindNext, 0);
  ADDMOTION("N", motionFindPrev, 0);
  */
  ADDMOTION("gg", motionToLineFirst, 0);
  ADDMOTION("G", motionToLineLast, 0);
  ADDMOTION("w", motionWordForward, IS_NOT_LINEWISE);
  ADDMOTION("W", motionWORDForward, IS_NOT_LINEWISE);
  ADDMOTION("<c-right>", motionWordForward, IS_NOT_LINEWISE);
  ADDMOTION("<c-left>", motionWordBackward, IS_NOT_LINEWISE);
  ADDMOTION("b", motionWordBackward, 0);
  ADDMOTION("B", motionWORDBackward, 0);
  ADDMOTION("e", motionToEndOfWord, 0);
  ADDMOTION("E", motionToEndOfWORD, 0);
  ADDMOTION("ge", motionToEndOfPrevWord, 0);
  ADDMOTION("gE", motionToEndOfPrevWORD, 0);
  ADDMOTION("|", motionToScreenColumn, 0);
  ADDMOTION("%", motionToMatchingItem, IS_NOT_LINEWISE);
  /*
  ADDMOTION("`[a-zA-Z^><\\.\\[\\]]", motionToMark, REGEX_PATTERN);
  ADDMOTION("'[a-zA-Z^><]", motionToMarkLine, REGEX_PATTERN);
  */
  ADDMOTION("[[", motionToPreviousBraceBlockStart, IS_NOT_LINEWISE);
  ADDMOTION("]]", motionToNextBraceBlockStart, IS_NOT_LINEWISE);
  ADDMOTION("[]", motionToPreviousBraceBlockEnd, IS_NOT_LINEWISE);
  ADDMOTION("][", motionToNextBraceBlockEnd, IS_NOT_LINEWISE);
  /*
  ADDMOTION("*", motionToNextOccurrence, 0);
  ADDMOTION("#", motionToPrevOccurrence, 0);
  */
  ADDMOTION("H", motionToFirstLineOfWindow, 0);
  ADDMOTION("M", motionToMiddleLineOfWindow, 0);
  ADDMOTION("L", motionToLastLineOfWindow, 0);
  ADDMOTION("gj", motionToNextVisualLine, 0);
  ADDMOTION("gk", motionToPrevVisualLine, 0);
  ADDMOTION("(", motionToPreviousSentence, 0);
  ADDMOTION(")", motionToNextSentence, 0);
  ADDMOTION("{", motionToBeforeParagraph, 0);
  ADDMOTION("}", motionToAfterParagraph, 0);

  // text objects
  ADDMOTION("iw", textObjectInnerWord, 0);
  ADDMOTION("aw", textObjectAWord, IS_NOT_LINEWISE);
  ADDMOTION("iW", textObjectInnerWORD, 0);
  ADDMOTION("aW", textObjectAWORD, IS_NOT_LINEWISE);
  ADDMOTION("is", textObjectInnerSentence, IS_NOT_LINEWISE);
  ADDMOTION("as", textObjectASentence, IS_NOT_LINEWISE);
  ADDMOTION("ip", textObjectInnerParagraph, IS_NOT_LINEWISE);
  ADDMOTION("ap", textObjectAParagraph, IS_NOT_LINEWISE);
  ADDMOTION("i\"", textObjectInnerQuoteDouble, IS_NOT_LINEWISE);
  ADDMOTION("a\"", textObjectAQuoteDouble, IS_NOT_LINEWISE);
  ADDMOTION("i'", textObjectInnerQuoteSingle, IS_NOT_LINEWISE);
  ADDMOTION("a'", textObjectAQuoteSingle, IS_NOT_LINEWISE);
  ADDMOTION("i`", textObjectInnerBackQuote, IS_NOT_LINEWISE);
  ADDMOTION("a`", textObjectABackQuote, IS_NOT_LINEWISE);
  // TODO: Support repeat to (){}<>[].
  ADDMOTION("i[()b]", textObjectInnerParen, REGEX_PATTERN | IS_NOT_LINEWISE);
  ADDMOTION("a[()b]", textObjectAParen, REGEX_PATTERN | IS_NOT_LINEWISE);
  ADDMOTION("i[{}B]", textObjectInnerCurlyBracket, REGEX_PATTERN | IS_NOT_LINEWISE);
  ADDMOTION("a[{}B]", textObjectACurlyBracket, REGEX_PATTERN | IS_NOT_LINEWISE);
  ADDMOTION("i[><]", textObjectInnerInequalitySign, REGEX_PATTERN | IS_NOT_LINEWISE);
  ADDMOTION("a[><]", textObjectAInequalitySign, REGEX_PATTERN | IS_NOT_LINEWISE);
  ADDMOTION("i[\\[\\]]", textObjectInnerBracket, REGEX_PATTERN | IS_NOT_LINEWISE);
  ADDMOTION("a[\\[\\]]", textObjectABracket, REGEX_PATTERN | IS_NOT_LINEWISE);
  // Not supported by Vim.
  // FIXME: Comment it out since the implementation has some bugs.
  /*
  ADDMOTION("i,", textObjectInnerComma, IS_NOT_LINEWISE);
  ADDMOTION("a,", textObjectAComma, IS_NOT_LINEWISE);
  */

  /*
  ADDMOTION("/<enter>", motionToIncrementalSearchMatch, IS_NOT_LINEWISE);
  ADDMOTION("?<enter>", motionToIncrementalSearchMatch, IS_NOT_LINEWISE);
  */
}

QRegularExpression NormalViMode::generateMatchingItemRegex() const {
  QString pattern(QStringLiteral("\\[|\\]|\\{|\\}|\\(|\\)|"));
  QList<QString> keys = m_matchingItems.keys();

  for (int i = 0; i < keys.size(); i++) {
    QString s = m_matchingItems[keys[i]];
    s.remove(QRegularExpression(QLatin1String("^-")));
    s.replace(QRegularExpression(QLatin1String("\\*")), QStringLiteral("\\*"));
    s.replace(QRegularExpression(QLatin1String("\\+")), QStringLiteral("\\+"));
    s.replace(QRegularExpression(QLatin1String("\\[")), QStringLiteral("\\["));
    s.replace(QRegularExpression(QLatin1String("\\]")), QStringLiteral("\\]"));
    s.replace(QRegularExpression(QLatin1String("\\(")), QStringLiteral("\\("));
    s.replace(QRegularExpression(QLatin1String("\\)")), QStringLiteral("\\)"));
    s.replace(QRegularExpression(QLatin1String("\\{")), QStringLiteral("\\{"));
    s.replace(QRegularExpression(QLatin1String("\\}")), QStringLiteral("\\}"));

    pattern.append(s);

    if (i != keys.size() - 1) {
      pattern.append(QLatin1Char('|'));
    }
  }

  return QRegularExpression(pattern);
}

// returns the operation mode that should be used. this is decided by using the
// following heuristic:
// 1. if we're in visual block mode, it should be Block
// 2. if we're in visual line mode OR the range spans several lines, it should
// be LineWise
// 3. if neither of these is true, CharWise is returned
// 4. there are some motion that makes all operator charwise, if we have one of
// them mode will be CharWise
OperationMode NormalViMode::getOperationMode() const {
  OperationMode m = CharWise;

  if (m_viInputModeManager->getCurrentViMode() == ViMode::VisualBlockMode) {
    m = Block;
  } else if (m_viInputModeManager->getCurrentViMode() == ViMode::VisualLineMode ||
             (m_commandRange.startLine != m_commandRange.endLine &&
              m_viInputModeManager->getCurrentViMode() != ViMode::VisualMode)) {
    m = LineWise;
  }

  if (m_commandWithMotion && !m_linewiseCommand) {
    m = CharWise;
  }

  if (m_lastMotionWasLinewiseInnerBlock) {
    m = LineWise;
  }

  return m;
}

bool NormalViMode::paste(PasteLocation pasteLocation, bool isgPaste, bool isIndentedPaste) {
  KateViI::Cursor pasteAt(m_interface->cursorPosition());
  KateViI::Cursor cursorAfterPaste = pasteAt;
  QChar reg = getChosenRegister(UnnamedRegister);

  OperationMode m = getRegisterFlag(reg);
  QString textToInsert = getRegisterContent(reg);
  const bool isTextMultiLine = textToInsert.count(QLatin1Char('\n')) > 0;

  // In temporary normal mode, p/P act as gp/gP.
  isgPaste |= m_viInputModeManager->getTemporaryNormalMode();

  if (textToInsert.isEmpty()) {
    error(tr("Nothing in register %1.", c_kateViTranslationContext).arg(reg));
    return false;
  }

  if (getCount() > 1) {
    // FIXME: does this make sense for blocks?
    textToInsert = textToInsert.repeated(getCount());
  }

  if (m == LineWise) {
    pasteAt.setColumn(0);
    if (isIndentedPaste) {
      // Note that this does indeed work if there is no non-whitespace on the
      // current line or if the line is empty!
      const QString lineText = m_interface->line(pasteAt.line());
      const QString leadingWhiteSpaceOnCurrentLine =
          lineText.mid(0, lineText.indexOf(QRegularExpression(QLatin1String("[^\\s]"))));
      const QString leadingWhiteSpaceOnFirstPastedLine =
          textToInsert.mid(0, textToInsert.indexOf(QRegularExpression(QLatin1String("[^\\s]"))));
      // QString has no "left trim" method, bizarrely.
      while (textToInsert[0].isSpace()) {
        textToInsert = textToInsert.mid(1);
      }
      textToInsert.prepend(leadingWhiteSpaceOnCurrentLine);
      // Remove the last \n, temporarily: we're going to alter the indentation
      // of each pasted line by doing a search and replace on '\n's, but don't
      // want to alter this one.
      textToInsert.chop(1);
      textToInsert.replace(QLatin1Char('\n') + leadingWhiteSpaceOnFirstPastedLine,
                           QLatin1Char('\n') + leadingWhiteSpaceOnCurrentLine);
      textToInsert.append(QLatin1Char('\n')); // Re-add the temporarily removed last '\n'.
    }
    if (pasteLocation == AfterCurrentPosition) {
      textToInsert.chop(1); // remove the last \n
      pasteAt.setColumn(
          m_interface->lineLength(pasteAt.line())); // paste after the current line and ...
      textToInsert.prepend(QLatin1Char('\n')); // ... prepend a \n, so the text starts on a new line

      cursorAfterPaste.setLine(cursorAfterPaste.line() + 1);
    }
    if (isgPaste) {
      cursorAfterPaste.setLine(cursorAfterPaste.line() + textToInsert.count(QLatin1Char('\n')));
    }
  } else {
    if (pasteLocation == AfterCurrentPosition) {
      // Move cursor forward one before we paste.  The position after the paste
      // must also be updated accordingly.
      if (getLine(pasteAt.line()).length() > 0) {
        pasteAt.setColumn(pasteAt.column() + 1);
      }
      cursorAfterPaste = pasteAt;
    }
    const bool leaveCursorAtStartOfPaste = isTextMultiLine && !isgPaste;
    if (!leaveCursorAtStartOfPaste) {
      cursorAfterPaste = cursorPosAtEndOfPaste(pasteAt, textToInsert);
      if (!isgPaste) {
        cursorAfterPaste.setColumn(cursorAfterPaste.column() - 1);
      }
    }
  }

  m_interface->editBegin();
  if (m_interface->selection()) {
    pasteAt = m_interface->selectionRange().start();
    m_interface->removeText(m_interface->selectionRange());
  }
  m_interface->insertText(pasteAt, textToInsert, m == Block);
  m_interface->editEnd();

  if (cursorAfterPaste.line() >= m_interface->lines()) {
    cursorAfterPaste.setLine(m_interface->lines() - 1);
  }
  updateCursor(cursorAfterPaste);

  return true;
}

KateViI::Cursor NormalViMode::cursorPosAtEndOfPaste(const KateViI::Cursor &pasteLocation,
                                                    const QString &pastedText) const {
  KateViI::Cursor cAfter = pasteLocation;
  const auto textLines = pastedText.split(QLatin1Char('\n'));
  if (textLines.length() == 1) {
    cAfter.setColumn(cAfter.column() + pastedText.length());
  } else {
    cAfter.setColumn(textLines.last().length() - 0);
    cAfter.setLine(cAfter.line() + textLines.length() - 1);
  }
  return cAfter;
}

void NormalViMode::joinLines(unsigned int from, unsigned int to) const {
  // make sure we don't try to join lines past the document end
  if (to >= (unsigned int)(m_interface->lines())) {
    to = m_interface->lines() - 1;
  }

  // joining one line is a no-op
  if (from == to) {
    return;
  }

  m_interface->joinLines(from, to, true);
}

#if 0
void NormalViMode::reformatLines(unsigned int from, unsigned int to) const
{
    joinLines(from, to);
    doc()->wrapText(from, to);
}
#endif

int NormalViMode::getFirstNonBlank(int line) const {
  if (line < 0) {
    line = m_interface->cursorPosition().line();
  }

  const auto text = m_interface->line(line);
  for (int i = 0; i < text.length(); ++i) {
    if (!text[i].isSpace()) {
      return i;
    }
  }

  return 0;
}

// Tries to shrinks toShrink so that it fits tightly around rangeToShrinkTo.
void NormalViMode::shrinkRangeAroundCursor(Range &toShrink, const Range &rangeToShrinkTo) const {
  if (!toShrink.valid || !rangeToShrinkTo.valid) {
    return;
  }
  KateViI::Cursor cursorPos = m_interface->cursorPosition();
  if (rangeToShrinkTo.startLine >= cursorPos.line()) {
    if (rangeToShrinkTo.startLine > cursorPos.line()) {
      // Does not surround cursor; aborting.
      return;
    }
    Q_ASSERT(rangeToShrinkTo.startLine == cursorPos.line());
    if (rangeToShrinkTo.startColumn > cursorPos.column()) {
      // Does not surround cursor; aborting.
      return;
    }
  }
  if (rangeToShrinkTo.endLine <= cursorPos.line()) {
    if (rangeToShrinkTo.endLine < cursorPos.line()) {
      // Does not surround cursor; aborting.
      return;
    }
    Q_ASSERT(rangeToShrinkTo.endLine == cursorPos.line());
    if (rangeToShrinkTo.endColumn < cursorPos.column()) {
      // Does not surround cursor; aborting.
      return;
    }
  }

  if (toShrink.startLine <= rangeToShrinkTo.startLine) {
    if (toShrink.startLine < rangeToShrinkTo.startLine) {
      toShrink.startLine = rangeToShrinkTo.startLine;
      toShrink.startColumn = rangeToShrinkTo.startColumn;
    }
    Q_ASSERT(toShrink.startLine == rangeToShrinkTo.startLine);
    if (toShrink.startColumn < rangeToShrinkTo.startColumn) {
      toShrink.startColumn = rangeToShrinkTo.startColumn;
    }
  }
  if (toShrink.endLine >= rangeToShrinkTo.endLine) {
    if (toShrink.endLine > rangeToShrinkTo.endLine) {
      toShrink.endLine = rangeToShrinkTo.endLine;
      toShrink.endColumn = rangeToShrinkTo.endColumn;
    }
    Q_ASSERT(toShrink.endLine == rangeToShrinkTo.endLine);
    if (toShrink.endColumn > rangeToShrinkTo.endColumn) {
      toShrink.endColumn = rangeToShrinkTo.endColumn;
    }
  }
}

Range NormalViMode::textObjectComma(bool inner) const {
  // Basic algorithm: look left and right of the cursor for all combinations
  // of enclosing commas and the various types of brackets, and pick the pair
  // closest to the cursor that surrounds the cursor.
  Range r(0, 0, m_interface->lines(), m_interface->line(m_interface->lastLine()).length(),
          InclusiveMotion);

  shrinkRangeAroundCursor(r, findSurroundingQuotes(QLatin1Char(','), inner));
  shrinkRangeAroundCursor(r, findSurroundingBrackets(QLatin1Char('('), QLatin1Char(')'), inner,
                                                     QLatin1Char('('), QLatin1Char(')')));
  shrinkRangeAroundCursor(r, findSurroundingBrackets(QLatin1Char('{'), QLatin1Char('}'), inner,
                                                     QLatin1Char('{'), QLatin1Char('}')));
  shrinkRangeAroundCursor(r, findSurroundingBrackets(QLatin1Char(','), QLatin1Char(')'), inner,
                                                     QLatin1Char('('), QLatin1Char(')')));
  shrinkRangeAroundCursor(r, findSurroundingBrackets(QLatin1Char(','), QLatin1Char(']'), inner,
                                                     QLatin1Char('['), QLatin1Char(']')));
  shrinkRangeAroundCursor(r, findSurroundingBrackets(QLatin1Char(','), QLatin1Char('}'), inner,
                                                     QLatin1Char('{'), QLatin1Char('}')));
  shrinkRangeAroundCursor(r, findSurroundingBrackets(QLatin1Char('('), QLatin1Char(','), inner,
                                                     QLatin1Char('('), QLatin1Char(')')));
  shrinkRangeAroundCursor(r, findSurroundingBrackets(QLatin1Char('['), QLatin1Char(','), inner,
                                                     QLatin1Char('['), QLatin1Char(']')));
  shrinkRangeAroundCursor(r, findSurroundingBrackets(QLatin1Char('{'), QLatin1Char(','), inner,
                                                     QLatin1Char('{'), QLatin1Char('}')));
  return r;
}

void NormalViMode::updateYankHighlightAttrib() {
#if 0
    if (!m_highlightYankAttribute) {
        m_highlightYankAttribute = new KTextEditor::Attribute;
    }
    const QColor &yankedColor = m_view->renderer()->config()->savedLineColor();
    m_highlightYankAttribute->setBackground(yankedColor);
    KTextEditor::Attribute::Ptr mouseInAttribute(new KTextEditor::Attribute());
    mouseInAttribute->setFontBold(true);
    m_highlightYankAttribute->setDynamicAttribute(KTextEditor::Attribute::ActivateMouseIn, mouseInAttribute);
    m_highlightYankAttribute->dynamicAttribute(KTextEditor::Attribute::ActivateMouseIn)->setBackground(yankedColor);
#endif
}

void NormalViMode::highlightYank(const Range &range, const OperationMode mode) {
  clearYankHighlight();

  // current MovingRange doesn't support block mode selection so split the
  // block range into per-line ranges
  if (mode == Block) {
    for (int i = range.startLine; i <= range.endLine; i++) {
      addHighlightYank(KateViI::Range(i, range.startColumn, i, range.endColumn));
    }
  } else {
    addHighlightYank(
        KateViI::Range(range.startLine, range.startColumn, range.endLine, range.endColumn));
  }
}

void NormalViMode::addHighlightYank(const KateViI::Range &yankRange) { KATEVI_NIY; }

void NormalViMode::clearYankHighlight() { KATEVI_NIY; }

void NormalViMode::aboutToDeleteMovingInterfaceContent() { KATEVI_NIY; }

bool NormalViMode::waitingForRegisterOrCharToSearch() const {
  // r, q, @ are never preceded by operators. There will always be a keys size
  // of 1 for them. f, t, F, T can be preceded by a delete/replace/yank/indent
  // operator. size = 2 in that case. f, t, F, T can be preceded by 'g'
  // case/formatting operators. size = 3 in that case.
  const int keysSize = m_keys.size();
  if (keysSize < 1) {
    // Just being defensive there.
    return false;
  }
  if (keysSize > 1) {
    // Multi-letter operation.
    QChar cPrefix = m_keys[0];
    if (keysSize == 2) {
      // delete/replace/yank/indent operator?
      if (cPrefix != QLatin1Char('c') && cPrefix != QLatin1Char('d') &&
          cPrefix != QLatin1Char('y') && cPrefix != QLatin1Char('=') &&
          cPrefix != QLatin1Char('>') && cPrefix != QLatin1Char('<')) {
        return false;
      }
    } else if (keysSize == 3) {
      // We need to look deeper. Is it a g motion?
      QChar cNextfix = m_keys[1];
      if (cPrefix != QLatin1Char('g') ||
          (cNextfix != QLatin1Char('U') && cNextfix != QLatin1Char('u') &&
           cNextfix != QLatin1Char('~') && cNextfix != QLatin1Char('q') &&
           cNextfix != QLatin1Char('w') && cNextfix != QLatin1Char('@'))) {
        return false;
      }
    } else {
      return false;
    }
  }

  QChar ch = m_keys[keysSize - 1];
  return (ch == QLatin1Char('f') || ch == QLatin1Char('t') || ch == QLatin1Char('F') ||
          ch == QLatin1Char('T')
          // c/d prefix unapplicable for the following cases.
          || (keysSize == 1 &&
              (ch == QLatin1Char('r') || ch == QLatin1Char('q') || ch == QLatin1Char('@'))));
}

#if 0
void NormalViMode::textInserted(KateViI::Range range)
{
    const bool isInsertReplaceMode = (m_viInputModeManager->getCurrentViMode() == ViMode::InsertMode || m_viInputModeManager->getCurrentViMode() == ViMode::ReplaceMode);
    const bool continuesInsertion = range.start().line() == m_currentChangeEndMarker.line() && range.start().column() == m_currentChangeEndMarker.column();
    const bool beginsWithNewline = doc()->text(range).at(0) == QLatin1Char('\n');
    if (!continuesInsertion) {
        KateViI::Cursor newBeginMarkerPos = range.start();
        if (beginsWithNewline && !isInsertReplaceMode) {
            // Presumably a linewise paste, in which case we ignore the leading '\n'
            newBeginMarkerPos = KateViI::Cursor(newBeginMarkerPos.line() + 1, 0);
        }
        m_viInputModeManager->marks()->setStartEditYanked(newBeginMarkerPos);
    }
    m_viInputModeManager->marks()->setLastChange(range.start());
    KateViI::Cursor editEndMarker = range.end();
    if (!isInsertReplaceMode) {
        editEndMarker.setColumn(editEndMarker.column() - 1);
    }
    m_viInputModeManager->marks()->setFinishEditYanked(editEndMarker);
    m_currentChangeEndMarker = range.end();
    if (m_isUndo) {
        const bool addsMultipleLines = range.start().line() != range.end().line();
        m_viInputModeManager->marks()->setStartEditYanked(KateViI::Cursor(m_viInputModeManager->marks()->getStartEditYanked().line(), 0));
        if (addsMultipleLines) {
            m_viInputModeManager->marks()->setFinishEditYanked(KateViI::Cursor(m_viInputModeManager->marks()->getFinishEditYanked().line() + 1, 0));
            m_viInputModeManager->marks()->setLastChange(KateViI::Cursor(m_viInputModeManager->marks()->getLastChange().line() + 1, 0));
        } else {
            m_viInputModeManager->marks()->setFinishEditYanked(KateViI::Cursor(m_viInputModeManager->marks()->getFinishEditYanked().line(), 0));
            m_viInputModeManager->marks()->setLastChange(KateViI::Cursor(m_viInputModeManager->marks()->getLastChange().line(), 0));
        }
    }
}

void NormalViMode::textRemoved(KateViI::Range range)
{
    const bool isInsertReplaceMode = (m_viInputModeManager->getCurrentViMode() == ViMode::InsertMode || m_viInputModeManager->getCurrentViMode() == ViMode::ReplaceMode);
    m_viInputModeManager->marks()->setLastChange(range.start());
    if (!isInsertReplaceMode) {
        // Don't go resetting [ just because we did a Ctrl-h!
        m_viInputModeManager->marks()->setStartEditYanked(range.start());
    } else {
        // Don't go disrupting our continued insertion just because we did a Ctrl-h!
        m_currentChangeEndMarker = range.start();
    }
    m_viInputModeManager->marks()->setFinishEditYanked(range.start());
    if (m_isUndo) {
        // Slavishly follow Vim's weird rules: if an undo removes several lines, then all markers should
        // be at the beginning of the line after the last line removed, else they should at the beginning
        // of the line above that.
        const int markerLineAdjustment = (range.start().line() != range.end().line()) ? 1 : 0;
        m_viInputModeManager->marks()->setStartEditYanked(KateViI::Cursor(m_viInputModeManager->marks()->getStartEditYanked().line() + markerLineAdjustment, 0));
        m_viInputModeManager->marks()->setFinishEditYanked(KateViI::Cursor(m_viInputModeManager->marks()->getFinishEditYanked().line() + markerLineAdjustment, 0));
        m_viInputModeManager->marks()->setLastChange(KateViI::Cursor(m_viInputModeManager->marks()->getLastChange().line() + markerLineAdjustment, 0));
    }
}

void NormalViMode::undoBeginning()
{
    m_isUndo = true;
}

void NormalViMode::undoEnded()
{
    m_isUndo = false;
}
#endif

bool NormalViMode::executeEditorCommand(const QString &command) {
  KATEVI_NIY;
  return true;
#if 0
    KateViI::Command *p = KateCmd::self()->queryCommand(command);

    if (!p) {
        return false;
    }

    QString msg;
    return p->exec(m_view, command, msg);
#endif
}

void NormalViMode::rewriteKeysInCWCase() {
  // Special case of the special case: :-)
  // If the cursor is at the end of the current word rewrite to "cl"
  const bool isWORD = (m_keys.at(1) == QLatin1Char('W'));
  const KateViI::Cursor currentPosition(m_interface->cursorPosition());
  const KateViI::Cursor endOfWordOrWORD =
      isWORD ? findWORDEnd(currentPosition.line(), currentPosition.column() - 1, true)
             : findWordEnd(currentPosition.line(), currentPosition.column() - 1, true);

  if (currentPosition == endOfWordOrWORD) {
    m_keys = QStringLiteral("cl");
  } else {
    if (isWORD) {
      m_keys = QStringLiteral("cE");
    } else {
      m_keys = QStringLiteral("ce");
    }
  }
}

bool NormalViMode::assignRegisterFromKeys() {
  if (m_keys.size() < 2) {
    // Waiting for a register.
    return true;
  } else {
    const QChar r = m_keys[1].toLower();
    if (ViUtils::isRegister(r)) {
      m_register = r;
      m_keys.clear();
      return true;
    } else {
      resetParser();
      return true;
    }
  }
}

void NormalViMode::updateMatchingCommands() {
  // If we have any matching commands so far, check which ones still match.
  if (!m_matchingCommands.isEmpty()) {
    const int n = m_matchingCommands.size() - 1;
    // Remove commands not matching anymore.
    for (int i = n; i >= 0; i--) {
      if (!m_commands.at(m_matchingCommands.at(i))->matches(m_keys)) {
        if (m_commands.at(m_matchingCommands.at(i))->needsMotion()) {
          // "cache" command needing a motion for later
          m_motionOperatorIndex = m_matchingCommands.at(i);
        }
        m_matchingCommands.remove(i);
      }
    }

    // check if any of the matching commands need a motion/text object, if so
    // push the current command length to m_awaitingMotionOrTextObject so one
    // knows where to split the command between the operator and the motion
    for (int i = 0; i < m_matchingCommands.size(); i++) {
      if (m_commands.at(m_matchingCommands.at(i))->needsMotion()) {
        m_awaitingMotionOrTextObject.push(m_keys.size());
        break;
      }
    }
  } else {
    // Go through all registered commands and get possible matches.
    for (int i = 0; i < m_commands.size(); i++) {
      if (m_commands.at(i)->matches(m_keys)) {
        m_matchingCommands.push_back(i);
        if (m_commands.at(i)->needsMotion() &&
            m_commands.at(i)->pattern().length() == m_keys.size()) {
          m_awaitingMotionOrTextObject.push(m_keys.size());
        }
      }
    }
  }
}

bool NormalViMode::updateMatchingMotions(int p_checkFrom) {
  // FIXME: if p_checkFrom hasn't changed, only motions whose index is in
  // m_matchingMotions should be checked.
  if (p_checkFrom < m_keys.size()) {
    const auto subKeys = m_keys.mid(p_checkFrom);
    for (int i = 0; i < m_motions.size(); i++) {
      if (m_motions.at(i)->matches(subKeys)) {
        m_lastMotionWasLinewiseInnerBlock = false;
        m_matchingMotions.push_back(i);

        // If it matches exactly, we have found the motion command to execute.
        if (m_motions.at(i)->matchesExact(subKeys)) {
          m_currentMotionWasVisualLineUpOrDown = false;
          if (p_checkFrom == 0) {
            executeMotionWithoutCommand(m_motions.at(i));
          } else {
            executeMotionWithCommand(m_motions.at(i));
          }
          return true;
        }
      }
    }
  }

  return false;
}

void NormalViMode::executeMotionWithoutCommand(Motion *p_motion) {
  // No command is given before motion, just move the cursor
  // to wherever the motion says it should go to.
  Range r = p_motion->execute();
  m_motionCanChangeWholeVisualModeSelection = p_motion->canChangeWholeVisualModeSelection();

  // Jump over folding regions since we are just moving the cursor.
  int currLine = m_interface->cursorPosition().line();
  int delta = r.endLine - currLine;
  int vline = m_interface->lineToVisibleLine(currLine);
  r.endLine = m_interface->visibleLineToLine(qMax(vline + delta, 0));
  if (r.endLine >= m_interface->lines()) {
    r.endLine = m_interface->lines() - 1;
  }

  // make sure the position is valid before moving the cursor there
  if (r.valid && r.endLine >= 0 && (r.endLine == 0 || r.endLine <= m_interface->lines() - 1) &&
      r.endColumn >= 0) {
    const int lineLength = m_interface->lineLength(r.endLine);
    if (lineLength == 0) {
      r.endColumn = 0;
    } else if (r.endColumn >= lineLength) {
      r.endColumn = lineLength - 1;
    }

    goToPos(r);

    // in the case of VisualMode we need to remember the motion commands as
    // well.
    if (!m_viInputModeManager->isAnyVisualMode()) {
      m_viInputModeManager->clearCurrentChangeLog();
    }
  } else {
    qWarning() << "invalid position: (" << r.endLine << "," << r.endColumn << ")";
  }

  resetParser();

  // if normal mode was started by using Ctrl-O in insert mode,
  // it's time to go back to insert mode.
  if (m_viInputModeManager->getTemporaryNormalMode()) {
    startInsertMode();
    m_interface->update();
  }

  m_lastMotionWasVisualLineUpOrDown = m_currentMotionWasVisualLineUpOrDown;
}

void NormalViMode::executeMotionWithCommand(Motion *p_motion) {
  // Execute the specified command and supply the position returned from
  // the motion.
  m_commandRange = p_motion->execute();
  m_linewiseCommand = p_motion->isLineWise();

  // if we didn't get an explicit start position, use the current cursor
  // position
  if (m_commandRange.startLine == -1) {
    KateViI::Cursor c(m_interface->cursorPosition());
    m_commandRange.startLine = c.line();
    m_commandRange.startColumn = c.column();
  }

  // special case: When using the "w" motion in combination with an operator and
  // the last word moved over is at the end of a line, the end of that word
  // becomes the end of the operated text, not the first word in the next line.
  const auto motionPattern = p_motion->pattern();
  if (motionPattern == QLatin1String("w") || motionPattern == QLatin1String("W")) {
    if (m_commandRange.endLine != m_commandRange.startLine &&
        m_commandRange.endColumn == getFirstNonBlank(m_commandRange.endLine)) {
      m_commandRange.endLine--;
      m_commandRange.endColumn = m_interface->lineLength(m_commandRange.endLine);
    }
  }

  m_commandWithMotion = true;

  if (m_commandRange.valid) {
    executeCommand(m_commands.at(m_motionOperatorIndex));
  } else {
    qWarning() << "Invalid range: "
               << "from (" << m_commandRange.startLine << "," << m_commandRange.startColumn << ")"
               << "to (" << m_commandRange.endLine << "," << m_commandRange.endColumn << ")";
  }

  if (m_viInputModeManager->getCurrentViMode() == ViMode::NormalMode) {
    m_viInputModeManager->inputAdapter()->setCaretStyle(KateViI::Block);
  }

  m_commandWithMotion = false;
  reset();
}
