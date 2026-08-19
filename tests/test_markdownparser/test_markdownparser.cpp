#include "test_markdownparser.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QPaintEvent>
#include <QRegion>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextLayout>
#include <algorithm>

#include <vtextedit/htmlimgscanner.h>
#include <vtextedit/markdowneditorconfig.h>
#include <vtextedit/markdownhighlighter.h>
#include <vtextedit/markdownutils.h>
#include <vtextedit/texteditorconfig.h>
#include <vtextedit/theme.h>
#include <vtextedit/vmarkdowneditor.h>
#include <vtextedit/vtextedit.h>

#include "cmarkadapter.h"
#include "markdownastwalker.h"
#include "markdownparser.h"

using namespace tests;
using vte::MarkdownLink;
using vte::MarkdownUtils;

// Highlight type ordinals (matching pmh_element_type / MarkdownSyntaxStyle values
// used by the cmark adapter).
enum {
  HLT_LINK = 0,
  HLT_AUTO_LINK_URL = 1,
  HLT_AUTO_LINK_EMAIL = 2,
  HLT_IMAGE = 3,
  HLT_CODE = 4,
  HLT_HTML = 5,
  HLT_HTML_ENTITY = 6,
  HLT_EMPH = 7,
  HLT_STRONG = 8,
  HLT_LIST_BULLET = 9,
  HLT_LIST_ENUMERATOR = 10,
  HLT_COMMENT = 11,
  HLT_H1 = 12,
  HLT_H2 = 13,
  HLT_H3 = 14,
  HLT_H4 = 15,
  HLT_H5 = 16,
  HLT_H6 = 17,
  HLT_BLOCKQUOTE = 18,
  HLT_VERBATIM = 19,
  HLT_HTMLBLOCK = 20,
  HLT_HRULE = 21,
  HLT_REFERENCE = 22,
  HLT_FENCEDCODEBLOCK = 23,
  HLT_NOTE = 24,
  HLT_STRIKE = 25,
  HLT_FRONTMATTER = 26,
  HLT_DISPLAYFORMULA = 27,
  HLT_INLINEEQUATION = 28,
  HLT_MARK = 29,
  HLT_TABLE = 30,
  HLT_TABLEHEADER = 31,
  HLT_TABLEBORDER = 32
};

// Helper: count blocks (newlines + 1) in UTF-8 text.
static int countBlocks(const QByteArray &p_utf8) {
  int n = 1;
  for (int i = 0; i < p_utf8.size(); ++i) {
    if (p_utf8[i] == '\n')
      ++n;
  }
  return n;
}

// Helper: parse text and return ASTWalkResult.
static vte::md::ASTWalkResult parse(const QString &p_text) {
  QByteArray utf8 = p_text.toUtf8();
  int numBlocks = countBlocks(utf8);
  return vte::md::walkAndConvert(utf8, numBlocks);
}

// Helper: count HLUnits with given style across all blocks.
static int countElements(const vte::md::ASTWalkResult &p_result, int p_style) {
  int count = 0;
  for (const auto &block : p_result.blocksHighlights) {
    for (const auto &unit : block) {
      if (unit.styleIndex == (unsigned int)p_style)
        ++count;
    }
  }
  return count;
}

// Helper: find all HLUnits with given style, returning doc-absolute (start, end) pairs.
// Elements are returned in the order they appear in blocksHighlights (block order, then
// unit order within block). The old parseCmark prepended elements (reverse order within
// a style bucket). We sort by position ascending here for consistent comparison.
static QVector<QPair<unsigned long, unsigned long>>
findElements(const vte::md::ASTWalkResult &p_result, int p_style, const QString &p_text) {
  QVector<QPair<unsigned long, unsigned long>> elems;
  // Build block start positions from text.
  QVector<int> blockStarts;
  blockStarts.append(0);
  for (int i = 0; i < p_text.size(); ++i) {
    if (p_text[i] == '\n')
      blockStarts.append(i + 1);
  }
  for (int blockNum = 0; blockNum < p_result.blocksHighlights.size(); ++blockNum) {
    for (const auto &unit : p_result.blocksHighlights[blockNum]) {
      if (unit.styleIndex == (unsigned int)p_style) {
        unsigned long absStart =
            (blockNum < blockStarts.size() ? blockStarts[blockNum] : 0) + unit.start;
        unsigned long absEnd = absStart + unit.length;
        elems.append(qMakePair(absStart, absEnd));
      }
    }
  }
  return elems;
}

static QString readFixture(const QString &p_name) {
  QFile f(QStringLiteral(FIXTURES_DIR) + "/" + p_name);
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return QString();
  }
  return QString::fromUtf8(f.readAll());
}

static QTextCharFormat formatAt(const QTextBlock &p_block, int p_position) {
  const auto ranges = p_block.layout()->formats();
  for (const auto &range : ranges) {
    if (range.start <= p_position && p_position < range.start + range.length) {
      return range.format;
    }
  }

  return QTextCharFormat();
}

void TestMarkdownParser::initTestCase() {}

void TestMarkdownParser::cleanupTestCase() {}

// ============================================================
// T5: Block Element Tests
// ============================================================

void TestMarkdownParser::testHeadings() {
  const QString input = QStringLiteral("# H1\n## H2\n### H3\n#### H4\n##### H5\n###### H6\n");
  auto result = parse(input);

  QCOMPARE(countElements(result, HLT_H1), 1);
  QCOMPARE(countElements(result, HLT_H2), 1);
  QCOMPARE(countElements(result, HLT_H3), 1);
  QCOMPARE(countElements(result, HLT_H4), 1);
  QCOMPARE(countElements(result, HLT_H5), 1);
  QCOMPARE(countElements(result, HLT_H6), 1);

  // cmark heading positions exclude trailing newline (unlike pmh).
  // Use headerRegions for doc-absolute position checks.
  QCOMPARE(result.headerRegions.size(), 6);

  // headerRegions are sorted by position.
  QCOMPARE(result.headerRegions[0].m_startPos, 0);
  QCOMPARE(result.headerRegions[0].m_endPos, 4);

  QCOMPARE(result.headerRegions[1].m_startPos, 5);
  QCOMPARE(result.headerRegions[1].m_endPos, 10);

  QCOMPARE(result.headerRegions[2].m_startPos, 11);
  QCOMPARE(result.headerRegions[2].m_endPos, 17);

  QCOMPARE(result.headerRegions[3].m_startPos, 18);
  QCOMPARE(result.headerRegions[3].m_endPos, 25);

  QCOMPARE(result.headerRegions[4].m_startPos, 26);
  QCOMPARE(result.headerRegions[4].m_endPos, 34);

  QCOMPARE(result.headerRegions[5].m_startPos, 35);
  QCOMPARE(result.headerRegions[5].m_endPos, 44);
}

void TestMarkdownParser::testBlockquotes() {
  const QString input = QStringLiteral("> quoted text\n> more quoted\n");
  auto result = parse(input);

  // Walker produces 1 HLUnit per block line for blockquote (2 lines = 2 units).
  QCOMPARE(countElements(result, HLT_BLOCKQUOTE), 2);

  auto elems = findElements(result, HLT_BLOCKQUOTE, input);
  QVERIFY(!elems.isEmpty());
  // Sort by position — first element starts at 0.
  std::sort(elems.begin(), elems.end());
  QCOMPARE((int)elems[0].first, 0);
  QVERIFY((int)elems[0].second > 0);
}

void TestMarkdownParser::testBlockquoteNestingDepth() {
  // Pins the contract the Enter continuation relies on: the number of
  // STYLE_BLOCKQUOTE units on a block equals its quote nesting depth.
  const QString input = QStringLiteral("> a\n\n> > b\n");
  auto result = parse(input);

  auto quoteUnits = [&result](int p_blockNumber) {
    int count = 0;
    for (const auto &unit : result.blocksHighlights.at(p_blockNumber)) {
      if ((int)unit.styleIndex == HLT_BLOCKQUOTE) {
        ++count;
      }
    }
    return count;
  };

  QVERIFY(result.blocksHighlights.size() >= 3);
  QCOMPARE(quoteUnits(0), 1);
  QCOMPARE(quoteUnits(2), 2);
}

void TestMarkdownParser::testHorizontalRules() {
  // Use *** instead of --- to avoid cmark frontmatter extension consuming it.
  const QString input = QStringLiteral("***\n");
  auto result = parse(input);

  QCOMPARE(countElements(result, HLT_HRULE), 1);

  QCOMPARE(result.hruleRegions.size(), 1);
  QCOMPARE(result.hruleRegions[0].m_startPos, 0);
  QCOMPARE(result.hruleRegions[0].m_endPos, 3);
}

void TestMarkdownParser::testFencedCodeBlocks() {
  const QString input = QStringLiteral("```cpp\ncode here\n```\n");
  auto result = parse(input);

  // Walker produces 1 HLUnit per block line (3 lines = 3 units). Use region for logical count.
  QCOMPARE(countElements(result, HLT_FENCEDCODEBLOCK), 3);

  QCOMPARE(result.codeBlockRegions.size(), 1);
  auto it = result.codeBlockRegions.constBegin();
  QCOMPARE(it.value().m_startPos, 0);
  QCOMPARE(it.value().m_endPos, 20);

  const QString nestedInput =
      QStringLiteral("1. Nested code block\n\n    ```cpp\n    code here\n    ```\n2. List item\n");
  auto nestedResult = parse(nestedInput);

  QCOMPARE(nestedResult.codeBlockRegions.size(), 1);
  const auto &openingFenceHighlights = nestedResult.blocksHighlights.at(2);
  const auto openingFence = std::find_if(
      openingFenceHighlights.cbegin(), openingFenceHighlights.cend(),
      [](const vte::md::HLUnit &p_unit) { return p_unit.styleIndex == HLT_FENCEDCODEBLOCK; });
  QVERIFY(openingFence != openingFenceHighlights.cend());
  // The list indentation is intentionally outside the AST range and must be formatted by the
  // highlighter's second pass.
  QCOMPARE(openingFence->start, 4UL);
}

void TestMarkdownParser::testFencedCodeBlockIndentationFormat() {
  const QString themeJson = QStringLiteral(R"({
    "metadata": {"type": "vtextedit", "name": "FencedCodeTest"},
    "editor": {"font-family": "Arial", "font-size": 11},
    "markdown-syntax-styles": {
      "FENCEDCODEBLOCK": {
        "font-family": "Courier New",
        "font-size": 19,
        "background-color": "#123456"
      }
    }
  })");
  auto theme = vte::Theme::createThemeFromContent(themeJson);
  QVERIFY(!theme.isNull());

  auto textConfig = QSharedPointer<vte::TextEditorConfig>::create();
  textConfig->m_theme = theme;
  auto markdownConfig = QSharedPointer<vte::MarkdownEditorConfig>::create(textConfig);
  markdownConfig->m_inplacePreviewSources = vte::MarkdownEditorConfig::NoInplacePreview;
  auto parameters = QSharedPointer<vte::TextEditorParameters>::create();
  vte::VMarkdownEditor editor(markdownConfig, parameters);
  auto highlighter = editor.getHighlighter();
  QVERIFY(highlighter);

  QSignalSpy completed(highlighter, &vte::MarkdownHighlighter::highlightCompleted);
  editor.setText(
      QStringLiteral("1. Nested code block\n\n    ```cpp\n    code here\n    ```\n2. List item\n"));
  completed.clear();
  highlighter->updateHighlight();
  QTRY_VERIFY(completed.count() > 0);

  const QTextBlock openingFence = editor.document()->findBlockByNumber(2);
  QCOMPARE(openingFence.userState(),
           static_cast<int>(vte::md::HighlightBlockState::CodeBlockStart));
  const auto expected = highlighter->codeBlockStyle();
  const auto indentationFormat = formatAt(openingFence, 0);
  const auto lastIndentationFormat = formatAt(openingFence, 3);
  const auto fenceFormat = formatAt(openingFence, 4);
  QCOMPARE(indentationFormat.fontPointSize(), expected.fontPointSize());
  QCOMPARE(indentationFormat.background().color(), expected.background().color());
  QCOMPARE(lastIndentationFormat.fontPointSize(), expected.fontPointSize());
  QCOMPARE(lastIndentationFormat.background().color(), expected.background().color());
  QCOMPARE(fenceFormat.fontPointSize(), expected.fontPointSize());
  QCOMPARE(fenceFormat.background().color(), expected.background().color());

  const QTextBlock nextListItem = editor.document()->findBlockByNumber(5);
  QVERIFY(formatAt(nextListItem, 0).background().color() != expected.background().color());
}

