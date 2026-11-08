#ifndef TESTS_TEST_MARKDOWNEDITOR_H
#define TESTS_TEST_MARKDOWNEDITOR_H

#include <QtTest>

namespace tests {
// Integration coverage of VMarkdownEditor source edits and MarkdownUtils actions.
class TestMarkdownEditor : public QObject {
  Q_OBJECT
private slots:
  // Keep first so a whole-suite run also covers cold repository initialization.
  void testDefaultNativeHighlighting();
  void testNativeSyntaxNameFallback();
  void testDiagramSyntaxBoundaries_data();
  void testDiagramSyntaxBoundaries();

  void testShiftWheelScroll_data();
  void testShiftWheelScroll();

  void testHighlightPaletteRefreshPreservesState();
  void testHighlightPaletteRejectsLateReplies();
  void testHighlightPalettesAreEditorLocal();
  void testKSyntaxThemeRefreshesCachedCode();
  void testKSyntaxThemeFileSnapshot();

  void testMarkdownDocumentDemandAndReuse();
  void testMarkdownDocumentInvalidation();
  void testMarkdownDocumentRetentionEviction();

  // Source-preserving Markdown concealment through the real editor and Vi input.
  void testConcealMarkdownConfig();
  void testConcealHoverToolTip();
  void testConcealCaretAndViMotion();
  void testConcealSyntaxRules_data();
  void testConcealSyntaxRules();
  void testConcealNestedAndMultilineSyntax();
  void testConcealCustomDestinationRule();
  void testConcealHeadingAndFormulaBoundaries();
  void testPersistentConcealKinds_data();
  void testPersistentConcealKinds();
  void testPersistentConcealNavigation();
  void testPersistentConcealClickBoundary_data();
  void testPersistentConcealClickBoundary();
  void testPersistentConcealSlowTyping_data();
  void testPersistentConcealSlowTyping();
  void testPersistentConcealBoundarySelection();
  void testPersistentConcealBoundaryBackspace_data();
  void testPersistentConcealBoundaryBackspace();
  void testPersistentConcealBoundaryIme();
  void testPersistentConcealNested_data();
  void testPersistentConcealNested();
  void testPersistentConcealEmptyHeading();
  void testPersistentConcealPendingEdits_data();
  void testPersistentConcealPendingEdits();
  void testPersistentConcealVi_data();
  void testPersistentConcealVi();
  void testPersistentConcealDirectCleanup_data();
  void testPersistentConcealDirectCleanup();
  void testPersistentConcealIme_data();
  void testPersistentConcealIme();
  void testPersistentConcealRevealPolicies();
  void testMarkBackgroundOnCursorLine_data();
  void testMarkBackgroundOnCursorLine();

  void testMultilineReplaceAllUndoRedo();
  void testMultilineReplacementCaptures();
  void testRegexReplacementEscapes();
  void testMultilineReplaceAndFind();
  void testReadOnlyReplacementIsRejected();
  void testMultilineReplacementKeepsLineEndings();

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

  // Source editing and canonical Qt geometry through inline concealment.
  void testConcealEditingAndGeometry();
  void testConcealIme_data();
  void testConcealIme();
  void testConcealImeCommit_data();
  void testConcealImeCommit();
  void testConcealPendingParseEdits_data();
  void testConcealPendingParseEdits();
  void testConcealPendingParseInvalidation_data();
  void testConcealPendingParseInvalidation();

  // Image previews and the `=WxH` size extension.
  void testImageLinksArePublished();

  void testSizedImagePreviewIsScaled();

  void testOneUrlAtTwoSizesGetsTwoResources();

  void testOversizedImageIsClamped();

  // Seeded image data (PreviewMgr::seedImageData).
  void testSeededImageAvoidsDownload();

  void testRootlessImageResourcesRespectIoPolicy();

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
  void testTableInterruptsListParagraph();
  void testTableSourceFormatSyntaxBoundaries();
  void testTableSourceFormatIdempotence();

  // AST-aware Enter and opt-in automatic ordered-list source numbering.
  void testOrderedListEnterTabStartsNestedList();
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

  // Opt-in, deferred section numbers written into actual Markdown source.
  void testSourceEditSettlement();
  void testSourceEditJoinedProvenance();
  void testSourceEditConcealment_data();
  void testSourceEditConcealment();
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
  void testHeadingSourceRemovalRestoresAuthored();
  void testHeadingSourceRemovalMatchingIsUnchanged();
  void testHeadingSourceRemovalRenumberAndPattern();
  void testHeadingSourceRemovalTracksOrdinaryEdits();
  void testHeadingSourceRemovalManualOverride();
  void testHeadingSourceRemovalReplacementAndHistory();
  void testHeadingSourceRemovalDeletionAndBranch();
  void testHeadingSourceRemovalReloadAndHistoryClear();
  void testHeadingSourceRemovalBareAtx_data();
  void testHeadingSourceRemovalBareAtx();
  void testHeadingSourceRemovalSelectionAndScroll();
  void testHeadingSourceRemovalUnavailable();
};
} // namespace tests

#endif // TESTS_TEST_MARKDOWNEDITOR_H
