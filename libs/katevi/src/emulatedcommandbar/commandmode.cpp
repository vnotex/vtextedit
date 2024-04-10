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

#include "commandmode.h"

#include <katevi/emulatedcommandbar.h>
// #include "interactivesedreplacemode.h"
// #include "searchmode.h"
// #include "../commandrangeexpressionparser.h"

// #include <vimode/appcommands.h>
#include <cmds.h>
#include <katevi/inputmodemanager.h>
#include <katevi/globalstate.h>
#include <katevi/global.h>
#include <history.h>

#include <QLineEdit>
#include <QRegularExpression>
#include <QWhatsThis>

using namespace KateVi;

CommandMode::CommandMode(EmulatedCommandBar* emulatedCommandBar,
                         MatchHighlighter* matchHighlighter,
                         InputModeManager* viInputModeManager,
                         QLineEdit* edit,
                         InteractiveSedReplaceMode* interactiveSedReplaceMode,
                         Completer* completer)
    : ActiveMode(emulatedCommandBar, matchHighlighter, viInputModeManager),
      m_edit(edit),
      m_interactiveSedReplaceMode(interactiveSedReplaceMode),
      m_completer(completer)
{
    QVector<KateViI::Command *> cmds;
    KATEVI_NIY;
    /*
    cmds.push_back(KateCommands::CoreCommands::self());
    cmds.push_back(Commands::self());
    cmds.push_back(AppCommands::self());
    cmds.push_back(SedReplace::self());
    cmds.push_back(BufferCommands::self());

    for (KTextEditor::Command *cmd : KateScriptManager::self()->commandLineScripts()) {
        cmds.push_back(cmd);
    }
    */

    for (KateViI::Command *cmd : qAsConst(cmds)) {
        QStringList l = cmd->cmds();

        for (int z = 0; z < l.count(); z++) {
            m_cmdDict.insert(l[z], cmd);
        }

        // m_cmdCompletion.insertItems(l);
    }
}

bool CommandMode::handleKeyPress(const QKeyEvent* keyEvent)
{
    if (keyEvent->modifiers() == Qt::ControlModifier
        && (keyEvent->key() == Qt::Key_D || keyEvent->key() == Qt::Key_F)) {
        KATEVI_NIY;
#if 0
        CommandMode::ParsedSedExpression parsedSedExpression = parseAsSedExpression();
        if (parsedSedExpression.parsedSuccessfully) {
            const bool clearFindTerm = (keyEvent->key() == Qt::Key_D);
            if (clearFindTerm) {
                m_edit->setSelection(parsedSedExpression.findBeginPos, parsedSedExpression.findEndPos - parsedSedExpression.findBeginPos + 1);
                m_edit->insert(QString());
            } else {
                // Clear replace term.
                m_edit->setSelection(parsedSedExpression.replaceBeginPos, parsedSedExpression.replaceEndPos - parsedSedExpression.replaceBeginPos + 1);
                m_edit->insert(QString());
            }
        }
        return true;
#endif
    }

    return false;
}

void CommandMode::editTextChanged(const QString& newText)
{
    KATEVI_NIY;
#if 0
    // We read the current text from m_edit.
    Q_UNUSED(newText);
    if (m_completer->isCompletionActive()) {
        return;
    }

    // Command completion doesn't need to be manually invoked.
    if (!withoutRangeExpression().isEmpty()
        && !m_completer->isNextTextChangeDueToCompletionChange()) {
        // ... However, command completion mode should not be automatically invoked if this is not the current leading
        // word in the text edit (it gets annoying if completion pops up after ":s/se" etc).
        const bool commandBeforeCursorIsLeading = (commandBeforeCursorBegin() == rangeExpression().length());
        if (commandBeforeCursorIsLeading) {
            CompletionStartParams completionStartParams = activateCommandCompletion();
            startCompletion(completionStartParams);
        }
    }
#endif
}

void CommandMode::deactivate(bool wasAborted)
{
    if (wasAborted) {
        // Appending the command to the history when it is executed is handled elsewhere; we can't
        // do it inside closed() as we may still be showing the command response display.
        viInputModeManager()->globalState()->commandHistory()->append(m_edit->text());
        // With Vim, aborting a command returns us to Normal mode, even if we were in Visual Mode.
        // If we switch from Visual to Normal mode, we need to clear the selection.
        editorInterface()->clearSelection();
    }

}

