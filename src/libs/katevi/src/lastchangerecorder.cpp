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

#include "lastchangerecorder.h"
#include <katevi/inputmodemanager.h>
#include <keyparser.h>
#include <viutils.h>
#include "completionreplayer.h"

using namespace KateVi;

void EventData::operator=(const QKeyEvent& e)
{
    key = e.key();
    text = e.text();
    modifiers = e.modifiers();
    type = e.type();
    toChar = KeyParser::self()->KeyEventToQChar(e);
}
void EventData::SetData(const QKeyEvent &e)
{
    key = e.key();
    text = e.text();
    modifiers = e.modifiers();
    type = e.type();
    toChar = KeyParser::self()->KeyEventToQChar(e);
}


bool KateVi::isRepeatOfLastShortcutOverrideAsKeyPress(const QKeyEvent& currentKeyPress,
                                                      const QList<EventData>& keyEventLog)
{
    if (keyEventLog.empty())
        return false;
    const EventData& lastKeyPress = keyEventLog.last();
    if (lastKeyPress.type == QEvent::ShortcutOverride
        && currentKeyPress.type() == QEvent::KeyPress
        && lastKeyPress.key == currentKeyPress.key()
        && lastKeyPress.modifiers == currentKeyPress.modifiers())
    {
        return true;
    }
    return false;
}

LastChangeRecorder::LastChangeRecorder(InputModeManager *viInputModeManager)
    : m_viInputModeManager(viInputModeManager)
{
}

LastChangeRecorder::~LastChangeRecorder()
{
}

void LastChangeRecorder::record(const QKeyEvent &e)
{
    if (isRepeatOfLastShortcutOverrideAsKeyPress(e, m_changeLog))
        return;

    if (!ViUtils::isModifier(e.key())) {
        EventData e2;
        e2.SetData(e);
        m_changeLog.append(e2);
    }
}

void LastChangeRecorder::dropLast()
{
    Q_ASSERT(!m_changeLog.isEmpty());
    m_changeLog.pop_back();
}

void LastChangeRecorder::clear()
{
    m_changeLog.clear();
}

QString LastChangeRecorder::encodedChanges() const
{
    QString result;

    QList<EventData> keyLog = m_changeLog;

    for (int i = 0; i < keyLog.size(); i++) {
        int keyCode = keyLog.at(i).key;
        QString text = keyLog.at(i).text;
        int mods = keyLog.at(i).modifiers;
        QChar key;

        if (text.length() > 0) {
            key = text.at(0);
        }

        if (text.isEmpty()
            || (text.length() == 1 && text.at(0).row() < 0x20)
            || (mods != Qt::NoModifier && mods != Qt::ShiftModifier)) {
            QString keyPress;

            keyPress.append(QLatin1Char('<'));
            keyPress.append((mods & Qt::ShiftModifier)   ? QStringLiteral("s-") : QString());
            keyPress.append((mods & Qt::ControlModifier) ? QStringLiteral("c-") : QString());
            keyPress.append((mods & Qt::AltModifier)     ? QStringLiteral("a-") : QString());
            keyPress.append((mods & Qt::MetaModifier)    ? QStringLiteral("m-") : QString());
            keyPress.append(keyCode <= 0xFF ? QChar(keyCode) : KeyParser::self()->qt2vi(keyCode));
            keyPress.append(QLatin1Char('>'));

            key = KeyParser::self()->encodeKeySequence(keyPress).at(0);
        }

        result.append(key);
    }

    return result;
}

bool LastChangeRecorder::isReplaying() const
{
    return m_isReplaying;
}

void LastChangeRecorder::replay(const QString &commands, const CompletionList &completions)
{
    m_isReplaying = true;
    m_viInputModeManager->completionReplayer()->start(completions);
    m_viInputModeManager->feedKeyPresses(commands);
    m_viInputModeManager->completionReplayer()->stop();
    m_isReplaying = false;
}
