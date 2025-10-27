/*  This file is part of the KDE libraries and the Kate part.
 *
 *  Copyright (C) 2008 - 2012 Erlend Hamberg <ehamberg@gmail.com>
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

#include "visualvimode.h"
#include <jumps.h>
#include <katevi/global.h>
#include <katevi/globalstate.h>
#include <katevi/inputmodemanager.h>
#include <katevi/interface/kateviconfig.h>
#include <katevi/interface/katevieditorinterface.h>
#include <katevi/interface/kateviinputmode.h>
#include <lastchangerecorder.h>
#include <modes/modebase.h>
#include <range.h>
#include <registers.h>
#include <searcher.h>

#include <QRegExp>
#include <QString>

using namespace KateVi;

// TODO: the "previous word/WORD [end]" methods should be optimized. now they're
// being called in a loop and all calculations done up to finding a match are
// trown away when called with a count > 1 because they will simply be called
// again from the last found position. They should take the count as a parameter
// and collect the positions in a QList, then return element (count - 1)

////////////////////////////////////////////////////////////////////////////////
// HELPER METHODS
////////////////////////////////////////////////////////////////////////////////

void ModeBase::yankToClipBoard(QChar chosen_register, QString text) {
  // For VNote: do not yank to system clipboard by default.
  Q_UNUSED(chosen_register);
  Q_UNUSED(text);
#if 0
    if ((chosen_register == QLatin1Char('0') || chosen_register == QLatin1Char('-'))
        && text.length() > 1 && !text.trimmed().isEmpty())
    {
        m_interface->copyToClipboard(text);
    }
#endif
}

bool ModeBase::deleteRange(Range &r, OperationMode mode, bool addToRegister) {
  Q_ASSERT(r.endColumn != KateVi::EOL);
  r.normalize();
  bool res = false;
  QString removedText = getRange(r, mode);

  if (mode == LineWise) {
    m_interface->editStart();
    for (int i = 0; i < r.endLine - r.startLine + 1; i++) {
      res = m_interface->removeLine(r.startLine);
    }
    m_interface->editEnd();
  } else {
    res = m_interface->removeText(r.toEditorRange(), mode == Block);
  }

  QChar chosenRegister = getChosenRegister(ZeroRegister);
  if (addToRegister) {
    if (r.startLine == r.endLine) {
      chosenRegister = getChosenRegister(SmallDeleteRegister);
      fillRegister(chosenRegister, removedText, mode);
    } else {
      fillRegister(chosenRegister, removedText, mode);
    }
  }
  yankToClipBoard(chosenRegister, removedText);

  return res;
}

const QString ModeBase::getRange(Range &r, OperationMode mode) const {
  r.normalize();
  QString s;

  if (mode == LineWise) {
    r.startColumn = 0;
    r.endColumn = getLine(r.endLine).length();
  }

  if (r.motionType == InclusiveMotion) {
    r.endColumn++;
  }

  KateViI::Range range = r.toEditorRange();
  if (mode == LineWise) {
    s = m_interface->textLines(range).join(QLatin1Char('\n'));
    s.append(QLatin1Char('\n'));
  } else if (mode == Block) {
    s = m_interface->getText(range, true);
  } else {
    s = m_interface->getText(range);
  }

  return s;
}

const QString ModeBase::getLine(int line) const {
  return (line < 0) ? m_interface->currentTextLine() : m_interface->line(line);
}

const QChar ModeBase::getCharUnderCursor() const {
  KateViI::Cursor c(m_interface->cursorPosition());

  QString line = getLine(c.line());
  if (line.length() == 0 && c.column() >= line.length()) {
    return QChar::Null;
  }

  return line.at(c.column());
}

const QString ModeBase::getWordUnderCursor() const {
  /*
  return doc()->text(getWordRangeUnderCursor());
  */
  return "";
}

const KateViI::Range ModeBase::getWordRangeUnderCursor() const {
  /*
  KTextEditor::Cursor c(m_view->cursorPosition());

  // find first character that is a "word letter" and start the search there
  QChar ch = doc()->characterAt(c);
  int i = 0;
  while (!ch.isLetterOrNumber() && ! ch.isMark() && ch != QLatin1Char('_')
          && m_extraWordCharacters.indexOf(ch) == -1) {

      // advance cursor one position
      c.setColumn(c.column() + 1);
      if (c.column() > doc()->lineLength(c.line())) {
          c.setColumn(0);
          c.setLine(c.line() + 1);
          if (c.line() == doc()->lines()) {
              return KTextEditor::Range::invalid();
          }
      }

      ch = doc()->characterAt(c);
      i++; // count characters that were advanced so we know where to start the
  search
  }

  // move cursor the word (if cursor was placed on e.g. a paren, this will move
  // it to the right
  updateCursor(c);

  KTextEditor::Cursor c1 = findPrevWordStart(c.line(), c.column() + 1 + i,
  true); KTextEditor::Cursor c2 = findWordEnd(c1.line(), c1.column() + i - 1,
  true); c2.setColumn(c2.column() + 1);

  return KTextEditor::Range(c1, c2);
  */
  return KateViI::Range::invalid();
}

