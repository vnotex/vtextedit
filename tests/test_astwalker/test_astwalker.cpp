#include "test_astwalker.h"

#include <QDir>
#include <QFile>

#include "markdownastwalker.h"
#include "markdownsyntaxstyles.h"

using namespace tests;

static const QStringList s_fixtureNames = {
    QStringLiteral("block_elements.md"),     QStringLiteral("inline_elements.md"),
    QStringLiteral("multiline_elements.md"), QStringLiteral("nested_elements.md"),
    QStringLiteral("edge_cases.md"),         QStringLiteral("extension_elements.md"),
    QStringLiteral("table_elements.md"),     QStringLiteral("math_elements.md")};

static QString readFile(const QString &p_path) {
  QFile f(p_path);
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return QString();
  }
  return QString::fromUtf8(f.readAll());
}

static QString
serializeBlocksHighlights(const QVector<QVector<vte::md::HLUnit>> &p_blocksHighlights) {
  QStringList lines;
  for (int blockNum = 0; blockNum < p_blocksHighlights.size(); ++blockNum) {
    for (const auto &unit : p_blocksHighlights[blockNum]) {
      lines.append(QStringLiteral("%1:%2:%3:%4")
                       .arg(blockNum)
                       .arg(unit.start)
                       .arg(unit.length)
                       .arg(unit.styleIndex));
    }
  }
  return lines.join('\n') + '\n';
}

void TestASTWalker::verifyBlocksHighlights() {
  QDir goldenDir(QStringLiteral(GOLDEN_DIR));

  for (const auto &name : s_fixtureNames) {
    QString text = readFile(QStringLiteral(FIXTURES_DIR) + "/" + name);
    if (text.isEmpty()) {
      QSKIP(qPrintable(QStringLiteral("Fixture not found: ") + name));
    }

    QByteArray utf8 = text.toUtf8();
    // Count blocks: number of lines in text. Each newline-separated segment is a block.
    int numBlocks = 1;
    for (int i = 0; i < utf8.size(); ++i) {
      if (utf8[i] == '\n') {
        ++numBlocks;
      }
    }

    vte::md::ASTWalkResult result = vte::md::walkAndConvert(utf8, numBlocks);

    QString actual = serializeBlocksHighlights(result.blocksHighlights);

    QString baseName = name;
    baseName.replace(QStringLiteral(".md"), QString());
    QString goldenPath = goldenDir.filePath(baseName + ".blocks.golden");

    QString expected = readFile(goldenPath);
    if (expected.isEmpty()) {
      QSKIP(qPrintable(QStringLiteral("Golden file not found: ") + goldenPath));
    }

    QCOMPARE(actual, expected);
  }
}

