#include "textdocumentlayout.h"

#include <QDebug>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QPointF>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextLayout>

#include <vtextedit/previewdata.h>
#include <vtextedit/textblockdata.h>

#include "documentresourcemgr.h"
#include "peghighlightblockdata.h"

using namespace vte;

const int TextDocumentLayout::c_markerThickness = 2;

const int TextDocumentLayout::c_maxInlineImageHeight = 400;

const int TextDocumentLayout::c_imagePadding = 2;

static bool realEqual(qreal p_a, qreal p_b) { return qAbs(p_a - p_b) < 1e-8; }

TextDocumentLayout::TextDocumentLayout(QTextDocument *p_doc, DocumentResourceMgr *p_resourceMgr)
    : QAbstractTextDocumentLayout(p_doc), m_margin(p_doc->documentMargin()),
      m_resourceMgr(p_resourceMgr) {}

static void fillBackground(QPainter *p_painter, const QRectF &p_rect, QBrush p_brush,
                           QRectF p_gradientRect = QRectF()) {
  p_painter->save();
  if (p_brush.style() >= Qt::LinearGradientPattern &&
      p_brush.style() <= Qt::ConicalGradientPattern) {
    if (!p_gradientRect.isNull()) {
      QTransform m = QTransform::fromTranslate(p_gradientRect.left(), p_gradientRect.top());
      m.scale(p_gradientRect.width(), p_gradientRect.height());
      p_brush.setTransform(m);
      const_cast<QGradient *>(p_brush.gradient())->setCoordinateMode(QGradient::LogicalMode);
    }
  } else {
    p_painter->setBrushOrigin(p_rect.topLeft());
  }

  p_painter->fillRect(p_rect, p_brush);
  p_painter->restore();
}

void TextDocumentLayout::blockRangeFromRect(const QRectF &p_rect, int &p_first, int &p_last) const {
  if (p_rect.isNull()) {
    p_first = 0;
    p_last = document()->blockCount() - 1;
    return;
  }

  p_first = -1;
  p_last = document()->blockCount() - 1;
  int y = p_rect.y();
  QTextBlock block = document()->firstBlock();
  while (block.isValid()) {
    auto info = BlockLayoutData::get(block);
    Q_ASSERT(info->hasOffset());

    if (info->top() == y || (info->top() < y && info->bottom() >= y)) {
      p_first = block.blockNumber();
      break;
    }

    block = block.next();
  }

  if (p_first == -1) {
    p_last = -1;
    return;
  }

  y += p_rect.height();
  while (block.isValid()) {
    auto info = BlockLayoutData::get(block);
    Q_ASSERT(info->hasOffset());

    if (info->bottom() > y) {
      p_last = block.blockNumber();
      break;
    }

    block = block.next();
  }
}

void TextDocumentLayout::blockRangeFromRectBS(const QRectF &p_rect, int &p_first,
                                              int &p_last) const {
  if (p_rect.isNull()) {
    p_first = 0;
    p_last = document()->blockCount() - 1;
    return;
  }

  p_first = findBlockByPosition(p_rect.topLeft());
  if (p_first == -1) {
    p_last = -1;
    return;
  }

  int y = p_rect.bottom();
  QTextBlock block = document()->findBlockByNumber(p_first);
  auto info = BlockLayoutData::get(block);
  if (realEqual(info->top(), p_rect.top()) && p_first > 0) {
    --p_first;
  }

  p_last = document()->blockCount() - 1;
  while (block.isValid()) {
    auto tinfo = BlockLayoutData::get(block);
    if (!tinfo->hasOffset()) {
      qWarning() << "block without offset" << block.blockNumber() << tinfo->m_offset
                 << tinfo->m_rect << tinfo->m_rect.isNull();
    }

    Q_ASSERT(tinfo->hasOffset());

    if (tinfo->bottom() > y) {
      p_last = block.blockNumber();
      break;
    }

    block = block.next();
  }
}

int TextDocumentLayout::findBlockByPosition(const QPointF &p_point) const {
  QTextDocument *doc = document();
  int first = 0, last = doc->blockCount() - 1;
  int y = p_point.y();
  while (first <= last) {
    int mid = (first + last) / 2;
    QTextBlock blk = doc->findBlockByNumber(mid);
    auto info = BlockLayoutData::get(blk);
    if (!info) {
      return -1;
    }

    Q_ASSERT(info->hasOffset());
    if (info->top() <= y && info->bottom() > y) {
      // Found it.
      return mid;
    } else if (info->top() > y) {
      last = mid - 1;
    } else {
      first = mid + 1;
    }
  }

  QTextBlock blk = doc->lastBlock();
  auto info = BlockLayoutData::get(blk);
  if (y >= info->bottom()) {
    return blk.blockNumber();
  }

  return 0;
}

