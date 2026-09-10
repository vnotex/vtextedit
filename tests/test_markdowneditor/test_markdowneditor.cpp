#include "test_markdowneditor.h"

#include <QAbstractTextDocumentLayout>
#include <QBuffer>
#include <QDir>
#include <QElapsedTimer>
#include <QImage>
#include <QInputMethodEvent>
#include <QPixmap>
#include <QScrollBar>
#include <QSharedPointer>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>

#include <vtextedit/markdowneditorconfig.h>
#include <vtextedit/markdownhighlighter.h>
#include <vtextedit/markdownutils.h>
#include <vtextedit/previewmgr.h>
#include <vtextedit/previewwidget.h>
#include <vtextedit/texteditorconfig.h>
#include <vtextedit/vmarkdowneditor.h>
#include <vtextedit/vtextedit.h>

// Only the inline busy-state observer is used; no private layout symbol is linked.
#include <markdowneditor/textdocumentlayout.h>

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

  // Select from (@p_startBlock, @p_startCol) to (@p_endBlock, @p_endCol).
  void select(int p_startBlock, int p_startCol, int p_endBlock, int p_endCol) {
    auto doc = m_editor.document();
    const auto startBlock = doc->findBlockByNumber(p_startBlock);
    const auto endBlock = doc->findBlockByNumber(p_endBlock);
    QTextCursor cursor(doc);
    cursor.setPosition(startBlock.position() + p_startCol);
    cursor.setPosition(endBlock.position() + p_endCol, QTextCursor::KeepAnchor);
    edit()->setTextCursor(cursor);
  }

  // Select the whole document content.
  void selectAll() {
    auto doc = m_editor.document();
    QTextCursor cursor(doc);
    cursor.setPosition(0);
    cursor.setPosition(blockEnd(doc->blockCount() - 1), QTextCursor::KeepAnchor);
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

namespace {
// Write a solid p_width x p_height PNG into p_dir and return its file name.
QString writeTestImage(const QTemporaryDir &p_dir, const QString &p_name, int p_width,
                       int p_height) {
  QImage img(p_width, p_height, QImage::Format_ARGB32);
  img.fill(Qt::red);
  const bool ok = img.save(QDir(p_dir.path()).filePath(p_name), "PNG");
  return ok ? p_name : QString();
}

// The resource name PreviewMgr composes for a link. The declared size is part
// of the key, which is what keeps two sizes of one file apart, and the
// destination is length-prefixed rather than fed through QString::arg() --
// a percent-encoded destination such as `a%2F.png` contains what arg() would
// read as a placeholder, which would make the key non-injective.
QString resourceName(const QString &p_shortUrl, int p_width, int p_height) {
  return QString::number(p_shortUrl.size()) + QLatin1Char(':') + p_shortUrl + QLatin1Char('_') +
         QString::number(p_width) + QLatin1Char('_') + QString::number(p_height);
}

// Drive an editor over a local image directory until its previews settle, then
// hand it to p_check.
template <typename Check>
void withImageEditor(const QString &p_markdown, const QString &p_basePath, Check p_check) {
  Fixture fixture(p_markdown, 0);
  fixture.editor()->setBasePath(p_basePath);
  // The base path changed after the first parse, so redo the preview pass.
  fixture.editor()->getHighlighter()->updateHighlight();
  fixture.waitForFreshAst(0);
  p_check(fixture);
}
} // namespace

// The image links must actually reach the highlighter's public channel.
//
// This is the one assertion that catches the construction-order trap in
// MarkdownHighlighterResult: every element-level test can pass while this is
// empty, and then the editor shows no previews at all.
void TestMarkdownEditor::testImageLinksArePublished() {
  Fixture fixture(QStringLiteral("![alt](a.png =500x300)\n"), 0);
  fixture.waitForFreshAst(0);

  const auto &links = fixture.editor()->getHighlighter()->getImageLinks();
  QCOMPARE(links.size(), 1);
  QCOMPARE(links.first().m_destination, QStringLiteral("a.png"));
  QCOMPARE(links.first().m_width, 500);
  QCOMPARE(links.first().m_height, 300);
  QVERIFY(links.first().m_region.m_startPos < links.first().m_region.m_endPos);
}

// The declared width is honored. Before this, `=500x` made the whole link fail
// to match the preview path's regular expression, so nothing was rendered at
// all; and even the matched case hardcoded a 0x0 (natural size) scale.
void TestMarkdownEditor::testSizedImagePreviewIsScaled() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  QVERIFY(!writeTestImage(dir, QStringLiteral("local.png"), 40, 20).isEmpty());

  withImageEditor(QStringLiteral("![](local.png =500x)\n"), dir.path(), [](Fixture &fixture) {
    const QPixmap *img = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((img = fixture.editor()->findImageFromDocumentResourceMgr(
                                  resourceName(QStringLiteral("local.png"), 500, 0))) != nullptr,
                             5000);
    // Width honored, aspect ratio preserved because the height is unspecified.
    QCOMPARE(img->width(), 500);
    QCOMPARE(img->height(), 250);
  });
}

// One destination at two declared sizes is two resources. Keying the cache on
// the URL alone would let whichever rendered first win for both.
void TestMarkdownEditor::testOneUrlAtTwoSizesGetsTwoResources() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  QVERIFY(!writeTestImage(dir, QStringLiteral("local.png"), 40, 20).isEmpty());

  withImageEditor(QStringLiteral("![](local.png =500x)\n\n![](local.png =250x)\n"), dir.path(),
                  [](Fixture &fixture) {
                    const QPixmap *big = nullptr;
                    const QPixmap *small = nullptr;
                    QTRY_VERIFY_WITH_TIMEOUT(
                        (big = fixture.editor()->findImageFromDocumentResourceMgr(
                             resourceName(QStringLiteral("local.png"), 500, 0))) != nullptr &&
                            (small = fixture.editor()->findImageFromDocumentResourceMgr(
                                 resourceName(QStringLiteral("local.png"), 250, 0))) != nullptr,
                        5000);
                    QCOMPARE(big->width(), 500);
                    QCOMPARE(small->width(), 250);
                  });
}

// A declared size is document-supplied and is multiplied again by the scale
// factor before allocation, so it is bounded.
void TestMarkdownEditor::testOversizedImageIsClamped() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  QVERIFY(!writeTestImage(dir, QStringLiteral("local.png"), 40, 20).isEmpty());

  withImageEditor(QStringLiteral("![](local.png =99999x)\n"), dir.path(), [](Fixture &fixture) {
    const QPixmap *img = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((img = fixture.editor()->findImageFromDocumentResourceMgr(
                                  resourceName(QStringLiteral("local.png"), 4096, 0))) != nullptr,
                             5000);
    QCOMPARE(img->width(), 4096);
    // The unclamped resource must not exist under any name.
    QVERIFY(!fixture.editor()->findImageFromDocumentResourceMgr(
        resourceName(QStringLiteral("local.png"), 99999, 0)));
  });
}

namespace {
// A solid p_width x p_height PNG as raw bytes.
QByteArray testImageData(int p_width, int p_height) {
  QImage img(p_width, p_height, QImage::Format_ARGB32);
  img.fill(Qt::blue);
  QByteArray data;
  QBuffer buffer(&data);
  buffer.open(QIODevice::WriteOnly);
  img.save(&buffer, "PNG");
  return data;
}
} // namespace

// Bytes handed over through seedImageData() are used instead of downloading the
// image back, at every declared size the document asks for.
void TestMarkdownEditor::testSeededImageAvoidsDownload() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());

  // Normalization-sensitive: percent-encoded plus a query string, spelled in a
  // form QUrl canonicalizes, so the raw string is NOT a usable key.
  const QString url = QStringLiteral("HTTPS://Example.invalid/img/a%20b.png?v=1");
  QVERIFY(MarkdownUtils::linkUrlToPath(dir.path(), url) != url);

  // Seed BEFORE the links exist, which is the production ordering: the seed has
  // to be in place when the re-highlight resolves the freshly inserted link, or
  // a download is issued.
  Fixture fixture(QString(), 0);
  fixture.editor()->setBasePath(dir.path());
  fixture.editor()->getPreviewMgr()->seedImageData(url, testImageData(60, 30));

  auto cursor = QTextCursor(fixture.editor()->document());
  cursor.insertText(QStringLiteral("![](") + url + QStringLiteral(")\n\n![](") + url +
                    QStringLiteral(" =300x)\n"));
  fixture.editor()->getHighlighter()->updateHighlight();
  fixture.waitForFreshAst(0);

  const QPixmap *natural = nullptr;
  const QPixmap *sized = nullptr;
  QTRY_VERIFY_WITH_TIMEOUT(
      (natural = fixture.editor()->findImageFromDocumentResourceMgr(resourceName(url, 0, 0))) !=
              nullptr &&
          (sized = fixture.editor()->findImageFromDocumentResourceMgr(resourceName(url, 300, 0))) !=
              nullptr,
      5000);
  QCOMPARE(natural->width(), 60);
  QCOMPARE(sized->width(), 300);
}

// The hand-off buffer is bounded and keyed: re-seeding one URL replaces, and a
// 5th distinct seed evicts the 1st.
void TestMarkdownEditor::testSeededImageBufferIsBounded() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());

  const QString first = QStringLiteral("https://example.invalid/1.png");
  const QString fifth = QStringLiteral("https://example.invalid/5.png");

  Fixture fixture(QString(), 0);
  fixture.editor()->setBasePath(dir.path());
  auto *previewMgr = fixture.editor()->getPreviewMgr();

  previewMgr->seedImageData(first, testImageData(10, 10));
  // Replacing in place must keep its slot, not append a duplicate.
  previewMgr->seedImageData(first, testImageData(20, 20));
  for (int i = 2; i <= 4; ++i) {
    previewMgr->seedImageData(QStringLiteral("https://example.invalid/%1.png").arg(i),
                              testImageData(10, 10));
  }

  // 4 distinct entries so far, the first still present (its re-seed did not
  // consume an extra slot) and carrying the REPLACED bytes.
  auto cursor = QTextCursor(fixture.editor()->document());
  cursor.insertText(QStringLiteral("![](") + first + QStringLiteral(")\n"));
  fixture.editor()->getHighlighter()->updateHighlight();
  fixture.waitForFreshAst(0);
  const QPixmap *img = nullptr;
  QTRY_VERIFY_WITH_TIMEOUT((img = fixture.editor()->findImageFromDocumentResourceMgr(
                                resourceName(first, 0, 0))) != nullptr,
                           5000);
  QCOMPARE(img->width(), 20);

  // A 5th distinct seed evicts the oldest, which is the first URL. Ask for BOTH
  // at a declared size that has never been built before: the 5th must render
  // from its seed, and the first - whose seed is gone and whose host does not
  // resolve - must stay absent even after the 5th has arrived.
  previewMgr->seedImageData(fifth, testImageData(30, 30));

  cursor.movePosition(QTextCursor::End);
  cursor.insertText(QStringLiteral("\n\n![](") + first + QStringLiteral(" =123x)\n\n![](") + fifth +
                    QStringLiteral(" =123x)\n"));
  fixture.editor()->getHighlighter()->updateHighlight();
  fixture.waitForFreshAst(0);

  const QPixmap *survivor = nullptr;
  QTRY_VERIFY_WITH_TIMEOUT((survivor = fixture.editor()->findImageFromDocumentResourceMgr(
                                resourceName(fifth, 123, 0))) != nullptr,
                           5000);
  QCOMPARE(survivor->width(), 123);
  QVERIFY(!fixture.editor()->findImageFromDocumentResourceMgr(resourceName(first, 123, 0)));
}

void TestMarkdownEditor::testMultiLineMarkerOnList() {
  Fixture fixture(QStringLiteral("1. a\n2. b\n3. c"), 0);
  fixture.selectAll();
  MarkdownUtils::typeStrikethrough(fixture.edit());
  QCOMPARE(fixture.text(), QStringLiteral("1. ~~a~~\n2. ~~b~~\n3. ~~c~~"));
}

void TestMarkdownEditor::testMultiLineMarkerToggleOff() {
  Fixture fixture(QStringLiteral("1. a\n2. b\n3. c"), 0);
  fixture.selectAll();
  MarkdownUtils::typeStrikethrough(fixture.edit());
  // Without rebuilding the selection: the restored selection is
  // syntax-inclusive, so the same action unwraps.
  MarkdownUtils::typeStrikethrough(fixture.edit());
  QCOMPARE(fixture.text(), QStringLiteral("1. a\n2. b\n3. c"));
}

void TestMarkdownEditor::testMultiLineMarkerMixedSelection() {
  Fixture fixture(QStringLiteral("~~a~~\nb"), 0);
  fixture.selectAll();
  MarkdownUtils::typeStrikethrough(fixture.edit());
  QCOMPARE(fixture.text(), QStringLiteral("~~a~~\n~~b~~"));
  // The first range received no insertion, but the selection still starts at
  // its opening marker.
  QCOMPARE(fixture.edit()->getSelection().start(), 0);
  QCOMPARE(fixture.edit()->getSelection().end(), fixture.blockEnd(1));

  // Normalizing toggle: the second press unwraps everything.
  MarkdownUtils::typeStrikethrough(fixture.edit());
  QCOMPARE(fixture.text(), QStringLiteral("a\nb"));
}

void TestMarkdownEditor::testMultiLineMarkerNesting() {
  Fixture fixture(QStringLiteral("1. a\n2. b\n3. c"), 0);
  fixture.selectAll();
  MarkdownUtils::typeStrikethrough(fixture.edit());
  QCOMPARE(fixture.edit()->getSelection().start(), 3);
  QCOMPARE(fixture.edit()->getSelection().end(), fixture.blockEnd(2));

  MarkdownUtils::typeBold(fixture.edit());
  QCOMPARE(fixture.text(), QStringLiteral("1. **~~a~~**\n2. **~~b~~**\n3. **~~c~~**"));
}

void TestMarkdownEditor::testMultiLineMarkerSkipsBlankLines() {
  Fixture fixture(QStringLiteral("a\n\nc"), 0);
  fixture.selectAll();
  MarkdownUtils::typeStrikethrough(fixture.edit());
  QCOMPARE(fixture.text(), QStringLiteral("~~a~~\n\n~~c~~"));
}

void TestMarkdownEditor::testMultiLineMarkerTrailingBlockBoundary() {
  Fixture fixture(QStringLiteral("a\nb\nc"), 0);
  fixture.select(0, 0, 2, 0);
  MarkdownUtils::typeStrikethrough(fixture.edit());
  QCOMPARE(fixture.text(), QStringLiteral("~~a~~\n~~b~~\nc"));
}

void TestMarkdownEditor::testMultiLineMarkerPartialEdges() {
  Fixture fixture(QStringLiteral("abcdef\nghijkl\nmnopqr"), 0);
  fixture.select(0, 2, 2, 3);
  MarkdownUtils::typeStrikethrough(fixture.edit());
  QCOMPARE(fixture.text(), QStringLiteral("ab~~cdef~~\n~~ghijkl~~\n~~mno~~pqr"));
}

