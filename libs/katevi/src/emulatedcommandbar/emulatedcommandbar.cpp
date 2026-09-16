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

#include <katevi/emulatedcommandbar.h>

// #include "../commandrangeexpressionparser.h"
#include <katevi/global.h>
#include <katevi/globalstate.h>
#include <katevi/inputmodemanager.h>
#include <katevi/interface/katevieditorinterface.h>
#include <katevi/interface/kateviinputmode.h>
#include <keyparser.h>
#include <modes/normalvimode.h>
// #include "matchhighlighter.h"
// #include "interactivesedreplacemode.h"
#include "commandmode.h"
#include "completer.h"

#include <history.h>

#include <registers.h>
#include <searcher.h>

#include <QApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

using namespace KateVi;

namespace {
/**
 * @return \a originalRegex but escaped in such a way that a Qt regex search for
 * the resulting string will match the string \a originalRegex.
 */
QString escapedForSearchingAsLiteral(const QString &originalQtRegex) {
  QString escapedForSearchingAsLiteral = originalQtRegex;
  escapedForSearchingAsLiteral.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
  escapedForSearchingAsLiteral.replace(QLatin1Char('$'), QLatin1String("\\$"));
  escapedForSearchingAsLiteral.replace(QLatin1Char('^'), QLatin1String("\\^"));
  escapedForSearchingAsLiteral.replace(QLatin1Char('.'), QLatin1String("\\."));
  escapedForSearchingAsLiteral.replace(QLatin1Char('*'), QLatin1String("\\*"));
  escapedForSearchingAsLiteral.replace(QLatin1Char('/'), QLatin1String("\\/"));
  escapedForSearchingAsLiteral.replace(QLatin1Char('['), QLatin1String("\\["));
  escapedForSearchingAsLiteral.replace(QLatin1Char(']'), QLatin1String("\\]"));
  escapedForSearchingAsLiteral.replace(QLatin1Char('\n'), QLatin1String("\\n"));
  return escapedForSearchingAsLiteral;
}
} // namespace

class KateVi::SearchMode : public ActiveMode {
public:
  SearchMode(EmulatedCommandBar *p_bar, InputModeManager *p_manager, QLineEdit *p_edit)
      : ActiveMode(p_bar, nullptr, p_manager), m_edit(p_edit) {}

  void init(bool p_backwards) {
    m_origin = editorInterface()->cursorPosition();
    m_params = Searcher::SearchParams();
    m_params.isBackwards = p_backwards;
    m_match = KateViI::Range::invalid();
    m_count = viInputModeManager()->getCurrentViModeHandler()->getCount();
    m_active = true;
  }

  bool handleKeyPress(const QKeyEvent *p_event) Q_DECL_OVERRIDE {
    Q_UNUSED(p_event);
    return false;
  }

  void editTextChanged(const QString &p_text) Q_DECL_OVERRIDE { updateSearch(p_text); }

  void completionChosen() Q_DECL_OVERRIDE {
    const QString pattern = m_edit->text().isEmpty()
                                ? viInputModeManager()->searcher()->getLastSearchPattern()
                                : m_edit->text();
    m_match = KateViI::Range::invalid();
    if (!pattern.isEmpty() && QRegularExpression(pattern).isValid()) {
      m_params.pattern = pattern;
      m_params.isCaseSensitive = std::any_of(pattern.cbegin(), pattern.cend(),
                                             [](QChar p_char) { return p_char.isUpper(); });
      m_match = viInputModeManager()->searcher()->findPattern(m_params, m_origin, m_count, true);
      if (m_match.isValid() && !moveTo(m_match.start())) {
        m_match = KateViI::Range::invalid();
      }
    }
    emulatedCommandBar()->m_wasAborted = false;
    emit emulatedCommandBar() -> hideMe();
  }

  CompletionStartParams
  completionInvoked(Completer::CompletionInvocation p_invocation) Q_DECL_OVERRIDE {
    Q_UNUSED(p_invocation);
    const auto &history = viInputModeManager()->globalState()->searchHistory()->items();
    return history.isEmpty() ? CompletionStartParams::invalid()
                             : CompletionStartParams::createModeSpecific(history, 0);
  }

  void deactivate(bool p_wasAborted) Q_DECL_OVERRIDE {
    if (!m_active) {
      return;
    }
    m_active = false;
    const bool success = m_match.isValid() && !p_wasAborted;
    if (!success) {
      moveTo(m_origin);
    }
    QScopedValueRollback<bool> sending(m_sendingSyntheticSearchCompletedKeypress, true);
    QKeyEvent event(QEvent::KeyPress, success ? Qt::Key_Enter : Qt::Key_Escape, Qt::NoModifier,
                    success ? QStringLiteral("\r") : QString());
    viInputModeManager()->handleKeyPress(&event);
  }