void TextDocumentLayout::draw(QPainter *p_painter, const PaintContext &p_context) {
  // Find out the blocks.
  int first, last;
  blockRangeFromRectBS(p_context.clip, first, last);
  if (first == -1) {
    return;
  }

  p_painter->setRenderHints(QPainter::SmoothPixmapTransform | QPainter::Antialiasing);

  QTextDocument *doc = document();
  QTextBlock block = doc->findBlockByNumber(first);
  QPointF offset(m_margin, BlockLayoutData::get(block)->top());
  QTextBlock lastBlock = doc->findBlockByNumber(last);

  QPen oldPen = p_painter->pen();
  p_painter->setPen(p_context.palette.color(QPalette::Text));

  while (block.isValid()) {
    auto info = BlockLayoutData::get(block);
    Q_ASSERT(info->hasOffset());

    const QRectF &rect = info->m_rect;
    QTextLayout *layout = block.layout();
    if (!block.isVisible()) {
      offset.ry() += rect.height();
      if (block == lastBlock) {
        break;
      }

      block = block.next();
      continue;
    }

    QTextBlockFormat blockFormat = block.blockFormat();
    QBrush bg = blockFormat.background();
    if (bg != Qt::NoBrush) {
      int x = offset.x();
      int y = offset.y();
      fillBackground(p_painter, rect.adjusted(x, y, x, y), bg);
    }

    auto selections = formatRangeFromSelection(block, p_context.selections);

    layout->draw(p_painter, offset, selections,
                 p_context.clip.isValid() ? p_context.clip : QRectF());

    drawPreview(p_painter, block, offset);

    drawPreviewMarker(p_painter, block, offset);

    // Draw the cursor.
    {
      int blpos = block.position();
      int bllen = block.length();
      bool drawCursor =
          p_context.cursorPosition >= blpos && p_context.cursorPosition < blpos + bllen;
      if (drawCursor || (p_context.cursorPosition < -1 && !layout->preeditAreaText().isEmpty())) {
        int cursorPosition = p_context.cursorPosition - blpos;
        if (p_context.cursorPosition < -1) {
          cursorPosition = layout->preeditAreaPosition() - (p_context.cursorPosition + 2);
        }

        layout->drawCursor(p_painter, offset, cursorPosition, m_cursorWidth);
      }
    }

    offset.ry() += rect.height();
    if (block == lastBlock) {
      break;
    }

    block = block.next();
  }

  p_painter->setPen(oldPen);
}

QVector<QTextLayout::FormatRange>
TextDocumentLayout::formatRangeFromSelection(const QTextBlock &p_block,
                                             const QVector<Selection> &p_selections) const {
  QVector<QTextLayout::FormatRange> ret;

  int blpos = p_block.position();
  int bllen = p_block.length();
  for (int i = 0; i < p_selections.size(); ++i) {
    const QAbstractTextDocumentLayout::Selection &range = p_selections.at(i);
    const int selStart = range.cursor.selectionStart() - blpos;
    const int selEnd = range.cursor.selectionEnd() - blpos;
    if (selStart < bllen && selEnd > 0 && selEnd > selStart) {
      QTextLayout::FormatRange o;
      o.start = selStart;
      o.length = selEnd - selStart;
      o.format = range.format;
      ret.append(o);
    } else if (!range.cursor.hasSelection() &&
               range.format.hasProperty(QTextFormat::FullWidthSelection) &&
               p_block.contains(range.cursor.position())) {
      // For full width selections we don't require an actual selection, just
      // a position to specify the line. that's more convenience in usage.
      QTextLayout::FormatRange o;
      QTextLine l = p_block.layout()->lineForTextPosition(range.cursor.position() - blpos);
      if (!l.isValid()) {
        qWarning() << "invalid layout lineForTextPosition" << p_block.blockNumber()
                   << range.cursor.position() << blpos;
        Q_ASSERT(false);
        continue;
      }
      o.start = l.textStart();
      o.length = l.textLength();
      if (o.start + o.length == bllen - 1) {
        ++o.length; // include newline
      }

      o.format = range.format;
      ret.append(o);
    }
  }

  return ret;
}