KateViI::Cursor ModeBase::findNextWordStart(int fromLine, int fromColumn,
                                            bool onlyCurrentLine) const {
  QString line = getLine(fromLine);

  // The start of word pattern need to take m_extraWordCharacters into account
  // if defined
  QString startOfWordPattern = QStringLiteral("\\b(\\w");
  if (m_extraWordCharacters.length() > 0) {
    startOfWordPattern.append(QLatin1String("|[") + m_extraWordCharacters + QLatin1Char(']'));
  }
  startOfWordPattern.append(QLatin1Char(')'));

  QRegExp startOfWord(startOfWordPattern);                  // start of a word
  QRegExp nonSpaceAfterSpace(QLatin1String("\\s\\S"));      // non-space right after space
  QRegExp nonWordAfterWord(QLatin1String("\\b(?!\\s)\\W")); // word-boundary followed by a non-word
                                                            // which is not a space

  int l = fromLine;
  int c = fromColumn;

  bool found = false;
  while (!found) {
    int c1 = startOfWord.indexIn(line, c + 1);
    int c2 = nonSpaceAfterSpace.indexIn(line, c);
    int c3 = nonWordAfterWord.indexIn(line, c + 1);

    if (c1 == -1 && c2 == -1 && c3 == -1) {
      if (onlyCurrentLine) {
        return KateViI::Cursor::invalid();
      } else if (l >= m_interface->lines() - 1) {
        c = qMax(line.length() - 1, 0);
        return KateViI::Cursor::invalid();
      } else {
        c = 0;
        l++;

        line = getLine(l);

        if (line.length() == 0 || !line.at(c).isSpace()) {
          found = true;
        }

        continue;
      }
    }

    c2++; // the second regexp will match one character *before* the character
          // we want to go to

    if (c1 <= 0) {
      c1 = line.length() - 1;
    }
    if (c2 <= 0) {
      c2 = line.length() - 1;
    }
    if (c3 <= 0) {
      c3 = line.length() - 1;
    }

    c = qMin(c1, qMin(c2, c3));

    found = true;
  }

  return KateViI::Cursor(l, c);
}

KateViI::Cursor ModeBase::findNextWORDStart(int fromLine, int fromColumn,
                                            bool onlyCurrentLine) const {
  QString line = getLine();

  int l = fromLine;
  int c = fromColumn;

  bool found = false;
  QRegExp startOfWORD(QLatin1String("\\s\\S"));

  while (!found) {
    c = startOfWORD.indexIn(line, c);

    if (c == -1) {
      if (onlyCurrentLine) {
        return KateViI::Cursor(l, c);
      } else if (l >= m_interface->lines() - 1) {
        c = line.length() - 1;
        break;
      } else {
        c = 0;
        l++;

        line = getLine(l);

        if (line.length() == 0 || !line.at(c).isSpace()) {
          found = true;
        }

        continue;
      }
    } else {
      c++;
      found = true;
    }
  }

  return KateViI::Cursor(l, c);
}

KateViI::Cursor ModeBase::findPrevWordEnd(int fromLine, int fromColumn,
                                          bool onlyCurrentLine) const {
  QString line = getLine(fromLine);

  QString endOfWordPattern = QStringLiteral("\\S\\s|\\S$|\\w\\W|\\S\\b|^$");

  if (m_extraWordCharacters.length() > 0) {
    endOfWordPattern.append(QLatin1String("|[") + m_extraWordCharacters + QLatin1String("][^") +
                            m_extraWordCharacters + QLatin1Char(']'));
  }

  QRegExp endOfWord(endOfWordPattern);

  int l = fromLine;
  int c = fromColumn;

  bool found = false;

  while (!found) {
    int c1 = endOfWord.lastIndexIn(line, c - 1);

    if (c1 != -1 && c - 1 != -1) {
      found = true;
      c = c1;
    } else {
      if (onlyCurrentLine) {
        return KateViI::Cursor::invalid();
      } else if (l > 0) {
        line = getLine(--l);
        c = line.length();

        continue;
      } else {
        return KateViI::Cursor::invalid();
      }
    }
  }

  return KateViI::Cursor(l, c);
}

