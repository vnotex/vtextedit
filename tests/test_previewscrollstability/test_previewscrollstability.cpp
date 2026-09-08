#include "test_previewscrollstability.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QPixmap>
#include <QScrollBar>
#include <QSharedPointer>
#include <QSignalSpy>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <QVector>

#include <vtextedit/markdowneditorconfig.h>
#include <vtextedit/markdownhighlighter.h>
#include <vtextedit/previewdata.h>
#include <vtextedit/previewmgr.h>
#include <vtextedit/texteditorconfig.h>
#include <vtextedit/texteditutils.h>
#include <vtextedit/vmarkdowneditor.h>
#include <vtextedit/vtextedit.h>

using namespace tests;
using namespace vte;

namespace {
// Enough paragraphs that the fixture scrolls comfortably at 900x600.
const int c_paragraphs = 400;

// Height of an injected preview pixmap, in logical pixels. Several text lines
// tall, so one arrival is unmistakably larger than any rounding.
const int c_previewHeight = 80;

// The fenced-code fixture used by the auto-fold case.
const int c_codeBlocks = 60;

const int c_codeBlockBodyLines = 8;

QSharedPointer<MarkdownEditorConfig> makeConfig() {
  auto textConfig = QSharedPointer<TextEditorConfig>::create();
  return QSharedPointer<MarkdownEditorConfig>::create(textConfig);
}

QString paragraphs(int p_count) {
  QString text;
  for (int i = 0; i < p_count; ++i) {
    text += QStringLiteral("Paragraph %1 of the fixture document.\n\n").arg(i);
  }
  return text;
}

// Fenced code blocks separated by prose. Each body is several lines long, so
// auto-folding one really removes height: TextFolding keeps a region's first
// and last block visible and hides only the interior.
QString codeBlocks(int p_count, int p_bodyLines) {
  QString text;
  for (int i = 0; i < p_count; ++i) {
    text += QStringLiteral("Paragraph before graph %1.\n\n").arg(i);
    text += QStringLiteral("```dot\n");
    for (int l = 0; l < p_bodyLines; ++l) {
      text += QStringLiteral("node%1 -> node%2\n").arg(i).arg(l);
    }
    text += QStringLiteral("```\n\n");
  }
  return text;
}

// Drive one parse generation to completion and let the pending zero timers run.
void settle(VMarkdownEditor &p_editor) {
  auto highlighter = p_editor.getHighlighter();
  QSignalSpy completed(highlighter, &MarkdownHighlighter::highlightCompleted);
  highlighter->updateHighlight();
  QTRY_VERIFY_WITH_TIMEOUT(completed.count() > 0, 60000);
  QTest::qWait(50);
  QCoreApplication::processEvents();
}

// A blockwise painted preview covering the whole of @p_blockNumber, exactly the
// shape PreviewHelper builds for a rendered diagram.
QSharedPointer<PreviewItem> makeItem(QTextDocument *p_doc, int p_blockNumber, int p_height) {
  auto block = p_doc->findBlockByNumber(p_blockNumber);
  Q_ASSERT(block.isValid());

  QPixmap image(120, p_height);
  image.fill(Qt::red);

  auto item = QSharedPointer<PreviewItem>::create();
  item->m_blockNumber = p_blockNumber;
  item->m_blockPos = block.position();
  item->m_startPos = block.position();
  item->m_endPos = block.position() + qMax(1, block.length() - 1);
  item->m_padding = 0;
  item->m_isBlockwise = true;
  // Distinct per block: the resource name is the identity of the image.
  item->m_name = QStringLiteral("test_preview_%1").arg(p_blockNumber);
  item->m_image = image;
  return item;
}

qreal blockDocY(VMarkdownEditor &p_editor, int p_blockNumber) {
  auto doc = p_editor.document();
  auto block = doc->findBlockByNumber(p_blockNumber);
  return doc->documentLayout()->blockBoundingRect(block).y();
}

// Build an editor holding the fixture, shown and settled, with the code block
// preview source enabled.
void setUpEditor(VMarkdownEditor &p_editor) {
  p_editor.resize(900, 600);
  p_editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&p_editor));
  p_editor.setText(paragraphs(c_paragraphs));
  settle(p_editor);
  p_editor.getPreviewMgr()->setPreviewEnabled(PreviewData::Source::CodeBlock, true);
}
} // namespace