void TestMarkdownParser::testIndentedCodeBlocks() {
  const QString input = QStringLiteral("    indented code\n");
  auto result = parse(input);

  QCOMPARE(countElements(result, HLT_VERBATIM), 1);

  auto elems = findElements(result, HLT_VERBATIM, input);
  QVERIFY(!elems.isEmpty());
  // cmark starts indented code block position at content (after indent).
  QCOMPARE((int)elems[0].first, 4);
  QVERIFY((int)elems[0].second > 4);
}

void TestMarkdownParser::testHTMLBlocks() {
  const QString input = QStringLiteral("<div>\nhtml content\n</div>\n");
  auto result = parse(input);

  // Walker produces 1 HLUnit per block line (3 lines = 3 units).
  QCOMPARE(countElements(result, HLT_HTMLBLOCK), 3);

  auto elems = findElements(result, HLT_HTMLBLOCK, input);
  QVERIFY(!elems.isEmpty());
  QCOMPARE((int)elems[0].first, 0);
  QVERIFY((int)elems[0].second > 0);
}

void TestMarkdownParser::testLists() {
  const QString input = QStringLiteral("- item 1\n- item 2\n\n1. first\n2. second\n");
  auto result = parse(input);

  QCOMPARE(countElements(result, HLT_LIST_BULLET), 2);
  QCOMPARE(countElements(result, HLT_LIST_ENUMERATOR), 2);

  // Find bullet elements sorted by position.
  auto bullets = findElements(result, HLT_LIST_BULLET, input);
  QCOMPARE(bullets.size(), 2);
  // Sort by position ascending.
  std::sort(bullets.begin(), bullets.end());
  QCOMPARE((int)bullets[0].first, 0);
  QCOMPARE((int)bullets[0].second, 1);
  QCOMPARE((int)bullets[1].first, 9);
  QCOMPARE((int)bullets[1].second, 10);

  // Find enumerator elements sorted by position.
  auto enums = findElements(result, HLT_LIST_ENUMERATOR, input);
  QCOMPARE(enums.size(), 2);
  std::sort(enums.begin(), enums.end());
  QCOMPARE((int)enums[0].first, 19);
  QCOMPARE((int)enums[0].second, 21);
  QCOMPARE((int)enums[1].first, 28);
  QCOMPARE((int)enums[1].second, 30);
}

void TestMarkdownParser::testFrontmatter() {
  const QString input = QStringLiteral("---\ntitle: test\n---\n\nContent\n");
  auto result = parse(input);

  // Walker produces 1 HLUnit per block line (3 lines = 3 units).
  QCOMPARE(countElements(result, HLT_FRONTMATTER), 3);

  auto elems = findElements(result, HLT_FRONTMATTER, input);
  QVERIFY(!elems.isEmpty());
  QCOMPARE((int)elems[0].first, 0);
  QVERIFY((int)elems[0].second > 0);
}

void TestMarkdownParser::testDisplayFormula() {
  const QString input = QStringLiteral("$$\nE = mc^2\n$$\n");
  auto result = parse(input);

  // Walker produces 1 HLUnit per block line (3 lines = 3 units). Use region for logical count.
  QCOMPARE(countElements(result, HLT_DISPLAYFORMULA), 3);

  QCOMPARE(result.displayFormulaRegions.size(), 1);
  QCOMPARE(result.displayFormulaRegions[0].m_startPos, 0);
  QVERIFY(result.displayFormulaRegions[0].m_endPos > 0);
}

void TestMarkdownParser::testTables() {
  const QString input = QStringLiteral("| h1 | h2 |\n|---|---|\n| a | b |\n");
  auto result = parse(input);

  // Walker produces 1 HLUnit per block line for TABLE (3 lines = 3 units).
  // Use region count for logical element count.
  QCOMPARE(countElements(result, HLT_TABLE), 3);
  // TABLEHEADER is only the header row (1 line = 1 unit).
  QCOMPARE(countElements(result, HLT_TABLEHEADER), 1);

  QVERIFY(!result.tableRegions.isEmpty());
  QCOMPARE(result.tableRegions[0].m_startPos, 0);
  QVERIFY(result.tableRegions[0].m_endPos > 0);

  QVERIFY(!result.tableHeaderRegions.isEmpty());
  QCOMPARE(result.tableHeaderRegions[0].m_startPos, 0);
  QVERIFY(result.tableHeaderRegions[0].m_endPos > 0);
}

// ============================================================
// T6: Inline Element Tests
// ============================================================

void TestMarkdownParser::testEmphasis() {
  const QString input = QStringLiteral("*emph*\n");
  auto result = parse(input);

  QCOMPARE(countElements(result, HLT_EMPH), 1);

  auto elems = findElements(result, HLT_EMPH, input);
  QVERIFY(!elems.isEmpty());
  QCOMPARE((int)elems[0].first, 0);
  QCOMPARE((int)elems[0].second, 6);
}

void TestMarkdownParser::testStrong() {
  const QString input = QStringLiteral("**strong**\n");
  auto result = parse(input);

  QCOMPARE(countElements(result, HLT_STRONG), 1);

  auto elems = findElements(result, HLT_STRONG, input);
  QVERIFY(!elems.isEmpty());
  QCOMPARE((int)elems[0].first, 0);
  QCOMPARE((int)elems[0].second, 10);
}

void TestMarkdownParser::testInlineCode() {
  const QString input = QStringLiteral("`code`\n");
  auto result = parse(input);

  QCOMPARE(countElements(result, HLT_CODE), 1);

  auto elems = findElements(result, HLT_CODE, input);
  QVERIFY(!elems.isEmpty());
  QCOMPARE((int)elems[0].first, 0);
  QCOMPARE((int)elems[0].second, 6);
}

void TestMarkdownParser::testLinks() {
  const QString input = QStringLiteral("[text](url)\n");
  auto result = parse(input);

  QCOMPARE(countElements(result, HLT_LINK), 1);

  auto elems = findElements(result, HLT_LINK, input);
  QVERIFY(!elems.isEmpty());
  QCOMPARE((int)elems[0].first, 0);
  QCOMPARE((int)elems[0].second, 11);
}

void TestMarkdownParser::testAutoLinks() {
  // URL auto link — cmark maps to LINK (not AUTO_LINK_URL). Count = 1.
  {
    const QString input = QStringLiteral("<http://example.com>\n");
    auto result = parse(input);

    QCOMPARE(countElements(result, HLT_AUTO_LINK_URL), 0);
    QCOMPARE(countElements(result, HLT_LINK), 1);

    auto elems = findElements(result, HLT_LINK, input);
    QVERIFY(!elems.isEmpty());
    QCOMPARE((int)elems[0].first, 0);
    QCOMPARE((int)elems[0].second, 20);
  }

  // Email auto link — cmark produces 1 AUTO_LINK_EMAIL (pmh produced 2 duplicates).
  {
    const QString input = QStringLiteral("<user@example.com>\n");
    auto result = parse(input);

    QCOMPARE(countElements(result, HLT_AUTO_LINK_EMAIL), 1);

    auto elems = findElements(result, HLT_AUTO_LINK_EMAIL, input);
    QVERIFY(!elems.isEmpty());
    QCOMPARE((int)elems[0].first, 0);
    QCOMPARE((int)elems[0].second, 18);
  }
}

void TestMarkdownParser::testImages() {
  const QString input = QStringLiteral("![alt](img.png)\n");
  auto result = parse(input);

  QCOMPARE(countElements(result, HLT_IMAGE), 1);

  QCOMPARE(result.imageRegions.size(), 1);
  QCOMPARE(result.imageRegions[0].m_startPos, 0);
  QCOMPARE(result.imageRegions[0].m_endPos, 15);
}

void TestMarkdownParser::testHTMLInline() {
  const QString input = QStringLiteral("text <span>html</span> text\n");
  auto result = parse(input);

  // cmark produces 2 HTML_INLINE elements for <span> and </span>.
  QCOMPARE(countElements(result, HLT_HTML), 2);

  // Find elements and sort by position.
  auto elems = findElements(result, HLT_HTML, input);
  QCOMPARE(elems.size(), 2);
  std::sort(elems.begin(), elems.end());

  // <span> at positions 5-11, </span> at positions 15-22.
  QCOMPARE((int)elems[0].first, 5);
  QCOMPARE((int)elems[0].second, 11);
  QCOMPARE((int)elems[1].first, 15);
  QCOMPARE((int)elems[1].second, 22);
}

void TestMarkdownParser::testHTMLEntities() {
  const QString input = QStringLiteral("&amp; &lt;\n");
  auto result = parse(input);

  // cmark does not produce HTML_ENTITY elements.
  QCOMPARE(countElements(result, HLT_HTML_ENTITY), 0);
}

void TestMarkdownParser::testComments() {
  const QString input = QStringLiteral("<!-- comment -->\n");
  auto result = parse(input);

  // cmark maps HTML comments to HTMLBLOCK, not COMMENT.
  QCOMPARE(countElements(result, HLT_COMMENT), 0);
  QCOMPARE(countElements(result, HLT_HTMLBLOCK), 1);
}

void TestMarkdownParser::testReferences() {
  const QString input = QStringLiteral("[id]: http://example.com\n\n[text][id]\n");
  auto result = parse(input);

  // cmark resolves references during parsing — no REFERENCE elements.
  QCOMPARE(countElements(result, HLT_REFERENCE), 0);
  // The link reference [text][id] should produce a LINK element.
  QCOMPARE(countElements(result, HLT_LINK), 1);
}

void TestMarkdownParser::testStrikethrough() {
  const QString input = QStringLiteral("~~strike~~\n");
  auto result = parse(input);

  QCOMPARE(countElements(result, HLT_STRIKE), 1);

  auto elems = findElements(result, HLT_STRIKE, input);
  QVERIFY(!elems.isEmpty());
  QCOMPARE((int)elems[0].first, 0);
  QCOMPARE((int)elems[0].second, 10);
}

void TestMarkdownParser::testMark() {
  const QString input = QStringLiteral("==marked==\n");
  auto result = parse(input);

  // cmark produces MARK elements (pmh did not).
  int markCount = countElements(result, HLT_MARK);
  qDebug() << "MARK count:" << markCount;
  QVERIFY(markCount >= 1);

  auto elems = findElements(result, HLT_MARK, input);
  QVERIFY(!elems.isEmpty());
  QCOMPARE((int)elems[0].first, 0);
  QCOMPARE((int)elems[0].second, 10);
}

void TestMarkdownParser::testFootnotes() {
  const QString input = QStringLiteral("[^1]: footnote\n\nText [^1]\n");
  auto result = parse(input);

  // Walker produces HLUnits for footnote definition and reference.
  // "[^1]: footnote\n" is 1 block, "\n" is empty, "Text [^1]\n" has 1 reference.
  // The walker may produce more units depending on how footnote nodes are structured.
  QVERIFY(countElements(result, HLT_NOTE) >= 2);
}

void TestMarkdownParser::testInlineEquation() {
  const QString input = QStringLiteral("$E=mc^2$\n");
  auto result = parse(input);

  QCOMPARE(countElements(result, HLT_INLINEEQUATION), 1);

  QCOMPARE(result.inlineEquationRegions.size(), 1);
  // cmark adapter adjusts FORMULA_INLINE to re-include $ delimiters.
  QCOMPARE(result.inlineEquationRegions[0].m_startPos, 0);
  QCOMPARE(result.inlineEquationRegions[0].m_endPos, 8);
}

// ============================================================
// T7: Edge Case Tests
// ============================================================

void TestMarkdownParser::testSurrogatePairs() {
  // U+1F389 is 4 UTF-8 bytes -> 2 QChars (surrogate pair).
  // cmark adapter uses QChar offsets via LineOffsetTable.
  const QString input = QString::fromUtf8("# \xF0\x9F\x8E\x89 Hello\n");
  auto result = parse(input);

  QCOMPARE(countElements(result, HLT_H1), 1);

  QCOMPARE(result.headerRegions.size(), 1);
  qDebug() << "Surrogate H1 start:" << result.headerRegions[0].m_startPos
           << "end:" << result.headerRegions[0].m_endPos;
  QCOMPARE(result.headerRegions[0].m_startPos, 0);
  QCOMPARE(result.headerRegions[0].m_endPos, 10);
}

void TestMarkdownParser::testEmptyElements() {
  // Empty bold: **** — no STRONG or EMPH.
  {
    const QString input = QStringLiteral("****\n");
    auto result = parse(input);

    QCOMPARE(countElements(result, HLT_STRONG), 0);
    QCOMPARE(countElements(result, HLT_EMPH), 0);
  }

  // Empty link text: [](url) — 1 LINK.
  {
    const QString input = QStringLiteral("[](url)\n");
    auto result = parse(input);

    QCOMPARE(countElements(result, HLT_LINK), 1);
  }
}

