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

#ifndef KATEVI_MODE_BASE_H
#define KATEVI_MODE_BASE_H

#include <QObject>

#include <katevi/interface/range.h>
#include <katevi/katevi_export.h>

#include <katevi/definitions.h>
#include <range.h>

class QKeyEvent;
class QString;
class QRegExp;

namespace KateViI {
class KateViEditorInterface;
}

namespace KateVi {
class InputModeManager;

enum Direction { Up, Down, Left, Right, Next, Prev };

class KATEVI_EXPORT ModeBase : public QObject {
  Q_OBJECT

public:
  ModeBase() : QObject() {}

  virtual ~ModeBase() {}

  /**
   * @return normal mode command accumulated so far
   */
  QString getVerbatimKeys() const;
  virtual bool handleKeyPress(const QKeyEvent *e) = 0;

  void setCount(unsigned int count) { m_count = count; }

  void setRegister(QChar reg) { m_register = reg; }

  void error(const QString &errorMsg);
  void message(const QString &msg);

  Range motionFindNext();
  Range motionFindPrev();

  // Only use endline and endcolumn of the range.
  virtual void goToPos(const Range &r);

  // Get the repeat count of a command.
  int getCount() const;

  bool startInsertMode();

protected:
  // helper methods
  // Copy @text to system clipboard if @chosen_register is not 0 or -.
  void yankToClipBoard(QChar chosen_register, QString text);

  bool deleteRange(Range &r, OperationMode mode = LineWise, bool addToRegister = true);

  const QString getRange(Range &r, OperationMode mode = LineWise) const;

  const QString getLine(int line = -1) const;

  const QChar getCharUnderCursor() const;

  const QString getWordUnderCursor() const;
  const KateViI::Range getWordRangeUnderCursor() const;
  KateViI::Cursor findNextWordStart(int fromLine, int fromColumn,
                                    bool onlyCurrentLine = false) const;
  KateViI::Cursor findNextWORDStart(int fromLine, int fromColumn,
                                    bool onlyCurrentLine = false) const;
  KateViI::Cursor findPrevWordStart(int fromLine, int fromColumn,
                                    bool onlyCurrentLine = false) const;
  KateViI::Cursor findPrevWORDStart(int fromLine, int fromColumn,
                                    bool onlyCurrentLine = false) const;
  KateViI::Cursor findPrevWordEnd(int fromLine, int fromColumn, bool onlyCurrentLine = false) const;
  KateViI::Cursor findPrevWORDEnd(int fromLine, int fromColumn, bool onlyCurrentLine = false) const;

  KateViI::Cursor findWordEnd(int fromLine, int fromColumn, bool onlyCurrentLine = false) const;

  KateViI::Cursor findWORDEnd(int fromLine, int fromColumn, bool onlyCurrentLine = false) const;

  Range findSurroundingBrackets(const QChar &c1, const QChar &c2, bool inner, const QChar &nested1,
                                const QChar &nested2) const;

  Range findSurrounding(const QRegExp &c1, const QRegExp &c2, bool inner = false) const;
  Range findSurroundingQuotes(const QChar &c, bool inner = false) const;

  int findLineStartingWitchChar(const QChar &c, int count, bool forward = true) const;
  void updateCursor(const KateViI::Cursor &c) const;
  const QChar getCharAtVirtualColumn(const QString &line, int virtualColumn, int tabWidht) const;

  void addToNumberUnderCursor(int count);

  Range goLineUp();
  Range goLineDown();
  Range goLineUpDown(int lines);
  Range goVisualLineUpDown(int lines);

  unsigned int linesDisplayed() const;

  bool isCounted() const { return m_iscounted; }

  bool startNormalMode();
  bool startVisualMode();
  bool startVisualLineMode();
  bool startVisualBlockMode();
  bool startReplaceMode();

  QChar getChosenRegister(const QChar &defaultReg) const;
  QString getRegisterContent(const QChar &reg);
  OperationMode getRegisterFlag(const QChar &reg) const;
  void fillRegister(const QChar &reg, const QString &text, OperationMode flag = CharWise);

  void switchView(Direction direction = Next);

  void addJump(KateViI::Cursor cursor);
  KateViI::Cursor getNextJump(KateViI::Cursor) const;
  KateViI::Cursor getPrevJump(KateViI::Cursor) const;

  void appendToKeysVerbatim(const QString &p_keys);
  void clearKeysVerbatim();

protected:
  // Register assigned to use.
  QChar m_register;

  // The range given by a motion to execute a command.
  Range m_commandRange;

  // Repeat count of a command.
  unsigned int m_count = 0;

  int m_oneTimeCountOverride = -1;

  bool m_iscounted = false;

  // Characters that would be treated as a word start.
  QString m_extraWordCharacters;

  int m_stickyColumn = -1;

  bool m_lastMotionWasVisualLineUpOrDown = false;

  bool m_currentMotionWasVisualLineUpOrDown = false;

  KateViI::KateViEditorInterface *m_interface = nullptr;

  InputModeManager *m_viInputModeManager = nullptr;

private:
  // Used to display pending key strokes.
  QString m_keysVerbatim;
};

} // namespace KateVi

#endif