CompletionStartParams CommandMode::completionInvoked(Completer::CompletionInvocation invocationType)
{
    KATEVI_NIY;
#if 0
    CompletionStartParams completionStartParams;
    if (invocationType == Completer::CompletionInvocation::ExtraContext)
    {
        if (isCursorInFindTermOfSed()) {
            completionStartParams = activateSedFindHistoryCompletion();
        } else if (isCursorInReplaceTermOfSed()) {
            completionStartParams = activateSedReplaceHistoryCompletion();
        } else {
            completionStartParams = activateCommandHistoryCompletion();
        }
    }
    else
    {
        // Normal context, so boring, ordinary History completion.
        completionStartParams = activateCommandHistoryCompletion();
    }
    return completionStartParams;
#endif
    return CompletionStartParams();
}

void CommandMode::completionChosen()
{
    KATEVI_NIY;
#if 0
    QString commandToExecute = m_edit->text();
    CommandMode::ParsedSedExpression parsedSedExpression = parseAsSedExpression();
    if (parsedSedExpression.parsedSuccessfully) {
        const QString originalFindTerm = sedFindTerm();
        const QString convertedFindTerm = vimRegexToQtRegexPattern(originalFindTerm);
        const QString commandWithSedSearchRegexConverted = withSedFindTermReplacedWith(convertedFindTerm);
        viInputModeManager()->globalState()->searchHistory()->append(originalFindTerm);
        const QString replaceTerm = sedReplaceTerm();
        viInputModeManager()->globalState()->replaceHistory()->append(replaceTerm);
        commandToExecute = commandWithSedSearchRegexConverted;
    }

    const QString commandResponseMessage = executeCommand(commandToExecute);
    // Don't close the bar if executing the command switched us to Interactive Sed Replace mode.
    if (!m_interactiveSedReplaceMode->isActive()) {
        if (commandResponseMessage.isEmpty()) {
            emulatedCommandBar()->hideMe();
        } else {
            closeWithStatusMessage(commandResponseMessage);
        }
    }
    viInputModeManager()->globalState()->commandHistory()->append(m_edit->text());
#endif
}

QString CommandMode::executeCommand(const QString& commandToExecute)
{
    KATEVI_NIY;
#if 0
    // Silently ignore leading space characters and colon characters (for vi-heads).
    uint n = 0;
    const uint textlen = commandToExecute.length();
    while ((n < textlen) && commandToExecute[n].isSpace()) {
        n++;
    }

    if (n >= textlen) {
        return QString();
    }

    QString commandResponseMessage;
    QString cmd = commandToExecute.mid(n);

    KTextEditor::Range range = CommandRangeExpressionParser(viInputModeManager()).parseRange(cmd, cmd);

    if (cmd.length() > 0) {
        KTextEditor::Command *p = queryCommand(cmd);
        if (p) {
            KateViCommandInterface *ci = dynamic_cast<KateViCommandInterface*>(p);
            if (ci) {
                ci->setViInputModeManager(viInputModeManager());
                ci->setViGlobal(viInputModeManager()->globalState());
            }

            // The following commands changes the focus themselves, so bar should be hidden before execution.

            // We got a range and a valid command, but the command does not support ranges.
            if (range.isValid() && !p->supportsRange(cmd)) {
                commandResponseMessage = i18n("Error: No range allowed for command \"%1\".",  cmd);
            } else {
                if (p->exec(view(), cmd, commandResponseMessage, range)) {

                    if (commandResponseMessage.length() > 0) {
                        commandResponseMessage = i18n("Success: ") + commandResponseMessage;
                    }
                } else {
                    if (commandResponseMessage.length() > 0) {
                        if (commandResponseMessage.contains(QLatin1Char('\n'))) {
                            // multiline error, use widget with more space
                            QWhatsThis::showText(emulatedCommandBar()->mapToGlobal(QPoint(0, 0)), commandResponseMessage);
                        }
                    } else {
                        commandResponseMessage = i18n("Command \"%1\" failed.",  cmd);
                    }
                }
            }
        } else {
            commandResponseMessage = i18n("No such command: \"%1\"",  cmd);
        }
    }

    // the following commands change the focus themselves
    static const QRegularExpression reCmds(QStringLiteral("^(buffer|b|new|vnew|bp|bprev|tabp|tabprev|bn|bnext|tabn|tabnext|bf|bfirst|tabf|tabfirst|bl|blast|tabl|tablast|e|edit|tabe|tabedit|tabnew)$"));
    if (!reCmds.match(cmd.leftRef(cmd.indexOf(QLatin1Char(' ')))).hasMatch()) {
        view()->setFocus();
    }

    viInputModeManager()->reset();
    return commandResponseMessage;
#endif
    return "";
}