void TestASTWalker::verifyRegions() {
  // Test that walkAndConvert produces non-empty regions for relevant fixtures.
  {
    QString text = readFile(QStringLiteral(FIXTURES_DIR) + "/block_elements.md");
    QVERIFY(!text.isEmpty());
    QByteArray utf8 = text.toUtf8();
    int numBlocks = 1;
    for (int i = 0; i < utf8.size(); ++i) {
      if (utf8[i] == '\n')
        ++numBlocks;
    }
    vte::md::ASTWalkResult result = vte::md::walkAndConvert(utf8, numBlocks);
    QVERIFY(!result.headerRegions.isEmpty());
    QVERIFY(!result.codeBlockRegions.isEmpty());
    QVERIFY(!result.hruleRegions.isEmpty());
  }

  {
    QString text = readFile(QStringLiteral(FIXTURES_DIR) + "/table_elements.md");
    QVERIFY(!text.isEmpty());
    QByteArray utf8 = text.toUtf8();
    int numBlocks = 1;
    for (int i = 0; i < utf8.size(); ++i) {
      if (utf8[i] == '\n')
        ++numBlocks;
    }
    vte::md::ASTWalkResult result = vte::md::walkAndConvert(utf8, numBlocks);
    QVERIFY(!result.tableRegions.isEmpty());
    QVERIFY(!result.tableHeaderRegions.isEmpty());
  }

  {
    QString text = readFile(QStringLiteral(FIXTURES_DIR) + "/math_elements.md");
    QVERIFY(!text.isEmpty());
    QByteArray utf8 = text.toUtf8();
    int numBlocks = 1;
    for (int i = 0; i < utf8.size(); ++i) {
      if (utf8[i] == '\n')
        ++numBlocks;
    }
    vte::md::ASTWalkResult result = vte::md::walkAndConvert(utf8, numBlocks);
    QVERIFY(!result.inlineEquationRegions.isEmpty());
    QVERIFY(!result.displayFormulaRegions.isEmpty());
  }

  // Test fast mode skips regions.
  {
    QString text = readFile(QStringLiteral(FIXTURES_DIR) + "/block_elements.md");
    QVERIFY(!text.isEmpty());
    QByteArray utf8 = text.toUtf8();
    int numBlocks = 1;
    for (int i = 0; i < utf8.size(); ++i) {
      if (utf8[i] == '\n')
        ++numBlocks;
    }
    vte::md::ASTWalkResult result = vte::md::walkAndConvert(utf8, numBlocks, 0, 0, true);
    QVERIFY(result.headerRegions.isEmpty());
    QVERIFY(result.codeBlockRegions.isEmpty());
    // But blocksHighlights should still be populated.
    bool hasAny = false;
    for (const auto &block : result.blocksHighlights) {
      if (!block.isEmpty()) {
        hasAny = true;
        break;
      }
    }
    QVERIFY(hasAny);
  }
}

void TestASTWalker::testFoldingRegionsHeadings() {
  // H1 on line 1, H2 on line 3, H3 on line 5 (1-based cmark lines).
  QByteArray md = "# Heading 1\n"
                  "\n"
                  "## Heading 2\n"
                  "\n"
                  "### Heading 3\n";
  int numBlocks = 5;
  auto result = vte::md::walkAndConvert(md, numBlocks);
  // Should have 3 folding regions for headings.
  int headingCount = 0;
  for (const auto &r : result.foldingRegions) {
    if (r.m_type == vte::md::Heading) {
      ++headingCount;
    }
  }
  QCOMPARE(headingCount, 3);

  // Check each heading's block and level.
  // Regions are sorted by startBlock.
  QVERIFY(result.foldingRegions.size() >= 3);
  const auto &h1 = result.foldingRegions[0];
  QCOMPARE(h1.m_type, vte::md::Heading);
  QCOMPARE(h1.m_startBlock, 0); // line 1 -> block 0
  QCOMPARE(h1.m_endBlock, 0);
  QCOMPARE(h1.m_level, 1);

  const auto &h2 = result.foldingRegions[1];
  QCOMPARE(h2.m_type, vte::md::Heading);
  QCOMPARE(h2.m_startBlock, 2); // line 3 -> block 2
  QCOMPARE(h2.m_endBlock, 2);
  QCOMPARE(h2.m_level, 2);

  const auto &h3 = result.foldingRegions[2];
  QCOMPARE(h3.m_type, vte::md::Heading);
  QCOMPARE(h3.m_startBlock, 4); // line 5 -> block 4
  QCOMPARE(h3.m_endBlock, 4);
  QCOMPARE(h3.m_level, 3);
}

void TestASTWalker::testFoldingRegionsCodeBlock() {
  // Fenced code block spanning lines 1-3.
  QByteArray md = "```\n"
                  "code\n"
                  "```\n";
  int numBlocks = 3;
  auto result = vte::md::walkAndConvert(md, numBlocks);

  bool found = false;
  for (const auto &r : result.foldingRegions) {
    if (r.m_type == vte::md::FencedCode) {
      QCOMPARE(r.m_startBlock, 0);
      QCOMPARE(r.m_endBlock, 2);
      QCOMPARE(r.m_level, 0);
      found = true;
      break;
    }
  }
  QVERIFY(found);
}

