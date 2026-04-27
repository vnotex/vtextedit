#include "test_goldenmaster.h"

#include <algorithm>

#include <QDir>
#include <QFile>
#include <QTextBlock>
#include <QTextDocument>

#include "markdownastwalker.h"

using namespace tests;

static const int NUM_HIGHLIGHT_STYLES = 33;

static const QStringList s_fixtureNames = {
    QStringLiteral("block_elements.md"),
    QStringLiteral("inline_elements.md"),
    QStringLiteral("multiline_elements.md"),
    QStringLiteral("nested_elements.md"),
    QStringLiteral("edge_cases.md"),
    QStringLiteral("extension_elements.md"),
    QStringLiteral("table_elements.md"),
    QStringLiteral("math_elements.md")};

static QString readFixture(const QString &p_name)
{
  QFile f(QStringLiteral(FIXTURES_DIR) + "/" + p_name);
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return QString();
  }
  return QString::fromUtf8(f.readAll());
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

// Serialize ASTWalkResult to elements golden format: "style:pos:end\n" lines.
// This reconstructs doc-absolute positions from block-relative HLUnits.
static QString serializeElements(const vte::md::ASTWalkResult &p_result,
                                 const QString &p_text)
{
  // Build block start positions from text.
  QVector<int> blockStarts;
  blockStarts.append(0);
  for (int i = 0; i < p_text.size(); ++i) {
    if (p_text[i] == '\n') blockStarts.append(i + 1);
  }

  QStringList lines;
  for (int blockNum = 0; blockNum < p_result.blocksHighlights.size(); ++blockNum) {
    for (const auto &unit : p_result.blocksHighlights[blockNum]) {
      unsigned long absStart =
          (blockNum < blockStarts.size() ? blockStarts[blockNum] : 0) + unit.start;
      unsigned long absEnd = absStart + unit.length;
      lines.append(
          QStringLiteral("%1:%2:%3").arg(unit.styleIndex).arg(absStart).arg(absEnd));
    }
  }
  std::sort(lines.begin(), lines.end(), [](const QString &a, const QString &b) {
    auto aParts = a.split(':');
    auto bParts = b.split(':');
    int aStyle = aParts[0].toInt();
    int bStyle = bParts[0].toInt();
    if (aStyle != bStyle) return aStyle < bStyle;
    unsigned long aPos = aParts[1].toULong();
    unsigned long bPos = bParts[1].toULong();
    return aPos < bPos;
  });
  return lines.join('\n') + '\n';
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

static bool writeGoldenFile(const QString &p_path, const QString &p_content)
{
  QFile f(p_path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
    return false;
  }
  f.write(p_content.toUtf8());
  return true;
}

static QString readGoldenFile(const QString &p_path)
{
  QFile f(p_path);
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return QString();
  }
  return QString::fromUtf8(f.readAll());
}

void TestGoldenMaster::generateGolden()
{
  QDir goldenDir(QStringLiteral(GOLDEN_DIR));
  QVERIFY(goldenDir.exists());

  for (const auto &name : s_fixtureNames) {
    QString text = readFixture(name);
    if (text.isEmpty()) {
      QSKIP(qPrintable(QStringLiteral("Fixture not found: ") + name));
    }

    QByteArray utf8 = text.toUtf8();
    int numBlocks = countBlocks(utf8);
    auto result = vte::md::walkAndConvert(utf8, numBlocks);

    QString elemSerialized = serializeElements(result, text);
    QString blocksSerialized = serializeBlocksHighlights(result.blocksHighlights);

    QString baseName = name;
    baseName.replace(QStringLiteral(".md"), QString());

    QVERIFY(writeGoldenFile(goldenDir.filePath(baseName + ".elements.golden"), elemSerialized));
    QVERIFY(writeGoldenFile(goldenDir.filePath(baseName + ".blocks.golden"), blocksSerialized));
  }
}

void TestGoldenMaster::verifyGolden()
{
  QDir goldenDir(QStringLiteral(GOLDEN_DIR));

  for (const auto &name : s_fixtureNames) {
    QString baseName = name;
    baseName.replace(QStringLiteral(".md"), QString());

    QString elemGoldenPath = goldenDir.filePath(baseName + ".elements.golden");
    QString blocksGoldenPath = goldenDir.filePath(baseName + ".blocks.golden");

    if (!QFile::exists(elemGoldenPath) || !QFile::exists(blocksGoldenPath)) {
      QSKIP("Golden files not found — run generateGolden() first");
    }

    QString text = readFixture(name);
    QVERIFY(!text.isEmpty());

    QByteArray utf8 = text.toUtf8();
    int numBlocks = countBlocks(utf8);
    auto result = vte::md::walkAndConvert(utf8, numBlocks);

    QString elemActual = serializeElements(result, text);
    QString blocksActual = serializeBlocksHighlights(result.blocksHighlights);

    QString elemExpected = readGoldenFile(elemGoldenPath);
    QString blocksExpected = readGoldenFile(blocksGoldenPath);

    QCOMPARE(elemActual, elemExpected);
    QCOMPARE(blocksActual, blocksExpected);
  }
}

QTEST_MAIN(tests::TestGoldenMaster)
