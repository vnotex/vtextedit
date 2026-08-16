#include "test_markdowneditor.h"

#include <QSharedPointer>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

#include <vtextedit/markdowneditorconfig.h>
#include <vtextedit/markdownhighlighter.h>
#include <vtextedit/markdownutils.h>
#include <vtextedit/texteditorconfig.h>
#include <vtextedit/vmarkdowneditor.h>
#include <vtextedit/vtextedit.h>

using namespace tests;
using namespace vte;

namespace {
QSharedPointer<MarkdownEditorConfig> makeConfig() {
  auto editorConfig = QSharedPointer<TextEditorConfig>::create();
  editorConfig->m_inputMode = InputMode::NormalMode;
  return QSharedPointer<MarkdownEditorConfig>::create(editorConfig);
}

// A VMarkdownEditor with the text set and the cursor placed at @p_block /
// @p_positionInBlock (a negative position means end of block).
class Fixture {
public:
  explicit Fixture(const QString &p_text, int p_block = -1, int p_positionInBlock = -1)
      : m_editor(makeConfig(), QSharedPointer<TextEditorParameters>::create(), nullptr) {
    m_editor.setText(p_text);
    auto doc = m_editor.document();
    const int blockNumber = p_block < 0 ? doc->blockCount() - 1 : p_block;
    const auto block = doc->findBlockByNumber(blockNumber);
    QTextCursor cursor(doc);
    const int inBlock =
        p_positionInBlock < 0 ? block.length() - 1 : qMin(p_positionInBlock, block.length() - 1);
    cursor.setPosition(block.position() + inBlock);
    edit()->setTextCursor(cursor);
  }

  VTextEdit *edit() const { return m_editor.getTextEdit(); }

  VMarkdownEditor *editor() { return &m_editor; }

  QString text() const { return m_editor.document()->toPlainText(); }

  QString blockText(int p_blockNumber) const {
    return m_editor.document()->findBlockByNumber(p_blockNumber).text();
  }

  // Document position of the end of block @p_blockNumber.
  int blockEnd(int p_blockNumber) const {
    const auto block = m_editor.document()->findBlockByNumber(p_blockNumber);
    return block.position() + block.length() - 1;
  }

  void pressReturn() { QTest::keyClick(edit(), Qt::Key_Return); }

  void pressShiftReturn() { QTest::keyClick(edit(), Qt::Key_Return, Qt::ShiftModifier); }

  void pressCtrlReturn() { QTest::keyClick(edit(), Qt::Key_Return, Qt::ControlModifier); }

  void moveTo(int p_blockNumber) {
    const auto block = m_editor.document()->findBlockByNumber(p_blockNumber);
    QTextCursor cursor(m_editor.document());
    cursor.setPosition(block.position() + block.length() - 1);
    edit()->setTextCursor(cursor);
  }

  // Bump the document time stamp without changing the block structure, so the
  // existing parse result becomes stale but stays structurally usable.
  void makeAstStale() {
    auto cursor = edit()->textCursor();
    cursor.insertText(QStringLiteral("x"));
    edit()->setTextCursor(cursor);
  }

  // Wait until the asynchronous parse has produced a result matching the
  // current document state.
  void waitForFreshAst(int p_blockNumber) {
    auto highlighter = m_editor.getHighlighter();
    QTRY_VERIFY_WITH_TIMEOUT(highlighter->getBlockContext(p_blockNumber).m_fresh, 5000);
  }

private:
  VMarkdownEditor m_editor;
};
} // namespace

