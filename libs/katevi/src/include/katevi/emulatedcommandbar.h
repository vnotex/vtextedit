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

#ifndef KATEVI_EMULATED_COMMAND_BAR_H
#define KATEVI_EMULATED_COMMAND_BAR_H

#include <QScopedPointer>
#include <QWidget>

// #include <cmds.h>

#include <katevi/interface/range.h>
// #include <katevi/interface/movingrange.h>
#include <katevi/katevi_export.h>
// #include <searcher.h>
// #include "activemode.h"

namespace KateViI {
class Command;
class KateViEditorInterface;
class KateViInputMode;
} // namespace KateViI

class QLabel;
class QLayout;
class QLineEdit;

namespace KateVi {
class MatchHighlighter;
class InteractiveSedReplaceMode;
class SearchMode;
class CommandMode;
class InputModeManager;
class Completer;
class ActiveMode;

/**
 * A QWidget that attempts to emulate some of the features of Vim's own command
 * bar, including insertion of register contents via ctr-r<registername>;
 * dismissal via ctrl-c and ctrl-[; bi-directional incremental searching, with
 * SmartCase; interactive sed-replace; plus a few extensions such as completion
 * from document and navigable sed search and sed replace history.
 */
class KATEVI_EXPORT EmulatedCommandBar : public QWidget {
  Q_OBJECT

public:
  enum Mode { NoMode, SearchForward, SearchBackward, Command };

  explicit EmulatedCommandBar(KateViI::KateViInputMode *viInputMode,
                              InputModeManager *viInputModeManager, QWidget *parent = nullptr);

  ~EmulatedCommandBar() override;

  void init(Mode mode, const QString &initialText = QString());

  void setCommandResponseMessageTimeout(long commandResponseMessageTimeOutMS);

  /*
  void
  startInteractiveSearchAndReplace(QSharedPointer<SedReplace::InteractiveSedReplacer>
  interactiveSedReplace);
  */

  QString executeCommand(const QString &commandToExecute);

  void setViInputModeManager(InputModeManager *viInputModeManager);

  bool isActive();

  bool handleKeyPress(const QKeyEvent *keyEvent);

  bool isSendingSyntheticSearchCompletedKeypress();

  // Should be called before hiding me.
  void closed();

signals:
  void hideMe();

  void showMe();

private:
  friend class ActiveMode;

  void showBarTypeIndicator(Mode mode);

  void switchToMode(ActiveMode *newMode);

  bool eventFilter(QObject *object, QEvent *event) override;

  bool barHandledKeypress(const QKeyEvent *keyEvent);

  void deleteSpacesToLeftOfCursor();
  void deleteWordCharsToLeftOfCursor();
  bool deleteNonWordCharsToLeftOfCursor();

  void hideAllWidgetsExcept(QWidget *widgetToKeepVisible);

  void closeWithStatusMessage(const QString &exitStatusMessage);

  void insertRegisterContents(const QKeyEvent *keyEvent);

  void createAndAddBarTypeIndicator(QLayout *layout);

  void createAndAddEditWidget(QLayout *layout);

  void createAndAddExitStatusMessageDisplay(QLayout *layout);

  void createAndInitExitStatusMessageDisplayTimer();

  void createAndAddWaitingForRegisterIndicator(QLayout *layout);

  KateViI::KateViInputMode *m_viInputMode = nullptr;

  InputModeManager *m_viInputModeManager = nullptr;

  bool m_isActive = false;
  bool m_wasAborted = true;
  Mode m_mode = NoMode;

  KateViI::KateViEditorInterface *m_interface = nullptr;

  QLineEdit *m_edit = nullptr;

  QLabel *m_barTypeIndicator = nullptr;

  bool m_suspendEditEventFiltering = false;

  bool m_waitingForRegister = false;
  QLabel *m_waitingForRegisterIndicator;
  bool m_insertedTextShouldBeEscapedForSearchingAsLiteral = false;

  QScopedPointer<Completer> m_completer;

#if 0
    QScopedPointer<MatchHighlighter> m_matchHighligher;
    QScopedPointer<InteractiveSedReplaceMode> m_interactiveSedReplaceMode;
    QScopedPointer<SearchMode> m_searchMode;
#endif

  QScopedPointer<CommandMode> m_commandMode;

  ActiveMode *m_currentMode = nullptr;

  QTimer *m_exitStatusMessageDisplayHideTimer = nullptr;

  QLabel *m_exitStatusMessageDisplay = nullptr;

  long m_exitStatusMessageHideTimeOutMS = 4000;

private Q_SLOTS:
  void editTextChanged(const QString &newText);

  void startHideExitStatusMessageTimer();
};

} // namespace KateVi

#endif /* KATEVI_EMULATED_COMMAND_BAR_H */