int TextDocumentLayout::hitTest(const QPointF &p_point, Qt::HitTestAccuracy p_accuracy) const {
  Q_UNUSED(p_accuracy);
  int bn = findBlockByPosition(p_point);
  if (bn == -1) {
    return -1;
  }

  QTextBlock block = document()->findBlockByNumber(bn);
  Q_ASSERT(block.isValid());
  QTextLayout *layout = block.layout();
  int off = 0;
  QPointF pos = p_point - QPointF(m_margin, BlockLayoutData::get(block)->top());
  for (int i = 0; i < layout->lineCount(); ++i) {
    QTextLine line = layout->lineAt(i);
    const QRectF lr = line.naturalTextRect();
    if (lr.top() > pos.y()) {
      off = qMin(off, line.textStart());
    } else if (lr.bottom() <= pos.y()) {
      off = qMax(off, line.textStart() + line.textLength());
    } else {
      off = line.xToCursor(pos.x(), QTextLine::CursorBetweenCharacters);
      break;
    }
  }

  return block.position() + off;
}

int TextDocumentLayout::pageCount() const { return 1; }

QSizeF TextDocumentLayout::documentSize() const { return QSizeF(m_width, m_height); }

QRectF TextDocumentLayout::frameBoundingRect(QTextFrame *p_frame) const {
  Q_UNUSED(p_frame);
  return QRectF(0, 0, qMax(document()->pageSize().width(), m_width), qreal(INT_MAX));
}

// Sometimes blockBoundingRect() may be called before documentChanged().
QRectF TextDocumentLayout::blockBoundingRect(const QTextBlock &p_block) const {
  if (!p_block.isValid()) {
    return QRectF();
  }

  auto info = BlockLayoutData::get(p_block);
  if (!info->hasOffset()) {
    if (info->isNull()) {
      const_cast<TextDocumentLayout *>(this)->layoutBlockAndUpdateOffset(p_block);
    } else {
      const_cast<TextDocumentLayout *>(this)->updateOffset(p_block);
    }
  }

  QRectF geo = info->m_rect.adjusted(0, info->m_offset, 0, info->m_offset);
  return geo;
}

void TextDocumentLayout::documentChanged(int p_from, int p_charsRemoved, int p_charsAdded) {
  QTextDocument *doc = document();
  int newBlockCount = doc->blockCount();

  // Update the margin.
  m_margin = doc->documentMargin();

  int charsChanged = p_charsRemoved + p_charsAdded;

  QTextBlock changeStartBlock = doc->findBlock(p_from);
  // May be an invalid block.
  QTextBlock changeEndBlock;
  if (p_charsRemoved == p_charsAdded && newBlockCount == m_blockCount &&
      changeStartBlock.position() == p_from && changeStartBlock.length() == p_charsAdded) {
    // TODO: we may need one more next block.
    changeEndBlock = changeStartBlock;
  } else {
    changeEndBlock = doc->findBlock(p_from + charsChanged);
  }

  /*
  qDebug() << "documentChanged" << p_from << p_charsRemoved << p_charsAdded
           << m_blockCount << newBlockCount
           << changeStartBlock.blockNumber() << changeEndBlock.blockNumber();
  */

  bool needRelayout = true;
  if (changeStartBlock == changeEndBlock && newBlockCount == m_blockCount) {
    // Change single block internal only.
    QTextBlock block = changeStartBlock;
    if (block.isValid() && block.length()) {
      needRelayout = false;
      QRectF oldBr = blockBoundingRect(block);
      clearBlockLayout(block);
      layoutBlockAndUpdateOffset(block);
      QRectF newBr = blockBoundingRect(block);
      // Only one block is affected.
      if (newBr.height() == oldBr.height()) {
        // Update document size.
        updateDocumentSizeWithOneBlockChanged(block);

        emit updateBlock(block);
        return;
      }
    }
  }

  if (needRelayout) {
    QTextBlock block = changeStartBlock;
    do {
      clearBlockLayout(block);
      layoutBlock(block);
      if (block == changeEndBlock) {
        break;
      }

      block = block.next();
    } while (block.isValid());

    updateOffset(changeStartBlock);
  }

  m_blockCount = newBlockCount;

  updateDocumentSize();

  // TODO: Update the view of all the blocks after changeStartBlock.
  qreal offset = BlockLayoutData::get(changeStartBlock)->m_offset;
  emit update(QRectF(0., offset, 1000000000., 1000000000.));
}

