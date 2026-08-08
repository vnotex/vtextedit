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
  void testSerializeCapsPadding();

  // Model.
  void testModelNormalization();
  void testModelNoOpCommit();
  void testModelCommitEmitsOnRealChange();
  void testModelAlignmentRole();
  void testModelRoundTrip();

  // Widget.
  void testWidgetRejectsNonTable();
  void testWidgetVisibleRows();
  void testLargeTableFitsVisibleRows();
  void testWidgetRejectsOversizedTable();
  void testWidgetRejectsTooManyRowsOrColumns();
  void testRaggedTableIsNotRoundTrippable();
  void testCellEditorHoldsRawMarkdown();
  void testOneClickStartsEditing();
  void testClickPutsTheCaretUnderThePointer();
  void testClickAtTheEndPutsTheCaretAtTheEnd();
  void testReadOnlySheetIgnoresTheClick();
  void testPreferredSizeCacheTracksContents();
  void testColumnsFillTheAssignedWidth();
  void testColumnLayoutFollowsACellEdit();

  // Delegate.
  void testDelegateWrapsOrdinaryText();
  void testDelegateWrapsAnUnbreakableToken();
  void testDelegateAccumulatesLineHeightsOnly();
  void testDelegateHonorsRolesAndDirection();
  void testDelegateHonorsPaletteState();
  void testDelegatePaintsPanelTextAndFocusInOrder();
  void testDelegatePreservesPainterState();

  // Column planning.
  void testColumnFloorIsTwelveCharacters();
  void testColumnsCompressProportionally();
  void testColumnsStopAtTheFloorAndScroll();
  void testVerticalScrollBarChromeIsReserved();

  // Band geometry.
  void testPureHeightMatchesTheLiveRows();
  void testInheritedMutationDropsTheMemo();
  void testConstrainedHeightTracksTheWidth();
  void testRowsGrowAndShrinkWithTheWidth();

  // Lazy row fitting.
  void testDistantRowsAreFittedLazily();
  void testRowFittingKeepsTheAnchorNearTheEnd();
  void testRowFittingIsBounded();

  // Host notification.
  void testSettledGeometryNotifiesTheHost();
};
} // namespace tests

#endif