  bool isSendingSyntheticSearchCompletedKeypress() const {
    return m_sendingSyntheticSearchCompletedKeypress;
  }

private:
  void updateSearch(const QString &p_pattern) {
    m_params.pattern = p_pattern;
    m_params.isCaseSensitive = std::any_of(p_pattern.cbegin(), p_pattern.cend(),
                                           [](QChar p_char) { return p_char.isUpper(); });
    m_match = KateViI::Range::invalid();
    auto feedback = emulatedCommandBar()->m_exitStatusMessageDisplay;
    feedback->hide();
    if (p_pattern.isEmpty()) {
      moveTo(m_origin);
      return;
    }
    const bool valid = QRegularExpression(p_pattern).isValid();
    if (valid) {
      m_match = viInputModeManager()->searcher()->findPattern(m_params, m_origin, m_count, false);
      if (m_match.isValid() && !moveTo(m_match.start())) {
        m_match = KateViI::Range::invalid();
      }
    }
    if (!m_match.isValid()) {
      moveTo(m_origin);
      feedback->setText(valid ? EmulatedCommandBar::tr("Pattern not found")
                              : EmulatedCommandBar::tr("Invalid regular expression"));
      feedback->show();
    }
  }

  bool moveTo(const KateViI::Cursor &p_cursor) {
    viInputModeManager()->getCurrentViModeHandler()->goToPos(Range(p_cursor, ExclusiveMotion));
    return editorInterface()->cursorPosition() == p_cursor;
  }

  QLineEdit *m_edit;
  KateViI::Cursor m_origin;
  Searcher::SearchParams m_params;
  KateViI::Range m_match;
  int m_count = 1;
  bool m_active = false;
  bool m_sendingSyntheticSearchCompletedKeypress = false;
};

EmulatedCommandBar::EmulatedCommandBar(KateViI::KateViInputMode *viInputMode,
                                       InputModeManager *viInputModeManager, QWidget *parent)
    : QWidget(parent), m_viInputMode(viInputMode), m_viInputModeManager(viInputModeManager),
      m_interface(viInputModeManager->editorInterface()) {
  QHBoxLayout *layout = new QHBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);

  createAndAddBarTypeIndicator(layout);

  createAndAddEditWidget(layout);

  createAndAddExitStatusMessageDisplay(layout);

  createAndInitExitStatusMessageDisplayTimer();

  createAndAddWaitingForRegisterIndicator(layout);

  m_completer.reset(new Completer(this, m_interface, m_edit));

  /*
  m_matchHighligher.reset(new MatchHighlighter(m_view));

  m_interactiveSedReplaceMode.reset(new InteractiveSedReplaceMode(this,
  m_matchHighligher.data(), m_viInputModeManager, m_view));
  layout->addWidget(m_interactiveSedReplaceMode->label());
  m_commandMode.reset(new
  CommandMode(this, m_matchHighligher.data(), m_viInputModeManager, m_view,
                                      m_edit,
                                      m_interactiveSedReplaceMode.data(),
                                      m_completer.data()));
  */
  m_searchMode.reset(new SearchMode(this, m_viInputModeManager, m_edit));
  m_commandMode.reset(
      new CommandMode(this, nullptr, m_viInputModeManager, m_edit, nullptr, m_completer.data()));
}

EmulatedCommandBar::~EmulatedCommandBar() {}

void EmulatedCommandBar::init(EmulatedCommandBar::Mode mode, const QString &initialText) {
  if (m_isActive) {
    closed();
  }
  m_mode = mode;
  m_isActive = true;
  m_wasAborted = true;
  m_waitingForRegister = false;
  m_insertedTextShouldBeEscapedForSearchingAsLiteral = false;
  m_waitingForRegisterIndicator->hide();
  m_exitStatusMessageDisplay->hide();
  m_exitStatusMessageDisplayHideTimer->stop();
  showBarTypeIndicator(mode);

  if (mode == SearchBackward || mode == SearchForward) {
    switchToMode(m_searchMode.data());
    m_searchMode->init(mode == SearchBackward);
  } else {
    switchToMode(m_commandMode.data());
  }

  const bool unchanged = m_edit->text() == initialText;
  m_edit->show();
  m_edit->setText(initialText);
  if (unchanged) {
    editTextChanged(initialText);
  }
  m_edit->setFocus();

  // Deliver focus changes before a mapping, macro or test feeds the next key.
  QApplication::processEvents();
}

