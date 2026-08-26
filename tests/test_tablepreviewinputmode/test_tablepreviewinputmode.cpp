#include "test_tablepreviewinputmode.h"

#include <QApplication>
#include <QSignalSpy>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextTable>
#include <QTextTableCell>

#include <inputmode/abstractinputmode.h>
#include <vtextedit/global.h>
#include <vtextedit/preview.h>

#include "previewbuilder.h"
#include "tablepreviewwidget.h"

using namespace tests;
using namespace vte;

namespace {
QSharedPointer<const TablePreview> makeTable(const QVector<QVector<QString>> &p_cells) {
  QVector<PreviewTableAlignment> alignments;
  for (int i = 0; i < p_cells.value(0).size(); ++i) {
    alignments.append(PreviewTableAlignment::None);
  }

  const QVector<QString> prefixes(p_cells.size(), QString());
  const QString source =
      TablePreviewSerializer::serialize(p_cells, alignments, prefixes, QString());
  auto preview = PreviewBuilder::createTable(1, 0, source.size(), source, alignments.size(),
                                             p_cells, alignments, prefixes, QString(),
                                             QVector<QVector<QVector<PreviewFormatRun>>>());
  return preview.staticCast<const TablePreview>();
}

// A 2x2 table whose body cells hold something an operator can visibly destroy.
QSharedPointer<const TablePreview> makeSampleTable() {
  QVector<QVector<QString>> cells;
  cells.append({QStringLiteral("h1"), QStringLiteral("h2")});
  cells.append({QStringLiteral("alpha"), QStringLiteral("beta")});
  return makeTable(cells);
}

QTextTable *tableOf(const QTextDocument *p_doc) {
  if (!p_doc || !p_doc->rootFrame()) {
    return nullptr;
  }

  const auto children = p_doc->rootFrame()->childFrames();
  for (QTextFrame *frame : children) {
    if (auto table = qobject_cast<QTextTable *>(frame)) {
      return table;
    }
  }

  return nullptr;
}

QString cellText(const QTextDocument *p_doc, int p_row, int p_column) {
  QTextTable *table = tableOf(p_doc);
  if (!table) {
    return QString();
  }

  const QTextTableCell cell = table->cellAt(p_row, p_column);
  if (!cell.isValid()) {
    return QString();
  }

  QTextCursor cursor = cell.firstCursorPosition();
  cursor.setPosition(cell.lastPosition(), QTextCursor::KeepAnchor);
  return cursor.selectedText();
}

TablePreviewSheet *sheetOf(TablePreviewWidget &p_widget) {
  return p_widget.findChild<TablePreviewSheet *>();
}

void putCaretIn(TablePreviewSheet *p_sheet, int p_row, int p_column, bool p_atEnd = false) {
  QTextTable *table = tableOf(p_sheet->document());
  QVERIFY2(table, "the sheet has no table");
  const QTextTableCell cell = table->cellAt(p_row, p_column);
  QVERIFY2(cell.isValid(), "no such cell");
  p_sheet->setTextCursor(p_atEnd ? cell.lastCursorPosition() : cell.firstCursorPosition());
}

// Row major index of the caret's cell, which is what the sheet reports.
int caretCell(TablePreviewSheet *p_sheet) { return p_sheet->currentCellIndex(); }

// A sheet in @p_mode, shown off-screen so its layout and resize events are
// real.
//
// Off-screen is not enough for anything which depends on the INPUT CONTEXT:
// QInputMethod acts on the application's focus object, and a widget in a
// window which was never activated never becomes one. Those tests pass
// p_visible.
TablePreviewWidget *
buildSheet(QScopedPointer<TablePreviewWidget> &p_holder, InputMode p_mode,
           const QSharedPointer<const TablePreview> &p_table = QSharedPointer<const TablePreview>(),
           bool p_visible = false) {
  p_holder.reset(new TablePreviewWidget(nullptr, nullptr));
  if (!p_holder->setPreview(p_table ? p_table : makeSampleTable())) {
    return nullptr;
  }

  p_holder->setInputMode(p_mode);

  if (!p_visible) {
    p_holder->setAttribute(Qt::WA_DontShowOnScreen, true);
  }
  p_holder->show();
  p_holder->resize(600, qMax(1, p_holder->heightForWidth(600)));
  if (p_visible) {
    if (!QTest::qWaitForWindowExposed(p_holder.data())) {
      return nullptr;
    }
    p_holder->activateWindow();
    QTest::qWaitForWindowActive(p_holder.data());
  }
  QCoreApplication::processEvents();
  return p_holder.data();
}

// Give @p_sheet the real keyboard focus. The input method contract cannot be
// observed without one, so a platform which will not grant it makes the test
// skip rather than fail.
bool takeFocus(TablePreviewSheet *p_sheet) {
  p_sheet->setFocus();
  QCoreApplication::processEvents();
  return p_sheet->hasFocus();
}

EditorMode editorModeOf(TablePreviewSheet *p_sheet) {
  auto mode = p_sheet->getInputMode();
  return mode ? mode->editorMode() : EditorMode::NormalModeInsert;
}
} // namespace

void TestTablePreviewInputMode::initTestCase() {
  // Nothing global to set up; every test owns its own widget.
}

// ---------------------------------------------------------------------------
// Installation and lifetime
// ---------------------------------------------------------------------------

void TestTablePreviewInputMode::testTheSheetTakesEachOfTheThreeModes() {
  const InputMode modes[] = {InputMode::NormalMode, InputMode::ViMode, InputMode::VscodeMode};
  for (InputMode mode : modes) {
    QScopedPointer<TablePreviewWidget> holder;
    auto widget = buildSheet(holder, mode);
    QVERIFY(widget);
    auto sheet = sheetOf(*widget);
    QVERIFY(sheet);

    // Decision D5: recorded, not built. A note can hold dozens of previewed
    // tables and a Vi mode is a whole KateVi::InputModeManager plus a command
    // bar.
    QVERIFY2(!sheet->getInputMode(), "the mode was created before the sheet was interacted with");

    sheet->ensureInputMode();
    auto installed = sheet->getInputMode();
    QVERIFY2(installed, "no input mode was installed");
    QCOMPARE(installed->mode(), mode);
  }
}

