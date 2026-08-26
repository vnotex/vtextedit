#include "test_richtexteditor.h"

#include <QApplication>
#include <QGuiApplication>
#include <QMimeData>
#include <QSignalSpy>
#include <QStyleHints>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextTable>
#include <QVBoxLayout>

#include <inputmode/abstractinputmode.h>
#include <vtextedit/richtexteditorconfig.h>
#include <vtextedit/vrichtexteditor.h>
#include <vtextedit/vtextedit.h>

using namespace tests;
using namespace vte;

namespace {
// createMimeDataFromSelection() is protected API; a subclass is the supported
// way to reach it.
class ExposedTextEdit : public VTextEdit {
public:
  using VTextEdit::createMimeDataFromSelection;
};

QSharedPointer<RichTextEditorConfig> configWithMode(InputMode p_mode) {
  auto config = QSharedPointer<RichTextEditorConfig>::create();
  config->m_inputMode = p_mode;
  return config;
}

// A short document with a bold run and a bullet list.
const char *c_html = "<p>plain <b>bold</b> tail</p>"
                     "<ul><li>alpha</li><li>beta</li></ul>";

bool isBoldAt(QTextDocument *p_doc, int p_position) {
  QTextCursor cursor(p_doc);
  cursor.setPosition(p_position);
  return cursor.charFormat().fontWeight() > QFont::Normal;
}
} // namespace

void TestRichTextEditor::testFoldKeysFallThroughWithoutFolding() {
  VRichTextEditor editor(configWithMode(InputMode::ViMode));
  editor.setHtml(QString::fromUtf8(c_html));
  auto edit = editor.getTextEdit();

  const auto before = editor.document()->toPlainText();

  // Vi normal mode: zc/zo are fold commands. With no text folding they must be
  // ignored rather than swallowed into an edit.
  QTest::keyClicks(edit, QStringLiteral("zc"));
  QTest::keyClicks(edit, QStringLiteral("zo"));

  QCOMPARE(editor.document()->toPlainText(), before);
  QCOMPARE(editor.getEditorMode(), EditorMode::ViModeNormal);
}

void TestRichTextEditor::testCompletionKeysDoNothing() {
  VRichTextEditor editor(configWithMode(InputMode::ViMode));
  editor.setHtml(QString::fromUtf8(c_html));
  auto edit = editor.getTextEdit();

  // Enter insert mode, then ask for a completion. There is no completer, so
  // nothing may be inserted.
  QTest::keyClick(edit, Qt::Key_I);
  QCOMPARE(editor.getEditorMode(), EditorMode::ViModeInsert);

  const auto before = editor.document()->toPlainText();
  QTest::keyClick(edit, Qt::Key_P, Qt::ControlModifier);
  QTest::keyClick(edit, Qt::Key_N, Qt::ControlModifier);
  QCOMPARE(editor.document()->toPlainText(), before);

  QTest::keyClick(edit, Qt::Key_Escape);
  QCOMPARE(editor.getEditorMode(), EditorMode::ViModeNormal);
}

void TestRichTextEditor::testSetInputModeSwitchesAndIsIdempotent() {
  VRichTextEditor editor;
  QCOMPARE(editor.getInputMode()->mode(), InputMode::NormalMode);

  QSignalSpy modeSpy(&editor, &VRichTextEditor::modeChanged);

  editor.setInputMode(InputMode::VscodeMode);
  QCOMPARE(editor.getInputMode()->mode(), InputMode::VscodeMode);
  QCOMPARE(modeSpy.count(), 1);

  // Idempotent: no mode object is created, no signal.
  editor.setInputMode(InputMode::VscodeMode);
  QCOMPARE(modeSpy.count(), 1);

  modeSpy.clear();
  editor.setInputMode(InputMode::ViMode);
  QCOMPARE(editor.getInputMode()->mode(), InputMode::ViMode);
  // Exactly one host emission per call: entering Vi normal mode is reported by
  // the interface relay before the final host emission, and the relay is
  // suppressed because the mode did not change afterwards.
  QCOMPARE(modeSpy.count(), 1);
  QCOMPARE(editor.getEditorMode(), EditorMode::ViModeNormal);

  modeSpy.clear();
  editor.setInputMode(InputMode::NormalMode);
  QCOMPARE(modeSpy.count(), 1);
}