bool EmulatedCommandBar::isActive() { return m_isActive; }

void EmulatedCommandBar::setCommandResponseMessageTimeout(
    long int commandResponseMessageTimeOutMS) {
  m_exitStatusMessageHideTimeOutMS = commandResponseMessageTimeOutMS;
}

void EmulatedCommandBar::closed() {
  m_isActive = false;
  m_completer->deactivateCompletion();
  auto mode = m_currentMode;
  m_currentMode = nullptr;
  m_completer->setCurrentMode(nullptr);
  if (mode) {
    mode->deactivate(m_wasAborted);
  }
}

void EmulatedCommandBar::switchToMode(ActiveMode *newMode) {
  if (m_currentMode)
    m_currentMode->deactivate(false);
  m_currentMode = newMode;
  m_completer->setCurrentMode(newMode);
}

bool EmulatedCommandBar::barHandledKeypress(const QKeyEvent *keyEvent) {
  const int key = keyEvent->key();
  const int modifiers = keyEvent->modifiers();
  if ((modifiers == Qt::ControlModifier && key == Qt::Key_H) || key == Qt::Key_Backspace) {
    if (m_edit->text().isEmpty()) {
      emit hideMe();
    }

    m_edit->backspace();
    return true;
  }

  if (modifiers != Qt::ControlModifier) {
    return false;
  }

  if (key == Qt::Key_B) {
    m_edit->setCursorPosition(0);
    return true;
  } else if (key == Qt::Key_E) {
    m_edit->setCursorPosition(m_edit->text().length());
    return true;
  } else if (key == Qt::Key_W) {
    deleteSpacesToLeftOfCursor();
    if (!deleteNonWordCharsToLeftOfCursor()) {
      deleteWordCharsToLeftOfCursor();
    }
    return true;
  } else if (key == Qt::Key_R || key == Qt::Key_G) {
    m_waitingForRegister = true;
    m_waitingForRegisterIndicator->setVisible(true);
    if (key == Qt::Key_G) {
      m_insertedTextShouldBeEscapedForSearchingAsLiteral = true;
    }
    return true;
  }
  return false;
}

void EmulatedCommandBar::insertRegisterContents(const QKeyEvent *keyEvent) {
  const int key = keyEvent->key();
  const int modifiers = keyEvent->modifiers();
  if (key != Qt::Key_Shift && key != Qt::Key_Control) {
    const QChar key = KeyParser::self()->KeyEventToQChar(*keyEvent).toLower();

    const int oldCursorPosition = m_edit->cursorPosition();
    QString textToInsert;
    if (modifiers == Qt::ControlModifier && key.toLatin1() == Qt::Key_W) {
      textToInsert = m_interface->wordAt(m_interface->cursorPosition());
    } else {
      textToInsert = m_viInputModeManager->globalState()->registers()->getContent(key);
    }
    if (m_insertedTextShouldBeEscapedForSearchingAsLiteral) {
      textToInsert = escapedForSearchingAsLiteral(textToInsert);
      m_insertedTextShouldBeEscapedForSearchingAsLiteral = false;
    }
    m_edit->setText(m_edit->text().insert(m_edit->cursorPosition(), textToInsert));
    m_edit->setCursorPosition(oldCursorPosition + textToInsert.length());
    m_waitingForRegister = false;
    m_waitingForRegisterIndicator->setVisible(false);
  }
}

bool EmulatedCommandBar::eventFilter(QObject *object, QEvent *event) {
  // The "object" will be either m_edit or m_completer's popup.
  if (m_suspendEditEventFiltering) {
    return false;
  }

  Q_UNUSED(object);
  if (event->type() == QEvent::KeyPress) {
    // Re-route this keypress through Vim's central keypress handling area,
    // so that we can use the keypress in e.g. mappings and macros.
    return m_viInputMode->keyPress(static_cast<QKeyEvent *>(event));
  }

  return false;
}

void EmulatedCommandBar::deleteSpacesToLeftOfCursor() {
  while (m_edit->cursorPosition() != 0 &&
         m_edit->text().at(m_edit->cursorPosition() - 1) == QLatin1Char(' ')) {
    m_edit->backspace();
  }
}

