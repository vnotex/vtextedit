/*  This file is part of the KDE libraries and the Kate part.
 *
 *  Copyright (C) 2008 Erlend Hamberg <ehamberg@gmail.com>
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
#include <katevi/interface/kateviconfig.h>
#include <katevi/interface/katevieditorinterface.h>
#include <marks.h>
#include <modes/replacevimode.h>
#include <viutils.h>

using namespace KateVi;

ReplaceViMode::ReplaceViMode(InputModeManager *viInputModeManager,
                             KateViI::KateViEditorInterface *editorInterface)
    : ModeBase() {
  m_interface = editorInterface;
  m_viInputModeManager = viInputModeManager;
}

ReplaceViMode::~ReplaceViMode() { /* There's nothing to do here. */ }

bool ReplaceViMode::commandInsertFromLine(int offset) {
  KateViI::Cursor c(m_interface->cursorPosition());

  if (c.line() + offset >= m_interface->lines() || c.line() + offset < 0) {
    return false;
  }

  // Fetch the new character from the specified line.
  KateViI::Cursor target(c.line() + offset, c.column());
  QChar ch = m_interface->characterAt(target);
  if (ch == QChar::Null) {
    return false;
  }

  // The cursor is at the end of the line: just append the char.
  if (c.column() == m_interface->lineLength(c.line())) {
    return m_interface->insertText(c, ch);
  }

  // We can replace the current one with the fetched character.
  KateViI::Cursor next(c.line(), c.column() + 1);
  QChar removed = m_interface->line(c.line()).at(c.column());
  if (m_interface->replaceText(KateViI::Range(c, next), ch)) {
    overwrittenChar(removed);
    return true;
  }
  return false;
}

bool ReplaceViMode::commandMoveOneWordLeft() {
  KateViI::Cursor c(m_interface->cursorPosition());
  c = findPrevWordStart(c.line(), c.column());

  if (!c.isValid()) {
    c = KateViI::Cursor(0, 0);
  }

  updateCursor(c);
  return true;
}

bool ReplaceViMode::commandMoveOneWordRight() {
  KateViI::Cursor c(m_interface->cursorPosition());
  c = findNextWordStart(c.line(), c.column());

  if (!c.isValid()) {
    c = m_interface->documentEnd();
  }

  updateCursor(c);
  return true;
}

bool ReplaceViMode::handleKeyPress(const QKeyEvent *e) {
  // backspace should work even if the shift key is down
  if (!ViUtils::isControlModifier(e->modifiers()) && e->key() == Qt::Key_Backspace) {
    backspace();
    return true;
  }

  if (e->modifiers() == Qt::NoModifier) {
    switch (e->key()) {
    case Qt::Key_Escape:
      m_overwritten.clear();
      leaveReplaceMode();
      return true;

    // Just let Qt handle these keys.
    case Qt::Key_Left:
      Q_FALLTHROUGH();
    case Qt::Key_Right:
      Q_FALLTHROUGH();
    case Qt::Key_Up:
      Q_FALLTHROUGH();
    case Qt::Key_Down:
      Q_FALLTHROUGH();
    case Qt::Key_Home:
      Q_FALLTHROUGH();
    case Qt::Key_End:
      Q_FALLTHROUGH();
    case Qt::Key_PageUp:
      Q_FALLTHROUGH();
    case Qt::Key_PageDown:
      m_overwritten.clear();
      return false;

#if 0
        // Just let Qt handle these keys.
        case Qt::Key_Delete:
            m_view->keyDelete();
            return true;
#endif

    case Qt::Key_Insert:
      startInsertMode();
      return true;
    default:
      return false;
    }
  } else if (ViUtils::isControlModifier(e->modifiers())) {
    switch (e->key()) {
    case Qt::Key_BracketLeft:
    case Qt::Key_C:
      startNormalMode();
      return true;
    case Qt::Key_E:
      commandInsertFromLine(1);
      return true;
    case Qt::Key_Y:
      commandInsertFromLine(-1);
      return true;
    case Qt::Key_W:
      commandBackWord();
      return true;
    case Qt::Key_U:
      commandBackLine();
      return true;
    case Qt::Key_Left:
      m_overwritten.clear();
      commandMoveOneWordLeft();
      return true;
    case Qt::Key_Right:
      m_overwritten.clear();
      commandMoveOneWordRight();
      return true;
    default:
      return false;
    }
  }

  return false;
}

void ReplaceViMode::backspace() {
  KateViI::Cursor c1(m_interface->cursorPosition());
  KateViI::Cursor c2(c1.line(), c1.column() - 1);

  if (c1.column() > 0) {
    if (!m_overwritten.isEmpty()) {
      m_interface->removeText(KateViI::Range(c1.line(), c1.column() - 1, c1.line(), c1.column()));
      m_interface->insertText(c2, m_overwritten.right(1));
      m_overwritten.remove(m_overwritten.length() - 1, 1);
    }
    updateCursor(c2);
  }
}

void ReplaceViMode::commandBackWord() {
  KateViI::Cursor current(m_interface->cursorPosition());
  KateViI::Cursor to(findPrevWordStart(current.line(), current.column()));

  if (!to.isValid()) {
    return;
  }

  while (current.isValid() && current != to) {
    backspace();
    current = m_interface->cursorPosition();
  }
}

void ReplaceViMode::commandBackLine() {
  const int column = m_interface->cursorPosition().column();

  for (int i = column; i >= 0 && !m_overwritten.isEmpty(); i--) {
    backspace();
  }
}

void ReplaceViMode::leaveReplaceMode() {
  // Redo replacement operation <count> times
  m_interface->abortCompletion();

  if (m_count > 1) {
    // Look at added text so that we can repeat the addition
    const QString added = m_interface->getText(KateViI::Range(
        m_viInputModeManager->marks()->getStartEditYanked(), m_interface->cursorPosition()));
    for (unsigned int i = 0; i < m_count - 1; i++) {
      KateViI::Cursor c(m_interface->cursorPosition());
      KateViI::Cursor c2(c.line(), c.column() + added.length());
      m_interface->replaceText(KateViI::Range(c, c2), added);
    }
  }

  startNormalMode();
}