void TestMarkdownParser::testUnclosedDelimiters() {
  // Unclosed bold — no crash, 0 STRONG elements.
  {
    const QString input = QStringLiteral("**broken\n");
    auto result = parse(input);

    QCOMPARE(countElements(result, HLT_STRONG), 0);
  }

  // Unclosed link — no crash, 0 LINK elements.
  {
    const QString input = QStringLiteral("[unclosed\n");
    auto result = parse(input);

    QCOMPARE(countElements(result, HLT_LINK), 0);
  }
}

void TestMarkdownParser::testDegenerate() {
  // Empty string — walkAndConvert returns empty result (no crash).
  {
    const QString input = QStringLiteral("");
    QByteArray utf8 = input.toUtf8();
    int numBlocks = countBlocks(utf8);
    auto result = vte::md::walkAndConvert(utf8, numBlocks);
    // Empty result — no highlights.
    bool hasAny = false;
    for (const auto &block : result.blocksHighlights) {
      if (!block.isEmpty()) {
        hasAny = true;
        break;
      }
    }
    QVERIFY(!hasAny);
  }

  // Single newline — no crash.
  {
    const QString input = QStringLiteral("\n");
    auto result = parse(input);
    // Just verify it doesn't crash — no specific elements expected.
    (void)result;
  }

  // Spaces — no crash.
  {
    const QString input = QStringLiteral("   \n");
    auto result = parse(input);
    (void)result;
  }
}

void TestMarkdownParser::testNestedOverlap() {
  const QString input = QStringLiteral("***bold-italic***\n");
  auto result = parse(input);

  QCOMPARE(countElements(result, HLT_EMPH), 1);
  QCOMPARE(countElements(result, HLT_STRONG), 1);

  // Verify both elements exist with valid ranges.
  auto emphElems = findElements(result, HLT_EMPH, input);
  QVERIFY(!emphElems.isEmpty());
  QVERIFY((int)emphElems[0].first < (int)emphElems[0].second);

  auto strongElems = findElements(result, HLT_STRONG, input);
  QVERIFY(!strongElems.isEmpty());
  QVERIFY((int)strongElems[0].first < (int)strongElems[0].second);
}

void TestMarkdownParser::testAllExtensions() {
  const QString input = QStringLiteral("---\ntitle: test\n---\n\n"
                                       "# Heading\n\n"
                                       "*emph* **strong** ~~strike~~ ==mark==\n\n"
                                       "$E=mc^2$ $$F=ma$$\n\n"
                                       "| h1 | h2 |\n|---|---|\n| a | b |\n\n"
                                       "[^1]: note\n\n"
                                       "Text [^1]\n");
  auto result = parse(input);

  QVERIFY(countElements(result, HLT_FRONTMATTER) >= 1);
  QVERIFY(countElements(result, HLT_H1) >= 1);
  QVERIFY(countElements(result, HLT_EMPH) >= 1);
  QVERIFY(countElements(result, HLT_STRONG) >= 1);
  QVERIFY(countElements(result, HLT_STRIKE) >= 1);
  QVERIFY(countElements(result, HLT_INLINEEQUATION) >= 1);
  QVERIFY(countElements(result, HLT_TABLE) >= 1);
  QVERIFY(countElements(result, HLT_NOTE) >= 1);

  // cmark produces MARK elements (pmh did not).
  QVERIFY(countElements(result, HLT_MARK) >= 1);
}

// ============================================================
// T13: Performance Benchmark
// ============================================================

void TestMarkdownParser::testPerformance() {
  // Generate a 1000-line markdown document with mixed content.
  QString doc;
  doc.reserve(64000);
  for (int i = 0; i < 100; i++) {
    doc += QString("# Heading %1\n\n").arg(i);
    doc += QString("## Sub-heading %1\n\n").arg(i);
    doc += QString("### Third level %1\n\n").arg(i);
    doc +=
        QString("Paragraph with *emph*, **strong**, `code`, ~~strike~~ and $E=mc^2$ inline.\n\n");
    doc += "```cpp\nint x = 42;\nreturn x;\n```\n\n";
    doc += "- bullet one\n- bullet two\n- bullet three\n\n";
    doc += "1. first\n2. second\n\n";
    doc += "| h1 | h2 | h3 |\n|---|---|---|\n| a | b | c |\n\n";
    doc += "$$\nF = ma\n$$\n\n";
  }

  QByteArray utf8 = doc.toUtf8();
  int numBlocks = countBlocks(utf8);

  // Run 3 iterations, collect times.
  QVector<qint64> times;
  times.reserve(3);
  for (int iter = 0; iter < 3; iter++) {
    QElapsedTimer timer;
    timer.start();
    auto result = vte::md::walkAndConvert(utf8, numBlocks);
    qint64 elapsed = timer.elapsed();
    // Verify non-empty result.
    bool hasAny = false;
    for (const auto &block : result.blocksHighlights) {
      if (!block.isEmpty()) {
        hasAny = true;
        break;
      }
    }
    QVERIFY(hasAny);
    times.append(elapsed);
  }

  std::sort(times.begin(), times.end());
  qint64 median = times[1];
  qDebug() << "walkAndConvert parse times (ms):" << times[0] << times[1] << times[2]
           << "median:" << median;

  QVERIFY2(median < 500,
           qPrintable(QString("Median parse time %1ms exceeds 500ms threshold").arg(median)));
}

// ============================================================
// Extra selection invalidation
// ============================================================

namespace {
// Accumulate the paint regions delivered to a widget.
class PaintRegionRecorder : public QObject {
public:
  explicit PaintRegionRecorder(QWidget *p_widget) : QObject(p_widget) {
    p_widget->installEventFilter(this);
  }

  void clear() { m_region = QRegion(); }

  const QRegion &region() const { return m_region; }

protected:
  bool eventFilter(QObject *p_obj, QEvent *p_event) Q_DECL_OVERRIDE {
    if (p_event->type() == QEvent::Paint) {
      m_region += static_cast<QPaintEvent *>(p_event)->region();
    }
    return QObject::eventFilter(p_obj, p_event);
  }

private:
  QRegion m_region;
};

// Wait until no paint event is received during a quiet window, so that delayed
// highlighting, initial show and resize paints could not pollute the recording.
bool settlePaints(PaintRegionRecorder &p_recorder, int p_quietMs = 150, int p_maxMs = 5000) {
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < p_maxMs) {
    p_recorder.clear();
    QTest::qWait(p_quietMs);
    if (p_recorder.region().isEmpty()) {
      return true;
    }
  }
  return false;
}

// A one pixel high row spanning the whole viewport.
QRect fullWidthRow(const QWidget *p_viewport, int p_y) {
  return QRect(0, p_y, p_viewport->width(), 1);
}

// QRegion::contains(QRect) semantics differ between Qt versions, so check the
// complete containment explicitly.
bool regionContains(const QRegion &p_region, const QRect &p_rect) {
  return QRegion(p_rect).subtracted(p_region).isEmpty();
}
} // namespace

void TestMarkdownParser::testCursorLineInvalidationExpanded_data() {
  QTest::addColumn<qreal>("lineSpacing");

  // A line spacing greater than 1.0 gives the lines a fractional geometry, for
  // which the rounded cursor rectangle differs from the aligned extent that the
  // full-width selection is painted over.
  QTest::newRow("integral line geometry") << 1.0;
  QTest::newRow("fractional line geometry") << 1.5;
}

void TestMarkdownParser::testCursorLineInvalidationExpanded() {
  QFETCH(qreal, lineSpacing);

  // Avoid cursor blink repaints polluting the recorded paint regions.
  const int cursorFlashTime = QApplication::cursorFlashTime();
  QApplication::setCursorFlashTime(0);
  struct FlashTimeRestorer {
    ~FlashTimeRestorer() { QApplication::setCursorFlashTime(m_value); }
    int m_value = 0;
  } flashTimeRestorer{cursorFlashTime};

  const QString themeJson = QStringLiteral(R"({
    "metadata": {"type": "vtextedit", "name": "CursorLineTest"},
    "editor-styles": {
      "Text": {"font-family": "Arial", "font-size": 12},
      "CursorLine": {"background-color": "#c5cae9"}
    }
  })");
  auto theme = vte::Theme::createThemeFromContent(themeJson);
  QVERIFY(!theme.isNull());

  auto textConfig = QSharedPointer<vte::TextEditorConfig>::create();
  textConfig->m_theme = theme;
  textConfig->m_lineSpacing = lineSpacing;
  auto markdownConfig = QSharedPointer<vte::MarkdownEditorConfig>::create(textConfig);
  markdownConfig->m_inplacePreviewSources = vte::MarkdownEditorConfig::NoInplacePreview;
  auto parameters = QSharedPointer<vte::TextEditorParameters>::create();
  vte::VMarkdownEditor editor(markdownConfig, parameters);
  auto highlighter = editor.getHighlighter();
  QVERIFY(highlighter);

  QString text;
  const int lineCount = 16;
  for (int i = 0; i < lineCount; ++i) {
    text += QStringLiteral("line %1 of plain text\n").arg(i);
  }

  QSignalSpy completed(highlighter, &vte::MarkdownHighlighter::highlightCompleted);
  editor.setText(text);
  completed.clear();
  highlighter->updateHighlight();
  QTRY_VERIFY(completed.count() > 0);

  auto textEdit = editor.getTextEdit();
  QVERIFY(textEdit);
  auto viewport = textEdit->viewport();

  editor.resize(600, 600);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));

  PaintRegionRecorder recorder(viewport);

  auto moveCursorToBlock = [&editor, textEdit](int p_blockNumber) {
    auto cursor = textEdit->textCursor();
    cursor.setPosition(editor.document()->findBlockByNumber(p_blockNumber).position());
    textEdit->setTextCursor(cursor);
  };

  const int oldBlock = 3;
  const int newBlock = 7;
  const int untouchedBlock = 12;

  // Settle the initial show/resize/highlight paints before recording.
  moveCursorToBlock(oldBlock);
  QVERIFY(settlePaints(recorder));

  const QRect oldRect = textEdit->cursorRect(textEdit->textCursor());
  const QRect untouchedRect =
      textEdit->cursorRect(QTextCursor(editor.document()->findBlockByNumber(untouchedBlock)));

  recorder.clear();
  moveCursorToBlock(newBlock);
  const QRect newRect = textEdit->cursorRect(textEdit->textCursor());

  QTRY_VERIFY(!recorder.region().isEmpty());
  // Collect the remaining queued paints of this cursor move.
  QTest::qWait(150);

  // VTextEdit::cursorRect() rounds the line geometry with qRound(), while the
  // selection is painted over its aligned (floor/ceil) extent. The fringe row
  // outside of the painted extent is therefore either the first or the second
  // row outside of the rounded rectangle, so both of them must be invalidated.
  const int maxMargin = 2;

  // All the involved lines must have room above and below within the viewport,
  // otherwise the expanded rows would be clipped away.
  const QRect viewportRect = viewport->rect();
  QVERIFY(oldRect.top() - maxMargin >= viewportRect.top());
  QVERIFY(oldRect.bottom() + maxMargin <= viewportRect.bottom());
  QVERIFY(newRect.top() - maxMargin >= viewportRect.top());
  QVERIFY(newRect.bottom() + maxMargin <= viewportRect.bottom());
  QVERIFY(untouchedRect.top() - 1 >= viewportRect.top());
  QVERIFY(untouchedRect.isValid() && untouchedRect.bottom() <= viewportRect.bottom());

  const QRegion region = recorder.region();
  for (int margin = 1; margin <= maxMargin; ++margin) {
    QVERIFY2(
        regionContains(region, fullWidthRow(viewport, oldRect.top() - margin)),
        qPrintable(
            QStringLiteral("Row %1 above the old cursor line is not invalidated").arg(margin)));
    QVERIFY2(
        regionContains(region, fullWidthRow(viewport, oldRect.bottom() + margin)),
        qPrintable(
            QStringLiteral("Row %1 below the old cursor line is not invalidated").arg(margin)));
    QVERIFY2(
        regionContains(region, fullWidthRow(viewport, newRect.top() - margin)),
        qPrintable(
            QStringLiteral("Row %1 above the new cursor line is not invalidated").arg(margin)));
    QVERIFY2(
        regionContains(region, fullWidthRow(viewport, newRect.bottom() + margin)),
        qPrintable(
            QStringLiteral("Row %1 below the new cursor line is not invalidated").arg(margin)));
  }

  // The supplemental invalidation must stay local. This also guards the
  // assertions above against a full viewport repaint.
  QVERIFY2(!regionContains(region, fullWidthRow(viewport, untouchedRect.top() - 1)),
           "An unrelated line is invalidated by a cursor line move");

  // Re-applying the extra selections without changing the full-width cursor
  // line selection must not invalidate its expanded rows again.
  recorder.clear();
  editor.clearSearchHighlight();
  QTest::qWait(500);
  QVERIFY2(!regionContains(recorder.region(), fullWidthRow(viewport, newRect.top() - 1)),
           "An unchanged full-width selection is invalidated again");
  QVERIFY2(!regionContains(recorder.region(), fullWidthRow(viewport, newRect.bottom() + 1)),
           "An unchanged full-width selection is invalidated again");
}