KateViI::Cursor ModeBase::findPrevWORDEnd(int fromLine, int fromColumn,
                                          bool onlyCurrentLine) const {
  QString line = getLine(fromLine);

  QRegExp endOfWORDPattern(QLatin1String("\\S\\s|\\S$|^$"));

  QRegExp endOfWORD(endOfWORDPattern);

  int l = fromLine;
  int c = fromColumn;

  bool found = false;

  while (!found) {
    int c1 = endOfWORD.lastIndexIn(line, c - 1);

    if (c1 != -1 && c - 1 != -1) {
      found = true;
      c = c1;
    } else {
      if (onlyCurrentLine) {
        return KateViI::Cursor::invalid();
      } else if (l > 0) {
        line = getLine(--l);
        c = line.length();

        continue;
      } else {
        c = 0;
        return KateViI::Cursor::invalid();
      }
    }
  }

  return KateViI::Cursor(l, c);
}

KateViI::Cursor ModeBase::findPrevWordStart(int fromLine, int fromColumn,
                                            bool onlyCurrentLine) const {
  QString line = getLine(fromLine);

  // the start of word pattern need to take m_extraWordCharacters into account
  // if defined
  QString startOfWordPattern = QStringLiteral("\\b(\\w");
  if (m_extraWordCharacters.length() > 0) {
    startOfWordPattern.append(QLatin1String("|[") + m_extraWordCharacters + QLatin1Char(']'));
  }
  startOfWordPattern.append(QLatin1Char(')'));

  QRegExp startOfWord(startOfWordPattern);                  // start of a word
  QRegExp nonSpaceAfterSpace(QLatin1String("\\s\\S"));      // non-space right after space
  QRegExp nonWordAfterWord(QLatin1String("\\b(?!\\s)\\W")); // word-boundary followed by a non-word
                                                            // which is not a space
  QRegExp startOfLine(QLatin1String("^\\S"));               // non-space at start of line

  int l = fromLine;
  int c = fromColumn;

  bool found = false;

  while (!found) {
    int c1 = startOfWord.lastIndexIn(line, -line.length() + c - 1);
    int c2 = nonSpaceAfterSpace.lastIndexIn(line, -line.length() + c - 2);
    int c3 = nonWordAfterWord.lastIndexIn(line, -line.length() + c - 1);
    int c4 = startOfLine.lastIndexIn(line, -line.length() + c - 1);

    if (c1 == -1 && c2 == -1 && c3 == -1 && c4 == -1) {
      if (onlyCurrentLine) {
        return KateViI::Cursor::invalid();
      } else if (l <= 0) {
        return KateViI::Cursor::invalid();
      } else {
        line = getLine(--l);
        c = line.length();

        if (line.length() == 0) {
          c = 0;
          found = true;
        }

        continue;
      }
    }

    c2++; // the second regexp will match one character *before* the character
          // we want to go to

    if (c1 <= 0) {
      c1 = 0;
    }
    if (c2 <= 0) {
      c2 = 0;
    }
    if (c3 <= 0) {
      c3 = 0;
    }
    if (c4 <= 0) {
      c4 = 0;
    }

    c = qMax(c1, qMax(c2, qMax(c3, c4)));

    found = true;
  }

  return KateViI::Cursor(l, c);
}

KateViI::Cursor ModeBase::findPrevWORDStart(int fromLine, int fromColumn,
                                            bool onlyCurrentLine) const {
  QString line = getLine(fromLine);

  QRegExp startOfWORD(QLatin1String("\\s\\S"));
  QRegExp startOfLineWORD(QLatin1String("^\\S"));

  int l = fromLine;
  int c = fromColumn;

  bool found = false;

  while (!found) {
    int c1 = startOfWORD.lastIndexIn(line, -line.length() + c - 2);
    int c2 = startOfLineWORD.lastIndexIn(line, -line.length() + c - 1);

    if (c1 == -1 && c2 == -1) {
      if (onlyCurrentLine) {
        return KateViI::Cursor::invalid();
      } else if (l <= 0) {
        return KateViI::Cursor::invalid();
      } else {
        line = getLine(--l);
        c = line.length();

        if (line.length() == 0) {
          c = 0;
          found = true;
        }

        continue;
      }
    }

    c1++; // the startOfWORD pattern matches one character before the word

    c = qMax(c1, c2);

    if (c <= 0) {
      c = 0;
    }

    found = true;
  }

  return KateViI::Cursor(l, c);
}