void TestMarkdownEditor::testMultiLineMarkerPrefixes() {
  Fixture fixture(QStringLiteral("> quoted\n## Title\n## 1.2. Title\n- [ ] todo\n> - nested\n"
                                 "  ## Indented\n  ## 1.2. Indented\n> ## Quoted title"),
                  0);
  fixture.selectAll();
  MarkdownUtils::typeStrikethrough(fixture.edit());
  QCOMPARE(fixture.blockText(0), QStringLiteral("> ~~quoted~~"));
  QCOMPARE(fixture.blockText(1), QStringLiteral("## ~~Title~~"));
  QCOMPARE(fixture.blockText(2), QStringLiteral("## 1.2. ~~Title~~"));
  QCOMPARE(fixture.blockText(3), QStringLiteral("- [ ] ~~todo~~"));
  QCOMPARE(fixture.blockText(4), QStringLiteral("> - ~~nested~~"));
  QCOMPARE(fixture.blockText(5), QStringLiteral("  ## ~~Indented~~"));
  QCOMPARE(fixture.blockText(6), QStringLiteral("  ## 1.2. ~~Indented~~"));
  QCOMPARE(fixture.blockText(7), QStringLiteral("> ## ~~Quoted title~~"));
}

void TestMarkdownEditor::testMultiLineMarkerSingleBlockUnchanged() {
  // Single-block selection.
  {
    Fixture fixture(QStringLiteral("abc"), 0);
    fixture.select(0, 0, 0, 3);
    MarkdownUtils::typeStrikethrough(fixture.edit());
    QCOMPARE(fixture.text(), QStringLiteral("~~abc~~"));
    // The single-line path does not restore a syntax-inclusive selection, so
    // the selection has to be rebuilt to unwrap. Unchanged behavior.
    fixture.select(0, 0, 0, 7);
    MarkdownUtils::typeStrikethrough(fixture.edit());
    QCOMPARE(fixture.text(), QStringLiteral("abc"));
  }
  // No selection.
  {
    Fixture fixture(QStringLiteral("abc"), 0, 3);
    MarkdownUtils::typeStrikethrough(fixture.edit());
    QCOMPARE(fixture.text(), QStringLiteral("abc~~~~"));
    QCOMPARE(fixture.edit()->textCursor().positionInBlock(), 5);
  }
}

void TestMarkdownEditor::testMultiLineMarkerAllMarkers() {
  struct Case {
    void (*m_func)(VTextEdit *);
    const char *m_expected;
  };
  const Case cases[] = {
      {&MarkdownUtils::typeBold, "**a**\n**b**"},
      {&MarkdownUtils::typeItalic, "*a*\n*b*"},
      {&MarkdownUtils::typeStrikethrough, "~~a~~\n~~b~~"},
      {&MarkdownUtils::typeMark, "<mark>a</mark>\n<mark>b</mark>"},
      {&MarkdownUtils::typeCode, "`a`\n`b`"},
      {&MarkdownUtils::typeMath, "$a$\n$b$"},
  };

  for (const auto &c : cases) {
    Fixture fixture(QStringLiteral("a\nb"), 0);
    fixture.selectAll();
    c.m_func(fixture.edit());
    QCOMPARE(fixture.text(), QString::fromUtf8(c.m_expected));
    // And back.
    c.m_func(fixture.edit());
    QCOMPARE(fixture.text(), QStringLiteral("a\nb"));
  }
}

void TestMarkdownEditor::testMultiLineMarkerUndo() {
  Fixture fixture(QStringLiteral("1. a\n2. b\n3. c"), 0);
  const auto before = fixture.text();
  fixture.selectAll();
  MarkdownUtils::typeStrikethrough(fixture.edit());
  QVERIFY(fixture.text() != before);

  fixture.edit()->undo();
  QCOMPARE(fixture.text(), before);
}

void TestMarkdownEditor::testMultiLineMarkerBlankOnly() {
  Fixture fixture(QStringLiteral("  \n\n \n"), 0);
  fixture.selectAll();
  const auto before = fixture.text();
  const int revision = fixture.edit()->document()->revision();
  MarkdownUtils::typeStrikethrough(fixture.edit());
  QCOMPARE(fixture.text(), before);
  QCOMPARE(fixture.edit()->document()->revision(), revision);
}

void TestMarkdownEditor::testMultiLineMarkerOverriddenSelection() {
  Fixture fixture(QStringLiteral("a\nb"), 0, 0);
  fixture.edit()->setOverriddenSelection(0, fixture.blockEnd(1));
  QVERIFY(fixture.edit()->hasSelection());
  MarkdownUtils::typeStrikethrough(fixture.edit());
  QCOMPARE(fixture.text(), QStringLiteral("~~a~~\n~~b~~"));
}

// Converting a multi-line selection numbers the items sequentially instead of
// writing `1.` on every line.
void TestMarkdownEditor::testOrderedListSequentialNumbering() {
  Fixture fixture(QStringLiteral("a\nb\nc"), 0);
  fixture.selectAll();
  MarkdownUtils::typeOrderedList(fixture.edit());
  QCOMPARE(fixture.text(), QStringLiteral("1. a\n2. b\n3. c"));
}

void TestMarkdownEditor::testOrderedListFromOtherListTypes() {
  {
    Fixture fixture(QStringLiteral("* a\n* b\n* c"), 0);
    fixture.selectAll();
    MarkdownUtils::typeOrderedList(fixture.edit());
    QCOMPARE(fixture.text(), QStringLiteral("1. a\n2. b\n3. c"));
  }
  {
    Fixture fixture(QStringLiteral("- [ ] a\n- [x] b"), 0);
    fixture.selectAll();
    MarkdownUtils::typeOrderedList(fixture.edit());
    QCOMPARE(fixture.text(), QStringLiteral("1. a\n2. b"));
  }
}

void TestMarkdownEditor::testOrderedListIndentationLevels() {
  // Each indentation level keeps its own counter.
  {
    Fixture fixture(QStringLiteral("a\n  b\n  c\nd"), 0);
    fixture.selectAll();
    MarkdownUtils::typeOrderedList(fixture.edit());
    QCOMPARE(fixture.text(), QStringLiteral("1. a\n  1. b\n  2. c\n2. d"));
  }
  // Leaving a deeper level discards its counter, so it restarts at 1.
  {
    Fixture fixture(QStringLiteral("  a\nb\n  c"), 0);
    fixture.selectAll();
    MarkdownUtils::typeOrderedList(fixture.edit());
    QCOMPARE(fixture.text(), QStringLiteral("  1. a\n1. b\n  1. c"));
  }
  // A toggled-off ordered line consumes no number but still resets the deeper
  // level below it.
  {
    Fixture fixture(QStringLiteral("  a\n1. parent\n  b"), 0);
    fixture.selectAll();
    MarkdownUtils::typeOrderedList(fixture.edit());
    QCOMPARE(fixture.text(), QStringLiteral("  1. a\nparent\n  1. b"));
  }
}

void TestMarkdownEditor::testOrderedListToggleOff() {
  Fixture fixture(QStringLiteral("1. a\n2. b\n3. c"), 0);
  fixture.selectAll();
  MarkdownUtils::typeOrderedList(fixture.edit());
  QCOMPARE(fixture.text(), QStringLiteral("a\nb\nc"));
}

void TestMarkdownEditor::testOrderedListSingleLine() {
  Fixture fixture(QStringLiteral("abc"), 0, 0);
  MarkdownUtils::typeOrderedList(fixture.edit());
  QCOMPARE(fixture.text(), QStringLiteral("1. abc"));
}

// Bounding only the DECLARED axis is not enough. scaleImage() preserves the
// aspect ratio when one axis is unspecified, so an extreme-ratio source turns
// an in-bounds `=4096x` into an enormous allocation along the other axis: a
// 1x8000 image would ask for 4096 x 32,768,000 px. The bound applies to the
// pixmap actually produced.
void TestMarkdownEditor::testAspectRatioDerivedAxisIsBounded() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  QVERIFY(!writeTestImage(dir, QStringLiteral("tall.png"), 1, 8000).isEmpty());

  withImageEditor(QStringLiteral("![](tall.png =4096x)\n"), dir.path(), [](Fixture &fixture) {
    const QPixmap *img = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((img = fixture.editor()->findImageFromDocumentResourceMgr(
                                  resourceName(QStringLiteral("tall.png"), 4096, 0))) != nullptr,
                             5000);
    // Neither axis may exceed the bound, whichever one the ratio derived.
    QVERIFY2(img->width() <= 4096 * 4, qPrintable(QString::number(img->width())));
    QVERIFY2(img->height() <= 4096 * 4, qPrintable(QString::number(img->height())));
  });
}

namespace {
const QString c_tableSource = QStringLiteral("| h1 | header2 |\n| --- | --- |\n| a | b |\n");
const QString c_tableSourceEdited = QStringLiteral("| h1 | header2 |\n| --- | --- |\n| z | b |\n");
const QString c_tableSourceAligned =
    QStringLiteral("| h1  | header2 |\n| --- | ------- |\n| z   | b       |\n");

QSharedPointer<MarkdownEditorConfig> makeTableSourceConfig(bool p_enabled = true) {
  auto config = makeConfig();
  config->m_autoFormatTableSourceEnabled = p_enabled;
  // Source formatting must not depend on the preview type mask or a sheet.
  config->m_inplacePreviewSources = MarkdownEditorConfig::NoInplacePreview;
  return config;
}

int tableSourcePosition(QTextDocument *p_doc, int p_block, int p_column) {
  return p_doc->findBlockByNumber(p_block).position() + p_column;
}

void selectTableSource(VMarkdownEditor &p_editor, int p_anchorBlock, int p_anchorColumn,
                       int p_positionBlock, int p_positionColumn) {
  QTextCursor cursor(p_editor.document());
  cursor.setPosition(tableSourcePosition(p_editor.document(), p_anchorBlock, p_anchorColumn));
  cursor.setPosition(tableSourcePosition(p_editor.document(), p_positionBlock, p_positionColumn),
                     QTextCursor::KeepAnchor);
  p_editor.getTextEdit()->setTextCursor(cursor);
}

void replaceTableSource(VMarkdownEditor &p_editor, int p_block, int p_column, int p_length,
                        const QString &p_text) {
  // Deliberately use an external cursor, not the widget's key-event pipeline.
  QTextCursor cursor(p_editor.document());
  cursor.setPosition(tableSourcePosition(p_editor.document(), p_block, p_column));
  cursor.setPosition(cursor.position() + p_length, QTextCursor::KeepAnchor);
  cursor.beginEditBlock();
  cursor.insertText(p_text);
  cursor.endEditBlock();
}
} // namespace

void TestMarkdownEditor::testTableSourceFormatDebounce() {
  for (bool enabled : {true, false}) {
    VMarkdownEditor editor(makeTableSourceConfig(enabled),
                           QSharedPointer<TextEditorParameters>::create());
    editor.setText(c_tableSource);
    QTRY_VERIFY_WITH_TIMEOUT(editor.getHighlighter()->getBlockContext(2).m_fresh, 5000);
    auto edit = editor.getTextEdit();
    selectTableSource(editor, 2, 2, 2, 3);
    QTest::keyClicks(edit, QStringLiteral("z"));
    QCOMPARE(editor.document()->toPlainText(), c_tableSourceEdited);
    QTest::qWait(200);
    QCOMPARE(editor.document()->toPlainText(), c_tableSourceEdited);

    const QString twiceEdited = QStringLiteral("| h1 | header2 |\n| --- | --- |\n| zx | b |\n");
    const QString aligned =
        QStringLiteral("| h1  | header2 |\n| --- | ------- |\n| zx  | b       |\n");
    QElapsedTimer sinceLastKey;
    qint64 firstFormattedAt = -1;
    const auto transition =
        connect(editor.document(), &QTextDocument::contentsChanged, &editor, [&]() {
          if (firstFormattedAt < 0 && editor.document()->toPlainText() == aligned) {
            firstFormattedAt = sinceLastKey.elapsed();
          }
        });
    sinceLastKey.start();
    QTest::keyClicks(edit, QStringLiteral("x"));
    QTest::qWait(200);
    QCOMPARE(editor.document()->toPlainText(), twiceEdited);
    if (enabled) {
      QTRY_COMPARE_WITH_TIMEOUT(editor.document()->toPlainText(), aligned, 5000);
      QVERIFY2(firstFormattedAt >= 500, qPrintable(QString::number(firstFormattedAt)));
    } else {
      QTest::qWait(800);
      QCOMPARE(editor.document()->toPlainText(), twiceEdited);
      QCOMPARE(firstFormattedAt, qint64(-1));
    }
    disconnect(transition);
  }
}