// ============================================================
// Typed preview element extraction
// ============================================================

void TestMarkdownParser::testTableElementBasic() {
  const QString input = QStringLiteral("| h1 | h2 |\n| --- | --- |\n| a | b |\n");
  auto result = parse(input);

  QCOMPARE(result.tableElements.size(), 1);
  const auto &table = result.tableElements.first();
  QCOMPARE(table.m_columns, 2);
  QCOMPARE(table.m_rows.size(), 3);
  QCOMPARE(table.m_startPos, 0);
  // The range excludes the terminating paragraph separator.
  QCOMPARE(table.m_endPos, input.indexOf(QStringLiteral("| a | b |")) + 9);

  QVERIFY(table.m_rows[0].m_type == vte::md::TableRowType::Header);
  QVERIFY(table.m_rows[1].m_type == vte::md::TableRowType::Delimiter);
  QVERIFY(table.m_rows[2].m_type == vte::md::TableRowType::Data);

  QCOMPARE(table.m_rows[0].m_cells, QVector<QString>({QStringLiteral("h1"), QStringLiteral("h2")}));
  QCOMPARE(table.m_rows[2].m_cells, QVector<QString>({QStringLiteral("a"), QStringLiteral("b")}));
  QVERIFY(table.m_rows[0].m_prefix.isEmpty());
}

void TestMarkdownParser::testTableElementAlignments() {
  const QString input =
      QStringLiteral("| a | b | c | d |\n| --- | :--- | :---: | ---: |\n| 1 | 2 | 3 | 4 |\n");
  auto result = parse(input);

  QCOMPARE(result.tableElements.size(), 1);
  // 0 none, 1 left, 2 center, 3 right.
  QCOMPARE(result.tableElements.first().m_alignments, QVector<int>({0, 1, 2, 3}));
}

void TestMarkdownParser::testTableElementRawCells() {
  const QString input =
      QStringLiteral("| **bold** | [x](y.md) |\n| --- | --- |\n| `a|b` | _i_ |\n");
  auto result = parse(input);

  QCOMPARE(result.tableElements.size(), 1);
  const auto &table = result.tableElements.first();
  // Raw Markdown is preserved; inline processing never touches these values.
  QCOMPARE(table.m_rows[0].m_cells[0], QStringLiteral("**bold**"));
  QCOMPARE(table.m_rows[0].m_cells[1], QStringLiteral("[x](y.md)"));
  // A pipe inside a code span still splits the cell in this dialect.
  QCOMPARE(table.m_rows[2].m_cells.size(), 3);
}

void TestMarkdownParser::testTableElementEscapedPipes() {
  const QString input = QStringLiteral("| a \\| b | c |\n| --- | --- |\n| d | e |\n");
  auto result = parse(input);

  QCOMPARE(result.tableElements.size(), 1);
  const auto &table = result.tableElements.first();
  QCOMPARE(table.m_columns, 2);
  QCOMPARE(table.m_rows[0].m_cells.size(), 2);
  // The escape is part of the raw source and must survive.
  QCOMPARE(table.m_rows[0].m_cells[0], QStringLiteral("a \\| b"));
}

void TestMarkdownParser::testTableElementEmptyAndRaggedRows() {
  const QString input = QStringLiteral("| a | b |\n| --- | --- |\n||\n| x | y | z |\n| only |\n");
  auto result = parse(input);

  QCOMPARE(result.tableElements.size(), 1);
  const auto &table = result.tableElements.first();
  QCOMPARE(table.m_columns, 2);
  QCOMPARE(table.m_rows.size(), 5);
  // Empty cell.
  QCOMPARE(table.m_rows[2].m_cells, QVector<QString>({QString()}));
  // Extra wide row is preserved verbatim, nothing is discarded.
  QCOMPARE(table.m_rows[3].m_cells.size(), 3);
  QCOMPARE(table.m_rows[3].m_cells[2], QStringLiteral("z"));
  // Narrower row.
  QCOMPARE(table.m_rows[4].m_cells, QVector<QString>({QStringLiteral("only")}));
}

void TestMarkdownParser::testTableElementSurrogatePositions() {
  // The emoji is a surrogate pair: UTF-16 offsets must not be byte offsets.
  const QString prefix = QStringLiteral("\xF0\x9F\x98\x80 head\n\n");
  const QString input = QString::fromUtf8(
      "\xF0\x9F\x98\x80 head\n\n| \xF0\x9F\x98\x80 | b |\n| --- | --- |\n| c | d |\n");
  auto result = parse(input);

  QCOMPARE(result.tableElements.size(), 1);
  const auto &table = result.tableElements.first();
  QCOMPARE(table.m_startPos, input.indexOf(QStringLiteral("| ")));
  QCOMPARE(table.m_rows[0].m_cells[0], QString::fromUtf8("\xF0\x9F\x98\x80"));
  QCOMPARE(input.mid(table.m_startPos, 1), QStringLiteral("|"));
  Q_UNUSED(prefix);
}

void TestMarkdownParser::testTableElementNestedPrefixes() {
  const QString input = QStringLiteral("> | a | b |\n> | --- | --- |\n> | c | d |\n");
  auto result = parse(input);

  QCOMPARE(result.tableElements.size(), 1);
  const auto &table = result.tableElements.first();
  // The range includes the container prefix.
  QCOMPARE(table.m_startPos, 0);
  for (const auto &row : table.m_rows) {
    QCOMPARE(row.m_prefix, QStringLiteral("> "));
  }
}

void TestMarkdownParser::testTableElementInvalid() {
  // Missing trailing pipe: not a table in this dialect.
  QCOMPARE(parse(QStringLiteral("| a | b\n| --- | ---\n")).tableElements.size(), 0);
  // Column count mismatch.
  QCOMPARE(parse(QStringLiteral("| a | b | c |\n| --- | --- |\n")).tableElements.size(), 0);
  // No delimiter row.
  QCOMPARE(parse(QStringLiteral("| a | b |\n| c | d |\n")).tableElements.size(), 0);
}

void TestMarkdownParser::testImageCodeMathElements() {
  {
    const QString input = QStringLiteral("![alt](pic.png \"t\")\n");
    auto result = parse(input);
    QCOMPARE(result.imageElements.size(), 1);
    const auto &image = result.imageElements.first();
    QCOMPARE(image.m_destination, QStringLiteral("pic.png"));
    QCOMPARE(image.m_alternateText, QStringLiteral("alt"));
    QCOMPARE(image.m_title, QStringLiteral("t"));
    QVERIFY(image.m_standalone);
    QCOMPARE(image.m_startPos, 0);
  }

  {
    const QString input = QStringLiteral("text ![a](b.png) more\n");
    auto result = parse(input);
    QCOMPARE(result.imageElements.size(), 1);
    QVERIFY(!result.imageElements.first().m_standalone);
  }

  {
    const QString input = QStringLiteral("```cpp\nint a;\n```\n");
    auto result = parse(input);
    QCOMPARE(result.codeElements.size(), 1);
    QCOMPARE(result.codeElements.first().m_language, QStringLiteral("cpp"));
    QCOMPARE(result.codeElements.first().m_code, QStringLiteral("int a;\n"));
  }

  {
    const QString input = QStringLiteral("$$\nx^2\n$$\n");
    auto result = parse(input);
    QCOMPARE(result.mathElements.size(), 1);
    QVERIFY(result.mathElements.first().m_display);
  }
}

// End-to-end plumbing of the walker's heading elements: walker ->
// MarkdownParseResult -> MarkdownHighlighterResult -> headingsUpdated. The
// walker test alone cannot catch an omitted std::move in either parse path, or
// an omitted copy in the highlighter result.
void TestMarkdownParser::testHeadingElementsPublished() {
  auto textConfig = QSharedPointer<vte::TextEditorConfig>::create();
  auto markdownConfig = QSharedPointer<vte::MarkdownEditorConfig>::create(textConfig);
  markdownConfig->m_inplacePreviewSources = vte::MarkdownEditorConfig::NoInplacePreview;
  auto parameters = QSharedPointer<vte::TextEditorParameters>::create();
  vte::VMarkdownEditor editor(markdownConfig, parameters);
  auto highlighter = editor.getHighlighter();
  QVERIFY(highlighter);

  // A direct lambda rather than a QSignalSpy: no metatype registration is
  // needed, and the signal is deliberately a same-thread direct connection.
  QVector<vte::md::HeadingInfo> published;
  int emissions = 0;
  QObject::connect(highlighter, &vte::MarkdownHighlighter::headingsUpdated, &editor,
                   [&published, &emissions](const QVector<vte::md::HeadingInfo> &p_headings) {
                     published = p_headings;
                     ++emissions;
                   });

  QSignalSpy completed(highlighter, &vte::MarkdownHighlighter::highlightCompleted);
  editor.setText(QStringLiteral("# A **bold** `x`\n\nbody\n\n## [a](b)\n"));
  completed.clear();
  highlighter->updateHighlight();
  QTRY_VERIFY(completed.count() > 0);

  QVERIFY(emissions > 0);
  QCOMPARE(published.size(), 2);
  QCOMPARE(published.at(0).m_level, 1);
  QCOMPARE(published.at(0).m_title, QStringLiteral("A bold x"));
  QCOMPARE(published.at(0).m_startPos, 0);
  QCOMPARE(published.at(1).m_level, 2);
  QCOMPARE(published.at(1).m_title, QStringLiteral("a"));
  QCOMPARE(published.at(1).m_anchorText, QStringLiteral("a"));
  QVERIFY(published.at(1).m_startPos > published.at(0).m_startPos);

  // The block of the start position is the heading's own line.
  QCOMPARE(editor.document()->findBlock(published.at(1).m_startPos).blockNumber(), 4);

  // The synchronous MarkdownParser::parse() path is a copy-paste twin of the
  // worker's; cover it directly, or an omitted std::move there stays invisible.
  vte::md::MarkdownParser parser;
  auto config = QSharedPointer<vte::md::MarkdownParseConfig>::create();
  config->m_data = QByteArray("# one\n\n## two\n");
  config->m_numOfBlocks = 3;
  auto syncResult = parser.parse(config);
  QVERIFY(!syncResult.isNull());
  QCOMPARE(syncResult->m_headingElements.size(), 2);
  QCOMPARE(syncResult->m_headingElements.at(0).m_title, QStringLiteral("one"));
  QCOMPARE(syncResult->m_headingElements.at(1).m_title, QStringLiteral("two"));

  // A fast parse publishes no heading data at all.
  config->m_fast = true;
  auto fastResult = parser.parse(config);
  QVERIFY(!fastResult.isNull());
  QVERIFY(fastResult->m_headingElements.isEmpty());

  // ... and unrelated rehighlighting of sensitive blocks does not republish
  // (only completeHighlight() emits, and the fast path never reaches it), so
  // the last full publication survives.
  const auto before = published;
  const int emissionsBefore = emissions;
  editor.getHighlighter()->rehighlightSensitiveBlocks();
  QCOMPARE(emissions, emissionsBefore);
  QCOMPARE(published.size(), before.size());
  QCOMPARE(published.at(0).m_title, before.at(0).m_title);
}

