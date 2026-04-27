#include "test_markdownparser.h"

#include <QFile>
#include <QElapsedTimer>
#include <algorithm>

#include "cmarkadapter.h"
#include "highlightelement.h"

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

static int countElements(HighlightElement **p_result, int p_type)
{
  int count = 0;
  HighlightElement *elem = p_result[p_type];
  while (elem) {
    count++;
    elem = elem->next;
  }
  return count;
}

static HighlightElement *firstElement(HighlightElement **p_result, int p_type)
{
  return p_result[p_type];
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
  HighlightElement **result = parseCmark(input.toUtf8());
  QVERIFY(result != NULL);

  QCOMPARE(countElements(result, HLT_H1), 1);
  QCOMPARE(countElements(result, HLT_H2), 1);
  QCOMPARE(countElements(result, HLT_H3), 1);
  QCOMPARE(countElements(result, HLT_H4), 1);
  QCOMPARE(countElements(result, HLT_H5), 1);
  QCOMPARE(countElements(result, HLT_H6), 1);

  // cmark heading positions exclude trailing newline (unlike pmh).
  HighlightElement *h1 = firstElement(result, HLT_H1);
  QVERIFY(h1 != NULL);
  QCOMPARE((int)h1->pos, 0);
  QCOMPARE((int)h1->end, 4);

  HighlightElement *h2 = firstElement(result, HLT_H2);
  QVERIFY(h2 != NULL);
  QCOMPARE((int)h2->pos, 5);
  QCOMPARE((int)h2->end, 10);

  HighlightElement *h3 = firstElement(result, HLT_H3);
  QVERIFY(h3 != NULL);
  QCOMPARE((int)h3->pos, 11);
  QCOMPARE((int)h3->end, 17);

  HighlightElement *h4 = firstElement(result, HLT_H4);
  QVERIFY(h4 != NULL);
  QCOMPARE((int)h4->pos, 18);
  QCOMPARE((int)h4->end, 25);

  HighlightElement *h5 = firstElement(result, HLT_H5);
  QVERIFY(h5 != NULL);
  QCOMPARE((int)h5->pos, 26);
  QCOMPARE((int)h5->end, 34);

  HighlightElement *h6 = firstElement(result, HLT_H6);
  QVERIFY(h6 != NULL);
  QCOMPARE((int)h6->pos, 35);
  QCOMPARE((int)h6->end, 44);

  freeHighlightElements(result, NUM_HIGHLIGHT_STYLES);
}

void TestMarkdownParser::testBlockquotes()
{
  const QString input = QStringLiteral("> quoted text\n> more quoted\n");
  HighlightElement **result = parseCmark(input.toUtf8());
  QVERIFY(result != NULL);

  // cmark produces 1 BLOCKQUOTE element per block (pmh produced 1 per > marker).
  QCOMPARE(countElements(result, HLT_BLOCKQUOTE), 1);

  HighlightElement *bq = firstElement(result, HLT_BLOCKQUOTE);
  QVERIFY(bq != NULL);
  QCOMPARE((int)bq->pos, 0);
  QVERIFY((int)bq->end > 0);

  freeHighlightElements(result, NUM_HIGHLIGHT_STYLES);
}

void TestMarkdownParser::testHorizontalRules()
{
  // Use *** instead of --- to avoid cmark frontmatter extension consuming it.
  const QString input = QStringLiteral("***\n");
  HighlightElement **result = parseCmark(input.toUtf8());
  QVERIFY(result != NULL);

  QCOMPARE(countElements(result, HLT_HRULE), 1);

  HighlightElement *hr = firstElement(result, HLT_HRULE);
  QVERIFY(hr != NULL);
  QCOMPARE((int)hr->pos, 0);
  QCOMPARE((int)hr->end, 3);

  freeHighlightElements(result, NUM_HIGHLIGHT_STYLES);
}