void TestMarkdownEditor::testTableSourceFormatProgrammaticEdits() {
  const QString firstGap = QStringLiteral("\nBetween first and middle.\n\n");
  const QString middle = QStringLiteral("| keep | untouched |\n| --- | --- |\n| mid | m |\n");
  const QString secondGap = QStringLiteral("\nBetween middle and last.\n\n");
  const QString last = QStringLiteral("| t | tail |\n|---|---|\n| c | d |\n");
  const QString source = c_tableSource + firstGap + middle + secondGap + last;
  const QString expected = c_tableSourceAligned + firstGap + middle + secondGap +
                           QStringLiteral("| t   | tail |\n| --- | ---- |\n| q   | d    |\n");
  VMarkdownEditor editor(makeTableSourceConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.setText(source);
  auto doc = editor.document();
  QTRY_VERIFY_WITH_TIMEOUT(editor.getHighlighter()->getBlockContext(14).m_fresh, 5000);
  selectTableSource(editor, 8, 3, 8, 3);

  // Qt reports one broad change spanning an untouched table in the middle.
  QTextCursor cursor(doc);
  cursor.beginEditBlock();
  cursor.setPosition(tableSourcePosition(doc, 14, 2));
  cursor.setPosition(cursor.position() + 1, QTextCursor::KeepAnchor);
  cursor.insertText(QStringLiteral("q"));
  cursor.setPosition(tableSourcePosition(doc, 2, 2));
  cursor.setPosition(cursor.position() + 1, QTextCursor::KeepAnchor);
  cursor.insertText(QStringLiteral("z"));
  cursor.endEditBlock();
  QTRY_COMPARE_WITH_TIMEOUT(doc->toPlainText(), expected, 5000);
  QCOMPARE(editor.getTextEdit()->textCursor().blockNumber(), 8);
  QCOMPARE(editor.getTextEdit()->textCursor().positionInBlock(), 3);

  // Retained block identity, rather than old absolute positions, determines dirtiness.
  cursor.setPosition(0);
  cursor.insertText(QStringLiteral("Preface.\n\n"));
  const QString shifted = QStringLiteral("Preface.\n\n") + expected;
  const int shiftedUndoSteps = doc->availableUndoSteps();
  QTest::qWait(800);
  QCOMPARE(doc->toPlainText(), shifted);
  QCOMPARE(doc->availableUndoSteps(), shiftedUndoSteps);
  QCOMPARE(editor.getTextEdit()->textCursor().blockNumber(), 10);
  QCOMPARE(editor.getTextEdit()->textCursor().positionInBlock(), 3);

  // Qt splits the first block when lines are inserted at its column zero.
  // Its old handle belongs to the prose, not the unchanged header following it.
  editor.setText(c_tableSource);
  QTRY_VERIFY_WITH_TIMEOUT(editor.getHighlighter()->getBlockContext(2).m_fresh, 5000);
  cursor = QTextCursor(doc);
  cursor.beginEditBlock();
  cursor.insertText(QStringLiteral("Intro.\n\n"));
  cursor.movePosition(QTextCursor::End);
  cursor.insertText(QStringLiteral("\nAfterward.\n"));
  cursor.endEditBlock();
  QTest::qWait(800);
  QCOMPARE(doc->toPlainText(),
           QStringLiteral("Intro.\n\n") + c_tableSource + QStringLiteral("\nAfterward.\n"));
  cursor.setPosition(0);
  cursor.setPosition(8, QTextCursor::KeepAnchor);
  cursor.removeSelectedText();
  QTest::qWait(800);
  QCOMPARE(doc->toPlainText(), c_tableSource + QStringLiteral("\nAfterward.\n"));

  // An inserted duplicate inherits the old header handle, but none of its
  // following rows. Only the new table formats; the original remains untouched.
  editor.setText(c_tableSource);
  QTRY_VERIFY_WITH_TIMEOUT(editor.getHighlighter()->getBlockContext(2).m_fresh, 5000);
  cursor = QTextCursor(doc);
  cursor.insertText(c_tableSource + QLatin1Char('\n'));
  const QString duplicate =
      QStringLiteral("| h1  | header2 |\n| --- | ------- |\n| a   | b       |\n\n") + c_tableSource;
  QTRY_COMPARE_WITH_TIMEOUT(doc->toPlainText(), duplicate, 5000);

  editor.setText(c_tableSource);
  QTRY_VERIFY_WITH_TIMEOUT(editor.getHighlighter()->getBlockContext(2).m_fresh, 5000);
  doc->setUndoRedoEnabled(false);
  replaceTableSource(editor, 2, 2, 1, QStringLiteral("z"));
  selectTableSource(editor, 2, 3, 2, 3);
  QTRY_COMPARE_WITH_TIMEOUT(doc->toPlainText(), c_tableSourceAligned, 5000);
  QVERIFY(!doc->isUndoRedoEnabled());
  QVERIFY(!doc->isUndoAvailable());
  QVERIFY(!doc->isRedoAvailable());
}

void TestMarkdownEditor::testTableSourceFormatLoadAndConfig() {
  // Each replacement clears a populated document, including the terminal character.
  for (int load = 0; load < 3; ++load) {
    VMarkdownEditor editor(makeTableSourceConfig(), QSharedPointer<TextEditorParameters>::create());
    editor.setText(QStringLiteral("previous document\n"));
    if (load == 0) {
      editor.setText(c_tableSource);
    } else if (load == 1) {
      editor.getTextEdit()->setPlainText(c_tableSource);
    } else {
      editor.document()->clear();
      editor.setText(c_tableSource);
    }
    auto doc = editor.document();
    const bool modified = doc->isModified();
    const bool undoAvailable = doc->isUndoAvailable();
    QTest::qWait(800);
    QCOMPARE(doc->toPlainText(), c_tableSource);
    QCOMPARE(doc->isModified(), modified);
    QCOMPARE(doc->isUndoAvailable(), undoAvailable);
  }

  auto config = makeTableSourceConfig(false);
  VMarkdownEditor editor(config, QSharedPointer<TextEditorParameters>::create());
  editor.setText(c_tableSource);
  auto doc = editor.document();
  QTRY_VERIFY_WITH_TIMEOUT(editor.getHighlighter()->getBlockContext(2).m_fresh, 5000);
  config->m_autoFormatTableSourceEnabled = true;
  editor.setConfig(config);
  const bool modified = doc->isModified();
  const int enabledUndoSteps = doc->availableUndoSteps();
  QTest::qWait(800);
  QCOMPARE(doc->toPlainText(), c_tableSource);
  QCOMPARE(doc->isModified(), modified);
  QCOMPARE(doc->availableUndoSteps(), enabledUndoSteps);

  replaceTableSource(editor, 2, 2, 1, QStringLiteral("z"));
  QTest::qWait(200);
  config->m_autoFormatTableSourceEnabled = false;
  editor.setConfig(config);
  QTest::qWait(800);
  QCOMPARE(doc->toPlainText(), c_tableSourceEdited);

  config->m_autoFormatTableSourceEnabled = true;
  editor.setConfig(config);
  QTest::qWait(800);
  QCOMPARE(doc->toPlainText(), c_tableSourceEdited);

  // No event-loop turn or first-parse wait between loading and a real edit.
  editor.setText(c_tableSource);
  replaceTableSource(editor, 2, 2, 1, QStringLiteral("z"));
  selectTableSource(editor, 2, 3, 2, 3);
  QTRY_COMPARE_WITH_TIMEOUT(doc->toPlainText(), c_tableSourceAligned, 5000);

  // A bare clear has no paired insertion; it must not swallow the next cursor edit.
  doc->clear();
  QTest::qWait(800);
  QCOMPARE(doc->toPlainText(), QString());
  QTextCursor cursor(doc);
  cursor.insertText(c_tableSourceEdited);
  selectTableSource(editor, 2, 3, 2, 3);
  QTRY_COMPARE_WITH_TIMEOUT(doc->toPlainText(), c_tableSourceAligned, 5000);

  // With undo disabled, bare clear still differs from setPlainText's paired
  // insertion. Even an immediate external cursor edit must not be swallowed.
  doc->setUndoRedoEnabled(false);
  doc->clear();
  cursor = QTextCursor(doc);
  cursor.insertText(c_tableSourceEdited);
  selectTableSource(editor, 2, 3, 2, 3);
  QTRY_COMPARE_WITH_TIMEOUT(doc->toPlainText(), c_tableSourceAligned, 5000);
  QVERIFY(!doc->isUndoRedoEnabled());
  QVERIFY(!doc->isUndoAvailable());
  doc->setUndoRedoEnabled(true);

  // Removing every visible character does not remove Qt's terminal character.
  editor.setText(QStringLiteral("replace all this visible source\n"));
  QTRY_VERIFY_WITH_TIMEOUT(editor.getHighlighter()->getBlockContext(0).m_fresh, 5000);
  cursor = QTextCursor(doc);
  cursor.select(QTextCursor::Document);
  cursor.insertText(c_tableSourceEdited);
  selectTableSource(editor, 2, 3, 2, 3);
  QTRY_COMPARE_WITH_TIMEOUT(doc->toPlainText(), c_tableSourceAligned, 5000);
}

void TestMarkdownEditor::testTableSourceFormatCursorAndSelection_data() {
  QTest::addColumn<QString>("source");
  QTest::addColumn<QString>("expected");
  QTest::addColumn<int>("anchorBlock");
  QTest::addColumn<int>("anchorColumn");
  QTest::addColumn<int>("positionBlock");
  QTest::addColumn<int>("positionColumn");
  QTest::addColumn<int>("mappedAnchorColumn");
  QTest::addColumn<int>("mappedPositionColumn");
  QTest::addColumn<QString>("selected");
  QTest::addColumn<bool>("overridden");

  QTest::newRow("caret-after-body-value")
      << c_tableSource << c_tableSourceAligned << 2 << 7 << 2 << 7 << 9 << 9 << QString() << false;
  QTest::newRow("forward-body-selection") << c_tableSource << c_tableSourceAligned << 2 << 6 << 2
                                          << 7 << 8 << 9 << QStringLiteral("b") << false;
  QTest::newRow("backward-body-selection") << c_tableSource << c_tableSourceAligned << 2 << 7 << 2
                                           << 6 << 9 << 8 << QStringLiteral("b") << false;
  QTest::newRow("caret-inside-header-content") << c_tableSource << c_tableSourceAligned << 0 << 10
                                               << 0 << 10 << 11 << 11 << QString() << false;
  QTest::newRow("cross-table-next-block-column-zero")
      << c_tableSource << c_tableSourceAligned << 2 << 6 << 3 << 0 << 8 << 0
      << QStringLiteral("b       |\u2029") << false;
  QTest::newRow("overridden-selection-with-unselected-caret")
      << c_tableSource << c_tableSourceAligned << 2 << 3 << 2 << 3 << 3 << 3 << QString() << true;

  QTest::newRow("duplicate-values-use-the-second-occurrence")
      << QStringLiteral("| h1 | header2 | third |\n| --- | --- | --- |\n| a | same | same |\n")
      << QStringLiteral("| h1  | header2 | third |\n| --- | ------- | ----- |\n"
                        "| z   | same    | same  |\n")
      << 2 << 13 << 2 << 17 << 18 << 22 << QStringLiteral("same") << false;
  QTest::newRow("escaped-pipe-is-content")
      << QStringLiteral("| h1 | header2 |\n| --- | --- |\n| a | c\\|d |\n")
      << QStringLiteral("| h1  | header2 |\n| --- | ------- |\n| z   | c\\|d    |\n") << 2 << 7 << 2
      << 10 << 9 << 12 << QStringLiteral("\\|d") << false;

  // The second value starts at UTF-16 column 6, then 8 after formatting:
  // CJK occupies one unit, e + combining acute two, and the emoji two.
  const QString unicodeSource =
      QStringLiteral("| h1 | header2 |\n| --- | --- |\n| a | \u4E2De\u0301\U0001F600 |\n");
  const QString unicodeExpected =
      QStringLiteral("| h1  | header2 |\n| --- | ------- |\n| z   | \u4E2De\u0301\U0001F600   |\n");
  QTest::newRow("cjk-code-unit-not-display-width")
      << unicodeSource << unicodeExpected << 2 << 6 << 2 << 7 << 8 << 9 << QStringLiteral("\u4E2D")
      << false;
  QTest::newRow("combining-mark-offset") << unicodeSource << unicodeExpected << 2 << 8 << 2 << 9
                                         << 10 << 11 << QStringLiteral("\u0301") << false;
  QTest::newRow("surrogate-pair-endpoints") << unicodeSource << unicodeExpected << 2 << 9 << 2 << 11
                                            << 11 << 13 << QStringLiteral("\U0001F600") << false;

  QTest::newRow("content-end-wins-over-unpadded-pipe")
      << QStringLiteral("| h1 | header2 |\n| --- | --- |\n| a |b|\n") << c_tableSourceAligned << 2
      << 6 << 2 << 6 << 9 << 9 << QString() << false;
  QTest::newRow("structural-pipe-keeps-its-column")
      << c_tableSource << c_tableSourceAligned << 2 << 4 << 2 << 4 << 6 << 6 << QString() << false;
  QTest::newRow("leading-padding-keeps-distance-to-content")
      << c_tableSource << c_tableSourceAligned << 2 << 5 << 2 << 5 << 7 << 7 << QString() << false;
  QTest::newRow("trailing-padding-keeps-distance-from-content")
      << QStringLiteral("| h1 | header2 |\n| --- | --- |\n| a | b  |\n") << c_tableSourceAligned
      << 2 << 8 << 2 << 8 << 10 << 10 << QString() << false;
  QTest::newRow("line-end-keeps-distance-after-final-pipe")
      << c_tableSource << c_tableSourceAligned << 2 << 9 << 2 << 9 << 17 << 17 << QString()
      << false;
  QTest::newRow("delimiter-right-colon-keeps-side")
      << QStringLiteral("| h1 | header2 |\n| :--- | ---: |\n| a | b |\n")
      << QStringLiteral("| h1   | header2 |\n| :--- | ------: |\n| z    |       b |\n") << 1 << 12
      << 1 << 12 << 15 << 15 << QString() << false;
}

void TestMarkdownEditor::testTableSourceFormatCursorAndSelection() {
  QFETCH(QString, source);
  QFETCH(QString, expected);
  QFETCH(int, anchorBlock);
  QFETCH(int, anchorColumn);
  QFETCH(int, positionBlock);
  QFETCH(int, positionColumn);
  QFETCH(int, mappedAnchorColumn);
  QFETCH(int, mappedPositionColumn);
  QFETCH(QString, selected);
  QFETCH(bool, overridden);
  VMarkdownEditor editor(makeTableSourceConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.setText(source);
  auto doc = editor.document();
  QTRY_VERIFY_WITH_TIMEOUT(editor.getHighlighter()->getBlockContext(2).m_fresh, 5000);
  replaceTableSource(editor, 2, 2, 1, QStringLiteral("z"));
  selectTableSource(editor, anchorBlock, anchorColumn, positionBlock, positionColumn);
  auto edit = editor.getTextEdit();
  if (overridden) {
    edit->setOverriddenSelection(tableSourcePosition(doc, 2, 6), tableSourcePosition(doc, 2, 7));
    QCOMPARE(edit->selectedText(), QStringLiteral("b"));
    QVERIFY(!edit->textCursor().hasSelection());
  }
  QTRY_COMPARE_WITH_TIMEOUT(doc->toPlainText(), expected, 5000);
  const auto cursor = edit->textCursor();
  QCOMPARE(cursor.anchor(), tableSourcePosition(doc, anchorBlock, mappedAnchorColumn));
  QCOMPARE(cursor.position(), tableSourcePosition(doc, positionBlock, mappedPositionColumn));
  QCOMPARE(cursor.hasSelection(), anchorBlock != positionBlock || anchorColumn != positionColumn);
  QCOMPARE(cursor.selectedText(), selected);
  if (overridden) {
    QCOMPARE(edit->getSelection().start(), tableSourcePosition(doc, 2, 8));
    QCOMPARE(edit->getSelection().end(), tableSourcePosition(doc, 2, 9));
    QCOMPARE(edit->selectedText(), QStringLiteral("b"));
    QVERIFY(!cursor.hasSelection());
  } else {
    QCOMPARE(edit->selectedText(), selected);
  }
}

void TestMarkdownEditor::testTableSourceFormatProtectedPositions() {
  struct ProtectedPosition {
    QString m_source;
    QString m_expected;
    int m_block;
    int m_column;
    int m_retainedColumn;
  };
  const ProtectedPosition cases[] = {
      {QStringLiteral("| h1 | header2 |\n| --- | --- |\n| a          | b |\n"),
       c_tableSourceAligned, 2, 10, 3},
      {QStringLiteral("| h1 | header2 |\n| --- | --- |\n| a |              |\n"),
       QStringLiteral("| h1  | header2 |\n| --- | ------- |\n| z   |         |\n"), 2, 15, 3},
      {QStringLiteral("| h1 | header2 |\n| ---------- | --- |\n| a | b |\n"), c_tableSourceAligned,
       1, 10, 3},
  };
  for (const auto &item : cases) {
    VMarkdownEditor editor(makeTableSourceConfig(), QSharedPointer<TextEditorParameters>::create());
    editor.setText(item.m_source);
    auto doc = editor.document();
    QTRY_VERIFY_WITH_TIMEOUT(editor.getHighlighter()->getBlockContext(2).m_fresh, 5000);
    replaceTableSource(editor, 2, 2, 1, QStringLiteral("z"));
    selectTableSource(editor, item.m_block, item.m_column, item.m_block, item.m_column);
    const QString edited = doc->toPlainText();
    const int position = editor.getTextEdit()->textCursor().position();
    QTest::qWait(800);
    QCOMPARE(doc->toPlainText(), edited);
    QCOMPARE(editor.getTextEdit()->textCursor().position(), position);
    QCOMPARE(editor.getTextEdit()->textCursor().anchor(), position);

    // Cursor movement, with no new source edit, releases only the deferred table.
    selectTableSource(editor, item.m_block, item.m_retainedColumn, item.m_block,
                      item.m_retainedColumn);
    QTRY_COMPARE_WITH_TIMEOUT(doc->toPlainText(), item.m_expected, 5000);
    QCOMPARE(editor.getTextEdit()->textCursor().position(),
             tableSourcePosition(doc, item.m_block, item.m_retainedColumn));
    QVERIFY(!editor.getTextEdit()->textCursor().hasSelection());
  }

  // An empty field maps by distance from its preceding pipe, not a guessed value start.
  {
    VMarkdownEditor editor(makeTableSourceConfig(), QSharedPointer<TextEditorParameters>::create());
    editor.setText(QStringLiteral("| h1 | header2 |\n| --- | --- |\n| a |  |\n"));
    QTRY_VERIFY_WITH_TIMEOUT(editor.getHighlighter()->getBlockContext(2).m_fresh, 5000);
    replaceTableSource(editor, 2, 2, 1, QStringLiteral("z"));
    selectTableSource(editor, 2, 6, 2, 6);
    QTRY_COMPARE_WITH_TIMEOUT(
        editor.document()->toPlainText(),
        QStringLiteral("| h1  | header2 |\n| --- | ------- |\n| z   |         |\n"), 5000);
    QCOMPARE(editor.getTextEdit()->textCursor().positionInBlock(), 8);
  }

  VMarkdownEditor editor(makeTableSourceConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.resize(640, 480);
  editor.show();
  editor.activateWindow();
  auto edit = editor.getTextEdit();
  edit->setFocus();
  QTRY_VERIFY_WITH_TIMEOUT(edit->hasFocus(), 5000);
  editor.setText(c_tableSource);
  auto doc = editor.document();
  QTRY_VERIFY_WITH_TIMEOUT(editor.getHighlighter()->getBlockContext(2).m_fresh, 5000);
  replaceTableSource(editor, 2, 2, 1, QStringLiteral("z"));
  selectTableSource(editor, 2, 3, 2, 3);
  QInputMethodEvent preedit(QStringLiteral("\u3042"), QList<QInputMethodEvent::Attribute>());
  QCoreApplication::sendEvent(edit, &preedit);
  const auto body = doc->findBlockByNumber(2);
  QVERIFY(body.layout());
  QCOMPARE(body.layout()->preeditAreaText(), QStringLiteral("\u3042"));
  const int preeditPosition = edit->textCursor().position();
  QTest::qWait(800);
  QCOMPARE(doc->toPlainText(), c_tableSourceEdited);
  QCOMPARE(body.layout()->preeditAreaText(), QStringLiteral("\u3042"));
  QCOMPARE(edit->textCursor().position(), preeditPosition);
  QVERIFY(edit->hasFocus());

  QInputMethodEvent commit;
  commit.setCommitString(QStringLiteral("\u3042"));
  QCoreApplication::sendEvent(edit, &commit);
  const QString committed = QStringLiteral("| h1 | header2 |\n| --- | --- |\n| z\u3042 | b |\n");
  const QString aligned =
      QStringLiteral("| h1  | header2 |\n| --- | ------- |\n| z\u3042 | b       |\n");
  QCOMPARE(doc->toPlainText(), committed);
  QTest::qWait(200);
  QCOMPARE(doc->toPlainText(), committed);
  QTRY_COMPARE_WITH_TIMEOUT(doc->toPlainText(), aligned, 5000);
  QCOMPARE(doc->findBlockByNumber(2).layout()->preeditAreaText(), QString());
  QCOMPARE(edit->textCursor().positionInBlock(), 4);
}

void TestMarkdownEditor::testTableSourceFormatUndoRedo() {
  for (bool programmatic : {false, true}) {
    VMarkdownEditor editor(makeTableSourceConfig(), QSharedPointer<TextEditorParameters>::create());
    editor.setText(c_tableSource);
    auto doc = editor.document();
    auto edit = editor.getTextEdit();
    QTRY_VERIFY_WITH_TIMEOUT(editor.getHighlighter()->getBlockContext(2).m_fresh, 5000);
    replaceTableSource(editor, 2, 2, 1, QStringLiteral("z"));
    selectTableSource(editor, 2, 3, 2, 3);
    QTRY_COMPARE_WITH_TIMEOUT(doc->toPlainText(), c_tableSourceAligned, 5000);

    if (programmatic) {
      doc->undo();
    } else {
      edit->undo();
    }
    QCOMPARE(doc->toPlainText(), c_tableSource);
    QVERIFY(doc->isRedoAvailable());
    const int redoSteps = doc->availableRedoSteps();
    QTest::qWait(800);
    QCOMPARE(doc->toPlainText(), c_tableSource);
    QCOMPARE(doc->availableRedoSteps(), redoSteps);
    QVERIFY(doc->isRedoAvailable());

    if (programmatic) {
      doc->redo();
    } else {
      edit->redo();
    }
    QCOMPARE(doc->toPlainText(), c_tableSourceAligned);
    const int undoSteps = doc->availableUndoSteps();
    QTest::qWait(800);
    QCOMPARE(doc->toPlainText(), c_tableSourceAligned);
    QCOMPARE(doc->availableUndoSteps(), undoSteps);
    QVERIFY(!doc->isRedoAvailable());

    // Undo a real edit before its debounce, then branch from the undone state.
    editor.setText(c_tableSource);
    QTRY_VERIFY_WITH_TIMEOUT(editor.getHighlighter()->getBlockContext(2).m_fresh, 5000);
    replaceTableSource(editor, 2, 2, 1, QStringLiteral("z"));
    QTest::qWait(200);
    if (programmatic) {
      doc->undo();
    } else {
      edit->undo();
    }
    QCOMPARE(doc->toPlainText(), c_tableSource);
    QVERIFY(doc->isRedoAvailable());
    QTest::qWait(800);
    QCOMPARE(doc->toPlainText(), c_tableSource);
    QVERIFY(doc->isRedoAvailable());

    replaceTableSource(editor, 2, 2, 1, QStringLiteral("y"));
    selectTableSource(editor, 2, 3, 2, 3);
    QVERIFY(!doc->isRedoAvailable());
    const QString branched =
        QStringLiteral("| h1  | header2 |\n| --- | ------- |\n| y   | b       |\n");
    QTRY_COMPARE_WITH_TIMEOUT(doc->toPlainText(), branched, 5000);
    if (programmatic) {
      doc->undo();
    } else {
      edit->undo();
    }
    QCOMPARE(doc->toPlainText(), c_tableSource);
  }
}

void TestMarkdownEditor::testTableSourceFormatSyntaxBoundaries() {
  {
    VMarkdownEditor editor(makeTableSourceConfig(), QSharedPointer<TextEditorParameters>::create());
    const QString source = QStringLiteral("> | left | center | right | \u4E2D\u6587 |\n"
                                          "> | :--- | :---: | ---: | --- |\n"
                                          "> | a | b | c\\|d | e |\n");
    // The existing sheet/parser fixture, with explicit full-source expectations.
    const QString expected =
        QStringLiteral("> | left               | center | right | \u4E2D\u6587 |\n"
                       "> | :----------------- | :----: | ----: | ---- |\n"
                       "> | a much wider value |   b    |  c\\|d | e    |\n");
    editor.setText(source);
    QTRY_VERIFY_WITH_TIMEOUT(editor.getHighlighter()->getBlockContext(2).m_fresh, 5000);
    replaceTableSource(editor, 2, 4, 1, QStringLiteral("a much wider value"));
    selectTableSource(editor, 2, 1, 2, 1);
    QTRY_COMPARE_WITH_TIMEOUT(editor.document()->toPlainText(), expected, 5000);
    QCOMPARE(editor.getTextEdit()->textCursor().positionInBlock(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(editor.getHighlighter()->getBlockContext(2).m_fresh, 5000);
  }

  {
    VMarkdownEditor editor(makeTableSourceConfig(), QSharedPointer<TextEditorParameters>::create());
    editor.setText(QStringLiteral("| h1 | header2 |\n| --- |  |\n| a | b |\n"));
    QTRY_VERIFY_WITH_TIMEOUT(editor.getHighlighter()->getBlockContext(2).m_fresh, 5000);
    replaceTableSource(editor, 1, 8, 0, QStringLiteral("---"));
    selectTableSource(editor, 2, 3, 2, 3);
    QTRY_COMPARE_WITH_TIMEOUT(
        editor.document()->toPlainText(),
        QStringLiteral("| h1  | header2 |\n| --- | ------- |\n| a   | b       |\n"), 5000);
  }

  struct UnsupportedSource {
    QString m_source;
    QString m_edited;
    int m_block;
    int m_column;
  };
  const UnsupportedSource unsupported[] = {
      {QStringLiteral("| h1 | header2 |\n| --- |  |\n| a | b |\n"),
       QStringLiteral("| h1 | header2 |\n| --- |  |\n| z | b |\n"), 2, 2},
      {QStringLiteral("| h1 | header2 |\nnot a delimiter\n| a | b |\n"),
       QStringLiteral("| h1 | header2 |\nnot a delimiter\n| z | b |\n"), 2, 2},
      {QStringLiteral("```markdown\n| h1 | header2 |\n| --- | --- |\n| a | b |\n```\n"),
       QStringLiteral("```markdown\n| h1 | header2 |\n| --- | --- |\n| z | b |\n```\n"), 3, 2},
      {QStringLiteral("<table>\n<tr><th>h1</th><th>header2</th></tr>\n"
                      "<tr><td>a</td><td>b</td></tr>\n</table>\n"),
       QStringLiteral("<table>\n<tr><th>h1</th><th>header2</th></tr>\n"
                      "<tr><td>z</td><td>b</td></tr>\n</table>\n"),
       2, 8},
      // These valid quote spellings have different continuation prefixes. Copying
      // the delimiter's prefix over the body would silently rewrite the container.
      {QStringLiteral("> | h1 | header2 |\n> | --- | --- |\n>| a | b |\n"),
       QStringLiteral("> | h1 | header2 |\n> | --- | --- |\n>| z | b |\n"), 2, 3},
      {QStringLiteral("| h1 | header2 |\n| --- | --- |\n| a | b | excess |\n"),
       QStringLiteral("| h1 | header2 |\n| --- | --- |\n| z | b | excess |\n"), 2, 2},
  };
  for (const auto &item : unsupported) {
    VMarkdownEditor editor(makeTableSourceConfig(), QSharedPointer<TextEditorParameters>::create());
    editor.setText(item.m_source);
    QTRY_VERIFY_WITH_TIMEOUT(editor.getHighlighter()->getBlockContext(item.m_block).m_fresh, 5000);
    replaceTableSource(editor, item.m_block, item.m_column, 1, QStringLiteral("z"));
    selectTableSource(editor, item.m_block, item.m_column + 1, item.m_block, item.m_column + 1);
    auto doc = editor.document();
    QCOMPARE(doc->toPlainText(), item.m_edited);
    const int undoSteps = doc->availableUndoSteps();
    QTest::qWait(800);
    QCOMPARE(doc->toPlainText(), item.m_edited);
    QCOMPARE(doc->availableUndoSteps(), undoSteps);
  }

  {
    VMarkdownEditor editor(makeTableSourceConfig(), QSharedPointer<TextEditorParameters>::create());
    editor.setText(QStringLiteral("| h1 | header2 |\n| --- | --- |\n| a |\n"));
    QTRY_VERIFY_WITH_TIMEOUT(editor.getHighlighter()->getBlockContext(2).m_fresh, 5000);
    replaceTableSource(editor, 2, 2, 1, QStringLiteral("z"));
    // The old last pipe closes the first field, not the newly inserted empty one.
    selectTableSource(editor, 2, 4, 2, 4);
    QTRY_COMPARE_WITH_TIMEOUT(
        editor.document()->toPlainText(),
        QStringLiteral("| h1  | header2 |\n| --- | ------- |\n| z   |         |\n"), 5000);
    QCOMPARE(editor.getTextEdit()->textCursor().positionInBlock(), 6);
  }

  {
    VMarkdownEditor editor(makeTableSourceConfig(), QSharedPointer<TextEditorParameters>::create());
    // 100 CJK characters (200 columns) plus one ASCII character exceed the
    // display-width ceiling despite taking only 101 UTF-16 code units.
    const QString wide = QString(100, QChar(0x4E2D)) + QLatin1Char('w');
    const QString source = QStringLiteral("| h1 | header2 |\n| --- | ------- |\n| a | ") + wide +
                           QStringLiteral(" |\n");
    const QString expected =
        QStringLiteral("| h1 | header2 |\n| --- | --- |\n| z | ") + wide + QStringLiteral(" |\n");
    editor.setText(source);
    QTRY_VERIFY_WITH_TIMEOUT(editor.getHighlighter()->getBlockContext(2).m_fresh, 5000);
    replaceTableSource(editor, 2, 2, 1, QStringLiteral("z"));
    selectTableSource(editor, 2, 3, 2, 3);
    QTRY_COMPARE_WITH_TIMEOUT(editor.document()->toPlainText(), expected, 5000);
  }
}

void TestMarkdownEditor::testTableSourceFormatIdempotence() {
  auto config = makeTableSourceConfig();
  VMarkdownEditor editor(config, QSharedPointer<TextEditorParameters>::create());
  editor.setText(c_tableSource);
  auto doc = editor.document();
  auto edit = editor.getTextEdit();
  QTRY_VERIFY_WITH_TIMEOUT(editor.getHighlighter()->getBlockContext(2).m_fresh, 5000);
  replaceTableSource(editor, 2, 2, 1, QStringLiteral("z"));
  selectTableSource(editor, 2, 7, 2, 6);
  QTest::qWait(200);
  // Applying the same enabled value must not cancel genuine pending work.
  editor.setConfig(config);
  QTRY_COMPARE_WITH_TIMEOUT(doc->toPlainText(), c_tableSourceAligned, 5000);
  QTRY_VERIFY_WITH_TIMEOUT(editor.getHighlighter()->getBlockContext(2).m_fresh, 5000);
  const auto cursor = edit->textCursor();
  // Fresh AST delivery precedes queued rehighlight/layout work, which itself
  // advances QTextDocument::revision() without changing source. Settle it first.
  QTest::qWait(250);
  const int revision = doc->revision();
  const int undoSteps = doc->availableUndoSteps();
  const int redoSteps = doc->availableRedoSteps();
  const bool undoAvailable = doc->isUndoAvailable();
  const bool redoAvailable = doc->isRedoAvailable();
  QTest::qWait(800);
  QCOMPARE(doc->toPlainText(), c_tableSourceAligned);
  QCOMPARE(edit->textCursor().anchor(), cursor.anchor());
  QCOMPARE(edit->textCursor().position(), cursor.position());
  QCOMPARE(doc->revision(), revision);
  QCOMPARE(doc->availableUndoSteps(), undoSteps);
  QCOMPARE(doc->availableRedoSteps(), redoSteps);
  QCOMPARE(doc->isUndoAvailable(), undoAvailable);
  QCOMPARE(doc->isRedoAvailable(), redoAvailable);

  editor.getHighlighter()->rehighlight();
  editor.setConfig(config);
  QTest::qWait(250);
  const int rehighlightRevision = doc->revision();
  const int reconfiguredUndoSteps = doc->availableUndoSteps();
  const int reconfiguredRedoSteps = doc->availableRedoSteps();
  QTest::qWait(800);
  QCOMPARE(doc->toPlainText(), c_tableSourceAligned);
  QCOMPARE(edit->textCursor().anchor(), cursor.anchor());
  QCOMPARE(edit->textCursor().position(), cursor.position());
  QCOMPARE(edit->textCursor().selectedText(), QStringLiteral("b"));
  QCOMPARE(doc->revision(), rehighlightRevision);
  QCOMPARE(doc->availableUndoSteps(), reconfiguredUndoSteps);
  QCOMPARE(doc->availableRedoSteps(), reconfiguredRedoSteps);

  // A real insert/delete sequence can finish at exactly the loaded baseline.
  // It must not align that otherwise-unaligned table or add an empty command.
  editor.setText(c_tableSource);
  QTRY_VERIFY_WITH_TIMEOUT(editor.getHighlighter()->getBlockContext(2).m_fresh, 5000);
  QTextCursor cancelling(doc);
  cancelling.setPosition(tableSourcePosition(doc, 2, 3));
  cancelling.insertText(QStringLiteral("x"));
  cancelling.deletePreviousChar();
  selectTableSource(editor, 2, 3, 2, 3);
  QTRY_VERIFY_WITH_TIMEOUT(editor.getHighlighter()->getBlockContext(2).m_fresh, 5000);
  QTest::qWait(250);
  const int cancelledRevision = doc->revision();
  const int cancelledUndoSteps = doc->availableUndoSteps();
  const int cancelledRedoSteps = doc->availableRedoSteps();
  QTest::qWait(800);
  QCOMPARE(doc->toPlainText(), c_tableSource);
  QCOMPARE(doc->revision(), cancelledRevision);
  QCOMPARE(doc->availableUndoSteps(), cancelledUndoSteps);
  QCOMPARE(doc->availableRedoSteps(), cancelledRedoSteps);
  QCOMPARE(edit->textCursor().positionInBlock(), 3);
  QVERIFY(!edit->textCursor().hasSelection());
}

namespace {
const QString c_headingSource = QStringLiteral("# Title\n## Alpha\n### Detail\n## Beta\n");
const QString c_headingNumbered =
    QStringLiteral("# Title\n## 1. Alpha\n### 1.1. Detail\n## 2. Beta\n");
const QVector<QString> c_headingPrefixes{QString(), QStringLiteral("1."), QStringLiteral("1.1."),
                                         QStringLiteral("2.")};

// These are explicit fixture outputs, not a second implementation of the host's
// title exemption, base-level or counter policy.
VMarkdownEditor::HeadingSectionNumberProvider headingPrefixes(const QVector<QString> &p_prefixes) {
  return [p_prefixes](const QVector<md::HeadingInfo> &) { return p_prefixes; };
}

class HeadingSnapshot : public QObject {
public:
  explicit HeadingSnapshot(VMarkdownEditor &p_editor) {
    connect(&p_editor, &VMarkdownEditor::headingsUpdated, this,
            [this, &p_editor](const QVector<md::HeadingInfo> &p_headings, bool p_numbered) {
              m_headings = p_headings;
              m_numbered = p_numbered;
              m_source = p_editor.document()->toPlainText();
              ++m_updates;
            });
  }

  QStringList titles() const {
    QStringList result;
    for (const auto &heading : m_headings) {
      result.append(heading.m_title);
    }
    return result;
  }

  QVector<int> levels() const {
    QVector<int> result;
    for (const auto &heading : m_headings) {
      result.append(heading.m_level);
    }
    return result;
  }

  QVector<md::HeadingInfo> m_headings;
  QString m_source;
  bool m_numbered = false;
  int m_updates = 0;
};

void selectHeadingSource(VMarkdownEditor &p_editor, int p_anchor, int p_position) {
  QTextCursor cursor(p_editor.document());
  cursor.setPosition(p_anchor);
  cursor.setPosition(p_position, QTextCursor::KeepAnchor);
  p_editor.getTextEdit()->setTextCursor(cursor);
}

void editHeadingSource(VMarkdownEditor &p_editor, int p_start, int p_length, const QString &p_text,
                       bool p_grouped = true) {
  QTextCursor cursor(p_editor.document());
  cursor.setPosition(p_start);
  cursor.setPosition(p_start + p_length, QTextCursor::KeepAnchor);
  if (p_grouped) {
    cursor.beginEditBlock();
  }
  cursor.insertText(p_text);
  if (p_grouped) {
    cursor.endEditBlock();
  }
}
} // namespace

void TestMarkdownEditor::testHeadingSourceDefaultAndActivation() {
  Fixture fixture(c_headingSource);
  auto editor = fixture.editor();
  auto doc = editor->document();
  HeadingSnapshot snapshot(*editor);
  QTRY_COMPARE_WITH_TIMEOUT(snapshot.m_headings.size(), 4, 5000);
  QTest::qWait(800);
  QCOMPARE(fixture.text(), c_headingSource);
  QVERIFY(!snapshot.m_numbered);
  fixture.moveTo(1);
  QTest::keyClicks(fixture.edit(), QStringLiteral("x"));
  const QString typed = QStringLiteral("# Title\n## Alphax\n### Detail\n## Beta\n");
  QTest::qWait(800);
  QCOMPARE(fixture.text(), typed);

  // A callback by itself is not permission to change a loading/read-mode editor.
  editor->setText(c_headingSource);
  editor->setHeadingSectionNumberProvider(headingPrefixes(c_headingPrefixes));
  QTest::qWait(800);
  QCOMPARE(fixture.text(), c_headingSource);
  QVERIFY(!snapshot.m_numbered);
  doc->setModified(false);
  QSignalSpy changed(fixture.edit(), &VTextEdit::contentsChanged);
  editor->setHeadingSectionNumberingActive(true);
  QCOMPARE(fixture.text(), c_headingSource);
  QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), c_headingNumbered, 5000);
  QVERIFY(doc->isModified());
  QVERIFY(changed.count() > 0);
  QTRY_VERIFY_WITH_TIMEOUT(snapshot.m_numbered, 5000);
  QCOMPARE(snapshot.m_source, c_headingNumbered);
  QCOMPARE(snapshot.titles(),
           QStringList({QStringLiteral("Title"), QStringLiteral("1. Alpha"),
                        QStringLiteral("1.1. Detail"), QStringLiteral("2. Beta")}));

  // Removing the provider is an OFF switch, never a source-number eraser.
  editor->setHeadingSectionNumberProvider({});
  QTRY_VERIFY_WITH_TIMEOUT(!snapshot.m_numbered, 5000);
  const int undoSteps = doc->availableUndoSteps();
  QTest::qWait(800);
  QCOMPARE(fixture.text(), c_headingNumbered);
  QCOMPARE(doc->availableUndoSteps(), undoSteps);
  editor->setText(c_headingSource);
  QTest::qWait(800);
  QCOMPARE(fixture.text(), c_headingSource);
  fixture.moveTo(1);
  QTest::keyClicks(fixture.edit(), QStringLiteral("x"));
  QTest::qWait(800);
  QCOMPARE(fixture.text(), typed);
}

void TestMarkdownEditor::testHeadingSourceReadOnly() {
  Fixture fixture(c_headingSource);
  auto editor = fixture.editor();
  auto doc = editor->document();
  HeadingSnapshot snapshot(*editor);
  editor->setReadOnly(true);
  editor->setHeadingSectionNumberProvider(headingPrefixes(c_headingPrefixes));
  editor->setHeadingSectionNumberingActive(true);
  QTRY_COMPARE_WITH_TIMEOUT(snapshot.m_headings.size(), 4, 5000);
  const int undoSteps = doc->availableUndoSteps();
  doc->setModified(false);
  QTest::qWait(800);
  QCOMPARE(fixture.text(), c_headingSource);
  QCOMPARE(doc->availableUndoSteps(), undoSteps);
  QVERIFY(!doc->isModified());
  QVERIFY(!snapshot.m_numbered);
  editor->setReadOnly(false);
  fixture.moveTo(1);
  QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), c_headingNumbered, 5000);

  editor->setHeadingSectionNumberProvider(headingPrefixes(
      {QString(), QStringLiteral("1)"), QStringLiteral("1.1)"), QStringLiteral("2)")}));
  editor->setReadOnly(true);
  QTest::qWait(800);
  QCOMPARE(fixture.text(), c_headingNumbered);
  editor->setReadOnly(false);
  fixture.moveTo(2);
  QTRY_COMPARE_WITH_TIMEOUT(
      fixture.text(), QStringLiteral("# Title\n## 1) Alpha\n### 1.1) Detail\n## 2) Beta\n"), 5000);
}

void TestMarkdownEditor::testHeadingSourceMaintenance() {
  Fixture fixture(QStringLiteral("## 9. Alpha\n### 42) Detail\n## 8 Beta\n"));
  auto editor = fixture.editor();
  QVector<QString> desired{QStringLiteral("1."), QStringLiteral("1.1."), QStringLiteral("2.")};
  editor->setHeadingSectionNumberProvider(
      [&desired](const QVector<md::HeadingInfo> &) { return desired; });
  editor->setHeadingSectionNumberingActive(true);
  const QString numbered = QStringLiteral("## 1. Alpha\n### 1.1. Detail\n## 2. Beta\n");
  QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), numbered, 5000);

  // Reopening does not turn generated prefixes into exempt authored content.
  editor->setText(numbered);
  desired = {QStringLiteral("1."), QStringLiteral("2."), QStringLiteral("2.1."),
             QStringLiteral("3.")};
  editHeadingSource(*editor, 0, 0, QStringLiteral("## Inserted\n"));
  QTRY_COMPARE_WITH_TIMEOUT(
      fixture.text(), QStringLiteral("## 1. Inserted\n## 2. Alpha\n### 2.1. Detail\n## 3. Beta\n"),
      5000);

  desired = {QStringLiteral("1."), QStringLiteral("1.1."), QStringLiteral("2.")};
  editHeadingSource(*editor, 0, fixture.blockEnd(0) + 1, QString());
  QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), numbered, 5000);

  // Move an actual source range; the callback still supplies numbers by order.
  QTextCursor reorder(editor->document());
  reorder.beginEditBlock();
  reorder.setPosition(editor->document()->findBlockByNumber(2).position());
  reorder.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
  reorder.removeSelectedText();
  reorder.setPosition(0);
  reorder.insertText(QStringLiteral("## 2. Beta\n"));
  desired = {QStringLiteral("1."), QStringLiteral("2."), QStringLiteral("2.1.")};
  reorder.endEditBlock();
  QTRY_COMPARE_WITH_TIMEOUT(fixture.text(),
                            QStringLiteral("## 1. Beta\n## 2. Alpha\n### 2.1. Detail\n"), 5000);

  desired = {QStringLiteral("1."), QStringLiteral("2."), QStringLiteral("3.")};
  editHeadingSource(*editor, editor->document()->findBlockByNumber(2).position(), 1, QString());
  QTRY_COMPARE_WITH_TIMEOUT(fixture.text(),
                            QStringLiteral("## 1. Beta\n## 2. Alpha\n## 3. Detail\n"), 5000);
  editor->setHeadingSectionNumberProvider(
      headingPrefixes({QStringLiteral("1)"), QStringLiteral("2)"), QStringLiteral("3)")}));
  QTRY_COMPARE_WITH_TIMEOUT(fixture.text(),
                            QStringLiteral("## 1) Beta\n## 2) Alpha\n## 3) Detail\n"), 5000);
  editor->setHeadingSectionNumberProvider(
      headingPrefixes({QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3")}));
  QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), QStringLiteral("## 1 Beta\n## 2 Alpha\n## 3 Detail\n"),
                            5000);
}

