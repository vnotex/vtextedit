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

QTEST_MAIN(tests::TestMarkdownFolding)
