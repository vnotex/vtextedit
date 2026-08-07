#ifndef TESTS_TEST_INTERACTIVEPREVIEW_H
#define TESTS_TEST_INTERACTIVEPREVIEW_H

#include <QtTest>

#include <QVector>

#include <functional>

#include <vtextedit/preview.h>
#include <vtextedit/previewwidget.h>

namespace vte {
class VMarkdownEditor;
}

namespace tests {
// A renderer which simply records what it was asked to render.
class RecordingPreviewWidget : public vte::PreviewWidget {
  Q_OBJECT
public:
  RecordingPreviewWidget(vte::PreviewWidgetContext *p_context, QWidget *p_parent,
                         const QVector<vte::PreviewElementType> &p_types, const QSize &p_hint);

  QVector<vte::PreviewElementType> supportedTypes() const Q_DECL_OVERRIDE;

  bool setPreview(const QSharedPointer<const vte::Preview> &p_preview) Q_DECL_OVERRIDE;

  // Destroy this instance from inside the second setPreview() call.
  bool m_selfDestructOnUpdate = false;

  // Run one bounded nested event loop from inside the next setPreview() call,
  // the way a real renderer opening a modal dialog would.
  bool m_spinOnNextSetPreview = false;

  // Invoked from inside that nested loop, so a test can assert on the host
  // while the callback is still on the stack.
  std::function<void()> m_duringSpin;

  int m_spinCount = 0;

  // Host object carrying the reconcile delivery counter, sampled around the
  // nested loop. A blocked delivery calls into nothing, so the counter is the
  // only way to tell one consumed delivery from a re-armed spin.
  QObject *m_deliveryCounterSource = nullptr;

  int m_deliveriesBeforeSpin = -1;

  int m_deliveriesAfterSpin = -1;

  QSize sizeHint() const Q_DECL_OVERRIDE;

  qreal preferredWidthFraction() const Q_DECL_OVERRIDE;

  // Share of the text column this double claims. Only a preview on its own
  // band may be widened by it.
  qreal m_widthFraction = 0;

  // Every measurement the host performs goes through here.
  mutable int m_sizeHintCount = 0;

  bool hasHeightForWidth() const Q_DECL_OVERRIDE;

  int heightForWidth(int p_width) const Q_DECL_OVERRIDE;

  // When set, the height is derived from the assigned width.
  bool m_wrapping = false;

  QSharedPointer<const vte::Preview> m_preview;

  int m_setPreviewCount = 0;

  vte::PreviewReplacementResult m_lastResult;

  int m_resultCount = 0;

private slots:
  void handleReplacementFinished(const vte::PreviewReplacementResult &p_result);

private:
  QVector<vte::PreviewElementType> m_types;

  QSize m_hint;
};

class RecordingPreviewFactory : public vte::PreviewWidgetFactory {
  Q_OBJECT
public:
  explicit RecordingPreviewFactory(const QVector<vte::PreviewElementType> &p_types,
                                   QObject *p_parent = nullptr);

  QVector<vte::PreviewElementType> supportedTypes() const Q_DECL_OVERRIDE;

  vte::PreviewWidget *createWidget(vte::PreviewWidgetContext *p_context,
                                   const QSharedPointer<const vte::Preview> &p_preview,
                                   QWidget *p_parent) Q_DECL_OVERRIDE;

  // Decline every request, so the host falls through to the next factory.
  bool m_decline = false;

  // Create an instance which refuses the snapshot it was created for.
  bool m_refuseSetPreview = false;

  // Create wrapping instances whose height depends on the assigned width.
  bool m_wrapping = false;

  // Handed to every instance this factory creates.
  qreal m_widthFraction = 0;

  // Unregister this factory from inside supportedTypes()/createWidget().
  vte::VMarkdownEditor *m_unregisterSelfIn = nullptr;

  // Attempt a (forbidden) reentrant registration from inside createWidget().
  vte::VMarkdownEditor *m_registerReentrantlyIn = nullptr;

  // Same, but from inside supportedTypes(). That callback is reached from the
  // registry scan rather than from widget construction, so it needs its own
  // hook to prove the scan is guarded too.
  vte::VMarkdownEditor *m_registerReentrantlyInSupportedTypes = nullptr;

  bool m_reentrantRegistrationAccepted = false;

  int m_createCount = 0;

  // Survives the factory, so a self-unregistering factory can still be
  // asserted on after it has been destroyed.
  int *m_createCountSink = nullptr;