void TestPreviewScrollStability::testPreviewAboveViewportDoesNotMoveContent() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  setUpEditor(editor);

  auto textEdit = editor.getTextEdit();
  auto vbar = textEdit->verticalScrollBar();
  QVERIFY2(vbar->maximum() > vbar->minimum(), "the fixture does not scroll");

  // Scroll to the middle, and park the caret where the user is looking so that
  // the caret gating is not what this case is measuring.
  vbar->setValue((vbar->minimum() + vbar->maximum()) / 2);
  QCoreApplication::processEvents();

  const int anchorBlock = TextEditUtils::firstVisibleBlock(textEdit).blockNumber();
  QVERIFY(anchorBlock > 8);

  QTextCursor cursor = textEdit->textCursor();
  cursor.setPosition(editor.document()->findBlockByNumber(anchorBlock + 2).position());
  textEdit->setTextCursor(cursor);

  const qreal anchorViewportY = blockDocY(editor, anchorBlock) - vbar->value();
  const int originalMaximum = vbar->maximum();

  // One preview at a time, each ENTIRELY above the viewport: one relayout each,
  // which is exactly how the async results arrive.
  QVector<QSharedPointer<PreviewItem>> items;
  for (int i = 1; i <= 5; ++i) {
    const int previousValue = vbar->value();
    items.append(makeItem(editor.document(), i, c_previewHeight));
    editor.getPreviewMgr()->updateCodeBlocks(items);
    QCoreApplication::processEvents();

    // The bar moved by the height the content above the viewport gained: that
    // IS the compensation. Without it the value would be unchanged and the text
    // would have slid down instead.
    QVERIFY2(vbar->value() > previousValue, "the scrollbar was not compensated");
    QCOMPARE(TextEditUtils::firstVisibleBlock(textEdit).blockNumber(), anchorBlock);
    const qreal y = blockDocY(editor, anchorBlock) - vbar->value();
    QVERIFY2(qAbs(y - anchorViewportY) <= 1.0,
             qPrintable(QStringLiteral("anchor moved from %1 to %2 after %3 preview(s)")
                            .arg(anchorViewportY)
                            .arg(y)
                            .arg(i)));
  }

  // The document really did grow; the scrollbar absorbed all of it.
  QVERIFY2(vbar->maximum() > originalMaximum, "the previews did not grow the document");
}

void TestPreviewScrollStability::testOffScreenCaretIsNotChasedTo() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  setUpEditor(editor);

  auto textEdit = editor.getTextEdit();
  auto vbar = textEdit->verticalScrollBar();

  // Caret at the very top, viewport in the middle.
  QTextCursor cursor = textEdit->textCursor();
  cursor.setPosition(0);
  textEdit->setTextCursor(cursor);

  vbar->setValue((vbar->minimum() + vbar->maximum()) / 2);
  QCoreApplication::processEvents();

  const int anchorBlock = TextEditUtils::firstVisibleBlock(textEdit).blockNumber();
  QVERIFY(anchorBlock > 8);
  const qreal anchorViewportY = blockDocY(editor, anchorBlock) - vbar->value();

  QVector<QSharedPointer<PreviewItem>> items;
  items.append(makeItem(editor.document(), 2, c_previewHeight));
  editor.getPreviewMgr()->updateCodeBlocks(items);
  QCoreApplication::processEvents();

  // Without the gating this would have scrolled all the way back to block 0.
  QCOMPARE(TextEditUtils::firstVisibleBlock(textEdit).blockNumber(), anchorBlock);
  QVERIFY(qAbs((blockDocY(editor, anchorBlock) - vbar->value()) - anchorViewportY) <= 1.0);
}