// The lazy install fires on the first interaction, and a configuration change
// before it still lands.
void TestTablePreviewInputMode::testTheModeIsBuiltOnFirstInteraction() {
  QScopedPointer<TablePreviewWidget> holder;
  auto widget = buildSheet(holder, InputMode::NormalMode);
  QVERIFY(widget);
  auto sheet = sheetOf(*widget);
  QVERIFY(sheet);
  QVERIFY(!sheet->getInputMode());

  // Changed again while still unbuilt: the LATEST value is the one built.
  widget->setInputMode(InputMode::ViMode);
  QVERIFY2(!sheet->getInputMode(), "a mode change must not build the mode either");

  putCaretIn(sheet, 1, 0);
  QTest::keyClick(sheet, Qt::Key_X);
  QVERIFY(sheet->getInputMode());
  QCOMPARE(sheet->getInputMode()->mode(), InputMode::ViMode);
  // Vi normal mode: x deleted a character, so the key really did reach the
  // mode that the same event installed.
  QCOMPARE(cellText(sheet->document(), 1, 0), QStringLiteral("lpha"));

  // And once built, a change replaces it at once rather than waiting again.
  widget->setInputMode(InputMode::VscodeMode);
  QCOMPARE(sheet->getInputMode()->mode(), InputMode::VscodeMode);
}

void TestTablePreviewInputMode::testAModeSwitchLeavesTheTableIntact() {
  QScopedPointer<TablePreviewWidget> holder;
  auto widget = buildSheet(holder, InputMode::NormalMode);
  QVERIFY(widget);
  auto sheet = sheetOf(*widget);
  QVERIFY(sheet);
  sheet->ensureInputMode();

  // Decision D7: every switch goes through VTextEdit::setInputMode(), which
  // deactivates the outgoing mode and activates the incoming one exactly once.
  // A double activate() or a manual deactivate() trips ViInputMode's own
  // assertions, which is what a debug build of this test catches.
  widget->setInputMode(InputMode::ViMode);
  QCOMPARE(sheet->getInputMode()->mode(), InputMode::ViMode);

  widget->setInputMode(InputMode::VscodeMode);
  QCOMPARE(sheet->getInputMode()->mode(), InputMode::VscodeMode);

  widget->setInputMode(InputMode::ViMode);
  QCOMPARE(sheet->getInputMode()->mode(), InputMode::ViMode);

  QVERIFY(sheet->tableDocument()->isIntact());
  QCOMPARE(cellText(sheet->document(), 1, 0), QStringLiteral("alpha"));
}

void TestTablePreviewInputMode::testTheSheetSurvivesDestructionWithAnActiveMode() {
  QScopedPointer<TablePreviewWidget> holder;
  auto widget = buildSheet(holder, InputMode::ViMode);
  QVERIFY(widget);
  auto sheet = sheetOf(*widget);
  QVERIFY(sheet);
  putCaretIn(sheet, 1, 0);
  QTest::keyClick(sheet, Qt::Key_I);

  // The mode holds the interface by RAW pointer and VTextEdit destroys the
  // mode from ~VTextEdit, i.e. after TablePreviewSheet's own members are gone.
  // A Vi status bar which is still parented asserts on the way out too.
  holder.reset();
  QCoreApplication::processEvents();
}

// ---------------------------------------------------------------------------
// Cell confinement: motions
// ---------------------------------------------------------------------------

void TestTablePreviewInputMode::testVerticalMotionsStayInTheCell() {
  QScopedPointer<TablePreviewWidget> holder;
  auto widget = buildSheet(holder, InputMode::ViMode);
  QVERIFY(widget);
  auto sheet = sheetOf(*widget);
  QVERIFY(sheet);

  putCaretIn(sheet, 1, 0);
  const int cell = caretCell(sheet);

  // j and k resolve through goVisualLineUpDownDry(), which reports failure for
  // a one-line buffer. The base would answer with QTextCursor::Up/Down, which
  // walks straight into the neighbouring cell.
  for (int i = 0; i < 4; ++i) {
    QTest::keyClick(sheet, Qt::Key_J);
  }
  QCOMPARE(caretCell(sheet), cell);

  for (int i = 0; i < 4; ++i) {
    QTest::keyClick(sheet, Qt::Key_K);
  }
  QCOMPARE(caretCell(sheet), cell);
}

void TestTablePreviewInputMode::testDocumentMotionsStayInTheCell() {
  QScopedPointer<TablePreviewWidget> holder;
  auto widget = buildSheet(holder, InputMode::ViMode);
  QVERIFY(widget);
  auto sheet = sheetOf(*widget);
  QVERIFY(sheet);

  putCaretIn(sheet, 1, 1);
  const int cell = caretCell(sheet);

  // G is "the last line", gg is "the first". Both go through updateCursor(),
  // whose base resolves a line number with findBlockByNumber() - block 0 of
  // the SHEET, which is the top-left cell.
  QTest::keyClicks(sheet, QStringLiteral("G"));
  QCOMPARE(caretCell(sheet), cell);

  QTest::keyClick(sheet, Qt::Key_G);
  QTest::keyClick(sheet, Qt::Key_G);
  QCOMPARE(caretCell(sheet), cell);
}

