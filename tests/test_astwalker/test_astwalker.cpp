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

QTEST_MAIN(tests::TestASTWalker)