void TestRichTextEditor::testLineMappingIsIdentity() {
  VRichTextEditor editor(configWithMode(InputMode::ViMode));
  editor.setHtml(QStringLiteral("<p>one</p><p>two</p><p>three</p><p>four</p>"));
  auto edit = editor.getTextEdit();
  auto doc = editor.document();
  QCOMPARE(doc->blockCount(), 4);

  auto cursor = edit->textCursor();
  cursor.movePosition(QTextCursor::Start);
  edit->setTextCursor(cursor);

  // G/gg go through lineToVisibleLine()/visibleLineToLine(); without folding
  // the mapping is the identity, so they must land on the real last/first
  // block.
  QTest::keyClicks(edit, QStringLiteral("G"));
  QCOMPARE(edit->textCursor().blockNumber(), 3);

  QTest::keyClicks(edit, QStringLiteral("gg"));
  QCOMPARE(edit->textCursor().blockNumber(), 0);
}

void TestRichTextEditor::testStatusWidgetEmissionsFollowTheTransitions() {
  VRichTextEditor editor;
  QSignalSpy spy(&editor, &VRichTextEditor::inputModeStatusWidgetChanged);

  // Normal <-> VSCode: neither mode publishes a status widget.
  editor.setInputMode(InputMode::VscodeMode);
  QCOMPARE(spy.count(), 0);
  editor.setInputMode(InputMode::NormalMode);
  QCOMPARE(spy.count(), 0);

  // non-Vi -> Vi: exactly one non-null emission.
  editor.setInputMode(InputMode::ViMode);
  QCOMPARE(spy.count(), 1);
  QVERIFY(!spy.at(0).at(0).value<QSharedPointer<QWidget>>().isNull());
  QVERIFY(!editor.inputModeStatusWidget().isNull());

  // Vi -> non-Vi: exactly one null emission, from the detach.
  spy.clear();
  editor.setInputMode(InputMode::NormalMode);
  QCOMPARE(spy.count(), 1);
  QVERIFY(spy.at(0).at(0).value<QSharedPointer<QWidget>>().isNull());
  QVERIFY(editor.inputModeStatusWidget().isNull());
}

void TestRichTextEditor::testDestructionWithAMountedViStatusWidget() {
  QWidget host;
  auto layout = new QVBoxLayout(&host);

  {
    VRichTextEditor editor(configWithMode(InputMode::ViMode), &host);
    layout->addWidget(&editor);

    auto statusWidget = editor.inputModeStatusWidget();
    QVERIFY(!statusWidget.isNull());
    // Mount it, the way a host status bar does.
    layout->addWidget(statusWidget.data());
    QVERIFY(statusWidget->parent() != nullptr);

    // Switching away must unmount it before the mode object dies.
    editor.setInputMode(InputMode::NormalMode);
    QVERIFY(statusWidget->parent() == nullptr);
  }

  // And a destruction directly in Vi mode.
  {
    auto editor = new VRichTextEditor(configWithMode(InputMode::ViMode), &host);
    layout->addWidget(editor);
    auto statusWidget = editor->inputModeStatusWidget();
    QVERIFY(!statusWidget.isNull());
    layout->addWidget(statusWidget.data());

    delete editor;
    QVERIFY(statusWidget->parent() == nullptr);
  }
}

void TestRichTextEditor::testConstructedInViMode() {
  VRichTextEditor editor(configWithMode(InputMode::ViMode));

  QCOMPARE(editor.getInputMode()->mode(), InputMode::ViMode);
  QCOMPARE(editor.getEditorMode(), EditorMode::ViModeNormal);

  // The status widget is available right away.
  QVERIFY(!editor.inputModeStatusWidget().isNull());

  // The input method is already disabled in Vi normal mode.
  QCOMPARE(editor.getTextEdit()->inputMethodQuery(Qt::ImEnabled).toBool(), false);
}

void TestRichTextEditor::testFocusTransitionsFireOncePerConsumer() {
  QWidget host;
  auto layout = new QVBoxLayout(&host);
  auto other = new QWidget(&host);
  other->setFocusPolicy(Qt::StrongFocus);
  auto editor = new VRichTextEditor(configWithMode(InputMode::ViMode), &host);
  layout->addWidget(other);
  layout->addWidget(editor);

  host.show();
  QVERIFY(QTest::qWaitForWindowExposed(&host));

  QSignalSpy inSpy(editor, &VRichTextEditor::focusIn);
  QSignalSpy outSpy(editor, &VRichTextEditor::focusOut);

  // All three consumers ride the same two TextEditInputMode signals: the
  // public focusIn()/focusOut() asserted here, AbstractInputMode::focusIn/Out
  // (observed below through the Vi cursor blinking side effect) and the KateVi
  // command-response callbacks registered through connectFocusIn/Out(). One
  // emission is therefore exactly one fire for each of them.
  other->setFocus();
  QTRY_COMPARE(other->hasFocus(), true);

  editor->setFocus();
  QTRY_COMPARE(inSpy.count(), 1);
  QCOMPARE(outSpy.count(), 0);

  // Vi normal mode suppresses the cursor blinking while it has the focus.
  QCOMPARE(QGuiApplication::styleHints()->cursorFlashTime(), 0);

  other->setFocus();
  QTRY_COMPARE(outSpy.count(), 1);
  QCOMPARE(inSpy.count(), 1);

  // And restores the application flash time on focus out.
  QVERIFY(QGuiApplication::styleHints()->cursorFlashTime() > 0);
}