void TestMarkdownEditor::testIsQuote() {
  struct Case {
    const char *m_text;
    const char *m_indentation;
    const char *m_prefix;
    const char *m_rest;
    int m_depth;
  };

  const Case cases[] = {
      {"> hello", "", "> ", "hello", 1},
      {">hello", "", ">", "hello", 1},
      {"  > hello", "  ", "> ", "hello", 1},
      {"> > a", "", "> > ", "a", 2},
      {">> a", "", ">> ", "a", 2},
      {"> ", "", "> ", "", 1},
      {">", "", ">", "", 1},
      {"> - item", "", "> ", "- item", 1},
  };

  for (const auto &c : cases) {
    QString indentation, prefix, rest;
    int depth = -1;
    QVERIFY2(MarkdownUtils::isQuote(QString::fromUtf8(c.m_text), indentation, prefix, rest, depth),
             c.m_text);
    QCOMPARE(indentation, QString::fromUtf8(c.m_indentation));
    QCOMPARE(prefix, QString::fromUtf8(c.m_prefix));
    QCOMPARE(rest, QString::fromUtf8(c.m_rest));
    QCOMPARE(depth, c.m_depth);
  }

  QString indentation, prefix, rest;
  int depth = -1;
  QVERIFY(!MarkdownUtils::isQuote(QStringLiteral("hello"), indentation, prefix, rest, depth));
  QVERIFY(!MarkdownUtils::isQuote(QString(), indentation, prefix, rest, depth));
}

void TestMarkdownEditor::testTypeQuoteUnchanged() {
  // c_quoteRegExp requires whitespace after the marker, so "> a" un-quotes
  // while ">a" is treated as unquoted and gets another marker.
  {
    Fixture fixture(QStringLiteral("> a"), 0);
    MarkdownUtils::typeQuote(fixture.edit());
    QCOMPARE(fixture.blockText(0), QStringLiteral("a"));
  }
  {
    Fixture fixture(QStringLiteral(">a"), 0);
    MarkdownUtils::typeQuote(fixture.edit());
    QCOMPARE(fixture.blockText(0), QStringLiteral("> >a"));
  }
}

void TestMarkdownEditor::testQuoteContinuation() {
  {
    Fixture fixture(QStringLiteral("> foo"), 0);
    fixture.pressReturn();
    QCOMPARE(fixture.blockText(1), QStringLiteral("> "));
    // The cursor sits right after the inserted prefix.
    QCOMPARE(fixture.edit()->textCursor().blockNumber(), 1);
    QCOMPARE(fixture.edit()->textCursor().positionInBlock(), 2);
  }
  {
    Fixture fixture(QStringLiteral("> > foo"), 0);
    fixture.pressReturn();
    QCOMPARE(fixture.blockText(1), QStringLiteral("> > "));
  }
  {
    Fixture fixture(QStringLiteral(">> foo"), 0);
    fixture.pressReturn();
    QCOMPARE(fixture.blockText(1), QStringLiteral(">> "));
  }
  {
    // Indented quote markers: the indentation is copied by AutoIndentHelper,
    // the prefix by the continuation.
    Fixture fixture(QStringLiteral("  > foo"), 0);
    fixture.pressReturn();
    QCOMPARE(fixture.blockText(1), QStringLiteral("  > "));
  }
}

void TestMarkdownEditor::testQuoteWithListContinuation() {
  {
    Fixture fixture(QStringLiteral("> - a"), 0);
    fixture.pressReturn();
    QCOMPARE(fixture.blockText(1), QStringLiteral("> - "));
  }
  {
    Fixture fixture(QStringLiteral("> 1. a"), 0);
    fixture.pressReturn();
    QCOMPARE(fixture.blockText(1), QStringLiteral("> 2. "));
  }
  {
    Fixture fixture(QStringLiteral("> - [x] a"), 0);
    fixture.pressReturn();
    QCOMPARE(fixture.blockText(1), QStringLiteral("> - [ ] "));
  }
}

void TestMarkdownEditor::testEmptyQuoteStartsANewQuoteLine() {
  // A bare quote line is a blank line inside the quote, so Enter continues the
  // quote instead of stripping a level.
  {
    Fixture fixture(QStringLiteral("> > "), 0);
    fixture.pressReturn();
    QCOMPARE(fixture.text(), QStringLiteral("> > \n> > "));
    QCOMPARE(fixture.edit()->textCursor().blockNumber(), 1);
    QCOMPARE(fixture.edit()->textCursor().positionInBlock(), 4);
  }
  {
    Fixture fixture(QStringLiteral("> "), 0);
    fixture.pressReturn();
    QCOMPARE(fixture.text(), QStringLiteral("> \n> "));
  }
  {
    // A bare ">" with no trailing space is continued verbatim too.
    Fixture fixture(QStringLiteral(">"), 0);
    fixture.pressReturn();
    QCOMPARE(fixture.text(), QStringLiteral(">\n>"));
  }
}

