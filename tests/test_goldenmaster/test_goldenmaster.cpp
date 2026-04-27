#include "test_goldenmaster.h"

#include <algorithm>

#include <QDir>
#include <QFile>
#include <QTextBlock>
#include <QTextDocument>

#include "cmarkadapter.h"
#include "highlightelement.h"
#include <vtextedit/markdownhighlighterdata.h>

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

static QString readFixture(const QString &p_name)
{
  QFile f(QStringLiteral(FIXTURES_DIR) + "/" + p_name);
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return QString();
  }
  return QString::fromUtf8(f.readAll());
}

static QString serializeElements(HighlightElement **p_result)
{
  QStringList lines;
  for (int i = 0; i < NUM_HIGHLIGHT_STYLES; ++i) {
    HighlightElement *elem = p_result[i];
    while (elem) {
      lines.append(QStringLiteral("%1:%2:%3").arg(i).arg(elem->pos).arg(elem->end));
      elem = elem->next;
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

static void parseBlocksHighlightOne(QVector<QVector<vte::md::HLUnit>> &p_blocksHighlights,
                                    const QTextDocument *p_doc, unsigned long p_pos,
                                    unsigned long p_end, int p_styleIndex)
{
  unsigned int nrChar = (unsigned int)p_doc->characterCount();
  if (p_end >= nrChar && nrChar > 0) {
    p_end = nrChar - 1;
  }

  QTextBlock block = p_doc->findBlock(p_pos);
  int startBlockNum = block.blockNumber();
  int endBlockNum = p_doc->findBlock(p_end - 1).blockNumber();
  if (endBlockNum >= p_blocksHighlights.size()) {
    endBlockNum = p_blocksHighlights.size() - 1;
  }

  while (block.isValid()) {
    int blockNum = block.blockNumber();
    if (blockNum > endBlockNum) {
      break;
    }

    int blockStartPos = block.position();
    vte::md::HLUnit unit;
    if (blockNum == startBlockNum) {
      unit.start = p_pos - blockStartPos;
      unit.length =
          (startBlockNum == endBlockNum) ? (p_end - p_pos) : (block.length() - unit.start);
    } else if (blockNum == endBlockNum) {
      unit.start = 0;
      unit.length = p_end - blockStartPos;
    } else {
      unit.start = 0;
      unit.length = block.length();
    }

    unit.styleIndex = p_styleIndex;

    if (unit.length > 0) {
      p_blocksHighlights[blockNum].append(unit);
    }

    block = block.next();
  }
}

static QString serializeBlocksHighlights(HighlightElement **p_result, const QString &p_text)
{
  QTextDocument doc(p_text);
  QVector<QVector<vte::md::HLUnit>> blocksHighlights(doc.blockCount());

  for (int i = 0; i < NUM_HIGHLIGHT_STYLES; ++i) {
    HighlightElement *elem = p_result[i];
    while (elem) {
      if (elem->end > elem->pos) {
        parseBlocksHighlightOne(blocksHighlights, &doc, elem->pos, elem->end, i);
      }
      elem = elem->next;
    }
  }

  // Sort each block's units by (start, length desc).
  for (auto &blockUnits : blocksHighlights) {
    std::sort(blockUnits.begin(), blockUnits.end(),
              [](const vte::md::HLUnit &a, const vte::md::HLUnit &b) {
                if (a.start != b.start) return a.start < b.start;
                return a.length > b.length;
              });
  }

  QStringList lines;
  for (int blockNum = 0; blockNum < blocksHighlights.size(); ++blockNum) {
    for (const auto &unit : blocksHighlights[blockNum]) {
      lines.append(
          QStringLiteral("%1:%2:%3:%4").arg(blockNum).arg(unit.start).arg(unit.length).arg(unit.styleIndex));
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

    HighlightElement **result = parseCmark(text.toUtf8());
    QVERIFY2(result != nullptr, qPrintable(QStringLiteral("parseCmark failed for ") + name));

    QString elemSerialized = serializeElements(result);
    QString blocksSerialized = serializeBlocksHighlights(result, text);

    QString baseName = name;
    baseName.replace(QStringLiteral(".md"), QString());

    QVERIFY(writeGoldenFile(goldenDir.filePath(baseName + ".elements.golden"), elemSerialized));
    QVERIFY(writeGoldenFile(goldenDir.filePath(baseName + ".blocks.golden"), blocksSerialized));

    freeHighlightElements(result, NUM_HIGHLIGHT_STYLES);
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

    HighlightElement **result = parseCmark(text.toUtf8());
    QVERIFY2(result != nullptr, qPrintable(QStringLiteral("parseCmark failed for ") + name));

    QString elemActual = serializeElements(result);
    QString blocksActual = serializeBlocksHighlights(result, text);

    QString elemExpected = readGoldenFile(elemGoldenPath);
    QString blocksExpected = readGoldenFile(blocksGoldenPath);

    QCOMPARE(elemActual, elemExpected);
    QCOMPARE(blocksActual, blocksExpected);

    freeHighlightElements(result, NUM_HIGHLIGHT_STYLES);
  }
}

QTEST_MAIN(tests::TestGoldenMaster)