void TestMarkdownParser::testFencedCodeBlocks()
{
  const QString input = QStringLiteral("```cpp\ncode here\n```\n");
  HighlightElement **result = parseCmark(input.toUtf8());
  QVERIFY(result != NULL);

  QCOMPARE(countElements(result, HLT_FENCEDCODEBLOCK), 1);

  HighlightElement *fcb = firstElement(result, HLT_FENCEDCODEBLOCK);
  QVERIFY(fcb != NULL);
  QCOMPARE((int)fcb->pos, 0);
  QCOMPARE((int)fcb->end, 20);

  freeHighlightElements(result, NUM_HIGHLIGHT_STYLES);
}

void TestMarkdownParser::testIndentedCodeBlocks()
{
  const QString input = QStringLiteral("    indented code\n");
  HighlightElement **result = parseCmark(input.toUtf8());
  QVERIFY(result != NULL);

  QCOMPARE(countElements(result, HLT_VERBATIM), 1);

  HighlightElement *v = firstElement(result, HLT_VERBATIM);
  QVERIFY(v != NULL);
  // cmark starts indented code block position at content (after indent).
  QCOMPARE((int)v->pos, 4);
  QVERIFY((int)v->end > 4);

  freeHighlightElements(result, NUM_HIGHLIGHT_STYLES);
}

void TestMarkdownParser::testHTMLBlocks()
{
  const QString input = QStringLiteral("<div>\nhtml content\n</div>\n");
  HighlightElement **result = parseCmark(input.toUtf8());
  QVERIFY(result != NULL);

  QCOMPARE(countElements(result, HLT_HTMLBLOCK), 1);

  HighlightElement *hb = firstElement(result, HLT_HTMLBLOCK);
  QVERIFY(hb != NULL);
  QCOMPARE((int)hb->pos, 0);
  QVERIFY((int)hb->end > 0);

  freeHighlightElements(result, NUM_HIGHLIGHT_STYLES);
}

void TestMarkdownParser::testLists()
{
  const QString input = QStringLiteral("- item 1\n- item 2\n\n1. first\n2. second\n");
  HighlightElement **result = parseCmark(input.toUtf8());
  QVERIFY(result != NULL);

  QCOMPARE(countElements(result, HLT_LIST_BULLET), 2);
  QCOMPARE(countElements(result, HLT_LIST_ENUMERATOR), 2);

  // Bullets in reverse order (prepended, same as pmh).
  HighlightElement *b0 = firstElement(result, HLT_LIST_BULLET);
  QVERIFY(b0 != NULL);
  QCOMPARE((int)b0->pos, 9);
  QCOMPARE((int)b0->end, 10);

  HighlightElement *b1 = b0->next;
  QVERIFY(b1 != NULL);
  QCOMPARE((int)b1->pos, 0);
  QCOMPARE((int)b1->end, 1);

  // Enumerators in reverse order.
  HighlightElement *e0 = firstElement(result, HLT_LIST_ENUMERATOR);
  QVERIFY(e0 != NULL);
  QCOMPARE((int)e0->pos, 28);
  QCOMPARE((int)e0->end, 30);

  HighlightElement *e1 = e0->next;
  QVERIFY(e1 != NULL);
  QCOMPARE((int)e1->pos, 19);
  QCOMPARE((int)e1->end, 21);

  freeHighlightElements(result, NUM_HIGHLIGHT_STYLES);
}

void TestMarkdownParser::testFrontmatter()
{
  const QString input = QStringLiteral("---\ntitle: test\n---\n\nContent\n");
  HighlightElement **result = parseCmark(input.toUtf8());
  QVERIFY(result != NULL);

  QCOMPARE(countElements(result, HLT_FRONTMATTER), 1);

  HighlightElement *fm = firstElement(result, HLT_FRONTMATTER);
  QVERIFY(fm != NULL);
  QCOMPARE((int)fm->pos, 0);
  QVERIFY((int)fm->end > 0);

  freeHighlightElements(result, NUM_HIGHLIGHT_STYLES);
}

