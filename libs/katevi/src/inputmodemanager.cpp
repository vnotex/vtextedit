/*  This file is part of the KDE libraries and the Kate part.
 *
 *  Copyright (C) 2008 - 2009 Erlend Hamberg <ehamberg@gmail.com>
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

#include <katevi/inputmodemanager.h>

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QString>
#include <QWidget>

#include "modes/insertvimode.h"
#include "modes/normalvimode.h"
#include "modes/replacevimode.h"
#include "modes/visualvimode.h"
#include <katevi/global.h>
#include <katevi/globalstate.h>
#include <katevi/interface/kateviinputmode.h>

#include "completionrecorder.h"
#include "completionreplayer.h"
#include "jumps.h"
#include "keymapper.h"
#include "keyparser.h"
#include "lastchangerecorder.h"
#include "macrorecorder.h"
#include "macros.h"
#include "marks.h"
#include "registers.h"
#include "viutils.h"
#include <katevi/emulatedcommandbar.h>
// #include "searcher.h"

using namespace KateVi;

InputModeManager::InputModeManager(KateViI::KateViInputMode *inputAdapter,
                                   KateViI::KateViEditorInterface *editorInterface)
    : m_inputAdapter(inputAdapter), m_interface(editorInterface) {
  m_currentViMode = ViMode::NormalMode;
  m_previousViMode = ViMode::NormalMode;

  m_viNormalMode.reset(new NormalViMode(this, m_interface));
  m_viVisualMode.reset(new VisualViMode(this, m_interface));
  m_viInsertMode.reset(new InsertViMode(this, m_interface));
  m_viReplaceMode.reset(new ReplaceViMode(this, m_interface));

  m_keyMapperStack.push(QSharedPointer<KeyMapper>(new KeyMapper(this, m_interface)));

  m_temporaryNormalMode = false;

  m_jumps.reset(new Jumps());

  m_marks.reset(new Marks(this));

  // m_searcher = new Searcher(this);

  m_completionRecorder.reset(new CompletionRecorder(this));
  m_completionReplayer.reset(new CompletionReplayer(this));

  m_macroRecorder.reset(new MacroRecorder(this));

  m_lastChangeRecorder.reset(new LastChangeRecorder(this));

  // We have to do this outside of NormalMode, as we don't want
  // VisualMode (which inherits from NormalMode) to respond
  // to changes in the document as well.
  m_viNormalMode->beginMonitoringDocumentChanges();

  m_interface->connectMouseReleased([this](QMouseEvent *p_event) {
    if (!m_inputAdapter->isActive() || getCurrentViMode() != ViMode::NormalMode) {
      return;
    }

    if (p_event->button() == Qt::LeftButton && !m_interface->selection()) {
      amendCursorPosition();
    }
  });
}

InputModeManager::~InputModeManager() {
  /*
  delete m_searcher;
  */
}

bool InputModeManager::handleKeyPress(const QKeyEvent *e) {
  m_insideHandlingKeyPressCount++;
  bool res = false;
  bool keyIsPartOfMapping = false;

  const bool isSyntheticSearchCompletedKeyPress =
      m_inputAdapter->viModeEmulatedCommandBar()->isSendingSyntheticSearchCompletedKeypress();

  // With macros, we want to record the keypresses *before* they are mapped, but
  // if they end up *not* being part of a mapping, we don't want to record them
  // when they are played back by m_keyMapper, hence the
  // "!isPlayingBackRejectedKeys()". And obviously, since we're recording keys
  // before they are mapped, we don't want to also record the executed mapping,
  // as when we replayed the macro, we'd get duplication!
  if (m_macroRecorder->isRecording() && !m_macroRecorder->isReplaying() &&
      !isSyntheticSearchCompletedKeyPress && !keyMapper()->isExecutingMapping() &&
      !keyMapper()->isPlayingBackRejectedKeys() && !lastChangeRecorder()->isReplaying()) {
    m_macroRecorder->record(*e);
  }

  const bool isReplaying = m_lastChangeRecorder->isReplaying();
  if (!isReplaying && !isSyntheticSearchCompletedKeyPress) {
    // On Windows, Ctrl+Alt.
    if (e->key() == Qt::Key_AltGr) {
      // Do nothing.
      return true;
    }

    // Hand off to the key mapper, and decide if this key is part of a mapping.
    if (!ViUtils::isModifier(e->key())) {
      const QChar key = KeyParser::self()->KeyEventToQChar(*e);
      if (keyMapper()->handleKeyPress(key)) {
        keyIsPartOfMapping = true;
        res = true;
      }
    }
  }

  if (!keyIsPartOfMapping) {
    if (!isReplaying && !isSyntheticSearchCompletedKeyPress) {
      // Record key press so that it can be repeated via "."
      m_lastChangeRecorder->record(*e);
    }

    if (m_inputAdapter->viModeEmulatedCommandBar()->isActive()) {
      res = m_inputAdapter->viModeEmulatedCommandBar()->handleKeyPress(e);
    } else {
      res = getCurrentViModeHandler()->handleKeyPress(e);
    }
  }

  m_insideHandlingKeyPressCount--;
  Q_ASSERT(m_insideHandlingKeyPressCount >= 0);

  return res;
}