KateViI::Cursor ModeBase::findWordEnd(int fromLine, int fromColumn, bool onlyCurrentLine) const {
  QString line = getLine(fromLine);

  QString endOfWordPattern = QStringLiteral("\\S\\s|\\S$|\\w\\W|\\S\\b");

  if (m_extraWordCharacters.length() > 0) {
    endOfWordPattern.append(QLatin1String("|[") + m_extraWordCharacters + QLatin1String("][^") +
                            m_extraWordCharacters + QLatin1Char(']'));
  }

  QRegExp endOfWORD(endOfWordPattern);

  int l = fromLine;
  int c = fromColumn;

  bool found = false;

  while (!found) {
    int c1 = endOfWORD.indexIn(line, c + 1);

    if (c1 != -1) {
      found = true;
      c = c1;
    } else {
      if (onlyCurrentLine) {
        return KateViI::Cursor::invalid();
      } else if (l >= m_interface->lines() - 1) {
        c = line.length() - 1;
        return KateViI::Cursor::invalid();
      } else {
        c = -1;
        line = getLine(++l);

        continue;
      }
    }
  }

  return KateViI::Cursor(l, c);
}

KateViI::Cursor ModeBase::findWORDEnd(int fromLine, int fromColumn, bool onlyCurrentLine) const {
  QString line = getLine(fromLine);

  QRegExp endOfWORD(QLatin1String("\\S\\s|\\S$"));

  int l = fromLine;
  int c = fromColumn;

  bool found = false;

  while (!found) {
    int c1 = endOfWORD.indexIn(line, c + 1);

    if (c1 != -1) {
      found = true;
      c = c1;
    } else {
      if (onlyCurrentLine) {
        return KateViI::Cursor::invalid();
      } else if (l >= m_interface->lines() - 1) {
        c = line.length() - 1;
        return KateViI::Cursor::invalid();
      } else {
        c = -1;
        line = getLine(++l);

        continue;
      }
    }
  }

  return KateViI::Cursor(l, c);
}

Range innerRange(Range range, bool inner) {
  Range r = range;

  if (inner) {
    const int columnDistance = qAbs(r.startColumn - r.endColumn);
    if ((r.startLine == r.endLine) && columnDistance == 1) {
      // Start and end are right next to each other; there is nothing inside
      // them.
      return Range::invalid();
    }
    r.startColumn++;
    r.endColumn--;
  }

  return r;
}

// Constrained within one line.
Range ModeBase::findSurroundingQuotes(const QChar &c, bool inner) const {
  KateViI::Cursor cursor(m_interface->cursorPosition());
  Range r;
  r.startLine = cursor.line();
  r.endLine = cursor.line();

  QString line = m_interface->line(cursor.line());

  // If cursor on the quote we should choose the best direction.
  if (line.at(cursor.column()) == c) {
    //  If at the beginning of the line - then we might search the end.
    if (cursor.column() == 0 && line.size() > 1) {
      r.startColumn = cursor.column();
      r.endColumn = line.indexOf(c, cursor.column() + 1);

      return innerRange(r, inner);
    }

    //  If at the end of the line - then we might search the beginning.
    if (cursor.column() > 0 && cursor.column() == line.size() - 1) {
      r.startColumn = line.lastIndexOf(c, cursor.column() - 1);
      r.endColumn = cursor.column();

      return innerRange(r, inner);
    }

    // Try to search the quote to right
    int c1 = line.indexOf(c, cursor.column() + 1);
    if (c1 != -1) {
      r.startColumn = cursor.column();
      r.endColumn = c1;

      return innerRange(r, inner);
    }

    // Try to search the quote to left
    int c2 = line.lastIndexOf(c, cursor.column() - 1);
    if (c2 != -1) {
      r.startColumn = c2;
      r.endColumn = cursor.column();

      return innerRange(r, inner);
    }

    // Nothing found - give up :)
    return Range::invalid();
  }

  r.startColumn = line.lastIndexOf(c, cursor.column());
  r.endColumn = line.indexOf(c, cursor.column());

  if (r.startColumn == -1 || r.endColumn == -1 || r.startColumn > r.endColumn) {
    return Range::invalid();
  }

  return innerRange(r, inner);
}

Range ModeBase::findSurroundingBrackets(const QChar &c1, const QChar &c2, bool inner,
                                        const QChar &nested1, const QChar &nested2) const {
  KateViI::Cursor cursor(m_interface->cursorPosition());
  Range r(cursor, InclusiveMotion);
  int line = cursor.line();
  int column = cursor.column();
  int catalan;

  // Chars should not equal. For equal chars use findSurroundingQuotes.
  Q_ASSERT(c1 != c2);

  const QString &l = m_interface->line(line);
  if (column < l.size() && l.at(column) == c2) {
    r.endLine = line;
    r.endColumn = column;
  } else {
    if (column < l.size() && l.at(column) == c1) {
      column++;
    }

    for (catalan = 1; line < m_interface->lines(); line++) {
      const QString &l = m_interface->line(line);

      for (; column < l.size(); column++) {
        const QChar &c = l.at(column);

        if (c == nested1) {
          catalan++;
        } else if (c == nested2) {
          catalan--;
        }
        if (!catalan) {
          break;
        }
      }
      if (!catalan) {
        break;
      }
      column = 0;
    }

    if (catalan != 0) {
      return Range::invalid();
    }
    r.endLine = line;
    r.endColumn = column;
  }

  // Same algorithm but backwards.
  line = cursor.line();
  column = cursor.column();

  if (column < l.size() && l.at(column) == c1) {
    r.startLine = line;
    r.startColumn = column;
  } else {
    if (column < l.size() && l.at(column) == c2) {
      column--;
    }

    for (catalan = 1; line >= 0; line--) {
      const QString &l = m_interface->line(line);
      for (; column >= 0 && column < l.size(); column--) {
        const QChar &c = l.at(column);
        if (c == nested1) {
          catalan--;
        } else if (c == nested2) {
          catalan++;
        }
        if (!catalan) {
          break;
        }
      }
      if (!catalan || !line) {
        break;
      }
      column = m_interface->line(line - 1).size() - 1;
    }
    if (catalan != 0) {
      return Range::invalid();
    }
    r.startColumn = column;
    r.startLine = line;
  }

  return innerRange(r, inner);
}