// MUST layout out the block after clearBlockLayout().
// TODO: Do we need to clear all the offset after @p_block?
void TextDocumentLayout::clearBlockLayout(QTextBlock &p_block) {
  p_block.clearLayout();
  auto info = BlockLayoutData::get(p_block);
  info->reset();
}

// From Qt's qguiapplication_p.h.
static Qt::Alignment visualAlignment(Qt::LayoutDirection p_direction, Qt::Alignment p_alignment) {
  if (!(p_alignment & Qt::AlignHorizontal_Mask)) {
    p_alignment |= Qt::AlignLeft;
  }

  if (!(p_alignment & Qt::AlignAbsolute) && (p_alignment & (Qt::AlignLeft | Qt::AlignRight))) {
    if (p_direction == Qt::RightToLeft) {
      p_alignment ^= (Qt::AlignLeft | Qt::AlignRight);
    }

    p_alignment |= Qt::AlignAbsolute;
  }

  return p_alignment;
}

void TextDocumentLayout::layoutBlock(const QTextBlock &p_block) {
  QTextDocument *doc = document();
  Q_ASSERT(m_margin == doc->documentMargin());

  QTextLayout *tl = p_block.layout();
  QTextOption option = doc->defaultTextOption();

  {
    auto direction = p_block.textDirection();
    option.setTextDirection(direction);

    auto alignment = option.alignment();
    QTextBlockFormat blockFormat = p_block.blockFormat();
    if (blockFormat.hasProperty(QTextFormat::BlockAlignment)) {
      alignment = blockFormat.alignment();
    }

    // For paragraph that are RTL, alignment is auto-reversed.
    option.setAlignment(visualAlignment(direction, alignment));
  }

  tl->setTextOption(option);

  int extraMargin = 0;
  if (option.flags() & QTextOption::AddSpaceForLineAndParagraphSeparators) {
    QFontMetrics fm(p_block.charFormat().font());
    extraMargin += fm.horizontalAdvance(QChar(0x21B5));
  }

  qreal availableWidth = doc->pageSize().width();
  if (availableWidth <= 0 || !shouldBlockWrapLine(p_block)) {
    availableWidth = qreal(INT_MAX);
  }

  availableWidth -= (2 * m_margin + extraMargin + m_cursorMargin + m_cursorWidth);

  QVector<Marker> markers;
  QVector<ImagePaintData> images;

  layoutLines(p_block, tl, markers, images, availableWidth, 0);

  // Set this block's line count to its layout's line count.
  // That is one block may occupy multiple visual lines.
  const_cast<QTextBlock &>(p_block).setLineCount(p_block.isVisible() ? tl->lineCount() : 0);

  // Update the info about this block.
  finishBlockLayout(p_block, markers, images);
}

void TextDocumentLayout::updateOffsetBefore(const QTextBlock &p_block) {
  auto info = BlockLayoutData::get(p_block);
  Q_ASSERT(!info->isNull());

  const int blockNum = p_block.blockNumber();
  if (blockNum == 0) {
    info->m_offset = 0;
  } else {
    QTextBlock blk = p_block.previous();
    while (blk.isValid()) {
      auto pinfo = BlockLayoutData::get(blk);
      if (!pinfo->hasOffset()) {
        int num = blk.blockNumber();
        if (pinfo->isNull()) {
          layoutBlock(blk);
        }

        if (num == 0) {
          pinfo->m_offset = 0;
        } else {
          blk = blk.previous();
          continue;
        }
      }

      // Now we reach a block with offset.
      qreal offset = pinfo->bottom();
      blk = blk.next();
      while (blk.isValid() && blk.blockNumber() <= blockNum) {
        auto ninfo = BlockLayoutData::get(blk);
        Q_ASSERT(!ninfo->isNull());
        ninfo->m_offset = offset;
        offset = ninfo->bottom();
        blk = blk.next();
      }

      break;
    }

    Q_ASSERT(info->hasOffset());
  }
}

// NOTICE: It will skip non-layouted or offset-non-changed blocks.
// So if you relayout separated blocks, you need to updateOffsetAfter() for each
// of them.
void TextDocumentLayout::updateOffsetAfter(const QTextBlock &p_block) {
  auto info = BlockLayoutData::get(p_block);
  Q_ASSERT(info->hasOffset());
  qreal offset = info->bottom();
  QTextBlock blk = p_block.next();
  while (blk.isValid()) {
    auto ninfo = BlockLayoutData::get(blk);
    if (ninfo->isNull()) {
      break;
    }

    if (realEqual(ninfo->m_offset, offset)) {
      break;
    }

    ninfo->m_offset = offset;
    offset = ninfo->bottom();
    blk = blk.next();
  }
}