void TestASTWalker::testFoldingRegionsBlockquote() {
  QByteArray md = "> line1\n"
                  "> line2\n"
                  "> line3\n";
  int numBlocks = 3;
  auto result = vte::md::walkAndConvert(md, numBlocks);

  bool found = false;
  for (const auto &r : result.foldingRegions) {
    if (r.m_type == vte::md::Blockquote) {
      QCOMPARE(r.m_startBlock, 0);
      QCOMPARE(r.m_endBlock, 2);
      QCOMPARE(r.m_level, 0);
      found = true;
      break;
    }
  }
  QVERIFY(found);
}

void TestASTWalker::testFoldingRegionsTable() {
  QByteArray md = "| A | B |\n"
                  "| - | - |\n"
                  "| 1 | 2 |\n";
  int numBlocks = 3;
  auto result = vte::md::walkAndConvert(md, numBlocks);

  bool found = false;
  for (const auto &r : result.foldingRegions) {
    if (r.m_type == vte::md::Table) {
      QCOMPARE(r.m_startBlock, 0);
      QCOMPARE(r.m_endBlock, 2);
      QCOMPARE(r.m_level, 0);
      found = true;
      break;
    }
  }
  QVERIFY(found);
}

void TestASTWalker::testFoldingRegionsMathBlock() {
  QByteArray md = "$$\n"
                  "E=mc^2\n"
                  "$$\n";
  int numBlocks = 3;
  auto result = vte::md::walkAndConvert(md, numBlocks);

  bool found = false;
  for (const auto &r : result.foldingRegions) {
    if (r.m_type == vte::md::Math) {
      QCOMPARE(r.m_startBlock, 0);
      QCOMPARE(r.m_endBlock, 2);
      QCOMPARE(r.m_level, 0);
      found = true;
      break;
    }
  }
  QVERIFY(found);
}

void TestASTWalker::testFoldingRegionsFrontMatter() {
  QByteArray md = "---\n"
                  "title: Test\n"
                  "---\n"
                  "content\n";
  int numBlocks = 4;
  auto result = vte::md::walkAndConvert(md, numBlocks);

  bool found = false;
  for (const auto &r : result.foldingRegions) {
    if (r.m_type == vte::md::FrontMatter) {
      QCOMPARE(r.m_startBlock, 0);
      QCOMPARE(r.m_endBlock, 2);
      QCOMPARE(r.m_level, 0);
      found = true;
      break;
    }
  }
  QVERIFY(found);
}

void TestASTWalker::testFoldingRegionsMixed() {
  // Document with heading, code block, and blockquote.
  QByteArray md = "# Title\n"   // line 1 -> block 0
                  "\n"          // line 2 -> block 1
                  "```\n"       // line 3 -> block 2
                  "code\n"      // line 4 -> block 3
                  "```\n"       // line 5 -> block 4
                  "\n"          // line 6 -> block 5
                  "> quote1\n"  // line 7 -> block 6
                  "> quote2\n"; // line 8 -> block 7
  int numBlocks = 8;
  auto result = vte::md::walkAndConvert(md, numBlocks);

  // Should have at least 3 folding regions.
  QVERIFY(result.foldingRegions.size() >= 3);

  // Verify sorted by startBlock.
  for (int i = 1; i < result.foldingRegions.size(); ++i) {
    QVERIFY(result.foldingRegions[i].m_startBlock >= result.foldingRegions[i - 1].m_startBlock);
  }

  // Check types present.
  bool hasHeading = false, hasCode = false, hasBlockquote = false;
  for (const auto &r : result.foldingRegions) {
    if (r.m_type == vte::md::Heading && r.m_startBlock == 0) {
      hasHeading = true;
    }
    if (r.m_type == vte::md::FencedCode && r.m_startBlock == 2) {
      hasCode = true;
    }
    if (r.m_type == vte::md::Blockquote && r.m_startBlock == 6) {
      hasBlockquote = true;
    }
  }
  QVERIFY(hasHeading);
  QVERIFY(hasCode);
  QVERIFY(hasBlockquote);
}

