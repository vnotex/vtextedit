#ifndef TEST_TYPSTEDITOR_H
#define TEST_TYPSTEDITOR_H

#include <QtTest>

namespace tests {
class TestTypstEditor : public QObject {
  Q_OBJECT
private slots:
  void overlaysDiagnosticsAndSnapshotRetention();
  void unicodeNavigationPdfPointsAndRasterLimits();
  void repeatedAndSplitOutputRecords();
  void modesPreserveSourceUndoAndSelection();
  void hiddenSourceAndImportChangesReachPdf();
  void diagnosticsPreserveLastGoodOutput();
  void selectionDefersPublicationAcrossScrollAndModes();
  void inPlaceReservationsDoNotEditSource();
  void occurrenceControlsAndSplitPartsRenderRealOutput();
  void supersededCompileCannotPublish();
  void syntaxHighlightsSurviveRehighlightAndThemeChanges();
  void syntaxHighlightsFollowEditsAndErrors();
  void pdfBudgetFailurePreservesOutputAndRecovers();
  void repeatedCloseDuringPendingOutput();
};
} // namespace tests

#endif