void TestRichTextEditor::testFocusThroughTheViCommandBar() {
  QWidget host;
  auto layout = new QVBoxLayout(&host);
  auto other = new QWidget(&host);
  other->setFocusPolicy(Qt::StrongFocus);
  layout->addWidget(other);
  auto editor = new VRichTextEditor(configWithMode(InputMode::ViMode), &host);
  layout->addWidget(editor);

  auto statusWidget = editor->inputModeStatusWidget();
  QVERIFY(!statusWidget.isNull());
  layout->addWidget(statusWidget.data());

  host.show();
  QVERIFY(QTest::qWaitForWindowExposed(&host));

  other->setFocus();
  QTRY_COMPARE(other->hasFocus(), true);

  QSignalSpy inSpy(editor, &VRichTextEditor::focusIn);
  QSignalSpy outSpy(editor, &VRichTextEditor::focusOut);

  auto edit = editor->getTextEdit();
  edit->setFocus();
  QTRY_COMPARE(inSpy.count(), 1);

  // Open the Vi command bar; the editor loses the focus to it.
  QTest::keyClicks(edit, QStringLiteral(":"));
  QTRY_COMPARE(outSpy.count(), 1);
  QCOMPARE(inSpy.count(), 1);

  // Leaving the command bar hands the focus back to the editor.
  QTest::keyClick(QApplication::focusWidget(), Qt::Key_Escape);
  QTRY_COMPARE(inSpy.count(), 2);
  QCOMPARE(outSpy.count(), 1);
  QVERIFY(edit->hasFocus());

  // The status widget must be unparented before the editor goes away.
  statusWidget->setParent(nullptr);
}

void TestRichTextEditor::testViMotionsKeepFormatting() {
  VRichTextEditor editor(configWithMode(InputMode::ViMode));
  editor.setHtml(QString::fromUtf8(c_html));
  auto edit = editor.getTextEdit();
  auto doc = editor.document();

  auto cursor = edit->textCursor();
  cursor.movePosition(QTextCursor::Start);
  edit->setTextCursor(cursor);

  const auto firstBlock = doc->firstBlock().text();
  const int boldIndex = firstBlock.indexOf(QStringLiteral("bold"));
  QVERIFY(boldIndex > 0);
  const int boldPosition = doc->firstBlock().position() + boldIndex + 1;
  QVERIFY(isBoldAt(doc, boldPosition));

  // Delete the very first (unformatted) character.
  QTest::keyClick(edit, Qt::Key_X);
  QCOMPARE(doc->firstBlock().text(), firstBlock.mid(1));

  // The bold run is untouched.
  QVERIFY(isBoldAt(doc, boldPosition - 1));

  // A motion down and back must not modify anything.
  const auto snapshot = doc->toPlainText();
  QTest::keyClicks(edit, QStringLiteral("jk"));
  QTest::keyClicks(edit, QStringLiteral("lh"));
  QCOMPARE(doc->toPlainText(), snapshot);
  QVERIFY(isBoldAt(doc, boldPosition - 1));
}