void TestMarkdownEditor::testEmptyListInQuoteExit() {
  Fixture fixture(QStringLiteral("> - "), 0);
  fixture.pressReturn();
  QCOMPARE(fixture.text(), QStringLiteral("> "));
}

void TestMarkdownEditor::testCursorBeforeTextDoesNotCollapse() {
  // "> |foo": the quote is not empty to the right, so Enter must split.
  Fixture fixture(QStringLiteral("> foo"), 0, 2);
  fixture.pressReturn();
  QCOMPARE(fixture.blockText(0), QStringLiteral("> "));
  QCOMPARE(fixture.blockText(1), QStringLiteral("> foo"));
}

void TestMarkdownEditor::testMidLineSplitInsideQuote() {
  // "> fo|o"
  Fixture fixture(QStringLiteral("> foo"), 0, 4);
  fixture.pressReturn();
  QCOMPARE(fixture.blockText(0), QStringLiteral("> fo"));
  QCOMPARE(fixture.blockText(1), QStringLiteral("> o"));
}

void TestMarkdownEditor::testSelectionFallsThrough() {
  // preKeyReturn never rewrites a selection: handleKeyReturn performs its
  // normal selection-replacing split, and the continuation then applies to the
  // resulting line.
  // Same-block selection, forward.
  {
    Fixture fixture(QStringLiteral("> - "), 0);
    auto cursor = fixture.edit()->textCursor();
    cursor.setPosition(2);
    cursor.setPosition(4, QTextCursor::KeepAnchor);
    fixture.edit()->setTextCursor(cursor);
    fixture.pressReturn();
    QCOMPARE(fixture.text(), QStringLiteral("> \n> "));
  }
  // Same-block selection, backward.
  {
    Fixture fixture(QStringLiteral("> - "), 0);
    auto cursor = fixture.edit()->textCursor();
    cursor.setPosition(4);
    cursor.setPosition(2, QTextCursor::KeepAnchor);
    fixture.edit()->setTextCursor(cursor);
    fixture.pressReturn();
    QCOMPARE(fixture.text(), QStringLiteral("> \n> "));
  }
  // Multi-block selection.
  {
    Fixture fixture(QStringLiteral("> - a\n> - b"), 0);
    auto cursor = fixture.edit()->textCursor();
    cursor.setPosition(4);
    cursor.setPosition(10, QTextCursor::KeepAnchor);
    fixture.edit()->setTextCursor(cursor);
    fixture.pressReturn();
    QCOMPARE(fixture.text(), QStringLiteral("> - \n> - b"));
  }
}

void TestMarkdownEditor::testSelectionDoesNotCarryAstContext() {
  // The probe is taken at the active end of the selection, which is not
  // necessarily the line that survives the selection-replacing split. Such a
  // context must never drive an insertion, in either selection direction.
  const QString text = QStringLiteral("plain\n> q");

  // Forward: anchor on the unquoted line, cursor on the quoted one.
  {
    Fixture fixture(text, 1);
    fixture.waitForFreshAst(1);
    QCOMPARE(fixture.editor()->getHighlighter()->getBlockContext(1).m_quoteDepth, 1);

    const int plainEnd = fixture.blockEnd(0);
    const int quoteEnd = fixture.blockEnd(1);
    auto cursor = fixture.edit()->textCursor();
    cursor.setPosition(plainEnd);
    cursor.setPosition(quoteEnd, QTextCursor::KeepAnchor);
    fixture.edit()->setTextCursor(cursor);
    fixture.pressReturn();
    QCOMPARE(fixture.text(), QStringLiteral("plain\n"));
  }
  // Backward: the same selection, opposite direction.
  {
    Fixture fixture(text, 1);
    fixture.waitForFreshAst(1);

    const int plainEnd = fixture.blockEnd(0);
    const int quoteEnd = fixture.blockEnd(1);
    auto cursor = fixture.edit()->textCursor();
    cursor.setPosition(quoteEnd);
    cursor.setPosition(plainEnd, QTextCursor::KeepAnchor);
    fixture.edit()->setTextCursor(cursor);
    fixture.pressReturn();
    QCOMPARE(fixture.text(), QStringLiteral("plain\n"));
  }
}