void TestMarkdownParser::testDisplayFormula()
{
  const QString input = QStringLiteral("$$\nE = mc^2\n$$\n");
  HighlightElement **result = parseCmark(input.toUtf8());
  QVERIFY(result != NULL);

  QCOMPARE(countElements(result, HLT_DISPLAYFORMULA), 1);

  HighlightElement *df = firstElement(result, HLT_DISPLAYFORMULA);
  QVERIFY(df != NULL);
  QCOMPARE((int)df->pos, 0);
  QVERIFY((int)df->end > 0);

  freeHighlightElements(result, NUM_HIGHLIGHT_STYLES);
}

void TestMarkdownParser::testTables()
{
  const QString input = QStringLiteral("| h1 | h2 |\n|---|---|\n| a | b |\n");
  HighlightElement **result = parseCmark(input.toUtf8());
  QVERIFY(result != NULL);

  QCOMPARE(countElements(result, HLT_TABLE), 1);
  QCOMPARE(countElements(result, HLT_TABLEHEADER), 1);

  HighlightElement *tbl = firstElement(result, HLT_TABLE);
  QVERIFY(tbl != NULL);
  QCOMPARE((int)tbl->pos, 0);
  QVERIFY((int)tbl->end > 0);

  HighlightElement *th = firstElement(result, HLT_TABLEHEADER);
  QVERIFY(th != NULL);
  QCOMPARE((int)th->pos, 0);
  QVERIFY((int)th->end > 0);

  freeHighlightElements(result, NUM_HIGHLIGHT_STYLES);
}

// ============================================================
// T6: Inline Element Tests
// ============================================================

void TestMarkdownParser::testEmphasis()
{
  const QString input = QStringLiteral("*emph*\n");
  HighlightElement **result = parseCmark(input.toUtf8());
  QVERIFY(result != NULL);

  QCOMPARE(countElements(result, HLT_EMPH), 1);

  HighlightElement *em = firstElement(result, HLT_EMPH);
  QVERIFY(em != NULL);
  QCOMPARE((int)em->pos, 0);
  QCOMPARE((int)em->end, 6);

  freeHighlightElements(result, NUM_HIGHLIGHT_STYLES);
}

void TestMarkdownParser::testStrong()
{
  const QString input = QStringLiteral("**strong**\n");
  HighlightElement **result = parseCmark(input.toUtf8());
  QVERIFY(result != NULL);

  QCOMPARE(countElements(result, HLT_STRONG), 1);

  HighlightElement *s = firstElement(result, HLT_STRONG);
  QVERIFY(s != NULL);
  QCOMPARE((int)s->pos, 0);
  QCOMPARE((int)s->end, 10);

  freeHighlightElements(result, NUM_HIGHLIGHT_STYLES);
}

void TestMarkdownParser::testInlineCode()
{
  const QString input = QStringLiteral("`code`\n");
  HighlightElement **result = parseCmark(input.toUtf8());
  QVERIFY(result != NULL);

  QCOMPARE(countElements(result, HLT_CODE), 1);

  HighlightElement *c = firstElement(result, HLT_CODE);
  QVERIFY(c != NULL);
  QCOMPARE((int)c->pos, 0);
  QCOMPARE((int)c->end, 6);

  freeHighlightElements(result, NUM_HIGHLIGHT_STYLES);
}

void TestMarkdownParser::testLinks()
{
  const QString input = QStringLiteral("[text](url)\n");
  HighlightElement **result = parseCmark(input.toUtf8());
  QVERIFY(result != NULL);

  QCOMPARE(countElements(result, HLT_LINK), 1);

  HighlightElement *lnk = firstElement(result, HLT_LINK);
  QVERIFY(lnk != NULL);
  QCOMPARE((int)lnk->pos, 0);
  QCOMPARE((int)lnk->end, 11);

  freeHighlightElements(result, NUM_HIGHLIGHT_STYLES);
}

