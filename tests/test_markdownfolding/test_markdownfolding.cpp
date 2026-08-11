#include "test_markdownfolding.h"

#include <QImage>
#include <QPainter>
#include <QSignalSpy>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

#include <vtextedit/markdownhighlighterdata.h>
#include <vtextedit/previewdata.h>

#include <foldingregionutils.h>
#include <markdownfoldingprovider.h>
#include <textfolding.h>

#include "documentresourcemgr.h"
#include "textdocumentlayout.h"
#include "textdocumentlayoutdata.h"

using namespace tests;
using namespace vte;

static QString generateLines(int p_count) {
  QString text;
  for (int i = 0; i < p_count; ++i) {
    if (i > 0) {
      text += QLatin1Char('\n');
    }
    text += QStringLiteral("Line %1").arg(i);
  }
  return text;
}

static bool realNear(qreal p_actual, qreal p_expected) {
  return qAbs(p_actual - p_expected) < 1e-6;
}

void TestMarkdownFolding::initTestCase() {
  Q_ASSERT(!m_doc);
  m_doc = new QTextDocument(generateLines(50));
  m_textFolding = new TextFolding(m_doc);
  m_provider = new MarkdownFoldingProvider(m_textFolding, m_doc);
}

void TestMarkdownFolding::cleanupTestCase() {
  delete m_provider;
  delete m_textFolding;
  delete m_doc;
  m_provider = nullptr;
  m_textFolding = nullptr;
  m_doc = nullptr;
}

void TestMarkdownFolding::cleanup() { m_provider->clear(); }

