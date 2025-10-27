/*  This file is part of the KDE libraries and the Kate part.
 *
 *  Copyright (C) 2008 - 2009 Erlend Hamberg <ehamberg@gmail.com>
 *  Copyright (C) 2009 Paul Gideon Dann <pdgiddie@gmail.com>
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

#ifndef KATEVI_NORMAL_VI_MODE_H
#define KATEVI_NORMAL_VI_MODE_H

#include <modes/modebase.h>
#include <range.h>

#include <QHash>
#include <QRegularExpression>
#include <QStack>
#include <QVector>

#include <katevi/interface/range.h>
#include <katevi/katevi_export.h>

class QKeyEvent;

namespace KateViI {
class KateViInputMode;
}

namespace KateVi {
class Command;
class Motion;
class KeyParser;
class InputModeManager;

/**
 * Commands for the vi normal mode
 */
class KATEVI_EXPORT NormalViMode : public ModeBase {
  Q_OBJECT

  friend KateViI::KateViInputMode;

public:
  explicit NormalViMode(InputModeManager *viInputModeManager,
                        KateViI::KateViEditorInterface *editorInterface);

  ~NormalViMode() override;

  bool handleKeyPress(const QKeyEvent *e) override;

  bool commandEnterVisualMode();

  bool commandEnterInsertMode();
  bool commandEnterInsertModeAppend();
  bool commandEnterInsertModeAppendEOL();
  bool commandEnterInsertModeBeforeFirstNonBlankInLine();
  bool commandEnterInsertModeLast();

#if 0
    bool commandEnterVisualBlockMode();
    bool commandReselectVisual();
#endif

  bool commandEnterVisualLineMode();

  bool commandToOtherEnd();

  bool commandEnterReplaceMode();

  bool commandDelete();
  bool commandDeleteToEOL();
  bool commandDeleteLine();

  bool commandMakeLowercase();
  bool commandMakeLowercaseLine();
  bool commandMakeUppercase();
  bool commandMakeUppercaseLine();
  bool commandChangeCase();
  bool commandChangeCaseRange();
  bool commandChangeCaseLine();

  bool commandOpenNewLineUnder();
  bool commandOpenNewLineOver();

  bool commandJoinLines();

  bool commandChange();

  bool commandYank();
  bool commandYankLine();
  bool commandYankToEOL();

  bool commandPaste();
  bool commandPasteBefore();
  bool commandgPaste();
  bool commandgPasteBefore();

  bool commandIndentedPaste();
  bool commandIndentedPasteBefore();

  bool commandChangeLine();
  bool commandChangeToEOL();

  bool commandSubstituteChar();
  bool commandSubstituteLine();

  bool commandDeleteChar();
  bool commandDeleteCharBackward();

  bool commandReplaceCharacter();

  bool commandSwitchToCmdLine();
#if 0
    bool commandSearchBackward();
    bool commandSearchForward();
#endif

  bool commandUndo();
  bool commandRedo();

  bool commandSetMark();

  bool commandIndentLine();
  bool commandUnindentLine();
  bool commandIndentLines();
  bool commandUnindentLines();

  bool commandScrollPageDown();
  bool commandScrollPageUp();
  bool commandScrollHalfPageUp();
  bool commandScrollHalfPageDown();

  bool commandCenterView(bool onFirst);
  bool commandCenterViewOnNonBlank();
  bool commandCenterViewOnCursor();
  bool commandTopView(bool onFirst);
  bool commandTopViewOnNonBlank();
  bool commandTopViewOnCursor();
  bool commandBottomView(bool onFirst);
  bool commandBottomViewOnNonBlank();
  bool commandBottomViewOnCursor();

  bool commandAbort();

#if 0
    bool commandPrintCharacterCode();
#endif

  bool commandRepeatLastChange();

  bool commandAlignLine();
  bool commandAlignLines();

  bool commandAddToNumber();
  bool commandSubtractFromNumber();

  bool commandGoToNextJump();
  bool commandGoToPrevJump();

  bool commandPrependToBlock();
  bool commandAppendToBlock();