void TestPreviewScrollStability::testVisibleCaretStillKeptVisible() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  setUpEditor(editor);

  auto textEdit = editor.getTextEdit();
  auto vbar = textEdit->verticalScrollBar();

  vbar->setValue((vbar->minimum() + vbar->maximum()) / 2);
  QCoreApplication::processEvents();

  const auto range = TextEditUtils::visibleBlockRange(textEdit);
  QVERIFY(range.second > range.first);

  // Caret on the LAST visible block, and a tall preview injected into the first
  // visible one, which pushes the caret past the viewport bottom. The caret was
  // visible before the relayout, so ensureCursorVisible() must still run.
  const int caretBlock = range.second;
  QTextCursor cursor = textEdit->textCursor();
  cursor.setPosition(editor.document()->findBlockByNumber(caretBlock).position());
  textEdit->setTextCursor(cursor);

  QVector<QSharedPointer<PreviewItem>> items;
  items.append(makeItem(editor.document(), range.first, textEdit->viewport()->height()));
  editor.getPreviewMgr()->updateCodeBlocks(items);
  QCoreApplication::processEvents();

  const auto newRange = TextEditUtils::visibleBlockRange(textEdit);
  QVERIFY2(caretBlock >= newRange.first && caretBlock <= newRange.second,
           qPrintable(QStringLiteral("caret block %1 left the visible range [%2, %3]")
                          .arg(caretBlock)
                          .arg(newRange.first)
                          .arg(newRange.second)));
}

void TestPreviewScrollStability::testNoCompensationAtTopOfDocument() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  setUpEditor(editor);

  auto textEdit = editor.getTextEdit();
  auto vbar = textEdit->verticalScrollBar();

  vbar->setValue(vbar->minimum());
  QTextCursor cursor = textEdit->textCursor();
  cursor.setPosition(0);
  textEdit->setTextCursor(cursor);
  QCoreApplication::processEvents();

  QVector<QSharedPointer<PreviewItem>> items;
  items.append(makeItem(editor.document(), 2, c_previewHeight));
  editor.getPreviewMgr()->updateCodeBlocks(items);
  QCoreApplication::processEvents();

  QCOMPARE(vbar->value(), vbar->minimum());
}

void TestPreviewScrollStability::testAutoFoldAboveViewportDoesNotMoveContent() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.resize(900, 600);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  editor.setText(codeBlocks(c_codeBlocks, c_codeBlockBodyLines));
  settle(editor);
  editor.getPreviewMgr()->setPreviewEnabled(PreviewData::Source::CodeBlock, true);

  auto textEdit = editor.getTextEdit();
  auto vbar = textEdit->verticalScrollBar();
  QVERIFY2(vbar->maximum() > vbar->minimum(), "the fixture does not scroll");

  // Every fence's closing line, which is the last block of its folding region -
  // one of the two blocks applyPreviewAutoFold() probes for a painted preview.
  // The opening fence is spelled "```dot", so a bare "```" is always a close.
  QVector<int> fenceEndBlocks;
  for (int i = 0; i < editor.document()->blockCount(); ++i) {
    if (editor.document()->findBlockByNumber(i).text() == QStringLiteral("```")) {
      fenceEndBlocks.append(i);
    }
  }
  QVERIFY(fenceEndBlocks.size() > 6);

  vbar->setValue((vbar->minimum() + vbar->maximum()) / 2);
  QCoreApplication::processEvents();

  const int anchorBlock = TextEditUtils::firstVisibleBlock(textEdit).blockNumber();
  const qreal anchorViewportY = blockDocY(editor, anchorBlock) - vbar->value();

  // Previews for the fences ABOVE the viewport only. Their arrival relayouts
  // the document AND schedules the fold refresh, so this exercises both
  // geometry changes in the order the editor really produces them.
  QVector<QSharedPointer<PreviewItem>> items;
  for (int blockNumber : fenceEndBlocks) {
    if (blockNumber >= anchorBlock) {
      break;
    }
    items.append(makeItem(editor.document(), blockNumber, c_previewHeight));
  }
  QVERIFY2(items.size() > 3, "not enough code blocks above the viewport");

  editor.getPreviewMgr()->updateCodeBlocks(items);

  // The fold refresh runs from the host's owed-work drain, not synchronously.
  QTRY_VERIFY_WITH_TIMEOUT(
      !editor.document()->findBlockByNumber(fenceEndBlocks.first() - 1).isVisible(), 5000);
  QTest::qWait(50);
  QCoreApplication::processEvents();

  // The anchor block survives the folds (every folded range is strictly above
  // it) and must still sit exactly where the user left it.
  QCOMPARE(TextEditUtils::firstVisibleBlock(textEdit).blockNumber(), anchorBlock);
  const qreal y = blockDocY(editor, anchorBlock) - vbar->value();
  QVERIFY2(qAbs(y - anchorViewportY) <= 1.0,
           qPrintable(QStringLiteral("anchor moved from %1 to %2 across the auto-fold")
                          .arg(anchorViewportY)
                          .arg(y)));
}

