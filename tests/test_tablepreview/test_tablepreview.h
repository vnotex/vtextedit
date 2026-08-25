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

  // Opt-in aligned source.
  void testAlignedSerializeIsOptIn();
  void testAlignedSerializePadsColumnsAndDelimiter();
  void testAlignedPlacementFollowsTheAlignment();
  void testAlignedWidthIsDisplayWidth();
  void testAlignedWidthUsesTheEscapedText();
  void testAlignedCeilingFallsBackToCompact();
  void testAlignedOutputRoundTrips();
  void testAlignedOutputKeepsThePrefixes();

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
  void testCellSyntaxFormatsArePainted();
  void testSameSourceSnapshotRepaintsTheCells();
  void testFormatOnlyDifferenceIsDetected();
  void testResolveFormatRunsMergesOverlaps();
  void testResolveFormatRunsSkipsUnknownStyles();
  void testStaleEchoDoesNotRepaintTheCells();
  void testARunDoesNotBleedIntoTheRestOfTheCell();
  void testTypingAfterARunIsNotHighlighted();
  void testTypingIntoAHeaderCellStaysBold();

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
  void testTabHandsBackAtTheEnds();
  void testArrowOutAtTheEdgesRequestsAFocusEscape();
  void testEscapeNoLongerRequestsAFocusEscape();

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

  // Merge and split, and the HTML write-back they force.
  void testHtmlSnapshotBuildsSpanningGrid();
  void testMergeJoinsTextAndConvertsToHtml();
  void testMergeRefusals();
  void testMergeContainmentRefusal();
  void testSplitCellRestoresTheGrid();
  void testHtmlSerializerKeepsCellsSingleLine();
  void testHtmlOnlyTableWritesBackVerbatim();
  void testMergeMenuEntriesAndAlignmentGating();
  void testLiveRectangleSurvivesCopyAndGatesMutations();
  void testBulkMutatorsAreRefused();
  void testMergedGridRowColumnOpsKeepTags();
  void testMalformedPayloadTableStaysWritable();
  void testAlignmentIsRefusedOnASpannedColumn();

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
  void testAFocusEscapeFlushesImmediately();
  void testEchoOfACommitKeepsANewerEdit();
  void testARevertToThePreCommitSourceIsHonoured();
  void testRejectionMakesTheSheetReadOnly();
  void testAnUntouchedDocumentDiscardsTheEdit();
  void testUndoUnwindsTheRingBeforeItReachesTheEditor();
  void testTypingAfterACommitIsStillUndoable();
  void testRedoIsDroppedWhileDirty();
  void testCommittedImeInputReachesTheFlush();
  void testAnActiveCompositionSurvivesTheRemovalFlush();
  void testRevokedAuthoritySilencesEveryCommit();

  // Row and column operations.
  void testInsertRowKeepsThePrefixes();
  void testRemoveRowKeepsThePrefixes();
  void testInsertColumnKeepsTheAlignmentsAndTheDeclaredWidth();
  void testRemoveColumnKeepsTheAlignmentsAndTheDeclaredWidth();
  void testColumnConstraintsFollowTheColumnCount();
  void testStructuralRefusalsChangeNothing();
  void testStructuralBoundsAreEnforced();
  void testSetColumnAlignmentRewritesTheDelimiterAndTheCells();
  void testAPrefixedTableSurvivesARowInsert();

  // Context menu.
  void testTheContextMenuOffersTheTableOperations();
  void testTheContextMenuReflectsWhereItWasOpened();
  void testAReadOnlySheetDisablesTableMutations();
  void testANonRoundTrippableSheetDisablesTableMutations();
  void testStandaloneMarkdownDropsThePrefixes();
  void testHtmlCarriesTheColumnAlignments();
  void testHtmlRendersInlineMarkdown();
  void testHtmlOmitsRawHtmlCells();
  void testHtmlKeepsAnEscapedPipeInOneCell();
  void testCopyActionsStayEnabledOnAReadOnlySheet();
  void testCopyActionsPutThePayloadOnTheClipboard();
  void testTheFactoryPropagatesTheAlignOption();
  void testAlignedDocumentPathsAndHtmlAreUnaffected();
  void testTheMenuActionsMutateTheTable();
  void testAnAlignmentOnlyChangeIsCommitted();

  // Palette.
  void testDarkPaletteReachesTheSheet();
};
} // namespace tests

#endif
