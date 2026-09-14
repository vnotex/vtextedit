#ifndef TESTS_TEST_MARKDOWNEDITOR_H
#define TESTS_TEST_MARKDOWNEDITOR_H

#include <QtTest>

namespace tests {
// Integration coverage of the real Return and Vi o/O pipelines of VMarkdownEditor, plus the
// MarkdownUtils quote helpers it is built on.
class TestMarkdownEditor : public QObject {
  Q_OBJECT
private slots:
  // MarkdownUtils helpers.
  void testIsQuote();

  // Pins that c_quoteRegExp / typeQuote are unchanged.
  void testTypeQuoteUnchanged();

  // Enter continuation.
  void testQuoteContinuation();

  void testQuoteWithListContinuation();

  void testEmptyQuoteStartsANewQuoteLine();

  void testEmptyListInQuoteExit();

  void testCursorBeforeTextDoesNotCollapse();

  void testMidLineSplitInsideQuote();

  void testSelectionFallsThrough();

  void testSelectionDoesNotCarryAstContext();

  void testLazyContinuation();

  void testLazyContinuationDepths();

  void testStaleAstNeverInserts();

  void testFenceVeto();

  void testStaleFenceStillSuppresses();

  void testIndentedQuoteIsStillContinued();

  void testShiftReturnDoesNotContinue();

  void testCtrlReturnDoesNotLeakContext();

  void testWhitespaceOnlyQuoteContinues();

  void testPlainListMidLineSplit();

  void testPlainListContinuationRegression();

  void testRepeatedReturnDoesNotLeakContext();

  void testUndoIsASingleStep();

  // Image previews and the `=WxH` size extension.
  void testImageLinksArePublished();

  void testSizedImagePreviewIsScaled();

  void testOneUrlAtTwoSizesGetsTwoResources();

  void testOversizedImageIsClamped();

  // Seeded image data (PreviewMgr::seedImageData).
  void testSeededImageAvoidsDownload();

  void testSeededImageBufferIsBounded();

  // Multi-line inline markers (typeMarker over a cross-block selection).
  void testMultiLineMarkerOnList();

  void testMultiLineMarkerToggleOff();

  void testMultiLineMarkerMixedSelection();

  void testMultiLineMarkerNesting();

  void testMultiLineMarkerSkipsBlankLines();

  void testMultiLineMarkerTrailingBlockBoundary();

  void testMultiLineMarkerPartialEdges();

  void testMultiLineMarkerPrefixes();

  void testMultiLineMarkerSingleBlockUnchanged();

  void testMultiLineMarkerAllMarkers();

  void testMultiLineMarkerUndo();

  void testMultiLineMarkerBlankOnly();

  void testMultiLineMarkerOverriddenSelection();

  // Ordered list numbering over a multi-line selection.
  void testOrderedListSequentialNumbering();

  void testOrderedListFromOtherListTypes();

  void testOrderedListIndentationLevels();

  void testOrderedListToggleOff();

  void testOrderedListSingleLine();

  void testAspectRatioDerivedAxisIsBounded();

  // Automatic, opt-in formatting of directly edited Markdown table source.
  void testTableSourceFormatDebounce();
  void testTableSourceFormatProgrammaticEdits();
  void testTableSourceFormatLoadAndConfig();
  void testTableSourceFormatCursorAndSelection_data();
  void testTableSourceFormatCursorAndSelection();
  void testTableSourceFormatProtectedPositions();
  void testTableSourceFormatUndoRedo();
  void testTableSourceFormatSyntaxBoundaries();
  void testTableSourceFormatIdempotence();

  // AST-aware Enter and opt-in automatic ordered-list source numbering.
  void testListAstContinuation();
  void testListAstExitAndSelection();
  void testListAstCodeAndStaleness();
  void testListAutoNumberStructuralEdits();
  void testListAutoNumberSplit();
  void testListAutoNumberNestedWidths();
  void testListAutoNumberConfigAndReplay();
  void testListAutoNumberProtectedPositions();
  void testListAndTableSourceFormatting();
  void testListAutoNumberStaleWorker();

  // AST-owned list-item decorations through public document-layout painting.
  void testListItemGuides();
  void testListItemActiveBackground();
  void testListItemDecorationsFreshness();
  void testListItemDecorationsGeometry();
  void testListItemDecorationsFoldingAndPreviews();
  // Vi open-line completion through the real normal/insert-mode key pipeline.
  void testViListOpenLines();
  void testViListOpenContext();
  void testViListOpenNumbering();
  void testViListOpenUndoAndReplay();
  void testViListInsertReturn();
};
} // namespace tests

#endif // TESTS_TEST_MARKDOWNEDITOR_H