  int m_supportedTypesCount = 0;

  QSize m_hint = QSize(120, 30);

  QVector<RecordingPreviewWidget *> m_widgets;

private:
  QVector<vte::PreviewElementType> m_types;
};

class TestInteractivePreview : public QObject {
  Q_OBJECT
private slots:
  void testBuiltinTableWidgetCreated();
  void testNoWidgetForImageCodeMathByDefault();
  void testCustomFactoryOverridesBuiltin();
  void testMultiTypeFactory();
  void testFactoryPriorityAndOrder();
  void testDecliningFactoryFallsThrough();
  void testRefusingWidgetFallsThrough();
  void testRegistrationValidation();
  void testUnregisterRestoresFallback();
  void testUnregisterDestroysFactory();
  void testEditorDestructionDestroysFactory();
  void testIdentityReuseOnUnrelatedEdit();
  void testReplacementAccepted();
  void testReplacementIsOneUndoStep();
  void testReplacementRejectedWhenReadOnly();
  void testReplacementRejectedOnStaleSnapshot();
  void testReplacementRejectedOnTypeMismatch();
  void testReplacementRejectedOnElementCountMismatch();
  void testReplacementAcceptedAfterUnrelatedEdit();
  void testReplacementPreservesBlockquotePrefix();
  void testTableEditCommitsCanonicalMarkdown();
  void testSourceBitDisablesTablePreview();
  void testGlobalDisableRemovesWidgets();
  void testTablePreviewVisibleRows();
  void testDuplicateTablesGetDistinctIdentities();
  void testWidgetGeometryFollowsScrolling();

  // Regressions.
  void testReplacementRejectedOnChangedContainerChain();
  void testReplacementRejectedWhenSplittingTrailingText();
  void testSourceMismatchDoesNotRestoreStaleValues();
  void testFactoryUnregisteringItselfFromCallback();
  void testReentrantRegistrationRejected();
  void testEditorDestructionDestroysWidgetsAndContexts();
  void testEditorDestructionDestroysPendingRemovals();
  void testWrappingWidgetGeometryIsStable();
  void testWrappedInlineSourceMeasuredAtAssignedWidth();
  void testReplacementOfLaterInlineElement();
  void testWidgetDestroyingItselfOnUpdateFallsBack();
  void testCommitKeepsTheSameWidget();
  void testConfigChangeKeepsLiveAnchors();
  void testReplacementRejectsExoticLineSeparators();
  void testOversizedTableFallsBackToSource();
  void testReadOnlyEditorDisablesCellEditing();
  void testNoSnapshotWorkWithoutAClaimableFactory();
  void testTableSheetFitsWithoutScrollBars();
  void testTableSheetRefitsAfterFontChange();
  void testTableSheetFitsWithALargeThemeFont();
  void testTableSheetUsesTheThemeGenericFont();
  void testBackToBackReplacementsAccepted();
  void testBackToBackReplacementsAcceptedForNonTable();
  void testRebasedSourceSurvivesRebuild();
  void testReadOnlyToggleReachesLiveSheets();
  void testRaggedTableIsNotEditable();
  void testCommitKeepsCellEditorAcrossNextParse();
  void testTableIsOptInByDefault();
  void testMultiLineImageIsStandalone();

  // Sheet geometry.
  void testTableSheetHeightMatchesItsRows();
  void testTableSheetSpansContentWidth();
  void testTableSheetKeepsANaturalWidthInsideTheBand();
  void testInlinePreviewIgnoresTheWidthFraction();
  void testTableColumnsShareTheExtraWidth();
  void testSingleColumnTableFillsTheSheet();
  void testTableWidthFollowsEditorResize();

  // Review fixes.
  void testRejectionAfterAcceptKeepsCommittedValues();
  void testReconcileIsDeferredDuringWidgetCallback();
  void testBlockedReconcileIsNotRearmedWhileBlocked();
  void testMeasurementIsNotRepeatedWhenNothingChanged();
  void testWideCellSurvivesRoundTrip();
  void testRebasedTableMatchesGenerationSnapshot();
  void testStaleGenerationReplayAfterShrink();

  // Local review fixes.
  void testUnregisteringTheBuiltinFactoryIsSafe();
  void testReentrantRegistrationFromSupportedTypesRejected();
  void testGenerationDeliveredDuringCallbackIsNotLost();
  void testReplacementCannotSplitATableCell();
  void testSourceTextRectFollowsTheSource();
};
} // namespace tests

#endif
