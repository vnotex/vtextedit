#ifndef TEST_TYPSTEDITOR_H
#define TEST_TYPSTEDITOR_H

#include <QImage>
#include <QtTest>

#include <functional>

#include <vtextedit/typstruntime.h>

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
  void nativeReplySurvivesSessionRetirement();
  void nativeCallbackFailureRetiresRequest();
  void unavailableRuntimePreservesSource_data();
  void unavailableRuntimePreservesSource();
  void runtimeSessionsRemainIndependent();
  void scannerFailsClosedWithoutEvaluation();
  void scannerDeadlineAndBusyFailClosed();
  void runtimeDrainsRunningNativeCall();
  void runtimeShutdownCompletesAdmittedExport();
  void deadlineRetryKeepsDispatchIdentityUntilCompletion();
  void runtimeDropsRepliesForDeletedReceiver();
  void compilerCompletionCanDestroyReceiver();
  void compilerGeometryAndScale();
  void compilerBitmapOutlivesRuntime();
  void previewThemeDefaultsRespectAuthoredStyles();
  void previewThemeSwitchAndOptOutPreserveSource();
  void exportsIgnorePreviewTheme();
  void textStyleOverridesMergeWithoutSharedMutation();
  void textFontDefaultsMatchAuthoredStyles_data();
  void textFontDefaultsMatchAuthoredStyles();
  void textFontThemeSwitchAndZoomPreserveSource();
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
  void diskDependenciesRecoverAfterReplacementAndRecreation();
  void diagnosticsPreserveLastGoodOutput();
  void selectionDefersPublicationAcrossScrollAndModes();
  void openingPreservesSourcePosition_data();
  void openingPreservesSourcePosition();
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
  void documentResetClearsPublishedState();
  void documentResetRetiresPendingPdf();
  void documentResetDrainsTimedOutRequest();
  void resourceRequestsAcrossDocumentReset_data();
  void resourceRequestsAcrossDocumentReset();
  void documentResetInvalidationCanDestroyEditor();
  void repeatedCloseDuringPendingOutput();
  void resourceContextMetadataAndGestures();
  void resourceImageDataRetentionAndCancellation();
  void resourceImageDataBoundsAndGeneratedPixels();
  void resourceIndependentSnapshotRetention();
  void resourceInternalLinkNavigation();
  void resourceStaleInternalLinkEligibility();
  void resourceResizePreservesSourceUndo();
  void resourceResizeRejectsUnsafeContexts();
  void resourceMenusFollowTheirSurface_data();
  void resourceMenusFollowTheirSurface();
  void resourceMenusSurviveDeletion_data();
  void resourceMenusSurviveDeletion();
  void resourceImageCompletionSurvivesDeletion_data();
  void resourceImageCompletionSurvivesDeletion();

private:
  vte::TypstRuntime m_runtime;
};
} // namespace tests

#endif