void TestTablePreviewInputMode::testViewportMotionsStayInTheCell() {
  QScopedPointer<TablePreviewWidget> holder;
  auto widget = buildSheet(holder, InputMode::ViMode);
  QVERIFY(widget);
  auto sheet = sheetOf(*widget);
  QVERIFY(sheet);

  putCaretIn(sheet, 1, 1);
  const int cell = caretCell(sheet);

  // H, M, L and the page motions all measure themselves against
  // linesDisplayed(), which the base answers with the number of visible blocks
  // of the WHOLE SHEET.
  QTest::keyClicks(sheet, QStringLiteral("H"));
  QCOMPARE(caretCell(sheet), cell);
  QTest::keyClicks(sheet, QStringLiteral("M"));
  QCOMPARE(caretCell(sheet), cell);
  QTest::keyClicks(sheet, QStringLiteral("L"));
  QCOMPARE(caretCell(sheet), cell);

  QTest::keyClick(sheet, Qt::Key_F, Qt::ControlModifier);
  QCOMPARE(caretCell(sheet), cell);
  QTest::keyClick(sheet, Qt::Key_B, Qt::ControlModifier);
  QCOMPARE(caretCell(sheet), cell);
  QTest::keyClick(sheet, Qt::Key_D, Qt::ControlModifier);
  QCOMPARE(caretCell(sheet), cell);
}

void TestTablePreviewInputMode::testParagraphMotionStaysInTheCell() {
  QScopedPointer<TablePreviewWidget> holder;
  auto widget = buildSheet(holder, InputMode::ViMode);
  QVERIFY(widget);
  auto sheet = sheetOf(*widget);
  QVERIFY(sheet);

  putCaretIn(sheet, 0, 0);
  const int cell = caretCell(sheet);

  QTest::keyClicks(sheet, QStringLiteral("}"));
  QCOMPARE(caretCell(sheet), cell);
  QTest::keyClicks(sheet, QStringLiteral("{"));
  QCOMPARE(caretCell(sheet), cell);
}

// ---------------------------------------------------------------------------
// Cell confinement: operators
// ---------------------------------------------------------------------------

void TestTablePreviewInputMode::testLineOperatorsOnlyClearTheCell() {
  QScopedPointer<TablePreviewWidget> holder;
  auto widget = buildSheet(holder, InputMode::ViMode);
  QVERIFY(widget);
  auto sheet = sheetOf(*widget);
  QVERIFY(sheet);

  QTextTable *table = tableOf(sheet->document());
  QVERIFY(table);
  const int rows = table->rows();
  const int columns = table->columns();

  putCaretIn(sheet, 1, 0);
  QTest::keyClicks(sheet, QStringLiteral("dd"));

  // removeLine(0) clears the cell's TEXT and never removes the block: a cell
  // is one block, and removing it takes the QTextTableCell with it.
  QCOMPARE(cellText(sheet->document(), 1, 0), QString());
  QCOMPARE(cellText(sheet->document(), 1, 1), QStringLiteral("beta"));
  QVERIFY(sheet->tableDocument()->isIntact());
  QCOMPARE(tableOf(sheet->document())->rows(), rows);
  QCOMPARE(tableOf(sheet->document())->columns(), columns);
}

void TestTablePreviewInputMode::testChangeLineOperatorsOnlyClearTheCell() {
  QScopedPointer<TablePreviewWidget> holder;
  auto widget = buildSheet(holder, InputMode::ViMode);
  QVERIFY(widget);
  auto sheet = sheetOf(*widget);
  QVERIFY(sheet);

  putCaretIn(sheet, 1, 0);
  QTest::keyClicks(sheet, QStringLiteral("cc"));
  QTest::keyClicks(sheet, QStringLiteral("xy"));
  QTest::keyClick(sheet, Qt::Key_Escape);

  QCOMPARE(cellText(sheet->document(), 1, 0), QStringLiteral("xy"));
  QCOMPARE(cellText(sheet->document(), 0, 0), QStringLiteral("h1"));
  QVERIFY(sheet->tableDocument()->isIntact());

  // S is the same operator under another name.
  putCaretIn(sheet, 1, 1);
  QTest::keyClicks(sheet, QStringLiteral("S"));
  QTest::keyClicks(sheet, QStringLiteral("z"));
  QTest::keyClick(sheet, Qt::Key_Escape);

  QCOMPARE(cellText(sheet->document(), 1, 1), QStringLiteral("z"));
  QVERIFY(sheet->tableDocument()->isIntact());
  QCOMPARE(tableOf(sheet->document())->rows(), 2);
}

void TestTablePreviewInputMode::testOpenLineInsertsNoRow() {
  QScopedPointer<TablePreviewWidget> holder;
  auto widget = buildSheet(holder, InputMode::ViMode);
  QVERIFY(widget);
  auto sheet = sheetOf(*widget);
  QVERIFY(sheet);

  const int blocks = sheet->document()->blockCount();

  putCaretIn(sheet, 1, 0);
  QTest::keyClick(sheet, Qt::Key_O);
  QTest::keyClick(sheet, Qt::Key_Escape);
  QTest::keyClicks(sheet, QStringLiteral("O"));
  QTest::keyClick(sheet, Qt::Key_Escape);

  // insertLine() refuses: a row is one source line and the serializer rejects
  // every separator that could end one. "One more row" is Enter in the last
  // cell, which is table vocabulary rather than a mode command.
  QCOMPARE(sheet->document()->blockCount(), blocks);
  QCOMPARE(tableOf(sheet->document())->rows(), 2);
  QVERIFY(sheet->tableDocument()->isIntact());
}

