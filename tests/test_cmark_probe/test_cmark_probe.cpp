#include "test_cmark_probe.h"

#include <cmark.h>

#include "cmarkadapter.h"
#include "markdownastwalker.h"

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

// Helper: count blocks (newlines + 1) in UTF-8 text.
static int countBlocks(const QByteArray &p_utf8)
{
  int n = 1;
  for (int i = 0; i < p_utf8.size(); ++i) {
    if (p_utf8[i] == '\n') ++n;
  }
  return n;
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
  int numBlocks = countBlocks(utf8);

  auto result = vte::md::walkAndConvert(utf8, numBlocks);

  // H1 (style 12) should have 1 element.
  QCOMPARE(countElements(result, 12), 1);

  // EMPH (style 7) should have 1 element.
  QCOMPARE(countElements(result, 7), 1);

  // H1 should be in headerRegions starting at pos 0.
  QVERIFY(!result.headerRegions.isEmpty());
  QCOMPARE(result.headerRegions[0].m_startPos, 0);

  // EMPH "*world*" starts at line 3, col 1 (byte 0 of line 3).
  // Lines: "# Hello\n" (8 bytes, 8 QChars), "\n" (1 byte, 1 QChar), "*world*\n"
  // Line 3 starts at QChar 9.
  // Find the EMPH HLUnit and compute its absolute position.
  QVector<int> blockStarts;
  blockStarts.append(0);
  QString qText = QString::fromUtf8(text);
  for (int i = 0; i < qText.size(); ++i) {
    if (qText[i] == '\n') blockStarts.append(i + 1);
  }
  bool foundEmph = false;
  for (int blockNum = 0; blockNum < result.blocksHighlights.size(); ++blockNum) {
    for (const auto &unit : result.blocksHighlights[blockNum]) {
      if (unit.styleIndex == 7) {
        int absStart = (blockNum < blockStarts.size() ? blockStarts[blockNum] : 0) + unit.start;
        int absEnd = absStart + unit.length;
        QCOMPARE(absStart, 9);
        // "*world*" is 7 bytes → end at QChar 9+7=16.
        QCOMPARE(absEnd, 16);
        foundEmph = true;
      }
    }
  }
  QVERIFY2(foundEmph, "EMPH element not found");
}

void TestCmarkProbe::testWalkerTable()
{
  const char *text =
      "| h1 | h2 |\n"
      "|---|---|\n"
      "| a | b |\n";

  QByteArray utf8(text);
  int numBlocks = countBlocks(utf8);

  auto result = vte::md::walkAndConvert(utf8, numBlocks);

  // TABLE (style 30) — walker produces 1 HLUnit per block line (3 lines = 3 units).
  QCOMPARE(countElements(result, 30), 3);

  // TABLEHEADER (style 31) should have 1 element (the header row).
  QCOMPARE(countElements(result, 31), 1);

  // Verify table regions exist (1 logical table).
  QVERIFY(!result.tableRegions.isEmpty());
  QVERIFY(!result.tableHeaderRegions.isEmpty());
}

void TestCmarkProbe::testParseCmark()
{
  const char *text = "# Hello\n\n*world*\n\n```cpp\ncode\n```\n";
  QByteArray utf8(text);
  int numBlocks = countBlocks(utf8);

  auto result = vte::md::walkAndConvert(utf8, numBlocks);

  // H1 (12) should have 1 element
  QCOMPARE(countElements(result, 12), 1);

  // EMPH (7) should have 1 element
  QCOMPARE(countElements(result, 7), 1);

  // FENCEDCODEBLOCK (23) — walker produces 1 HLUnit per block line (3 lines = 3 units).
  QCOMPARE(countElements(result, 23), 3);
}

