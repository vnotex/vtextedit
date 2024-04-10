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

#ifndef KATEVI_EMULATED_COMMAND_BAR_COMMANDMODE_H
#define KATEVI_EMULATED_COMMAND_BAR_COMMANDMODE_H

#include "activemode.h"

#include <katevi/interface/command.h>
#include <katevi/interface/kcompletion.h>

#include <QHash>

namespace KateVi
{
    class EmulatedCommandBar;
    class MatchHighlighter;
    class InteractiveSedReplaceMode;
    class Completer;
    class InputModeManager;

    class CommandMode : public ActiveMode
    {
    public:
        CommandMode(EmulatedCommandBar* emulatedCommandBar,
                    MatchHighlighter* matchHighlighter,
                    InputModeManager* viInputModeManager,
                    QLineEdit* edit,
                    InteractiveSedReplaceMode *interactiveSedReplaceMode,
                    Completer* completer);

        bool handleKeyPress(const QKeyEvent* keyEvent) override;

        void editTextChanged(const QString &newText) override;

        CompletionStartParams completionInvoked(Completer::CompletionInvocation invocationType) override;

        void completionChosen() override;

        void deactivate(bool wasAborted) override;

        QString executeCommand(const QString &commandToExecute);

    private:
        // Stuff to do with expressions of the form:
        //   s/find/replace/<sedflags>
        struct ParsedSedExpression {
            bool parsedSuccessfully;
            int findBeginPos;
            int findEndPos;
            int replaceBeginPos;
            int replaceEndPos;
            QChar delimiter;
        };

#if 0
        CompletionStartParams activateCommandCompletion();
        CompletionStartParams activateCommandHistoryCompletion();
        CompletionStartParams activateSedFindHistoryCompletion();
        CompletionStartParams activateSedReplaceHistoryCompletion();
        QString withoutRangeExpression();
        QString rangeExpression();
        QString withSedFindTermReplacedWith(const QString &newFindTerm);
        QString withSedDelimiterEscaped(const QString &text);
        bool isCursorInFindTermOfSed();
        bool isCursorInReplaceTermOfSed();
        QString sedFindTerm();
        QString sedReplaceTerm();

        // The "range expression" is the (optional) expression before the command that describes
        // the range over which the command should be run e.g. '<,'>.  @see CommandRangeExpressionParser
        CommandMode::ParsedSedExpression parseAsSedExpression();

        void replaceCommandBeforeCursorWith(const QString &newCommand);

        int commandBeforeCursorBegin();

        KateViI::Command *queryCommand(const QString &cmd) const;
#endif

        QLineEdit *m_edit = nullptr;

        InteractiveSedReplaceMode *m_interactiveSedReplaceMode = nullptr;

        Completer *m_completer = nullptr;

        KateViI::KCompletion m_cmdCompletion;

        QHash<QString, KateViI::Command *> m_cmdDict;
    };
}

#endif