void TestTablePreviewInputMode::testALinewisePutStaysInTheCell() {
  QScopedPointer<TablePreviewWidget> holder;
  auto widget = buildSheet(holder, InputMode::ViMode);
  QVERIFY(widget);
  auto sheet = sheetOf(*widget);
  QVERIFY(sheet);

  // yy fills the register linewise, i.e. with a trailing separator.
  const int blocks = sheet->document()->blockCount();
  putCaretIn(sheet, 1, 0);
  QTest::keyClicks(sheet, QStringLiteral("yy"));

  putCaretIn(sheet, 1, 1);
  QTest::keyClick(sheet, Qt::Key_P);

  // Whatever the register held, it landed inside one cell and carried no
  // separator with it: insertText() runs every payload through the sheet's
  // sanitizer, which is what a paste goes through too.
  QVERIFY(sheet->tableDocument()->isIntact());
  QCOMPARE(tableOf(sheet->document())->rows(), 2);
  QCOMPARE(tableOf(sheet->document())->columns(), 2);
  QCOMPARE(sheet->document()->blockCount(), blocks);
  QVERIFY(!cellText(sheet->document(), 1, 1).contains(QChar(QChar::ParagraphSeparator)));
  QVERIFY(!cellText(sheet->document(), 1, 1).contains(QLatin1Char('\n')));
}

void TestTablePreviewInputMode::testVisualBlockModeDoesNotAssert() {
  QScopedPointer<TablePreviewWidget> holder;
  auto widget = buildSheet(holder, InputMode::ViMode);
  QVERIFY(widget);
  auto sheet = sheetOf(*widget);
  QVERIFY(sheet);

  putCaretIn(sheet, 1, 0);

  // The base answers setBlockSelection(true) with Q_ASSERT(!p_enabled), so
  // this is a debug-build crash without the override.
  QTest::keyClick(sheet, Qt::Key_V, Qt::ControlModifier);
  QTest::keyClick(sheet, Qt::Key_L);
  QTest::keyClick(sheet, Qt::Key_Escape);

  QVERIFY(sheet->tableDocument()->isIntact());
  QCOMPARE(cellText(sheet->document(), 1, 0), QStringLiteral("alpha"));
}

void TestTablePreviewInputMode::testInsertExitAtCellStartStaysInTheCell() {
  QScopedPointer<TablePreviewWidget> holder;
  auto widget = buildSheet(holder, InputMode::ViMode);
  QVERIFY(widget);
  auto sheet = sheetOf(*widget);
  QVERIFY(sheet);

  putCaretIn(sheet, 1, 1);
  const int cell = caretCell(sheet);

  // Leaving insert mode runs cursorPrevChar(), whose base moves one
  // PreviousCharacter unconditionally - at a cell's first position that is the
  // block separator, i.e. the previous cell.
  QTest::keyClick(sheet, Qt::Key_I);
  QTest::keyClick(sheet, Qt::Key_Escape);

  QCOMPARE(caretCell(sheet), cell);
  QCOMPARE(cellText(sheet->document(), 1, 1), QStringLiteral("beta"));
}

void TestTablePreviewInputMode::testVscodeLineOperationsStayInTheCell() {
  QScopedPointer<TablePreviewWidget> holder;
  auto widget = buildSheet(holder, InputMode::VscodeMode);
  QVERIFY(widget);
  auto sheet = sheetOf(*widget);
  QVERIFY(sheet);

  QTextTable *table = tableOf(sheet->document());
  QVERIFY(table);
  const int rows = table->rows();
  const int columns = table->columns();
  const int blocks = sheet->document()->blockCount();

  putCaretIn(sheet, 1, 0);

  // Every one of these reaches the document only through the projected
  // interface, so decision D1 confines them with no vscode-specific code.
  // Move line up/down, duplicate line down, delete line, select line.
  QTest::keyClick(sheet, Qt::Key_Up, Qt::AltModifier);
  QTest::keyClick(sheet, Qt::Key_Down, Qt::AltModifier);
  QTest::keyClick(sheet, Qt::Key_Down, Qt::AltModifier | Qt::ShiftModifier);
  QTest::keyClick(sheet, Qt::Key_L, Qt::ControlModifier);
  QTest::keyClick(sheet, Qt::Key_K, Qt::ControlModifier);
  QTest::keyClick(sheet, Qt::Key_K, Qt::ControlModifier | Qt::ShiftModifier);

  QVERIFY(sheet->tableDocument()->isIntact());
  QCOMPARE(tableOf(sheet->document())->rows(), rows);
  QCOMPARE(tableOf(sheet->document())->columns(), columns);
  QCOMPARE(sheet->document()->blockCount(), blocks);
  QCOMPARE(cellText(sheet->document(), 0, 0), QStringLiteral("h1"));
}

// ---------------------------------------------------------------------------
// Per-key precedence
// ---------------------------------------------------------------------------

void TestTablePreviewInputMode::testTableKeysWinInEveryMode() {
  const InputMode modes[] = {InputMode::NormalMode, InputMode::ViMode, InputMode::VscodeMode};
  for (InputMode mode : modes) {
    QScopedPointer<TablePreviewWidget> holder;
    auto widget = buildSheet(holder, mode);
    QVERIFY(widget);
    auto sheet = sheetOf(*widget);
    QVERIFY(sheet);

    // Tab and Backtab move between cells, and hand the caret back at the ends
    // (decision D3). katevi never sees them: they are intercepted ahead of the
    // mode, precisely so all three modes agree.
    QSignalSpy escapes(widget, &TablePreviewWidget::focusEscapeRequested);
    putCaretIn(sheet, 0, 0);
    QTest::keyClick(sheet, Qt::Key_Tab);
    QCOMPARE(caretCell(sheet), 1);
    QTest::keyClick(sheet, Qt::Key_Backtab);
    QCOMPARE(caretCell(sheet), 0);
    QCOMPARE(escapes.count(), 0);

    QTest::keyClick(sheet, Qt::Key_Backtab);
    QCOMPARE(escapes.count(), 1);
    QCOMPARE(escapes.last().at(0).value<FocusEscapeDirection>(), FocusEscapeDirection::Up);

    // Enter in the last cell of the last row appends a row - and nowhere else
    // does it do anything at all. Return is a normal-mode MOTION in katevi, so
    // without the interception this would differ per mode.
    QTest::keyClick(sheet, Qt::Key_Return);
    QCOMPARE(tableOf(sheet->document())->rows(), 2);

    putCaretIn(sheet, 1, 1, true);
    QTest::keyClick(sheet, Qt::Key_Return);
    QCOMPARE(tableOf(sheet->document())->rows(), 3);
    QVERIFY(sheet->tableDocument()->isIntact());

    // Tab out of the now-last cell hands back downwards.
    putCaretIn(sheet, 2, 1, true);
    QTest::keyClick(sheet, Qt::Key_Tab);
    QCOMPARE(escapes.count(), 2);
    QCOMPARE(escapes.last().at(0).value<FocusEscapeDirection>(), FocusEscapeDirection::Down);

    // The edge arrows hand the caret back too.
    putCaretIn(sheet, 0, 0);
    QTest::keyClick(sheet, Qt::Key_Up);
    QCOMPARE(escapes.count(), 3);
    QCOMPARE(escapes.last().at(0).value<FocusEscapeDirection>(), FocusEscapeDirection::Up);
  }
}