void EmulatedCommandBar::deleteWordCharsToLeftOfCursor() {
  while (m_edit->cursorPosition() != 0) {
    const QChar charToTheLeftOfCursor = m_edit->text().at(m_edit->cursorPosition() - 1);
    if (!charToTheLeftOfCursor.isLetterOrNumber() && charToTheLeftOfCursor != QLatin1Char('_')) {
      break;
    }

    m_edit->backspace();
  }
}

bool EmulatedCommandBar::deleteNonWordCharsToLeftOfCursor() {
  bool deletionsMade = false;
  while (m_edit->cursorPosition() != 0) {
    const QChar charToTheLeftOfCursor = m_edit->text().at(m_edit->cursorPosition() - 1);
    if (charToTheLeftOfCursor.isLetterOrNumber() || charToTheLeftOfCursor == QLatin1Char('_') ||
        charToTheLeftOfCursor == QLatin1Char(' ')) {
      break;
    }

    m_edit->backspace();
    deletionsMade = true;
  }
  return deletionsMade;
}

bool EmulatedCommandBar::handleKeyPress(const QKeyEvent *keyEvent) {
  if (m_waitingForRegister) {
    insertRegisterContents(keyEvent);
    return true;
  }

  const bool completerHandled = m_completer->completerHandledKeyPress(keyEvent);
  if (completerHandled) {
    return true;
  }

  const int key = keyEvent->key();
  const int modifiers = keyEvent->modifiers();
  if ((modifiers == Qt::ControlModifier && (key == Qt::Key_C || key == Qt::Key_BracketLeft)) ||
      (modifiers == Qt::NoModifier && key == Qt::Key_Escape)) {
    emit hideMe();
    return true;
  }

  // Is this a built-in Emulated Command Bar keypress e.g. insert from register,
  // ctrl-h, etc?
  const bool barHandled = barHandledKeypress(keyEvent);
  if (barHandled) {
    return true;
  }

  // Can the current mode handle it?
  const bool currentModeHandled = m_currentMode->handleKeyPress(keyEvent);
  if (currentModeHandled) {
    return true;
  }

  // Couldn't handle this key event.
  // Send the keypress back to the QLineEdit.
  // Ideally, instead of doing this, we would simply return "false",
  // and let Qt re-dispatch the event itself; however, there is a corner case
  // in that if the selection changes (as a result of e.g. incremental searches
  // during Visual Mode), and the keypress that causes it
  // is not dispatched from within KateViInputModeHandler::handleKeypress(...)
  // (so KateViInputModeManager::isHandlingKeypress() returns false),
  // we lose information about whether we are in Visual Mode, Visual Line Mode,
  // etc.  See VisualViMode::updateSelection( ).
  if (m_edit->isVisible()) {
    if (m_suspendEditEventFiltering) {
      return false;
    }

    m_suspendEditEventFiltering = true;
    QKeyEvent keyEventCopy(keyEvent->type(), keyEvent->key(), keyEvent->modifiers(),
                           keyEvent->text(), keyEvent->isAutoRepeat(), keyEvent->count());
    qApp->notify(m_edit, &keyEventCopy);
    m_suspendEditEventFiltering = false;
  }
  return true;
}

bool EmulatedCommandBar::isSendingSyntheticSearchCompletedKeypress() {
  return m_searchMode && m_searchMode->isSendingSyntheticSearchCompletedKeypress();
}

/*
void
EmulatedCommandBar::startInteractiveSearchAndReplace(QSharedPointer<SedReplace::InteractiveSedReplacer>
interactiveSedReplace)
{
    KATEVI_NIY;
    Q_ASSERT_X(interactiveSedReplace->currentMatch().isValid(),
               "startInteractiveSearchAndReplace",
               "KateCommands shouldn't initiate an interactive sed replace with
no initial match"); switchToMode(m_interactiveSedReplaceMode.data());
    m_interactiveSedReplaceMode->activate(interactiveSedReplace);
}
*/

void EmulatedCommandBar::showBarTypeIndicator(EmulatedCommandBar::Mode mode) {
  QChar barTypeIndicator = QChar::Null;
  switch (mode) {
  case SearchForward:
    barTypeIndicator = QLatin1Char('/');
    break;
  case SearchBackward:
    barTypeIndicator = QLatin1Char('?');
    break;
  case Command:
    barTypeIndicator = QLatin1Char(':');
    break;
  default:
    Q_ASSERT(false && "Unknown mode!");
  }
  m_barTypeIndicator->setText(barTypeIndicator);
  m_barTypeIndicator->show();
}

QString EmulatedCommandBar::executeCommand(const QString &commandToExecute) {
  return m_commandMode->executeCommand(commandToExecute);
}