void TestRichTextEditor::testViEditCommandsKeepFormatting() {
  VRichTextEditor editor(configWithMode(InputMode::ViMode));
  editor.setHtml(QStringLiteral("<p>head</p><p>plain <b>bold</b> tail</p>"
                                "<ul><li>alpha</li><li>beta</li></ul>"));
  auto edit = editor.getTextEdit();
  auto doc = editor.document();

  auto cursor = edit->textCursor();
  cursor.movePosition(QTextCursor::Start);
  edit->setTextCursor(cursor);

  const int blockCount = doc->blockCount();
  const auto formattedLine = doc->findBlockByNumber(1).text();

  // dd on the first line leaves the formatted line untouched.
  QTest::keyClicks(edit, QStringLiteral("dd"));
  QCOMPARE(doc->blockCount(), blockCount - 1);
  QCOMPARE(doc->firstBlock().text(), formattedLine);
  const int boldIndex = formattedLine.indexOf(QStringLiteral("bold"));
  QVERIFY(boldIndex > 0);
  QVERIFY(isBoldAt(doc, doc->firstBlock().position() + boldIndex + 1));

  // yy/p duplicate a line. The put is plain text by design (D5), but the
  // source line keeps its formatting.
  QTest::keyClicks(edit, QStringLiteral("yy"));
  QTest::keyClicks(edit, QStringLiteral("p"));
  QCOMPARE(doc->blockCount(), blockCount);
  QCOMPARE(doc->firstBlock().text(), formattedLine);
  QVERIFY(isBoldAt(doc, doc->firstBlock().position() + boldIndex + 1));

  // The put line carries the same characters but none of the formatting: the
  // Vi register is a QString. This pins the accepted D5 limitation.
  const auto pastedBlock = doc->findBlockByNumber(1);
  QCOMPARE(pastedBlock.text(), formattedLine);
  QVERIFY(!isBoldAt(doc, pastedBlock.position() + boldIndex + 1));

  // A appends at the end of the line, Esc returns to normal mode.
  auto cursorAtFirst = edit->textCursor();
  cursorAtFirst.setPosition(doc->firstBlock().position());
  edit->setTextCursor(cursorAtFirst);
  QTest::keyClicks(edit, QStringLiteral("A"));
  QCOMPARE(editor.getEditorMode(), EditorMode::ViModeInsert);
  QTest::keyClicks(edit, QStringLiteral("!"));
  QTest::keyClick(edit, Qt::Key_Escape);
  QCOMPARE(editor.getEditorMode(), EditorMode::ViModeNormal);
  QCOMPARE(doc->firstBlock().text(), formattedLine + QLatin1Char('!'));
  QVERIFY(isBoldAt(doc, doc->firstBlock().position() + boldIndex + 1));
}

void TestRichTextEditor::testMimeDataOfOverriddenSelectionHasHtmlAndPlain() {
  ExposedTextEdit edit;
  edit.setHtml(QStringLiteral("<p>plain <b>bold</b></p><p>second</p>"));

  auto doc = edit.document();
  const int end = doc->lastBlock().position() + doc->lastBlock().length() - 1;
  edit.setOverriddenSelection(0, end);

  QScopedPointer<QMimeData> data(edit.createMimeDataFromSelection());
  QVERIFY(data);
  QVERIFY(data->hasHtml());
  QVERIFY(data->html().contains(QStringLiteral("font-weight")));
  QVERIFY(data->hasText());

  // Paragraph separators are normalized in the plain flavour.
  QVERIFY(!data->text().contains(QChar(QChar::ParagraphSeparator)));
  QVERIFY(data->text().contains(QLatin1Char('\n')));
}

void TestRichTextEditor::testMimeDataOfAListSelection() {
  ExposedTextEdit edit;
  edit.setHtml(QStringLiteral("<ul><li>alpha</li><li>beta</li></ul>"
                              "<table border=\"1\"><tr><td>cell one</td>"
                              "<td>cell two</td></tr></table>"));
  auto doc = edit.document();

  // The list part only.
  const auto secondItem = doc->findBlockByNumber(1);
  edit.setOverriddenSelection(doc->firstBlock().position(),
                              secondItem.position() + secondItem.length() - 1);
  {
    QScopedPointer<QMimeData> data(edit.createMimeDataFromSelection());
    QVERIFY(data->hasHtml());
    QVERIFY(data->html().contains(QStringLiteral("<li")));
    QVERIFY(data->text().contains(QStringLiteral("alpha")));
    QVERIFY(data->text().contains(QStringLiteral("beta")));
    QVERIFY(!data->text().contains(QChar(QChar::ParagraphSeparator)));
  }

  // A single table cell.
  QTextCursor cellCursor(doc);
  cellCursor.movePosition(QTextCursor::End);
  auto table = doc->rootFrame()->childFrames().isEmpty()
                   ? nullptr
                   : qobject_cast<QTextTable *>(doc->rootFrame()->childFrames().first());
  QVERIFY(table);
  auto cell = table->cellAt(0, 0);
  const int start = cell.firstCursorPosition().position();
  const int end = cell.lastCursorPosition().position();
  edit.setOverriddenSelection(start, end);
  {
    QScopedPointer<QMimeData> data(edit.createMimeDataFromSelection());
    QVERIFY(data->hasHtml());
    QCOMPARE(data->text(), QStringLiteral("cell one"));
  }
}

