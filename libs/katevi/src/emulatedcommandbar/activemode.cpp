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

#include "activemode.h"
#include <katevi/emulatedcommandbar.h>
// #include "matchhighlighter.h"

#include <katevi/inputmodemanager.h>
#include <modes/visualvimode.h>

#include <katevi/interface/katevieditorinterface.h>

using namespace KateVi;

ActiveMode::ActiveMode(EmulatedCommandBar *emulatedCommandBar, MatchHighlighter *matchHighlighter,
                       InputModeManager *viInputModeManager)
    : m_emulatedCommandBar(emulatedCommandBar), m_viInputModeManager(viInputModeManager),
      m_interface(viInputModeManager->editorInterface()), m_matchHighligher(matchHighlighter) {}

ActiveMode::~ActiveMode() {}

CompletionStartParams
ActiveMode::completionInvoked(Completer::CompletionInvocation invocationType) {
  Q_UNUSED(invocationType);
  return CompletionStartParams();
}

void ActiveMode::setViInputModeManager(InputModeManager *viInputModeManager) {
  m_viInputModeManager = viInputModeManager;
}

#if 0
void ActiveMode::hideAllWidgetsExcept(QWidget* widgetToKeepVisible)
{
    m_emulatedCommandBar->hideAllWidgetsExcept(widgetToKeepVisible);
}

void ActiveMode::updateMatchHighlight(const KTextEditor::Range& matchRange)
{
    m_matchHighligher->updateMatchHighlight(matchRange);
}

void ActiveMode::close( bool wasAborted )
{
    m_emulatedCommandBar->m_wasAborted = wasAborted;
    m_emulatedCommandBar->hideMe();
}

void ActiveMode::closeWithStatusMessage(const QString& exitStatusMessage)
{
    m_emulatedCommandBar->closeWithStatusMessage(exitStatusMessage);
}

void ActiveMode::startCompletion ( const CompletionStartParams& completionStartParams )
{
    m_emulatedCommandBar->m_completer->startCompletion(completionStartParams);
}

void ActiveMode::moveCursorTo(const KTextEditor::Cursor &cursorPos)
{
    m_view->setCursorPosition(cursorPos);
    if (m_viInputModeManager->getCurrentViMode() == ViMode::VisualMode ||
        m_viInputModeManager->getCurrentViMode() == ViMode::VisualLineMode) {

        m_viInputModeManager->getViVisualMode()->goToPos(cursorPos);
    }
}
#endif

EmulatedCommandBar *ActiveMode::emulatedCommandBar() { return m_emulatedCommandBar; }

KateViI::KateViEditorInterface *ActiveMode::editorInterface() { return m_interface; }

InputModeManager *ActiveMode::viInputModeManager() { return m_viInputModeManager; }
