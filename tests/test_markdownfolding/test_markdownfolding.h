#ifndef TESTS_TEST_MARKDOWNFOLDING_H
#define TESTS_TEST_MARKDOWNFOLDING_H

#include <QtTest>

#include <textfolding.h>

class QTextDocument;

namespace vte {
class MarkdownFoldingProvider;
}

namespace tests {
class TestMarkdownFolding : public QObject {
  Q_OBJECT
private slots:
  void initTestCase();

  void testApplyFoldingRegions();

  void testDiffPreservesFoldState();

  void testDiffRemovesStaleRanges();

  void testDiffAddsNewRanges();

  void testSkipsSmallRanges();

  void testNesting();

  void testEmptyRegions();

  void testClearOnDisable();

  // Heading section tests.
  void testHeadingSectionBasic();
  void testHeadingSectionMultiple();
  void testHeadingSectionNested();
  void testHeadingSectionTooSmall();
  void testHeadingSectionAtEnd();
  void testHeadingSectionInsideBlockquote();

  void testEndToEndFolding();

  void testFoldingBlockHeights();

  void testFractionalBlockCoordinates();

  void testFractionalClipDraw();

  void testDocumentSizeSignals();

  void testWrappedInlinePreviewCoordinates();

  void testMalformedPreviewData();

  void testCursorWidthPaintOnly();

  void testExactHitTesting();

  void testFuzzyHitInLeadingSpace();

  void testFuzzyHitInWrappedLineGap();

  void testFuzzyHitBelowLastLine();

  void testFuzzyHitAtLineBoundary();

  void testFuzzyHitNearerPreviousLine();

  void testFuzzyHitAboveDocument();

  void testFuzzyHitInInlinePreviewGap();

  // Interactive preview widget reservations.
  void testWidgetPreviewBlockReservation();

  void testWidgetPreviewStacking();

  void testWidgetPreviewGeometryWithEqualDocumentSize();

  void testDocumentSizeRepairsAMissingBlockOffset();

  void testLayoutIsBusyDuringWidgetGeometryEmission();

  void testWidgetPreviewFolding();

  void testWidgetPreviewWidthClamped();

  void testWidgetPreviewInlineBand();

  void testWidgetPreviewBlockMarker();

  void testWidgetPreviewInlineMarker();

  void testWidgetMarkerCoexistsWithBlockImage();

  void testWidgetMarkerRemovedWhenPreviewDisabled();

  void testClaimSuppressesStaticPreview();

  void testClaimIsTypeScoped();

  void testSourceTextRectSharesWidgetCoordinates();

  void testInPlaceRewriteKeepsFoldRange();

  void testLiveRangeIsNotRecreated();

  // Reconciliation from the ranges' live positions.
  void testReconcileSurvivesBlockShift();

  void testReconcileEndBlockChange();

  void testReconcileTypeChange();

  void testExactExtentDeduplication();

  void testTryRegionFolded();

  void testRestoreFoldedRange();

  // Preview driven auto-folding.
  void testAutoFoldWidgetPreview();

  void testAutoFoldCaretRule();

  void testAutoFoldPaintedPreview();

  void testAutoFoldSkipsWrapperRegion();

  void testAutoFoldOptionOff();

  void testAutoFoldRestoresReportedState();

  void testAutoFoldWithTextFoldingDisabled();

  void testRestoreFoldAfterInPlaceRewrite();

  void cleanupTestCase();

  void cleanup();

private:
  QTextDocument *m_doc = nullptr;
  vte::TextFolding *m_textFolding = nullptr;
  vte::MarkdownFoldingProvider *m_provider = nullptr;
};
} // namespace tests

#endif
