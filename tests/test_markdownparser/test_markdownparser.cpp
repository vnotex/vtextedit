#include "test_markdownparser.h"

#include <QFile>
#include <QElapsedTimer>
#include <algorithm>

#include "markdownastwalker.h"

using namespace tests;

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
static int countBlocks(const QByteArray &p_utf8)
{
  int n = 1;
  for (int i = 0; i < p_utf8.size(); ++i) {
    if (p_utf8[i] == '\n') ++n;
  }
  return n;
}

// Helper: parse text and return ASTWalkResult.
static vte::md::ASTWalkResult parse(const QString &p_text)
{
  QByteArray utf8 = p_text.toUtf8();
  int numBlocks = countBlocks(utf8);
  return vte::md::walkAndConvert(utf8, numBlocks);
}

// Helper: count HLUnits with given style across all blocks.
static int countElements(const vte::md::ASTWalkResult &p_result, int p_style)
{
  int count = 0;
  for (const auto &block : p_result.blocksHighlights) {
    for (const auto &unit : block) {
      if (unit.styleIndex == (unsigned int)p_style) ++count;
    }
  }
  return count;
}

// Helper: find all HLUnits with given style, returning doc-absolute (start, end) pairs.
// Elements are returned in the order they appear in blocksHighlights (block order, then
// unit order within block). The old parseCmark prepended elements (reverse order within
// a style bucket). We sort by position ascending here for consistent comparison.
static QVector<QPair<unsigned long, unsigned long>> findElements(
    const vte::md::ASTWalkResult &p_result, int p_style, const QString &p_text)
{
  QVector<QPair<unsigned long, unsigned long>> elems;
  // Build block start positions from text.
  QVector<int> blockStarts;
  blockStarts.append(0);
  for (int i = 0; i < p_text.size(); ++i) {
    if (p_text[i] == '\n') blockStarts.append(i + 1);
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

static QString readFixture(const QString &p_name)
{
  QFile f(QStringLiteral(FIXTURES_DIR) + "/" + p_name);
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return QString();
  }
  return QString::fromUtf8(f.readAll());
}

void TestMarkdownParser::initTestCase()
{
}

void TestMarkdownParser::cleanupTestCase()
{
}

// ============================================================
// T5: Block Element Tests
// ============================================================

void TestMarkdownParser::testHeadings()
{
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

void TestMarkdownParser::testBlockquotes()
{
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

void TestMarkdownParser::testHorizontalRules()
{
  // Use *** instead of --- to avoid cmark frontmatter extension consuming it.
  const QString input = QStringLiteral("***\n");
  auto result = parse(input);

  QCOMPARE(countElements(result, HLT_HRULE), 1);

  QCOMPARE(result.hruleRegions.size(), 1);
  QCOMPARE(result.hruleRegions[0].m_startPos, 0);
  QCOMPARE(result.hruleRegions[0].m_endPos, 3);
}

void TestMarkdownParser::testFencedCodeBlocks()
{
  const QString input = QStringLiteral("```cpp\ncode here\n```\n");
  auto result = parse(input);

  // Walker produces 1 HLUnit per block line (3 lines = 3 units). Use region for logical count.
  QCOMPARE(countElements(result, HLT_FENCEDCODEBLOCK), 3);

  QCOMPARE(result.codeBlockRegions.size(), 1);
  auto it = result.codeBlockRegions.constBegin();
  QCOMPARE(it.value().m_startPos, 0);
  QCOMPARE(it.value().m_endPos, 20);
}

void TestMarkdownParser::testIndentedCodeBlocks()
{
  const QString input = QStringLiteral("    indented code\n");
  auto result = parse(input);

  QCOMPARE(countElements(result, HLT_VERBATIM), 1);

  auto elems = findElements(result, HLT_VERBATIM, input);
  QVERIFY(!elems.isEmpty());
  // cmark starts indented code block position at content (after indent).
  QCOMPARE((int)elems[0].first, 4);
  QVERIFY((int)elems[0].second > 4);
}

void TestMarkdownParser::testHTMLBlocks()
{
  const QString input = QStringLiteral("<div>\nhtml content\n</div>\n");
  auto result = parse(input);

  // Walker produces 1 HLUnit per block line (3 lines = 3 units).
  QCOMPARE(countElements(result, HLT_HTMLBLOCK), 3);

  auto elems = findElements(result, HLT_HTMLBLOCK, input);
  QVERIFY(!elems.isEmpty());
  QCOMPARE((int)elems[0].first, 0);
  QVERIFY((int)elems[0].second > 0);
}

void TestMarkdownParser::testLists()
{
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

void TestMarkdownParser::testFrontmatter()
{
  const QString input = QStringLiteral("---\ntitle: test\n---\n\nContent\n");
  auto result = parse(input);

  // Walker produces 1 HLUnit per block line (3 lines = 3 units).
  QCOMPARE(countElements(result, HLT_FRONTMATTER), 3);

  auto elems = findElements(result, HLT_FRONTMATTER, input);
  QVERIFY(!elems.isEmpty());
  QCOMPARE((int)elems[0].first, 0);
  QVERIFY((int)elems[0].second > 0);
}

void TestMarkdownParser::testDisplayFormula()
{
  const QString input = QStringLiteral("$$\nE = mc^2\n$$\n");
  auto result = parse(input);

  // Walker produces 1 HLUnit per block line (3 lines = 3 units). Use region for logical count.
  QCOMPARE(countElements(result, HLT_DISPLAYFORMULA), 3);

  QCOMPARE(result.displayFormulaRegions.size(), 1);
  QCOMPARE(result.displayFormulaRegions[0].m_startPos, 0);
  QVERIFY(result.displayFormulaRegions[0].m_endPos > 0);
}

void TestMarkdownParser::testTables()
{
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

void TestMarkdownParser::testEmphasis()
{
  const QString input = QStringLiteral("*emph*\n");
  auto result = parse(input);

  QCOMPARE(countElements(result, HLT_EMPH), 1);

  auto elems = findElements(result, HLT_EMPH, input);
  QVERIFY(!elems.isEmpty());
  QCOMPARE((int)elems[0].first, 0);
  QCOMPARE((int)elems[0].second, 6);
}

void TestMarkdownParser::testStrong()
{
  const QString input = QStringLiteral("**strong**\n");
  auto result = parse(input);

  QCOMPARE(countElements(result, HLT_STRONG), 1);

  auto elems = findElements(result, HLT_STRONG, input);
  QVERIFY(!elems.isEmpty());
  QCOMPARE((int)elems[0].first, 0);
  QCOMPARE((int)elems[0].second, 10);
}

void TestMarkdownParser::testInlineCode()
{
  const QString input = QStringLiteral("`code`\n");
  auto result = parse(input);

  QCOMPARE(countElements(result, HLT_CODE), 1);

  auto elems = findElements(result, HLT_CODE, input);
  QVERIFY(!elems.isEmpty());
  QCOMPARE((int)elems[0].first, 0);
  QCOMPARE((int)elems[0].second, 6);
}

void TestMarkdownParser::testLinks()
{
  const QString input = QStringLiteral("[text](url)\n");
  auto result = parse(input);

  QCOMPARE(countElements(result, HLT_LINK), 1);

  auto elems = findElements(result, HLT_LINK, input);
  QVERIFY(!elems.isEmpty());
  QCOMPARE((int)elems[0].first, 0);
  QCOMPARE((int)elems[0].second, 11);
}

void TestMarkdownParser::testAutoLinks()
{
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

void TestMarkdownParser::testImages()
{
  const QString input = QStringLiteral("![alt](img.png)\n");
  auto result = parse(input);

  QCOMPARE(countElements(result, HLT_IMAGE), 1);

  QCOMPARE(result.imageRegions.size(), 1);
  QCOMPARE(result.imageRegions[0].m_startPos, 0);
  QCOMPARE(result.imageRegions[0].m_endPos, 15);
}

void TestMarkdownParser::testHTMLInline()
{
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

void TestMarkdownParser::testHTMLEntities()
{
  const QString input = QStringLiteral("&amp; &lt;\n");
  auto result = parse(input);

  // cmark does not produce HTML_ENTITY elements.
  QCOMPARE(countElements(result, HLT_HTML_ENTITY), 0);
}

void TestMarkdownParser::testComments()
{
  const QString input = QStringLiteral("<!-- comment -->\n");
  auto result = parse(input);

  // cmark maps HTML comments to HTMLBLOCK, not COMMENT.
  QCOMPARE(countElements(result, HLT_COMMENT), 0);
  QCOMPARE(countElements(result, HLT_HTMLBLOCK), 1);
}

void TestMarkdownParser::testReferences()
{
  const QString input = QStringLiteral("[id]: http://example.com\n\n[text][id]\n");
  auto result = parse(input);

  // cmark resolves references during parsing — no REFERENCE elements.
  QCOMPARE(countElements(result, HLT_REFERENCE), 0);
  // The link reference [text][id] should produce a LINK element.
  QCOMPARE(countElements(result, HLT_LINK), 1);
}

void TestMarkdownParser::testStrikethrough()
{
  const QString input = QStringLiteral("~~strike~~\n");
  auto result = parse(input);

  QCOMPARE(countElements(result, HLT_STRIKE), 1);

  auto elems = findElements(result, HLT_STRIKE, input);
  QVERIFY(!elems.isEmpty());
  QCOMPARE((int)elems[0].first, 0);
  QCOMPARE((int)elems[0].second, 10);
}

void TestMarkdownParser::testMark()
{
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

void TestMarkdownParser::testFootnotes()
{
  const QString input = QStringLiteral("[^1]: footnote\n\nText [^1]\n");
  auto result = parse(input);

  // Walker produces HLUnits for footnote definition and reference.
  // "[^1]: footnote\n" is 1 block, "\n" is empty, "Text [^1]\n" has 1 reference.
  // The walker may produce more units depending on how footnote nodes are structured.
  QVERIFY(countElements(result, HLT_NOTE) >= 2);
}

void TestMarkdownParser::testInlineEquation()
{
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

void TestMarkdownParser::testSurrogatePairs()
{
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

void TestMarkdownParser::testEmptyElements()
{
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

void TestMarkdownParser::testUnclosedDelimiters()
{
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

void TestMarkdownParser::testDegenerate()
{
  // Empty string — walkAndConvert returns empty result (no crash).
  {
    const QString input = QStringLiteral("");
    QByteArray utf8 = input.toUtf8();
    int numBlocks = countBlocks(utf8);
    auto result = vte::md::walkAndConvert(utf8, numBlocks);
    // Empty result — no highlights.
    bool hasAny = false;
    for (const auto &block : result.blocksHighlights) {
      if (!block.isEmpty()) { hasAny = true; break; }
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

void TestMarkdownParser::testNestedOverlap()
{
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

void TestMarkdownParser::testAllExtensions()
{
  const QString input = QStringLiteral(
      "---\ntitle: test\n---\n\n"
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

void TestMarkdownParser::testPerformance()
{
  // Generate a 1000-line markdown document with mixed content.
  QString doc;
  doc.reserve(64000);
  for (int i = 0; i < 100; i++) {
    doc += QString("# Heading %1\n\n").arg(i);
    doc += QString("## Sub-heading %1\n\n").arg(i);
    doc += QString("### Third level %1\n\n").arg(i);
    doc += QString(
        "Paragraph with *emph*, **strong**, `code`, ~~strike~~ and $E=mc^2$ inline.\n\n");
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
      if (!block.isEmpty()) { hasAny = true; break; }
    }
    QVERIFY(hasAny);
    times.append(elapsed);
  }

  std::sort(times.begin(), times.end());
  qint64 median = times[1];
  qDebug() << "walkAndConvert parse times (ms):" << times[0] << times[1] << times[2]
           << "median:" << median;

  QVERIFY2(median < 500,
           qPrintable(
               QString("Median parse time %1ms exceeds 500ms threshold").arg(median)));
}

QTEST_MAIN(tests::TestMarkdownParser)