// Regression: a styled span whose LAST character is a 4-byte UTF-8 char (an emoji,
// i.e. a UTF-16 surrogate pair) must not have its end position land in the MIDDLE of
// that surrogate pair. cmark reports end_column at the last BYTE of the last char;
// converting it with toDocPosition(el, ec) + 1 under-counted by 1 QChar for astral
// chars, slicing the emoji and rendering it as two "tofu" boxes in the editor.
void TestASTWalker::testHLUnitEndingInEmoji() {
  const unsigned int STYLE_H1 = 12;
  const unsigned int STYLE_H2 = 13;

  // Helper: return the first HLUnit with the given style across all blocks.
  auto findUnit = [](const vte::md::ASTWalkResult &r,
                     unsigned int style) -> QPair<bool, vte::md::HLUnit> {
    for (const auto &block : r.blocksHighlights) {
      for (const auto &unit : block) {
        if (unit.styleIndex == style) {
          return qMakePair(true, unit);
        }
      }
    }
    return qMakePair(false, vte::md::HLUnit());
  };

  // Case 1 — the exact bug report: "## 教学原则 💎💎".
  // QChars: "## "(3) + 教学原则(4) + " "(1) + 💎(2) + 💎(2) = 12.
  // Each 💎 (U+1F48E) is F0 9F 92 8E; 教学原则 are 3-byte CJK.
  {
    QByteArray md("## \xe6\x95\x99\xe5\xad\xa6\xe5\x8e\x9f\xe5\x88\x99"
                  " \xf0\x9f\x92\x8e\xf0\x9f\x92\x8e\n");
    auto result = vte::md::walkAndConvert(md, 2);
    auto found = findUnit(result, STYLE_H2);
    QVERIFY2(found.first, "H2 HLUnit not found");
    QCOMPARE((int)found.second.start, 0);
    // Must be 12 (whole heading). The bug produced 11, ending between the two
    // surrogates of the trailing 💎.
    QCOMPARE((int)found.second.length, 12);
  }

  // Case 2 — a single trailing emoji also splits: "# 💎".
  // QChars: "# "(2) + 💎(2) = 4.
  {
    QByteArray md("# \xf0\x9f\x92\x8e\n");
    auto result = vte::md::walkAndConvert(md, 2);
    auto found = findUnit(result, STYLE_H1);
    QVERIFY2(found.first, "H1 HLUnit not found");
    QCOMPARE((int)found.second.start, 0);
    QCOMPARE((int)found.second.length, 4);
  }
}

// Cross-checks the walker's image classification against the rule
// PreviewMgr::buildImageLinksForLayout() applies to the painted preview path:
// what precedes the element on its first line and what follows it on its last
// line must both be blank. The two must not drift apart, or the same image
// would render as a block preview in one path and an inline one in the other.
//
// The multiline cases below are the interesting ones. They used to be
// satisfied by exclusion -- cmark collapsed a multiline construct onto its last
// line, so isStandaloneOnLine()'s `startLine == endLine` guard rejected them
// outright. cmark now reports the true span and the walker applies the real
// whole-span rule, so these assert the rule rather than the workaround.
void TestASTWalker::testImageStandaloneMatchesPaintedPath() {
  const QVector<QByteArray> cases{
      // Alone on one line.
      QByteArray("![alt](img.png)\n"),
      // The alt text wraps onto a second line.
      QByteArray("![alt\ntext](img.png)\n"),
      QByteArray("![alt\ntext](img.png)\ntrailing\n"),
      // Something else shares the element's line.
      QByteArray("lead ![alt\ntext](img.png)\n"),
      QByteArray("![a](\nimg.png)\n"),
      QByteArray("text ![alt](img.png) more\n"),
  };

  for (const auto &markdown : cases) {
    const int blocks = markdown.count('\n') + 1;
    auto result = vte::md::walkAndConvert(markdown, blocks);
    QCOMPARE(result.imageElements.size(), 1);

    const auto &image = result.imageElements.first();
    const QString text = QString::fromUtf8(markdown);
    QVERIFY(image.m_startPos >= 0 && image.m_endPos <= text.size());
    QVERIFY(image.m_startPos < image.m_endPos);

    // Independently evaluate the painted-path rule over the element's whole
    // span: leading text on its first line, trailing text on its last line.
    const int lineStart =
        image.m_startPos == 0 ? 0 : text.lastIndexOf(QLatin1Char('\n'), image.m_startPos - 1) + 1;
    int lineEnd = text.indexOf(QLatin1Char('\n'), image.m_endPos - 1);
    if (lineEnd < 0) {
      lineEnd = text.size();
    }

    const bool paintedStandalone =
        text.mid(lineStart, image.m_startPos - lineStart).trimmed().isEmpty() &&
        text.mid(image.m_endPos, lineEnd - image.m_endPos).trimmed().isEmpty();

    QVERIFY2(image.m_standalone == paintedStandalone, markdown.constData());
  }
}