  bool commandSwitchToLeftView();
  bool commandSwitchToUpView();
  bool commandSwitchToDownView();
  bool commandSwitchToRightView();
  bool commandSwitchToNextView();

  bool commandSplitHoriz();
  bool commandSplitVert();
  bool commandCloseView();

  bool commandSwitchToNextTab();
  bool commandSwitchToPrevTab();

#if 0
    bool commandFormatLine();
    bool commandFormatLines();

    bool commandCollapseToplevelNodes();
    bool commandCollapseLocal();
    bool commandExpandAll();
    bool commandExpandLocal();
    bool commandToggleRegionVisibility();
#endif

  bool commandStartRecordingMacro();
  bool commandReplayMacro();

  bool commandCloseWrite();
  bool commandCloseNocheck();

  // MOTIONS
  Range motionLeft();
  Range motionDown();
  Range motionRight();
  Range motionUp();

  Range motionUpToFirstNonBlank();
  Range motionDownToFirstNonBlank();

  Range motionWordForward();
  Range motionWordBackward();
  Range motionWORDForward();
  Range motionWORDBackward();

  Range motionPageDown();
  Range motionPageUp();
  Range motionHalfPageDown();
  Range motionHalfPageUp();

  Range motionToEndOfWord();
  Range motionToEndOfWORD();
  Range motionToEndOfPrevWord();
  Range motionToEndOfPrevWORD();

  Range motionFindChar();
  Range motionFindCharBackward();
  Range motionToChar();
  Range motionToCharBackward();
  Range motionRepeatlastTF();
  Range motionRepeatlastTFBackward();

  Range motionToEOL();
  Range motionToColumn0();
  Range motionToFirstCharacterOfLine();

  Range motionToLineFirst();
  Range motionToLineLast();

  Range motionToScreenColumn();

  Range motionToMatchingItem();

  Range motionToPreviousBraceBlockStart();
  Range motionToNextBraceBlockStart();
  Range motionToPreviousBraceBlockEnd();
  Range motionToNextBraceBlockEnd();

  Range motionToFirstLineOfWindow();
  Range motionToMiddleLineOfWindow();
  Range motionToLastLineOfWindow();

  Range motionToNextVisualLine();
  Range motionToPrevVisualLine();

  Range motionToPreviousSentence();
  Range motionToNextSentence();

  Range motionToBeforeParagraph();
  Range motionToAfterParagraph();

#if 0
    Range motionToMark();
    Range motionToMarkLine();

    Range motionToNextOccurrence();
    Range motionToPrevOccurrence();

    Range motionToIncrementalSearchMatch();
#endif

  // TEXT OBJECTS
  Range textObjectAWord();
  Range textObjectInnerWord();
  Range textObjectAWORD();
  Range textObjectInnerWORD();

  Range textObjectInnerSentence();
  Range textObjectASentence();

  Range textObjectInnerParagraph();
  Range textObjectAParagraph();

  Range textObjectAQuoteDouble();
  Range textObjectInnerQuoteDouble();

  Range textObjectAQuoteSingle();
  Range textObjectInnerQuoteSingle();

  Range textObjectABackQuote();
  Range textObjectInnerBackQuote();

  Range textObjectAParen();
  Range textObjectInnerParen();

  Range textObjectABracket();
  Range textObjectInnerBracket();

  Range textObjectACurlyBracket();
  Range textObjectInnerCurlyBracket();

  Range textObjectAInequalitySign();
  Range textObjectInnerInequalitySign();

  Range textObjectAComma();
  Range textObjectInnerComma();

  virtual void reset();

  void beginMonitoringDocumentChanges();

protected:
  void resetParser();

  void initializeCommands();

  QRegularExpression generateMatchingItemRegex() const;

  void executeCommand(const Command *cmd);

  bool motionWillBeUsedWithCommand() const { return !m_awaitingMotionOrTextObject.isEmpty(); };

  void joinLines(unsigned int from, unsigned int to) const;

  OperationMode getOperationMode() const;

  void highlightYank(const Range &range, const OperationMode mode = CharWise);
  void addHighlightYank(const KateViI::Range &range);

#if 0
    void reformatLines(unsigned int from, unsigned int to) const;
#endif

