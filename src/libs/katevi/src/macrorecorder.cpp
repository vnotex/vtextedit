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

#include "macrorecorder.h"
#include <katevi/inputmodemanager.h>
#include "completionrecorder.h"
#include <keymapper.h>
#include <katevi/globalstate.h>
#include "macros.h"
#include "completionreplayer.h"
#include "lastchangerecorder.h"

namespace {
    const QChar LastPlayedRegister = QLatin1Char('@');
}

using namespace KateVi;

MacroRecorder::MacroRecorder(InputModeManager *viInputModeManager)
    : m_viInputModeManager(viInputModeManager)
{
}

MacroRecorder::~MacroRecorder()
{
}

void MacroRecorder::start(const QChar &macroRegister)
{
    Q_ASSERT(!m_isRecording);
    m_isRecording = true;
    m_register = macroRegister;
    m_viInputModeManager->globalState()->macros()->remove(macroRegister);
    m_eventsLog.clear();
    m_viInputModeManager->completionRecorder()->start();
}

void MacroRecorder::stop()
{
    Q_ASSERT(m_isRecording);
    m_isRecording = false;
    CompletionList completions = m_viInputModeManager->completionRecorder()->stop();
    m_viInputModeManager->globalState()->macros()->store(m_register, m_eventsLog, completions);
}

bool MacroRecorder::isRecording() const
{
    return m_isRecording;
}

void MacroRecorder::record(const QKeyEvent &event)
{
    if (isRepeatOfLastShortcutOverrideAsKeyPress(event, m_eventsLog))
        return;
    EventData data;
    data.SetData(event);
    m_eventsLog.append(data);
}

void MacroRecorder::dropLast()
{
    if (m_isRecording) {
        Q_ASSERT(!m_eventsLog.isEmpty());
        m_eventsLog.pop_back();
    }
}

void MacroRecorder::replay(const QChar &macroRegister)
{
    const QChar reg = (macroRegister == LastPlayedRegister) ? m_lastPlayedMacroRegister
                                                            : macroRegister;
    m_lastPlayedMacroRegister = reg;
    const QString macroAsFeedableKeypresses = m_viInputModeManager->globalState()->macros()->get(reg);

    QSharedPointer<KeyMapper> mapper(new KeyMapper(m_viInputModeManager,
                                                   m_viInputModeManager->editorInterface()));
    CompletionList completions = m_viInputModeManager->globalState()->macros()->getCompletions(reg);

    m_macrosBeingReplayedCount++;
    m_viInputModeManager->completionReplayer()->start(completions);
    m_viInputModeManager->pushKeyMapper(mapper);
    m_viInputModeManager->feedKeyPresses(macroAsFeedableKeypresses);
    m_viInputModeManager->popKeyMapper();
    m_viInputModeManager->completionReplayer()->stop();
    m_macrosBeingReplayedCount--;
}

bool MacroRecorder::isReplaying()
{
    return m_macrosBeingReplayedCount > 0;
}