// ---------------------------------------------------------------------------
// Undo (decision D2)
// ---------------------------------------------------------------------------

void TestTablePreviewInputMode::testTheRingReplaysAModeDrivenOperator() {
  QScopedPointer<TablePreviewWidget> holder;
  auto widget = buildSheet(holder, InputMode::ViMode);
  QVERIFY(widget);
  auto sheet = sheetOf(*widget);
  QVERIFY(sheet);

  putCaretIn(sheet, 1, 0);
  QTest::keyClick(sheet, Qt::Key_X);
  QCOMPARE(cellText(sheet->document(), 1, 0), QStringLiteral("lpha"));

  QTest::keyClick(sheet, Qt::Key_X);
  QCOMPARE(cellText(sheet->document(), 1, 0), QStringLiteral("pha"));

  // u replays the ring, never QTextDocument::undo() - the inner stack is
  // disabled precisely because a document step would also revert a structural
  // mutation without the metadata beside it.
  QTest::keyClick(sheet, Qt::Key_U);
  QCOMPARE(cellText(sheet->document(), 1, 0), QStringLiteral("lpha"));

  QTest::keyClick(sheet, Qt::Key_U);
  QCOMPARE(cellText(sheet->document(), 1, 0), QStringLiteral("alpha"));

  // Exhausted: nothing more to give, and the cell is left alone.
  QTest::keyClick(sheet, Qt::Key_U);
  QCOMPARE(cellText(sheet->document(), 1, 0), QStringLiteral("alpha"));

  QTest::keyClick(sheet, Qt::Key_R, Qt::ControlModifier);
  QCOMPARE(cellText(sheet->document(), 1, 0), QStringLiteral("lpha"));

  QVERIFY(sheet->tableDocument()->isIntact());
}

// One katevi command is one undo step, even when it drives several mutating
// interface calls.
void TestTablePreviewInputMode::testACompoundCommandIsOneUndoStep() {
  QScopedPointer<TablePreviewWidget> holder;
  auto widget = buildSheet(holder, InputMode::ViMode);
  QVERIFY(widget);
  auto sheet = sheetOf(*widget);
  QVERIFY(sheet);
  sheet->ensureInputMode();

  // Fill the register, then replace a visual selection with it. A visual put
  // is editStart(), removeText(), insertText(), editEnd() - two mutating calls
  // in one session. A checkpoint per CALL would record the intermediate
  // "selection deleted, replacement not yet inserted" state as a step of its
  // own, and one `u` would leave the cell mangled.
  putCaretIn(sheet, 0, 0);
  QTest::keyClicks(sheet, QStringLiteral("yy"));

  putCaretIn(sheet, 1, 0);
  const QString before = cellText(sheet->document(), 1, 0);
  QCOMPARE(before, QStringLiteral("alpha"));

  QTest::keyClick(sheet, Qt::Key_V);
  QTest::keyClicks(sheet, QStringLiteral("ll"));
  QTest::keyClick(sheet, Qt::Key_P);
  const QString after = cellText(sheet->document(), 1, 0);
  QVERIFY2(after != before, qPrintable(after));

  QTest::keyClick(sheet, Qt::Key_U);
  QCOMPARE(cellText(sheet->document(), 1, 0), before);
  QVERIFY(sheet->tableDocument()->isIntact());
}

// A whole Vi insert session is one undo step, not one per character.
void TestTablePreviewInputMode::testAnInsertSessionIsOneUndoStep() {
  QScopedPointer<TablePreviewWidget> holder;
  auto widget = buildSheet(holder, InputMode::ViMode);
  QVERIFY(widget);
  auto sheet = sheetOf(*widget);
  QVERIFY(sheet);
  sheet->ensureInputMode();

  putCaretIn(sheet, 1, 0);
  QCOMPARE(cellText(sheet->document(), 1, 0), QStringLiteral("alpha"));

  // Insert mode is NOT an open edit session: katevi turns setUndoMergeAllEdits
  // on for its duration, but every key in it still gets its own
  // editStart()/editEnd() pair, so the session count is back to 0 between
  // characters. Checkpointing on those would make `u` undo one character.
  QTest::keyClick(sheet, Qt::Key_I);
  QTest::keyClicks(sheet, QStringLiteral("xyz"));
  QTest::keyClick(sheet, Qt::Key_Escape);
  QCOMPARE(cellText(sheet->document(), 1, 0), QStringLiteral("xyzalpha"));

  QTest::keyClick(sheet, Qt::Key_U);
  QCOMPARE(cellText(sheet->document(), 1, 0), QStringLiteral("alpha"));
  QVERIFY(sheet->tableDocument()->isIntact());
}