void TestPreviewScrollStability::testAutoFoldOfTheAnchorBlockItself() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.resize(900, 600);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  editor.setText(codeBlocks(c_codeBlocks, c_codeBlockBodyLines));
  settle(editor);
  editor.getPreviewMgr()->setPreviewEnabled(PreviewData::Source::CodeBlock, true);

  auto textEdit = editor.getTextEdit();
  auto vbar = textEdit->verticalScrollBar();
  QVERIFY2(vbar->maximum() > vbar->minimum(), "the fixture does not scroll");

  // A fence roughly in the middle of the document, and the interior body line
  // the viewport is going to be parked on.
  int openBlock = -1;
  for (int i = editor.document()->blockCount() / 2; i < editor.document()->blockCount(); ++i) {
    if (editor.document()->findBlockByNumber(i).text() == QStringLiteral("```dot")) {
      openBlock = i;
      break;
    }
  }
  QVERIFY(openBlock > 0);
  const int closeBlock = openBlock + c_codeBlockBodyLines + 1;
  QCOMPARE(editor.document()->findBlockByNumber(closeBlock).text(), QStringLiteral("```"));

  // Caret well outside the range, so the caret rule does not veto the fold.
  // Set BEFORE scrolling: setTextCursor() scrolls the caret into view.
  QTextCursor cursor = textEdit->textCursor();
  cursor.setPosition(0);
  textEdit->setTextCursor(cursor);

  // Scroll so the top of the viewport lands on an INTERIOR line of that fence,
  // i.e. on a block the fold is about to hide.
  const int interiorBlock = openBlock + 2;
  vbar->setValue(qRound(blockDocY(editor, interiorBlock)));
  QCoreApplication::processEvents();
  QCOMPARE(TextEditUtils::firstVisibleBlock(textEdit).blockNumber(), interiorBlock);

  QVector<QSharedPointer<PreviewItem>> items;
  items.append(makeItem(editor.document(), closeBlock, c_previewHeight));
  editor.getPreviewMgr()->updateCodeBlocks(items);

  QTRY_VERIFY_WITH_TIMEOUT(!editor.document()->findBlockByNumber(interiorBlock).isVisible(), 5000);
  QTest::qWait(50);
  QCoreApplication::processEvents();

  // The anchor was folded away. The recovery walks back to the range's first
  // block - the one setRangeFolded() keeps visible - so the collapsed range
  // sits at the top of the viewport instead of the view jumping elsewhere.
  QCOMPARE(TextEditUtils::firstVisibleBlock(textEdit).blockNumber(), openBlock);
}

void TestPreviewScrollStability::testLogicalImageSizePreservesLayout() {
  for (auto source : {PreviewData::Source::CodeBlock, PreviewData::Source::MathBlock}) {
    auto config = makeConfig();
    config->m_textEditorConfig->m_scaleFactor = 1.25;
    VMarkdownEditor editor(config, QSharedPointer<TextEditorParameters>::create());
    setUpEditor(editor);
    editor.getPreviewMgr()->setPreviewEnabled(source, true);
    auto publish = [&](const QSharedPointer<PreviewItem> &p_item) {
      if (source == PreviewData::Source::CodeBlock) {
        editor.getPreviewMgr()->updateCodeBlocks({p_item});
      } else {
        editor.getPreviewMgr()->updateMathBlocks({p_item});
      }
      QCoreApplication::processEvents();
    };

    auto item = makeItem(editor.document(), 2, 120);
    item->m_image = QPixmap(400, 120);
    item->m_image.fill(Qt::red);
    publish(item);
    const auto block = editor.document()->findBlockByNumber(2);
    const auto legacyRect = editor.document()->documentLayout()->blockBoundingRect(block);
    const auto legacyY = blockDocY(editor, 3);

    item->m_name += QStringLiteral("_dense");
    item->m_image = QPixmap(800, 240);
    item->m_image.fill(Qt::red);
    item->m_logicalSize = QSize(320, 96);
    publish(item);
    QCOMPARE(blockDocY(editor, 3), legacyY);
    QCOMPARE(editor.document()->documentLayout()->blockBoundingRect(block), legacyRect);
    const auto previews = BlockPreviewData::get(block)->getPreviewData();
    QCOMPARE(previews.size(), 1);
    const auto image =
        editor.findImageFromDocumentResourceMgr(previews.first()->getImageData()->m_imageName);
    QVERIFY(image);
    QCOMPARE(image->size(), QSize(800, 240));
  }
}