void TestMarkdownParser::testAutoLinks()
{
  // URL auto link — cmark maps to LINK (not AUTO_LINK_URL). Count = 1.
  {
    const QString input = QStringLiteral("<http://example.com>\n");
    HighlightElement **result = parseCmark(input.toUtf8());
    QVERIFY(result != NULL);

    QCOMPARE(countElements(result, HLT_AUTO_LINK_URL), 0);
    QCOMPARE(countElements(result, HLT_LINK), 1);

    HighlightElement *al = firstElement(result, HLT_LINK);
    QVERIFY(al != NULL);
    QCOMPARE((int)al->pos, 0);
    QCOMPARE((int)al->end, 20);

    freeHighlightElements(result, NUM_HIGHLIGHT_STYLES);
  }

  // Email auto link — cmark produces 1 AUTO_LINK_EMAIL (pmh produced 2 duplicates).
  {
    const QString input = QStringLiteral("<user@example.com>\n");
    HighlightElement **result = parseCmark(input.toUtf8());
    QVERIFY(result != NULL);

    QCOMPARE(countElements(result, HLT_AUTO_LINK_EMAIL), 1);

    HighlightElement *ae = firstElement(result, HLT_AUTO_LINK_EMAIL);
    QVERIFY(ae != NULL);
    QCOMPARE((int)ae->pos, 0);
    QCOMPARE((int)ae->end, 18);

    freeHighlightElements(result, NUM_HIGHLIGHT_STYLES);
  }
}

void TestMarkdownParser::testImages()
{
  const QString input = QStringLiteral("![alt](img.png)\n");
  HighlightElement **result = parseCmark(input.toUtf8());
  QVERIFY(result != NULL);

  QCOMPARE(countElements(result, HLT_IMAGE), 1);

  HighlightElement *img = firstElement(result, HLT_IMAGE);
  QVERIFY(img != NULL);
  QCOMPARE((int)img->pos, 0);
  QCOMPARE((int)img->end, 15);

  freeHighlightElements(result, NUM_HIGHLIGHT_STYLES);
}

void TestMarkdownParser::testHTMLInline()
{
  const QString input = QStringLiteral("text <span>html</span> text\n");
  HighlightElement **result = parseCmark(input.toUtf8());
  QVERIFY(result != NULL);

  // cmark produces 2 HTML_INLINE elements for <span> and </span>.
  QCOMPARE(countElements(result, HLT_HTML), 2);

  // Reverse order (prepended): </span> first, <span> second.
  HighlightElement *h0 = firstElement(result, HLT_HTML);
  QVERIFY(h0 != NULL);
  QCOMPARE((int)h0->pos, 15);
  QCOMPARE((int)h0->end, 22);

  HighlightElement *h1 = h0->next;
  QVERIFY(h1 != NULL);
  QCOMPARE((int)h1->pos, 5);
  QCOMPARE((int)h1->end, 11);

  freeHighlightElements(result, NUM_HIGHLIGHT_STYLES);
}

void TestMarkdownParser::testHTMLEntities()
{
  const QString input = QStringLiteral("&amp; &lt;\n");
  HighlightElement **result = parseCmark(input.toUtf8());
  QVERIFY(result != NULL);

  // cmark does not produce HTML_ENTITY elements.
  QCOMPARE(countElements(result, HLT_HTML_ENTITY), 0);

  freeHighlightElements(result, NUM_HIGHLIGHT_STYLES);
}

void TestMarkdownParser::testComments()
{
  const QString input = QStringLiteral("<!-- comment -->\n");
  HighlightElement **result = parseCmark(input.toUtf8());
  QVERIFY(result != NULL);

  // cmark maps HTML comments to HTMLBLOCK, not COMMENT.
  QCOMPARE(countElements(result, HLT_COMMENT), 0);
  QCOMPARE(countElements(result, HLT_HTMLBLOCK), 1);

  freeHighlightElements(result, NUM_HIGHLIGHT_STYLES);
}

void TestMarkdownParser::testReferences()
{
  const QString input = QStringLiteral("[id]: http://example.com\n\n[text][id]\n");
  HighlightElement **result = parseCmark(input.toUtf8());
  QVERIFY(result != NULL);

  // cmark resolves references during parsing — no REFERENCE elements.
  QCOMPARE(countElements(result, HLT_REFERENCE), 0);
  // The link reference [text][id] should produce a LINK element.
  QCOMPARE(countElements(result, HLT_LINK), 1);

  freeHighlightElements(result, NUM_HIGHLIGHT_STYLES);
}

