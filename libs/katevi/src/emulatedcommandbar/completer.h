/*  This file is part of the KDE libraries and the Kate part.
 *
 *  Copyright (C) 2013-2016 Simon St James <kdedevel@etotheipiplusone.com>
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

#ifndef KATEVI_EMULATED_COMMAND_BAR_COMPLETER_H
#define KATEVI_EMULATED_COMMAND_BAR_COMPLETER_H

#include <functional>

#include <QStringList>

class QLineEdit;
class QCompleter;
class QStringListModel;
class QKeyEvent;

namespace KateViI {
class KateViEditorInterface;
}

namespace KateVi {
class ActiveMode;
class EmulatedCommandBar;

struct CompletionStartParams {
  static CompletionStartParams
  createModeSpecific(const QStringList &completions, int wordStartPos,
                     std::function<QString(const QString &)> completionTransform =
                         std::function<QString(const QString &)>()) {
    CompletionStartParams completionStartParams;
    completionStartParams.completionType = ModeSpecific;
    completionStartParams.completions = completions;
    completionStartParams.wordStartPos = wordStartPos;
    completionStartParams.completionTransform = completionTransform;
    return completionStartParams;
  }

  static CompletionStartParams invalid() {
    CompletionStartParams completionStartParams;
    completionStartParams.completionType = None;
    return completionStartParams;
  }

  enum CompletionType { None, ModeSpecific, WordFromDocument };

  CompletionType completionType = None;

  int wordStartPos = -1;

  QStringList completions;

  std::function<QString(const QString &)> completionTransform;
};

class Completer {
public:
  enum class CompletionInvocation { ExtraContext, NormalContext };

  Completer(EmulatedCommandBar *emulatedCommandBar, KateViI::KateViEditorInterface *editorInterface,
            QLineEdit *edit);

  void startCompletion(const CompletionStartParams &completionStartParams);

  void deactivateCompletion();

  bool isCompletionActive() const;

  bool isNextTextChangeDueToCompletionChange() const;

  bool completerHandledKeyPress(const QKeyEvent *keyEvent);

  void editTextChanged(const QString &newText);

  void setCurrentMode(ActiveMode *currentMode);

private:
  void currentCompletionChanged();

  QString wordBeforeCursor();

  int wordBeforeCursorBegin();

  void updateCompletionPrefix();

  void setCompletionIndex(int index);

  CompletionStartParams activateWordFromDocumentCompletion();

  void abortCompletionAndResetToPreCompletion();

  QLineEdit *m_edit = nullptr;

  KateViI::KateViEditorInterface *m_interface = nullptr;

  ActiveMode *m_currentMode = nullptr;

  // Managed by QObject.
  QCompleter *m_completer = nullptr;

  QStringListModel *m_completionModel = nullptr;

  QString m_textToRevertToIfCompletionAborted;

  int m_cursorPosToRevertToIfCompletionAborted = 0;

  bool m_isNextTextChangeDueToCompletionChange = false;

  CompletionStartParams m_currentCompletionStartParams;

  CompletionStartParams::CompletionType m_currentCompletionType = CompletionStartParams::None;
};
} // namespace KateVi
#endif