qreal TextDocumentLayout::layoutLines(const QTextBlock &p_block, QTextLayout *p_tl,
                                      QVector<Marker> &p_markers, QVector<ImagePaintData> &p_images,
                                      qreal p_availableWidth, qreal p_height) {
  Q_ASSERT(p_block.isValid());

  // Handle block inline image.
  const QVector<PreviewData *> *pPreviewData = nullptr;
  if (m_previewEnabled) {
    const auto &previewData = BlockPreviewData::get(p_block)->getPreviewData();
    if (!previewData.isEmpty()) {
      auto imageData = previewData.first()->getImageData();
      if (imageData && imageData->m_inline) {
        pPreviewData = &previewData;
      }
    }
  }

  p_tl->beginLayout();

  int imgIdx = 0;
  while (true) {
    QTextLine line = p_tl->createLine();
    if (!line.isValid()) {
      break;
    }

    // Will introduce extra space on macOS.
    // line.setLeadingIncluded(true);
    line.setLineWidth(p_availableWidth);
    p_height += m_leadingSpaceOfLine;

    if (pPreviewData) {
      QVector<const PreviewImageData *> images;
      QVector<QPair<qreal, qreal>> imageRange;
      qreal imgHeight =
          fetchInlineImagesForOneLine(*pPreviewData, &line, m_margin, imgIdx, images, imageRange);

      for (int i = 0; i < images.size(); ++i) {
        layoutInlineImage(images[i], p_height, imgHeight, imageRange[i].first, imageRange[i].second,
                          p_markers, p_images);
      }

      if (!images.isEmpty()) {
        p_height += imgHeight + c_markerThickness * 2 + c_imagePadding * 2;
      }
    }

    line.setPosition(QPointF(m_margin, p_height));
    p_height += line.height();
  }

  p_tl->endLayout();

  return p_height;
}

void TextDocumentLayout::layoutInlineImage(const PreviewImageData *p_data, qreal p_heightInBlock,
                                           qreal p_imageSpaceHeight, qreal p_xStart, qreal p_xEnd,
                                           QVector<Marker> &p_markers,
                                           QVector<ImagePaintData> &p_images) {
  Marker mk;
  qreal mky = p_imageSpaceHeight + p_heightInBlock + c_imagePadding * 2 + c_markerThickness;
  mk.m_start = QPointF(p_xStart, mky);
  mk.m_end = QPointF(p_xEnd, mky);
  p_markers.append(mk);

  if (p_data) {
    QSize size = p_data->m_imageSize;
    scaleSize(size, p_xEnd - p_xStart, p_imageSpaceHeight);

    ImagePaintData ipd;
    ipd.m_name = p_data->m_imageName;
    ipd.m_rect = QRectF(
        QPointF(p_xStart, p_heightInBlock + c_imagePadding + p_imageSpaceHeight - size.height()),
        size);
    if (p_data->m_backgroundColor != 0) {
      ipd.m_backgroundColor = QColor(p_data->m_backgroundColor);
    }

    p_images.append(ipd);
  }
}

void TextDocumentLayout::finishBlockLayout(const QTextBlock &p_block,
                                           const QVector<Marker> &p_markers,
                                           const QVector<ImagePaintData> &p_images) {
  Q_ASSERT(p_block.isValid());
  ImagePaintData ipd;
  auto info = BlockLayoutData::get(p_block);
  Q_ASSERT(info->isNull());
  info->reset();
  info->m_rect = blockRectFromTextLayout(p_block, &ipd);
  Q_ASSERT(!info->m_rect.isNull());

  bool hasImage = false;
  if (ipd.isValid()) {
    Q_ASSERT(p_markers.isEmpty());
    Q_ASSERT(p_images.isEmpty());
    info->m_images.append(ipd);
    hasImage = true;
  } else if (!p_markers.isEmpty()) {
    info->m_markers = p_markers;
    info->m_images = p_images;
    hasImage = true;
  }

  // Add vertical marker.
  if (hasImage) {
    // Fill the marker.
    // Will be adjusted using offset.
    Marker mk;
    mk.m_start = QPointF(-1, 0);
    mk.m_end = QPointF(-1, info->m_rect.height());

    info->m_markers.append(mk);
  }
}