void TestASTWalker::testTableCellOffsets() {
  //             0123456789...
  QByteArray md = "|  a |\\| b\t|\n"
                  "| - | - |\n"
                  "| \xC3\xA9\xF0\x9F\x98\x80 | |\n";
  auto result = vte::md::walkAndConvert(md, 3);
  QCOMPARE(result.tableElements.size(), 1);

  const auto &table = result.tableElements.first();
  QCOMPARE(table.m_startBlock, 0);
  QCOMPARE(table.m_rows.size(), 3);

  const QString line0 = QString::fromUtf8("|  a |\\| b\t|");
  const auto &header = table.m_rows.at(0);
  QCOMPARE(header.m_cells.size(), 2);
  QCOMPARE(header.m_cellOffsets.size(), 2);
  QCOMPARE(header.m_cells.at(0), QStringLiteral("a"));
  QCOMPARE(line0.mid(header.m_cellOffsets.at(0), header.m_cells.at(0).size()),
           header.m_cells.at(0));
  QCOMPARE(header.m_cells.at(1), QStringLiteral("\\| b"));
  QCOMPARE(line0.mid(header.m_cellOffsets.at(1), header.m_cells.at(1).size()),
           header.m_cells.at(1));

  // Surrogate-safe QChar offsets.
  const QString line2 = QString::fromUtf8("| \xC3\xA9\xF0\x9F\x98\x80 | |");
  const auto &body = table.m_rows.at(2);
  QCOMPARE(body.m_cells.size(), 2);
  QCOMPARE(line2.mid(body.m_cellOffsets.at(0), body.m_cells.at(0).size()), body.m_cells.at(0));
  QVERIFY(body.m_cells.at(1).isEmpty());
}

void TestASTWalker::testTableCellHighlights() {
  QByteArray md = "| **a** | x |\n"
                  "| - | - |\n"
                  "| t `b` | |\n";
  auto result = vte::md::walkAndConvert(md, 3);
  QCOMPARE(result.tableElements.size(), 1);

  const auto &table = result.tableElements.first();
  QCOMPARE(table.m_rows.size(), 3);

  const auto &header = table.m_rows.at(0);
  QCOMPARE(header.m_cellHighlights.size(), 2);
  QCOMPARE(header.m_cellHighlights.at(0).size(), 1);
  // Cell-local, covering exactly "**a**".
  QCOMPARE(static_cast<int>(header.m_cellHighlights.at(0).first().start), 0);
  QCOMPARE(static_cast<int>(header.m_cellHighlights.at(0).first().length), 5);
  QVERIFY(header.m_cellHighlights.at(1).isEmpty());

  const auto &body = table.m_rows.at(2);
  QCOMPARE(body.m_cellHighlights.size(), 2);
  QCOMPARE(body.m_cellHighlights.at(0).size(), 1);
  // "t `b`" - the code span starts at cell-local offset 2.
  QCOMPARE(static_cast<int>(body.m_cellHighlights.at(0).first().start), 2);
  QCOMPARE(static_cast<int>(body.m_cellHighlights.at(0).first().length), 3);

  // No row-wide table style leaks into a cell.
  for (const auto &row : table.m_rows) {
    for (int c = 0; c < row.m_cellHighlights.size(); ++c) {
      for (const auto &unit : row.m_cellHighlights.at(c)) {
        const int style = static_cast<int>(unit.styleIndex);
        QVERIFY(style != STYLE_TABLE);
        QVERIFY(style != STYLE_TABLEHEADER);
        QVERIFY(style != STYLE_BLOCKQUOTE);
        QVERIFY(unit.length > 0);
        QVERIFY(static_cast<int>(unit.start + unit.length) <= row.m_cells.at(c).size());
      }
    }
  }
}