// cc turns merge mode on BEFORE it deletes
// (NormalViMode::commandChangeLine), so the whole delete-then-retype is one
// step: one `u` restores the original text, never the empty cell.
void TestTablePreviewInputMode::testAChangeLineIsOneUndoStep() {
  QScopedPointer<TablePreviewWidget> holder;
  auto widget = buildSheet(holder, InputMode::ViMode);
  QVERIFY(widget);
  auto sheet = sheetOf(*widget);
  QVERIFY(sheet);
  sheet->ensureInputMode();

  putCaretIn(sheet, 1, 0);
  QTest::keyClicks(sheet, QStringLiteral("cc"));
  QTest::keyClicks(sheet, QStringLiteral("new"));
  QTest::keyClick(sheet, Qt::Key_Escape);
  QCOMPARE(cellText(sheet->document(), 1, 0), QStringLiteral("new"));

  QTest::keyClick(sheet, Qt::Key_U);
  QVERIFY2(cellText(sheet->document(), 1, 0) == QStringLiteral("alpha"),
           qPrintable(cellText(sheet->document(), 1, 0)));
  QVERIFY(sheet->tableDocument()->isIntact());

  // C and s are the DELETE-BEFORE-MERGE shape: commandChangeToEOL() and
  // commandSubstituteChar() delete first and only then enter insert mode, so
  // the merge edge is not where the command began. What protects them is the
  // checkpoint the deletion's own editStart() already took during the same
  // top-level key dispatch.
  putCaretIn(sheet, 1, 1);
  QTest::keyClicks(sheet, QStringLiteral("C"));
  QTest::keyClicks(sheet, QStringLiteral("zz"));
  QTest::keyClick(sheet, Qt::Key_Escape);
  QVERIFY(cellText(sheet->document(), 1, 1) != QStringLiteral("beta"));

  QTest::keyClick(sheet, Qt::Key_U);
  QCOMPARE(cellText(sheet->document(), 1, 1), QStringLiteral("beta"));
  QVERIFY(sheet->tableDocument()->isIntact());
}

// The same change REPLAYED. A `.` repeat opens an outer edit session and then
// feeds synthetic key events back through the sheet, so the nested `C` must
// not be mistaken for a new top-level command - if it were, it would
// checkpoint the cell it had just emptied.
void TestTablePreviewInputMode::testARepeatedChangeIsOneUndoStep() {
  QScopedPointer<TablePreviewWidget> holder;
  auto widget = buildSheet(holder, InputMode::ViMode);
  QVERIFY(widget);
  auto sheet = sheetOf(*widget);
  QVERIFY(sheet);
  sheet->ensureInputMode();

  putCaretIn(sheet, 1, 0);
  QTest::keyClicks(sheet, QStringLiteral("C"));
  QTest::keyClicks(sheet, QStringLiteral("zz"));
  QTest::keyClick(sheet, Qt::Key_Escape);
  QCOMPARE(cellText(sheet->document(), 1, 0), QStringLiteral("zz"));

  // A different cell, so the repeat has something to change.
  putCaretIn(sheet, 1, 1);
  QCOMPARE(cellText(sheet->document(), 1, 1), QStringLiteral("beta"));
  QTest::keyClick(sheet, Qt::Key_Period);
  QVERIFY2(cellText(sheet->document(), 1, 1) != QStringLiteral("beta"),
           "the repeat changed nothing, so this proves nothing");

  QTest::keyClick(sheet, Qt::Key_U);
  QVERIFY2(cellText(sheet->document(), 1, 1) == QStringLiteral("beta"),
           qPrintable(cellText(sheet->document(), 1, 1)));
  QVERIFY(sheet->tableDocument()->isIntact());
}

// An operator and a following, independent insert session are two steps, not
// one. This is what the merge-EDGE checkpoint buys over merely suppressing
// checkpoints for the duration of the window.
void TestTablePreviewInputMode::testAnOperatorBeforeAnInsertIsItsOwnStep() {
  QScopedPointer<TablePreviewWidget> holder;
  auto widget = buildSheet(holder, InputMode::ViMode);
  QVERIFY(widget);
  auto sheet = sheetOf(*widget);
  QVERIFY(sheet);
  sheet->ensureInputMode();

  putCaretIn(sheet, 1, 0);
  QTest::keyClick(sheet, Qt::Key_X);
  QCOMPARE(cellText(sheet->document(), 1, 0), QStringLiteral("lpha"));

  QTest::keyClick(sheet, Qt::Key_I);
  QTest::keyClicks(sheet, QStringLiteral("QQ"));
  QTest::keyClick(sheet, Qt::Key_Escape);
  QCOMPARE(cellText(sheet->document(), 1, 0), QStringLiteral("QQlpha"));

  // The first u undoes the INSERT ONLY.
  QTest::keyClick(sheet, Qt::Key_U);
  QVERIFY2(cellText(sheet->document(), 1, 0) == QStringLiteral("lpha"),
           qPrintable(cellText(sheet->document(), 1, 0)));

  // The second undoes the x.
  QTest::keyClick(sheet, Qt::Key_U);
  QCOMPARE(cellText(sheet->document(), 1, 0), QStringLiteral("alpha"));
  QVERIFY(sheet->tableDocument()->isIntact());
}

