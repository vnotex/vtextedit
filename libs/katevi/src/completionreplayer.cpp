/*
 *  This file is part of the KDE libraries
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
 *
 */

#include "completionreplayer.h"
#include "completionrecorder.h"
#include "lastchangerecorder.h"
#include "macrorecorder.h"
#include <katevi/inputmodemanager.h>

#include <QKeyEvent>
#include <QRegularExpression>

using namespace KateVi;

CompletionReplayer::CompletionReplayer(InputModeManager *viInputModeManager)
    : m_viInputModeManager(viInputModeManager) {}

CompletionReplayer::~CompletionReplayer() {}

void CompletionReplayer::start(const CompletionList &completions) {
  m_nextCompletionIndex.push(0);
  m_CompletionsToReplay.push(completions);
}

void CompletionReplayer::stop() {
  m_CompletionsToReplay.pop();
  m_nextCompletionIndex.pop();
}

void CompletionReplayer::replay() {
  const Completion completion = nextCompletion();
  auto interface = m_viInputModeManager->editorInterface();

  // Find beginning of the word.
  KateViI::Cursor cursorPos = interface->cursorPosition();
  KateViI::Cursor wordStart = KateViI::Cursor::invalid();
  if (!interface->characterAt(cursorPos).isLetterOrNumber() &&
      interface->characterAt(cursorPos) != QLatin1Char('_')) {
    cursorPos.setColumn(cursorPos.column() - 1);
  }
  while (cursorPos.column() >= 0 && (interface->characterAt(cursorPos).isLetterOrNumber() ||
                                     interface->characterAt(cursorPos) == QLatin1Char('_'))) {
    wordStart = cursorPos;
    cursorPos.setColumn(cursorPos.column() - 1);
  }

  // Find end of current word.
  cursorPos = interface->cursorPosition();
  KateViI::Cursor wordEnd = KateViI::Cursor(cursorPos.line(), cursorPos.column() - 1);
  const int lineLength = interface->lineLength(cursorPos.line());
  while (cursorPos.column() < lineLength &&
         (interface->characterAt(cursorPos).isLetterOrNumber() ||
          interface->characterAt(cursorPos) == QLatin1Char('_'))) {
    wordEnd = cursorPos;
    cursorPos.setColumn(cursorPos.column() + 1);
  }

  QString completionText = completion.completedText();
  const KateViI::Range currentWord =
      KateViI::Range(wordStart, KateViI::Cursor(wordEnd.line(), wordEnd.column() + 1));

  // Should we merge opening brackets?
  // Yes, if completion is a function with arguments and after the cursor
  // there is (optional whitespace) followed by an open bracket.
  int offsetFinalCursorPosBy = 0;
  if (completion.completionType() == Completion::FunctionWithArgs) {
    const int nextMergableBracketAfterCursorPos = findNextMergeableBracketPos(currentWord.end());
    if (nextMergableBracketAfterCursorPos != -1) {
      if (completionText.endsWith(QLatin1String("()"))) {
        // Strip "()".
        completionText.chop(2);
      } else if (completionText.endsWith(QLatin1String("();"))) {
        // Strip "();".
        completionText.chop(3);
      }
      // Ensure cursor ends up after the merged open bracket.
      offsetFinalCursorPosBy = nextMergableBracketAfterCursorPos + 1;
    } else {
      if (!completionText.endsWith(QLatin1String("()")) &&
          !completionText.endsWith(QLatin1String("();"))) {
        // Original completion merged with an opening bracket; we'll have to add
        // our own brackets.
        completionText.append(QLatin1String("()"));
      }
      // Position cursor correctly i.e. we'll have added "functionname()" or
      // "functionname();"; need to step back by one or two to be after the
      // opening bracket.
      offsetFinalCursorPosBy = completionText.endsWith(QLatin1Char(';')) ? -2 : -1;
    }
  }

  KateViI::Cursor deleteEnd = completion.removeTail()
                                  ? currentWord.end()
                                  : KateViI::Cursor(interface->cursorPosition().line(),
                                                    interface->cursorPosition().column() + 0);

  if (currentWord.isValid()) {
    interface->removeText(KateViI::Range(currentWord.start(), deleteEnd));
    interface->insertText(currentWord.start(), completionText);
  } else {
    interface->insertText(interface->cursorPosition(), completionText);
  }

  if (offsetFinalCursorPosBy != 0) {
    interface->setCursorPosition(
        KateViI::Cursor(interface->cursorPosition().line(),
                        interface->cursorPosition().column() + offsetFinalCursorPosBy));
  }

  if (!m_viInputModeManager->lastChangeRecorder()->isReplaying()) {
    Q_ASSERT(m_viInputModeManager->macroRecorder()->isReplaying());
    // Post the completion back: it needs to be added to the "last change" list
    // ...
    m_viInputModeManager->completionRecorder()->logCompletionEvent(completion);
    // ... but don't log the ctrl-space that led to this call to
    // replayCompletion(), as a synthetic ctrl-space was just added to the last
    // change keypresses by logCompletionEvent(), and we don't want to duplicate
    // them!
    m_viInputModeManager->doNotLogCurrentKeypress();
  }
}

Completion CompletionReplayer::nextCompletion() {
  Q_ASSERT(m_viInputModeManager->lastChangeRecorder()->isReplaying() ||
           m_viInputModeManager->macroRecorder()->isReplaying());

  if (m_nextCompletionIndex.top() >= m_CompletionsToReplay.top().length()) {
    qWarning() << "Requesting more completions to replay than we actually "
                  "have. Returning a dummy one.";
    return Completion(QString(), false, Completion::PlainText);
  }

  return m_CompletionsToReplay.top()[m_nextCompletionIndex.top()++];
}

int CompletionReplayer::findNextMergeableBracketPos(const KateViI::Cursor &startPos) const {
  auto interface = m_viInputModeManager->editorInterface();
  const KateViI::Cursor endPos(startPos.line(), interface->lineLength(startPos.line()));
  const QString lineAfterCursor = interface->getText(KateViI::Range(startPos, endPos));
  static const QRegularExpression whitespaceThenOpeningBracket(QStringLiteral("^\\s*(\\()"));
  QRegularExpressionMatch match = whitespaceThenOpeningBracket.match(lineAfterCursor);
  int nextMergableBracketAfterCursorPos = -1;
  if (match.hasMatch()) {
    nextMergableBracketAfterCursorPos = match.capturedStart(1);
  }
  return nextMergableBracketAfterCursorPos;
}