void TestASTWalker::testTableCellHighlightsInBlockquote() {
  QByteArray md = "> | **a** | b |\n"
                  "> | - | - |\n"
                  "> | c | |\n";
  auto result = vte::md::walkAndConvert(md, 3);
  QCOMPARE(result.tableElements.size(), 1);

  const auto &table = result.tableElements.first();
  QCOMPARE(table.m_startBlock, 0);

  const QString line0 = QStringLiteral("> | **a** | b |");
  const auto &header = table.m_rows.at(0);
  QCOMPARE(header.m_prefix, QStringLiteral("> "));
  QCOMPARE(line0.mid(header.m_cellOffsets.at(0), header.m_cells.at(0).size()),
           header.m_cells.at(0));
  QCOMPARE(header.m_cellHighlights.at(0).size(), 1);
  QCOMPARE(static_cast<int>(header.m_cellHighlights.at(0).first().start), 0);
  QCOMPARE(static_cast<int>(header.m_cellHighlights.at(0).first().length), 5);
}

void TestASTWalker::testTableCellHighlightsInListAndRaggedRow() {
  QByteArray md = "- item\n"
                  "\n"
                  "  | **a** | b |\n"
                  "  | - | - |\n"
                  "  | *c* |\n";
  auto result = vte::md::walkAndConvert(md, 5);
  QCOMPARE(result.tableElements.size(), 1);

  const auto &table = result.tableElements.first();
  QCOMPARE(table.m_startBlock, 2);
  QCOMPARE(table.m_rows.size(), 3);

  const QString line = QStringLiteral("  | **a** | b |");
  const auto &header = table.m_rows.at(0);
  QCOMPARE(header.m_prefix, QStringLiteral("  "));
  QCOMPARE(line.mid(header.m_cellOffsets.at(0), header.m_cells.at(0).size()), header.m_cells.at(0));
  QCOMPARE(header.m_cellHighlights.at(0).size(), 1);
  QCOMPARE(static_cast<int>(header.m_cellHighlights.at(0).first().start), 0);
  QCOMPARE(static_cast<int>(header.m_cellHighlights.at(0).first().length), 5);

  // A ragged body row keeps its own width, and the matrices stay parallel.
  const auto &body = table.m_rows.at(2);
  QCOMPARE(body.m_cells.size(), 1);
  QCOMPARE(body.m_cellOffsets.size(), 1);
  QCOMPARE(body.m_cellHighlights.size(), 1);
  QCOMPARE(body.m_cellHighlights.at(0).size(), 1);
  QCOMPARE(static_cast<int>(body.m_cellHighlights.at(0).first().start), 0);
  QCOMPARE(static_cast<int>(body.m_cellHighlights.at(0).first().length), 3);

  // A plain row still gets one (empty) entry per cell.
  const auto &delimiter = table.m_rows.at(1);
  QCOMPARE(delimiter.m_cellHighlights.size(), delimiter.m_cells.size());
}

QTEST_MAIN(tests::TestASTWalker)