void TestMarkdownParser::testStrikethrough()
{
  const QString input = QStringLiteral("~~strike~~\n");
  HighlightElement **result = parseCmark(input.toUtf8());
  QVERIFY(result != NULL);

  QCOMPARE(countElements(result, HLT_STRIKE), 1);

  HighlightElement *s = firstElement(result, HLT_STRIKE);
  QVERIFY(s != NULL);
  QCOMPARE((int)s->pos, 0);
  QCOMPARE((int)s->end, 10);

  freeHighlightElements(result, NUM_HIGHLIGHT_STYLES);
}

void TestMarkdownParser::testMark()
{
  const QString input = QStringLiteral("==marked==\n");
  HighlightElement **result = parseCmark(input.toUtf8());
  QVERIFY(result != NULL);

  // cmark produces MARK elements (pmh did not).
  int markCount = countElements(result, HLT_MARK);
  qDebug() << "MARK count:" << markCount;
  QVERIFY(markCount >= 1);

  HighlightElement *m = firstElement(result, HLT_MARK);
  QVERIFY(m != NULL);
  QCOMPARE((int)m->pos, 0);
  QCOMPARE((int)m->end, 10);

  freeHighlightElements(result, NUM_HIGHLIGHT_STYLES);
}

void TestMarkdownParser::testFootnotes()
{
  const QString input = QStringLiteral("[^1]: footnote\n\nText [^1]\n");
  HighlightElement **result = parseCmark(input.toUtf8());
  QVERIFY(result != NULL);

  // cmark produces NOTE elements for footnote definition and reference.
  QCOMPARE(countElements(result, HLT_NOTE), 2);

  freeHighlightElements(result, NUM_HIGHLIGHT_STYLES);
}

void TestMarkdownParser::testInlineEquation()
{
  const QString input = QStringLiteral("$E=mc^2$\n");
  HighlightElement **result = parseCmark(input.toUtf8());
  QVERIFY(result != NULL);

  QCOMPARE(countElements(result, HLT_INLINEEQUATION), 1);

  HighlightElement *eq = firstElement(result, HLT_INLINEEQUATION);
  QVERIFY(eq != NULL);
  // cmark adapter adjusts FORMULA_INLINE to re-include $ delimiters.
  QCOMPARE((int)eq->pos, 0);
  QCOMPARE((int)eq->end, 8);

  freeHighlightElements(result, NUM_HIGHLIGHT_STYLES);
}

// ============================================================
// T7: Edge Case Tests
// ============================================================

void TestMarkdownParser::testSurrogatePairs()
{
  // U+1F389 is 4 UTF-8 bytes -> 2 QChars (surrogate pair).
  // cmark adapter uses QChar offsets via LineOffsetTable.
  const QString input = QString::fromUtf8("# \xF0\x9F\x8E\x89 Hello\n");
  HighlightElement **result = parseCmark(input.toUtf8());
  QVERIFY(result != NULL);

  QCOMPARE(countElements(result, HLT_H1), 1);

  HighlightElement *h1 = firstElement(result, HLT_H1);
  QVERIFY(h1 != NULL);
  qDebug() << "Surrogate H1 pos:" << h1->pos << "end:" << h1->end;
  QCOMPARE((int)h1->pos, 0);
  QCOMPARE((int)h1->end, 10);

  freeHighlightElements(result, NUM_HIGHLIGHT_STYLES);
}