#if 0
QString CommandMode::withoutRangeExpression()
{
    const QString originalCommand = m_edit->text();
    return originalCommand.mid(rangeExpression().length());
}

QString CommandMode::rangeExpression()
{
    const QString command = m_edit->text();
    return CommandRangeExpressionParser(viInputModeManager()).parseRangeString(command);
}

CommandMode::ParsedSedExpression CommandMode::parseAsSedExpression()
{
    const QString commandWithoutRangeExpression = withoutRangeExpression();
    ParsedSedExpression parsedSedExpression;
    QString delimiter;
    parsedSedExpression.parsedSuccessfully = SedReplace::parse(commandWithoutRangeExpression, delimiter, parsedSedExpression.findBeginPos, parsedSedExpression.findEndPos, parsedSedExpression.replaceBeginPos, parsedSedExpression.replaceEndPos);
    if (parsedSedExpression.parsedSuccessfully) {
        parsedSedExpression.delimiter = delimiter.at(0);
        if (parsedSedExpression.replaceBeginPos == -1) {
            if (parsedSedExpression.findBeginPos != -1) {
                // The replace term was empty, and a quirk of the regex used is that replaceBeginPos will be -1.
                // It's actually the position after the first occurrence of the delimiter after the end of the find pos.
                parsedSedExpression.replaceBeginPos = commandWithoutRangeExpression.indexOf(delimiter, parsedSedExpression.findEndPos) + 1;
                parsedSedExpression.replaceEndPos = parsedSedExpression.replaceBeginPos - 1;
            } else {
                // Both find and replace terms are empty; replace term is at the third occurrence of the delimiter.
                parsedSedExpression.replaceBeginPos = 0;
                for (int delimiterCount = 1; delimiterCount <= 3; delimiterCount++) {
                    parsedSedExpression.replaceBeginPos = commandWithoutRangeExpression.indexOf(delimiter, parsedSedExpression.replaceBeginPos + 1);
                }
                parsedSedExpression.replaceEndPos = parsedSedExpression.replaceBeginPos - 1;
            }
        }
        if (parsedSedExpression.findBeginPos == -1) {
            // The find term was empty, and a quirk of the regex used is that findBeginPos will be -1.
            // It's actually the position after the first occurrence of the delimiter.
            parsedSedExpression.findBeginPos = commandWithoutRangeExpression.indexOf(delimiter) + 1;
            parsedSedExpression.findEndPos = parsedSedExpression.findBeginPos - 1;
        }

    }

    if (parsedSedExpression.parsedSuccessfully) {
        parsedSedExpression.findBeginPos += rangeExpression().length();
        parsedSedExpression.findEndPos += rangeExpression().length();
        parsedSedExpression.replaceBeginPos += rangeExpression().length();
        parsedSedExpression.replaceEndPos += rangeExpression().length();
    }
    return parsedSedExpression;

}

QString CommandMode::sedFindTerm()
{
    const QString command = m_edit->text();
    ParsedSedExpression parsedSedExpression = parseAsSedExpression();
    Q_ASSERT(parsedSedExpression.parsedSuccessfully);
    return command.mid(parsedSedExpression.findBeginPos, parsedSedExpression.findEndPos - parsedSedExpression.findBeginPos + 1);
}

QString CommandMode::sedReplaceTerm()
{
    const QString command = m_edit->text();
    ParsedSedExpression parsedSedExpression = parseAsSedExpression();
    Q_ASSERT(parsedSedExpression.parsedSuccessfully);
    return command.mid(parsedSedExpression.replaceBeginPos, parsedSedExpression.replaceEndPos - parsedSedExpression.replaceBeginPos + 1);
}

QString CommandMode::withSedFindTermReplacedWith ( const QString& newFindTerm )
{
    const QString command = m_edit->text();
    ParsedSedExpression parsedSedExpression = parseAsSedExpression();
    Q_ASSERT(parsedSedExpression.parsedSuccessfully);
    return command.midRef(0, parsedSedExpression.findBeginPos) +
    newFindTerm +
    command.midRef(parsedSedExpression.findEndPos + 1);
}