void InputModeManager::feedKeyPresses(const QString &keyPresses) const {
  int key;
  Qt::KeyboardModifiers mods;
  QString text;

  for (const QChar &keyc : keyPresses) {
    QString decoded = KeyParser::self()->decodeKeySequence(QString(keyc));
    key = -1;
    mods = Qt::NoModifier;
    text.clear();

    if (decoded.length() > 1) { // special key

      // remove the angle brackets
      decoded.remove(0, 1);
      decoded.remove(decoded.indexOf(QLatin1Char('>')), 1);

      // check if one or more modifier keys where used
      if (decoded.indexOf(QLatin1String("s-")) != -1 ||
          decoded.indexOf(QLatin1String("c-")) != -1 ||
          decoded.indexOf(QLatin1String("m-")) != -1 ||
          decoded.indexOf(QLatin1String("a-")) != -1) {
        int s = decoded.indexOf(QLatin1String("s-"));
        if (s != -1) {
          mods |= Qt::ShiftModifier;
          decoded.remove(s, 2);
        }

        int c = decoded.indexOf(QLatin1String("c-"));
        if (c != -1) {
          mods |= Qt::ControlModifier;
          decoded.remove(c, 2);
        }

        int a = decoded.indexOf(QLatin1String("a-"));
        if (a != -1) {
          mods |= Qt::AltModifier;
          decoded.remove(a, 2);
        }

        int m = decoded.indexOf(QLatin1String("m-"));
        if (m != -1) {
          mods |= Qt::MetaModifier;
          decoded.remove(m, 2);
        }

        if (decoded.length() > 1) {
          key = KeyParser::self()->vi2qt(decoded);
        } else if (decoded.length() == 1) {
          key = int(decoded.at(0).toUpper().toLatin1());
          text = decoded.at(0);
        }
      } else { // no modifiers
        key = KeyParser::self()->vi2qt(decoded);
      }
    } else {
      key = decoded.at(0).unicode();
      text = decoded.at(0);
    }

    if (key == -1)
      continue;

    // We have to be clever about which widget we dispatch to, as we can trigger
    // shortcuts if we're not careful (even if Vim mode is configured to steal
    // shortcuts).
    QKeyEvent k(QEvent::KeyPress, key, mods, text);
    QWidget *destWidget = nullptr;
    if (QApplication::activePopupWidget()) {
      // According to the docs, the activePopupWidget, if present, takes all
      // events.
      destWidget = QApplication::activePopupWidget();
    } else if (QApplication::focusWidget()) {
      if (QApplication::focusWidget()->focusProxy()) {
        destWidget = QApplication::focusWidget()->focusProxy();
      } else {
        destWidget = QApplication::focusWidget();
      }
    } else {
      destWidget = m_interface->focusProxy();
    }
    QApplication::sendEvent(destWidget, &k);
  }
}

bool InputModeManager::isHandlingKeyPress() const { return m_insideHandlingKeyPressCount > 0; }