void TestMarkdownEditor::testLazyContinuation() {
  // "> a" followed by a lazy continuation line "b": the text of "b" carries no
  // marker, only the AST knows it is inside the quote.
  struct Case {
    const char *m_second;
    const char *m_expected;
  };
  const Case cases[] = {
      {"b", "> "},
      {" b", " > "},
      {"   b", "   > "},
      {"\tb", "\t> "},
  };

  for (const auto &c : cases) {
    const QString text = QStringLiteral("> a\n") + QString::fromUtf8(c.m_second);
    Fixture fixture(text, 1);
    fixture.waitForFreshAst(1);
    QCOMPARE(fixture.editor()->getHighlighter()->getBlockContext(1).m_quoteDepth, 1);
    fixture.pressReturn();
    // The new block carries the copied indentation plus the synthesized prefix.
    QCOMPARE(fixture.blockText(2), QString::fromUtf8(c.m_expected));
  }
}

void TestMarkdownEditor::testLazyContinuationDepths() {
  // Depth 0: no AST quote context, so nothing is synthesized.
  {
    Fixture fixture(QStringLiteral("a\nb"), 1);
    fixture.waitForFreshAst(1);
    QCOMPARE(fixture.editor()->getHighlighter()->getBlockContext(1).m_quoteDepth, 0);
    fixture.pressReturn();
    QCOMPARE(fixture.blockText(2), QString());
  }
  // Nested: a lazy continuation of a doubly nested quote.
  {
    Fixture fixture(QStringLiteral("> > a\nb"), 1);
    fixture.waitForFreshAst(1);
    QCOMPARE(fixture.editor()->getHighlighter()->getBlockContext(1).m_quoteDepth, 2);
    fixture.pressReturn();
    QCOMPARE(fixture.blockText(2), QStringLiteral("> > "));
  }
}

void TestMarkdownEditor::testStaleAstNeverInserts() {
  // A stale result may carry a positive quote depth, but inserting from it
  // would leave permanently wrong text, so it must never be used.
  Fixture fixture(QStringLiteral("> a\nb"), 1);
  fixture.waitForFreshAst(1);
  QCOMPARE(fixture.editor()->getHighlighter()->getBlockContext(1).m_quoteDepth, 1);

  fixture.makeAstStale();
  const auto context = fixture.editor()->getHighlighter()->getBlockContext(1);
  QVERIFY(context.m_valid);
  QVERIFY(!context.m_fresh);
  // The stale depth is still readable...
  QCOMPARE(context.m_quoteDepth, 1);

  // ...but must not produce any insertion.
  fixture.pressReturn();
  QCOMPARE(fixture.blockText(1), QStringLiteral("bx"));
  QCOMPARE(fixture.blockText(2), QString());
}

void TestMarkdownEditor::testFenceVeto() {
  const QString text = QStringLiteral("```\n> foo\n```");
  Fixture fixture(text, 1);
  fixture.waitForFreshAst(1);
  QVERIFY(fixture.editor()->getHighlighter()->getBlockContext(1).m_inFencedCode);

  fixture.pressReturn();
  // Nothing is continued inside a fence.
  QCOMPARE(fixture.blockText(2), QString());
}

void TestMarkdownEditor::testStaleFenceStillSuppresses() {
  // Suppression is the safe direction, so it is allowed to use stale data.
  Fixture fixture(QStringLiteral("```\n> foo\n```"), 1);
  fixture.waitForFreshAst(1);

  fixture.makeAstStale();
  const auto context = fixture.editor()->getHighlighter()->getBlockContext(1);
  QVERIFY(context.m_valid);
  QVERIFY(!context.m_fresh);
  QVERIFY(context.m_inFencedCode);

  // The regex would happily continue "> foox"; the veto wins.
  fixture.pressReturn();
  QCOMPARE(fixture.blockText(2), QString());
}

void TestMarkdownEditor::testIndentedQuoteIsStillContinued() {
  // Four-space indented text is CommonMark indented code, not a quote.
  // Detection is deliberately textual, so it is continued anyway (Decision 7).
  Fixture fixture(QStringLiteral("    > foo"), 0);
  fixture.pressReturn();
  QCOMPARE(fixture.blockText(1), QStringLiteral("    > "));
}

