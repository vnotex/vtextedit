#ifndef TESTS_TEST_MARKDOWNEDITOR_H
#define TESTS_TEST_MARKDOWNEDITOR_H

#include <QtTest>

namespace tests {
// Integration coverage of VMarkdownEditor source edits and MarkdownUtils actions.
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

  // Opt-in, deferred section numbers written into actual Markdown source.
  void testHeadingSourceDefaultAndActivation();
  void testHeadingSourceReadOnly();
  void testHeadingSourceMaintenance();
  void testHeadingSourceInvalidProvider_data();
  void testHeadingSourceInvalidProvider();
  void testHeadingSourceTitleAndEmpty_data();
  void testHeadingSourceTitleAndEmpty();
  void testHeadingSourceExemptTitleTransition();
  void testHeadingSourceSyntax_data();
  void testHeadingSourceSyntax();
  void testHeadingSourceSetextPatterns();
  void testHeadingSourceUnresolvedSetextBoundary();
  void testHeadingSourceMarkerActions();
  void testHeadingSourceDebounce();
  void testHeadingSourceCancellationAndLoad();
  void testHeadingSourceFreshParseAndPublication();
  void testHeadingSourceProviderInvalidation();
  void testHeadingSourceUndoRedo_data();
  void testHeadingSourceUndoRedo();
  void testHeadingSourceUndoBeforeDebounceAndBranch();
  void testHeadingSourceExplicitHistory();
  void testHeadingSourceCursorAndSelection_data();
  void testHeadingSourceCursorAndSelection();
  void testHeadingSourceOverriddenSelectionAndScroll();
  void testHeadingSourceInputMethodDeferral();
  void testHeadingSourcePreviewFocusDeferral();
  void testHeadingSourceLayoutDeferral();
  void testHeadingSourceGuaranteeReset();
  void testHeadingSourceTableCoexistence();
};
} // namespace tests

#endif // TESTS_TEST_MARKDOWNEDITOR_H