void TestTablePreviewInputMode::testAMergeDropsTheRing() {
  QScopedPointer<TablePreviewWidget> holder;
  auto widget = buildSheet(holder, InputMode::ViMode);
  QVERIFY(widget);
  auto sheet = sheetOf(*widget);
  QVERIFY(sheet);

  putCaretIn(sheet, 1, 0);
  QTest::keyClick(sheet, Qt::Key_X);
  QCOMPARE(cellText(sheet->document(), 1, 0), QStringLiteral("lpha"));
  QVERIFY(sheet->undoRingDepth() > 0);

  // A structural mutation moves the geometry a ring entry was recorded
  // against. The ring is keyed to the document's monotonic structure
  // generation plus a fingerprint of the live shape, so it invalidates itself
  // rather than relying on every operation to remember to clear it.
  QTextTable *table = tableOf(sheet->document());
  QVERIFY(table);
  QTextCursor rect = table->cellAt(1, 0).firstCursorPosition();
  rect.setPosition(table->cellAt(1, 1).lastPosition(), QTextCursor::KeepAnchor);
  QVERIFY(rect.hasComplexSelection());
  QVERIFY(sheet->tableDocument()->canMergeCells(rect));
  QVERIFY(sheet->tableDocument()->mergeCells(rect));

  QCOMPARE(sheet->undoRingDepth(), 0);
  QCOMPARE(sheet->redoRingDepth(), 0);

  // And a replay after it is a no-op rather than a revert of a cell which no
  // longer has the shape it was recorded with.
  const QString merged = cellText(sheet->document(), 1, 0);
  QVERIFY(!sheet->undoFromRing());
  QCOMPARE(cellText(sheet->document(), 1, 0), merged);
  QVERIFY(sheet->tableDocument()->isIntact());
}

// The same, for a REVERSIBLE pair of structural operations. A merge followed
// by a split restores the row and column counts and every slot's owner, so a
// ring keyed only to the shape would come back to life - and replay a
// pre-merge cell over a post-split one, whose text the merge rearranged.
void TestTablePreviewInputMode::testAReversibleStructuralPairAlsoDropsTheRing() {
  QScopedPointer<TablePreviewWidget> holder;
  auto widget = buildSheet(holder, InputMode::ViMode);
  QVERIFY(widget);
  auto sheet = sheetOf(*widget);
  QVERIFY(sheet);
  sheet->ensureInputMode();

  putCaretIn(sheet, 1, 0);
  QTest::keyClick(sheet, Qt::Key_X);
  QCOMPARE(cellText(sheet->document(), 1, 0), QStringLiteral("lpha"));
  QVERIFY(sheet->undoRingDepth() > 0);

  QTextTable *table = tableOf(sheet->document());
  QVERIFY(table);
  QTextCursor rect = table->cellAt(1, 0).firstCursorPosition();
  rect.setPosition(table->cellAt(1, 1).lastPosition(), QTextCursor::KeepAnchor);
  QVERIFY(sheet->tableDocument()->canMergeCells(rect));
  QVERIFY(sheet->tableDocument()->mergeCells(rect));

  // Straight back to a 2x2 grid, without the ring being consulted in between -
  // so nothing had a chance to notice and clear it on the way.
  QVERIFY(sheet->tableDocument()->canSplitCell(1, 0));
  QVERIFY(sheet->tableDocument()->splitCell(1, 0));

  table = tableOf(sheet->document());
  QVERIFY(table);
  QCOMPARE(table->rows(), 2);
  QCOMPARE(table->columns(), 2);

  QCOMPARE(sheet->undoRingDepth(), 0);
  QCOMPARE(sheet->redoRingDepth(), 0);

  const QString origin = cellText(sheet->document(), 1, 0);
  const QString exposed = cellText(sheet->document(), 1, 1);
  QVERIFY(!sheet->undoFromRing());
  QCOMPARE(cellText(sheet->document(), 1, 0), origin);
  QCOMPARE(cellText(sheet->document(), 1, 1), exposed);
  QVERIFY(sheet->tableDocument()->isIntact());
}

// ---------------------------------------------------------------------------
// The original ask
// ---------------------------------------------------------------------------

void TestTablePreviewInputMode::testInputMethodFollowsTheSheetsMode() {
  QScopedPointer<TablePreviewWidget> holder;
  auto widget =
      buildSheet(holder, InputMode::NormalMode, QSharedPointer<const TablePreview>(), true);
  QVERIFY(widget);
  auto sheet = sheetOf(*widget);
  QVERIFY(sheet);
  if (!takeFocus(sheet)) {
    QSKIP("the platform did not grant the keyboard focus");
  }

  // Ordinary typing: the input method is on, which is what it has always been.
  QVERIFY(sheet->inputMethodQuery(Qt::ImEnabled).toBool());
  QVERIFY(sheet->isTextInsertingMode());

  widget->setInputMode(InputMode::ViMode);
  putCaretIn(sheet, 1, 0);

  // Vi starts in normal mode, where every printable key is a command: an input
  // method would swallow the keystroke and compose with it instead.
  QCOMPARE(editorModeOf(sheet), EditorMode::ViModeNormal);
  QVERIFY(!sheet->isTextInsertingMode());
  QVERIFY(!sheet->inputMethodQuery(Qt::ImEnabled).toBool());

  QTest::keyClick(sheet, Qt::Key_I);
  QCOMPARE(editorModeOf(sheet), EditorMode::ViModeInsert);
  QVERIFY(sheet->isTextInsertingMode());
  QVERIFY(sheet->inputMethodQuery(Qt::ImEnabled).toBool());

  QTest::keyClick(sheet, Qt::Key_Escape);
  QCOMPARE(editorModeOf(sheet), EditorMode::ViModeNormal);
  QVERIFY(!sheet->inputMethodQuery(Qt::ImEnabled).toBool());

  // Visual mode is a command mode too.
  QTest::keyClick(sheet, Qt::Key_V);
  QCOMPARE(editorModeOf(sheet), EditorMode::ViModeVisual);
  QVERIFY(!sheet->inputMethodQuery(Qt::ImEnabled).toBool());
  QTest::keyClick(sheet, Qt::Key_Escape);

  // Rapid toggling: each transition resets the input method, and on some
  // platforms the reset re-enters the sheet synchronously. Nothing here may
  // change the document.
  const QString before = cellText(sheet->document(), 1, 0);
  for (int i = 0; i < 20; ++i) {
    QTest::keyClick(sheet, Qt::Key_I);
    QTest::keyClick(sheet, Qt::Key_Escape);
  }
  QCOMPARE(cellText(sheet->document(), 1, 0), before);
  QVERIFY(sheet->tableDocument()->isIntact());

  // Back to a mode with no command layer at all.
  widget->setInputMode(InputMode::NormalMode);
  QVERIFY(sheet->inputMethodQuery(Qt::ImEnabled).toBool());
}