QString CommandMode::withSedDelimiterEscaped ( const QString& text )
{
    ParsedSedExpression parsedSedExpression = parseAsSedExpression();
    QString delimiterEscaped = ensuredCharEscaped(text, parsedSedExpression.delimiter);
    return delimiterEscaped;
}

bool CommandMode::isCursorInFindTermOfSed()
{
    ParsedSedExpression parsedSedExpression = parseAsSedExpression();
    return parsedSedExpression.parsedSuccessfully && (m_edit->cursorPosition() >= parsedSedExpression.findBeginPos && m_edit->cursorPosition() <= parsedSedExpression.findEndPos + 1);
}

bool CommandMode::isCursorInReplaceTermOfSed()
{
    ParsedSedExpression parsedSedExpression = parseAsSedExpression();
    return parsedSedExpression.parsedSuccessfully && m_edit->cursorPosition() >= parsedSedExpression.replaceBeginPos && m_edit->cursorPosition() <= parsedSedExpression.replaceEndPos + 1;
}

int CommandMode::commandBeforeCursorBegin()
{
    const QString textWithoutRangeExpression = withoutRangeExpression();
    const int cursorPositionWithoutRangeExpression = m_edit->cursorPosition() - rangeExpression().length();
    int commandBeforeCursorBegin = cursorPositionWithoutRangeExpression - 1;
    while (commandBeforeCursorBegin >= 0 && (textWithoutRangeExpression[commandBeforeCursorBegin].isLetterOrNumber() || textWithoutRangeExpression[commandBeforeCursorBegin] == QLatin1Char('_') || textWithoutRangeExpression[commandBeforeCursorBegin] == QLatin1Char('-'))) {
        commandBeforeCursorBegin--;
    }
    commandBeforeCursorBegin++;
    commandBeforeCursorBegin += rangeExpression().length();
    return commandBeforeCursorBegin;
}

CompletionStartParams CommandMode::activateCommandCompletion()
{
    return CompletionStartParams::createModeSpecific(m_cmdCompletion.items(), commandBeforeCursorBegin());
}

CompletionStartParams CommandMode::activateCommandHistoryCompletion()
{
    return CompletionStartParams::createModeSpecific(reversed(viInputModeManager()->globalState()->commandHistory()->items()), 0);
}

CompletionStartParams CommandMode::activateSedFindHistoryCompletion()
{
    if (viInputModeManager()->globalState()->searchHistory()->isEmpty())
    {
        return CompletionStartParams::invalid();
    }
    CommandMode::ParsedSedExpression parsedSedExpression = parseAsSedExpression();
    return CompletionStartParams::createModeSpecific(reversed(viInputModeManager()->globalState()->searchHistory()->items()),
                                                     parsedSedExpression.findBeginPos,
                                                     [this] (const  QString& completion) -> QString { return withCaseSensitivityMarkersStripped(withSedDelimiterEscaped(completion)); });
}

CompletionStartParams CommandMode::activateSedReplaceHistoryCompletion()
{
    if (viInputModeManager()->globalState()->replaceHistory()->isEmpty())
    {
        return CompletionStartParams::invalid();
    }
    CommandMode::ParsedSedExpression parsedSedExpression = parseAsSedExpression();
    return CompletionStartParams::createModeSpecific(reversed(viInputModeManager()->globalState()->replaceHistory()->items()),
                                                     parsedSedExpression.replaceBeginPos,
                                                     [this] (const  QString& completion) -> QString { return withCaseSensitivityMarkersStripped(withSedDelimiterEscaped(completion)); });
}

KTextEditor::Command* CommandMode::queryCommand ( const QString& cmd ) const
{
    // a command can be named ".*[\w\-]+" with the constrain that it must
    // contain at least one letter.
    int f = 0;
    bool b = false;

    // special case: '-' and '_' can be part of a command name, but if the
    // command is 's' (substitute), it should be considered the delimiter and
    // should not be counted as part of the command name
    if (cmd.length() >= 2 && cmd.at(0) == QLatin1Char('s') && (cmd.at(1) == QLatin1Char('-') || cmd.at(1) == QLatin1Char('_'))) {
        return m_cmdDict.value(QStringLiteral("s"));
    }

    for (; f < cmd.length(); f++) {
        if (cmd[f].isLetter()) {
            b = true;
        }
        if (b && (! cmd[f].isLetterOrNumber() && cmd[f] != QLatin1Char('-') && cmd[f] != QLatin1Char('_'))) {
            break;
        }
    }
    return m_cmdDict.value(cmd.left(f));

}
#endif
