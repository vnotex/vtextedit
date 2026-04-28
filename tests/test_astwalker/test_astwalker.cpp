#include "test_astwalker.h"

#include <QDir>
#include <QFile>

#include "markdownastwalker.h"

using namespace tests;

static const QStringList s_fixtureNames = {
    QStringLiteral("block_elements.md"),
    QStringLiteral("inline_elements.md"),
    QStringLiteral("multiline_elements.md"),
    QStringLiteral("nested_elements.md"),
    QStringLiteral("edge_cases.md"),
    QStringLiteral("extension_elements.md"),
    QStringLiteral("table_elements.md"),
    QStringLiteral("math_elements.md")};

static QString readFile(const QString &p_path)
{
  QFile f(p_path);
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return QString();
  }
  return QString::fromUtf8(f.readAll());
}

static QString serializeBlocksHighlights(
    const QVector<QVector<vte::md::HLUnit>> &p_blocksHighlights)
{
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

void TestASTWalker::verifyBlocksHighlights()
{
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

void TestASTWalker::verifyRegions()
{
  // Test that walkAndConvert produces non-empty regions for relevant fixtures.
  {
    QString text = readFile(QStringLiteral(FIXTURES_DIR) + "/block_elements.md");
    QVERIFY(!text.isEmpty());
    QByteArray utf8 = text.toUtf8();
    int numBlocks = 1;
    for (int i = 0; i < utf8.size(); ++i) {
      if (utf8[i] == '\n') ++numBlocks;
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
      if (utf8[i] == '\n') ++numBlocks;
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
      if (utf8[i] == '\n') ++numBlocks;
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
      if (utf8[i] == '\n') ++numBlocks;
    }
    vte::md::ASTWalkResult result =
        vte::md::walkAndConvert(utf8, numBlocks, 0, 0, true);
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

void TestASTWalker::testFoldingRegionsHeadings()
{
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
  QCOMPARE(h1.m_startBlock, 0);  // line 1 -> block 0
  QCOMPARE(h1.m_endBlock, 0);
  QCOMPARE(h1.m_level, 1);

  const auto &h2 = result.foldingRegions[1];
  QCOMPARE(h2.m_type, vte::md::Heading);
  QCOMPARE(h2.m_startBlock, 2);  // line 3 -> block 2
  QCOMPARE(h2.m_endBlock, 2);
  QCOMPARE(h2.m_level, 2);

  const auto &h3 = result.foldingRegions[2];
  QCOMPARE(h3.m_type, vte::md::Heading);
  QCOMPARE(h3.m_startBlock, 4);  // line 5 -> block 4
  QCOMPARE(h3.m_endBlock, 4);
  QCOMPARE(h3.m_level, 3);
}

void TestASTWalker::testFoldingRegionsCodeBlock()
{
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

void TestASTWalker::testFoldingRegionsBlockquote()
{
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

void TestASTWalker::testFoldingRegionsTable()
{
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

void TestASTWalker::testFoldingRegionsMathBlock()
{
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

void TestASTWalker::testFoldingRegionsFrontMatter()
{
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

void TestASTWalker::testFoldingRegionsMixed()
{
  // Document with heading, code block, and blockquote.
  QByteArray md = "# Title\n"           // line 1 -> block 0
                  "\n"                   // line 2 -> block 1
                  "```\n"               // line 3 -> block 2
                  "code\n"              // line 4 -> block 3
                  "```\n"               // line 5 -> block 4
                  "\n"                   // line 6 -> block 5
                  "> quote1\n"          // line 7 -> block 6
                  "> quote2\n";         // line 8 -> block 7
  int numBlocks = 8;
  auto result = vte::md::walkAndConvert(md, numBlocks);

  // Should have at least 3 folding regions.
  QVERIFY(result.foldingRegions.size() >= 3);

  // Verify sorted by startBlock.
  for (int i = 1; i < result.foldingRegions.size(); ++i) {
    QVERIFY(result.foldingRegions[i].m_startBlock >=
            result.foldingRegions[i - 1].m_startBlock);
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

QTEST_MAIN(tests::TestASTWalker)