Range ModeBase::findSurrounding(const QRegExp &c1, const QRegExp &c2, bool inner) const {
  return Range();
  /*
  KTextEditor::Cursor cursor(m_view->cursorPosition());
  QString line = getLine();

  int col1 = line.lastIndexOf(c1, cursor.column());
  int col2 = line.indexOf(c2, cursor.column());

  Range r(cursor.line(), col1, cursor.line(), col2, InclusiveMotion);

  if (col1 == -1 || col2 == -1 || col1 > col2) {
      return Range::invalid();
  }

  if (inner) {
      r.startColumn++;
      r.endColumn--;
  }

  return r;
  */
}

int ModeBase::findLineStartingWitchChar(const QChar &c, int count, bool forward) const {
  int line = m_interface->cursorPosition().line();
  int lines = m_interface->lines();
  int hits = 0;

  if (forward) {
    line++;
  } else {
    line--;
  }

  while (line < lines && line >= 0 && hits < count) {
    QString l = getLine(line);
    if (l.length() > 0 && l.at(0) == c) {
      hits++;
    }
    if (hits != count) {
      if (forward) {
        line++;
      } else {
        line--;
      }
    }
  }

  if (hits == getCount()) {
    return line;
  }

  return -1;
}

void ModeBase::updateCursor(const KateViI::Cursor &c) const {
  m_viInputModeManager->updateCursor(c);
}

/**
 * @return the register given for the command. If no register was given,
 * defaultReg is returned.
 */
QChar ModeBase::getChosenRegister(const QChar &defaultReg) const {
  return (m_register != QChar::Null) ? m_register : defaultReg;
}

QString ModeBase::getRegisterContent(const QChar &reg) {
  QString r = m_viInputModeManager->globalState()->registers()->getContent(reg);

  if (r.isNull()) {
    error(tr("Nothing in register %1.", c_kateViTranslationContext).arg(reg));
  }

  return r;
}

OperationMode ModeBase::getRegisterFlag(const QChar &reg) const {
  return m_viInputModeManager->globalState()->registers()->getFlag(reg);
}

void ModeBase::fillRegister(const QChar &reg, const QString &text, OperationMode flag) {
  qDebug() << "fillRegister" << reg << text << flag;
  m_viInputModeManager->globalState()->registers()->set(reg, text, flag);
}

KateViI::Cursor ModeBase::getNextJump(KateViI::Cursor cursor) const {
  return m_viInputModeManager->jumps()->next(cursor);
}

KateViI::Cursor ModeBase::getPrevJump(KateViI::Cursor cursor) const {
  return m_viInputModeManager->jumps()->prev(cursor);
}

Range ModeBase::goLineDown() { return goLineUpDown(getCount()); }

Range ModeBase::goLineUp() { return goLineUpDown(-getCount()); }

/**
 * method for moving up or down one or more lines
 * note: the sticky column is always a virtual column
 */
Range ModeBase::goLineUpDown(int lines) {
  KateViI::Cursor c(m_interface->cursorPosition());
  Range r(c, InclusiveMotion);
  const int tabstop = m_viInputModeManager->inputAdapter()->kateViConfig()->tabWidth();

  if (lines == 0) {
    return r;
  }

  r.endLine += lines;

  // limit end line to be from line 0 through the last line
  if (r.endLine < 0) {
    r.endLine = 0;
  } else if (r.endLine > m_interface->lines() - 1) {
    r.endLine = m_interface->lines() - 1;
  }

  int endLineLen = m_interface->lineLength(r.endLine) - 1;
  if (endLineLen < 0) {
    endLineLen = 0;
  }

  int endLineLenVirt = m_interface->toVirtualColumn(r.endLine, endLineLen, tabstop);
  int virtColumnStart = m_interface->toVirtualColumn(c.line(), c.column(), tabstop);

  // if sticky column isn't set, set end column and set sticky column to its
  // virtual column
  if (m_stickyColumn == -1) {
    r.endColumn = m_interface->fromVirtualColumn(r.endLine, virtColumnStart, tabstop);
    m_stickyColumn = virtColumnStart;
  } else {
    // sticky is set - set end column to its value
    r.endColumn = m_interface->fromVirtualColumn(r.endLine, m_stickyColumn, tabstop);
  }

  // make sure end column won't be after the last column of a line
  if (r.endColumn > endLineLen) {
    r.endColumn = endLineLen;
  }

  // if we move to a line shorter than the current column, go to its end
  if (virtColumnStart > endLineLenVirt) {
    r.endColumn = endLineLen;
  }

  return r;
}