void TestCmarkProbe::testWalkerListItemInlines()
{
  // Test inline element positions inside list items.
  // This is TDD RED: tests expose expected positions; failures indicate offset bug.

  // Helper lambda to find first HLUnit with given style.
  auto findUnit = [](const vte::md::ASTWalkResult &r, int style) -> QPair<bool, vte::md::HLUnit> {
    for (int b = 0; b < r.blocksHighlights.size(); ++b) {
      for (const auto &u : r.blocksHighlights[b]) {
        if (u.styleIndex == (unsigned int)style) {
          return qMakePair(true, u);
        }
      }
    }
    vte::md::HLUnit empty;
    return qMakePair(false, empty);
  };

  // Helper lambda to find first HLUnit with given style in a specific block.
  auto findUnitInBlock = [](const vte::md::ASTWalkResult &r, int block, int style) -> QPair<bool, vte::md::HLUnit> {
    if (block < 0 || block >= r.blocksHighlights.size()) {
      vte::md::HLUnit empty;
      return qMakePair(false, empty);
    }
    for (const auto &u : r.blocksHighlights[block]) {
      if (u.styleIndex == (unsigned int)style) {
        return qMakePair(true, u);
      }
    }
    vte::md::HLUnit empty;
    return qMakePair(false, empty);
  };

  const int CODE = 4;
  const int EMPH = 7;

  // --- Case 1: ASCII, first-line code span ---
  {
    const char *text = "- list with `code span`\n";
    QByteArray utf8(text);
    int numBlocks = countBlocks(utf8);
    auto result = vte::md::walkAndConvert(utf8, numBlocks);

    auto pair = findUnit(result, CODE);
    QVERIFY2(pair.first, "Case 1: CODE element not found");
    qDebug() << "Case 1: CODE start=" << pair.second.start
             << "length=" << pair.second.length;
    // "`code span`" starts at QChar 12 within block 0, length 11
    QCOMPARE((int)pair.second.start, 12);
    QCOMPARE((int)pair.second.length, 11);
  }

  // --- Case 2: ASCII, first-line emphasis ---
  {
    const char *text = "- list with *emphasis*\n";
    QByteArray utf8(text);
    int numBlocks = countBlocks(utf8);
    auto result = vte::md::walkAndConvert(utf8, numBlocks);

    auto pair = findUnit(result, EMPH);
    QVERIFY2(pair.first, "Case 2: EMPH element not found");
    qDebug() << "Case 2: EMPH start=" << pair.second.start
             << "length=" << pair.second.length;
    // "*emphasis*" starts at QChar 12 within block 0, length 10
    QCOMPARE((int)pair.second.start, 12);
    QCOMPARE((int)pair.second.length, 10);
  }

  // --- Case 3: ASCII, continuation line code span ---
  {
    const char *text = "- first line\n  `code on continuation`\n";
    QByteArray utf8(text);
    int numBlocks = countBlocks(utf8);
    auto result = vte::md::walkAndConvert(utf8, numBlocks);

    // CODE is on line 1 (block 1)
    auto pair = findUnitInBlock(result, 1, CODE);
    QVERIFY2(pair.first, "Case 3: CODE element not found in block 1");
    qDebug() << "Case 3: CODE start=" << pair.second.start
             << "length=" << pair.second.length;
    // "  `code on continuation`" → backtick at QChar 2 within block 1, length 22
    QCOMPARE((int)pair.second.start, 2);
    QCOMPARE((int)pair.second.length, 22);
  }

  // --- Case 4: CJK, first-line code span ---
  {
    // "- 你好 `code`\n"
    const char *text = "- \xe4\xbd\xa0\xe5\xa5\xbd `code`\n";
    QByteArray utf8(text);
    int numBlocks = countBlocks(utf8);
    auto result = vte::md::walkAndConvert(utf8, numBlocks);

    auto pair = findUnit(result, CODE);
    QVERIFY2(pair.first, "Case 4: CODE element not found");
    qDebug() << "Case 4: CODE start=" << pair.second.start
             << "length=" << pair.second.length;
    // "- 你好 `code`" → QChars: '-'(0),' '(1),'你'(2),'好'(3),' '(4),'`'(5)...
    // CODE starts at QChar 5, length 6
    QCOMPARE((int)pair.second.start, 5);
    QCOMPARE((int)pair.second.length, 6);
  }

  // --- Case 5: CJK, continuation line code span ---
  {
    // "- 你好世界\n  `code`\n"
    const char *text = "- \xe4\xbd\xa0\xe5\xa5\xbd\xe4\xb8\x96\xe7\x95\x8c\n  `code`\n";
    QByteArray utf8(text);
    int numBlocks = countBlocks(utf8);
    auto result = vte::md::walkAndConvert(utf8, numBlocks);

    // CODE is on line 1 (block 1)
    auto pair = findUnitInBlock(result, 1, CODE);
    QVERIFY2(pair.first, "Case 5: CODE element not found in block 1");
    qDebug() << "Case 5: CODE start=" << pair.second.start
             << "length=" << pair.second.length;
    // Line 1: "  `code`\n" → backtick at QChar 2, length 6
    QCOMPARE((int)pair.second.start, 2);
    QCOMPARE((int)pair.second.length, 6);
  }

  // --- Case 6: Nested list, CJK + code span ---
  {
    // "- outer\n    1. 你好 `code`\n"
    const char *text = "- outer\n    1. \xe4\xbd\xa0\xe5\xa5\xbd `code`\n";
    QByteArray utf8(text);
    int numBlocks = countBlocks(utf8);
    auto result = vte::md::walkAndConvert(utf8, numBlocks);

    // CODE is on line 1 (block 1)
    auto pair = findUnitInBlock(result, 1, CODE);
    QVERIFY2(pair.first, "Case 6: CODE element not found in block 1");
    qDebug() << "Case 6: CODE start=" << pair.second.start
             << "length=" << pair.second.length;
    // Line 1: "    1. 你好 `code`" → QChars: 7 ASCII + 你(1) + 好(1) + ' '(1) + '`'(1)...
    // CODE "`code`" starts at QChar 10, length 6
    QCOMPARE((int)pair.second.start, 10);
    QCOMPARE((int)pair.second.length, 6);
  }

  // --- Case 7: Mixed CJK + ASCII emphasis ---
  {
    // "- 你好 *emphasis* world\n"
    const char *text = "- \xe4\xbd\xa0\xe5\xa5\xbd *emphasis* world\n";
    QByteArray utf8(text);
    int numBlocks = countBlocks(utf8);
    auto result = vte::md::walkAndConvert(utf8, numBlocks);

    auto pair = findUnit(result, EMPH);
    QVERIFY2(pair.first, "Case 7: EMPH element not found");
    qDebug() << "Case 7: EMPH start=" << pair.second.start
             << "length=" << pair.second.length;
    // "- 你好 *emphasis*" → '-'(0),' '(1),'你'(2),'好'(3),' '(4),'*'(5)...
    // EMPH "*emphasis*" starts at QChar 5, length 10
    QCOMPARE((int)pair.second.start, 5);
    QCOMPARE((int)pair.second.length, 10);
  }

  // --- Case 8: Multiple inlines on same list line ---
  {
    // "- `a` and *b*\n"
    const char *text = "- `a` and *b*\n";
    QByteArray utf8(text);
    int numBlocks = countBlocks(utf8);
    auto result = vte::md::walkAndConvert(utf8, numBlocks);

    auto codePair = findUnit(result, CODE);
    QVERIFY2(codePair.first, "Case 8: CODE element not found");
    qDebug() << "Case 8: CODE start=" << codePair.second.start
             << "length=" << codePair.second.length;
    // "`a`" starts at QChar 2, length 3
    QCOMPARE((int)codePair.second.start, 2);
    QCOMPARE((int)codePair.second.length, 3);

    auto emphPair = findUnit(result, EMPH);
    QVERIFY2(emphPair.first, "Case 8: EMPH element not found");
    qDebug() << "Case 8: EMPH start=" << emphPair.second.start
             << "length=" << emphPair.second.length;
    // "*b*" starts at QChar 10, length 3
    QCOMPARE((int)emphPair.second.start, 10);
    QCOMPARE((int)emphPair.second.length, 3);
  }

  // --- Case 9: Multiline code span in list item (SOURCEPOS test) ---
  {
    // "- start `code\n  end` after\n"
    const char *text = "- start `code\n  end` after\n";
    QByteArray utf8(text);
    int numBlocks = countBlocks(utf8);
    auto result = vte::md::walkAndConvert(utf8, numBlocks);

    // Code span crosses lines; check if CODE unit is found in block 0 or 1
    auto pair0 = findUnitInBlock(result, 0, CODE);
    auto pair1 = findUnitInBlock(result, 1, CODE);
    qDebug() << "Case 9: block0 CODE found=" << pair0.first
             << "block1 CODE found=" << pair1.first;
    if (pair0.first) {
      qDebug() << "  block0 CODE start=" << pair0.second.start
               << "length=" << pair0.second.length;
    }
    if (pair1.first) {
      qDebug() << "  block1 CODE start=" << pair1.second.start
               << "length=" << pair1.second.length;
    }
    // At least one block should contain the CODE unit
    QVERIFY2(pair0.first || pair1.first, "Case 9: CODE element not found in any block");
  }
}