void TestMarkdownEditor::testHeadingSourceInvalidProvider_data() {
  QTest::addColumn<QVector<QString>>("prefixes");
  QTest::newRow("too-short") << QVector<QString>{QStringLiteral("1.")};
  QTest::newRow("too-long") << QVector<QString>{QStringLiteral("1."), QStringLiteral("2."),
                                                QStringLiteral("3.")};
  QTest::newRow("space") << QVector<QString>{QStringLiteral("1."), QStringLiteral("2. ")};
  QTest::newRow("newline") << QVector<QString>{QStringLiteral("1."), QStringLiteral("2.\n")};
  QTest::newRow("escaped-output") << QVector<QString>{QStringLiteral("1."), QStringLiteral("2\\.")};
  QTest::newRow("double-dot") << QVector<QString>{QStringLiteral("1."), QStringLiteral("2..3")};
  QTest::newRow("non-ascii-digit")
      << QVector<QString>{QStringLiteral("1."), QStringLiteral("\u0662.")};
}

void TestMarkdownEditor::testHeadingSourceInvalidProvider() {
  QFETCH(QVector<QString>, prefixes);
  const QString source = QStringLiteral("## Alpha\n## Beta\n");
  Fixture fixture(source);
  auto editor = fixture.editor();
  auto doc = editor->document();
  HeadingSnapshot snapshot(*editor);
  QTRY_COMPARE_WITH_TIMEOUT(snapshot.m_headings.size(), 2, 5000);
  QTest::qWait(250);
  const int revision = doc->revision();
  const int undoSteps = doc->availableUndoSteps();
  doc->setModified(false);
  editor->setHeadingSectionNumberProvider(headingPrefixes(prefixes));
  editor->setHeadingSectionNumberingActive(true);
  QTest::qWait(800);
  // Even the valid first entry must not be applied in a rejected pass.
  QCOMPARE(fixture.text(), source);
  QCOMPARE(doc->revision(), revision);
  QCOMPARE(doc->availableUndoSteps(), undoSteps);
  QVERIFY(!doc->isModified());
  QVERIFY(!snapshot.m_numbered);
  editor->setHeadingSectionNumberProvider(
      headingPrefixes({QStringLiteral("1."), QStringLiteral("2.")}));
  QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), QStringLiteral("## 1. Alpha\n## 2. Beta\n"), 5000);
  QTRY_VERIFY_WITH_TIMEOUT(snapshot.m_numbered, 5000);
}

