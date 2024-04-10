/*  This file is part of the KDE libraries and the Kate part.
 *
 *  Copyright (C) 2008 Erlend Hamberg <ehamberg@gmail.com>
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

#include <katevi/interface/kateviinputmode.h>

#include <modes/visualvimode.h>
#include <katevi/inputmodemanager.h>
#include <katevi/global.h>
#include <range.h>
#include <motion.h>
#include <marks.h>

#include <QDebug>

using namespace KateVi;

#define ADDCMD(STR,FUNC, FLGS) m_commands.push_back( \
        new Command( this, QStringLiteral(STR), &NormalViMode::FUNC, FLGS ) );

#define ADDMOTION(STR, FUNC, FLGS) m_motions.push_back( new \
        Motion( this, QStringLiteral(STR), &NormalViMode::FUNC, FLGS ) );

VisualViMode::VisualViMode(InputModeManager *viInputModeManager,
                           KateViI::KateViEditorInterface *editorInterface)
    : NormalViMode(viInputModeManager, editorInterface)
{
    initializeCommands();

    m_interface->connectSelectionChanged([this]() {
        updateSelection();
    });
}

VisualViMode::~VisualViMode()
{
}

void VisualViMode::selectInclusive(const KateViI::Cursor &c1,
                                   const KateViI::Cursor &c2)
{
    if (c1 >= c2) {
        m_interface->setSelection(KateViI::Range(c1.line(), c1.column() + 1,
                                                 c2.line(), c2.column()));
    } else {
        m_interface->setSelection(KateViI::Range(c1.line(), c1.column(),
                                                 c2.line(), c2.column() + 1));
    }
}

void VisualViMode::selectBlockInclusive(const KateViI::Cursor &c1,
                                        const KateViI::Cursor &c2)
{
    m_interface->setBlockSelection(true);

    if (c1.column() >= c2.column()) {
        m_interface->setSelection(KateViI::Range(c1.line(), c1.column() + 1,
                                                 c2.line(), c2.column()));
    } else {
        m_interface->setSelection(KateViI::Range(c1.line(), c1.column(),
                                                 c2.line(), c2.column() + 1));
    }
}

void VisualViMode::selectLines(const KateViI::Range &range)
{
    int sline = qMin(range.start().line(), range.end().line());
    int eline = qMax(range.start().line(), range.end().line());
    // FIXME: there is no need to add 1.
    int ecol = m_interface->lineLength(eline);

    m_interface->setSelection(KateViI::Range(KateViI::Cursor(sline, 0),
                              KateViI::Cursor(eline, ecol)));
}

void VisualViMode::goToPos(const Range &r)
{
    KateViI::Cursor c = m_interface->cursorPosition();

    if (r.startLine != -1 && r.startColumn != -1 && c == m_start) {
        m_start.setLine(r.startLine);
        m_start.setColumn(r.startColumn);
        c.setLine(r.endLine);
        c.setColumn(r.endColumn);
    } else if (r.startLine != -1 && r.startColumn != -1 && m_motionCanChangeWholeVisualModeSelection) {
        const KateViI::Cursor textObjectBegin(r.startLine, r.startColumn);
        if (textObjectBegin < m_start) {
            m_start.setLine(r.startLine);
            m_start.setColumn(r.startColumn);
            c.setLine(r.endLine);
            c.setColumn(r.endColumn);
        }
    } else {
        c.setLine(r.endLine);
        c.setColumn(r.endColumn);
    }

    if (c.line() >= m_interface->lines()) {
        c.setLine(m_interface->lines() - 1);
    }

    updateCursor(c);

    // Setting range for a command
    m_commandRange = Range(m_start, c, m_commandRange.motionType);

    // If visual mode is blockwise
    if (isVisualBlock()) {
        selectBlockInclusive(m_start, c);

        // Need to correct command range to make it inclusive.
        if ((c.line() < m_start.line() && c.column() > m_start.column()) ||
            (c.line() > m_start.line() && c.column() < m_start.column())) {

            qSwap(m_commandRange.endColumn, m_commandRange.startColumn);
        }
        return;
    } else {
        m_interface->setBlockSelection(false);
    }

    // If visual mode is linewise
    if (isVisualLine()) {
        selectLines(KateViI::Range(m_start, c));
        return;
    }

    // If visual mode is charwise
    selectInclusive(m_start, c);
}

void VisualViMode::reset()
{
    m_mode = ViMode::VisualMode;

    // only switch to normal mode if still in visual mode. commands like c, s, ...
    // can have switched to insert mode
    if (m_viInputModeManager->isAnyVisualMode()) {
        saveRangeMarks();
        m_lastVisualMode = m_viInputModeManager->getCurrentViMode();

        // Return the cursor back to start of selection after.
        if (!m_pendingResetIsDueToExit) {
            KateViI::Cursor c = m_interface->cursorPosition();
            if (m_start.line() != -1 && m_start.column() != -1) {
                if (m_viInputModeManager->getCurrentViMode() == ViMode::VisualLineMode) {
                    if (m_start.line() < c.line()) {
                        updateCursor(KateViI::Cursor(m_start.line(), 0));
                        m_stickyColumn = -1;
                    }
                } else {
                    updateCursor(qMin(m_start, c));
                    m_stickyColumn = -1;
                }
            }
        }

        if (m_viInputModeManager->getPreviousViMode() == ViMode::InsertMode) {
            startInsertMode();
        } else {
            startNormalMode();
        }
    } else {
        bool blinkCursor = m_viInputModeManager->getCurrentViMode() == ViMode::InsertMode;
        m_viInputModeManager->inputAdapter()->setCursorBlinkingEnabled(blinkCursor);
    }

    if (!m_commandShouldKeepSelection) {
        m_interface->removeSelection();
    } else {
        m_commandShouldKeepSelection = false;
    }

    m_start.setPosition(-1, -1);
    m_pendingResetIsDueToExit = false;
}

void VisualViMode::saveRangeMarks()
{
    // DO NOT save these marks if the
    // action that exited visual mode deleted the selection
    if (m_deleteCommand == false) {
        m_viInputModeManager->marks()->setSelectionStart(m_start);
        m_viInputModeManager->marks()->setSelectionFinish(m_interface->cursorPosition());
    }
}

void VisualViMode::init()
{
    // when using "gv" we already have a start position
    if (!m_start.isValid()) {
        m_start = m_interface->cursorPosition();
    }

    if (isVisualLine()) {
        KateViI::Cursor c = m_interface->cursorPosition();
        selectLines(KateViI::Range(c, c));
    } else if (!m_interface->selection()) {
        // Select current char.
        selectInclusive(m_start, m_start);
    }

    m_commandRange = Range(m_start, m_start, m_commandRange.motionType);
}

void VisualViMode::setVisualModeType(ViMode mode)
{
    Q_ASSERT(mode == ViMode::VisualMode ||
             mode == ViMode::VisualLineMode ||
             mode == ViMode::VisualBlockMode);
    m_mode = mode;
}

void VisualViMode::switchStartEnd()
{
    KateViI::Cursor c = m_start;
    m_start = m_interface->cursorPosition();

    updateCursor(c);

    m_stickyColumn = -1;
}

void VisualViMode::goToPos(const KateViI::Cursor &c)
{
    Range r(c, InclusiveMotion);
    goToPos(r);
}

void VisualViMode::updateSelection()
{
    if (!m_viInputModeManager->inputAdapter()->isActive()) {
        return;
    }
    if (m_viInputModeManager->isHandlingKeyPress() && !m_isUndo) {
        return;
    }

    // If we are there it's already not VisualBlock mode.
    m_interface->setBlockSelection(false);

    // If not valid going back to normal mode
    KateViI::Range r = m_interface->selectionRange();
    if (!r.isValid()) {
        // Don't screw up the cursor's position. See BUG #337286.
        m_pendingResetIsDueToExit = true;
        reset();
        return;
    }

    // If already not in visual mode, it's time to go there.
    if (m_viInputModeManager->getCurrentViMode() != ViMode::VisualMode) {
        commandEnterVisualMode();
    }

    // Need to clear the overridden selection.
    m_interface->clearOverriddenSelection();

    // Set range for commands
    m_start = (m_interface->cursorPosition() == r.start()) ? r.end() : r.start();
    m_commandRange = Range(r.start(), r.end(), m_commandRange.motionType);

    // The end of the range seems to be one space forward of where it should be.
    m_commandRange.endColumn--;

    // Blink the cursor to indicate that the char under cursor is not selected.
    m_viInputModeManager->inputAdapter()->setCursorBlinkingEnabled(true);
}

void VisualViMode::initializeCommands()
{
    // Remove the commands put in here by NormalMode
    qDeleteAll(m_commands);
    m_commands.clear();

    // Remove the motions put in here by NormalMode
    qDeleteAll(m_motions);
    m_motions.clear();

    ADDCMD("J", commandJoinLines, IS_CHANGE);
    ADDCMD("c", commandChange, IS_CHANGE);
    ADDCMD("s", commandChange, IS_CHANGE);
    ADDCMD("C", commandChangeToEOL, IS_CHANGE);
    ADDCMD("S", commandChangeToEOL, IS_CHANGE);
    ADDCMD("d", commandDelete, IS_CHANGE);
    ADDCMD("<delete>", commandDelete, IS_CHANGE);
    ADDCMD("D", commandDeleteToEOL, IS_CHANGE);
    ADDCMD("x", commandDeleteChar, IS_CHANGE);
    ADDCMD("X", commandDeleteCharBackward, IS_CHANGE);
    ADDCMD("gu", commandMakeLowercase, IS_CHANGE);
    ADDCMD("u", commandMakeLowercase, IS_CHANGE);
    ADDCMD("gU", commandMakeUppercase, IS_CHANGE);
    ADDCMD("g~", commandChangeCaseRange, IS_CHANGE);
    ADDCMD("U", commandMakeUppercase, IS_CHANGE);
    ADDCMD("y", commandYank, 0);
    ADDCMD("Y", commandYankToEOL, 0);
    ADDCMD("p", commandPaste, IS_CHANGE);
    ADDCMD("P", commandPasteBefore, IS_CHANGE);
    ADDCMD("r.", commandReplaceCharacter, IS_CHANGE | REGEX_PATTERN);
    ADDCMD(":", commandSwitchToCmdLine, SHOULD_NOT_RESET);
    ADDCMD("m.", commandSetMark, REGEX_PATTERN | SHOULD_NOT_RESET);
    ADDCMD(">", commandIndentLines, IS_CHANGE);
    ADDCMD("<", commandUnindentLines, IS_CHANGE);
    ADDCMD("<c-c>", commandAbort, 0);
    ADDCMD("<c-[>", commandAbort, 0);
    /*
    ADDCMD("ga", commandPrintCharacterCode, SHOULD_NOT_RESET);
    */
    ADDCMD("v", commandEnterVisualMode, SHOULD_NOT_RESET);
    ADDCMD("V", commandEnterVisualLineMode, SHOULD_NOT_RESET);
    ADDCMD("o", commandToOtherEnd, SHOULD_NOT_RESET);
    ADDCMD("=", commandAlignLines, SHOULD_NOT_RESET);
    ADDCMD("~", commandChangeCase, IS_CHANGE);
    ADDCMD("I", commandPrependToBlock, IS_CHANGE);
    ADDCMD("A", commandAppendToBlock, IS_CHANGE);
    // ADDCMD("gq", commandFormatLines, IS_CHANGE);
    ADDCMD("q.", commandStartRecordingMacro, REGEX_PATTERN | SHOULD_NOT_RESET);
    ADDCMD("@.", commandReplayMacro, REGEX_PATTERN | SHOULD_NOT_RESET);
    // Different than Vim: When requesting to go to first non blank position,
    // will reset to Normal mode.
    ADDCMD("z.", commandCenterViewOnNonBlank, 0);
    ADDCMD("zz", commandCenterViewOnCursor, SHOULD_NOT_RESET);
    ADDCMD("z<return>", commandTopViewOnNonBlank, 0);
    ADDCMD("zt", commandTopViewOnCursor, SHOULD_NOT_RESET);
    ADDCMD("z-", commandBottomViewOnNonBlank, 0);
    ADDCMD("zb", commandBottomViewOnCursor, SHOULD_NOT_RESET);

    // regular motions
    ADDMOTION("h", motionLeft, 0);
    ADDMOTION("<left>", motionLeft, 0);
    ADDMOTION("<backspace>", motionLeft, 0);
    ADDMOTION("j", motionDown, 0);
    ADDMOTION("<down>", motionDown, 0);
    ADDMOTION("k", motionUp, 0);
    ADDMOTION("<up>", motionUp, 0);
    ADDMOTION("l", motionRight, 0);
    ADDMOTION("<right>", motionRight, 0);
    ADDMOTION(" ", motionRight, 0);
    ADDMOTION("$", motionToEOL, 0);
    ADDMOTION("<end>", motionToEOL, 0);
    ADDMOTION("0", motionToColumn0, 0);
    ADDMOTION("<home>", motionToColumn0, 0);
    ADDMOTION("^", motionToFirstCharacterOfLine, 0);
    ADDMOTION("f.", motionFindChar, REGEX_PATTERN);
    ADDMOTION("F.", motionFindCharBackward, REGEX_PATTERN);
    ADDMOTION("t.", motionToChar, REGEX_PATTERN);
    ADDMOTION("T.", motionToCharBackward, REGEX_PATTERN);
    ADDMOTION(";", motionRepeatlastTF, 0);
    ADDMOTION(",", motionRepeatlastTFBackward, 0);
    /*
    ADDMOTION("n", motionFindNext, 0);
    ADDMOTION("N", motionFindPrev, 0);
    */
    ADDMOTION("gg", motionToLineFirst, 0);
    ADDMOTION("G", motionToLineLast, 0);
    ADDMOTION("w", motionWordForward, 0);
    ADDMOTION("W", motionWORDForward, 0);
    ADDMOTION("<c-right>", motionWordForward, IS_NOT_LINEWISE);
    ADDMOTION("<c-left>", motionWordBackward, IS_NOT_LINEWISE);
    ADDMOTION("b", motionWordBackward, 0);
    ADDMOTION("B", motionWORDBackward, 0);
    ADDMOTION("e", motionToEndOfWord, 0);
    ADDMOTION("E", motionToEndOfWORD, 0);
    ADDMOTION("ge", motionToEndOfPrevWord, 0);
    ADDMOTION("gE", motionToEndOfPrevWORD, 0);
    ADDMOTION("|", motionToScreenColumn, 0);
    ADDMOTION("%", motionToMatchingItem, 0);
    /*
    ADDMOTION("`.", motionToMark, REGEX_PATTERN);
    ADDMOTION("'.", motionToMarkLine, REGEX_PATTERN);
    */
    ADDMOTION("[[", motionToPreviousBraceBlockStart, 0);
    ADDMOTION("]]", motionToNextBraceBlockStart, 0);
    ADDMOTION("[]", motionToPreviousBraceBlockEnd, 0);
    ADDMOTION("][", motionToNextBraceBlockEnd, 0);
    /*
    ADDMOTION("*", motionToNextOccurrence, 0);
    ADDMOTION("#", motionToPrevOccurrence, 0);
    */
    ADDMOTION("<c-f>", motionPageDown, 0);
    ADDMOTION("<pagedown>", motionPageDown, 0);
    ADDMOTION("<c-b>", motionPageUp, 0);
    ADDMOTION("<pageup>", motionPageUp, 0);
    ADDMOTION("gj", motionToNextVisualLine, 0);
    ADDMOTION("gk", motionToPrevVisualLine, 0);
    ADDMOTION("(", motionToPreviousSentence, 0);
    ADDMOTION(")", motionToNextSentence, 0);
    ADDMOTION("{", motionToBeforeParagraph, 0);
    ADDMOTION("}", motionToAfterParagraph, 0);
    ADDMOTION("<c-u>", motionHalfPageUp, 0);
    ADDMOTION("<c-d>", motionHalfPageDown, 0);

    // text objects
    // It seems that there is no need for text objects in visual mode.
