#include "test_markdownfolding.h"

#include <QTextBlock>
#include <QTextDocument>

#include <vtextedit/markdownhighlighterdata.h>

#include <markdownfoldingprovider.h>
#include <textfolding.h>

using namespace tests;
using namespace vte;

static QString generateLines(int p_count)
{
  QString text;
  for (int i = 0; i < p_count; ++i) {
    if (i > 0) {
      text += QLatin1Char('\n');
    }
    text += QStringLiteral("Line %1").arg(i);
  }
  return text;
}

void TestMarkdownFolding::initTestCase()
{
  Q_ASSERT(!m_doc);
  m_doc = new QTextDocument(generateLines(50));
  m_textFolding = new TextFolding(m_doc);
  m_provider = new MarkdownFoldingProvider(m_textFolding, m_doc);
}

void TestMarkdownFolding::cleanupTestCase()
{
  delete m_provider;
  delete m_textFolding;
  delete m_doc;
  m_provider = nullptr;
  m_textFolding = nullptr;
  m_doc = nullptr;
}

void TestMarkdownFolding::cleanup()
{
  m_provider->clear();
}

// 1. Apply regions and verify fold ranges exist.
void TestMarkdownFolding::testApplyFoldingRegions()
{
  QVector<md::FoldingRegion> regions;
  // Heading section [0, 9].
  regions.append({0, 9, md::Heading, 1});
  // Code block [3, 7] nested inside heading.
  regions.append({3, 7, md::FencedCodeBlock, 0});

  m_provider->updateFoldingRegions(regions);

  // Verify fold range starting on block 0 exists.
  auto rangesAt0 = m_textFolding->foldingRangesStartingOnBlock(0);
  QCOMPARE(rangesAt0.size(), 1);
  QVERIFY(rangesAt0[0].second.testFlag(TextFolding::Persistent));

  // Verify fold range starting on block 3 exists.
  auto rangesAt3 = m_textFolding->foldingRangesStartingOnBlock(3);
  QCOMPARE(rangesAt3.size(), 1);
  QVERIFY(rangesAt3[0].second.testFlag(TextFolding::Persistent));
}

// 2. Re-apply same regions after folding one — fold state preserved.
void TestMarkdownFolding::testDiffPreservesFoldState()
{
  QVector<md::FoldingRegion> regions;
  regions.append({0, 9, md::Heading, 1});
  regions.append({3, 7, md::FencedCodeBlock, 0});

  m_provider->updateFoldingRegions(regions);

  // Fold the heading range [0,9].
  auto rangesAt0 = m_textFolding->foldingRangesStartingOnBlock(0);
  QCOMPARE(rangesAt0.size(), 1);
  qint64 headingId = rangesAt0[0].first;
  m_textFolding->toggleRange(headingId);

  // Blocks 1..9 should be invisible.
  auto block1 = m_doc->findBlockByNumber(1);
  QVERIFY(!block1.isVisible());

  // Re-apply the same regions (simulating re-parse).
  m_provider->updateFoldingRegions(regions);

  // The heading range should still be folded — block 1 still invisible.
  block1 = m_doc->findBlockByNumber(1);
  QVERIFY(!block1.isVisible());

  // The range at block 0 should still exist.
  rangesAt0 = m_textFolding->foldingRangesStartingOnBlock(0);
  QCOMPARE(rangesAt0.size(), 1);
}

// 3. Stale ranges removed when not in new set.
void TestMarkdownFolding::testDiffRemovesStaleRanges()
{
  QVector<md::FoldingRegion> regions;
  regions.append({0, 9, md::Heading, 1});
  regions.append({12, 19, md::Heading, 2});
  regions.append({22, 29, md::FencedCodeBlock, 0});

  m_provider->updateFoldingRegions(regions);

  // All three should exist.
  QCOMPARE(m_textFolding->foldingRangesStartingOnBlock(0).size(), 1);
  QCOMPARE(m_textFolding->foldingRangesStartingOnBlock(12).size(), 1);
  QCOMPARE(m_textFolding->foldingRangesStartingOnBlock(22).size(), 1);

  // Re-apply with only 2 regions — remove the middle one.
  QVector<md::FoldingRegion> newRegions;
  newRegions.append({0, 9, md::Heading, 1});
  newRegions.append({22, 29, md::FencedCodeBlock, 0});

  m_provider->updateFoldingRegions(newRegions);

  QCOMPARE(m_textFolding->foldingRangesStartingOnBlock(0).size(), 1);
  QCOMPARE(m_textFolding->foldingRangesStartingOnBlock(12).size(), 0);
  QCOMPARE(m_textFolding->foldingRangesStartingOnBlock(22).size(), 1);
}