void InputModeManager::storeLastChangeCommand() {
  m_lastChange = m_lastChangeRecorder->encodedChanges();
  m_lastChangeCompletionsLog = m_completionRecorder->currentChangeCompletionsLog();
}

void InputModeManager::repeatLastChange() {
  m_lastChangeRecorder->replay(m_lastChange, m_lastChangeCompletionsLog);
}

void InputModeManager::clearCurrentChangeLog() {
  m_lastChangeRecorder->clear();
  m_completionRecorder->clearCurrentChangeCompletionsLog();
}

void InputModeManager::doNotLogCurrentKeypress() {
  m_macroRecorder->dropLast();
  m_lastChangeRecorder->dropLast();
}

void InputModeManager::changeViMode(ViMode newMode) {
  m_previousViMode = m_currentViMode;
  m_currentViMode = newMode;
}

ViMode InputModeManager::getCurrentViMode() const { return m_currentViMode; }

KateViI::ViewMode InputModeManager::getCurrentViewMode() const {
  switch (m_currentViMode) {
  case ViMode::InsertMode:
    return KateViI::ViModeInsert;
  case ViMode::VisualMode:
    return KateViI::ViModeVisual;
  case ViMode::VisualLineMode:
    return KateViI::ViModeVisualLine;
  case ViMode::VisualBlockMode:
    return KateViI::ViModeVisualBlock;
  case ViMode::ReplaceMode:
    return KateViI::ViModeReplace;
  case ViMode::NormalMode:
  default:
    return KateViI::ViModeNormal;
  }
}

ViMode InputModeManager::getPreviousViMode() const { return m_previousViMode; }

bool InputModeManager::isAnyVisualMode() const {
  return ((m_currentViMode == ViMode::VisualMode) || (m_currentViMode == ViMode::VisualLineMode) ||
          (m_currentViMode == ViMode::VisualBlockMode));
}

::ModeBase *InputModeManager::getCurrentViModeHandler() const {
  switch (m_currentViMode) {
  case ViMode::NormalMode:
    return m_viNormalMode.data();

  case ViMode::VisualMode:
  case ViMode::VisualLineMode:
  case ViMode::VisualBlockMode:
    return m_viVisualMode.data();

  case ViMode::InsertMode:
    return m_viInsertMode.data();

  case ViMode::ReplaceMode:
    return m_viReplaceMode.data();

  default:
    return nullptr;
  }
}

void InputModeManager::viEnterNormalMode() {
  bool moveCursorLeft =
      (m_currentViMode == ViMode::InsertMode || m_currentViMode == ViMode::ReplaceMode) &&
      m_interface->cursorPosition().column() > 0;

  if (!m_lastChangeRecorder->isReplaying() &&
      (m_currentViMode == ViMode::InsertMode || m_currentViMode == ViMode::ReplaceMode)) {
    // '^ is the insert mark and "^ is the insert register,
    // which holds the last inserted text
    KateViI::Range r(m_interface->cursorPosition(), m_marks->getInsertStopped());

    if (r.isValid()) {
      QString insertedText = m_interface->getText(r);
      m_inputAdapter->globalState()->registers()->setInsertStopped(insertedText);
    }

    m_marks->setInsertStopped(KateViI::Cursor(m_interface->cursorPosition()));
  }

  changeViMode(ViMode::NormalMode);

  if (moveCursorLeft) {
    m_interface->cursorPrevChar();
  }
  m_inputAdapter->setCaretStyle(KateViI::CaretStyle::Block);
  m_inputAdapter->setCursorBlinkingEnabled(false);
  m_interface->update();
  m_interface->setOverwriteMode(false);
}