#if 0
    ADDMOTION("iw", textObjectInnerWord, 0);
    ADDMOTION("aw", textObjectAWord, 0);
    ADDMOTION("iW", textObjectInnerWORD, 0);
    ADDMOTION("aW", textObjectAWORD, IS_NOT_LINEWISE);
    ADDMOTION("is", textObjectInnerSentence, IS_NOT_LINEWISE);
    ADDMOTION("as", textObjectASentence, IS_NOT_LINEWISE);
    ADDMOTION("ip", textObjectInnerParagraph, IS_NOT_LINEWISE);
    ADDMOTION("ap", textObjectAParagraph, IS_NOT_LINEWISE);
    ADDMOTION("i\"", textObjectInnerQuoteDouble, CAN_CHANGE_WHOLE_VISUAL_MODE_SELECTION);
    ADDMOTION("a\"", textObjectAQuoteDouble, 0);
    ADDMOTION("i'", textObjectInnerQuoteSingle, CAN_CHANGE_WHOLE_VISUAL_MODE_SELECTION);
    ADDMOTION("a'", textObjectAQuoteSingle, 0);
    ADDMOTION("i[()b]", textObjectInnerParen, REGEX_PATTERN | CAN_CHANGE_WHOLE_VISUAL_MODE_SELECTION);
    ADDMOTION("a[()b]", textObjectAParen, REGEX_PATTERN);
    ADDMOTION("i[{}B]", textObjectInnerCurlyBracket, REGEX_PATTERN | IS_NOT_LINEWISE | CAN_CHANGE_WHOLE_VISUAL_MODE_SELECTION);
    ADDMOTION("a[{}B]", textObjectACurlyBracket, REGEX_PATTERN | IS_NOT_LINEWISE);
    ADDMOTION("i[><]", textObjectInnerInequalitySign, REGEX_PATTERN | IS_NOT_LINEWISE | CAN_CHANGE_WHOLE_VISUAL_MODE_SELECTION);
    ADDMOTION("i[\\[\\]]", textObjectInnerBracket, REGEX_PATTERN | CAN_CHANGE_WHOLE_VISUAL_MODE_SELECTION);
    ADDMOTION("a[\\[\\]]", textObjectABracket, REGEX_PATTERN);
    ADDMOTION("i,", textObjectInnerComma, 0);
    ADDMOTION("a,", textObjectAComma, 0);
#endif

    /*
    ADDMOTION("/<enter>", motionToIncrementalSearchMatch, 0);
    ADDMOTION("?<enter>", motionToIncrementalSearchMatch, 0);
    */
}
