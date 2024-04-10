/*  This file is part of the KDE libraries and the Kate part.
 *
 *  Copyright (C) 2008 Erlend Hamberg <ehamberg@gmail.com>
 *  Copyright (C) 2011 Svyatoslav Kuzmich <svatoslav1@gmail.com>
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

#ifndef KATEVI_COMMAND_H
#define KATEVI_COMMAND_H

#include <QString>

#include <modes/normalvimode.h>

namespace KateVi
{
class KeyParser;

enum CommandFlags {
    REGEX_PATTERN = 0x1,    // the pattern is a regex
    NEEDS_MOTION = 0x2,     // the command needs a motion before it can be executed
    SHOULD_NOT_RESET = 0x4, // the command should not cause the current mode to be left
    IS_CHANGE = 0x8,        // the command changes the buffer
    IS_NOT_LINEWISE = 0x10, // the motion is not line wise
    CAN_CHANGE_WHOLE_VISUAL_MODE_SELECTION = 0x20   // the motion is a text object that can set the
            // whole Visual Mode selection to the text object
};

class Command
{
public:
    Command(NormalViMode *parent, QString pattern,
            bool (NormalViMode::*pt2Func)(), unsigned int flags = 0);
    ~Command();

    bool matches(const QString &pattern) const;
    bool matchesExact(const QString &pattern) const;
    bool execute() const;
    const QString pattern() const
    {
        return m_pattern;
    }
    bool isRegexPattern() const
    {
        return m_flags & REGEX_PATTERN;
    }
    bool needsMotion() const
    {
        return m_flags & NEEDS_MOTION;
    }
    bool shouldReset() const
    {
        return !(m_flags & SHOULD_NOT_RESET);
    }
    bool isChange() const
    {
        return m_flags & IS_CHANGE;
    }
    bool isLineWise() const
    {
        return !(m_flags & IS_NOT_LINEWISE);
    }
    bool canChangeWholeVisualModeSelection() const
    {
        return m_flags & CAN_CHANGE_WHOLE_VISUAL_MODE_SELECTION;
    }

protected:
    NormalViMode *m_parent;
    QString m_pattern;
    unsigned int m_flags;
    bool (NormalViMode::*m_ptr2commandMethod)();
    KeyParser *m_keyParser;
};

}

#endif /* KATEVI_COMMAND_H */
