/*  This file is part of the KDE libraries and the Kate part.
 *
 *  Copyright (C) 2008-2011 Erlend Hamberg <ehamberg@gmail.com>
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

#ifndef KATEVI_INSERT_VI_MODE_H
#define KATEVI_INSERT_VI_MODE_H

#include <modes/modebase.h>
#include <katevi/katevi_export.h>

class QKeyEvent;

namespace KateVi
{
class Motion;

/**
 * Commands for the vi insert mode
 */
enum BlockInsert {
    None,
    Prepend,
    Append,
    AppendEOL
};

class KATEVI_EXPORT InsertViMode : public ModeBase
{
    Q_OBJECT

public:
    explicit InsertViMode(InputModeManager *viInputModeManager,
                          KateViI::KateViEditorInterface *editorInterface);

    ~InsertViMode() override;

    bool handleKeyPress(const QKeyEvent *e) override;

    bool commandInsertFromAbove();
    bool commandInsertFromBelow();

    bool commandDeleteWord();
    bool commandDeleteLine();
    bool commandNewLine();
    bool commandDeleteCharBackward();

    bool commandIndent();
    bool commandUnindent();

    bool commandToFirstCharacterInFile();
    bool commandToLastCharacterInFile();

    bool commandMoveOneWordLeft();
    bool commandMoveOneWordRight();

    bool commandCompleteNext();
    bool commandCompletePrevious();

    bool commandInsertContentOfRegister();
    bool commandSwitchToNormalModeForJustOneCommand();

    void setBlockPrependMode(Range blockRange);
    void setBlockAppendMode(Range blockRange, BlockInsert b);

    void setCount(int count)
    {
        m_count = count;
    }

    void setCountedRepeatsBeginOnNewLine(bool countedRepeatsBeginOnNewLine)
    {
        m_countedRepeatsBeginOnNewLine = countedRepeatsBeginOnNewLine;
    }

protected:
    void leaveInsertMode(bool force = false);

    void completionFinished();

protected:
    BlockInsert m_blockInsert = None;

    // length of first line in eol mode before text is appended.
    unsigned int m_eolPos = 0;

    Range m_blockRange;

    QString m_keys;

    bool m_waitingRegister = false;

    unsigned int m_count = 1;

    bool m_countedRepeatsBeginOnNewLine = false;

    bool m_isExecutingCompletion = false;

    QString m_textInsertedByCompletion;

    KateViI::Cursor m_textInsertedByCompletionEndPos;

private Q_SLOTS:
    void textInserted(const KateViI::Range &p_range);
};

}

#endif /* KATEVI_INSERT_VI_MODE_H */