void TestMarkdownEditor::testHeadingSourceTitleAndEmpty_data() {
  QTest::addColumn<QString>("source");
  QTest::addColumn<QVector<QString>>("prefixes");
  QTest::addColumn<QString>("expected");
  QTest::addColumn<QStringList>("titles");
  QTest::addColumn<bool>("numbered");
  QTest::newRow("empty") << QString() << QVector<QString>() << QString() << QStringList() << false;
  QTest::newRow("authored-title-only")
      << QStringLiteral("prose\n\n# 99. Title\n") << QVector<QString>{QString()}
      << QStringLiteral("prose\n\n# 99. Title\n") << QStringList{QStringLiteral("99. Title")}
      << false;
  QTest::newRow("title-and-skipped-level")
      << QStringLiteral("# 99. Title\n#### [EMPTY]\n")
      << QVector<QString>{QString(), QStringLiteral("1.")}
      << QStringLiteral("# 99. Title\n#### 1. [EMPTY]\n")
      << QStringList{QStringLiteral("99. Title"), QStringLiteral("1. [EMPTY]")} << true;
  QTest::newRow("nonfirst-sole-h1")
      << QStringLiteral("## A\n# B\n")
      << QVector<QString>{QStringLiteral("1.1."), QStringLiteral("2.")}
      << QStringLiteral("## 1.1. A\n# 2. B\n")
      << QStringList{QStringLiteral("1.1. A"), QStringLiteral("2. B")} << true;
  QTest::newRow("multiple-h1") << QStringLiteral("# A\n# B\n")
                               << QVector<QString>{QStringLiteral("1."), QStringLiteral("2.")}
                               << QStringLiteral("# 1. A\n# 2. B\n")
                               << QStringList{QStringLiteral("1. A"), QStringLiteral("2. B")}
                               << true;
  QTest::newRow("bare-and-whitespace-atx")
      << QStringLiteral("##\n###   \n")
      << QVector<QString>{QStringLiteral("1."), QStringLiteral("1.1.")}
      << QStringLiteral("## 1. \n###   1.1. \n")
      << QStringList{QStringLiteral("1."), QStringLiteral("1.1.")} << true;
}

