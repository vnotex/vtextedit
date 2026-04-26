#include "test_cmark_probe.h"

#include <cmark.h>

#include "cmarkadapter.h"
#include "highlightelement.h"

#include <QDebug>
#include <cstring>

using namespace tests;

// Helper: parse document with default options
static cmark_node *parseDoc(const char *p_text)
{
  return cmark_parse_document(p_text, strlen(p_text), CMARK_OPT_DEFAULT);
}

// Helper: return node type name for debug output
static const char *typeName(cmark_node *p_node)
{
  return cmark_node_get_type_string(p_node);
}

void TestCmarkProbe::initTestCase()
{
  qDebug() << "cmark version:" << cmark_version_string();
}

void TestCmarkProbe::cleanupTestCase()
{
}

void TestCmarkProbe::testPositionModel()
{
  // Parse: "*emph* **strong** ~~strike~~\n"
  // Positions:  123456789...
  const char *text = "*emph* **strong** ~~strike~~\n";
  cmark_node *doc = parseDoc(text);
  QVERIFY(doc != nullptr);

  cmark_iter *iter = cmark_iter_new(doc);
  cmark_event_type ev;

  qDebug() << "=== testPositionModel: AST dump ===";
  qDebug() << "Input:" << text;

  while ((ev = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
    cmark_node *node = cmark_iter_get_node(iter);
    cmark_node_type type = cmark_node_get_type(node);
    int sl = cmark_node_get_start_line(node);
    int sc = cmark_node_get_start_column(node);
    int el = cmark_node_get_end_line(node);
    int ec = cmark_node_get_end_column(node);

    qDebug().nospace()
        << (ev == CMARK_EVENT_ENTER ? "ENTER " : (ev == CMARK_EVENT_EXIT ? "EXIT  " : "LEAF  "))
        << typeName(node) << " [" << sl << ":" << sc << " - " << el << ":" << ec << "]";

    // Verify columns are 1-indexed (at minimum, start_line >= 1 for non-NONE)
    if (type != CMARK_NODE_NONE && type != CMARK_NODE_DOCUMENT) {
      QVERIFY2(sl >= 1, qPrintable(QString("start_line < 1 for %1").arg(typeName(node))));
    }

    // Check EMPH position on ENTER event
    if (type == CMARK_NODE_EMPH && ev == CMARK_EVENT_ENTER) {
      qDebug() << "EMPH: start_col=" << sc << "end_col=" << ec;
      qDebug() << "  If sc=1, delimiters INCLUDED (first * is col 1)";
      qDebug() << "  If sc=2, delimiters EXCLUDED (content 'e' starts at col 2)";
      // '*' is at byte offset 0, 1-indexed = col 1
      // 'e' starts at byte offset 1, 1-indexed = col 2
      QVERIFY(sc >= 1);
    }

    // Check STRONG position on ENTER event
    if (type == CMARK_NODE_STRONG && ev == CMARK_EVENT_ENTER) {
      qDebug() << "STRONG: start_col=" << sc << "end_col=" << ec;
      qDebug() << "  '**' starts at col 8 (1-indexed byte offset)";
      qDebug() << "  If sc=8, delimiters INCLUDED";
      qDebug() << "  If sc=10, delimiters EXCLUDED";
      QVERIFY(sc >= 1);
    }

    // Check STRIKETHROUGH position on ENTER event
    if (type == CMARK_NODE_STRIKETHROUGH && ev == CMARK_EVENT_ENTER) {
      qDebug() << "STRIKETHROUGH: start_col=" << sc << "end_col=" << ec;
      qDebug() << "  '~~' starts at col 19 (1-indexed byte offset)";
      qDebug() << "  If sc=19, delimiters INCLUDED";
      qDebug() << "  If sc=21, delimiters EXCLUDED";
      QVERIFY(sc >= 1);
    }
  }

  cmark_iter_free(iter);
  cmark_node_free(doc);
}

void TestCmarkProbe::testDelimiterBoundary()
{
  // Test each delimiter type and record positions
  struct TestCase {
    const char *input;
    cmark_node_type expectedType;
    const char *label;
  };

  TestCase cases[] = {
      {"***bold-italic***\n", CMARK_NODE_EMPH, "EMPH(in bold-italic)"},
      {"[link text](url)\n", CMARK_NODE_LINK, "LINK"},
      {"`code`\n", CMARK_NODE_CODE, "CODE"},
      {"==mark==\n", CMARK_NODE_MARK, "MARK"},
      {"$math$\n", CMARK_NODE_FORMULA_INLINE, "FORMULA_INLINE"},
  };

  qDebug() << "=== testDelimiterBoundary ===";

  for (const auto &tc : cases) {
    cmark_node *doc = parseDoc(tc.input);
    QVERIFY2(doc != nullptr, qPrintable(QString("Failed to parse: %1").arg(tc.input)));

    cmark_iter *iter = cmark_iter_new(doc);
    cmark_event_type ev;
    bool found = false;

    while ((ev = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
      cmark_node *node = cmark_iter_get_node(iter);
      cmark_node_type type = cmark_node_get_type(node);

      if (type == tc.expectedType && (ev == CMARK_EVENT_ENTER || cmark_node_is_leaf(node))) {
        int sc = cmark_node_get_start_column(node);
        int ec = cmark_node_get_end_column(node);
        int sl = cmark_node_get_start_line(node);
        int el = cmark_node_get_end_line(node);
        qDebug().nospace()
            << tc.label << " input=\"" << tc.input << "\" => "
            << "[" << sl << ":" << sc << " - " << el << ":" << ec << "]"
            << " (sc=1 means delimiter included, sc>1 means content-only)";
        QVERIFY(sc >= 1);
        QVERIFY(ec >= sc);
        found = true;
        break;
      }
    }

    QVERIFY2(found, qPrintable(QString("Node type %1 not found for input: %2")
                                    .arg(tc.expectedType)
                                    .arg(tc.input)));

    cmark_iter_free(iter);
    cmark_node_free(doc);
  }
}

void TestCmarkProbe::testExtensionSourcePositions()
{
  const char *text =
      "---\n"
      "title: test\n"
      "---\n"
      "\n"
      "~~strike~~ ==mark== $math$ $$display$$\n"
      "\n"
      "| h1 | h2 |\n"
      "|---|---|\n"
      "| a | b |\n"
      "\n"
      "[^1]: footnote def\n"
      "\n"
      "Text with [^1] reference.\n";

  cmark_node *doc = parseDoc(text);
  QVERIFY(doc != nullptr);

  cmark_iter *iter = cmark_iter_new(doc);
  cmark_event_type ev;
  int nodeCount = 0;
  int nodesWithPositions = 0;
  int nodesWithoutPositions = 0;

  qDebug() << "=== testExtensionSourcePositions: full AST ===";

  while ((ev = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
    cmark_node *node = cmark_iter_get_node(iter);
    cmark_node_type type = cmark_node_get_type(node);

    if (ev == CMARK_EVENT_ENTER || cmark_node_is_leaf(node)) {
      int sl = cmark_node_get_start_line(node);
      int sc = cmark_node_get_start_column(node);
      int el = cmark_node_get_end_line(node);
      int ec = cmark_node_get_end_column(node);

      nodeCount++;
      if (sl > 0) {
        nodesWithPositions++;
      } else {
        nodesWithoutPositions++;
        qDebug().nospace()
            << "WARNING: No position for " << typeName(node) << " type=" << type;
      }

      qDebug().nospace()
          << (ev == CMARK_EVENT_ENTER ? "ENTER " : "LEAF  ")
          << typeName(node) << " type=" << type
          << " [" << sl << ":" << sc << " - " << el << ":" << ec << "]";
    }
  }

  qDebug() << "Total nodes (enter/leaf):" << nodeCount
           << "with positions:" << nodesWithPositions
           << "without:" << nodesWithoutPositions;

  // Most nodes should have positions; document node itself may be 0
  QVERIFY2(nodesWithPositions > 0, "No nodes have source positions!");

  cmark_iter_free(iter);
  cmark_node_free(doc);
}

void TestCmarkProbe::testMultiLinePositions()
{
  const char *text =
      "> blockquote\n"
      "> continues\n"
      "\n"
      "- item 1\n"
      "  continues\n"
      "- item 2\n"
      "\n"
      "```cpp\n"
      "code\n"
      "```\n";

  cmark_node *doc = parseDoc(text);
  QVERIFY(doc != nullptr);

  cmark_iter *iter = cmark_iter_new(doc);
  cmark_event_type ev;

  qDebug() << "=== testMultiLinePositions ===";

  while ((ev = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
    cmark_node *node = cmark_iter_get_node(iter);
    cmark_node_type type = cmark_node_get_type(node);

    if (ev == CMARK_EVENT_ENTER || cmark_node_is_leaf(node)) {
      int sl = cmark_node_get_start_line(node);
      int el = cmark_node_get_end_line(node);
      int sc = cmark_node_get_start_column(node);
      int ec = cmark_node_get_end_column(node);

      qDebug().nospace()
          << typeName(node) << " [" << sl << ":" << sc << " - " << el << ":" << ec << "]";

      // Block-level multi-line elements should span multiple lines
      if (type == CMARK_NODE_BLOCK_QUOTE && ev == CMARK_EVENT_ENTER) {
        qDebug() << "  BLOCK_QUOTE: end_line=" << el << "start_line=" << sl;
        QVERIFY2(el >= sl, "block_quote end_line should be >= start_line");
        // Should span lines 1-2
        QVERIFY(el > sl);
      }

      if (type == CMARK_NODE_LIST && ev == CMARK_EVENT_ENTER) {
        qDebug() << "  LIST: end_line=" << el << "start_line=" << sl;
        QVERIFY(el > sl);
      }

      if (type == CMARK_NODE_CODE_BLOCK) {
        qDebug() << "  CODE_BLOCK: end_line=" << el << "start_line=" << sl;
        QVERIFY2(el > sl, "code_block should span multiple lines");
      }
    }
  }

  cmark_iter_free(iter);
  cmark_node_free(doc);
}

void TestCmarkProbe::testUtf8Columns()
{
  // CRITICAL: determine if columns are byte offsets or codepoint offsets
  qDebug() << "=== testUtf8Columns ===";

  // Test 1: CJK characters (3 bytes each in UTF-8)
  // "# 你好世界\n"
  // Bytes: # (1) SPACE (1) 你 (3) 好 (3) 世 (3) 界 (3) = 14 bytes + newline
  // Codepoints: # (1) SPACE (1) 你 (1) 好 (1) 世 (1) 界 (1) = 6 codepoints
  {
    const char *text = "# \xe4\xbd\xa0\xe5\xa5\xbd\xe4\xb8\x96\xe7\x95\x8c\n"; // "# 你好世界\n"
    cmark_node *doc = parseDoc(text);
    QVERIFY(doc != nullptr);

    cmark_iter *iter = cmark_iter_new(doc);
    cmark_event_type ev;

    while ((ev = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
      cmark_node *node = cmark_iter_get_node(iter);
      cmark_node_type type = cmark_node_get_type(node);

      if (type == CMARK_NODE_HEADING && ev == CMARK_EVENT_ENTER) {
        int sc = cmark_node_get_start_column(node);
        int ec = cmark_node_get_end_column(node);
        qDebug() << "HEADING (CJK): start_col=" << sc << "end_col=" << ec;
        qDebug() << "  If end_col=14 -> byte offsets";
        qDebug() << "  If end_col=6 -> codepoint offsets";
      }

      if (type == CMARK_NODE_TEXT) {
        int sc = cmark_node_get_start_column(node);
        int ec = cmark_node_get_end_column(node);
        const char *lit = cmark_node_get_literal(node);
        qDebug() << "TEXT (CJK content): start_col=" << sc << "end_col=" << ec
                 << "literal=" << (lit ? lit : "(null)");
        qDebug() << "  '你' starts at byte 3 (1-indexed). If sc=3 -> byte offset";
        qDebug() << "  '你' is codepoint 3 (1-indexed). Ambiguous if sc=3";
      }
    }

    cmark_iter_free(iter);
    cmark_node_free(doc);
  }

  // Test 2: Emoji (4 bytes each in UTF-8) to disambiguate
  // "**🎉bold🎉**\n"
  // Bytes: ** (2) 🎉 (4) b (1) o (1) l (1) d (1) 🎉 (4) ** (2) = 16 bytes
  // Codepoints: ** (2) 🎉 (1) b (1) o (1) l (1) d (1) 🎉 (1) ** (2) = 10 codepoints
  {
    const char *text = "**\xf0\x9f\x8e\x89" "bold\xf0\x9f\x8e\x89**\n"; // "**🎉bold🎉**\n"
    cmark_node *doc = parseDoc(text);
    QVERIFY(doc != nullptr);

    cmark_iter *iter = cmark_iter_new(doc);
    cmark_event_type ev;

    while ((ev = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
      cmark_node *node = cmark_iter_get_node(iter);
      cmark_node_type type = cmark_node_get_type(node);

      if (type == CMARK_NODE_STRONG && ev == CMARK_EVENT_ENTER) {
        int sc = cmark_node_get_start_column(node);
        int ec = cmark_node_get_end_column(node);
        qDebug() << "STRONG (emoji): start_col=" << sc << "end_col=" << ec;
        qDebug() << "  Total bytes=16. If ec=16 -> byte offsets";
        qDebug() << "  Total codepoints=10. If ec=10 -> codepoint offsets";
      }

      if (type == CMARK_NODE_TEXT) {
        int sc = cmark_node_get_start_column(node);
        int ec = cmark_node_get_end_column(node);
        const char *lit = cmark_node_get_literal(node);
        qDebug() << "TEXT (emoji content): start_col=" << sc << "end_col=" << ec
                 << "literal=" << (lit ? lit : "(null)");
        // 🎉 starts at byte 3 (after **), codepoint 3
        // But 🎉 is 4 bytes, so 'b' starts at byte 7, codepoint 4
        qDebug() << "  After '**🎉': byte=7, codepoint=4. Check sc value.";
      }
    }

    cmark_iter_free(iter);
    cmark_node_free(doc);
  }
}

void TestCmarkProbe::testHeadingLevel()
{
  const char *text =
      "# H1\n"
      "## H2\n"
      "### H3\n"
      "#### H4\n"
      "##### H5\n"
      "###### H6\n";

  cmark_node *doc = parseDoc(text);
  QVERIFY(doc != nullptr);

  cmark_iter *iter = cmark_iter_new(doc);
  cmark_event_type ev;
  int headingCount = 0;

  while ((ev = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
    cmark_node *node = cmark_iter_get_node(iter);

    if (cmark_node_get_type(node) == CMARK_NODE_HEADING && ev == CMARK_EVENT_ENTER) {
      headingCount++;
      int level = cmark_node_get_heading_level(node);
      QCOMPARE(level, headingCount);
      qDebug() << "Heading level" << level << "at line" << cmark_node_get_start_line(node);
    }
  }

  QCOMPARE(headingCount, 6);

  cmark_iter_free(iter);
  cmark_node_free(doc);
}

void TestCmarkProbe::testFenceInfo()
{
  const char *text = "```cpp\ncode\n```\n";

  cmark_node *doc = parseDoc(text);
  QVERIFY(doc != nullptr);

  cmark_iter *iter = cmark_iter_new(doc);
  cmark_event_type ev;
  bool found = false;

  while ((ev = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
    cmark_node *node = cmark_iter_get_node(iter);

    if (cmark_node_get_type(node) == CMARK_NODE_CODE_BLOCK) {
      const char *info = cmark_node_get_fence_info(node);
      QVERIFY(info != nullptr);
      QCOMPARE(QString(info), QString("cpp"));
      qDebug() << "CODE_BLOCK fence_info:" << info;

      const char *literal = cmark_node_get_literal(node);
      qDebug() << "CODE_BLOCK literal:" << (literal ? literal : "(null)");
      found = true;
      break;
    }
  }

  QVERIFY2(found, "CODE_BLOCK not found");

  cmark_iter_free(iter);
  cmark_node_free(doc);
}

void TestCmarkProbe::testListType()
{
  const char *text = "- bullet\n\n1. ordered\n";

  cmark_node *doc = parseDoc(text);
  QVERIFY(doc != nullptr);

  cmark_iter *iter = cmark_iter_new(doc);
  cmark_event_type ev;
  bool foundBullet = false;
  bool foundOrdered = false;

  while ((ev = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
    cmark_node *node = cmark_iter_get_node(iter);

    if (cmark_node_get_type(node) == CMARK_NODE_LIST && ev == CMARK_EVENT_ENTER) {
      cmark_list_type lt = cmark_node_get_list_type(node);

      if (lt == CMARK_BULLET_LIST) {
        qDebug() << "Found BULLET_LIST";
        foundBullet = true;
      } else if (lt == CMARK_ORDERED_LIST) {
        qDebug() << "Found ORDERED_LIST";
        foundOrdered = true;
      }
    }
  }

  QVERIFY2(foundBullet, "BULLET_LIST not found");
  QVERIFY2(foundOrdered, "ORDERED_LIST not found");

  cmark_iter_free(iter);
  cmark_node_free(doc);
}

void TestCmarkProbe::testTableStructure()
{
  const char *text =
      "| h1 | h2 |\n"
      "|---|---|\n"
      "| a | b |\n"
      "| c | d |\n";

  cmark_node *doc = parseDoc(text);
  QVERIFY(doc != nullptr);

  cmark_iter *iter = cmark_iter_new(doc);
  cmark_event_type ev;

  bool foundTable = false;
  bool foundRow = false;
  bool foundCell = false;
  int rowCount = 0;
  int cellCount = 0;

  qDebug() << "=== testTableStructure ===";

  while ((ev = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
    cmark_node *node = cmark_iter_get_node(iter);
    cmark_node_type type = cmark_node_get_type(node);

    if (type == CMARK_NODE_TABLE && ev == CMARK_EVENT_ENTER) {
      foundTable = true;
      qDebug() << "TABLE found at line" << cmark_node_get_start_line(node);
    }

    if (type == CMARK_NODE_TABLE_ROW && ev == CMARK_EVENT_ENTER) {
      foundRow = true;
      rowCount++;
      qDebug() << "TABLE_ROW #" << rowCount << "at line" << cmark_node_get_start_line(node);
    }

    if (type == CMARK_NODE_TABLE_CELL && ev == CMARK_EVENT_ENTER) {
      foundCell = true;
      cellCount++;
    }
  }

  qDebug() << "Rows:" << rowCount << "Cells:" << cellCount;

  QVERIFY2(foundTable, "TABLE not found");
  QVERIFY2(foundRow, "TABLE_ROW not found");
  QVERIFY2(foundCell, "TABLE_CELL not found");
  // cmark includes separator row as a TABLE_ROW:
  // header row + separator row + 2 data rows = 4 rows, 8 cells
  qDebug() << "NOTE: cmark includes the separator '|---|---|' as a TABLE_ROW";
  QCOMPARE(rowCount, 4);
  QCOMPARE(cellCount, 8);

  cmark_iter_free(iter);
  cmark_node_free(doc);
}

void TestCmarkProbe::testFirstTableRowIsHeader()
{
  const char *text =
      "| h1 | h2 |\n"
      "|---|---|\n"
      "| a | b |\n";

  cmark_node *doc = parseDoc(text);
  QVERIFY(doc != nullptr);

  // Find TABLE node
  cmark_iter *iter = cmark_iter_new(doc);
  cmark_event_type ev;
  cmark_node *tableNode = nullptr;

  while ((ev = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
    cmark_node *node = cmark_iter_get_node(iter);
    if (cmark_node_get_type(node) == CMARK_NODE_TABLE && ev == CMARK_EVENT_ENTER) {
      tableNode = node;
      break;
    }
  }
  cmark_iter_free(iter);

  QVERIFY2(tableNode != nullptr, "TABLE not found");

  // First child of TABLE should be the header row
  cmark_node *firstRow = cmark_node_first_child(tableNode);
  QVERIFY2(firstRow != nullptr, "TABLE has no children");
  QCOMPARE((int)cmark_node_get_type(firstRow), (int)CMARK_NODE_TABLE_ROW);

  // Second child is a data row
  cmark_node *secondRow = cmark_node_next(firstRow);
  QVERIFY2(secondRow != nullptr, "TABLE has only one row");
  QCOMPARE((int)cmark_node_get_type(secondRow), (int)CMARK_NODE_TABLE_ROW);

  qDebug() << "=== testFirstTableRowIsHeader ===";
  qDebug() << "First TABLE_ROW line:" << cmark_node_get_start_line(firstRow);
  qDebug() << "Second TABLE_ROW line:" << cmark_node_get_start_line(secondRow);
  qDebug() << "STRATEGY: First child of TABLE node is always the header row.";
  qDebug() << "  The adapter should treat row index 0 as TABLEHEADER.";

  // Verify header row has cells
  cmark_node *firstCell = cmark_node_first_child(firstRow);
  QVERIFY2(firstCell != nullptr, "Header row has no cells");
  QCOMPARE((int)cmark_node_get_type(firstCell), (int)CMARK_NODE_TABLE_CELL);

  cmark_node_free(doc);
}

void TestCmarkProbe::testLinkUrl()
{
  const char *text = "[link text](http://example.com)\n";

  cmark_node *doc = parseDoc(text);
  QVERIFY(doc != nullptr);

  cmark_iter *iter = cmark_iter_new(doc);
  cmark_event_type ev;
  bool found = false;

  while ((ev = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
    cmark_node *node = cmark_iter_get_node(iter);

    if (cmark_node_get_type(node) == CMARK_NODE_LINK && ev == CMARK_EVENT_ENTER) {
      const char *url = cmark_node_get_url(node);
      QVERIFY(url != nullptr);
      QCOMPARE(QString(url), QString("http://example.com"));
      qDebug() << "LINK url:" << url;

      int sc = cmark_node_get_start_column(node);
      int ec = cmark_node_get_end_column(node);
      qDebug() << "LINK position: start_col=" << sc << "end_col=" << ec;
      qDebug() << "  '[' is at col 1. If sc=1 -> includes opening bracket";

      found = true;
      break;
    }
  }

  QVERIFY2(found, "LINK not found");

  cmark_iter_free(iter);
  cmark_node_free(doc);
}

void TestCmarkProbe::testLineOffsetTableAscii()
{
  QByteArray text("Hello world\n");
  LineOffsetTable table(text);

  // "Hello world\n"
  // Byte offsets (0-indexed): H=0 e=1 l=2 l=3 o=4 ' '=5 w=6 o=7 r=8 l=9 d=10 \n=11
  // cmark columns are 1-indexed: H=1 e=2 ... ' '=6 w=7 ...
  QCOMPARE(table.toDocPosition(1, 1), 0); // 'H' at QChar 0
  QCOMPARE(table.toDocPosition(1, 6), 5); // ' ' at QChar 5
  QCOMPARE(table.toDocPosition(1, 7), 6); // 'w' at QChar 6
}

void TestCmarkProbe::testLineOffsetTableCJK()
{
  // "你好\n" — each CJK char is 3 UTF-8 bytes, 1 QChar.
  // Bytes: 你(E4 BD A0) 好(E5 A5 BD) \n = 7 bytes
  QByteArray text("\xe4\xbd\xa0\xe5\xa5\xbd\n");
  LineOffsetTable table(text);

  QCOMPARE(table.toDocPosition(1, 1), 0); // '你' at QChar 0
  QCOMPARE(table.toDocPosition(1, 4), 1); // '好' starts at byte 3 (col 4), QChar 1
  QCOMPARE(table.toDocPosition(1, 7), 2); // '\n' at byte 6 (col 7), QChar 2
}

void TestCmarkProbe::testLineOffsetTableEmoji()
{
  // "🎉 test\n" — emoji is 4 UTF-8 bytes = 2 QChars (surrogate pair).
  // Bytes: 🎉(F0 9F 8E 89) ' '(20) t(74) e(65) s(73) t(74) \n = 10 bytes
  QByteArray text("\xf0\x9f\x8e\x89 test\n");
  LineOffsetTable table(text);

  QCOMPARE(table.toDocPosition(1, 1), 0); // '🎉' starts at byte 0 (col 1), QChar 0
  QCOMPARE(table.toDocPosition(1, 5), 2); // ' ' at byte 4 (col 5), QChar 2
  QCOMPARE(table.toDocPosition(1, 6), 3); // 't' at byte 5 (col 6), QChar 3
}

void TestCmarkProbe::testLineOffsetTableMultiLine()
{
  // "line1\nline2\n"
  // Line 1: l(0) i(1) n(2) e(3) 1(4) \n(5) — 6 bytes, 6 QChars
  // Line 2: l(6) i(7) n(8) e(9) 2(10) \n(11)
  QByteArray text("line1\nline2\n");
  LineOffsetTable table(text);

  QCOMPARE(table.toDocPosition(1, 1), 0); // 'l' of line1 at QChar 0
  QCOMPARE(table.toDocPosition(2, 1), 6); // 'l' of line2 at QChar 6
  QCOMPARE(table.toDocPosition(2, 5), 10); // '2' of line2 at QChar 10
}

void TestCmarkProbe::testWalkerSimple()
{
  // Parse "# Hello\n\n*world*\n"
  const char *text = "# Hello\n\n*world*\n";
  QByteArray utf8(text);
  LineOffsetTable offsets(utf8);

  cmark_node *doc = cmark_parse_document(text, strlen(text), CMARK_OPT_DEFAULT);
  QVERIFY(doc != nullptr);

  auto *result = new HighlightElement *[NUM_HIGHLIGHT_STYLES]();

  walkCmarkTree(doc, offsets, result, NUM_HIGHLIGHT_STYLES);

  // H1 (style 12) should have 1 element.
  QVERIFY2(result[12] != nullptr, "H1 element not found");
  QVERIFY(result[12]->next == nullptr); // Only one H1.

  // EMPH (style 7) should have 1 element.
  QVERIFY2(result[7] != nullptr, "EMPH element not found");
  QVERIFY(result[7]->next == nullptr); // Only one EMPH.

  // H1 should start at pos 0 ("# Hello\n" starts at byte 0).
  QCOMPARE(static_cast<int>(result[12]->pos), 0);

  // EMPH "*world*" starts at line 3, col 1 (byte 0 of line 3).
  // Lines: "# Hello\n" (8 bytes, 8 QChars), "\n" (1 byte, 1 QChar), "*world*\n"
  // Line 3 starts at QChar 9.
  QCOMPARE(static_cast<int>(result[7]->pos), 9);
  // "*world*" is 7 bytes → end at QChar 9+7=16.
  QCOMPARE(static_cast<int>(result[7]->end), 16);

  freeHighlightElements(result, NUM_HIGHLIGHT_STYLES);
  cmark_node_free(doc);
}

void TestCmarkProbe::testWalkerTable()
{
  const char *text =
      "| h1 | h2 |\n"
      "|---|---|\n"
      "| a | b |\n";

  QByteArray utf8(text);
  LineOffsetTable offsets(utf8);

  cmark_node *doc = cmark_parse_document(text, strlen(text), CMARK_OPT_DEFAULT);
  QVERIFY(doc != nullptr);

  auto *result = new HighlightElement *[NUM_HIGHLIGHT_STYLES]();

  walkCmarkTree(doc, offsets, result, NUM_HIGHLIGHT_STYLES);

  // TABLE (style 30) should have 1 element.
  QVERIFY2(result[30] != nullptr, "TABLE element not found");
  QVERIFY(result[30]->next == nullptr); // Only one TABLE.

  // TABLEHEADER (style 31) should have 1 element (the header row).
  QVERIFY2(result[31] != nullptr, "TABLEHEADER element not found");
  QVERIFY(result[31]->next == nullptr); // Only one header row.

  freeHighlightElements(result, NUM_HIGHLIGHT_STYLES);
  cmark_node_free(doc);
}

void TestCmarkProbe::testParseCmark()
{
  const char *text = "# Hello\n\n*world*\n\n```cpp\ncode\n```\n";
  QByteArray utf8(text);
  HighlightElement **result = parseCmark(utf8);
  QVERIFY(result != nullptr);

  // H1 (12) should have 1 element
  QVERIFY(result[12] != nullptr);

  // EMPH (7) should have 1 element
  QVERIFY(result[7] != nullptr);

  // FENCEDCODEBLOCK (23) should have 1 element
  QVERIFY(result[23] != nullptr);

  freeHighlightElements(result, NUM_HIGHLIGHT_STYLES);
}

QTEST_MAIN(tests::TestCmarkProbe)