Range ModeBase::goVisualLineUpDown(int lines) {
  KateViI::Cursor c(m_interface->cursorPosition());
  Range r(c, InclusiveMotion);
  const int tabstop = m_viInputModeManager->inputAdapter()->kateViConfig()->tabWidth();

  if (lines == 0) {
    // We're not moving anywhere.
    return r;
  }

  // Different from KateVi, it is not necessary to adhere to Vi strictly.
  // Pass to the implementation to calculate it.
  // Different from Vi, we do not move if there are not enough lines to finish.
  bool succeed = false;
  KateViI::Cursor pos = m_interface->goVisualLineUpDownDry(lines, succeed);
  if (!succeed) {
    r.endLine = -1;
    r.endColumn = -1;
    return r;
  }

  r.endLine = pos.line();
  r.endColumn = pos.column();

  m_stickyColumn = m_interface->toVirtualColumn(r.endLine, r.endColumn, tabstop);
  m_currentMotionWasVisualLineUpOrDown = true;

  return r;
}

bool ModeBase::startNormalMode() {
  /* store the key presses for this "insert mode session" so that it can be
   * repeated with the
   * '.' command
   * - ignore transition from Visual Modes
   */
  if (!(m_viInputModeManager->isAnyVisualMode() ||
        m_viInputModeManager->lastChangeRecorder()->isReplaying())) {
    m_viInputModeManager->storeLastChangeCommand();
    m_viInputModeManager->clearCurrentChangeLog();
  }

  m_viInputModeManager->viEnterNormalMode();
  m_interface->setUndoMergeAllEdits(false);
  m_interface->notifyViewModeChanged(m_interface->viewMode());
  return true;
}

bool ModeBase::startInsertMode() {
  m_viInputModeManager->viEnterInsertMode();
  m_interface->setUndoMergeAllEdits(true);
  m_interface->notifyViewModeChanged(m_interface->viewMode());
  return true;
}

bool ModeBase::startReplaceMode() {
  m_viInputModeManager->viEnterReplaceMode();
  m_interface->setUndoMergeAllEdits(true);
  m_interface->notifyViewModeChanged(m_interface->viewMode());
  return true;
}

bool ModeBase::startVisualMode() {
  if (m_viInputModeManager->getCurrentViMode() == ViMode::VisualLineMode) {
    m_viInputModeManager->getViVisualMode()->setVisualModeType(ViMode::VisualMode);
    m_viInputModeManager->changeViMode(ViMode::VisualMode);
  } else if (m_viInputModeManager->getCurrentViMode() == ViMode::VisualBlockMode) {
    m_viInputModeManager->getViVisualMode()->setVisualModeType(ViMode::VisualMode);
    m_viInputModeManager->changeViMode(ViMode::VisualMode);
  } else {
    m_viInputModeManager->viEnterVisualMode();
  }

  m_interface->notifyViewModeChanged(m_interface->viewMode());
  return true;
}

bool ModeBase::startVisualBlockMode() {
  /*
  if (m_viInputModeManager->getCurrentViMode() == ViMode::VisualMode) {
      m_viInputModeManager->getViVisualMode()->setVisualModeType(ViMode::VisualBlockMode);
      m_viInputModeManager->changeViMode(ViMode::VisualBlockMode);
  } else {
      m_viInputModeManager->viEnterVisualMode(ViMode::VisualBlockMode);
  }

  emit m_view->viewModeChanged(m_view, m_view->viewMode());
  */

  return true;
}

bool ModeBase::startVisualLineMode() {
  if (m_viInputModeManager->getCurrentViMode() == ViMode::VisualMode) {
    m_viInputModeManager->getViVisualMode()->setVisualModeType(ViMode::VisualLineMode);
    m_viInputModeManager->changeViMode(ViMode::VisualLineMode);
  } else {
    m_viInputModeManager->viEnterVisualMode(ViMode::VisualLineMode);
  }

  m_interface->notifyViewModeChanged(m_interface->viewMode());
  return true;
}