void TestMarkdownEditor::testHeadingSourceTitleAndEmpty() {
  QFETCH(QString, source);
  QFETCH(QVector<QString>, prefixes);
  QFETCH(QString, expected);
  QFETCH(QStringList, titles);
  QFETCH(bool, numbered);
  Fixture fixture(source);
  auto editor = fixture.editor();
  HeadingSnapshot snapshot(*editor);
  editor->setHeadingSectionNumberProvider(headingPrefixes(prefixes));
  editor->setHeadingSectionNumberingActive(true);
  QTRY_VERIFY_WITH_TIMEOUT(snapshot.m_updates > 0, 5000);
  QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), expected, 5000);
  QTRY_COMPARE_WITH_TIMEOUT(snapshot.titles(), titles, 5000);
  QTRY_COMPARE_WITH_TIMEOUT(snapshot.m_numbered, numbered, 5000);
  QTest::qWait(800);
  QCOMPARE(fixture.text(), expected);
  QCOMPARE(snapshot.m_headings.size(), prefixes.size());
  QCOMPARE(snapshot.m_numbered, numbered);
}

void TestMarkdownEditor::testHeadingSourceExemptTitleTransition() {
  Fixture fixture(QStringLiteral("# A\n# B\n"));
  auto editor = fixture.editor();
  QVector<QString> desired{QStringLiteral("1."), QStringLiteral("2.")};
  editor->setHeadingSectionNumberProvider(
      [&desired](const QVector<md::HeadingInfo> &) { return desired; });
  editor->setHeadingSectionNumberingActive(true);
  QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), QStringLiteral("# 1. A\n# 2. B\n"), 5000);
  desired = {QString()};
  editHeadingSource(*editor, 0, fixture.blockEnd(0) + 1, QString());
  HeadingSnapshot snapshot(*editor);
  QTRY_COMPARE_WITH_TIMEOUT(snapshot.titles(), QStringList{QStringLiteral("2. B")}, 5000);
  QTest::qWait(800);
  QCOMPARE(fixture.text(), QStringLiteral("# 2. B\n"));
  QVERIFY(!snapshot.m_numbered);
}

void TestMarkdownEditor::testHeadingSourceSyntax_data() {
  QTest::addColumn<QString>("source");
  QTest::addColumn<QVector<QString>>("prefixes");
  QTest::addColumn<QString>("expected");
  QTest::addColumn<QVector<int>>("levels");
  QTest::addColumn<QStringList>("titles");
  QTest::newRow("atx-containers-unicode-and-inline-content")
      << QStringLiteral(
             "prose \U0001F642\n\n   ##\t9\\.2\\) \t**bold** [link](url) `code` &amp; "
             "\u4E2D\U0001F642 ###\n"
             "\n> ### 12) Quoted\n\n- #### 99 List\n\n```\n# not a heading\n```\n\n<h2>HTML</h2>\n")
      << QVector<QString>{QStringLiteral("1.2."), QStringLiteral("2)"), QStringLiteral("3")}
      << QStringLiteral(
             "prose \U0001F642\n\n   ##\t1.2. **bold** [link](url) `code` &amp; \u4E2D\U0001F642 "
             "###\n"
             "\n> ### 2) Quoted\n\n- #### 3 List\n\n```\n# not a heading\n```\n\n<h2>HTML</h2>\n")
      << QVector<int>{2, 3, 4}
      << QStringList{QStringLiteral("1.2. bold link code & \u4E2D\U0001F642"),
                     QStringLiteral("2) Quoted"), QStringLiteral("3 List")};
  QTest::newRow("markup-is-not-a-structural-prefix")
      << QStringLiteral("## **1. Topic**\n### `2)` code\n")
      << QVector<QString>{QStringLiteral("1000."), QStringLiteral("1000.1)")}
      << QStringLiteral("## 1000. **1. Topic**\n### 1000.1) `2)` code\n") << QVector<int>{2, 3}
      << QStringList{QStringLiteral("1000. 1. Topic"), QStringLiteral("1000.1) 2) code")};
  QTest::newRow("prefix-needs-a-separator")
      << QStringLiteral("## 123abc\n## 1.2topic\n")
      << QVector<QString>{QStringLiteral("1."), QStringLiteral("2.")}
      << QStringLiteral("## 1. 123abc\n## 2. 1.2topic\n") << QVector<int>{2, 2}
      << QStringList{QStringLiteral("1. 123abc"), QStringLiteral("2. 1.2topic")};
  QTest::newRow("unicode-prefix-separator")
      << QStringLiteral("## 12345.\u00A0Alpha\n") << QVector<QString>{QStringLiteral("1.")}
      << QStringLiteral("## 1. Alpha\n") << QVector<int>{2}
      << QStringList{QStringLiteral("1. Alpha")};
  QTest::newRow("setext-indented-hard-and-soft-breaks")
      << QStringLiteral("# Title\n\n  **Alpha**  \n  continuation `code`\n  last\n  -----\n")
      << QVector<QString>{QString(), QStringLiteral("1.")}
      << QStringLiteral("# Title\n\n  1\\. **Alpha**  \n  continuation `code`\n  last\n  -----\n")
      << QVector<int>{1, 2}
      << QStringList{QStringLiteral("Title"), QStringLiteral("1. Alpha continuation code last")};
  QTest::newRow("setext-quote-and-list")
      << QStringLiteral("> Alpha  \n> continuation\n> -----\n\n- Beta\n  continuation\n  -----\n")
      << QVector<QString>{QStringLiteral("1)"), QStringLiteral("2.")}
      << QStringLiteral(
             "> 1\\) Alpha  \n> continuation\n> -----\n\n- 2\\. Beta\n  continuation\n  -----\n")
      << QVector<int>{2, 2}
      << QStringList{QStringLiteral("1) Alpha continuation"),
                     QStringLiteral("2. Beta continuation")};
  QTest::newRow("setext-prefix-only-first-line")
      << QStringLiteral("9\\.\ncontinuation\n-----\n") << QVector<QString>{QStringLiteral("1.")}
      << QStringLiteral("1\\. \ncontinuation\n-----\n") << QVector<int>{2}
      << QStringList{QStringLiteral("1. continuation")};
  QTest::newRow("setext-leading-link-is-title-content")
      << QStringLiteral("[link](url)\n-----\n") << QVector<QString>{QStringLiteral("1.")}
      << QStringLiteral("1\\. [link](url)\n-----\n") << QVector<int>{2}
      << QStringList{QStringLiteral("1. link")};
  QTest::newRow("setext-multicomponent-is-not-escaped")
      << QStringLiteral("99\\.2\\) Alpha\n-----\n") << QVector<QString>{QStringLiteral("1.2)")}
      << QStringLiteral("1.2) Alpha\n-----\n") << QVector<int>{2}
      << QStringList{QStringLiteral("1.2) Alpha")};
}

void TestMarkdownEditor::testHeadingSourceSyntax() {
  QFETCH(QString, source);
  QFETCH(QVector<QString>, prefixes);
  QFETCH(QString, expected);
  QFETCH(QVector<int>, levels);
  QFETCH(QStringList, titles);
  Fixture fixture(source);
  auto editor = fixture.editor();
  auto doc = editor->document();
  HeadingSnapshot snapshot(*editor);
  editor->setHeadingSectionNumberProvider(headingPrefixes(prefixes));
  editor->setHeadingSectionNumberingActive(true);
  QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), expected, 5000);
  QTRY_VERIFY_WITH_TIMEOUT(snapshot.m_numbered, 5000);
  QCOMPARE(snapshot.levels(), levels);
  QCOMPARE(snapshot.titles(), titles);
  QCOMPARE(snapshot.m_source, expected);
  int previousEnd = 0;
  for (const auto &heading : snapshot.m_headings) {
    QVERIFY(heading.m_startPos >= previousEnd);
    QVERIFY(heading.m_endPos > heading.m_startPos);
    QVERIFY(heading.m_endPos <= expected.size());
    previousEnd = heading.m_endPos;
  }

  // A second explicit pass must recognize its own escaped prefixes. It must
  // not dirty the note, create an undo action, or even open an empty edit block.
  QTest::qWait(250);
  const int revision = doc->revision();
  const int undoSteps = doc->availableUndoSteps();
  doc->setModified(false);
  editor->setHeadingSectionNumberProvider(headingPrefixes(prefixes));
  QTest::qWait(800);
  QCOMPARE(fixture.text(), expected);
  QCOMPARE(doc->revision(), revision);
  QCOMPARE(doc->availableUndoSteps(), undoSteps);
  QVERIFY(!doc->isModified());
  QVERIFY(snapshot.m_numbered);
}

void TestMarkdownEditor::testHeadingSourceSetextPatterns() {
  Fixture fixture(QStringLiteral("# Title\n\nAlpha\n-----\n"));
  auto editor = fixture.editor();
  HeadingSnapshot snapshot(*editor);
  editor->setHeadingSectionNumberingActive(true);
  const QStringList desired{QStringLiteral("1."), QStringLiteral("1)"), QStringLiteral("1")};
  const QStringList written{QStringLiteral("1\\."), QStringLiteral("1\\)"), QStringLiteral("1")};
  for (int i = 0; i < desired.size(); ++i) {
    editor->setHeadingSectionNumberProvider(headingPrefixes({QString(), desired[i]}));
    const QString expected =
        QStringLiteral("# Title\n\n") + written[i] + QStringLiteral(" Alpha\n-----\n");
    QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), expected, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(snapshot.m_source, expected, 5000);
    QVERIFY(snapshot.m_numbered);
    QCOMPARE(snapshot.levels(), QVector<int>({1, 2}));
    QCOMPARE(snapshot.titles(),
             QStringList({QStringLiteral("Title"), desired[i] + QStringLiteral(" Alpha")}));
  }
}

void TestMarkdownEditor::testHeadingSourceUnresolvedSetextBoundary() {
  const QString source = QStringLiteral("[ref]: /url\nAlpha\n-----\n");
  Fixture fixture(source);
  auto editor = fixture.editor();
  auto doc = editor->document();
  HeadingSnapshot snapshot(*editor);
  QTRY_COMPARE_WITH_TIMEOUT(snapshot.titles(), QStringList{QStringLiteral("Alpha")}, 5000);
  QCOMPARE(snapshot.levels(), QVector<int>{2});
  QTest::qWait(250);
  const int revision = doc->revision();
  const int undoSteps = doc->availableUndoSteps();
  doc->setModified(false);
  int providerCalls = 0;
  editor->setHeadingSectionNumberProvider([&providerCalls](const QVector<md::HeadingInfo> &) {
    ++providerCalls;
    return QVector<QString>{QStringLiteral("1.")};
  });
  editor->setHeadingSectionNumberingActive(true);
  QTest::qWait(800);
  // cmark's heading origin may include a discarded reference definition.
  // An unresolved title boundary must never be passed to the host or rewritten.
  QCOMPARE(providerCalls, 0);
  QCOMPARE(fixture.text(), source);
  QCOMPARE(snapshot.titles(), QStringList{QStringLiteral("Alpha")});
  QVERIFY(!snapshot.m_numbered);
  QCOMPARE(doc->revision(), revision);
  QCOMPARE(doc->availableUndoSteps(), undoSteps);
  QVERIFY(!doc->isModified());
}

void TestMarkdownEditor::testHeadingSourceMarkerActions() {
  struct Marker {
    void (*m_apply)(VTextEdit *);
    QString m_marker;
  };
  const Marker markers[] = {{&MarkdownUtils::typeBold, QStringLiteral("**")},
                            {&MarkdownUtils::typeItalic, QStringLiteral("*")},
                            {&MarkdownUtils::typeCode, QStringLiteral("`")}};
  const QString source = QStringLiteral("# 1000) Alpha\n## 1.2 Beta\n### 3.4. Gamma");
  for (const auto &marker : markers) {
    Fixture fixture(source);
    auto editor = fixture.editor();
    HeadingSnapshot snapshot(*editor);
    editor->setHeadingSectionNumberProvider(
        headingPrefixes({QStringLiteral("1000)"), QStringLiteral("1.2"), QStringLiteral("3.4.")}));
    editor->setHeadingSectionNumberingActive(true);
    QTRY_VERIFY_WITH_TIMEOUT(snapshot.m_numbered, 5000);
    fixture.selectAll();
    marker.m_apply(fixture.edit());
    const QString expected =
        QStringLiteral("# 1000) ") + marker.m_marker + QStringLiteral("Alpha") + marker.m_marker +
        QStringLiteral("\n## 1.2 ") + marker.m_marker + QStringLiteral("Beta") + marker.m_marker +
        QStringLiteral("\n### 3.4. ") + marker.m_marker + QStringLiteral("Gamma") + marker.m_marker;
    QCOMPARE(fixture.text(), expected);
    QTest::qWait(800);
    QCOMPARE(fixture.text(), expected);
    marker.m_apply(fixture.edit());
    QCOMPARE(fixture.text(), source);
  }
}

void TestMarkdownEditor::testHeadingSourceDebounce() {
  for (bool keyboard : {false, true}) {
    Fixture fixture(QStringLiteral("## 1. Alpha\n"), 0);
    auto editor = fixture.editor();
    HeadingSnapshot snapshot(*editor);
    editor->setHeadingSectionNumberProvider(headingPrefixes({QStringLiteral("1.")}));
    editor->setHeadingSectionNumberingActive(true);
    QTRY_VERIFY_WITH_TIMEOUT(snapshot.m_numbered, 5000);
    if (keyboard) {
      fixture.select(0, 3, 0, 4);
      QTest::keyClicks(fixture.edit(), QStringLiteral("9"));
    } else {
      editHeadingSource(*editor, 3, 1, QStringLiteral("9"));
    }
    const QString first = QStringLiteral("## 9. Alpha\n");
    QCOMPARE(fixture.text(), first);
    QTest::qWait(200);
    QCOMPARE(fixture.text(), first);
    QElapsedTimer sinceLastEdit;
    qint64 normalizedAt = -1;
    const QString expected = QStringLiteral("## 1. Alphax\n");
    const auto transition =
        connect(editor->document(), &QTextDocument::contentsChanged, editor, [&]() {
          if (normalizedAt < 0 && fixture.text() == expected) {
            normalizedAt = sinceLastEdit.elapsed();
          }
        });
    sinceLastEdit.start();
    if (keyboard) {
      fixture.moveTo(0);
      QTest::keyClicks(fixture.edit(), QStringLiteral("x"));
    } else {
      editHeadingSource(*editor, fixture.blockEnd(0), 0, QStringLiteral("x"));
    }
    const QString twiceEdited = QStringLiteral("## 9. Alphax\n");
    QTest::qWait(200);
    QCOMPARE(fixture.text(), twiceEdited);
    // Highlight-only revision changes cannot make a current full parse stale
    // forever, or restart the source-inactivity interval.
    editor->getHighlighter()->rehighlight();
    QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), expected, 5000);
    QVERIFY2(normalizedAt >= 500, qPrintable(QString::number(normalizedAt)));
    disconnect(transition);
  }
}

