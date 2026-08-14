#ifndef TESTS_TEST_TABLEPREVIEW_H
#define TESTS_TEST_TABLEPREVIEW_H

#include <QtTest>

namespace tests {
class TestTablePreview : public QObject {
  Q_OBJECT
private slots:
  // Serializer.
  void testEscapeCellParity();
  void testEscapeCellIdempotent();
  void testSerializeCanonical();
  void testSerializeAlignments();
  void testSerializeRaggedRows();
  void testSerializePreservesPrefixes();
  void testSerializeRejectsUnsafePrefixes();
  void testSerializeRejectsLineSeparators();
  void testSerializePreservesInlineMarkdown();
  void testEscapedPipeDoesNotWidenColumn();
  void testSerializeDoesNotPadOtherRows();

  // Document.
  void testDocumentBuildsTheTable();
  void testDocumentNormalization();
  void testHeaderRowIsBold();
  void testColumnAlignmentIsABlockFormat();
  void testCellsHoldRawMarkdown();
  void testCellWalkReturnsTheEditedText();
  void testDocumentRoundTrip();
  void testRaggedTableIsNotRoundTrippable();
  void testFormatRefreshKeepsTheCaret();

  // Limits.
  void testWidgetRejectsNonTable();
  void testWidgetRejectsOversizedTable();
  void testWidgetRejectsTooManyRowsOrColumns();

  // Sheet geometry.
  void testHeightForWidthComesFromTheDocumentLayout();
  void testHeightForWidthConvertsTheOuterWidth();
  void testColumnsFillTheAssignedWidth();
  void testSettledGeometryNotifiesTheHost();

  // Caret and navigation.
  void testClickPutsTheCaretUnderThePointer();
  void testClickAtTheEndPutsTheCaretAtTheEnd();
  void testTypingNeedsNoEditMode();
  void testTheCaretNeverLeavesTheTable();
  void testReadOnlySheetKeepsTheCaretButSwallowsTyping();
  void testTabWrapsBetweenCells();
  void testArrowOutAtTheEdgesRequestsAFocusEscape();
  void testEscapeRequestsAFocusEscape();

  // Content invariants.
  void testEnterIsSwallowed();
  void testAppendRowKeepsThePrefixesAndTheRowCount();
  void testEnterInLastCellAppendsRow();
  void testEnterAppendsTheFirstBodyRowOfAHeaderOnlyTable();
  void testEnterInTheLastCellOfANonLastRowDoesNotGrow();
  void testEnterModifiersDecideWhetherARowIsAppended();
  void testARefusedEnterHasNoSideEffects();
  void testAnAcceptedEnterCollapsesTheSelection();
  void testEnterRespectsCellBound();
  void testEnterInReadOnlySheetDoesNothing();
  void testTheAppendedRowKeepsTheTableFormat();
  void testTheAppendIsObservedAsOneChange();
  void testEnterAppendedRowIsCommitted();
  void testSelectAllCannotTakeTheTableApart();
  void testCutAndDeleteStayInsideOneCell();
  void testWordDeleteShortcutsStayInsideOneCell();
  void testPastedTextIsSanitized();
  void testPastedRichTextIsFlattened();
  void testAPurelySeparatorPayloadIsRefused();
  void testDroppedTextGoesThroughTheSameValidator();
  void testCommittedImeSeparatorsAreSanitized();
  void testImeReplacementRangesStayInsideOneCell();
  void testAReadOnlySheetRefusesImeMutations();
  void testBecomingAViewerCancelsTheComposition();
  void testABackgroundSheetLeavesTheFocusedCompositionAlone();
  void testASeparatorOnlyImeCommitKeepsTheSelection();

  // Selection clearing.
  void testClearSelectionCollapsesOntoTheCaret();
  void testClearSelectionIsANoOpWithoutASelection();

  // Commit machine.
  void testDebouncedCommit();
  void testCellLeaveFlushesImmediately();
  void testFocusOutFlushesImmediately();
  void testEscapeFlushesImmediately();
  void testEchoOfACommitKeepsANewerEdit();
  void testARevertToThePreCommitSourceIsHonoured();
  void testRejectionMakesTheSheetReadOnly();
  void testAnUntouchedDocumentDiscardsTheEdit();
  void testUndoFlushesBeforeItReachesTheEditor();
  void testRedoIsDroppedWhileDirty();
  void testCommittedImeInputReachesTheFlush();
  void testAnActiveCompositionSurvivesTheRemovalFlush();
  void testRevokedAuthoritySilencesEveryCommit();

  // Palette.
  void testDarkPaletteReachesTheSheet();
};
} // namespace tests

#endif