// cmarkNodeSpan() / cmarkNodeUrlSpan() are the single implementation of
// cmark-coordinates-to-document-offset mapping, shared by the walker and by the
// snapshot API. Exercise them directly, in the container shapes where cmark's
// block_offset accounting needs correcting, and over destinations whose cleaned
// form differs in length from their raw source spelling.
void TestMarkdownParser::testCmarkNodeSpans() {
  struct Case {
    const char *markdown;
    const char *region; // exact raw text of the image construct
    const char *rawUrl; // exact raw text of the destination, "" when spanless
  };

  const QVector<Case> cases{
      {"![a](i.png)\n", "![a](i.png)", "i.png"},
      // Block quote: the stripped `> ` prefix must not shift the span.
      {"> ![a](i.png)\n", "![a](i.png)", "i.png"},
      // Block quote continuation line: the prefix is stripped per line.
      {"> lead\n> ![a](i.png)\n", "![a](i.png)", "i.png"},
      // List item continuation.
      {"- lead\n  ![a](i.png)\n", "![a](i.png)", "i.png"},
      // Lazy continuation (no indent on the second line).
      {"- lead\n![a](i.png)\n", "![a](i.png)", "i.png"},
      // Spanning two lines, both ways round.
      {"![a\nb](i.png)\n", "![a\nb](i.png)", "i.png"},
      {"![a](\ni.png)\n", "![a](\ni.png)", "i.png"},
      // Ending in an astral character: exercises qcharWidthAtEndColumn().
      {"![\xf0\x9f\x92\x8e](i.png)\n", "![\xf0\x9f\x92\x8e](i.png)", "i.png"},
      {"![a](\xf0\x9f\x92\x8e.png)\n", "![a](\xf0\x9f\x92\x8e.png)", "\xf0\x9f\x92\x8e.png"},
      // The raw destination keeps what cmark_node_get_url() resolves away, and
      // differs from it in length -- which is exactly what the old
      // indexOf(cleanedUrl) search could not handle.
      {"![a](a\\_b.png)\n", "![a](a\\_b.png)", "a\\_b.png"},
      {"![a](<a b.png>)\n", "![a](<a b.png>)", "<a b.png>"},
      {"![a](a&amp;b.png)\n", "![a](a&amp;b.png)", "a&amp;b.png"},
      // The `=WxH` suffix is inside the region but outside the destination.
      {"![a](i.png =500x300)\n", "![a](i.png =500x300)", "i.png"},
      // No inline destination at all.
      {"![a][r]\n\n[r]: i.png\n", "![a][r]", ""},
      {"![a]()\n", "![a]()", ""},
  };

  for (const auto &c : cases) {
    const QByteArray utf8(c.markdown);
    const QString text = QString::fromUtf8(utf8);
    LineOffsetTable offsets(utf8);

    cmark_node *doc = cmark_parse_document(utf8.constData(), utf8.size(), CMARK_OPT_DEFAULT);
    QVERIFY(doc);

    cmark_iter *iter = cmark_iter_new(doc);
    cmark_node *image = nullptr;
    cmark_event_type ev;
    while ((ev = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
      cmark_node *cur = cmark_iter_get_node(iter);
      if (ev == CMARK_EVENT_ENTER && cmark_node_get_type(cur) == CMARK_NODE_IMAGE) {
        image = cur;
        break;
      }
    }
    cmark_iter_free(iter);
    QVERIFY2(image, c.markdown);

    int start = -1;
    int end = -1;
    QVERIFY2(cmarkNodeSpan(image, offsets, start, end), c.markdown);
    QCOMPARE(text.mid(start, end - start), QString::fromUtf8(c.region));

    int urlStart = -1;
    int urlEnd = -1;
    const bool hasUrl = cmarkNodeUrlSpan(image, offsets, urlStart, urlEnd);
    if (*c.rawUrl == '\0') {
      QVERIFY2(!hasUrl, c.markdown);
    } else {
      QVERIFY2(hasUrl, c.markdown);
      QCOMPARE(text.mid(urlStart, urlEnd - urlStart), QString::fromUtf8(c.rawUrl));
      // The destination always sits inside the construct it belongs to.
      QVERIFY(start <= urlStart && urlStart < urlEnd && urlEnd <= end);
    }

    cmark_node_free(doc);
  }
}

// The `=WxH` size extension, all the way to the ImageElement, and the
// projection onto what the highlighter publishes.
void TestMarkdownParser::testImageSizeElements() {
  struct Case {
    const char *markdown;
    const char *destination;
    int width;
    int height;
    const char *title;
  };

  const QVector<Case> cases{
      {"![](a.png)\n", "a.png", 0, 0, ""},
      {"![](a.png =500x)\n", "a.png", 500, 0, ""},
      {"![](a.png =500x300)\n", "a.png", 500, 300, ""},
      {"![](a.png =x300)\n", "a.png", 0, 300, ""},
      {"![](a.png \"the title\" =500x)\n", "a.png", 500, 0, "the title"},
      // Without a separating space the token is part of the destination.
      {"![](a.png=500x)\n", "a.png=500x", 0, 0, ""},
      // The escape is resolved; the size is still parsed off the end.
      {"![](a\\_b.png =500x)\n", "a_b.png", 500, 0, ""},
      // A link is not an image, so no size is ever parsed for one.
      {"![](<a b.png> =64x64)\n", "a b.png", 64, 64, ""},
  };

  for (const auto &c : cases) {
    const QString input = QString::fromUtf8(c.markdown);
    auto result = parse(input);
    QCOMPARE(result.imageElements.size(), 1);

    const auto &image = result.imageElements.first();
    QCOMPARE(image.m_destination, QString::fromUtf8(c.destination));
    QCOMPARE(image.m_width, c.width);
    QCOMPARE(image.m_height, c.height);
    QCOMPARE(image.m_title, QString::fromUtf8(c.title));
    // The size token is never part of the destination.
    QVERIFY2(!image.m_destination.contains(QStringLiteral(" =")), c.markdown);

    // buildImageLinks() is a 1:1, order-preserving projection.
    const auto links = vte::md::buildImageLinks(result.imageElements);
    QCOMPARE(links.size(), 1);
    QCOMPARE(links.first().m_destination, image.m_destination);
    QCOMPARE(links.first().m_width, image.m_width);
    QCOMPARE(links.first().m_height, image.m_height);
    QCOMPARE(links.first().m_region.m_startPos, image.m_startPos);
    QCOMPARE(links.first().m_region.m_endPos, image.m_endPos);
  }

  // Order is preserved across several images.
  {
    auto result = parse(QStringLiteral("![](a.png =1x) ![](b.png =2x) ![](c.png =3x)\n"));
    QCOMPARE(result.imageElements.size(), 3);
    const auto links = vte::md::buildImageLinks(result.imageElements);
    QCOMPARE(links.size(), 3);
    for (int i = 0; i < 3; ++i) {
      QCOMPARE(links[i].m_destination, result.imageElements[i].m_destination);
      QCOMPARE(links[i].m_width, i + 1);
    }
  }
}

namespace {
// Every fixture that contains image links, for the corpus-wide gates.
const QStringList &imageFixtures() {
  static const QStringList names{QStringLiteral("image_elements.md"),
                                 QStringLiteral("inline_elements.md")};
  return names;
}

MarkdownLink::TypeFlags allTypes() {
  return MarkdownLink::TypeFlag::LocalRelativeInternal |
         MarkdownLink::TypeFlag::LocalRelativeExternal | MarkdownLink::TypeFlag::LocalAbsolute |
         MarkdownLink::TypeFlag::QtResource | MarkdownLink::TypeFlag::Remote;
}
} // namespace

// P3.1/P3.3: exact region and RAW destination spans, across the destination
// spellings where the cleaned value differs from the source text. The old
// implementation searched the content for the CLEANED url, so these were either
// dropped outright or matched against an unrelated earlier occurrence.
void TestMarkdownParser::testFetchImageLinksSpans() {
  struct Case {
    const char *markdown;
    const char *region;
    const char *rawUrl; // "" when the image has no destination span
    const char *cleanUrl;
  };

  const QVector<Case> cases{
      {"![a](i.png)\n", "![a](i.png)", "i.png", "i.png"},
      {"![a](i.png =500x300)\n", "![a](i.png =500x300)", "i.png", "i.png"},
      // The three spellings that break a text search for the cleaned value.
      {"![a](a\\_b.png)\n", "![a](a\\_b.png)", "a\\_b.png", "a_b.png"},
      {"![a](<a b.png>)\n", "![a](<a b.png>)", "<a b.png>", "a b.png"},
      {"![a](a&amp;b.png)\n", "![a](a&amp;b.png)", "a&amp;b.png", "a&b.png"},
      {"![a](a%20b.png)\n", "![a](a%20b.png)", "a%20b.png", "a%20b.png"},
      // A title containing `](` defeats any scan for the last `](`.
      {"![a](i.png \"x](y\")\n", "![a](i.png \"x](y\")", "i.png", "i.png"},
      {"![a](a(b)c.png)\n", "![a](a(b)c.png)", "a(b)c.png", "a(b)c.png"},
      // Containers and continuations.
      {"> ![a](i.png)\n", "![a](i.png)", "i.png", "i.png"},
      {"- lead\n  ![a](i.png)\n", "![a](i.png)", "i.png", "i.png"},
      {"![a\nb](i.png)\n", "![a\nb](i.png)", "i.png", "i.png"},
      {"![a](\ni.png)\n", "![a](\ni.png)", "i.png", "i.png"},
      {"> ![a](\n> i.png)\n", "![a](\n> i.png)", "i.png", "i.png"},
  };

  for (const auto &c : cases) {
    const QString content = QString::fromUtf8(c.markdown);
    const auto links = MarkdownUtils::fetchImageLinks(content, QStringLiteral("/base"), allTypes());
    QVERIFY2(links.size() == 1, c.markdown);

    const auto &link = links.first();
    QCOMPARE(link.m_urlInLink, QString::fromUtf8(c.cleanUrl));
    QCOMPARE(content.mid(link.m_regionStart, link.m_regionEnd - link.m_regionStart),
             QString::fromUtf8(c.region));
    QVERIFY2(link.hasUrlSpan(), c.markdown);
    QCOMPARE(content.mid(link.m_urlStart, link.m_urlEnd - link.m_urlStart),
             QString::fromUtf8(c.rawUrl));
  }
}

// P3.2: a reference-style image and an empty destination have a valid region
// but no destination span. The old implementation dropped reference-style
// images entirely, which is why a reference-style local image was silently
// omitted from an export bundle.
void TestMarkdownParser::testFetchImageLinksWithoutUrlSpan() {
  {
    const QString content = QStringLiteral("![a][r]\n\n[r]: p.png\n");
    const auto links = MarkdownUtils::fetchImageLinks(content, QStringLiteral("/base"), allTypes());
    QCOMPARE(links.size(), 1);
    QCOMPARE(links.first().m_urlInLink, QStringLiteral("p.png"));
    QVERIFY(!links.first().hasUrlSpan());
    QCOMPARE(links.first().m_urlStart, -1);
    QCOMPARE(content.mid(links.first().m_regionStart,
                         links.first().m_regionEnd - links.first().m_regionStart),
             QStringLiteral("![a][r]"));
  }

  {
    // An empty destination points at nothing at all, so it is not an image link
    // any caller can act on.
    const auto links = MarkdownUtils::fetchImageLinks(QStringLiteral("![a]()\n"),
                                                      QStringLiteral("/base"), allTypes());
    QCOMPARE(links.size(), 0);
  }
}

// P3.4: classification is syntactic. A relative link to a missing file stays
// LocalRelative* with exists == false; it used to be classified Remote purely
// because the file was not there, so a caller asking for local images skipped
// exactly the broken links a user would want repaired.
void TestMarkdownParser::testFetchImageLinksClassification() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("vx_images")));
  QFile present(QDir(dir.path()).filePath(QStringLiteral("vx_images/here.png")));
  QVERIFY(present.open(QIODevice::WriteOnly));
  present.write("x");
  present.close();

  const QString content = QStringLiteral("![a](vx_images/here.png)\n"
                                         "![b](vx_images/missing.png)\n"
                                         "![c](https://example.com/x.png)\n"
                                         "![d](qrc:/icons/x.png)\n"
                                         "![e](../outside.png)\n");
  const auto links = MarkdownUtils::fetchImageLinks(content, dir.path(), allTypes());
  QCOMPARE(links.size(), 5);

  QHash<QString, MarkdownLink> byAlt;
  for (const auto &l : links) {
    byAlt.insert(l.m_alt, l);
  }

  QVERIFY(byAlt[QStringLiteral("a")].m_type & MarkdownLink::TypeFlag::LocalRelativeInternal);
  QVERIFY(byAlt[QStringLiteral("a")].m_exists);

  // The one that matters: present-tense classification, absent file.
  QVERIFY(byAlt[QStringLiteral("b")].m_type & MarkdownLink::TypeFlag::LocalRelativeInternal);
  QVERIFY(!byAlt[QStringLiteral("b")].m_exists);
  QVERIFY(!(byAlt[QStringLiteral("b")].m_type & MarkdownLink::TypeFlag::Remote));

  QVERIFY(byAlt[QStringLiteral("c")].m_type & MarkdownLink::TypeFlag::Remote);
  QVERIFY(byAlt[QStringLiteral("d")].m_type & MarkdownLink::TypeFlag::QtResource);
  QVERIFY(byAlt[QStringLiteral("e")].m_type & MarkdownLink::TypeFlag::LocalRelativeExternal);

  // The filter selects on those same syntactic flags.
  const auto localOnly =
      MarkdownUtils::fetchImageLinks(content, dir.path(),
                                     MarkdownLink::TypeFlag::LocalRelativeInternal |
                                         MarkdownLink::TypeFlag::LocalRelativeExternal);
  QCOMPARE(localOnly.size(), 3);
}