void InputModeManager::viEnterInsertMode() {
  changeViMode(ViMode::InsertMode);
  m_marks->setInsertStopped(KateViI::Cursor(m_interface->cursorPosition()));
  if (getTemporaryNormalMode()) {
    // Ensure the key log contains a request to re-enter Insert mode, else the
    // keystrokes made after returning from temporary normal mode will be
    // treated as commands!
    m_lastChangeRecorder->record(
        QKeyEvent(QEvent::KeyPress, Qt::Key_I, Qt::NoModifier, QStringLiteral("i")));
  }

  m_inputAdapter->setCaretStyle(KateViI::CaretStyle::Line);
  m_inputAdapter->setCursorBlinkingEnabled(true);
  setTemporaryNormalMode(false);
  m_interface->update();
  m_interface->setOverwriteMode(false);
}

void InputModeManager::viEnterVisualMode(ViMode mode) {
  changeViMode(mode);

  m_inputAdapter->setCaretStyle(KateViI::CaretStyle::Block);
  m_inputAdapter->setCursorBlinkingEnabled(false);
  m_interface->update();
  m_interface->setOverwriteMode(false);

  getViVisualMode()->setVisualModeType(mode);
  getViVisualMode()->init();
}

void InputModeManager::viEnterReplaceMode() {
  // Different from Vi: we just simply enter the overwrite mode.
  changeViMode(ViMode::ReplaceMode);
  m_marks->setStartEditYanked(KateViI::Cursor(m_interface->cursorPosition()));
  m_inputAdapter->setCaretStyle(KateViI::CaretStyle::Half);
  m_inputAdapter->setCursorBlinkingEnabled(true);
  m_interface->update();
  m_interface->setOverwriteMode(true);
}

NormalViMode *InputModeManager::getViNormalMode() { return m_viNormalMode.data(); }

InsertViMode *InputModeManager::getViInsertMode() { return m_viInsertMode.data(); }

VisualViMode *InputModeManager::getViVisualMode() { return m_viVisualMode.data(); }

ReplaceViMode *InputModeManager::getViReplaceMode() { return m_viReplaceMode.data(); }

#if 0
void InputModeManager::readSessionConfig()
{
    /*
    m_jumps->readSessionConfig(config);
    m_marks->readSessionConfig(config);
    */
}

void InputModeManager::writeSessionConfig()
{
    /*
    m_jumps->writeSessionConfig(config);
    m_marks->writeSessionConfig(config);
    */
}
#endif

void InputModeManager::reset() {
  if (m_viVisualMode) {
    m_viVisualMode->reset();
  }
}

KeyMapper *InputModeManager::keyMapper() { return m_keyMapperStack.top().data(); }

void InputModeManager::pushKeyMapper(QSharedPointer<KeyMapper> mapper) {
  m_keyMapperStack.push(mapper);
}

void InputModeManager::popKeyMapper() { m_keyMapperStack.pop(); }

const QString InputModeManager::getVerbatimKeys() const {
  QString cmd;

  switch (getCurrentViMode()) {
  case ViMode::NormalMode:
    cmd = m_viNormalMode->getVerbatimKeys();
    break;

  case ViMode::InsertMode:
  case ViMode::ReplaceMode:
    // ...
    break;

  case ViMode::VisualMode:
  case ViMode::VisualLineMode:
  case ViMode::VisualBlockMode:
    cmd = m_viVisualMode->getVerbatimKeys();
    break;

  default:
    break;
  }

  return cmd;
}

void InputModeManager::updateCursor(const KateViI::Cursor &c) { m_inputAdapter->updateCursor(c); }

GlobalState *InputModeManager::globalState() const { return m_inputAdapter->globalState(); }

KateViI::KateViConfig *InputModeManager::kateViConfig() const {
  return m_inputAdapter->kateViConfig();
}

KateViI::KateViEditorInterface *InputModeManager::editorInterface() const { return m_interface; }

bool InputModeManager::isRecording() const { return m_macroRecorder->isRecording(); }

void InputModeManager::amendCursorPosition() {
  KateViI::Cursor c(m_interface->cursorPosition());
  int lineLength = m_interface->lineLength(c.line());
  if (c.column() >= lineLength) {
    if (lineLength == 0) {
      c.setColumn(0);
    } else {
      c.setColumn(lineLength - 1);
    }
    updateCursor(c);
  }
}

void InputModeManager::startInsertMode() { getCurrentViModeHandler()->startInsertMode(); }