void TestPreviewScrollStability::testLogicalImageSizeChangeRelayoutsSharedResource() {
  auto config = makeConfig();
  config->m_textEditorConfig->m_scaleFactor = 1.25;
  VMarkdownEditor editor(config, QSharedPointer<TextEditorParameters>::create());
  setUpEditor(editor);
  auto item = makeItem(editor.document(), 2, 240);
  item->m_image = QPixmap(800, 240);
  item->m_image.fill(Qt::red);
  auto publish = [&]() {
    editor.getPreviewMgr()->updateCodeBlocks({item});
    QCoreApplication::processEvents();
  };
  publish();
  const auto intrinsicY = blockDocY(editor, 3);
  item->m_logicalSize = QSize(320, 96);
  publish();
  const auto originalY = blockDocY(editor, 3);
  item->m_logicalSize.setHeight(144);
  publish();
  QCOMPARE(blockDocY(editor, 3), originalY + 48);
  publish();
  QCOMPARE(blockDocY(editor, 3), originalY + 48);

  for (const auto &size : {QSize(), QSize(0, 0), QSize(320, 0), QSize(0, 96)}) {
    item->m_logicalSize = size;
    publish();
    QCOMPARE(blockDocY(editor, 3), intrinsicY);
  }

  item->m_logicalSize = QSize(320, 96);
  publish();
  item->clear();
  auto replacement = makeItem(editor.document(), 2, 240);
  item->m_blockNumber = replacement->m_blockNumber;
  item->m_blockPos = replacement->m_blockPos;
  item->m_startPos = replacement->m_startPos;
  item->m_endPos = replacement->m_endPos;
  item->m_isBlockwise = true;
  item->m_name = replacement->m_name;
  item->m_image = QPixmap(800, 240);
  item->m_image.fill(Qt::red);
  publish();
  QCOMPARE(blockDocY(editor, 3), intrinsicY);
}

void TestPreviewScrollStability::testInlineLogicalImageSizePreservesLayout() {
  auto config = makeConfig();
  config->m_textEditorConfig->m_scaleFactor = 1.25;
  VMarkdownEditor editor(config, QSharedPointer<TextEditorParameters>::create());
  editor.resize(900, 600);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  editor.setText(
      QStringLiteral("Before a sufficiently wide inline source span after.\nFollowing."));
  settle(editor);
  editor.getPreviewMgr()->setPreviewEnabled(PreviewData::Source::MathBlock, true);
  auto item = makeItem(editor.document(), 0, 30);
  item->m_isBlockwise = false;
  item->m_startPos = 7;
  item->m_endPos = 45;
  item->m_image = QPixmap(100, 30);
  item->m_image.fill(Qt::red);
  editor.getPreviewMgr()->updateMathBlocks({item});
  QCoreApplication::processEvents();
  const auto block = editor.document()->firstBlock();
  const auto legacyRect = editor.document()->documentLayout()->blockBoundingRect(block);
  const auto legacyLineRect = block.layout()->lineAt(0).rect();
  const auto legacyY = blockDocY(editor, 1);

  item->m_name += QStringLiteral("_dense");
  item->m_image = QPixmap(200, 60);
  item->m_image.fill(Qt::red);
  item->m_logicalSize = QSize(80, 24);
  editor.getPreviewMgr()->updateMathBlocks({item});
  QCoreApplication::processEvents();
  QCOMPARE(blockDocY(editor, 1), legacyY);
  QCOMPARE(editor.document()->documentLayout()->blockBoundingRect(block), legacyRect);
  QCOMPARE(block.layout()->lineAt(0).rect(), legacyLineRect);
  const auto previews = BlockPreviewData::get(block)->getPreviewData();
  QCOMPARE(previews.size(), 1);
  const auto image =
      editor.findImageFromDocumentResourceMgr(previews.first()->getImageData()->m_imageName);
  QVERIFY(image);
  QCOMPARE(image->size(), QSize(200, 60));
}

QTEST_MAIN(tests::TestPreviewScrollStability)