// P3.5: the sort contract rewriting callers depend on.
void TestMarkdownParser::testFetchImageLinksSortContract() {
  const QString content = QStringLiteral("![a](one.png) ![b][r] ![c](two.png) ![d][r]\n"
                                         "![e](one.png)\n\n[r]: ref.png\n");
  const auto links = MarkdownUtils::fetchImageLinks(content, QStringLiteral("/base"), allTypes());
  QCOMPARE(links.size(), 5);

  // Spanned entries first, strictly descending by raw destination start.
  int spanned = 0;
  for (const auto &l : links) {
    if (!l.hasUrlSpan()) {
      break;
    }
    ++spanned;
  }
  QCOMPARE(spanned, 3);
  for (int i = 1; i < spanned; ++i) {
    QVERIFY(links[i - 1].m_urlStart > links[i].m_urlStart);
  }

  // Spanless entries last, and in document order (the sort is stable).
  for (int i = spanned; i < links.size(); ++i) {
    QVERIFY(!links[i].hasUrlSpan());
  }
  QCOMPARE(links[spanned].m_alt, QStringLiteral("b"));
  QCOMPARE(links[spanned + 1].m_alt, QStringLiteral("d"));

  // No deduplication: one.png appears twice and both must be rewritable.
  int oneCount = 0;
  for (const auto &l : links) {
    if (l.m_urlInLink == QStringLiteral("one.png")) {
      ++oneCount;
    }
  }
  QCOMPARE(oneCount, 2);
}

// G2: the walker and the snapshot API must report the same image regions, in
// the same order, for the same content. They are two consumers of one mapping;
// a divergence means one of them grew its own.
void TestMarkdownParser::testWalkerAndSnapshotAgreeOnRegions() {
  for (const auto &name : imageFixtures()) {
    const QString content = readFixture(name);
    QVERIFY2(!content.isEmpty(), qPrintable(name));

    const auto walked = parse(content).imageElements;
    const auto links = MarkdownUtils::fetchImageLinks(content, QStringLiteral("/base"), allTypes());

    // The snapshot API drops images with an empty destination; the walker keeps
    // them. Compare against the walker's list filtered the same way.
    QVector<vte::md::ImageElement> comparable;
    for (const auto &e : walked) {
      if (!e.m_destination.isEmpty()) {
        comparable.append(e);
      }
    }

    QCOMPARE(links.size(), comparable.size());

    // fetchImageLinks() sorts for rewriting; compare as sets of regions.
    QVector<QPair<int, int>> fromWalker;
    QVector<QPair<int, int>> fromSnapshot;
    for (const auto &e : comparable) {
      fromWalker.append(qMakePair(e.m_startPos, e.m_endPos));
    }
    for (const auto &l : links) {
      fromSnapshot.append(qMakePair(l.m_regionStart, l.m_regionEnd));
    }
    std::sort(fromWalker.begin(), fromWalker.end());
    std::sort(fromSnapshot.begin(), fromSnapshot.end());
    QCOMPARE(fromSnapshot, fromWalker);
  }
}

// G3: properties, checked for EVERY image in every fixture. Hand-enumerated
// cases only guard what someone thought of; these scale to grammar nobody
// anticipated.
void TestMarkdownParser::testImageLinkInvariants() {
  for (const auto &name : imageFixtures()) {
    const QString content = readFixture(name);
    QVERIFY2(!content.isEmpty(), qPrintable(name));

    const auto links = MarkdownUtils::fetchImageLinks(content, QStringLiteral("/base"), allTypes());
    QVERIFY2(!links.isEmpty(), qPrintable(name));

    for (const auto &link : links) {
      const QString where = QStringLiteral("%1: %2").arg(name, content.mid(link.m_regionStart, 40));

      // Every span lies inside the content. This is the class of bug the old
      // indexOf-based location produced when it matched the wrong occurrence.
      QVERIFY2(link.m_regionStart >= 0 && link.m_regionStart < link.m_regionEnd &&
                   link.m_regionEnd <= content.size(),
               qPrintable(where));

      if (!link.hasUrlSpan()) {
        continue;
      }

      QVERIFY2(link.m_regionStart <= link.m_urlStart && link.m_urlStart < link.m_urlEnd &&
                   link.m_urlEnd <= link.m_regionEnd,
               qPrintable(where));

      // THE invariant, checked against an INDEPENDENT oracle: feed the raw
      // span back through cmark as the destination of a fresh image and the
      // parser must resolve it to the same cleaned value. A span that is
      // off by one, or that points at the wrong occurrence, produces a
      // different destination here. Comparing the raw text to itself would
      // prove nothing -- replacing any in-bounds span with its own contents is
      // a no-op for every span, right or wrong.
      const QString raw = content.mid(link.m_urlStart, link.m_urlEnd - link.m_urlStart);
      if (link.m_syntax == MarkdownLink::Syntax::Html) {
        // The independent oracle for an HTML image is the scanner run over the
        // reported REGION alone: it must find exactly one tag, spanning the
        // whole region, whose decoded src is the reported destination and whose
        // src value span is the reported url span.
        vte::RawTextState state;
        const QString region =
            content.mid(link.m_regionStart, link.m_regionEnd - link.m_regionStart);
        const auto tags = vte::scanHtmlImgTags(region, link.m_regionStart, &state);
        QVERIFY2(tags.size() == 1, qPrintable(where));
        QCOMPARE(tags.first().m_tagStart, link.m_regionStart);
        QCOMPARE(tags.first().m_tagEnd, link.m_regionEnd);
        QCOMPARE(tags.first().src(), link.m_urlInLink);
        const auto *srcAttr = tags.first().attr("src");
        QVERIFY(srcAttr);
        QCOMPARE(srcAttr->m_valueStart, link.m_urlStart);
        QCOMPARE(srcAttr->m_valueEnd, link.m_urlEnd);
        QCOMPARE(content.mid(link.m_regionStart, 4).toLower(), QStringLiteral("<img"));
        continue;
      }

      {
        const QString probeMd = QStringLiteral("![](") + raw + QStringLiteral(")\n");
        const QByteArray probeUtf8 = probeMd.toUtf8();
        cmark_node *probeDoc =
            cmark_parse_document(probeUtf8.constData(), probeUtf8.size(), CMARK_OPT_DEFAULT);
        QVERIFY(probeDoc);
        cmark_iter *probeIter = cmark_iter_new(probeDoc);
        QString reparsed;
        bool sawImage = false;
        cmark_event_type pev;
        while ((pev = cmark_iter_next(probeIter)) != CMARK_EVENT_DONE) {
          cmark_node *cur = cmark_iter_get_node(probeIter);
          if (pev == CMARK_EVENT_ENTER && cmark_node_get_type(cur) == CMARK_NODE_IMAGE) {
            sawImage = true;
            const char *u = cmark_node_get_url(cur);
            reparsed = u ? QString::fromUtf8(u) : QString();
            break;
          }
        }
        cmark_iter_free(probeIter);
        cmark_node_free(probeDoc);
        QVERIFY2(sawImage, qPrintable(where + QStringLiteral(" raw=") + raw));
        QCOMPARE(reparsed, link.m_urlInLink);
      }

      // The region begins at the `!` of `![`.
      QCOMPARE(content.mid(link.m_regionStart, 2), QStringLiteral("!["));
    }

    // Regions are properly nested: any two are either disjoint or one wholly
    // contains the other. CommonMark permits an image inside another image's
    // description, so plain disjointness is NOT an invariant -- asserting it
    // would encode a false grammar rule and give whole-region rewriting
    // callers a guarantee the parser does not make.
    for (int i = 0; i < links.size(); ++i) {
      for (int j = i + 1; j < links.size(); ++j) {
        const auto &a = links[i];
        const auto &b = links[j];
        const bool disjoint = a.m_regionEnd <= b.m_regionStart || b.m_regionEnd <= a.m_regionStart;
        const bool aInB = b.m_regionStart <= a.m_regionStart && a.m_regionEnd <= b.m_regionEnd;
        const bool bInA = a.m_regionStart <= b.m_regionStart && b.m_regionEnd <= a.m_regionEnd;
        QVERIFY2(disjoint || aInB || bInA, qPrintable(name));
      }
    }

    // Destination spans, unlike regions, NEVER overlap -- which is what makes
    // destination-only rewriting safe even across nested images.
    QVector<QPair<int, int>> urlSpans;
    for (const auto &l : links) {
      if (l.hasUrlSpan()) {
        urlSpans.append(qMakePair(l.m_urlStart, l.m_urlEnd));
      }
    }
    std::sort(urlSpans.begin(), urlSpans.end());
    for (int i = 1; i < urlSpans.size(); ++i) {
      QVERIFY2(urlSpans[i - 1].second <= urlSpans[i].first, qPrintable(name));
    }
  }
}

// CommonMark allows an image inside another image's description. Both are
// reported, their regions nest, and -- crucially -- their destination spans do
// not overlap, so destination rewriting stays safe.
void TestMarkdownParser::testNestedImages() {
  const QString content = QStringLiteral("![foo ![bar](/a.png)](/b.png) tail\n");
  const auto links = MarkdownUtils::fetchImageLinks(content, QStringLiteral("/base"), allTypes());
  QCOMPARE(links.size(), 2);

  const auto *outer = &links[0];
  const auto *inner = &links[1];
  if (outer->m_urlInLink != QStringLiteral("/b.png")) {
    std::swap(outer, inner);
  }
  QCOMPARE(outer->m_urlInLink, QStringLiteral("/b.png"));
  QCOMPARE(inner->m_urlInLink, QStringLiteral("/a.png"));

  QCOMPARE(content.mid(outer->m_regionStart, outer->m_regionEnd - outer->m_regionStart),
           QStringLiteral("![foo ![bar](/a.png)](/b.png)"));
  QCOMPARE(content.mid(inner->m_regionStart, inner->m_regionEnd - inner->m_regionStart),
           QStringLiteral("![bar](/a.png)"));

  // Regions nest.
  QVERIFY(outer->m_regionStart <= inner->m_regionStart);
  QVERIFY(inner->m_regionEnd <= outer->m_regionEnd);

  // Destination spans do not.
  QVERIFY(inner->m_urlEnd <= outer->m_urlStart || outer->m_urlEnd <= inner->m_urlStart);
  QCOMPARE(content.mid(inner->m_urlStart, inner->m_urlEnd - inner->m_urlStart),
           QStringLiteral("/a.png"));
  QCOMPARE(content.mid(outer->m_urlStart, outer->m_urlEnd - outer->m_urlStart),
           QStringLiteral("/b.png"));
}

// A `file:` URL names an absolute location. Letting it fall through to the
// relative branch would put it in front of consumers that copy or migrate
// notebook-relative assets.
void TestMarkdownParser::testFileUrlClassification() {
  const QString content = QStringLiteral("![a](file:///tmp/notes/x.png)\n"
                                         "![b](/abs/x.png)\n"
                                         "![c](x:/drive-or-scheme.png)\n"
                                         "![d](ftp://host/x.png)\n");
  const auto links =
      MarkdownUtils::fetchImageLinks(content, QStringLiteral("/tmp/notes"), allTypes());
  QCOMPARE(links.size(), 4);

  QHash<QString, MarkdownLink> byAlt;
  for (const auto &l : links) {
    byAlt.insert(l.m_alt, l);
  }

  QVERIFY2(byAlt[QStringLiteral("a")].m_type & MarkdownLink::TypeFlag::LocalAbsolute,
           "a file: URL is absolute, not relative");
  QVERIFY(!(byAlt[QStringLiteral("a")].m_type & (MarkdownLink::TypeFlag::LocalRelativeInternal |
                                                 MarkdownLink::TypeFlag::LocalRelativeExternal)));
  QVERIFY(byAlt[QStringLiteral("b")].m_type & MarkdownLink::TypeFlag::LocalAbsolute);
  // `x:/...` is inherently ambiguous -- a Windows drive path and a legal
  // one-character URI scheme are spelled identically. VNote resolves it as a
  // drive path, which is what a user writing it means in practice.
  QVERIFY(byAlt[QStringLiteral("c")].m_type & MarkdownLink::TypeFlag::LocalAbsolute);
  QVERIFY(byAlt[QStringLiteral("d")].m_type & MarkdownLink::TypeFlag::Remote);

  // A relative-only request must not see the absolute ones at all.
  const auto relative =
      MarkdownUtils::fetchImageLinks(content, QStringLiteral("/tmp/notes"),
                                     MarkdownLink::TypeFlag::LocalRelativeInternal |
                                         MarkdownLink::TypeFlag::LocalRelativeExternal);
  QCOMPARE(relative.size(), 0);
}