void TestMarkdownEditor::testHeadingSourceCancellationAndLoad() {
  Fixture fixture(QStringLiteral("## Original\n"));
  auto editor = fixture.editor();
  auto doc = editor->document();
  editor->setHeadingSectionNumberingActive(true);
  editor->setHeadingSectionNumberProvider(headingPrefixes({QStringLiteral("1.")}));
  QTest::qWait(200);
  editor->setHeadingSectionNumberingActive(false);
  QTest::qWait(800);
  QCOMPARE(fixture.text(), QStringLiteral("## Original\n"));
  editor->setHeadingSectionNumberingActive(true);
  QCOMPARE(fixture.text(), QStringLiteral("## Original\n"));
  QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), QStringLiteral("## 1. Original\n"), 5000);

  editHeadingSource(*editor, 3, 1, QStringLiteral("9"));
  QTest::qWait(200);
  editor->setHeadingSectionNumberProvider({});
  QTest::qWait(800);
  QCOMPARE(fixture.text(), QStringLiteral("## 9. Original\n"));

  editor->setHeadingSectionNumberProvider(headingPrefixes({QStringLiteral("1.")}));
  // Replace the whole document while old prefix positions and a timer are owed.
  for (int load = 0; load < 3; ++load) {
    editor->setText(QStringLiteral("paragraph before\n\n## Previous\n"));
    QTest::qWait(200);
    if (load == 0) {
      editor->setText(QStringLiteral("## New\n"));
    } else if (load == 1) {
      doc->setPlainText(QStringLiteral("## New\n"));
    } else {
      doc->clear();
      QTextCursor cursor(doc);
      cursor.insertText(QStringLiteral("## New\n"));
    }
    // No event-loop turn between loading and the first genuine source edit.
    editHeadingSource(*editor, 6, 0, QStringLiteral("x"));
    QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), QStringLiteral("## 1. Newx\n"), 5000);
  }

  // Bare clear must not swallow the next edit even when there is no undo stack.
  doc->setUndoRedoEnabled(false);
  doc->clear();
  QTextCursor cursor(doc);
  cursor.insertText(QStringLiteral("## No undo\n"));
  QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), QStringLiteral("## 1. No undo\n"), 5000);
  QVERIFY(!doc->isUndoAvailable());
  QVERIFY(!doc->isRedoAvailable());
}

void TestMarkdownEditor::testHeadingSourceFreshParseAndPublication() {
  Fixture fixture(QStringLiteral("## Before\n"));
  auto editor = fixture.editor();
  auto highlighter = editor->getHighlighter();
  HeadingSnapshot snapshot(*editor);
  QVector<md::HeadingInfo> parsed;
  const auto rawConnection =
      connect(highlighter, &MarkdownHighlighter::headingsUpdated, editor,
              [&parsed](const QVector<md::HeadingInfo> &p_headings) { parsed = p_headings; });
  QTRY_COMPARE_WITH_TIMEOUT(snapshot.titles(), QStringList{QStringLiteral("Before")}, 5000);
  editor->setHeadingSectionNumberProvider(headingPrefixes({QStringLiteral("1.")}));
  editor->setHeadingSectionNumberingActive(true);
  QVERIFY(!snapshot.m_numbered);
  QCOMPARE(snapshot.titles(), QStringList{QStringLiteral("Before")});

  // Invalidate the accepted full result without pumping events. The old heading
  // offset is now in prose; fast block context must not authorize that rewrite.
  editHeadingSource(*editor, 0, 0, QStringLiteral("\U0001F642 shifted prose\n\n"));
  const QString pending = QStringLiteral("\U0001F642 shifted prose\n\n## Before\n");
  QCOMPARE(fixture.text(), pending);
  QTest::qWait(200);
  QCOMPARE(fixture.text(), pending);
  QVERIFY(!snapshot.m_numbered);
  QTRY_COMPARE_WITH_TIMEOUT(fixture.text(),
                            QStringLiteral("\U0001F642 shifted prose\n\n## 1. Before\n"), 5000);
  QTRY_VERIFY_WITH_TIMEOUT(snapshot.m_numbered, 5000);
  QCOMPARE(snapshot.m_headings.size(), parsed.size());
  for (int i = 0; i < parsed.size(); ++i) {
    QCOMPARE(snapshot.m_headings[i].m_title, parsed[i].m_title);
    QCOMPARE(snapshot.m_headings[i].m_anchorText, parsed[i].m_anchorText);
    QCOMPARE(snapshot.m_headings[i].m_level, parsed[i].m_level);
    QCOMPARE(snapshot.m_headings[i].m_startPos, parsed[i].m_startPos);
    QCOMPARE(snapshot.m_headings[i].m_endPos, parsed[i].m_endPos);
  }
  QCOMPARE(snapshot.m_headings[0].m_startPos, fixture.text().indexOf(QStringLiteral("##")));
  disconnect(rawConnection);

  // A provider/activation request made on full-parse delivery cannot mutate the
  // source inside that stack, even if a previous request has already aged out.
  editor->setHeadingSectionNumberingActive(false);
  editor->setText(QStringLiteral("## New\n"));
  QTest::qWait(800);
  bool delivered = false;
  bool changedOnStack = false;
  const auto publication = connect(highlighter, &MarkdownHighlighter::headingsUpdated, editor,
                                   [&](const QVector<md::HeadingInfo> &) {
                                     if (delivered) {
                                       return;
                                     }
                                     delivered = true;
                                     editor->setHeadingSectionNumberingActive(true);
                                     changedOnStack = fixture.text() != QStringLiteral("## New\n");
                                   });
  highlighter->updateHighlight();
  QTRY_VERIFY_WITH_TIMEOUT(delivered, 5000);
  QVERIFY(!changedOnStack);
  QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), QStringLiteral("## 1. New\n"), 5000);
  disconnect(publication);
}

void TestMarkdownEditor::testHeadingSourceProviderInvalidation() {
  Fixture fixture(QStringLiteral("## Alpha\n"));
  auto editor = fixture.editor();
  HeadingSnapshot snapshot(*editor);
  QTRY_COMPARE_WITH_TIMEOUT(snapshot.m_headings.size(), 1, 5000);
  bool replaced = false;
  editor->setHeadingSectionNumberingActive(true);
  editor->setHeadingSectionNumberProvider([editor, &replaced](const QVector<md::HeadingInfo> &) {
    if (!replaced) {
      replaced = true;
      editor->setHeadingSectionNumberProvider(headingPrefixes({QStringLiteral("2)")}));
    }
    return QVector<QString>{QStringLiteral("1.")};
  });
  QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), QStringLiteral("## 2) Alpha\n"), 5000);
  QTRY_VERIFY_WITH_TIMEOUT(snapshot.m_numbered, 5000);
  QTest::qWait(800);
  QCOMPARE(fixture.text(), QStringLiteral("## 2) Alpha\n"));
}

void TestMarkdownEditor::testHeadingSourceUndoRedo_data() {
  QTest::addColumn<bool>("grouped");
  QTest::addColumn<bool>("documentReplay");
  QTest::newRow("grouped-editor") << true << false;
  QTest::newRow("grouped-document") << true << true;
  QTest::newRow("bare-editor") << false << false;
  QTest::newRow("bare-document") << false << true;
}

void TestMarkdownEditor::testHeadingSourceUndoRedo() {
  QFETCH(bool, grouped);
  QFETCH(bool, documentReplay);
  const QString original = QStringLiteral("# Title\n## 1. Alpha\n");
  const QString inserted = QStringLiteral("# Title\n## New\n## 1. Alpha\n");
  const QString numbered = QStringLiteral("# Title\n## 1. New\n## 2. Alpha\n");
  QVector<QString> desired{QString(), QStringLiteral("1.")};
  Fixture fixture(original);
  auto editor = fixture.editor();
  auto doc = editor->document();
  HeadingSnapshot snapshot(*editor);
  editor->setHeadingSectionNumberProvider(
      [&desired](const QVector<md::HeadingInfo> &) { return desired; });
  editor->setHeadingSectionNumberingActive(true);
  QTRY_VERIFY_WITH_TIMEOUT(snapshot.m_numbered, 5000);
  desired = {QString(), QStringLiteral("1."), QStringLiteral("2.")};
  editHeadingSource(*editor, 8, 0, QStringLiteral("## New\n"), grouped);
  QCOMPARE(fixture.text(), inserted);
  QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), numbered, 5000);
  const auto undo = [&]() {
    if (documentReplay) {
      doc->undo();
    } else {
      fixture.edit()->undo();
    }
  };
  const auto redo = [&]() {
    if (documentReplay) {
      doc->redo();
    } else {
      fixture.edit()->redo();
    }
  };
  undo();
  const bool separateNormalization = fixture.text() == inserted;
  if (grouped) {
    QCOMPARE(fixture.text(), original);
  } else {
    // Qt may not promote a bare insertion into a joined edit block. Both
    // permitted forms must leave a useful, stable undo and redo history.
    QVERIFY(fixture.text() == original || separateNormalization);
  }
  const QString undone = fixture.text();
  const int undoSteps = doc->availableUndoSteps();
  const int redoSteps = doc->availableRedoSteps();
  QVERIFY(doc->isRedoAvailable());
  editor->setHeadingSectionNumberingActive(false);
  editor->setHeadingSectionNumberingActive(true);
  editor->getHighlighter()->updateHighlight();
  fixture.moveTo(0);
  QTest::qWait(800);
  QCOMPARE(fixture.text(), undone);
  QCOMPARE(doc->availableUndoSteps(), undoSteps);
  QCOMPARE(doc->availableRedoSteps(), redoSteps);

  if (separateNormalization) {
    undo();
    QCOMPARE(fixture.text(), original);
    QTest::qWait(800);
    QCOMPARE(fixture.text(), original);
    redo();
    QCOMPARE(fixture.text(), inserted);
    const int intermediateRedo = doc->availableRedoSteps();
    const int intermediateUndo = doc->availableUndoSteps();
    QTest::qWait(800);
    QCOMPARE(fixture.text(), inserted);
    QCOMPARE(doc->availableRedoSteps(), intermediateRedo);
    QCOMPARE(doc->availableUndoSteps(), intermediateUndo);
  }
  redo();
  QCOMPARE(fixture.text(), numbered);
  const int restoredUndo = doc->availableUndoSteps();
  QTest::qWait(800);
  QCOMPARE(fixture.text(), numbered);
  QCOMPARE(doc->availableUndoSteps(), restoredUndo);
  QVERIFY(!doc->isRedoAvailable());
}

void TestMarkdownEditor::testHeadingSourceUndoBeforeDebounceAndBranch() {
  const QString original = QStringLiteral("## 1. Alpha\n");
  Fixture fixture(original);
  auto editor = fixture.editor();
  auto doc = editor->document();
  HeadingSnapshot snapshot(*editor);
  editor->setHeadingSectionNumberProvider(headingPrefixes({QStringLiteral("1.")}));
  editor->setHeadingSectionNumberingActive(true);
  QTRY_VERIFY_WITH_TIMEOUT(snapshot.m_numbered, 5000);
  editHeadingSource(*editor, 3, 1, QStringLiteral("9"));
  QTest::qWait(200);
  doc->undo();
  QCOMPARE(fixture.text(), original);
  const int undoSteps = doc->availableUndoSteps();
  const int redoSteps = doc->availableRedoSteps();
  QTest::qWait(800);
  QCOMPARE(fixture.text(), original);
  QCOMPARE(doc->availableUndoSteps(), undoSteps);
  QCOMPARE(doc->availableRedoSteps(), redoSteps);
  QVERIFY(doc->isRedoAvailable());

  editHeadingSource(*editor, 3, 1, QStringLiteral("8"));
  QVERIFY(!doc->isRedoAvailable());
  editHeadingSource(*editor, fixture.blockEnd(0), 0, QStringLiteral(" branch"));
  QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), QStringLiteral("## 1. Alpha branch\n"), 5000);
  fixture.edit()->undo();
  QCOMPARE(fixture.text(), QStringLiteral("## 8. Alpha\n"));
  QTest::qWait(800);
  QCOMPARE(fixture.text(), QStringLiteral("## 8. Alpha\n"));
  QVERIFY(doc->isRedoAvailable());
}

void TestMarkdownEditor::testHeadingSourceExplicitHistory() {
  const QString original = QStringLiteral("## Alpha\n");
  const QString edited = QStringLiteral("## Alpha\nprose\n");
  Fixture fixture(original);
  auto editor = fixture.editor();
  auto doc = editor->document();
  editHeadingSource(*editor, original.size(), 0, QStringLiteral("prose\n"));
  QCOMPARE(fixture.text(), edited);
  editor->setHeadingSectionNumberProvider(headingPrefixes({QStringLiteral("1.")}));
  editor->setHeadingSectionNumberingActive(true);
  QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), QStringLiteral("## 1. Alpha\nprose\n"), 5000);
  doc->undo();
  QCOMPARE(fixture.text(), edited);
  const int undoSteps = doc->availableUndoSteps();
  const int redoSteps = doc->availableRedoSteps();
  editor->getHighlighter()->rehighlight();
  editor->setHeadingSectionNumberingActive(false);
  editor->setHeadingSectionNumberingActive(true);
  QTest::qWait(800);
  QCOMPARE(fixture.text(), edited);
  QCOMPARE(doc->availableUndoSteps(), undoSteps);
  QCOMPARE(doc->availableRedoSteps(), redoSteps);
  doc->undo();
  QCOMPARE(fixture.text(), original);

  // An explicit provider/pattern change is the only non-source event here
  // that intentionally clears replay suppression and discards the redo branch.
  editor->setHeadingSectionNumberProvider(headingPrefixes({QStringLiteral("1)")}));
  QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), QStringLiteral("## 1) Alpha\n"), 5000);
  QVERIFY(!doc->isRedoAvailable());
  editor->setHeadingSectionNumberProvider(headingPrefixes({QStringLiteral("1")}));
  QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), QStringLiteral("## 1 Alpha\n"), 5000);
  doc->undo();
  QCOMPARE(fixture.text(), QStringLiteral("## 1) Alpha\n"));
  QTest::qWait(800);
  QCOMPARE(fixture.text(), QStringLiteral("## 1) Alpha\n"));
  doc->undo();
  QCOMPARE(fixture.text(), original);
}

void TestMarkdownEditor::testHeadingSourceCursorAndSelection_data() {
  QTest::addColumn<QString>("source");
  QTest::addColumn<int>("anchor");
  QTest::addColumn<int>("position");
  QTest::addColumn<int>("mappedAnchor");
  QTest::addColumn<int>("mappedPosition");
  QTest::addColumn<QString>("selected");
  const QString insertion = QStringLiteral("## Alpha\n## Beta\n");
  QTest::newRow("caret-at-insertion") << insertion << 3 << 3 << 6 << 6 << QString();
  QTest::newRow("forward-title") << insertion << 3 << 8 << 6 << 11 << QStringLiteral("Alpha");
  QTest::newRow("backward-title") << insertion << 8 << 3 << 11 << 6 << QStringLiteral("Alpha");
  QTest::newRow("upper-endpoint-at-insertion")
      << insertion << 0 << 12 << 0 << 15 << QStringLiteral("## 1. Alpha\u2029## ");
  QTest::newRow("backward-upper-endpoint-at-insertion")
      << insertion << 12 << 0 << 15 << 0 << QStringLiteral("## 1. Alpha\u2029## ");
  QTest::newRow("both-insertion-boundaries")
      << insertion << 3 << 12 << 6 << 15 << QStringLiteral("Alpha\u2029## ");
  QTest::newRow("caret-at-later-insertion") << insertion << 12 << 12 << 18 << 18 << QString();
  const QString replacement = QStringLiteral("## 123456. Alpha\n## 42) Beta\n");
  QTest::newRow("inside-prefix-retains-offset") << replacement << 5 << 5 << 5 << 5 << QString();
  QTest::newRow("inside-prefix-clamps-to-end") << replacement << 10 << 10 << 6 << 6 << QString();
  QTest::newRow("selection-inside-prefix-clamps")
      << replacement << 10 << 4 << 6 << 4 << QStringLiteral(". ");
  QTest::newRow("caret-after-replaced-prefix") << replacement << 16 << 16 << 11 << 11 << QString();
}