// 1. Apply regions and verify fold ranges exist.
void TestMarkdownFolding::testApplyFoldingRegions() {
  QVector<md::FoldingRegion> regions;
  // Heading section [0, 9].
  regions.append({0, 9, md::Heading, 1});
  // Code block [3, 7] nested inside heading.
  regions.append({3, 7, md::FencedCode, 0});

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
void TestMarkdownFolding::testDiffPreservesFoldState() {
  QVector<md::FoldingRegion> regions;
  regions.append({0, 9, md::Heading, 1});
  regions.append({3, 7, md::FencedCode, 0});

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
void TestMarkdownFolding::testDiffRemovesStaleRanges() {
  QVector<md::FoldingRegion> regions;
  regions.append({0, 9, md::Heading, 1});
  regions.append({12, 19, md::Heading, 2});
  regions.append({22, 29, md::FencedCode, 0});

  m_provider->updateFoldingRegions(regions);

  // All three should exist.
  QCOMPARE(m_textFolding->foldingRangesStartingOnBlock(0).size(), 1);
  QCOMPARE(m_textFolding->foldingRangesStartingOnBlock(12).size(), 1);
  QCOMPARE(m_textFolding->foldingRangesStartingOnBlock(22).size(), 1);

  // Re-apply with only 2 regions — remove the middle one.
  QVector<md::FoldingRegion> newRegions;
  newRegions.append({0, 9, md::Heading, 1});
  newRegions.append({22, 29, md::FencedCode, 0});

  m_provider->updateFoldingRegions(newRegions);

  QCOMPARE(m_textFolding->foldingRangesStartingOnBlock(0).size(), 1);
  QCOMPARE(m_textFolding->foldingRangesStartingOnBlock(12).size(), 0);
  QCOMPARE(m_textFolding->foldingRangesStartingOnBlock(22).size(), 1);
}

// 4. New ranges added when not in old set.
void TestMarkdownFolding::testDiffAddsNewRanges() {
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
  newRegions.append({22, 29, md::FencedCode, 0});

  m_provider->updateFoldingRegions(newRegions);

  QCOMPARE(m_textFolding->foldingRangesStartingOnBlock(0).size(), 1);
  QCOMPARE(m_textFolding->foldingRangesStartingOnBlock(12).size(), 1);
  QCOMPARE(m_textFolding->foldingRangesStartingOnBlock(22).size(), 1);
}

// 5. Regions spanning a single block are skipped.
void TestMarkdownFolding::testSkipsSmallRanges() {
  QVector<md::FoldingRegion> regions;
  // Single-block region: startBlock == endBlock.
  regions.append({5, 5, md::Heading, 1});
  // Also test endBlock < startBlock + 1 (adjacent).
  regions.append({10, 10, md::FencedCode, 0});

  m_provider->updateFoldingRegions(regions);

  QCOMPARE(m_textFolding->foldingRangesStartingOnBlock(5).size(), 0);
  QCOMPARE(m_textFolding->foldingRangesStartingOnBlock(10).size(), 0);
}

// 6. Nested regions both created correctly.
void TestMarkdownFolding::testNesting() {
  QVector<md::FoldingRegion> regions;
  regions.append({0, 20, md::Heading, 1});
  regions.append({5, 10, md::FencedCode, 0});

  m_provider->updateFoldingRegions(regions);

  // Outer range at block 0.
  auto rangesAt0 = m_textFolding->foldingRangesStartingOnBlock(0);
  QCOMPARE(rangesAt0.size(), 1);

  // Inner range at block 5.
  auto rangesAt5 = m_textFolding->foldingRangesStartingOnBlock(5);
  QCOMPARE(rangesAt5.size(), 1);
}

// 7. Empty regions vector produces no folds.
void TestMarkdownFolding::testEmptyRegions() {
  QVector<md::FoldingRegion> regions;
  m_provider->updateFoldingRegions(regions);

  // Spot-check a few blocks.
  QCOMPARE(m_textFolding->foldingRangesStartingOnBlock(0).size(), 0);
  QCOMPARE(m_textFolding->foldingRangesStartingOnBlock(5).size(), 0);
}

// 8. clear() removes all markdown folds.
void TestMarkdownFolding::testClearOnDisable() {
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

// --- Heading section tests ---

// Helper to find a FoldingRegion by type and startBlock.
static const md::FoldingRegion *findRegion(const QVector<md::FoldingRegion> &p_regions,
                                           md::FoldingRegionType p_type, int p_startBlock) {
  for (const auto &r : p_regions) {
    if (r.m_type == p_type && r.m_startBlock == p_startBlock) {
      return &r;
    }
  }
  return nullptr;
}

// 9. Single heading extends to end of document.
void TestMarkdownFolding::testHeadingSectionBasic() {
  const int numBlocks = 20;
  QVector<md::FoldingRegion> regions;
  // AST walker produces heading with single-line range; endBlock is placeholder.
  regions.append({0, 0, md::Heading, 2});

  md::computeHeadingSections(regions, numBlocks);

  QCOMPARE(regions.size(), 1);
  auto *h = findRegion(regions, md::Heading, 0);
  QVERIFY(h != nullptr);
  QCOMPARE(h->m_endBlock, numBlocks - 1);
}

// 10. Two same-level headings: first section ends before second.
void TestMarkdownFolding::testHeadingSectionMultiple() {
  const int numBlocks = 20;
  QVector<md::FoldingRegion> regions;
  regions.append({0, 0, md::Heading, 2});
  regions.append({5, 5, md::Heading, 2});

  md::computeHeadingSections(regions, numBlocks);

  QCOMPARE(regions.size(), 2);
  auto *h0 = findRegion(regions, md::Heading, 0);
  auto *h5 = findRegion(regions, md::Heading, 5);
  QVERIFY(h0 != nullptr);
  QVERIFY(h5 != nullptr);
  QCOMPARE(h0->m_endBlock, 4);
  QCOMPARE(h5->m_endBlock, numBlocks - 1);
}

// 11. Nested headings: H1 contains H2, next H1 terminates both.
void TestMarkdownFolding::testHeadingSectionNested() {
  const int numBlocks = 20;
  QVector<md::FoldingRegion> regions;
  regions.append({0, 0, md::Heading, 1});   // H1
  regions.append({2, 2, md::Heading, 2});   // H2
  regions.append({10, 10, md::Heading, 1}); // H1

  md::computeHeadingSections(regions, numBlocks);

  QCOMPARE(regions.size(), 3);
  auto *h1a = findRegion(regions, md::Heading, 0);
  auto *h2 = findRegion(regions, md::Heading, 2);
  auto *h1b = findRegion(regions, md::Heading, 10);
  QVERIFY(h1a != nullptr);
  QVERIFY(h2 != nullptr);
  QVERIFY(h1b != nullptr);
  QCOMPARE(h1a->m_endBlock, 9);
  QCOMPARE(h2->m_endBlock, 9);
  QCOMPARE(h1b->m_endBlock, numBlocks - 1);
}

// 12. Heading section spanning only 1 block is filtered out.
void TestMarkdownFolding::testHeadingSectionTooSmall() {
  const int numBlocks = 20;
  QVector<md::FoldingRegion> regions;
  regions.append({5, 5, md::Heading, 2});
  regions.append({6, 6, md::Heading, 2});

  md::computeHeadingSections(regions, numBlocks);

  // First heading [5,5] has endBlock = 5 (next same-level is block 6, so 6-1=5).
  // Section size = 5-5 = 0 < 1, so filtered out.
  // Second heading [6,19] remains.
  QCOMPARE(regions.size(), 1);
  auto *h6 = findRegion(regions, md::Heading, 6);
  QVERIFY(h6 != nullptr);
  QCOMPARE(h6->m_endBlock, numBlocks - 1);
}

// 13. Heading near end of document extends to last block.
void TestMarkdownFolding::testHeadingSectionAtEnd() {
  const int numBlocks = 20;
  QVector<md::FoldingRegion> regions;
  regions.append({15, 15, md::Heading, 3});

  md::computeHeadingSections(regions, numBlocks);

  QCOMPARE(regions.size(), 1);
  auto *h = findRegion(regions, md::Heading, 15);
  QVERIFY(h != nullptr);
  QCOMPARE(h->m_endBlock, 19);
}

// 14. Heading inside a blockquote is NOT converted to a section fold.
void TestMarkdownFolding::testHeadingSectionInsideBlockquote() {
  const int numBlocks = 20;
  QVector<md::FoldingRegion> regions;
  // Blockquote spanning [2, 8].
  regions.append({2, 8, md::Blockquote, 0});
  // Heading at block 3 inside the blockquote.
  regions.append({3, 3, md::Heading, 2});

  md::computeHeadingSections(regions, numBlocks);

  // The heading section [3, 19] is inside blockquote [2, 8]?
  // No — heading section [3, 19] is NOT fully inside [2, 8].
  // Need to adjust: heading fully inside blockquote means heading section is also inside.
  // The algorithm checks h.m_startBlock >= bq.m_startBlock && h.m_endBlock <= bq.m_endBlock.
  // Here h.m_endBlock = 19 > bq.m_endBlock = 8, so it's NOT filtered.
  // To test blockquote filtering, the heading section must be fully inside.
  // Use two headings so the first's section is bounded.

  // Reset and redo with proper setup.
  regions.clear();
  regions.append({2, 12, md::Blockquote, 0});
  // Heading at block 3, next same-level heading at block 8 -> section [3, 7].
  regions.append({3, 3, md::Heading, 2});
  regions.append({8, 8, md::Heading, 2});

  md::computeHeadingSections(regions, numBlocks);

  // Heading at block 3: section [3, 7], fully inside blockquote [2, 12] -> filtered.
  // Heading at block 8: section [8, 19], NOT fully inside [2, 12] -> kept.
  // Blockquote itself remains.
  auto *hFiltered = findRegion(regions, md::Heading, 3);
  QVERIFY(hFiltered == nullptr);

  auto *hKept = findRegion(regions, md::Heading, 8);
  QVERIFY(hKept != nullptr);
  QCOMPARE(hKept->m_endBlock, numBlocks - 1);

  // Blockquote is preserved.
  auto *bq = findRegion(regions, md::Blockquote, 2);
  QVERIFY(bq != nullptr);
}

// Integration: heading section computation feeds into provider.
void TestMarkdownFolding::testEndToEndFolding() {
  // Simulate full pipeline: raw heading regions -> computeHeadingSections -> provider.
  const int numBlocks = 20;
  QVector<md::FoldingRegion> regions;
  // Raw heading lines (as produced by AST walker).
  regions.append({0, 0, md::Heading, 1});   // H1
  regions.append({3, 3, md::Heading, 2});   // H2
  regions.append({10, 10, md::Heading, 1}); // H1
  // A code block inside the first section.
  regions.append({5, 7, md::FencedCode, 0});

  // Run heading section computation.
  md::computeHeadingSections(regions, numBlocks);

  // Verify heading sections were computed correctly.
  // H1 at 0 -> section [0, 9] (before next H1 at 10)
  // H2 at 3 -> section [3, 9] (before next same-or-higher at 10)
  // H1 at 10 -> section [10, 19] (end of doc)
  // Code block [5, 7] unchanged.

  // Apply to provider.
  m_provider->updateFoldingRegions(regions);

  // Verify all 4 ranges were created (3 headings + 1 code block).
  // Check heading at block 0.
  auto ranges0 = m_textFolding->foldingRangesStartingOnBlock(0);
  QCOMPARE(ranges0.size(), 1);

  // Check heading at block 3.
  auto ranges3 = m_textFolding->foldingRangesStartingOnBlock(3);
  QCOMPARE(ranges3.size(), 1);

  // Check code block at block 5.
  auto ranges5 = m_textFolding->foldingRangesStartingOnBlock(5);
  QCOMPARE(ranges5.size(), 1);

  // Check heading at block 10.
  auto ranges10 = m_textFolding->foldingRangesStartingOnBlock(10);
  QCOMPARE(ranges10.size(), 1);
}

// 16. Folding sets hidden blocks to zero-height rects; unfolding restores them.
void TestMarkdownFolding::testFoldingBlockHeights() {
  // Use a standalone document + layout for this test (not the shared m_doc).
  QTextDocument doc(generateLines(25));
  DocumentResourceMgr resourceMgr;
  auto *layout = new TextDocumentLayout(&doc, &resourceMgr);
  doc.setDocumentLayout(layout);

  TextFolding folding(&doc);

  // Force initial layout by querying document size.
  qreal preFoldHeight = layout->documentSize().height();
  QVERIFY(preFoldHeight > 0);

  // Verify all blocks have positive height before folding.
  for (int i = 0; i < 25; ++i) {
    auto block = doc.findBlockByNumber(i);
    auto info = BlockLayoutData::get(block);
    QVERIFY2(info->m_rect.height() > 0,
             qPrintable(QStringLiteral("Block %1 has zero height before fold").arg(i)));
  }

  // Create a persistent fold range spanning blocks 2-10.
  QTextBlock startBlock = doc.findBlockByNumber(2);
  QTextBlock endBlock = doc.findBlockByNumber(10);
  TextBlockRange range(startBlock, endBlock);
  auto id = folding.newFoldingRange(range, TextFolding::Persistent);
  QVERIFY(id != TextFolding::InvalidRangeId);

  // Fold it.
  folding.toggleRange(id);

  // Hidden interior blocks (3-9) should have zero-height rects.
  for (int i = 3; i <= 9; ++i) {
    auto block = doc.findBlockByNumber(i);
    auto info = BlockLayoutData::get(block);
    QCOMPARE(info->m_rect.height(), 0.0);
  }

  // Both fold endpoints should still have positive height.
  for (int i : {2, 10}) {
    auto info = BlockLayoutData::get(doc.findBlockByNumber(i));
    QVERIFY(info->m_rect.height() > 0);
  }

  // Document height should have decreased.
  qreal foldedHeight = layout->documentSize().height();
  QVERIFY(foldedHeight < preFoldHeight);

  // Unfold.
  folding.toggleRange(id);

  // All blocks in the range should be restored to positive height.
  for (int i = 2; i <= 10; ++i) {
    auto block = doc.findBlockByNumber(i);
    auto info = BlockLayoutData::get(block);
    QVERIFY2(info->m_rect.height() > 0,
             qPrintable(QStringLiteral("Block %1 has zero height after unfold").arg(i)));
  }

  // Document height should be restored.
  qreal unfoldedHeight = layout->documentSize().height();
  QVERIFY(unfoldedHeight > foldedHeight);
  QVERIFY(qFuzzyCompare(unfoldedHeight, preFoldHeight));
}

void TestMarkdownFolding::testFractionalBlockCoordinates() {
  QTextDocument doc(generateLines(10));
  doc.setTextWidth(600);
  DocumentResourceMgr resourceMgr;
  auto *layout = new TextDocumentLayout(&doc, &resourceMgr);
  doc.setDocumentLayout(layout);
  layout->setLeadingSpaceOfLine(3.28);
  layout->relayout();

  for (int i = 0; i < doc.blockCount(); ++i) {
    QTextBlock block = doc.findBlockByNumber(i);
    const auto info = BlockLayoutData::get(block);
    const QPointF point(doc.documentMargin(), info->top() + 0.25);
    QCOMPARE(layout->findBlockByPosition(point), i);
    QCOMPARE(layout->hitTest(point, Qt::FuzzyHit), block.position());
  }
}

void TestMarkdownFolding::testFractionalClipDraw() {
  QTextDocument doc(QStringLiteral("First block\nSecond block"));
  doc.setTextWidth(200);

  QTextCursor cursor(doc.findBlockByNumber(1));
  QTextBlockFormat format;
  format.setBackground(Qt::green);
  cursor.setBlockFormat(format);

  DocumentResourceMgr resourceMgr;
  auto *layout = new TextDocumentLayout(&doc, &resourceMgr);
  doc.setDocumentLayout(layout);
  layout->setLeadingSpaceOfLine(3.28);
  layout->relayout();

  const qreal secondTop = BlockLayoutData::get(doc.findBlockByNumber(1))->top();
  QVERIFY(!realNear(secondTop, qFloor(secondTop)));

  QImage image(220, qCeil(layout->documentSize().height()) + 10, QImage::Format_ARGB32);
  image.fill(Qt::white);
  QPainter painter(&image);
  const qreal clipBottom = (secondTop + qCeil(secondTop)) / 2;
  QVERIFY(clipBottom > secondTop);
  QVERIFY(qFloor(clipBottom) < secondTop);
  const QRectF clip(0, 0, image.width(), clipBottom);
  painter.setClipRect(clip);

  QAbstractTextDocumentLayout::PaintContext context;
  context.clip = clip;
  layout->draw(&painter, context);
  painter.end();

  const int sampleX = qFloor(doc.documentMargin()) + 2;
  const int sampleY = qFloor(secondTop);
  QCOMPARE(image.pixelColor(sampleX, sampleY), QColor(Qt::green));
}

void TestMarkdownFolding::testDocumentSizeSignals() {
  QTextDocument doc(generateLines(10));
  doc.setTextWidth(600);
  DocumentResourceMgr resourceMgr;
  auto *layout = new TextDocumentLayout(&doc, &resourceMgr);
  doc.setDocumentLayout(layout);
  layout->setLeadingSpaceOfLine(3.28);
  layout->relayout();

  const QSizeF originalSize = layout->documentSize();
  QSignalSpy sizeSpy(layout, SIGNAL(documentSizeChanged(QSizeF)));
  layout->relayout();
  QCOMPARE(layout->documentSize(), originalSize);
  QCOMPARE(sizeSpy.count(), 0);

  layout->setLeadingSpaceOfLine(4.28);
  layout->relayout();
  QVERIFY(layout->documentSize() != originalSize);
  QCOMPARE(sizeSpy.count(), 1);
}

void TestMarkdownFolding::testWrappedInlinePreviewCoordinates() {
  QTextDocument doc(QString(160, QLatin1Char('x')));
  doc.setTextWidth(120);
  DocumentResourceMgr resourceMgr;
  auto *layout = new TextDocumentLayout(&doc, &resourceMgr);
  doc.setDocumentLayout(layout);
  layout->relayout();

  QTextBlock block = doc.firstBlock();
  QTextLayout *textLayout = block.layout();
  QVERIFY(textLayout->lineCount() >= 3);
  const int start = textLayout->lineAt(0).textStart() + 1;
  const int end = textLayout->lineAt(2).textStart() + 2;
  auto previewData = BlockPreviewData::get(block);
  previewData->insert(new PreviewData(PreviewData::ImageLink, 1, start, end, 0, true,
                                      QStringLiteral("wrapped-image"), QSize(100, 40), 0));

  layout->setPreviewEnabled(true);
  textLayout = block.layout();
  QVERIFY(textLayout->lineCount() >= 3);

  const auto info = BlockLayoutData::get(block);
  QCOMPARE(info->m_markers.size(), 4);
  const QTextLine continuationLine = textLayout->lineAt(1);
  const QTextLine endingLine = textLayout->lineAt(2);
  QVERIFY(
      realNear(info->m_markers.at(1).m_end.x(), continuationLine.x() + continuationLine.width()));
  QVERIFY(realNear(info->m_markers.at(2).m_end.x(), endingLine.cursorToX(end)));
}

void TestMarkdownFolding::testMalformedPreviewData() {
  QTextDocument doc(QStringLiteral("Preview data"));
  DocumentResourceMgr resourceMgr;
  auto *layout = new TextDocumentLayout(&doc, &resourceMgr);
  doc.setDocumentLayout(layout);

  auto previewData = BlockPreviewData::get(doc.firstBlock());
  QVERIFY(!previewData->insert(nullptr));
  QCOMPARE(previewData->getPreviewData().size(), 0);
  QVERIFY(!previewData->insert(new PreviewData()));
  QCOMPARE(previewData->getPreviewData().size(), 0);

  layout->setPreviewEnabled(true);
  QVERIFY(layout->documentSize().height() > 0);
}

void TestMarkdownFolding::testCursorWidthPaintOnly() {
  QTextDocument doc(QString(120, QLatin1Char('x')) + QStringLiteral("\nfolded\nlast"));
  doc.setTextWidth(120);
  DocumentResourceMgr resourceMgr;
  auto *layout = new TextDocumentLayout(&doc, &resourceMgr);
  doc.setDocumentLayout(layout);
  doc.findBlockByNumber(1).setVisible(false);
  layout->relayout();

  QVector<QPair<int, int>> lineBreaks;
  QTextBlock block = doc.firstBlock();
  for (int i = 0; i < block.layout()->lineCount(); ++i) {
    const QTextLine line = block.layout()->lineAt(i);
    lineBreaks.append(qMakePair(line.textStart(), line.textLength()));
  }

  QVector<QRectF> blockRects;
  for (block = doc.firstBlock(); block.isValid(); block = block.next()) {
    blockRects.append(BlockLayoutData::get(block)->m_rect);
  }
  const QSizeF documentSize = layout->documentSize();

  QSignalSpy updateSpy(layout, SIGNAL(update(QRectF)));
  QSignalSpy sizeSpy(layout, SIGNAL(documentSizeChanged(QSizeF)));
  const int newWidth = layout->cursorWidth() + 10;
  layout->setCursorWidth(newWidth);
  QCOMPARE(updateSpy.count(), 1);
  QCOMPARE(sizeSpy.count(), 0);

  updateSpy.clear();
  layout->setCursorWidth(newWidth);
  QCOMPARE(updateSpy.count(), 0);
  QCOMPARE(sizeSpy.count(), 0);

  layout->relayout();
  QCOMPARE(sizeSpy.count(), 0);
  QCOMPARE(layout->documentSize(), documentSize);

  QVector<QPair<int, int>> newLineBreaks;
  block = doc.firstBlock();
  for (int i = 0; i < block.layout()->lineCount(); ++i) {
    const QTextLine line = block.layout()->lineAt(i);
    newLineBreaks.append(qMakePair(line.textStart(), line.textLength()));
  }
  QCOMPARE(newLineBreaks, lineBreaks);

  int blockNumber = 0;
  for (block = doc.firstBlock(); block.isValid(); block = block.next(), ++blockNumber) {
    QCOMPARE(BlockLayoutData::get(block)->m_rect, blockRects.at(blockNumber));
  }
}

void TestMarkdownFolding::testExactHitTesting() {
  QTextDocument doc(QStringLiteral("Exact hit text"));
  doc.setTextWidth(300);
  DocumentResourceMgr resourceMgr;
  auto *layout = new TextDocumentLayout(&doc, &resourceMgr);
  doc.setDocumentLayout(layout);
  layout->setLeadingSpaceOfLine(8.5);
  layout->relayout();

  const QTextBlock block = doc.firstBlock();
  const QTextLine line = block.layout()->lineAt(0);
  const QRectF textRect = line.naturalTextRect().translated(doc.documentMargin(), 0);
  const qreal blockTop = BlockLayoutData::get(block)->top();
  const QPointF interior(textRect.center().x(), blockTop + textRect.center().y());
  QVERIFY(layout->hitTest(interior, Qt::ExactHit) >= block.position());

  const QVector<QPointF> outsidePoints = {
      QPointF(textRect.left() - 2, blockTop + textRect.center().y()),
      QPointF(textRect.right() + 2, blockTop + textRect.center().y()),
      QPointF(textRect.center().x(), blockTop + line.naturalTextRect().top() / 2)};
  for (const auto &point : outsidePoints) {
    QCOMPARE(layout->hitTest(point, Qt::ExactHit), -1);
    QVERIFY(layout->hitTest(point, Qt::FuzzyHit) >= 0);
  }
}

// A click in the leading space above a line must still honor the x coordinate
// instead of collapsing to the start of the block.
void TestMarkdownFolding::testFuzzyHitInLeadingSpace() {
  QTextDocument doc(QStringLiteral("Leading space hit testing"));
  doc.setTextWidth(400);
  DocumentResourceMgr resourceMgr;
  auto *layout = new TextDocumentLayout(&doc, &resourceMgr);
  doc.setDocumentLayout(layout);
  layout->setLeadingSpaceOfLine(6);
  layout->relayout();

  const QTextBlock block = doc.firstBlock();
  const QTextLine line = block.layout()->lineAt(0);
  const QRectF lr = line.naturalTextRect();
  QVERIFY(lr.top() > 0);

  const qreal blockTop = BlockLayoutData::get(block)->top();
  const qreal localX = lr.center().x();
  // Vertically inside the leading space, above the line's natural text rect.
  const QPointF point(localX + doc.documentMargin(), blockTop + lr.top() / 2);

  const int expected = block.position() + line.xToCursor(localX, QTextLine::CursorBetweenCharacters);
  QVERIFY(expected > block.position());
  QCOMPARE(layout->hitTest(point, Qt::FuzzyHit), expected);
}

// A click in the gap between two wrapped lines must resolve horizontally within
// the nearest line instead of returning the line boundary.
void TestMarkdownFolding::testFuzzyHitInWrappedLineGap() {
  QTextDocument doc(QString(160, QLatin1Char('x')));
  doc.setTextWidth(120);
  DocumentResourceMgr resourceMgr;
  auto *layout = new TextDocumentLayout(&doc, &resourceMgr);
  doc.setDocumentLayout(layout);
  layout->setLeadingSpaceOfLine(6);
  layout->relayout();

  const QTextBlock block = doc.firstBlock();
  QTextLayout *textLayout = block.layout();
  QVERIFY(textLayout->lineCount() >= 2);

  const QTextLine firstLine = textLayout->lineAt(0);
  const QTextLine secondLine = textLayout->lineAt(1);
  const QRectF secondRect = secondLine.naturalTextRect();
  // The leading space opens a real gap between consecutive wrapped lines.
  QVERIFY(secondRect.top() > firstLine.naturalTextRect().bottom());

  const qreal blockTop = BlockLayoutData::get(block)->top();
  const qreal localX = secondRect.center().x();
  // Just above the second line, inside the inter-line gap.
  const QPointF point(localX + doc.documentMargin(), blockTop + secondRect.top() - 0.25);

  const int expected =
      block.position() + secondLine.xToCursor(localX, QTextLine::CursorBetweenCharacters);
  QVERIFY(expected > block.position() + secondLine.textStart());
  QCOMPARE(layout->hitTest(point, Qt::FuzzyHit), expected);
}

// A click below every line of a block, such as the block preview image area,
// keeps the historical end-of-block behavior.
void TestMarkdownFolding::testFuzzyHitBelowLastLine() {
  QTextDocument doc(QStringLiteral("Below the last line"));
  doc.setTextWidth(400);
  DocumentResourceMgr resourceMgr;
  auto *layout = new TextDocumentLayout(&doc, &resourceMgr);
  doc.setDocumentLayout(layout);
  layout->setLeadingSpaceOfLine(6);
  layout->relayout();

  const QTextBlock block = doc.firstBlock();
  QTextLayout *textLayout = block.layout();
  const QTextLine lastLine = textLayout->lineAt(textLayout->lineCount() - 1);
  const QRectF lr = lastLine.naturalTextRect();

  const qreal blockTop = BlockLayoutData::get(block)->top();
  const QPointF point(lr.center().x() + doc.documentMargin(), blockTop + lr.bottom() + 5);

  const int expected = block.position() + lastLine.textStart() + lastLine.textLength();
  QCOMPARE(layout->hitTest(point, Qt::FuzzyHit), expected);
}

// Without leading space consecutive lines are tightly packed. This is a
// non-regression guard: a point exactly on a line's top edge belongs to that
// line, not to the preceding one.
void TestMarkdownFolding::testFuzzyHitAtLineBoundary() {
  QTextDocument doc(QString(160, QLatin1Char('x')));
  doc.setTextWidth(120);
  DocumentResourceMgr resourceMgr;
  auto *layout = new TextDocumentLayout(&doc, &resourceMgr);
  doc.setDocumentLayout(layout);
  layout->setLeadingSpaceOfLine(0);
  layout->relayout();

  const QTextBlock block = doc.firstBlock();
  QTextLayout *textLayout = block.layout();
  QVERIFY(textLayout->lineCount() >= 2);

  const QTextLine secondLine = textLayout->lineAt(1);
  const QRectF secondRect = secondLine.naturalTextRect();
  QVERIFY(secondRect.top() >= textLayout->lineAt(0).naturalTextRect().bottom());

  const qreal blockTop = BlockLayoutData::get(block)->top();
  const qreal localX = secondRect.center().x();
  const QPointF point(localX + doc.documentMargin(), blockTop + secondRect.top());

  const int expected =
      block.position() + secondLine.xToCursor(localX, QTextLine::CursorBetweenCharacters);
  QVERIFY(expected > block.position() + secondLine.textStart());
  QCOMPARE(layout->hitTest(point, Qt::FuzzyHit), expected);
}

// Inside plain leading space the nearest line wins, including the line above.
void TestMarkdownFolding::testFuzzyHitNearerPreviousLine() {
  QTextDocument doc(QString(160, QLatin1Char('x')));
  doc.setTextWidth(120);
  DocumentResourceMgr resourceMgr;
  auto *layout = new TextDocumentLayout(&doc, &resourceMgr);
  doc.setDocumentLayout(layout);
  layout->setLeadingSpaceOfLine(6);
  layout->relayout();

  const QTextBlock block = doc.firstBlock();
  QTextLayout *textLayout = block.layout();
  QVERIFY(textLayout->lineCount() >= 2);

  const QTextLine firstLine = textLayout->lineAt(0);
  const QRectF firstRect = firstLine.naturalTextRect();
  const QRectF secondRect = textLayout->lineAt(1).naturalTextRect();
  QVERIFY(secondRect.top() > firstRect.bottom());

  const qreal blockTop = BlockLayoutData::get(block)->top();
  const qreal localX = firstRect.center().x();
  // Just below the first line, i.e. nearer to it than to the second one.
  const QPointF point(localX + doc.documentMargin(), blockTop + firstRect.bottom() + 0.25);

  const int expected =
      block.position() + firstLine.xToCursor(localX, QTextLine::CursorBetweenCharacters);
  QVERIFY(expected < block.position() + firstLine.textStart() + firstLine.textLength());
  QCOMPARE(layout->hitTest(point, Qt::FuzzyHit), expected);
}

// A point above the whole document keeps resolving to the document start so
// that dragging a selection past the top still selects to the beginning.
void TestMarkdownFolding::testFuzzyHitAboveDocument() {
  QTextDocument doc(generateLines(5));
  doc.setTextWidth(400);
  DocumentResourceMgr resourceMgr;
  auto *layout = new TextDocumentLayout(&doc, &resourceMgr);
  doc.setDocumentLayout(layout);
  layout->setLeadingSpaceOfLine(6);
  layout->relayout();

  const QTextBlock block = doc.firstBlock();
  const QRectF lr = block.layout()->lineAt(0).naturalTextRect();
  const QPointF point(lr.center().x() + doc.documentMargin(), -20);

  QCOMPARE(layout->hitTest(point, Qt::FuzzyHit), block.position());
}

// The space an inline preview image occupies belongs to the line owning the
// image, not to the preceding line, however close the point is to the latter.
void TestMarkdownFolding::testFuzzyHitInInlinePreviewGap() {
  QTextDocument doc(QString(220, QLatin1Char('x')));
  doc.setTextWidth(120);
  DocumentResourceMgr resourceMgr;
  auto *layout = new TextDocumentLayout(&doc, &resourceMgr);
  doc.setDocumentLayout(layout);
  layout->setLeadingSpaceOfLine(6);
  layout->relayout();

  QTextBlock block = doc.firstBlock();
  QTextLayout *textLayout = block.layout();
  QVERIFY(textLayout->lineCount() >= 3);

  // Anchor the preview on the second visual line so that the image space is
  // laid out above a line that has a predecessor.
  const int start = textLayout->lineAt(1).textStart() + 1;
  const int end = textLayout->lineAt(2).textStart() + 2;
  BlockPreviewData::get(block)->insert(new PreviewData(PreviewData::ImageLink, 1, start, end, 0,
                                                       true, QStringLiteral("inline-gap-image"),
                                                       QSize(100, 60), 0));
  layout->setPreviewEnabled(true);

  block = doc.firstBlock();
  textLayout = block.layout();
  QVERIFY(textLayout->lineCount() >= 3);

  const QTextLine firstLine = textLayout->lineAt(0);
  const QTextLine imageLine = textLayout->lineAt(1);
  const QRectF firstRect = firstLine.naturalTextRect();
  const QRectF imageRect = imageLine.naturalTextRect();
  // The gap holds the image and is therefore wider than plain leading space.
  QVERIFY(imageRect.top() - firstRect.bottom() > layout->getLeadingSpaceOfLine() + 1);

  const qreal blockTop = BlockLayoutData::get(block)->top();
  const qreal localX = imageRect.center().x();
  // Deep inside the image area but much nearer to the preceding line.
  const QPointF point(localX + doc.documentMargin(), blockTop + firstRect.bottom() + 1);

  const int expected =
      block.position() + imageLine.xToCursor(localX, QTextLine::CursorBetweenCharacters);
  const int previousLineResult =
      block.position() + firstLine.xToCursor(localX, QTextLine::CursorBetweenCharacters);
  QVERIFY(expected != previousLineResult);
  QCOMPARE(layout->hitTest(point, Qt::FuzzyHit), expected);
}

// ---------------------------------------------------------------------------
// Interactive preview widget reservations.
// ---------------------------------------------------------------------------

static TextDocumentLayout::WidgetPreviewSpec makeSpec(quint64 p_id, int p_start, int p_end,
                                                      qreal p_width, qreal p_height,
                                                      PreviewPlacement p_placement) {
  TextDocumentLayout::WidgetPreviewSpec spec;
  spec.m_id = p_id;
  spec.m_startPos = p_start;
  spec.m_endPos = p_end;
  spec.m_width = p_width;
  spec.m_height = p_height;
  spec.m_placement = p_placement;
  return spec;
}

void TestMarkdownFolding::testWidgetPreviewBlockReservation() {
  QTextDocument doc(generateLines(5));
  DocumentResourceMgr resourceMgr;
  auto *layout = new TextDocumentLayout(&doc, &resourceMgr);
  doc.setDocumentLayout(layout);
  layout->setPreviewEnabled(true);

  const QTextBlock block = doc.findBlockByNumber(1);
  const qreal plainHeight = BlockLayoutData::get(block)->m_rect.height();
  const qreal plainDocHeight = layout->documentSize().height();

  QSignalSpy geometrySpy(layout, &TextDocumentLayout::widgetPreviewGeometryChanged);

  QVector<TextDocumentLayout::WidgetPreviewSpec> specs;
  specs.append(makeSpec(7, block.position(), block.position() + block.length() - 1, 120, 40,
                        PreviewPlacement::BlockAfterSource));
  layout->setWidgetPreviews(specs);

  auto info = BlockLayoutData::get(block);
  QCOMPARE(info->m_widgets.size(), 1);
  QCOMPARE(info->m_widgets.first().m_id, quint64(7));
  QCOMPARE(info->m_widgets.first().m_rect.height(), 40.0);
  QCOMPARE(info->m_widgets.first().m_rect.width(), 120.0);
  QVERIFY(info->m_rect.height() > plainHeight + 39);
  QVERIFY(layout->documentSize().height() > plainDocHeight + 39);
  QVERIFY(geometrySpy.count() >= 1);

  // The published rect is in document coordinates.
  const QRectF docRect = layout->widgetPreviewRect(7);
  QVERIFY(!docRect.isNull());
  QCOMPARE(docRect.top(), info->top() + info->m_widgets.first().m_rect.top());
  QCOMPARE(docRect.left(), doc.documentMargin());

  // Removing the reservation restores the original geometry.
  layout->setWidgetPreviews(QVector<TextDocumentLayout::WidgetPreviewSpec>());
  QVERIFY(BlockLayoutData::get(block)->m_widgets.isEmpty());
  QCOMPARE(BlockLayoutData::get(block)->m_rect.height(), plainHeight);
  QVERIFY(layout->widgetPreviewRect(7).isNull());
}

void TestMarkdownFolding::testWidgetPreviewStacking() {
  QTextDocument doc(QStringLiteral("alpha beta gamma\nnext"));
  DocumentResourceMgr resourceMgr;
  auto *layout = new TextDocumentLayout(&doc, &resourceMgr);
  doc.setDocumentLayout(layout);
  layout->setPreviewEnabled(true);

  const QTextBlock block = doc.firstBlock();
  QVector<TextDocumentLayout::WidgetPreviewSpec> specs;
  // Submitted out of order on purpose: stacking must follow the source start.
  specs.append(makeSpec(2, block.position() + 6, block.position() + 10, 60, 30,
                        PreviewPlacement::BlockAfterSource));
  specs.append(makeSpec(1, block.position(), block.position() + 5, 60, 20,
                        PreviewPlacement::BlockAfterSource));
  layout->setWidgetPreviews(specs);

  auto info = BlockLayoutData::get(block);
  QCOMPARE(info->m_widgets.size(), 2);
  QCOMPARE(info->m_widgets.at(0).m_id, quint64(1));
  QCOMPARE(info->m_widgets.at(1).m_id, quint64(2));
  QVERIFY(info->m_widgets.at(0).m_rect.bottom() <= info->m_widgets.at(1).m_rect.top());
}

void TestMarkdownFolding::testWidgetPreviewGeometryWithEqualDocumentSize() {
  QTextDocument doc(generateLines(6));
  DocumentResourceMgr resourceMgr;
  auto *layout = new TextDocumentLayout(&doc, &resourceMgr);
  doc.setDocumentLayout(layout);
  layout->setPreviewEnabled(true);

  const QTextBlock first = doc.findBlockByNumber(1);
  const QTextBlock second = doc.findBlockByNumber(3);

  QVector<TextDocumentLayout::WidgetPreviewSpec> specs;
  specs.append(makeSpec(1, first.position(), first.position() + first.length() - 1, 50, 20,
                        PreviewPlacement::BlockAfterSource));
  specs.append(makeSpec(2, second.position(), second.position() + second.length() - 1, 50, 40,
                        PreviewPlacement::BlockAfterSource));
  layout->setWidgetPreviews(specs);

  const QSizeF sizeBefore = layout->documentSize();
  const QRectF firstBefore = layout->widgetPreviewRect(1);

  QSignalSpy sizeSpy(layout, SIGNAL(documentSizeChanged(QSizeF)));
  QSignalSpy geometrySpy(layout, &TextDocumentLayout::widgetPreviewGeometryChanged);

  // Swap the heights: the total document size is unchanged but the geometry of
  // both widgets moved.
  specs[0].m_height = 40;
  specs[1].m_height = 20;
  layout->setWidgetPreviews(specs);

  QCOMPARE(layout->documentSize(), sizeBefore);
  QCOMPARE(sizeSpy.count(), 0);
  QCOMPARE(geometrySpy.count(), 1);
  QVERIFY(layout->widgetPreviewRect(1) != firstBefore);
}

// A block can lose its layout offset when a relayout walk missed it - which is
// what a document mutation performed from inside a layout pass produces. The
// document size pass must repair the whole offset chain instead of aborting on
// it.
void TestMarkdownFolding::testDocumentSizeRepairsAMissingBlockOffset() {
  QTextDocument doc(generateLines(8));
  DocumentResourceMgr resourceMgr;
  auto *layout = new TextDocumentLayout(&doc, &resourceMgr);
  doc.setDocumentLayout(layout);
  layout->setPreviewEnabled(true);

  const QTextBlock anchorBlock = doc.findBlockByNumber(5);
  QVector<TextDocumentLayout::WidgetPreviewSpec> specs;
  specs.append(makeSpec(1, anchorBlock.position(),
                        anchorBlock.position() + anchorBlock.length() - 1, 50, 30,
                        PreviewPlacement::BlockAfterSource));
  layout->setWidgetPreviews(specs);
  layout->relayout();

  const qreal heightBefore = layout->documentSize().height();
  QVERIFY(!layout->widgetPreviewRect(1).isNull());

  // Knock a middle block off the offset chain, exactly as a merged nested edit
  // does, and force a document size recomputation through a discontinuous
  // relayout of an unrelated block.
  QTextBlock damaged = doc.findBlockByNumber(3);
  BlockLayoutData::get(damaged)->reset();

  OrderedIntSet blocks;
  blocks.insert(1, QMapDummyValue());
  layout->relayout(blocks);

  // Every block has an offset again, and the offsets are monotonic.
  qreal previousBottom = -1;
  for (QTextBlock blk = doc.firstBlock(); blk.isValid(); blk = blk.next()) {
    const auto info = BlockLayoutData::get(blk);
    QVERIFY2(
        info->hasOffset(),
        qPrintable(QStringLiteral("block %1 was left without an offset").arg(blk.blockNumber())));
    QVERIFY(info->m_offset >= previousBottom - 1e-6);
    previousBottom = info->bottom();
  }

  // The height was sampled from the repaired last block, not from the stale
  // one the pass started with.
  QCOMPARE(layout->documentSize().height(), BlockLayoutData::get(doc.lastBlock())->bottom());
  QCOMPARE(layout->documentSize().height(), heightBefore);

  // And the widget geometry map is complete again.
  QVERIFY(!layout->widgetPreviewRect(1).isNull());
}

// Every widgetPreviewGeometryChanged emission has to be observable as "the
// layout is mid-pass", including the setWidgetPreviews() path whose spec delta
// is empty and which therefore calls the emitter directly.
void TestMarkdownFolding::testLayoutIsBusyDuringWidgetGeometryEmission() {
  QTextDocument doc(generateLines(6));
  DocumentResourceMgr resourceMgr;
  auto *layout = new TextDocumentLayout(&doc, &resourceMgr);
  doc.setDocumentLayout(layout);
  layout->setPreviewEnabled(true);

  int emissions = 0;
  int busyEmissions = 0;
  QObject::connect(layout, &TextDocumentLayout::widgetPreviewGeometryChanged, layout, [&]() {
    ++emissions;
    if (layout->isBusy()) {
      ++busyEmissions;
    }
  });

  QVERIFY(!layout->isBusy());

  const QTextBlock block = doc.findBlockByNumber(2);
  QVector<TextDocumentLayout::WidgetPreviewSpec> specs;
  specs.append(makeSpec(1, block.position(), block.position() + block.length() - 1, 50, 20,
                        PreviewPlacement::BlockAfterSource));
  layout->setWidgetPreviews(specs);
  QVERIFY(emissions > 0);

  // The empty-delta path: the added spec resolves to no block at all, so
  // setWidgetPreviews() never relayouts anything and calls the emitter
  // directly, outside every other pass scope. Guarding the emitter itself is
  // what keeps the invariant below true regardless of the caller.
  specs.append(makeSpec(2, doc.characterCount() + 100, doc.characterCount() + 140, 50, 20,
                        PreviewPlacement::BlockAfterSource));
  layout->setWidgetPreviews(specs);

  // A relayout emits from a third scope again.
  layout->relayout();

  QVERIFY(emissions > 0);
  QCOMPARE(busyEmissions, emissions);
  QVERIFY(!layout->isBusy());
}

void TestMarkdownFolding::testWidgetPreviewFolding() {
  QTextDocument doc(generateLines(10));
  DocumentResourceMgr resourceMgr;
  auto *layout = new TextDocumentLayout(&doc, &resourceMgr);
  doc.setDocumentLayout(layout);
  layout->setPreviewEnabled(true);

  TextFolding folding(&doc);

  const QTextBlock block = doc.findBlockByNumber(5);
  QVector<TextDocumentLayout::WidgetPreviewSpec> specs;
  specs.append(makeSpec(3, block.position(), block.position() + block.length() - 1, 50, 30,
                        PreviewPlacement::BlockAfterSource));
  layout->setWidgetPreviews(specs);
  QVERIFY(!layout->widgetPreviewRect(3).isNull());

  TextBlockRange range(doc.findBlockByNumber(4), doc.findBlockByNumber(7));
  auto id = folding.newFoldingRange(range, TextFolding::Persistent);
  QVERIFY(id != TextFolding::InvalidRangeId);
  folding.toggleRange(id);

  // The anchoring block became invisible: the widget must disappear without
  // being forgotten.
  QVERIFY(layout->widgetPreviewRect(3).isNull());
  QCOMPARE(layout->widgetPreviews().size(), 1);

  folding.toggleRange(id);
  QVERIFY(!layout->widgetPreviewRect(3).isNull());
}

void TestMarkdownFolding::testWidgetPreviewWidthClamped() {
  QTextDocument doc(QStringLiteral("short"));
  doc.setTextWidth(200);
  DocumentResourceMgr resourceMgr;
  auto *layout = new TextDocumentLayout(&doc, &resourceMgr);
  doc.setDocumentLayout(layout);
  layout->setPreviewEnabled(true);

  const qreal available = layout->availableContentWidth();
  QVERIFY(available > 0);
  QVERIFY(available < 200);

  const QTextBlock block = doc.firstBlock();
  QVector<TextDocumentLayout::WidgetPreviewSpec> specs;
  specs.append(makeSpec(9, block.position(), block.position() + block.length() - 1, 10000, 25,
                        PreviewPlacement::BlockAfterSource));
  layout->setWidgetPreviews(specs);

  auto info = BlockLayoutData::get(block);
  QCOMPARE(info->m_widgets.size(), 1);
  QCOMPARE(info->m_widgets.first().m_rect.width(), available);
}

void TestMarkdownFolding::testWidgetPreviewInlineBand() {
  QTextDocument doc(QStringLiteral("alpha beta gamma delta"));
  DocumentResourceMgr resourceMgr;
  auto *layout = new TextDocumentLayout(&doc, &resourceMgr);
  doc.setDocumentLayout(layout);
  layout->setPreviewEnabled(true);

  const QTextBlock block = doc.firstBlock();
  const qreal plainHeight = BlockLayoutData::get(block)->m_rect.height();

  QVector<TextDocumentLayout::WidgetPreviewSpec> specs;
  specs.append(makeSpec(4, block.position() + 6, block.position() + 10, 40, 35,
                        PreviewPlacement::InlineAboveLine));
  layout->setWidgetPreviews(specs);

  auto info = BlockLayoutData::get(block);
  QCOMPARE(info->m_widgets.size(), 1);
  QCOMPARE(info->m_widgets.first().m_rect.height(), 35.0);
  // The band sits above the visual line and matches the source span.
  // Widget rects are stored relative to the content origin, so the document
  // margin is added only when the geometry is published.
  const QTextLine line = block.layout()->lineAt(0);
  const qreal margin = doc.documentMargin();
  QVERIFY(info->m_widgets.first().m_rect.bottom() <= line.y() + 1e-6);
  QVERIFY(realNear(info->m_widgets.first().m_rect.left(), line.cursorToX(6) - margin));
  QVERIFY(
      realNear(info->m_widgets.first().m_rect.width(), line.cursorToX(10) - line.cursorToX(6)));
  QVERIFY(info->m_rect.height() > plainHeight + 34);
  QCOMPARE(layout->widgetPreviewRect(4).left(), line.cursorToX(6));
}

void TestMarkdownFolding::testClaimSuppressesStaticPreview() {
  QTextDocument doc(QStringLiteral("![img](a.png)\ntail"));
  DocumentResourceMgr resourceMgr;
  auto *layout = new TextDocumentLayout(&doc, &resourceMgr);
  doc.setDocumentLayout(layout);

  const QTextBlock block = doc.firstBlock();
  const qreal plainHeight = BlockLayoutData::get(block)->m_rect.height();

  auto previewData = BlockPreviewData::get(block);
  previewData->insert(new PreviewData(PreviewData::ImageLink, 1, 0, block.length() - 1, 0, false,
                                      QStringLiteral("claim-image"), QSize(80, 60), 0));
  layout->setPreviewEnabled(true);

  const qreal paintedHeight = BlockLayoutData::get(block)->m_rect.height();
  QVERIFY(paintedHeight > plainHeight + 59);
  QCOMPARE(BlockLayoutData::get(block)->m_images.size(), 1);

  // A claim of a different type must not suppress this painted image.
  QVector<TextDocumentLayout::PreviewClaim> claims;
  TextDocumentLayout::PreviewClaim claim;
  claim.m_startPos = block.position();
  claim.m_endPos = block.position() + block.length() - 1;
  claim.m_type = PreviewElementType::Table;
  claims.append(claim);
  layout->setPreviewClaims(claims);

  QCOMPARE(BlockLayoutData::get(block)->m_rect.height(), paintedHeight);
  QCOMPARE(BlockLayoutData::get(block)->m_images.size(), 1);

  // Claiming the element with a matching type suppresses exactly that painted
  // preview.
  claims[0].m_type = PreviewElementType::Image;
  layout->setPreviewClaims(claims);

  QCOMPARE(BlockLayoutData::get(block)->m_rect.height(), plainHeight);
  QVERIFY(BlockLayoutData::get(block)->m_images.isEmpty());

  // Removing the claim restores the painted fallback immediately.
  layout->setPreviewClaims(QVector<TextDocumentLayout::PreviewClaim>());
  QCOMPARE(BlockLayoutData::get(block)->m_rect.height(), paintedHeight);
  QCOMPARE(BlockLayoutData::get(block)->m_images.size(), 1);
}

void TestMarkdownFolding::testClaimIsTypeScoped() {
  // An image nested in a claimed table range keeps its painted preview: the
  // table widget renders the source, not the image.
  QTextDocument doc(QStringLiteral("| ![img](a.png) |\n| --- |\ntail"));
  DocumentResourceMgr resourceMgr;
  auto *layout = new TextDocumentLayout(&doc, &resourceMgr);
  doc.setDocumentLayout(layout);

  const QTextBlock block = doc.firstBlock();
  const qreal plainHeight = BlockLayoutData::get(block)->m_rect.height();

  auto previewData = BlockPreviewData::get(block);
  previewData->insert(new PreviewData(PreviewData::ImageLink, 1, 2, 15, 0, true,
                                      QStringLiteral("nested-image"), QSize(40, 30), 0));
  layout->setPreviewEnabled(true);
  const qreal paintedHeight = BlockLayoutData::get(block)->m_rect.height();
  QVERIFY(paintedHeight > plainHeight);

  const QTextBlock lastRow = doc.findBlockByNumber(1);
  QVector<TextDocumentLayout::PreviewClaim> claims;
  TextDocumentLayout::PreviewClaim claim;
  claim.m_startPos = block.position();
  claim.m_endPos = lastRow.position() + lastRow.length() - 1;
  claim.m_type = PreviewElementType::Table;
  claims.append(claim);
  layout->setPreviewClaims(claims);

  QCOMPARE(BlockLayoutData::get(block)->m_rect.height(), paintedHeight);
  QVERIFY(!BlockLayoutData::get(block)->m_images.isEmpty());
}

void TestMarkdownFolding::testSourceTextRectSharesWidgetCoordinates() {
  QTextDocument doc(QStringLiteral("alpha beta gamma delta"));
  DocumentResourceMgr resourceMgr;
  auto *layout = new TextDocumentLayout(&doc, &resourceMgr);
  doc.setDocumentLayout(layout);
  layout->setPreviewEnabled(true);

  const QTextBlock block = doc.firstBlock();
  QVector<TextDocumentLayout::WidgetPreviewSpec> specs;
  specs.append(makeSpec(11, block.position() + 6, block.position() + 10, 40, 20,
                        PreviewPlacement::InlineAboveLine));
  layout->setWidgetPreviews(specs);

  // Both rectangles are documented as document coordinates and are handed to
  // the same widget, so they must share the same origin.
  const QRectF sourceRect = layout->sourceTextRect(block.position() + 6, block.position() + 10);
  const QRectF widgetRect = layout->widgetPreviewRect(11);
  QVERIFY(!sourceRect.isNull());
  QVERIFY(!widgetRect.isNull());
  QVERIFY(realNear(sourceRect.left(), widgetRect.left()));
  QVERIFY(realNear(sourceRect.width(), widgetRect.width()));

  // The width the widget is measured at is the width it is assigned.
  QVERIFY(realNear(layout->inlinePlacementWidth(block.position() + 6, block.position() + 10),
                   widgetRect.width()));
}

// An element whose source is rewritten in one go - what the preview write-back
// path does when a table cell is edited - loses its fold range inside
// TextFolding, because the blocks it spans are replaced. The next parse reports
// the very same (startBlock, endBlock) pair, so the range has to be recreated
// even though the diff sees no change.
void TestMarkdownFolding::testInPlaceRewriteKeepsFoldRange() {
  const QString table = QStringLiteral("| Left | Center | Right |\n"
                                       "| :--- | :----: | ----: |\n"
                                       "| a    | b      | c     |\n"
                                       "| d    | e      | f     |");
  QTextDocument doc(QStringLiteral("# Title\n\nintro\n") + table +
                    QStringLiteral("\n\ntail 1\ntail 2"));

  TextFolding folding(&doc);
  MarkdownFoldingProvider provider(&folding, &doc);

  QVector<md::FoldingRegion> regions;
  // A second range which survives the rewrite, so TextFolding never becomes
  // empty and the editor's reset-on-empty safety net does not kick in.
  regions.append({0, doc.blockCount() - 1, md::Heading, 1});
  regions.append({3, 6, md::Table, 0});

  provider.updateFoldingRegions(regions);
  QCOMPARE(folding.foldingRangesStartingOnBlock(0).size(), 1);
  QCOMPARE(folding.foldingRangesStartingOnBlock(3).size(), 1);

  const QTextBlock firstRow = doc.findBlockByNumber(3);
  const QTextBlock lastRow = doc.findBlockByNumber(6);
  QTextCursor cursor(&doc);
  cursor.beginEditBlock();
  cursor.setPosition(firstRow.position());
  cursor.setPosition(lastRow.position() + lastRow.length() - 1, QTextCursor::KeepAnchor);
  cursor.insertText(QString(table).replace(QStringLiteral("| b "), QStringLiteral("| B ")));
  cursor.endEditBlock();

  // The blocks the range spanned were replaced, so the range no longer starts
  // where the parser says it does.
  QCOMPARE(doc.blockCount(), 10);
  QCOMPARE(folding.foldingRangesStartingOnBlock(3).size(), 0);
  QCOMPARE(folding.foldingRangesStartingOnBlock(0).size(), 1);

  // The re-parse yields identical regions and must restore the lost range.
  provider.updateFoldingRegions(regions);
  QCOMPARE(folding.foldingRangesStartingOnBlock(3).size(), 1);
  QCOMPARE(folding.foldingRangesStartingOnBlock(0).size(), 1);

  // The restored range is usable.
  auto ranges = folding.foldingRangesStartingOnBlock(3);
  QVERIFY(ranges[0].second.testFlag(TextFolding::Persistent));
  QVERIFY(folding.toggleRange(ranges[0].first));
  QVERIFY(!doc.findBlockByNumber(4).isVisible());
}

// A range which is still live must not be recreated: re-applying the same
// regions keeps a single range, its id and its fold state.
void TestMarkdownFolding::testLiveRangeIsNotRecreated() {
  QVector<md::FoldingRegion> regions;
  regions.append({0, 9, md::Heading, 1});
  regions.append({3, 7, md::FencedCode, 0});

  m_provider->updateFoldingRegions(regions);
  const qint64 headingId = m_textFolding->foldingRangesStartingOnBlock(0).first().first;
  const qint64 codeId = m_textFolding->foldingRangesStartingOnBlock(3).first().first;

  m_provider->updateFoldingRegions(regions);
  m_provider->updateFoldingRegions(regions);

  auto rangesAt0 = m_textFolding->foldingRangesStartingOnBlock(0);
  auto rangesAt3 = m_textFolding->foldingRangesStartingOnBlock(3);
  QCOMPARE(rangesAt0.size(), 1);
  QCOMPARE(rangesAt3.size(), 1);
  QCOMPARE(rangesAt0.first().first, headingId);
  QCOMPARE(rangesAt3.first().first, codeId);

  // The ids alone do not prove the range was left alone: recreating a live
  // range is rejected by TextFolding anyway, which would leave the tree
  // untouched but drop the cached pair -> id entry. Retiring the region is
  // what makes that loss observable, because the removal goes through that
  // very entry.
  QVector<md::FoldingRegion> shrunk;
  shrunk.append({0, 9, md::Heading, 1});
  m_provider->updateFoldingRegions(shrunk);

  QCOMPARE(m_textFolding->foldingRangesStartingOnBlock(3).size(), 0);
  auto remaining = m_textFolding->foldingRangesStartingOnBlock(0);
  QCOMPARE(remaining.size(), 1);
  QCOMPARE(remaining.first().first, headingId);
}

// ---------------------------------------------------------------------------
// Reconciliation from the ranges' live positions
// ---------------------------------------------------------------------------

static PreviewedRange makeWidgetRange(quint64 p_identity, int p_startBlock, int p_endBlock,
                                      PreviewElementType p_type,
                                      PreviewFoldState p_state = PreviewFoldState::Undecided) {
  PreviewedRange range;
  range.m_identity = p_identity;
  range.m_startBlock = p_startBlock;
  range.m_endBlock = p_endBlock;
  range.m_type = p_type;
  range.m_foldState = p_state;
  return range;
}

// The fold state of a range must survive an edit which only shifts the blocks
// below it. Matching the parsed regions against the *keys* of the last
// reconciliation drops and recreates every range under the edit, and loses its
// fold state with it.
void TestMarkdownFolding::testReconcileSurvivesBlockShift() {
  QTextDocument doc(generateLines(40));
  TextFolding folding(&doc);
  MarkdownFoldingProvider provider(&folding, &doc);

  QVector<md::FoldingRegion> regions;
  regions.append({0, 9, md::Heading, 1});
  regions.append({12, 19, md::FencedCode, 0});
  provider.updateFoldingRegions(regions);

  const qint64 headingId = folding.foldingRangesStartingOnBlock(0).first().first;
  const qint64 codeId = folding.foldingRangesStartingOnBlock(12).first().first;
  QVERIFY(folding.foldRange(codeId));
  QVERIFY(!doc.findBlockByNumber(13).isVisible());

  // Insert a line between the two regions: the code range moves down by one,
  // the heading range does not move at all.
  QTextCursor cursor(&doc);
  cursor.setPosition(doc.findBlockByNumber(10).position());
  cursor.insertText(QStringLiteral("inserted\n"));

  int first = -1;
  int last = -1;
  QVERIFY(folding.foldingRangeBlocks(codeId, &first, &last));
  QCOMPARE(first, 13);
  QCOMPARE(last, 20);

  // The re-parse reports the shifted extents.
  QVector<md::FoldingRegion> shifted;
  shifted.append({0, 9, md::Heading, 1});
  shifted.append({13, 20, md::FencedCode, 0});
  provider.updateFoldingRegions(shifted);

  QCOMPARE(folding.foldingRangesStartingOnBlock(13).size(), 1);
  QCOMPARE(folding.foldingRangesStartingOnBlock(13).first().first, codeId);
  QCOMPARE(folding.foldingRangesStartingOnBlock(0).first().first, headingId);
  QVERIFY(folding.isRangeFolded(codeId));
  QVERIFY(!doc.findBlockByNumber(14).isVisible());
}

// A region which keeps its start block and changes its end block gets a live
// range back within the same single pass: the removal has to precede the
// creation, otherwise TextFolding refuses the new range for sharing a start
// block with the old one.
void TestMarkdownFolding::testReconcileEndBlockChange() {
  QTextDocument doc(generateLines(30));
  TextFolding folding(&doc);
  MarkdownFoldingProvider provider(&folding, &doc);

  QVector<md::FoldingRegion> regions;
  regions.append({5, 10, md::FencedCode, 0});
  provider.updateFoldingRegions(regions);
  QCOMPARE(folding.foldingRangesStartingOnBlock(5).size(), 1);

  QVector<md::FoldingRegion> grown;
  grown.append({5, 14, md::FencedCode, 0});
  provider.updateFoldingRegions(grown);

  auto ranges = folding.foldingRangesStartingOnBlock(5);
  QCOMPARE(ranges.size(), 1);
  int first = -1;
  int last = -1;
  QVERIFY(folding.foldingRangeBlocks(ranges.first().first, &first, &last));
  QCOMPARE(first, 5);
  QCOMPARE(last, 14);
}

// A region whose type changes at the same extent is a different element, so it
// must not inherit the old range or its settled decision.
void TestMarkdownFolding::testReconcileTypeChange() {
  QTextDocument doc(generateLines(30));
  TextFolding folding(&doc);
  MarkdownFoldingProvider provider(&folding, &doc);

  QVector<md::FoldingRegion> code;
  code.append({5, 10, md::FencedCode, 0});
  provider.updateFoldingRegions(code);

  const qint64 codeId = folding.foldingRangesStartingOnBlock(5).first().first;
  QVERIFY(folding.foldRange(codeId));

  QVector<md::FoldingRegion> table;
  table.append({5, 10, md::Table, 0});
  provider.updateFoldingRegions(table);

  auto ranges = folding.foldingRangesStartingOnBlock(5);
  QCOMPARE(ranges.size(), 1);
  QVERIFY(ranges.first().first != codeId);
  QVERIFY(!folding.isRangeFolded(ranges.first().first));
  QVERIFY(!folding.foldingRangeBlocks(codeId, nullptr, nullptr));

  // The type is what the query keys on, too.
  bool folded = true;
  QVERIFY(!provider.tryRegionFolded(PreviewElementType::Code, 5, 10, &folded));
  QVERIFY(provider.tryRegionFolded(PreviewElementType::Table, 5, 10, &folded));
  QVERIFY(!folded);
}

// A blockquote wrapping nothing but a table emits two regions covering exactly
// the same blocks, and TextFolding can only hold one. The preview-bearing one
// has to win, deterministically and over repeated passes.
void TestMarkdownFolding::testExactExtentDeduplication() {
  QTextDocument doc(generateLines(30));
  TextFolding folding(&doc);
  MarkdownFoldingProvider provider(&folding, &doc);

  QVector<md::FoldingRegion> wrapperFirst;
  wrapperFirst.append({5, 10, md::Blockquote, 0});
  wrapperFirst.append({5, 10, md::Table, 0});

  QVector<md::FoldingRegion> tableFirst;
  tableFirst.append({5, 10, md::Table, 0});
  tableFirst.append({5, 10, md::Blockquote, 0});

  bool folded = true;
  for (int i = 0; i < 3; ++i) {
    provider.updateFoldingRegions(i % 2 == 0 ? wrapperFirst : tableFirst);
    QCOMPARE(folding.foldingRangesStartingOnBlock(5).size(), 1);
    QVERIFY(provider.tryRegionFolded(PreviewElementType::Table, 5, 10, &folded));
    QVERIFY(!folded);
  }

  // A live wrapper entry at that extent is replaced once the preview-bearing
  // region appears there: nothing matches the wrapper, so it is removed and
  // the table is created in its place.
  MarkdownFoldingProvider second(&folding, &doc);
  provider.clear();

  QVector<md::FoldingRegion> wrapperOnly;
  wrapperOnly.append({5, 10, md::Blockquote, 0});
  second.updateFoldingRegions(wrapperOnly);
  QCOMPARE(folding.foldingRangesStartingOnBlock(5).size(), 1);
  QVERIFY(!second.tryRegionFolded(PreviewElementType::Table, 5, 10, &folded));

  second.updateFoldingRegions(wrapperFirst);
  QCOMPARE(folding.foldingRangesStartingOnBlock(5).size(), 1);
  QVERIFY(second.tryRegionFolded(PreviewElementType::Table, 5, 10, &folded));
}

// Live/folded, live/unfolded and "no live range" are three distinct outcomes.
void TestMarkdownFolding::testTryRegionFolded() {
  QTextDocument doc(generateLines(30));
  TextFolding folding(&doc);
  MarkdownFoldingProvider provider(&folding, &doc);

  QVector<md::FoldingRegion> regions;
  regions.append({5, 10, md::FencedCode, 0});
  provider.updateFoldingRegions(regions);

  bool folded = true;
  QVERIFY(provider.tryRegionFolded(PreviewElementType::Code, 5, 10, &folded));
  QVERIFY(!folded);

  const qint64 id = folding.foldingRangesStartingOnBlock(5).first().first;
  QVERIFY(folding.foldRange(id));
  QVERIFY(provider.tryRegionFolded(PreviewElementType::Code, 5, 10, &folded));
  QVERIFY(folded);

  // A different extent, a different type and a type which never produces a
  // folding region are all "no live range".
  QVERIFY(!provider.tryRegionFolded(PreviewElementType::Code, 5, 11, &folded));
  QVERIFY(!provider.tryRegionFolded(PreviewElementType::Table, 5, 10, &folded));
  QVERIFY(!provider.tryRegionFolded(PreviewElementType::Image, 5, 10, &folded));

  provider.clear();
  QVERIFY(!provider.tryRegionFolded(PreviewElementType::Code, 5, 10, &folded));
}

// The restored range is created folded and already settled, and it carries the
// type the parser is going to report, so the next reconciliation keeps it
// instead of dropping and recreating it.
void TestMarkdownFolding::testRestoreFoldedRange() {
  QTextDocument doc(generateLines(30));
  TextFolding folding(&doc);
  MarkdownFoldingProvider provider(&folding, &doc);

  provider.restoreFoldedRange(PreviewElementType::Table, 5, 10);

  auto ranges = folding.foldingRangesStartingOnBlock(5);
  QCOMPARE(ranges.size(), 1);
  const qint64 id = ranges.first().first;
  QVERIFY(folding.isRangeFolded(id));
  QVERIFY(!doc.findBlockByNumber(6).isVisible());

  QVector<md::FoldingRegion> regions;
  regions.append({5, 10, md::Table, 0});
  provider.updateFoldingRegions(regions);

  ranges = folding.foldingRangesStartingOnBlock(5);
  QCOMPARE(ranges.size(), 1);
  QCOMPARE(ranges.first().first, id);
  QVERIFY(folding.isRangeFolded(id));

  // A region type which never carries a preview is refused.
  provider.restoreFoldedRange(PreviewElementType::Image, 15, 20);
  QCOMPARE(folding.foldingRangesStartingOnBlock(15).size(), 0);
}

// ---------------------------------------------------------------------------
// Preview driven auto-folding
// ---------------------------------------------------------------------------

void TestMarkdownFolding::testAutoFoldWidgetPreview() {
  QTextDocument doc(generateLines(30));
  TextFolding folding(&doc);
  MarkdownFoldingProvider provider(&folding, &doc);
  provider.setAutoFoldPreviewsEnabled(true);

  QVector<md::FoldingRegion> regions;
  regions.append({5, 10, md::Table, 0});
  provider.updateFoldingRegions(regions);
  const qint64 id = folding.foldingRangesStartingOnBlock(5).first().first;

  QVector<PreviewedRange> ranges;
  ranges.append(makeWidgetRange(1, 5, 10, PreviewElementType::Table));

  auto states = provider.applyPreviewAutoFold(ranges, -1);
  QVERIFY(folding.isRangeFolded(id));
  QVERIFY(!doc.findBlockByNumber(6).isVisible());
  QCOMPARE(states.size(), 1);
  QCOMPARE(states.first().first, quint64(1));
  QVERIFY(states.first().second == PreviewFoldState::Folded);

  // The user unfolds it by hand: the region is settled, so no later pass may
  // fold it again, and the new state is reported back.
  QVERIFY(folding.toggleRange(id));
  QVERIFY(doc.findBlockByNumber(6).isVisible());

  ranges[0].m_foldState = PreviewFoldState::Folded;
  states = provider.applyPreviewAutoFold(ranges, -1);
  QVERIFY(!folding.isRangeFolded(id));
  QCOMPARE(states.size(), 1);
  QVERIFY(states.first().second == PreviewFoldState::Unfolded);

  // Nothing changed, so nothing is reported.
  ranges[0].m_foldState = PreviewFoldState::Unfolded;
  states = provider.applyPreviewAutoFold(ranges, -1);
  QVERIFY(states.isEmpty());
  QVERIFY(!folding.isRangeFolded(id));

  // A widget range which does not describe the region exactly is not its
  // preview.
  MarkdownFoldingProvider other(&folding, &doc);
  provider.clear();
  other.updateFoldingRegions(regions);
  const qint64 otherId = folding.foldingRangesStartingOnBlock(5).first().first;
  QVector<PreviewedRange> mismatched;
  mismatched.append(makeWidgetRange(2, 5, 11, PreviewElementType::Table));
  mismatched.append(makeWidgetRange(3, 5, 10, PreviewElementType::Code));
  QVERIFY(other.applyPreviewAutoFold(mismatched, -1).isEmpty());
  QVERIFY(!folding.isRangeFolded(otherId));
}

// Folding keeps the first and last block visible, so only a caret in the
// interior would be hidden by the fold - and a region which was left open for
// the caret is left open for good.
void TestMarkdownFolding::testAutoFoldCaretRule() {
  QTextDocument doc(generateLines(30));
  TextFolding folding(&doc);
  MarkdownFoldingProvider provider(&folding, &doc);
  provider.setAutoFoldPreviewsEnabled(true);

  QVector<md::FoldingRegion> regions;
  regions.append({5, 10, md::FencedCode, 0});
  regions.append({15, 20, md::FencedCode, 0});
  provider.updateFoldingRegions(regions);
  const qint64 caretId = folding.foldingRangesStartingOnBlock(5).first().first;
  const qint64 boundaryId = folding.foldingRangesStartingOnBlock(15).first().first;

  QVector<PreviewedRange> ranges;
  ranges.append(makeWidgetRange(1, 5, 10, PreviewElementType::Code));
  ranges.append(makeWidgetRange(2, 15, 20, PreviewElementType::Code));

  // The caret sits inside the first region, and on the first block of the
  // second one - which stays visible when it folds.
  auto states = provider.applyPreviewAutoFold(ranges, 7);
  QVERIFY(!folding.isRangeFolded(caretId));
  QVERIFY(folding.isRangeFolded(boundaryId));

  QHash<quint64, PreviewFoldState> reported;
  for (const auto &state : states) {
    reported.insert(state.first, state.second);
  }
  QVERIFY(reported.value(1) == PreviewFoldState::Unfolded);
  QVERIFY(reported.value(2) == PreviewFoldState::Folded);

  // Moving the caret away does not re-decide the region it kept open.
  ranges[0].m_foldState = PreviewFoldState::Unfolded;
  ranges[1].m_foldState = PreviewFoldState::Folded;
  QVERIFY(provider.applyPreviewAutoFold(ranges, 0).isEmpty());
  QVERIFY(!folding.isRangeFolded(caretId));
}

void TestMarkdownFolding::testAutoFoldPaintedPreview() {
  QTextDocument doc(generateLines(40));
  TextFolding folding(&doc);
  MarkdownFoldingProvider provider(&folding, &doc);
  provider.setAutoFoldPreviewsEnabled(true);

  // A painted code preview drawn on the region's last block, which folding
  // keeps visible.
  auto lastBlock = doc.findBlockByNumber(10);
  BlockPreviewData::get(lastBlock)
      ->insert(new PreviewData(PreviewData::CodeBlock, 1, 0, lastBlock.length() - 1, 0, false,
                               QStringLiteral("code-preview"), QSize(80, 40), 0));

  // One drawn on an interior block only, which folding hides.
  auto interiorBlock = doc.findBlockByNumber(23);
  BlockPreviewData::get(interiorBlock)
      ->insert(new PreviewData(PreviewData::CodeBlock, 1, 0, interiorBlock.length() - 1, 0, false,
                               QStringLiteral("interior-preview"), QSize(80, 40), 0));

  QVector<md::FoldingRegion> regions;
  regions.append({5, 10, md::FencedCode, 0});
  regions.append({20, 26, md::FencedCode, 0});
  // No preview at all.
  regions.append({30, 36, md::FencedCode, 0});
  provider.updateFoldingRegions(regions);

  const qint64 previewedId = folding.foldingRangesStartingOnBlock(5).first().first;
  const qint64 interiorId = folding.foldingRangesStartingOnBlock(20).first().first;
  const qint64 bareId = folding.foldingRangesStartingOnBlock(30).first().first;

  // A painted preview has no identity, so nothing is ever reported for it.
  QVERIFY(provider.applyPreviewAutoFold(QVector<PreviewedRange>(), -1).isEmpty());
  QVERIFY(folding.isRangeFolded(previewedId));
  QVERIFY(!folding.isRangeFolded(interiorId));
  QVERIFY(!folding.isRangeFolded(bareId));

  // Probing must not install block user data on a block which had none.
  QVERIFY(doc.findBlockByNumber(30).userData() == nullptr);
  QVERIFY(doc.findBlockByNumber(36).userData() == nullptr);
  QVERIFY(doc.findBlockByNumber(20).userData() == nullptr);
}

// A wrapper region which merely *contains* a previewed element never folds:
// the region type is what decides which previews count.
void TestMarkdownFolding::testAutoFoldSkipsWrapperRegion() {
  QTextDocument doc(generateLines(30));
  TextFolding folding(&doc);
  MarkdownFoldingProvider provider(&folding, &doc);
  provider.setAutoFoldPreviewsEnabled(true);

  QVector<md::FoldingRegion> regions;
  regions.append({5, 20, md::Heading, 1});
  regions.append({8, 12, md::FencedCode, 0});
  provider.updateFoldingRegions(regions);

  const qint64 headingId = folding.foldingRangesStartingOnBlock(5).first().first;
  const qint64 codeId = folding.foldingRangesStartingOnBlock(8).first().first;

  QVector<PreviewedRange> ranges;
  ranges.append(makeWidgetRange(1, 8, 12, PreviewElementType::Code));
  provider.applyPreviewAutoFold(ranges, -1);

  QVERIFY(folding.isRangeFolded(codeId));
  QVERIFY(!folding.isRangeFolded(headingId));
  QVERIFY(doc.findBlockByNumber(6).isVisible());
  QVERIFY(!doc.findBlockByNumber(9).isVisible());
}

// The option is read exactly once per region: a region settled unfolded while
// it was off stays unfolded when it is turned on afterwards.
void TestMarkdownFolding::testAutoFoldOptionOff() {
  QTextDocument doc(generateLines(30));
  TextFolding folding(&doc);
  MarkdownFoldingProvider provider(&folding, &doc);
  provider.setAutoFoldPreviewsEnabled(false);

  QVector<md::FoldingRegion> regions;
  regions.append({5, 10, md::Table, 0});
  provider.updateFoldingRegions(regions);
  const qint64 id = folding.foldingRangesStartingOnBlock(5).first().first;

  QVector<PreviewedRange> ranges;
  ranges.append(makeWidgetRange(1, 5, 10, PreviewElementType::Table));
  auto states = provider.applyPreviewAutoFold(ranges, -1);
  QVERIFY(!folding.isRangeFolded(id));
  QCOMPARE(states.size(), 1);
  QVERIFY(states.first().second == PreviewFoldState::Unfolded);

  provider.setAutoFoldPreviewsEnabled(true);
  ranges[0].m_foldState = PreviewFoldState::Unfolded;
  QVERIFY(provider.applyPreviewAutoFold(ranges, -1).isEmpty());
  QVERIFY(!folding.isRangeFolded(id));

  // A fold and an unfold between two passes cancel out, and the pass only ever
  // samples the current state, so nothing is lost by the coalescing.
  QVERIFY(folding.foldRange(id));
  QVERIFY(folding.toggleRange(id));
  QVERIFY(provider.applyPreviewAutoFold(ranges, -1).isEmpty());
  QVERIFY(!folding.isRangeFolded(id));
}

// A preview which already knows its state has its fold restored, even with the
// option off: requirement 2 does not depend on requirement 1.
void TestMarkdownFolding::testAutoFoldRestoresReportedState() {
  QTextDocument doc(generateLines(30));
  TextFolding folding(&doc);
  MarkdownFoldingProvider provider(&folding, &doc);
  provider.setAutoFoldPreviewsEnabled(false);

  QVector<md::FoldingRegion> regions;
  regions.append({5, 10, md::Table, 0});
  provider.updateFoldingRegions(regions);
  const qint64 id = folding.foldingRangesStartingOnBlock(5).first().first;

  QVector<PreviewedRange> ranges;
  ranges.append(makeWidgetRange(1, 5, 10, PreviewElementType::Table, PreviewFoldState::Folded));
  auto states = provider.applyPreviewAutoFold(ranges, -1);
  QVERIFY(folding.isRangeFolded(id));
  // The live state now agrees with what the preview remembered.
  QVERIFY(states.isEmpty());
}

// Text folding being switched off must never hide source, and must not destroy
// what a widget preview remembers.
void TestMarkdownFolding::testAutoFoldWithTextFoldingDisabled() {
  QTextDocument doc(generateLines(30));
  TextFolding folding(&doc);
  MarkdownFoldingProvider provider(&folding, &doc);
  provider.setAutoFoldPreviewsEnabled(true);
  folding.setEnabled(false);

  QVector<md::FoldingRegion> regions;
  regions.append({5, 10, md::Table, 0});
  provider.updateFoldingRegions(regions);
  const qint64 id = folding.foldingRangesStartingOnBlock(5).first().first;

  QVector<PreviewedRange> ranges;
  ranges.append(makeWidgetRange(1, 5, 10, PreviewElementType::Table, PreviewFoldState::Folded));

  // Switching folding off clears every range, which is what makes the editor
  // reset the provider's table.
  folding.setEnabled(false);
  provider.resetState();
  QVERIFY(!folding.foldingRangeBlocks(id, nullptr, nullptr));

  // A parse while folding is off still creates ranges - TextFolding only gates
  // its own maintenance - so the pass has an entry to work with and the gate is
  // what has to decline.
  provider.updateFoldingRegions(regions);
  QCOMPARE(folding.foldingRangesStartingOnBlock(5).size(), 1);
  QVERIFY(provider.applyPreviewAutoFold(ranges, -1).isEmpty());
  QVERIFY(doc.findBlockByNumber(6).isVisible());

  // Nor may the query report a state: the range exists but is not something
  // the user can act on, and answering "unfolded" would let the rewrite path
  // overwrite what the preview remembers.
  bool folded = true;
  QVERIFY(!provider.tryRegionFolded(PreviewElementType::Table, 5, 10, &folded));

  provider.restoreFoldedRange(PreviewElementType::Table, 15, 20);
  QCOMPARE(folding.foldingRangesStartingOnBlock(15).size(), 0);
  QVERIFY(doc.findBlockByNumber(16).isVisible());

  // Re-enabling restores the state the widget preview remembered, on the next
  // pass, without the option having to be involved.
  folding.setEnabled(true);
  provider.updateFoldingRegions(regions);
  provider.applyPreviewAutoFold(ranges, -1);
  QVERIFY(!doc.findBlockByNumber(6).isVisible());
}

// The rewrite path folds the recreated range in the same event-loop turn, so
// the source never visibly expands, and the next parse keeps it.
void TestMarkdownFolding::testRestoreFoldAfterInPlaceRewrite() {
  const QString table = QStringLiteral("| Left | Center | Right |\n"
                                       "| :--- | :----: | ----: |\n"
                                       "| a    | b      | c     |\n"
                                       "| d    | e      | f     |");
  QTextDocument doc(QStringLiteral("# Title\n\nintro\n") + table +
                    QStringLiteral("\n\ntail 1\ntail 2"));

  TextFolding folding(&doc);
  MarkdownFoldingProvider provider(&folding, &doc);

  QVector<md::FoldingRegion> regions;
  regions.append({0, doc.blockCount() - 1, md::Heading, 1});
  regions.append({3, 6, md::Table, 0});
  provider.updateFoldingRegions(regions);

  const qint64 tableId = folding.foldingRangesStartingOnBlock(3).first().first;
  QVERIFY(folding.foldRange(tableId));

  bool folded = false;
  QVERIFY(provider.tryRegionFolded(PreviewElementType::Table, 3, 6, &folded));
  QVERIFY(folded);

  // The rewrite adds a row, so everything below it shifts too.
  const QString rewritten = table + QStringLiteral("\n| g    | h      | i     |");
  const QTextBlock firstRow = doc.findBlockByNumber(3);
  const QTextBlock lastRow = doc.findBlockByNumber(6);
  QTextCursor cursor(&doc);
  cursor.beginEditBlock();
  cursor.setPosition(firstRow.position());
  cursor.setPosition(lastRow.position() + lastRow.length() - 1, QTextCursor::KeepAnchor);
  cursor.insertText(rewritten);
  cursor.endEditBlock();

  // The blocks the range spanned were replaced, so it no longer starts where
  // the parser says it does: the fold marker is gone and the range is only
  // waiting to be removed by the next reconciliation.
  QCOMPARE(folding.foldingRangesStartingOnBlock(3).size(), 0);

  provider.restoreFoldedRange(PreviewElementType::Table, 3, 7);
  auto ranges = folding.foldingRangesStartingOnBlock(3);
  QCOMPARE(ranges.size(), 1);
  const qint64 restoredId = ranges.first().first;
  QVERIFY(folding.isRangeFolded(restoredId));
  QVERIFY(!doc.findBlockByNumber(4).isVisible());

  // The next parse describes the rewritten document and must keep the very
  // same range, still folded.
  QVector<md::FoldingRegion> reparsed;
  reparsed.append({0, doc.blockCount() - 1, md::Heading, 1});
  reparsed.append({3, 7, md::Table, 0});
  provider.updateFoldingRegions(reparsed);

  ranges = folding.foldingRangesStartingOnBlock(3);
  QCOMPARE(ranges.size(), 1);
  QCOMPARE(ranges.first().first, restoredId);
  QVERIFY(folding.isRangeFolded(restoredId));
  QVERIFY(!doc.findBlockByNumber(4).isVisible());
}

QTEST_MAIN(tests::TestMarkdownFolding)