// 4. New ranges added when not in old set.
void TestMarkdownFolding::testDiffAddsNewRanges()
{
  QVector<md::FoldingRegion> regions;
  regions.append({0, 9, md::Heading, 1});
  regions.append({12, 19, md::Heading, 2});

  m_provider->updateFoldingRegions(regions);

  QCOMPARE(m_textFolding->foldingRangesStartingOnBlock(0).size(), 1);
  QCOMPARE(m_textFolding->foldingRangesStartingOnBlock(12).size(), 1);
  QCOMPARE(m_textFolding->foldingRangesStartingOnBlock(22).size(), 0);

  // Re-apply with an additional region.
  QVector<md::FoldingRegion> newRegions;
  newRegions.append({0, 9, md::Heading, 1});
  newRegions.append({12, 19, md::Heading, 2});
  newRegions.append({22, 29, md::FencedCodeBlock, 0});

  m_provider->updateFoldingRegions(newRegions);

  QCOMPARE(m_textFolding->foldingRangesStartingOnBlock(0).size(), 1);
  QCOMPARE(m_textFolding->foldingRangesStartingOnBlock(12).size(), 1);
  QCOMPARE(m_textFolding->foldingRangesStartingOnBlock(22).size(), 1);
}

// 5. Regions spanning a single block are skipped.
void TestMarkdownFolding::testSkipsSmallRanges()
{
  QVector<md::FoldingRegion> regions;
  // Single-block region: startBlock == endBlock.
  regions.append({5, 5, md::Heading, 1});
  // Also test endBlock < startBlock + 1 (adjacent).
  regions.append({10, 10, md::FencedCodeBlock, 0});

  m_provider->updateFoldingRegions(regions);

  QCOMPARE(m_textFolding->foldingRangesStartingOnBlock(5).size(), 0);
  QCOMPARE(m_textFolding->foldingRangesStartingOnBlock(10).size(), 0);
}

// 6. Nested regions both created correctly.
void TestMarkdownFolding::testNesting()
{
  QVector<md::FoldingRegion> regions;
  regions.append({0, 20, md::Heading, 1});
  regions.append({5, 10, md::FencedCodeBlock, 0});

  m_provider->updateFoldingRegions(regions);

  // Outer range at block 0.
  auto rangesAt0 = m_textFolding->foldingRangesStartingOnBlock(0);
  QCOMPARE(rangesAt0.size(), 1);

  // Inner range at block 5.
  auto rangesAt5 = m_textFolding->foldingRangesStartingOnBlock(5);
  QCOMPARE(rangesAt5.size(), 1);
}

// 7. Empty regions vector produces no folds.
void TestMarkdownFolding::testEmptyRegions()
{
  QVector<md::FoldingRegion> regions;
  m_provider->updateFoldingRegions(regions);

  // Spot-check a few blocks.
  QCOMPARE(m_textFolding->foldingRangesStartingOnBlock(0).size(), 0);
  QCOMPARE(m_textFolding->foldingRangesStartingOnBlock(5).size(), 0);
}

// 8. clear() removes all markdown folds.
void TestMarkdownFolding::testClearOnDisable()
{
  QVector<md::FoldingRegion> regions;
  regions.append({0, 9, md::Heading, 1});
  regions.append({12, 19, md::Heading, 2});

  m_provider->updateFoldingRegions(regions);

  QCOMPARE(m_textFolding->foldingRangesStartingOnBlock(0).size(), 1);
  QCOMPARE(m_textFolding->foldingRangesStartingOnBlock(12).size(), 1);

  m_provider->clear();

  QCOMPARE(m_textFolding->foldingRangesStartingOnBlock(0).size(), 0);
  QCOMPARE(m_textFolding->foldingRangesStartingOnBlock(12).size(), 0);
}

QTEST_MAIN(tests::TestMarkdownFolding)