// ---------------------------------------------------------------------------
// HTML `<img>`
// ---------------------------------------------------------------------------

// The scanner is the ONE place allowed to pattern-match `<img` in note source,
// so its quoting, casing and entity handling are pinned here rather than only
// through the callers.
void TestMarkdownParser::testHtmlImgScannerQuoting() {
  struct Case {
    const char *text;
    const char *src;
    const char *alt;
    int width;
    int height;
    bool unknownAttrs;
  };

  const QVector<Case> cases{
      {"<img src=\"a.png\"/>", "a.png", "", 0, 0, false},
      {"<img src='a.png'>", "a.png", "", 0, 0, false},
      {"<img src=a.png>", "a.png", "", 0, 0, false},
      {"<IMG SRC=\"a.png\" ALT=\"Hi\">", "a.png", "Hi", 0, 0, false},
      // Entities are decoded in every value.
      {"<img src=\"a&amp;b.png\" alt=\"&quot;q&quot;\">", "a&b.png", "\"q\"", 0, 0, false},
      {"<img src=\"a.png\" width=\"500\" height=\"300\" />", "a.png", "", 500, 300, false},
      // A percentage, a non-integer and a non-positive value are all "no size".
      {"<img src=\"a.png\" width=\"50%\">", "a.png", "", 0, 0, false},
      {"<img src=\"a.png\" width=\"abc\">", "a.png", "", 0, 0, false},
      {"<img src=\"a.png\" width=\"0\">", "a.png", "", 0, 0, false},
      {"<img src=\"a.png\" width=\"-5\">", "a.png", "", 0, 0, false},
      // A `>` inside a quoted value does not terminate the tag.
      {"<img src=\"a.png\" alt=\"a>b\">", "a.png", "a>b", 0, 0, false},
      {"<img src=\"a.png\" class=\"x\">", "a.png", "", 0, 0, true},
      {"<img src=\"a.png\" style=\"width:1px\">", "a.png", "", 0, 0, true},
      {"<img src=\"a.png\" data-id=\"7\">", "a.png", "", 0, 0, true},
      // A bare attribute is still an attribute.
      {"<img src=\"a.png\" hidden>", "a.png", "", 0, 0, true},
  };

  for (const auto &c : cases) {
    const QString text = QString::fromUtf8(c.text);
    vte::RawTextState state;
    const auto tags = vte::scanHtmlImgTags(text, 0, &state);
    QVERIFY2(tags.size() == 1, c.text);
    QCOMPARE(tags.first().src(), QString::fromUtf8(c.src));
    QCOMPARE(tags.first().alt(), QString::fromUtf8(c.alt));
    QCOMPARE(tags.first().width(), c.width);
    QCOMPARE(tags.first().height(), c.height);
    QCOMPARE(tags.first().hasUnknownAttrs(), c.unknownAttrs);
    // The tag span is byte-exact.
    QCOMPARE(text.mid(tags.first().m_tagStart, tags.first().m_tagEnd - tags.first().m_tagStart),
             text);
  }

  // Entity decoding uses cmark''s decoder and its complete HTML5 named-reference
  // table, so a decoded destination agrees with what the RENDERER resolves. That
  // agreement is load-bearing: obsolete-image cleanup compares decoded
  // destinations before DELETING assets, so a subset table would let a
  // still-rendered `a&copy;.png` be classified as obsolete.
  {
    struct EntityCase {
      const char *src;
      const char *decoded;
    };
    const QVector<EntityCase> entities{
        {"a&amp;b.png", "a&b.png"},
        {"a&copy;b.png", "a\xC2\xA9"
                         "b.png"}, // outside any hand-written subset
        {"a&AElig;b.png", "a\xC3\x86"
                          "b.png"},                         // upper-case name, a REAL reference
        {"a&#38;b.png", "a&b.png"},                         // decimal
        {"a&#x26;b.png", "a&b.png"},                        // hex
        {"a&notareference;b.png", "a&notareference;b.png"}, // left literal
    };

    for (const auto &c : entities) {
      const QString text = QStringLiteral("<img src=\"%1\">").arg(QString::fromUtf8(c.src));
      vte::RawTextState state;
      const auto tags = vte::scanHtmlImgTags(text, 0, &state);
      QVERIFY2(tags.size() == 1, c.src);
      QCOMPARE(tags.first().src(), QString::fromUtf8(c.decoded));
      // The SPAN still measures the source spelling, never the decoded value.
      const auto *srcAttr = tags.first().attr("src");
      QVERIFY(srcAttr);
      QCOMPARE(text.mid(srcAttr->m_valueStart, srcAttr->m_valueEnd - srcAttr->m_valueStart),
               QString::fromUtf8(c.src));
    }
  }

  // The first occurrence wins for reads, and the duplicate is still reported.
  {
    vte::RawTextState state;
    const auto tags = vte::scanHtmlImgTags(
        QStringLiteral("<img src=\"a.png\" width=\"100\" width=\"200\">"), 0, &state);
    QCOMPARE(tags.size(), 1);
    QCOMPARE(tags.first().width(), 100);
    QVERIFY(tags.first().hasDuplicateAttrs());
    QVERIFY(!tags.first().hasUnknownAttrs());
    QCOMPARE(tags.first().m_attrs.size(), 3);
  }
}

// Everything the scanner must NOT report.
void TestMarkdownParser::testHtmlImgScannerSuppression() {
  const QVector<const char *> ignored{
      // A multiline tag is out of scope by design (unchanged behaviour).
      "<img\n  src=\"a.png\">",
      "<img src=\"a.png\"\n  width=\"5\">",
      "<!-- <img src=\"a.png\"> -->",
      // No src, or an empty one.
      "<img alt=\"a\">",
      "<img src=\"\">",
      // Raw-text elements: an `<img>` there is text, not an image.
      "<script>var s = '<img src=\"a.png\">';</script>",
      "<style>/* <img src=\"a.png\"> */</style>",
      "<textarea><img src=\"a.png\"></textarea>",
      "<title><img src=\"a.png\"></title>",
      // Unclosed raw text suppresses to the end -- fail safe.
      "<script>'<img src=\"a.png\">'",
      // A tag spelled inside another tag's quoted attribute value.
      "<span title=\"<img src='a.png'>\">x</span>",
  };

  for (const char *text : ignored) {
    vte::RawTextState state;
    const auto tags = vte::scanHtmlImgTags(QString::fromUtf8(text), 0, &state);
    QVERIFY2(tags.isEmpty(), text);
  }

  // A real tag immediately after the closing raw-text tag IS found.
  {
    vte::RawTextState state;
    const auto tags = vte::scanHtmlImgTags(
        QStringLiteral("<script><img src=\"no.png\"></script><img src=\"yes.png\">"), 0, &state);
    QCOMPARE(tags.size(), 1);
    QCOMPARE(tags.first().src(), QStringLiteral("yes.png"));
    QVERIFY(state.m_element.isEmpty());
  }

  // The state is carried ACROSS calls, because cmark splits an element's
  // opening tag, contents and closing tag into separate nodes.
  {
    vte::RawTextState state;
    QVERIFY(vte::scanHtmlImgTags(QStringLiteral("<script>"), 0, &state).isEmpty());
    QCOMPARE(state.m_element, QStringLiteral("script"));
    QVERIFY(vte::scanHtmlImgTags(QStringLiteral("<img src=\"a.png\">"), 0, &state).isEmpty());
    QVERIFY(vte::scanHtmlImgTags(QStringLiteral("</SCRIPT>"), 0, &state).isEmpty());
    QVERIFY(state.m_element.isEmpty());
    const auto tags = vte::scanHtmlImgTags(QStringLiteral("<img src=\"a.png\">"), 0, &state);
    QCOMPARE(tags.size(), 1);
  }

  // Two tags on one line are both found.
  {
    vte::RawTextState state;
    const auto tags = vte::scanHtmlImgTags(
        QStringLiteral("<img src=\"a.png\"> and <img src=\"b.png\">"), 0, &state);
    QCOMPARE(tags.size(), 2);
    QCOMPARE(tags.at(0).src(), QStringLiteral("a.png"));
    QCOMPARE(tags.at(1).src(), QStringLiteral("b.png"));
  }
}

// Attribute spans are what every rewriter measures with; they must be
// byte-exact, and the base offset must be applied.
void TestMarkdownParser::testHtmlImgScannerAttrSpans() {
  const QString text = QStringLiteral("xx<img src=\"a b.png\" width=500 alt='q'>");
  const int base = 1000;
  vte::RawTextState state;
  const auto tags = vte::scanHtmlImgTags(text, base, &state);
  QCOMPARE(tags.size(), 1);

  const auto &tag = tags.first();
  QCOMPARE(tag.m_tagStart, base + 2);
  QCOMPARE(tag.m_tagEnd, base + text.size());

  const auto *src = tag.attr("src");
  QVERIFY(src);
  QCOMPARE(text.mid(src->m_attrStart - base, src->m_attrEnd - src->m_attrStart),
           QStringLiteral("src=\"a b.png\""));
  QCOMPARE(text.mid(src->m_valueStart - base, src->m_valueEnd - src->m_valueStart),
           QStringLiteral("a b.png"));
  QCOMPARE(src->m_quote, QLatin1Char('"'));

  const auto *width = tag.attr("width");
  QVERIFY(width);
  QCOMPARE(text.mid(width->m_attrStart - base, width->m_attrEnd - width->m_attrStart),
           QStringLiteral("width=500"));
  QVERIFY(width->m_quote.isNull());

  const auto *alt = tag.attr("alt");
  QVERIFY(alt);
  QCOMPARE(text.mid(alt->m_valueStart - base, alt->m_valueEnd - alt->m_valueStart),
           QStringLiteral("q"));
  QCOMPARE(alt->m_quote, QLatin1Char('\''));

  QVERIFY(!tag.attr("title"));
}

// An HTML image is a first-class entry of the snapshot API: same region/url
// span contract, same classification, same flag filtering.
void TestMarkdownParser::testFetchImageLinksHtml() {
  const QString content = QStringLiteral(
      "<img src=\"a b.png\" alt=\"the alt\" title=\"the title\" width=\"500\" height=\"300\" />\n");
  const auto links = MarkdownUtils::fetchImageLinks(content, QStringLiteral("/base"), allTypes());
  QCOMPARE(links.size(), 1);

  const auto &link = links.first();
  QCOMPARE(link.m_syntax, MarkdownLink::Syntax::Html);
  QCOMPARE(link.m_urlInLink, QStringLiteral("a b.png"));
  QCOMPARE(link.m_alt, QStringLiteral("the alt"));
  QCOMPARE(link.m_title, QStringLiteral("the title"));
  QCOMPARE(link.m_width, 500);
  QCOMPARE(link.m_height, 300);
  QVERIFY(link.m_type & MarkdownLink::TypeFlag::LocalRelativeInternal);
  QCOMPARE(content.mid(link.m_regionStart, link.m_regionEnd - link.m_regionStart),
           content.trimmed());
  // The url span is the `src` VALUE, quotes excluded.
  QVERIFY(link.hasUrlSpan());
  QCOMPARE(content.mid(link.m_urlStart, link.m_urlEnd - link.m_urlStart),
           QStringLiteral("a b.png"));

  // Entities are decoded, and the raw span still measures the source spelling.
  {
    const QString html = QStringLiteral("<img src=\"a&amp;b.png\">\n");
    const auto entity = MarkdownUtils::fetchImageLinks(html, QStringLiteral("/base"), allTypes());
    QCOMPARE(entity.size(), 1);
    QCOMPARE(entity.first().m_urlInLink, QStringLiteral("a&b.png"));
    QCOMPARE(
        html.mid(entity.first().m_urlStart, entity.first().m_urlEnd - entity.first().m_urlStart),
        QStringLiteral("a&amp;b.png"));
  }

  // Remote and absolute destinations classify exactly as Markdown ones do.
  {
    const QString html = QStringLiteral("<img src=\"https://h/x.png\">\n");
    const auto remote = MarkdownUtils::fetchImageLinks(html, QStringLiteral("/base"), allTypes());
    QCOMPARE(remote.size(), 1);
    QVERIFY(remote.first().m_type & MarkdownLink::TypeFlag::Remote);

    const auto relativeOnly = MarkdownUtils::fetchImageLinks(
        html, QStringLiteral("/base"), MarkdownLink::TypeFlag::LocalRelativeInternal);
    QVERIFY(relativeOnly.isEmpty());
  }

  // A multiline tag is invisible, exactly as before this feature existed.
  {
    const auto none = MarkdownUtils::fetchImageLinks(QStringLiteral("<img\n  src=\"a.png\">\n"),
                                                     QStringLiteral("/base"), allTypes());
    QVERIFY(none.isEmpty());
  }
}