void EmulatedCommandBar::closeWithStatusMessage(const QString &exitStatusMessage) {
  // Display the message for a while.
  // Become inactive, so we don't steal keys in the meantime.
  m_isActive = false;

  m_exitStatusMessageDisplay->show();
  m_exitStatusMessageDisplay->setText(exitStatusMessage);
  hideAllWidgetsExcept(m_exitStatusMessageDisplay);

  m_exitStatusMessageDisplayHideTimer->start(m_exitStatusMessageHideTimeOutMS);
}

void EmulatedCommandBar::editTextChanged(const QString &newText) {
  if (!m_isActive || !m_currentMode) {
    return;
  }
  m_currentMode->editTextChanged(newText);
  m_completer->editTextChanged(newText);
}

void EmulatedCommandBar::startHideExitStatusMessageTimer() {
  if (m_exitStatusMessageDisplay->isVisible() && !m_exitStatusMessageDisplayHideTimer->isActive()) {
    m_exitStatusMessageDisplayHideTimer->start(m_exitStatusMessageHideTimeOutMS);
  }
}

void EmulatedCommandBar::setViInputModeManager(InputModeManager *viInputModeManager) {
  m_viInputModeManager = viInputModeManager;
  m_searchMode->setViInputModeManager(viInputModeManager);
  m_commandMode->setViInputModeManager(viInputModeManager);
  // m_interactiveSedReplaceMode->setViInputModeManager(viInputModeManager);
}

void EmulatedCommandBar::hideAllWidgetsExcept(QWidget *widgetToKeepVisible) {
  const QList<QWidget *> widgets = this->findChildren<QWidget *>();
  for (QWidget *widget : widgets) {
    if (widget != widgetToKeepVisible) {
      widget->hide();
    }
  }
}

void EmulatedCommandBar::createAndAddBarTypeIndicator(QLayout *layout) {
  m_barTypeIndicator = new QLabel(this);
  m_barTypeIndicator->setObjectName(QStringLiteral("BarTypeIndicator.EmulatedCommandBar.KateVi"));
  layout->addWidget(m_barTypeIndicator);
}

void EmulatedCommandBar::createAndAddEditWidget(QLayout *layout) {
  m_edit = new QLineEdit(this);
  m_edit->setObjectName(QStringLiteral("CommandText.EmulatedCommandBar.KateVi"));
  layout->addWidget(m_edit);

  m_edit->installEventFilter(this);
  connect(m_edit, SIGNAL(textChanged(const QString &)), this,
          SLOT(editTextChanged(const QString &)));
}

void EmulatedCommandBar::createAndAddExitStatusMessageDisplay(QLayout *layout) {
  m_exitStatusMessageDisplay = new QLabel(this);
  m_exitStatusMessageDisplay->setObjectName(
      QStringLiteral("CommandResponseMessage.EmulatedCommandBar.KateVi"));
  m_exitStatusMessageDisplay->setAlignment(Qt::AlignLeft);
  layout->addWidget(m_exitStatusMessageDisplay);
}

void EmulatedCommandBar::createAndInitExitStatusMessageDisplayTimer() {
  m_exitStatusMessageDisplayHideTimer = new QTimer(this);
  m_exitStatusMessageDisplayHideTimer->setSingleShot(true);
  connect(m_exitStatusMessageDisplayHideTimer, SIGNAL(timeout()), this, SIGNAL(hideMe()));
  // Make sure the timer is stopped when the user switches views. If not, focus
  // will be given to the wrong view when KateViewBar::hideCurrentBarWidget() is
  // called as a result of m_commandResponseMessageDisplayHide timing out.
  m_interface->connectFocusOut([this]() {
    if (!m_viInputModeManager->inputAdapter()->isActive()) {
      return;
    }
    m_exitStatusMessageDisplayHideTimer->stop();
  });
  // We can restart the timer once the view has focus again, though.
  m_interface->connectFocusIn([this]() {
    if (!m_viInputModeManager->inputAdapter()->isActive()) {
      return;
    }
    startHideExitStatusMessageTimer();
  });
}

void EmulatedCommandBar::createAndAddWaitingForRegisterIndicator(QLayout *layout) {
  m_waitingForRegisterIndicator = new QLabel(this);
  m_waitingForRegisterIndicator->setObjectName(
      QStringLiteral("WaitingForRegisterIndicator.EmulatedCommandBar.KateVi"));
  m_waitingForRegisterIndicator->setVisible(false);
  m_waitingForRegisterIndicator->setText(QStringLiteral("\""));
  layout->addWidget(m_waitingForRegisterIndicator);
}