void TextDocumentLayout::updateDocumentSize() {
  QTextBlock block = document()->lastBlock();
  auto info = BlockLayoutData::get(block);
  if (!info->hasOffset()) {
    if (info->isNull()) {
      layoutBlock(block);
    }

    updateOffsetBefore(block);
  }

  int oldHeight = m_height;
  int oldWidth = m_width;

  m_height = info->bottom();

  m_width = 0;
  QTextBlock blk = document()->firstBlock();
  while (blk.isValid()) {
    auto ninfo = BlockLayoutData::get(blk);
    Q_ASSERT(ninfo->hasOffset());
    if (m_width < ninfo->m_rect.width()) {
      m_width = ninfo->m_rect.width();
      m_maximumWidthBlockNumber = blk.blockNumber();
    }

    blk = blk.next();
  }

  if (oldHeight != m_height || oldWidth != m_width) {
    emit documentSizeChanged(documentSize());
  }
}

QRectF TextDocumentLayout::blockRectFromTextLayout(const QTextBlock &p_block,
                                                   ImagePaintData *p_image) {
  if (p_image) {
    *p_image = ImagePaintData();
  }

  QTextLayout *tl = p_block.layout();
  if (tl->lineCount() < 1) {
    return QRectF();
  }

  QRectF tlRect = tl->boundingRect();
  QRectF br(QPointF(0, 0), tlRect.bottomRight());

  // Do not know why. Copied from QPlainTextDocumentLayout.
  if (tl->lineCount() == 1) {
    br.setWidth(qMax(br.width(), tl->lineAt(0).naturalTextWidth()));
  }

  // Handle block non-inline image.
  if (m_previewEnabled) {
    const auto &previewData = BlockPreviewData::get(p_block)->getPreviewData();
    if (previewData.size() == 1) {
      auto img = previewData.first()->getImageData();
      if (!img->m_inline) {
        int maximumWidth = tlRect.width();
        int padding;
        QSize size;
        adjustImagePaddingAndSize(img, maximumWidth, padding, size);

        if (p_image) {
          p_image->m_name = img->m_imageName;
          p_image->m_rect =
              QRectF(padding + m_margin, br.height() + m_leadingSpaceOfLine + c_imagePadding,
                     size.width(), size.height());
          if (img->m_backgroundColor != 0) {
            p_image->m_backgroundColor = QColor(img->m_backgroundColor);
          }
        }

        int dw = padding + size.width() + m_margin - br.width();
        int dh = size.height() + m_leadingSpaceOfLine + c_imagePadding * 2;
        br.adjust(0, 0, dw > 0 ? dw : 0, dh);
      }
    }
  }

  // Add margins to both sides.
  br.adjust(0, 0, m_margin * 2 + m_cursorWidth, 0);

  // Add bottom margin.
  if (!p_block.next().isValid()) {
    br.adjust(0, 0, 0, m_margin);
  }

  return br;
}

void TextDocumentLayout::updateDocumentSizeWithOneBlockChanged(const QTextBlock &p_block) {
  auto info = BlockLayoutData::get(p_block);
  qreal width = info->m_rect.width();
  if (width > m_width) {
    m_width = width;
    m_maximumWidthBlockNumber = p_block.blockNumber();
    emit documentSizeChanged(documentSize());
  } else if (width < m_width && p_block.blockNumber() == m_maximumWidthBlockNumber) {
    // Shrink the longest block.
    updateDocumentSize();
  }
}

void TextDocumentLayout::adjustImagePaddingAndSize(const PreviewImageData *p_data,
                                                   int p_maximumWidth, int &p_padding,
                                                   QSize &p_size) const {
  const int minimumImageWidth = 400;

  p_padding = p_data->m_padding;
  p_size = p_data->m_imageSize;

  if (!m_constrainPreviewWidthEnabled) {
    return;
  }

  int availableWidth = p_maximumWidth - p_data->m_padding;
  if (availableWidth < p_data->m_imageSize.width()) {
    // Need to resize the width.
    if (availableWidth >= minimumImageWidth) {
      p_size.scale(availableWidth, p_size.height(), Qt::KeepAspectRatio);
    } else {
      // Omit the padding.
      p_padding = 0;
      p_size.scale(p_maximumWidth, p_size.height(), Qt::KeepAspectRatio);
    }
  }
}