void ModeBase::error(const QString &errorMsg) { qDebug() << "Not implemented yet" << errorMsg; }

void ModeBase::message(const QString &msg) { qDebug() << "Not implemented yet" << msg; }

QString ModeBase::getVerbatimKeys() const { return m_keysVerbatim; }

const QChar ModeBase::getCharAtVirtualColumn(const QString &line, int virtualColumn,
                                             int tabWidth) const {
  int column = 0;
  int tempCol = 0;

  // sanity check: if the line is empty, there are no chars
  if (line.length() == 0) {
    return QChar::Null;
  }

  while (tempCol < virtualColumn) {
    if (line.at(column) == QLatin1Char('\t')) {
      tempCol += tabWidth - (tempCol % tabWidth);
    } else {
      tempCol++;
    }

    if (tempCol <= virtualColumn) {
      column++;

      if (column >= line.length()) {
        return QChar::Null;
      }
    }
  }

  if (line.length() > column) {
    return line.at(column);
  }

  return QChar::Null;
}

void ModeBase::addToNumberUnderCursor(int count) {
  KateViI::Cursor c(m_interface->cursorPosition());
  QString line = getLine();

  if (line.isEmpty()) {
    return;
  }

  int numberStartPos = -1;
  QString numberAsString;
  QRegExp numberRegex(QLatin1String("(0x)([0-9a-fA-F]+)|\\-?\\d+"));
  const int cursorColumn = c.column();
  const int currentLineLength = m_interface->lineLength(c.line());
  const KateViI::Cursor prevWordStart = findPrevWordStart(c.line(), cursorColumn);
  int wordStartPos = prevWordStart.column();
  if (prevWordStart.line() < c.line()) {
    // The previous word starts on the previous line: ignore.
    wordStartPos = 0;
  }
  if (wordStartPos > 0 && line.at(wordStartPos - 1) == QLatin1Char('-')) {
    wordStartPos--;
  }
  for (int searchFromColumn = wordStartPos; searchFromColumn < currentLineLength;
       searchFromColumn++) {
    numberStartPos = numberRegex.indexIn(line, searchFromColumn);

    numberAsString = numberRegex.cap();

    const bool numberEndedBeforeCursor = (numberStartPos + numberAsString.length() <= c.column());
    if (!numberEndedBeforeCursor) {
      // This is the first number-like string under or after the cursor -
      // this'll do!
      break;
    }
  }

  if (numberStartPos == -1) {
    // None found.
    return;
  }

  bool parsedNumberSuccessfully = false;
  int base = numberRegex.cap(1).isEmpty() ? 10 : 16;
  if (base != 16 && numberAsString.startsWith(QLatin1Char('0')) && numberAsString.length() > 1) {
    // If a non-hex number with a leading 0 can be parsed as octal, then assume
    // it is octal.
    numberAsString.toInt(&parsedNumberSuccessfully, 8);
    if (parsedNumberSuccessfully) {
      base = 8;
    }
  }
  const int originalNumber = numberAsString.toInt(&parsedNumberSuccessfully, base);

  if (!parsedNumberSuccessfully) {
    // conversion to int failed. give up.
    return;
  }

  QString basePrefix;
  if (base == 16) {
    basePrefix = QStringLiteral("0x");
  } else if (base == 8) {
    basePrefix = QStringLiteral("0");
  }
  const QString withoutBase = numberAsString.mid(basePrefix.length());

  const int newNumber = originalNumber + count;

  // Create the new text string to be inserted. Prepend with "0x" if in base 16,
  // and "0" if base 8. For non-decimal numbers, try to keep the length of the
  // number the same (including leading 0's).
  const QString newNumberPadded =
      (base == 10)
          ? QStringLiteral("%1").arg(newNumber, 0, base)
          : QStringLiteral("%1").arg(newNumber, withoutBase.length(), base, QLatin1Char('0'));
  const QString newNumberText = basePrefix + newNumberPadded;

  // Replace the old number string with the new.
  m_interface->editStart();
  m_interface->removeText(
      KateViI::Range(c.line(), numberStartPos, c.line(), numberStartPos + numberAsString.length()));
  m_interface->insertText(KateViI::Cursor(c.line(), numberStartPos), newNumberText);
  m_interface->editEnd();
  updateCursor(KateViI::Cursor(m_interface->cursorPosition().line(),
                               numberStartPos + newNumberText.length() - 1));
}