void TestCmarkProbe::testWalkerLazyContinuation()
{
  // Test inline element positions on LAZY continuation lines in list items.
  // Lazy continuation = no leading indent on continuation line.
  // Bug: cmark adds block_offset to columns on lazy lines where whitespace
  // was never stripped, causing over-reported positions.

  // Helper lambda to find first HLUnit with given style in a specific block.
  auto findUnitInBlock = [](const vte::md::ASTWalkResult &r, int block, int style) -> QPair<bool, vte::md::HLUnit> {
    if (block < 0 || block >= r.blocksHighlights.size()) {
      vte::md::HLUnit empty;
      return qMakePair(false, empty);
    }
    for (const auto &u : r.blocksHighlights[block]) {
      if (u.styleIndex == (unsigned int)style) {
        return qMakePair(true, u);
      }
    }
    vte::md::HLUnit empty;
    return qMakePair(false, empty);
  };

  const int CODE = 4;
  const int EMPH = 7;

  // --- Case L1: Lazy continuation with code span (ASCII) ---
  {
    // "- first line\n`code on lazy`\n"
    const char *text = "- first line\n`code on lazy`\n";
    QByteArray utf8(text);
    int numBlocks = countBlocks(utf8);
    auto result = vte::md::walkAndConvert(utf8, numBlocks);

    auto pair = findUnitInBlock(result, 1, CODE);
    QVERIFY2(pair.first, "Case L1: CODE element not found in block 1");
    qDebug() << "Case L1: CODE start=" << pair.second.start
             << "length=" << pair.second.length;
    // Line 1: "`code on lazy`" starts at QChar 0, length 14
    QCOMPARE((int)pair.second.start, 0);
    QCOMPARE((int)pair.second.length, 14);
  }

  // --- Case L2: CJK first line + lazy continuation with code ---
  {
    // "- 你好世界\n`code`\n"
    const char *text = "- \xe4\xbd\xa0\xe5\xa5\xbd\xe4\xb8\x96\xe7\x95\x8c\n`code`\n";
    QByteArray utf8(text);
    int numBlocks = countBlocks(utf8);
    auto result = vte::md::walkAndConvert(utf8, numBlocks);

    auto pair = findUnitInBlock(result, 1, CODE);
    QVERIFY2(pair.first, "Case L2: CODE element not found in block 1");
    qDebug() << "Case L2: CODE start=" << pair.second.start
             << "length=" << pair.second.length;
    // Line 1: "`code`" starts at QChar 0, length 6
    QCOMPARE((int)pair.second.start, 0);
    QCOMPARE((int)pair.second.length, 6);
  }

  // --- Case L3: Lazy line with CJK before code span ---
  {
    // "- first line\n你好 `code` world\n"
    const char *text = "- first line\n\xe4\xbd\xa0\xe5\xa5\xbd `code` world\n";
    QByteArray utf8(text);
    int numBlocks = countBlocks(utf8);
    auto result = vte::md::walkAndConvert(utf8, numBlocks);

    auto pair = findUnitInBlock(result, 1, CODE);
    QVERIFY2(pair.first, "Case L3: CODE element not found in block 1");
    qDebug() << "Case L3: CODE start=" << pair.second.start
             << "length=" << pair.second.length;
    // Line 1: "你好 `code` world" → 你(0)好(1)space(2)`(3)c(4)o(5)d(6)e(7)`(8)
    // CODE "`code`" starts at QChar 3, length 6
    QCOMPARE((int)pair.second.start, 3);
    QCOMPARE((int)pair.second.length, 6);
  }

  // --- Case L4: Lazy continuation with emphasis ---
  {
    // "- first line\n*emphasis on lazy*\n"
    const char *text = "- first line\n*emphasis on lazy*\n";
    QByteArray utf8(text);
    int numBlocks = countBlocks(utf8);
    auto result = vte::md::walkAndConvert(utf8, numBlocks);

    auto pair = findUnitInBlock(result, 1, EMPH);
    QVERIFY2(pair.first, "Case L4: EMPH element not found in block 1");
    qDebug() << "Case L4: EMPH start=" << pair.second.start
             << "length=" << pair.second.length;
    // Line 1: "*emphasis on lazy*" starts at QChar 0, length 18
    QCOMPARE((int)pair.second.start, 0);
    QCOMPARE((int)pair.second.length, 18);
  }

  // --- Case L5: Ordered list lazy continuation with code ---
  {
    // "1. first line\nlazy `code`\n"
    const char *text = "1. first line\nlazy `code`\n";
    QByteArray utf8(text);
    int numBlocks = countBlocks(utf8);
    auto result = vte::md::walkAndConvert(utf8, numBlocks);

    auto pair = findUnitInBlock(result, 1, CODE);
    QVERIFY2(pair.first, "Case L5: CODE element not found in block 1");
    qDebug() << "Case L5: CODE start=" << pair.second.start
             << "length=" << pair.second.length;
    // Line 1: "lazy `code`" → l(0)a(1)z(2)y(3)space(4)`(5)c(6)o(7)d(8)e(9)`(10)
    // CODE "`code`" starts at QChar 5, length 6
    QCOMPARE((int)pair.second.start, 5);
    QCOMPARE((int)pair.second.length, 6);
  }

  // --- Case L6: User reproduction - CJK first line, lazy with Markdown`code`end ---
  {
    // "- 段落和换行\nMarkdown`code`end\n"
    const char *text = "- \xe6\xae\xb5\xe8\x90\xbd\xe5\x92\x8c\xe6\x8d\xa2\xe8\xa1\x8c\nMarkdown`code`end\n";
    QByteArray utf8(text);
    int numBlocks = countBlocks(utf8);
    auto result = vte::md::walkAndConvert(utf8, numBlocks);

    auto pair = findUnitInBlock(result, 1, CODE);
    QVERIFY2(pair.first, "Case L6: CODE element not found in block 1");
    qDebug() << "Case L6: CODE start=" << pair.second.start
             << "length=" << pair.second.length;
    // Line 1: "Markdown`code`end" → M(0)a(1)r(2)k(3)d(4)o(5)w(6)n(7)`(8)c(9)o(10)d(11)e(12)`(13)
    // CODE "`code`" starts at QChar 8, length 6
    QCOMPARE((int)pair.second.start, 8);
    QCOMPARE((int)pair.second.length, 6);
  }

  // --- Probe: lineLeadingSpaces utility ---
  {
    QByteArray probeText("- hello\nworld\n  indented\n");
    LineOffsetTable probeTable(probeText);
    QCOMPARE(probeTable.lineLeadingSpaces(0), 0);  // '-' not a space
    QCOMPARE(probeTable.lineLeadingSpaces(1), 0);  // 'w' not a space
    QCOMPARE(probeTable.lineLeadingSpaces(2), 2);  // 2 spaces
  }
}

QTEST_MAIN(tests::TestCmarkProbe)