// A process-wide forced disable belongs to the application, and lifting it
// must not leave a command-mode sheet input-method enabled.
void TestTablePreviewInputMode::testAForcedDisableDoesNotUnstickTheMode() {
  QScopedPointer<TablePreviewWidget> holder;
  auto widget =
      buildSheet(holder, InputMode::NormalMode, QSharedPointer<const TablePreview>(), true);
  QVERIFY(widget);
  auto sheet = sheetOf(*widget);
  QVERIFY(sheet);
  if (!takeFocus(sheet)) {
    QSKIP("the platform did not grant the keyboard focus");
  }

  VTextEdit::forceInputMethodDisabled(true);
  QVERIFY(!sheet->inputMethodQuery(Qt::ImEnabled).toBool());

  // The transition happens while the effective ImEnabled is already false. A
  // sync which deduplicated against that effective value would see "already
  // false, nothing to do" and never record the mode's own decision.
  widget->setInputMode(InputMode::ViMode);
  QCOMPARE(editorModeOf(sheet), EditorMode::ViModeNormal);
  QVERIFY(!sheet->inputMethodQuery(Qt::ImEnabled).toBool());

  VTextEdit::forceInputMethodDisabled(false);
  const bool stuck = sheet->inputMethodQuery(Qt::ImEnabled).toBool();
  QVERIFY2(!stuck,
           "lifting the process-wide force must not enable the input method in Vi normal mode");

  QTest::keyClick(sheet, Qt::Key_I);
  QVERIFY(sheet->inputMethodQuery(Qt::ImEnabled).toBool());
  QTest::keyClick(sheet, Qt::Key_Escape);
  QVERIFY(!sheet->inputMethodQuery(Qt::ImEnabled).toBool());
}

// A background sheet may record what its mode wants but must not touch the
// application's input context, which belongs to whichever widget has the focus.
void TestTablePreviewInputMode::testABackgroundSheetDefersItsInputMethodState() {
  // Both are real windows, so both CAN own the input context - but only one
  // does at a time, which is the whole point.
  QScopedPointer<TablePreviewWidget> background;
  QVERIFY(
      buildSheet(background, InputMode::NormalMode, QSharedPointer<const TablePreview>(), true));
  auto backgroundSheet = sheetOf(*background);
  QVERIFY(backgroundSheet);

  QScopedPointer<TablePreviewWidget> focused;
  QVERIFY(buildSheet(focused, InputMode::NormalMode, QSharedPointer<const TablePreview>(), true));
  auto focusedSheet = sheetOf(*focused);
  QVERIFY(focusedSheet);
  if (!takeFocus(focusedSheet)) {
    QSKIP("the platform did not grant the keyboard focus");
  }
  QVERIFY(focusedSheet->inputMethodQuery(Qt::ImEnabled).toBool());
  QVERIFY(!backgroundSheet->hasFocus());

  // The unfocused sheet is switched into a command mode. It already has a mode
  // - it is the only focusable widget in its own window, so activating that
  // window focused it once - but it does not have the focus NOW, which is what
  // this is about: the swap must not reach the platform input context.
  QVERIFY(!backgroundSheet->hasFocus());
  background->setInputMode(InputMode::ViMode);
  QCOMPARE(editorModeOf(backgroundSheet), EditorMode::ViModeNormal);

  // The focused sheet is untouched: nothing reset the input context out from
  // under it, which is what a background QInputMethod::reset() would have done.
  QVERIFY(focusedSheet->hasFocus());
  QVERIFY(focusedSheet->inputMethodQuery(Qt::ImEnabled).toBool());

  // And the deferred state is really applied when the background sheet does
  // take the focus, rather than being swallowed as "already recorded".
  background->activateWindow();
  QTest::qWaitForWindowActive(background.data());
  if (!takeFocus(backgroundSheet)) {
    QSKIP("the platform did not grant the keyboard focus");
  }

  QCOMPARE(editorModeOf(backgroundSheet), EditorMode::ViModeNormal);
  QVERIFY2(!backgroundSheet->inputMethodQuery(Qt::ImEnabled).toBool(),
           "the deferred Vi normal mode state was never applied");
}

// ---------------------------------------------------------------------------
// Shared state (decision D5)
// ---------------------------------------------------------------------------

void TestTablePreviewInputMode::testRegistersCrossBetweenSheets() {
  QScopedPointer<TablePreviewWidget> first;
  QVERIFY(buildSheet(first, InputMode::ViMode));
  auto firstSheet = sheetOf(*first);
  QVERIFY(firstSheet);

  QScopedPointer<TablePreviewWidget> second;
  QVERIFY(buildSheet(second, InputMode::ViMode));
  auto secondSheet = sheetOf(*second);
  QVERIFY(secondSheet);

  // One KateVi::GlobalState for the whole process, so a yank in one buffer is
  // available in every other - which is the point of a register.
  putCaretIn(firstSheet, 1, 0);
  QTest::keyClicks(firstSheet, QStringLiteral("yw"));

  putCaretIn(secondSheet, 1, 1, true);
  QTest::keyClick(secondSheet, Qt::Key_P);

  QVERIFY2(cellText(secondSheet->document(), 1, 1).contains(QStringLiteral("alpha")),
           qPrintable(cellText(secondSheet->document(), 1, 1)));
  QVERIFY(secondSheet->tableDocument()->isIntact());
}

QTEST_MAIN(tests::TestTablePreviewInputMode)