void TestMarkdownEditor::testShiftReturnDoesNotContinue() {
  Fixture fixture(QStringLiteral("> foo"), 0);
  fixture.pressShiftReturn();
  QCOMPARE(fixture.blockText(0), QStringLiteral("> foo  "));
  // Shift+Enter must not continue the quote.
  QCOMPARE(fixture.blockText(1), QString());

  // And the context cached by the Shift+Enter probe must not leak into the
  // next, ordinary Enter on an unquoted (empty) line.
  fixture.pressReturn();
  QCOMPARE(fixture.blockText(2), QString());
}

void TestMarkdownEditor::testCtrlReturnDoesNotLeakContext() {
  // Ctrl+Return probes the context but is swallowed by handleKeyReturn without
  // a matching postKeyReturn, so the cached value is never consumed. The next
  // ordinary Return must re-probe rather than reuse it.
  Fixture fixture(QStringLiteral("> a\nb\n\nc"), 1);
  fixture.waitForFreshAst(1);
  QCOMPARE(fixture.editor()->getHighlighter()->getBlockContext(1).m_quoteDepth, 1);

  const auto before = fixture.text();
  fixture.pressCtrlReturn();
  QCOMPARE(fixture.text(), before);

  // "c" is outside the quote; no prefix may be synthesized from the cached
  // depth of block 1.
  fixture.moveTo(3);
  QCOMPARE(fixture.editor()->getHighlighter()->getBlockContext(3).m_quoteDepth, 0);
  fixture.pressReturn();
  QCOMPARE(fixture.blockText(4), QString());
}

void TestMarkdownEditor::testWhitespaceOnlyQuoteContinues() {
  Fixture fixture(QStringLiteral(">   "), 0);
  fixture.pressReturn();
  // The prefix is reproduced verbatim, spacing included.
  QCOMPARE(fixture.text(), QStringLiteral(">   \n>   "));
}

void TestMarkdownEditor::testPlainListMidLineSplit() {
  {
    // "- fo|o"
    Fixture fixture(QStringLiteral("- foo"), 0, 4);
    fixture.pressReturn();
    QCOMPARE(fixture.text(), QStringLiteral("- fo\n- o"));
  }
  {
    // "1. fo|o"
    Fixture fixture(QStringLiteral("1. foo"), 0, 5);
    fixture.pressReturn();
    QCOMPARE(fixture.text(), QStringLiteral("1. fo\n2. o"));
  }
}

void TestMarkdownEditor::testPlainListContinuationRegression() {
  {
    Fixture fixture(QStringLiteral("- a"), 0);
    fixture.pressReturn();
    QCOMPARE(fixture.blockText(1), QStringLiteral("- "));
  }
  {
    Fixture fixture(QStringLiteral("1. a"), 0);
    fixture.pressReturn();
    QCOMPARE(fixture.blockText(1), QStringLiteral("2. "));
  }
  {
    Fixture fixture(QStringLiteral("  - [x] a"), 0);
    fixture.pressReturn();
    QCOMPARE(fixture.blockText(1), QStringLiteral("  - [ ] "));
  }
  {
    // Empty list item exit.
    Fixture fixture(QStringLiteral("  - "), 0);
    fixture.pressReturn();
    QCOMPARE(fixture.text(), QStringLiteral("  "));
  }
}

void TestMarkdownEditor::testRepeatedReturnDoesNotLeakContext() {
  Fixture fixture(QStringLiteral("> - "), 0);
  fixture.pressReturn();
  QCOMPARE(fixture.text(), QStringLiteral("> "));

  // The cached context of the first (handled) Return must not influence the
  // next one, which now continues the quote.
  fixture.pressReturn();
  QCOMPARE(fixture.text(), QStringLiteral("> \n> "));

  fixture.pressReturn();
  QCOMPARE(fixture.text(), QStringLiteral("> \n> \n> "));
}

void TestMarkdownEditor::testUndoIsASingleStep() {
  Fixture fixture(QStringLiteral("> foo"), 0);
  const auto before = fixture.text();

  fixture.pressReturn();
  QCOMPARE(fixture.blockText(1), QStringLiteral("> "));

  fixture.edit()->undo();
  QCOMPARE(fixture.text(), before);
}

QTEST_MAIN(tests::TestMarkdownEditor)