// Container prefixes (`> `, list indent) and multiline HTML blocks: the raw
// slice keeps the prefixes, but D8 guarantees a tag never contains one, so
// every reported span must still be byte-exact.
void TestMarkdownParser::testFetchImageLinksHtmlContainers() {
  const QVector<QString> contents{
      QStringLiteral("> <img src=\"a.png\">\n"),
      QStringLiteral("- <img src=\"a.png\">\n"),
      QStringLiteral("- item\n\n  <img src=\"a.png\">\n"),
      QStringLiteral("<div>\n<img src=\"a.png\">\n</div>\n"),
      QStringLiteral("> <div>\n> <img src=\"a.png\">\n> </div>\n"),
      QStringLiteral("- <div>\n  <img src=\"a.png\">\n  </div>\n"),
      // Ending at EOF with no trailing newline.
      QStringLiteral("<div>\n<img src=\"a.png\">\n</div>"),
      // A lazy continuation: the container prefix is absent on the tag's line,
      // which shifts every reported column (D12).
      QStringLiteral("> lead\n<img src=\"a.png\">\n"),
      QStringLiteral("- lead\n<img src=\"a.png\">\n"),
      // Nested in a Markdown image's description (regions may nest).
      QStringLiteral("![d <img src=\"a.png\"> e](m.png)\n"),
  };

  for (const QString &content : contents) {
    const auto links = MarkdownUtils::fetchImageLinks(content, QStringLiteral("/base"), allTypes());
    const MarkdownLink *html = nullptr;
    for (const auto &link : links) {
      if (link.m_syntax == MarkdownLink::Syntax::Html) {
        QVERIFY2(!html, qPrintable(content));
        html = &link;
      }
    }
    QVERIFY2(html, qPrintable(content));
    QCOMPARE(content.mid(html->m_regionStart, html->m_regionEnd - html->m_regionStart),
             QStringLiteral("<img src=\"a.png\">"));
    QCOMPARE(content.mid(html->m_urlStart, html->m_urlEnd - html->m_urlStart),
             QStringLiteral("a.png"));
  }

  // Two identical tags on one line: both are reported, at distinct spans.
  {
    const QString content =
        QStringLiteral("<div>\n<img src=\"a.png\"><img src=\"a.png\">\n</div>\n");
    const auto links = MarkdownUtils::fetchImageLinks(content, QStringLiteral("/base"), allTypes());
    QCOMPARE(links.size(), 2);
    QVERIFY(links.at(0).m_regionStart != links.at(1).m_regionStart);
    for (const auto &link : links) {
      QCOMPARE(content.mid(link.m_regionStart, link.m_regionEnd - link.m_regionStart),
               QStringLiteral("<img src=\"a.png\">"));
    }
  }
}

// Raw-text suppression must hold THROUGH fetchImageLinks(), not only inside the
// scanner: cmark splits `<script>`, its contents and `</script>` into separate
// HTML nodes.
void TestMarkdownParser::testFetchImageLinksHtmlRawText() {
  const QVector<QString> suppressed{
      QStringLiteral("<script>\nvar s = '<img src=\"a.png\">';\n</script>\n"),
      QStringLiteral("<style>\n/* <img src=\"a.png\"> */\n</style>\n"),
      QStringLiteral("<textarea>\n<img src=\"a.png\">\n</textarea>\n"),
      QStringLiteral("<title>\n<img src=\"a.png\">\n</title>\n"),
      QStringLiteral("para <script>var s = '<img src=\"a.png\">';</script> tail\n"),
  };

  for (const QString &content : suppressed) {
    const auto links = MarkdownUtils::fetchImageLinks(content, QStringLiteral("/base"), allTypes());
    QVERIFY2(links.isEmpty(), qPrintable(content));
  }

  // A real image after the closing tag is still found.
  {
    const QString content =
        QStringLiteral("<script>\nvar s = '<img src=\"no.png\">';\n</script>\n\n"
                       "<img src=\"yes.png\">\n");
    const auto links = MarkdownUtils::fetchImageLinks(content, QStringLiteral("/base"), allTypes());
    QCOMPARE(links.size(), 1);
    QCOMPARE(links.first().m_urlInLink, QStringLiteral("yes.png"));
  }

  // The state must advance even for a node this walk cannot place, or an
  // unresolvable `<script>` would unmask an `<img>` inside it. A lazy
  // continuation is what makes the inline node unresolvable.
  {
    const QString content = QStringLiteral("> lead <script>\n'<img src=\"a.png\">'\n</script>\n");
    const auto links = MarkdownUtils::fetchImageLinks(content, QStringLiteral("/base"), allTypes());
    QVERIFY(links.isEmpty());
  }
}

// The T0 regression: a single-line `<img>` following a multiline construct.
// Before the cmark fix, every inline node after one carried stale coordinates.
void TestMarkdownParser::testFetchImageLinksHtmlAfterMultilineConstruct() {
  const QVector<QString> contents{
      QStringLiteral("a `co\nde` <img src=\"a.png\"> b\n"),
      QStringLiteral("a <span\nclass=\"x\">b</span> <img src=\"a.png\"> c\n"),
      QStringLiteral("> a `co\n> de` <img src=\"a.png\"> b\n"),
      QStringLiteral("- a <span\n  class=\"x\">b</span> <img src=\"a.png\"> c\n"),
  };

  for (const QString &content : contents) {
    const auto links = MarkdownUtils::fetchImageLinks(content, QStringLiteral("/base"), allTypes());
    QCOMPARE(links.size(), 1);
    QCOMPARE(content.mid(links.first().m_regionStart,
                         links.first().m_regionEnd - links.first().m_regionStart),
             QStringLiteral("<img src=\"a.png\">"));
  }
}

// The sort runs over the MERGED vector, so the descending-m_urlStart contract
// holds across syntaxes.
void TestMarkdownParser::testFetchImageLinksMixedOrdering() {
  const QString content = QStringLiteral("![a](one.png)\n"
                                         "<img src=\"two.png\">\n"
                                         "![b](three.png)\n"
                                         "<img src=\"four.png\">\n");
  const auto links = MarkdownUtils::fetchImageLinks(content, QStringLiteral("/base"), allTypes());
  QCOMPARE(links.size(), 4);

  QStringList order;
  for (int i = 0; i < links.size(); ++i) {
    order << links.at(i).m_urlInLink;
    if (i > 0) {
      QVERIFY(links.at(i - 1).m_urlStart > links.at(i).m_urlStart);
    }
  }
  QCOMPARE(order, QStringList({QStringLiteral("four.png"), QStringLiteral("three.png"),
                               QStringLiteral("two.png"), QStringLiteral("one.png")}));

  QCOMPARE(links.at(0).m_syntax, MarkdownLink::Syntax::Html);
  QCOMPARE(links.at(1).m_syntax, MarkdownLink::Syntax::Markdown);
}

// The live path: the walker reports an HTML image exactly as it reports a
// Markdown one, so PreviewMgr and the editor's Image menu need no branch.
void TestMarkdownParser::testWalkerHtmlImages() {
  {
    const QString input =
        QStringLiteral("<img src=\"a.png\" alt=\"A\" title=\"T\" width=\"200\"/>\n");
    const auto result = parse(input);
    QCOMPARE(result.imageElements.size(), 1);

    const auto &image = result.imageElements.first();
    QCOMPARE(image.m_syntax, vte::md::ImageLinkInfo::Syntax::Html);
    QCOMPARE(image.m_destination, QStringLiteral("a.png"));
    QCOMPARE(image.m_alternateText, QStringLiteral("A"));
    QCOMPARE(image.m_title, QStringLiteral("T"));
    QCOMPARE(image.m_width, 200);
    QCOMPARE(image.m_height, 0);
    QCOMPARE(input.mid(image.m_startPos, image.m_endPos - image.m_startPos), input.trimmed());
    // Sole content of its line.
    QVERIFY(image.m_standalone);

    const auto links = vte::md::buildImageLinks(result.imageElements);
    QCOMPARE(links.size(), 1);
    QCOMPARE(links.first().m_syntax, vte::md::ImageLinkInfo::Syntax::Html);
    QCOMPARE(links.first().m_alt, QStringLiteral("A"));
    QCOMPARE(links.first().m_title, QStringLiteral("T"));
    QCOMPARE(links.first().m_region.m_startPos, image.m_startPos);
    QCOMPARE(links.first().m_region.m_endPos, image.m_endPos);
  }

  // Mid-sentence: not standalone.
  {
    const auto result = parse(QStringLiteral("text <img src=\"a.png\"> more\n"));
    QCOMPARE(result.imageElements.size(), 1);
    QVERIFY(!result.imageElements.first().m_standalone);
  }

  // Inside a multiline HTML block, on its own line.
  {
    const QString input = QStringLiteral("<div>\n<img src=\"a.png\">\n</div>\n");
    const auto result = parse(input);
    QCOMPARE(result.imageElements.size(), 1);
    QVERIFY(result.imageElements.first().m_standalone);
    QCOMPARE(
        input.mid(result.imageElements.first().m_startPos,
                  result.imageElements.first().m_endPos - result.imageElements.first().m_startPos),
        QStringLiteral("<img src=\"a.png\">"));
  }

  // Raw-text suppression holds in the live path too.
  {
    const auto result =
        parse(QStringLiteral("<script>\nvar s = '<img src=\"a.png\">';\n</script>\n"));
    QVERIFY(result.imageElements.isEmpty());
  }

  // The T0 regression, through the walker.
  {
    const QString input = QStringLiteral("a `co\nde` <img src=\"a.png\"> b\n");
    const auto result = parse(input);
    QCOMPARE(result.imageElements.size(), 1);
    QCOMPARE(
        input.mid(result.imageElements.first().m_startPos,
                  result.imageElements.first().m_endPos - result.imageElements.first().m_startPos),
        QStringLiteral("<img src=\"a.png\">"));
  }
}

// Generation is the inverse of the scanner, and the 3-argument
// generateImageLink() must stay byte-identical for untouched call sites.
void TestMarkdownParser::testGenerateImageTag() {
  QCOMPARE(
      MarkdownUtils::generateImageLink(QStringLiteral("alt"), QStringLiteral("a.png"), QString()),
      QStringLiteral("![alt](a.png)"));
  QCOMPARE(MarkdownUtils::generateImageLink(QStringLiteral("alt"), QStringLiteral("a.png"),
                                            QStringLiteral("title")),
           QStringLiteral("![alt](a.png \"title\")"));
  QCOMPARE(MarkdownUtils::generateImageLink(QStringLiteral("alt"), QStringLiteral("a.png"),
                                            QString(), 0, 0),
           QStringLiteral("![alt](a.png)"));

  // Any size at all switches to HTML, which every Markdown tool understands.
  QCOMPARE(MarkdownUtils::generateImageLink(QString(), QStringLiteral("a.png"), QString(), 500, 0),
           QStringLiteral("<img src=\"a.png\" width=\"500\" />"));
  QCOMPARE(MarkdownUtils::generateImageLink(QStringLiteral("alt"), QStringLiteral("a.png"),
                                            QStringLiteral("title"), 500, 300),
           QStringLiteral(
               "<img src=\"a.png\" alt=\"alt\" title=\"title\" width=\"500\" height=\"300\" />"));

  // Every value is escaped, so the result always round trips through the
  // scanner unchanged.
  const QString tag = MarkdownUtils::generateImageTag(
      QStringLiteral("a\"b<c"), QStringLiteral("a&b.png"), QStringLiteral("t'x"), 10, 20);
  QCOMPARE(tag, QStringLiteral("<img src=\"a&amp;b.png\" alt=\"a&quot;b&lt;c\" title=\"t&#39;x\" "
                               "width=\"10\" height=\"20\" />"));

  vte::RawTextState state;
  const auto tags = vte::scanHtmlImgTags(tag, 0, &state);
  QCOMPARE(tags.size(), 1);
  QCOMPARE(tags.first().src(), QStringLiteral("a&b.png"));
  QCOMPARE(tags.first().alt(), QStringLiteral("a\"b<c"));
  QCOMPARE(tags.first().title(), QStringLiteral("t'x"));
  QCOMPARE(tags.first().width(), 10);
  QCOMPARE(tags.first().height(), 20);
  QVERIFY(!tags.first().hasUnknownAttrs());
}

QTEST_MAIN(tests::TestMarkdownParser)