void TextDocumentLayout::drawPreview(QPainter *p_painter, const QTextBlock &p_block,
                                     const QPointF &p_offset) {
  const QVector<ImagePaintData> &images = BlockLayoutData::get(p_block)->m_images;
  if (images.isEmpty()) {
    return;
  }

  for (auto const &img : images) {
    const QPixmap *image = m_resourceMgr->findImage(img.m_name);
    if (!image) {
      continue;
    }

    QRect targetRect =
        img.m_rect.adjusted(p_offset.x(), p_offset.y(), p_offset.x(), p_offset.y()).toRect();

    // Qt do not render the background of some SVGs.
    // We add a forced background mechanism to complement this.
    if (img.hasForcedBackground()) {
      p_painter->fillRect(targetRect, img.m_backgroundColor);
    }

    p_painter->drawPixmap(targetRect, *image);
  }
}

void TextDocumentLayout::drawPreviewMarker(QPainter *p_painter, const QTextBlock &p_block,
                                           const QPointF &p_offset) {
  const QVector<Marker> &markers = BlockLayoutData::get(p_block)->m_markers;
  if (markers.isEmpty()) {
    return;
  }

  QPen oldPen = p_painter->pen();
  QPen newPen(m_previewMarkerForeground, c_markerThickness, Qt::DashLine);
  p_painter->setPen(newPen);

  for (auto const &mk : markers) {
    p_painter->drawLine(mk.m_start + p_offset, mk.m_end + p_offset);
  }

  p_painter->setPen(oldPen);
}

void TextDocumentLayout::relayout() {
  QTextDocument *doc = document();

  // Update the margin.
  m_margin = doc->documentMargin();

  QTextBlock block = doc->firstBlock();
  while (block.isValid()) {
    clearBlockLayout(block);
    layoutBlock(block);

    block = block.next();
  }

  updateOffset(doc->firstBlock());

  updateDocumentSize();

  emit update(QRectF(0., 0., 1000000000., 1000000000.));
}

void TextDocumentLayout::relayout(const OrderedIntSet &p_blocks) {
  if (p_blocks.isEmpty()) {
    return;
  }

  QTextDocument *doc = document();

  // Need to relayout and update blocks in ascending order.
  QVector<QTextBlock> blocks;
  blocks.reserve(p_blocks.size());
  for (auto bn = p_blocks.keyBegin(); bn != p_blocks.keyEnd(); ++bn) {
    QTextBlock block = doc->findBlockByNumber(*bn);
    if (block.isValid()) {
      blocks.append(block);
      clearBlockLayout(block);
      layoutBlock(block);
    }
  }

  if (blocks.isEmpty()) {
    return;
  }

  // Need to update offset for each of these discontinuous blocks, because
  // the offset of the non-touched blocks may be the same but there are still
  // touched blocks after them.
  for (auto &blk : blocks) {
    updateOffset(blk);
  }

  updateDocumentSize();

  qreal offset = BlockLayoutData::get(blocks.first())->m_offset;
  emit update(QRectF(0., offset, 1000000000., 1000000000.));
}

