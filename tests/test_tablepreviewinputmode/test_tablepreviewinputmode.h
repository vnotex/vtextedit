#ifndef TESTS_TEST_TABLEPREVIEWINPUTMODE_H
#define TESTS_TEST_TABLEPREVIEWINPUTMODE_H

#include <QObject>
#include <QtTest>

namespace tests {
// The three input modes running inside a table preview sheet.
//
// The contract under test is decision D1: the mode's buffer is the caret's
// CELL, projected as a one-line document, and no motion, operator or command
// can address anything else. A breach is not a cosmetic bug - removing a
// selection which crosses a QTextTable frame boundary removes the frame, and
// the sheet is then holding a dangling QTextTable.
class TestTablePreviewInputMode : public QObject {
  Q_OBJECT
private slots:
  void initTestCase();

  // Installation and lifetime.
  void testTheSheetTakesEachOfTheThreeModes();
  void testTheModeIsBuiltOnFirstInteraction();
  void testAModeSwitchLeavesTheTableIntact();
  void testTheSheetSurvivesDestructionWithAnActiveMode();

  // Cell confinement: motions.
  void testVerticalMotionsStayInTheCell();
  void testDocumentMotionsStayInTheCell();
  void testViewportMotionsStayInTheCell();
  void testParagraphMotionStaysInTheCell();

  // Cell confinement: operators.
  void testLineOperatorsOnlyClearTheCell();
  void testChangeLineOperatorsOnlyClearTheCell();
  void testOpenLineInsertsNoRow();
  void testALinewisePutStaysInTheCell();
  void testVisualBlockModeDoesNotAssert();
  void testInsertExitAtCellStartStaysInTheCell();
  void testVscodeLineOperationsStayInTheCell();

  // Per-key precedence: the table vocabulary wins in every mode.
  void testTableKeysWinInEveryMode();

  // Undo (decision D2).
  void testTheRingReplaysAModeDrivenOperator();
  void testACompoundCommandIsOneUndoStep();
  void testAnInsertSessionIsOneUndoStep();
  void testAChangeLineIsOneUndoStep();
  void testARepeatedChangeIsOneUndoStep();
  void testAnOperatorBeforeAnInsertIsItsOwnStep();
  void testAMergeDropsTheRing();
  void testAReversibleStructuralPairAlsoDropsTheRing();

  // The original ask.
  void testInputMethodFollowsTheSheetsMode();
  void testAForcedDisableDoesNotUnstickTheMode();
  void testABackgroundSheetDefersItsInputMethodState();

  // Decision D5.
  void testRegistersCrossBetweenSheets();
};
} // namespace tests

#endif // TESTS_TEST_TABLEPREVIEWINPUTMODE_H