  bool executeEditorCommand(const QString &command);

  /**
   * Get the index of the first non-blank character from the given line.
   *
   * @param line The line to be picked. The current line will picked instead
   * if this parameter is set to a negative value.
   * @returns the index of the first non-blank character from the given line.
   * If a non-space character cannot be found, the 0 is returned.
   */
  int getFirstNonBlank(int line = -1) const;

  KateViI::Cursor findSentenceStart();
  KateViI::Cursor findSentenceEnd();

  Range textObjectComma(bool inner) const;
  void shrinkRangeAroundCursor(Range &toShrink, const Range &rangeToShrinkTo) const;

  KateViI::Cursor findParagraphStart();
  KateViI::Cursor findParagraphEnd();

protected:
  // The 'current position' is the current cursor position for non-linewise
  // pastes, and the current line for linewise.
  enum PasteLocation { AtCurrentPosition, AfterCurrentPosition };
  bool paste(NormalViMode::PasteLocation pasteLocation, bool isgPaste, bool isIndentedPaste);
  KateViI::Cursor cursorPosAtEndOfPaste(const KateViI::Cursor &pasteLocation,
                                        const QString &pastedText) const;

protected:
  // Pending keys.
  QString m_keys;

  // Holds the last t/T/f/F command so that it can be repeated with ;/,
  QString m_lastTFcommand;

  // Used to accumulate the count.
  unsigned int m_countTemp = 0;

  // Cached index in m_commands of the command which needs a motion.
  int m_motionOperatorIndex = 0;

  // Limit of count for scroll commands.
  int m_scrollCountLimit = 1000;

  // All registered commands.
  QVector<Command *> m_commands;

  // All registered motions.
  QVector<Motion *> m_motions;

  // Index in m_commands of possible matched commands (matching m_keys) so far.
  QVector<int> m_matchingCommands;

  // Index in m_motions of possible matched motions (matching m_keys) so far.
  QVector<int> m_matchingMotions;

  // Index in m_keys which splits the operator keys and motion.
  // That is, we could look for a motion command from that position.
  QStack<int> m_awaitingMotionOrTextObject;

  bool m_findWaitingForChar = false;
  bool m_isRepeatedTFcommand = false;

  // Indicates whether it is a line wise command being executed.
  bool m_linewiseCommand = false;

  // Indicate we are executing a command with motion specified.
  bool m_commandWithMotion = false;

  bool m_lastMotionWasLinewiseInnerBlock = false;
  bool m_motionCanChangeWholeVisualModeSelection = false;
  bool m_commandShouldKeepSelection = false;
  bool m_deleteCommand = false;

  // Ctrl-c or ESC have been pressed, leading to a call to reset().
  bool m_pendingResetIsDueToExit = false;

  bool m_isUndo = false;

  // Whether we are waiting for a register or char next.
  bool waitingForRegisterOrCharToSearch() const;

  // item matching ('%' motion)
  QHash<QString, QString> m_matchingItems;
  QRegularExpression m_matchItemRegex;

  KeyParser *m_keyParser = nullptr;

  KateViI::Cursor m_currentChangeEndMarker;
  KateViI::Cursor m_positionWhenIncrementalSearchBegan;

private Q_SLOTS:
  /*
  void textInserted(KateViI::Range range);
  void textRemoved(KateViI::Range);

  void undoBeginning();
  void undoEnded();
  */

  void updateYankHighlightAttrib();

  void clearYankHighlight();
  void aboutToDeleteMovingInterfaceContent();

private:
  void rewriteKeysInCWCase();

  bool assignRegisterFromKeys();

  void updateMatchingCommands();

  // Return true if one motion is executed.
  bool updateMatchingMotions(int p_checkFrom);

  // Execute @p_motion without command.
  void executeMotionWithoutCommand(Motion *p_motion);

  // Execute @p_motion with a command.
  void executeMotionWithCommand(Motion *p_motion);
};

} // namespace KateVi

#endif /* KATEVI_NORMAL_VI_MODE_H */