void TestRichTextEditor::testJoinLinesIsFormatLossy() {
  VRichTextEditor editor(configWithMode(InputMode::ViMode));
  editor.setHtml(QStringLiteral("<p>first</p><p><b>second</b></p>"));
  auto edit = editor.getTextEdit();
  auto doc = editor.document();

  auto cursor = edit->textCursor();
  cursor.movePosition(QTextCursor::Start);
  edit->setTextCursor(cursor);

  QCOMPARE(doc->blockCount(), 2);

  // Vi J joins through the QString-based interface, so the joined line ends up
  // with a single, uniform character format. This pins the accepted D5
  // limitation.
  QTest::keyClicks(edit, QStringLiteral("J"));

  QCOMPARE(doc->blockCount(), 1);
  const auto block = doc->firstBlock();
  QCOMPARE(block.textFormats().count(), 1);
}

void TestRichTextEditor::testVscodeMoveLineIsFormatLossy() {
  VRichTextEditor editor(configWithMode(InputMode::VscodeMode));
  editor.setHtml(QStringLiteral("<p>first</p><p><b>second</b></p><p>third</p>"));
  auto edit = editor.getTextEdit();
  auto doc = editor.document();

  auto cursor = edit->textCursor();
  cursor.movePosition(QTextCursor::Start);
  edit->setTextCursor(cursor);

  QVERIFY(isBoldAt(doc, doc->findBlockByNumber(1).position() + 1));

  // Alt+Down moves the current line down through the QString-based interface.
  // The moved lines are rebuilt as plain text, which is the accepted D5
  // limitation.
  QTest::keyClick(edit, Qt::Key_Down, Qt::AltModifier);

  QCOMPARE(doc->firstBlock().text(), QStringLiteral("second"));
  QCOMPARE(doc->findBlockByNumber(1).text(), QStringLiteral("first"));
  // Formatting of the touched lines is gone.
  QVERIFY(!isBoldAt(doc, doc->firstBlock().position() + 1));
}

void TestRichTextEditor::testVscodeDuplicateLineIsFormatLossy() {
  VRichTextEditor editor(configWithMode(InputMode::VscodeMode));
  editor.setHtml(QStringLiteral("<p>plain <b>bold</b> tail</p><p>second</p>"));
  auto edit = editor.getTextEdit();
  auto doc = editor.document();

  auto cursor = edit->textCursor();
  cursor.movePosition(QTextCursor::Start);
  edit->setTextCursor(cursor);

  const auto formattedLine = doc->firstBlock().text();
  const int boldIndex = formattedLine.indexOf(QStringLiteral("bold"));
  QVERIFY(boldIndex > 0);
  const int blockCount = doc->blockCount();

  // Shift+Alt+Down duplicates the current line through the QString-based
  // interface, so the copy is plain text (accepted D5 limitation).
  QTest::keyClick(edit, Qt::Key_Down, Qt::ShiftModifier | Qt::AltModifier);

  QCOMPARE(doc->blockCount(), blockCount + 1);
  QCOMPARE(doc->firstBlock().text(), formattedLine);
  QCOMPARE(doc->findBlockByNumber(1).text(), formattedLine);
  QVERIFY(!isBoldAt(doc, doc->findBlockByNumber(1).position() + boldIndex + 1));
}

void TestRichTextEditor::testViMotionsTraverseATable() {
  VRichTextEditor editor(configWithMode(InputMode::ViMode));
  editor.setHtml(QStringLiteral("<p>before</p>"
                                "<table border=\"1\"><tr><td>one</td><td>two</td></tr>"
                                "<tr><td>three</td><td>four</td></tr></table>"
                                "<p>after</p>"));
  auto edit = editor.getTextEdit();
  auto doc = editor.document();

  auto cursor = edit->textCursor();
  cursor.movePosition(QTextCursor::Start);
  edit->setTextCursor(cursor);

  // Accepted D6 limitation: the line model is flat, so j walks into the table
  // cells one by one instead of skipping the table.
  const int blocks = doc->blockCount();
  QVERIFY(blocks > 3);

  QTest::keyClicks(edit, QStringLiteral("j"));
  QCOMPARE(edit->textCursor().blockNumber(), 1);
  QCOMPARE(edit->textCursor().block().text(), QStringLiteral("one"));

  // G still reaches the very last block.
  QTest::keyClicks(edit, QStringLiteral("G"));
  QCOMPARE(edit->textCursor().blockNumber(), blocks - 1);
  QCOMPARE(edit->textCursor().block().text(), QStringLiteral("after"));
}

QTEST_MAIN(tests::TestRichTextEditor)