void TestMarkdownEditor::testHeadingSourceCursorAndSelection() {
  QFETCH(QString, source);
  QFETCH(int, anchor);
  QFETCH(int, position);
  QFETCH(int, mappedAnchor);
  QFETCH(int, mappedPosition);
  QFETCH(QString, selected);
  Fixture fixture(source);
  auto editor = fixture.editor();
  selectHeadingSource(*editor, anchor, position);
  editor->setHeadingSectionNumberProvider(
      headingPrefixes({QStringLiteral("1."), QStringLiteral("2.")}));
  editor->setHeadingSectionNumberingActive(true);
  QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), QStringLiteral("## 1. Alpha\n## 2. Beta\n"), 5000);
  const auto cursor = fixture.edit()->textCursor();
  QCOMPARE(cursor.anchor(), mappedAnchor);
  QCOMPARE(cursor.position(), mappedPosition);
  QCOMPARE(cursor.selectedText(), selected);
  QCOMPARE(fixture.edit()->selectedText(), selected);
}

void TestMarkdownEditor::testHeadingSourceOverriddenSelectionAndScroll() {
  {
    Fixture fixture(QStringLiteral("## Alpha\n## Beta\n"), 0, 2);
    auto editor = fixture.editor();
    fixture.edit()->setOverriddenSelection(3, 12);
    QVERIFY(!fixture.edit()->textCursor().hasSelection());
    editor->setHeadingSectionNumberProvider(
        headingPrefixes({QStringLiteral("1."), QStringLiteral("2.")}));
    editor->setHeadingSectionNumberingActive(true);
    QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), QStringLiteral("## 1. Alpha\n## 2. Beta\n"), 5000);
    QCOMPARE(fixture.edit()->textCursor().position(), 2);
    QVERIFY(!fixture.edit()->textCursor().hasSelection());
    QCOMPARE(fixture.edit()->getSelection().start(), 6);
    QCOMPARE(fixture.edit()->getSelection().end(), 15);
    QCOMPARE(fixture.edit()->selectedText(), QStringLiteral("Alpha\u2029## "));
  }
  {
    Fixture fixture(QStringLiteral("## Alpha\n## Beta\n"));
    auto editor = fixture.editor();
    selectHeadingSource(*editor, 8, 3);
    fixture.edit()->setOverriddenSelection(12, 16);
    editor->setHeadingSectionNumberProvider(
        headingPrefixes({QStringLiteral("1."), QStringLiteral("2.")}));
    editor->setHeadingSectionNumberingActive(true);
    QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), QStringLiteral("## 1. Alpha\n## 2. Beta\n"), 5000);
    QCOMPARE(fixture.edit()->textCursor().anchor(), 11);
    QCOMPARE(fixture.edit()->textCursor().position(), 6);
    QCOMPARE(fixture.edit()->textCursor().selectedText(), QStringLiteral("Alpha"));
    QCOMPARE(fixture.edit()->getSelection().start(), 18);
    QCOMPARE(fixture.edit()->getSelection().end(), 22);
    QCOMPARE(fixture.edit()->selectedText(), QStringLiteral("Beta"));
  }
  const QString filler =
      QStringLiteral("long unchanged paragraph ").repeated(30) + QLatin1Char('\n');
  const QString source = QStringLiteral("## Alpha\n") + filler.repeated(80);
  Fixture fixture(source, 0, 3);
  auto editor = fixture.editor();
  auto edit = fixture.edit();
  editor->resize(480, 240);
  edit->setLineWrapMode(QTextEdit::NoWrap);
  editor->show();
  QVERIFY(QTest::qWaitForWindowExposed(editor));
  HeadingSnapshot snapshot(*editor);
  QSignalSpy completed(editor->getHighlighter(), &MarkdownHighlighter::highlightCompleted);
  editor->getHighlighter()->updateHighlight();
  QTRY_COMPARE_WITH_TIMEOUT(snapshot.m_headings.size(), 1, 5000);
  QTRY_VERIFY_WITH_TIMEOUT(!completed.isEmpty(), 5000);
  completed.clear();
  QTRY_VERIFY_WITH_TIMEOUT(edit->verticalScrollBar()->maximum() > 100, 5000);
  QTRY_VERIFY_WITH_TIMEOUT(edit->horizontalScrollBar()->maximum() > 100, 5000);
  edit->verticalScrollBar()->setValue(100);
  edit->horizontalScrollBar()->setValue(100);
  editor->setHeadingSectionNumberProvider(headingPrefixes({QStringLiteral("1.")}));
  editor->setHeadingSectionNumberingActive(true);
  QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), QStringLiteral("## 1. Alpha\n") + filler.repeated(80),
                            5000);
  QCOMPARE(edit->verticalScrollBar()->value(), 100);
  QCOMPARE(edit->horizontalScrollBar()->value(), 100);
  QCOMPARE(edit->textCursor().position(), 6);
  QTRY_VERIFY_WITH_TIMEOUT(!completed.isEmpty(), 5000);
  QCOMPARE(edit->verticalScrollBar()->value(), 100);
  QCOMPARE(edit->horizontalScrollBar()->value(), 100);
  for (int pass = 0; pass < 2; ++pass) {
    completed.clear();
    editor->getHighlighter()->updateHighlight();
    QTRY_VERIFY_WITH_TIMEOUT(!completed.isEmpty(), 5000);
    QCOMPARE(edit->verticalScrollBar()->value(), 100);
    QCOMPARE(edit->horizontalScrollBar()->value(), 100);
    QCOMPARE(edit->textCursor().position(), 6);
  }

  // User navigation owns the viewport again; later highlighting must not
  // restore the numbering pass's old scroll position.
  QTest::keyClick(edit, Qt::Key_End, Qt::ControlModifier);
  QCOMPARE(edit->textCursor().position(), editor->document()->characterCount() - 1);
  QVERIFY(edit->viewport()->rect().contains(edit->cursorRect().center()));
  const int navigationScroll = edit->verticalScrollBar()->value();
  QVERIFY(navigationScroll > 100);
  completed.clear();
  editor->getHighlighter()->updateHighlight();
  QTRY_VERIFY_WITH_TIMEOUT(!completed.isEmpty(), 5000);
  QCOMPARE(edit->verticalScrollBar()->value(), navigationScroll);
}

void TestMarkdownEditor::testHeadingSourceInputMethodDeferral() {
  const QString source = QStringLiteral("## Alpha\n\nprose\n");
  Fixture fixture(source, 2);
  auto editor = fixture.editor();
  auto edit = fixture.edit();
  editor->resize(640, 480);
  editor->show();
  editor->activateWindow();
  edit->setFocus();
  QTRY_VERIFY_WITH_TIMEOUT(edit->hasFocus(), 5000);
  HeadingSnapshot snapshot(*editor);
  editor->setHeadingSectionNumberProvider(headingPrefixes({QStringLiteral("1.")}));
  editor->setHeadingSectionNumberingActive(true);
  QInputMethodEvent preedit(QStringLiteral("\u3042"), QList<QInputMethodEvent::Attribute>());
  QCoreApplication::sendEvent(edit, &preedit);
  auto block = editor->document()->findBlockByNumber(2);
  QVERIFY(block.layout());
  QCOMPARE(block.layout()->preeditAreaText(), QStringLiteral("\u3042"));
  const int position = edit->textCursor().position();
  QTest::qWait(800);
  QCOMPARE(fixture.text(), source);
  QCOMPARE(block.layout()->preeditAreaText(), QStringLiteral("\u3042"));
  QCOMPARE(edit->textCursor().position(), position);
  QVERIFY(!snapshot.m_numbered);
  // End composition without a source edit: the input-method event itself must
  // release the owed pass, including when preedit was not in a heading block.
  QInputMethodEvent cancel;
  QCoreApplication::sendEvent(edit, &cancel);
  QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), QStringLiteral("## 1. Alpha\n\nprose\n"), 5000);
  QCOMPARE(editor->document()->findBlockByNumber(2).layout()->preeditAreaText(), QString());
  QCOMPARE(edit->textCursor().position(), position + 3);
}

void TestMarkdownEditor::testHeadingSourcePreviewFocusDeferral() {
  auto config = makeConfig();
  config->m_inplacePreviewSources |= MarkdownEditorConfig::Table;
  VMarkdownEditor editor(config, QSharedPointer<TextEditorParameters>::create());
  editor.resize(640, 480);
  editor.show();
  editor.activateWindow();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  const QString source = QStringLiteral("## Alpha\n\n") + c_tableSource;
  editor.setText(source);
  auto edit = editor.getTextEdit();
  PreviewWidget *preview = nullptr;
  QTRY_VERIFY_WITH_TIMEOUT((preview = edit->viewport()->findChild<PreviewWidget *>()) != nullptr,
                           5000);
  auto sheet = preview->findChild<QTextEdit *>();
  QVERIFY(sheet);
  sheet->setFocus();
  QTRY_VERIFY_WITH_TIMEOUT(sheet->hasFocus(), 5000);
  QVERIFY(edit->isViewportWidgetFocused());
  editor.setHeadingSectionNumberProvider(headingPrefixes({QStringLiteral("1.")}));
  editor.setHeadingSectionNumberingActive(true);
  QTest::qWait(800);
  QCOMPARE(editor.document()->toPlainText(), source);
  QVERIFY(sheet->hasFocus());
  edit->setFocus();
  QTRY_VERIFY_WITH_TIMEOUT(edit->hasFocus(), 5000);
  QTRY_COMPARE_WITH_TIMEOUT(editor.document()->toPlainText(),
                            QStringLiteral("## 1. Alpha\n\n") + c_tableSource, 5000);
}

void TestMarkdownEditor::testHeadingSourceLayoutDeferral() {
  Fixture fixture(QStringLiteral("## Alpha\n"));
  auto editor = fixture.editor();
  auto doc = editor->document();
  HeadingSnapshot snapshot(*editor);
  QTRY_COMPARE_WITH_TIMEOUT(snapshot.m_headings.size(), 1, 5000);
  editor->setHeadingSectionNumberProvider(headingPrefixes({QStringLiteral("1.")}));
  QTest::qWait(800);
  bool entered = false;
  bool changedDuringLayout = false;
  // Use a real Qt layout notification and a bounded nested event loop. The
  // inline isBusy() observer needs no private implementation linked/exported.
  const auto layoutNotification = connect(
      doc->documentLayout(), &QAbstractTextDocumentLayout::update, editor, [&](const QRectF &) {
        if (entered || !editor->documentLayout()->isBusy()) {
          return;
        }
        entered = true;
        editor->setHeadingSectionNumberingActive(true);
        QTest::qWait(650);
        changedDuringLayout = fixture.text() != QStringLiteral("## Alpha\n");
      });
  doc->markContentsDirty(0, doc->characterCount());
  QTRY_VERIFY_WITH_TIMEOUT(entered, 5000);
  QVERIFY(!changedDuringLayout);
  disconnect(layoutNotification);
  QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), QStringLiteral("## 1. Alpha\n"), 5000);
  QTRY_VERIFY_WITH_TIMEOUT(snapshot.m_numbered, 5000);
}

void TestMarkdownEditor::testHeadingSourceGuaranteeReset() {
  Fixture fixture(QStringLiteral("## Alpha\n"));
  auto editor = fixture.editor();
  HeadingSnapshot snapshot(*editor);
  editor->setHeadingSectionNumberProvider(headingPrefixes({QStringLiteral("1.")}));
  editor->setHeadingSectionNumberingActive(true);
  QTRY_VERIFY_WITH_TIMEOUT(snapshot.m_numbered, 5000);
  QCOMPARE(snapshot.titles(), QStringList{QStringLiteral("1. Alpha")});
  editor->setHeadingSectionNumberingActive(false);
  editor->setText(QString());
  QTRY_VERIFY_WITH_TIMEOUT(snapshot.m_headings.isEmpty(), 5000);
  QVERIFY(!snapshot.m_numbered);
  editor->setText(QStringLiteral("# Title\n"));
  editor->setHeadingSectionNumberProvider(headingPrefixes({QString()}));
  QTRY_COMPARE_WITH_TIMEOUT(snapshot.titles(), QStringList{QStringLiteral("Title")}, 5000);
  QVERIFY(!snapshot.m_numbered);
  editor->setText(QStringLiteral("## 1. Alpha\n"));
  editor->setHeadingSectionNumberProvider(headingPrefixes({QStringLiteral("1.")}));
  QTRY_VERIFY_WITH_TIMEOUT(snapshot.m_numbered, 5000);
  // The guarantee describes actual current source, not whether edits are active.
  QCOMPARE(snapshot.m_source, QStringLiteral("## 1. Alpha\n"));
  editor->setHeadingSectionNumberProvider({});
  QTRY_VERIFY_WITH_TIMEOUT(!snapshot.m_numbered, 5000);
  QCOMPARE(snapshot.titles(), QStringList{QStringLiteral("1. Alpha")});
  editor->setHeadingSectionNumberProvider(headingPrefixes({QString()}));
  QTest::qWait(800);
  QVERIFY(!snapshot.m_numbered);
  QCOMPARE(fixture.text(), QStringLiteral("## 1. Alpha\n"));
}

void TestMarkdownEditor::testHeadingSourceTableCoexistence() {
  VMarkdownEditor editor(makeTableSourceConfig(), QSharedPointer<TextEditorParameters>::create());
  const QString source = QStringLiteral("## Alpha\n\n") + c_tableSource;
  editor.setText(source);
  HeadingSnapshot snapshot(editor);
  QTRY_COMPARE_WITH_TIMEOUT(snapshot.m_headings.size(), 1, 5000);
  editor.setHeadingSectionNumberProvider(headingPrefixes({QStringLiteral("1.")}));
  editor.setHeadingSectionNumberingActive(true);
  replaceTableSource(editor, 4, 2, 1, QStringLiteral("z"));
  selectTableSource(editor, 4, 3, 4, 3);
  const QString expected = QStringLiteral("## 1. Alpha\n\n") + c_tableSourceAligned;
  QTRY_COMPARE_WITH_TIMEOUT(editor.document()->toPlainText(), expected, 5000);
  QTRY_VERIFY_WITH_TIMEOUT(snapshot.m_numbered, 5000);
  QTest::qWait(250);
  const int revision = editor.document()->revision();
  const int undoSteps = editor.document()->availableUndoSteps();
  const auto cursor = editor.getTextEdit()->textCursor();
  QTest::qWait(1000);
  QCOMPARE(editor.document()->toPlainText(), expected);
  QCOMPARE(editor.document()->revision(), revision);
  QCOMPARE(editor.document()->availableUndoSteps(), undoSteps);
  QCOMPARE(editor.getTextEdit()->textCursor().position(), cursor.position());
  QCOMPARE(editor.getTextEdit()->textCursor().anchor(), cursor.anchor());
}

QTEST_MAIN(tests::TestMarkdownEditor)