qreal TextDocumentLayout::fetchInlineImagesForOneLine(const QVector<PreviewData *> &p_data,
                                                      const QTextLine *p_line, qreal p_margin,
                                                      int &p_index,
                                                      QVector<const PreviewImageData *> &p_images,
                                                      QVector<QPair<qreal, qreal>> &p_imageRange) {
  qreal maxHeight = 0;
  int start = p_line->textStart();
  int end = p_line->textLength() + start;

  for (int i = 0; i < p_data.size(); ++i) {
    auto img = p_data[i]->getImageData();
    Q_ASSERT(img && img->m_inline);

    if (img->m_startPos >= start && img->m_startPos < end) {
      // Start of a new image.
      qreal startX = p_line->cursorToX(img->m_startPos) + p_margin;
      qreal endX;
      if (img->m_endPos <= end) {
        // End an image.
        endX = p_line->cursorToX(img->m_endPos) + p_margin;
        p_images.append(img);
        p_imageRange.append(QPair<qreal, qreal>(startX, endX));

        QSize size = img->m_imageSize;
        scaleSize(size, endX - startX, c_maxInlineImageHeight);
        if (size.height() > maxHeight) {
          maxHeight = size.height();
        }

        // Image i has been drawn.
        p_index = i + 1;
      } else {
        // This image cross the line.
        endX = p_line->x() + p_line->width() + p_margin;
        if (end - img->m_startPos >= ((img->m_endPos - img->m_startPos) >> 1)) {
          // Put image at this side.
          p_images.append(img);
          p_imageRange.append(QPair<qreal, qreal>(startX, endX));

          QSize size = img->m_imageSize;
          scaleSize(size, endX - startX, c_maxInlineImageHeight);
          if (size.height() > maxHeight) {
            maxHeight = size.height();
          }

          // Image i has been drawn.
          p_index = i + 1;
        } else {
          // Just put a marker here.
          p_images.append(NULL);
          p_imageRange.append(QPair<qreal, qreal>(startX, endX));
        }

        break;
      }
    } else if (img->m_endPos > start && img->m_startPos < start) {
      qreal startX = p_line->x() + p_margin;
      qreal endX =
          img->m_endPos > end ? p_line->x() + p_line->width() : p_line->cursorToX(img->m_endPos);
      if (p_index <= i) {
        // Image i has not been drawn. Draw it here.
        p_images.append(img);
        p_imageRange.append(QPair<qreal, qreal>(startX, endX));

        QSize size = img->m_imageSize;
        scaleSize(size, endX - startX, c_maxInlineImageHeight);
        if (size.height() > maxHeight) {
          maxHeight = size.height();
        }

        // Image i has been drawn.
        p_index = i + 1;
      } else {
        // Image i has been drawn. Just put a marker here.
        p_images.append(NULL);
        p_imageRange.append(QPair<qreal, qreal>(startX, endX));
      }

      if (img->m_endPos >= end) {
        break;
      }
    } else if (img->m_endPos <= start) {
      continue;
    } else {
      break;
    }
  }

  return maxHeight;
}

int TextDocumentLayout::getTextWidthWithinTextLine(const QTextLayout *p_layout, int p_pos,
                                                   int p_length) {
  QTextLine line = p_layout->lineForTextPosition(p_pos);
  Q_ASSERT(line.isValid());
  Q_ASSERT(p_pos + p_length <= line.textStart() + line.textLength());
  Q_ASSERT(p_pos + p_length >= 0);
  return qAbs(line.cursorToX(p_pos + p_length) - line.cursorToX(p_pos));
}

void TextDocumentLayout::updateBlockByNumber(int p_blockNumber) {
  if (p_blockNumber == -1) {
    return;
  }

  QTextBlock block = document()->findBlockByNumber(p_blockNumber);
  if (block.isValid()) {
    emit updateBlock(block);
  }
}

void TextDocumentLayout::scaleSize(QSize &p_size, int p_width, int p_height) {
  if (p_size.width() > p_width || p_size.height() > p_height) {
    p_size.scale(p_width, p_height, Qt::KeepAspectRatio);
  }
}

void TextDocumentLayout::setCursorWidth(int p_width) { m_cursorWidth = p_width; }

int TextDocumentLayout::cursorWidth() const { return m_cursorWidth; }

void TextDocumentLayout::layoutBlockAndUpdateOffset(const QTextBlock &p_block) {
  layoutBlock(p_block);
  updateOffset(p_block);
}

void TextDocumentLayout::updateOffset(const QTextBlock &p_block) {
  updateOffsetBefore(p_block);
  updateOffsetAfter(p_block);
}

void TextDocumentLayout::setPreviewMarkerForeground(const QColor &p_color) {
  m_previewMarkerForeground = p_color;
}

void TextDocumentLayout::setConstrainPreviewWidthEnabled(bool p_enabled) {
  if (m_constrainPreviewWidthEnabled == p_enabled) {
    return;
  }

  m_constrainPreviewWidthEnabled = p_enabled;
  relayout();
}

void TextDocumentLayout::setPreviewEnabled(bool p_enabled) {
  if (m_previewEnabled == p_enabled) {
    return;
  }

  m_previewEnabled = p_enabled;
  relayout();
}

qreal TextDocumentLayout::getLeadingSpaceOfLine() const { return m_leadingSpaceOfLine; }

void TextDocumentLayout::setLeadingSpaceOfLine(qreal p_leading) {
  if (p_leading >= 0) {
    m_leadingSpaceOfLine = p_leading;
  }
}

bool TextDocumentLayout::shouldBlockWrapLine(const QTextBlock &p_block) const {
  return PegHighlightBlockData::get(p_block)->getWrapLineEnabled();
}