void TestMarkdownParser::testEmptyElements()
{
  // Empty bold: **** — no STRONG or EMPH.
  {
    const QString input = QStringLiteral("****\n");
    HighlightElement **result = parseCmark(input.toUtf8());
    QVERIFY(result != NULL);

    QCOMPARE(countElements(result, HLT_STRONG), 0);
    QCOMPARE(countElements(result, HLT_EMPH), 0);

    freeHighlightElements(result, NUM_HIGHLIGHT_STYLES);
  }

  // Empty link text: [](url) — 1 LINK.
  {
    const QString input = QStringLiteral("[](url)\n");
    HighlightElement **result = parseCmark(input.toUtf8());
    QVERIFY(result != NULL);

    QCOMPARE(countElements(result, HLT_LINK), 1);

    freeHighlightElements(result, NUM_HIGHLIGHT_STYLES);
  }
}

void TestMarkdownParser::testUnclosedDelimiters()
{
  // Unclosed bold — no crash, 0 STRONG elements.
  {
    const QString input = QStringLiteral("**broken\n");
    HighlightElement **result = parseCmark(input.toUtf8());
    QVERIFY(result != NULL);

    QCOMPARE(countElements(result, HLT_STRONG), 0);

    freeHighlightElements(result, NUM_HIGHLIGHT_STYLES);
  }

  // Unclosed link — no crash, 0 LINK elements.
  {
    const QString input = QStringLiteral("[unclosed\n");
    HighlightElement **result = parseCmark(input.toUtf8());
    QVERIFY(result != NULL);

    QCOMPARE(countElements(result, HLT_LINK), 0);

    freeHighlightElements(result, NUM_HIGHLIGHT_STYLES);
  }
}

void TestMarkdownParser::testDegenerate()
{
  // Empty string — parseCmark returns null (no crash).
  {
    const QString input = QStringLiteral("");
    HighlightElement **result = parseCmark(input.toUtf8());
    QVERIFY(result == NULL);
  }

  // Single newline — no crash.
  {
    const QString input = QStringLiteral("\n");
    HighlightElement **result = parseCmark(input.toUtf8());
    QVERIFY(result != NULL);
    freeHighlightElements(result, NUM_HIGHLIGHT_STYLES);
  }

  // Spaces — no crash.
  {
    const QString input = QStringLiteral("   \n");
    HighlightElement **result = parseCmark(input.toUtf8());
    QVERIFY(result != NULL);
    freeHighlightElements(result, NUM_HIGHLIGHT_STYLES);
  }
}

void TestMarkdownParser::testNestedOverlap()
{
  const QString input = QStringLiteral("***bold-italic***\n");
  HighlightElement **result = parseCmark(input.toUtf8());
  QVERIFY(result != NULL);

  QCOMPARE(countElements(result, HLT_EMPH), 1);
  QCOMPARE(countElements(result, HLT_STRONG), 1);

  // Verify both elements exist with valid ranges.
  HighlightElement *em = firstElement(result, HLT_EMPH);
  QVERIFY(em != NULL);
  QVERIFY((int)em->pos < (int)em->end);

  HighlightElement *st = firstElement(result, HLT_STRONG);
  QVERIFY(st != NULL);
  QVERIFY((int)st->pos < (int)st->end);

  freeHighlightElements(result, NUM_HIGHLIGHT_STYLES);
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
  HighlightElement **result = parseCmark(input.toUtf8());
  QVERIFY(result != NULL);

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

  freeHighlightElements(result, NUM_HIGHLIGHT_STYLES);
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

  // Run 3 iterations, collect times.
  QVector<qint64> times;
  times.reserve(3);
  for (int iter = 0; iter < 3; iter++) {
    QElapsedTimer timer;
    timer.start();
    HighlightElement **result = parseCmark(utf8);
    qint64 elapsed = timer.elapsed();
    QVERIFY(result != NULL);
    freeHighlightElements(result, NUM_HIGHLIGHT_STYLES);
    times.append(elapsed);
  }

  std::sort(times.begin(), times.end());
  qint64 median = times[1];
  qDebug() << "cmark parse times (ms):" << times[0] << times[1] << times[2]
           << "median:" << median;

  QVERIFY2(median < 500,
           qPrintable(
               QString("Median parse time %1ms exceeds 500ms threshold").arg(median)));
}

QTEST_MAIN(tests::TestMarkdownParser)
