#ifndef TEST_TYPSTEDITOR_H
#define TEST_TYPSTEDITOR_H

#include <QtTest>
#include <QImage>

#include <functional>

namespace tests {
// Runtime signal connections keep GUI regressions independent of private DLL symbols.
class TypstRasterObserver : public QObject {
  Q_OBJECT
public:
  std::function<void(quint64, quint64, int, qreal)> requested;
  std::function<void(quint64, quint64, int, qreal)> cancelled;
  std::function<void(quint64, quint64, int, qreal, const QImage &)> completed;

public slots:
  void rasterRequested(quint64 p_epoch, quint64 p_snapshot, int p_page, qreal p_scale) {
    if (requested)
      requested(p_epoch, p_snapshot, p_page, p_scale);
  }
  void rasterCancelled(quint64 p_epoch, quint64 p_snapshot, int p_page, qreal p_scale) {
    if (cancelled)
      cancelled(p_epoch, p_snapshot, p_page, p_scale);
  }
  void pageImageReady(quint64 p_epoch, quint64 p_snapshot, int p_page, qreal p_scale,
                      const QImage &p_image) {
    if (completed)
      completed(p_epoch, p_snapshot, p_page, p_scale, p_image);
  }
};
class TypstPdfObserver : public QObject {
  Q_OBJECT
public:
  std::function<void(quint64)> published;

public slots:
  void snapshotPublished(quint64 p_snapshot) {
    if (published)
      published(p_snapshot);
  }
};
class TestTypstEditor : public QObject {
  Q_OBJECT
private slots:
  void compilerGeometryAndScale();
  void compilerUnicodeImportsAndNormalization();
  void compilerAsyncFailureAndRecovery();
  void compilerRejectsInvalidScaleAndPixelLimit_data();
  void compilerRejectsInvalidScaleAndPixelLimit();
  void compilerResultBudgetClearsPartialPages_data();
  void compilerResultBudgetClearsPartialPages();
  void overlaysDiagnosticsAndSnapshotRetention();
  void unicodeNavigationPdfPointsAndRasterLimits();
  void previewClickNavigatesSource();
  void previewClickPreservesSelectionLinksAndStaleOutput();
  void repeatedAndSplitOutputRecords();
  void outlineIncludesEvaluatedHeadingsAndUnicodeRanges();
  void outlineInvalidationFailureAndRecovery();
  void outlineScopeAndModesPreserveSource();
  void supersededCompileCannotPublishOutline();
  void modesPreserveSourceUndoAndSelection();
  void pdfZoomModeNotifications();
  void pdfFitBeforePublication_data();
  void pdfFitBeforePublication();
  void pdfZoomPublicationIsReentrant();
  void pdfZoomWheelAndContextMenu();
  void independentEditPreviews_data();
  void independentEditPreviews();
  void combinedPreviewsAdvanceWhilePdfSelectionIsDeferred();
  void hiddenSourceAndImportChangesReachPdf();
  void diagnosticsPreserveLastGoodOutput();
  void selectionDefersPublicationAcrossScrollAndModes();
  void sideBySideScrollSync();
  void scrollSyncWithNonScrollableSource();
  void cursorLocationFlashesThreeTimes();
  void cursorHighlightFollowsLatestPublication();
  void headingCursorTargetsTitleInsteadOfOutline();
  void inPlaceReservationsDoNotEditSource();
  void occurrencesAndSplitPartsRenderSequentially();
  void supersededCompileCannotPublish();
  void syntaxHighlightsSurviveRehighlightAndThemeChanges();
  void syntaxHighlightsFollowEditsAndErrors();
  void rawCodeHighlightsUseSyntaxTheme();
  void rawCodeHighlightsFollowEditsAndErrors();
  void lazyHighlightsFollowPendingEdits();
  void lazyPreviewsFollowPendingEdits();
  void lazyStateClearsOnReplacement_data();
  void rasterHandoffRetainsLastGoodWhileTyping();
  void rasterHandoffTransitions_data();
  void rasterHandoffTransitions();
  void lazyStateClearsOnReplacement();
  void pdfVisiblePagesStaySharpAndRecover_data();
  void pdfVisiblePagesStaySharpAndRecover();
  void pdfBudgetFailurePreservesOutputAndRecovers();
  void repeatedCloseDuringPendingOutput();
};
} // namespace tests

#endif