void ModeBase::switchView(Direction direction) {
  KATEVI_NIY;
#if 0
    QList<KTextEditor::ViewPrivate *> visible_views;
    const auto views = KTextEditor::EditorPrivate::self()->views();
    for (KTextEditor::ViewPrivate *view : views) {
        if (view->isVisible()) {
            visible_views.push_back(view);
        }
    }

    QPoint current_point = m_view->mapToGlobal(m_view->pos());
    int curr_x1 = current_point.x();
    int curr_x2 = current_point.x() + m_view->width();
    int curr_y1 = current_point.y();
    int curr_y2 = current_point.y() + m_view->height();
    const KTextEditor::Cursor cursorPos = m_view->cursorPosition();
    const QPoint globalPos = m_view->mapToGlobal(m_view->cursorToCoordinate(cursorPos));
    int curr_cursor_y = globalPos.y();
    int curr_cursor_x = globalPos.x();

    KTextEditor::ViewPrivate *bestview = nullptr;
    int  best_x1 = -1, best_x2 = -1, best_y1 = -1, best_y2 = -1, best_center_y = -1, best_center_x = -1;

    if (direction == Next && visible_views.count() != 1) {
        for (int i = 0; i < visible_views.count(); i++) {
            if (visible_views.at(i) == m_view) {
                if (i != visible_views.count() - 1) {
                    bestview = visible_views.at(i + 1);
                } else {
                    bestview = visible_views.at(0);
                }
            }
        }
    } else {
        for (KTextEditor::ViewPrivate *view : qAsConst(visible_views)) {
            QPoint point = view->mapToGlobal(view->pos());
            int x1 = point.x();
            int x2 = point.x() + view->width();
            int y1 = point.y();
            int y2 = point.y() + m_view->height();
            int  center_y = (y1 + y2) / 2;
            int  center_x = (x1 + x2) / 2;

            switch (direction) {
            case Left:
                if (view != m_view && x2 <= curr_x1 &&
                        (x2 > best_x2 ||
                         (x2 == best_x2 && qAbs(curr_cursor_y - center_y) < qAbs(curr_cursor_y - best_center_y)) ||
                         bestview == nullptr)) {
                    bestview = view;
                    best_x2 = x2;
                    best_center_y = center_y;
                }
                break;
            case Right:
                if (view != m_view && x1 >= curr_x2 &&
                        (x1 < best_x1 ||
                         (x1 == best_x1 && qAbs(curr_cursor_y - center_y) < qAbs(curr_cursor_y - best_center_y)) ||
                         bestview == nullptr)) {
                    bestview = view;
                    best_x1 = x1;
                    best_center_y = center_y;
                }
                break;
            case Down:
                if (view != m_view && y1 >= curr_y2 &&
                        (y1 < best_y1 ||
                         (y1 == best_y1 && qAbs(curr_cursor_x - center_x) < qAbs(curr_cursor_x - best_center_x)) ||
                         bestview == nullptr)) {
                    bestview = view;
                    best_y1 = y1;
                    best_center_x = center_x;
                }
                break;
            case Up:
                if (view != m_view && y2 <= curr_y1 &&
                        (y2 > best_y2 ||
                         (y2 == best_y2 && qAbs(curr_cursor_x - center_x) < qAbs(curr_cursor_x - best_center_x)) ||
                         bestview == nullptr)) {
                    bestview = view;
                    best_y2 = y2;
                    best_center_x = center_x;
                }
                break;
            default:
                return;
            }

        }
    }
    if (bestview != nullptr) {
        bestview->setFocus();
        bestview->setInputMode(KateViI::ViInputMode);
    }
#endif
}

Range ModeBase::motionFindPrev() {
  return Range();
  /*
  return m_viInputModeManager->searcher()->motionFindPrev(getCount());
  */
}

Range ModeBase::motionFindNext() {
  return Range();
  /*
  return m_viInputModeManager->searcher()->motionFindNext(getCount());
  */
}

void ModeBase::goToPos(const Range &r) {
  KateViI::Cursor c;
  c.setLine(r.endLine);
  c.setColumn(r.endColumn);

  if (!c.isValid()) {
    return;
  }

  if (r.jump) {
    m_viInputModeManager->jumps()->add(m_interface->cursorPosition());
  }

  if (c.line() >= m_interface->lines()) {
    c.setLine(m_interface->lines() - 1);
  }

  updateCursor(c);
}

unsigned int ModeBase::linesDisplayed() const {
  return m_viInputModeManager->inputAdapter()->linesDisplayed();
}

int ModeBase::getCount() const {
  if (m_oneTimeCountOverride != -1) {
    return m_oneTimeCountOverride;
  }
  return (m_count > 0) ? m_count : 1;
}

void ModeBase::appendToKeysVerbatim(const QString &p_keys) {
  m_keysVerbatim.append(p_keys);
  m_viInputModeManager->inputAdapter()->updateKeyStroke();
}

void ModeBase::clearKeysVerbatim() {
  m_keysVerbatim.clear();
  m_viInputModeManager->inputAdapter()->updateKeyStroke();
}
