#include "indicatorsborder.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFontMetricsF>
#include <QPaintEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QTimer>
#include <QWheelEvent>

#include <cmath>

#include "textfolding.h"
#include <vtextedit/textblockdata.h>
#include <vtextedit/textrange.h>

using namespace vte;

IndicatorsBorder::IndicatorsBorder(IndicatorsBorderInterface *p_interface,
                                   IndicatorsBorder::LineNumberType p_lineNumberType,
                                   bool p_enableTextFolding, QWidget *p_parent)
    : QWidget(p_parent), m_interface(p_interface), m_lineNumberType(p_lineNumberType),
      m_enableTextFolding(p_enableTextFolding) {
  setAttribute(Qt::WA_StaticContents);
  setAttribute(Qt::WA_OpaquePaintEvent);

  setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Minimum);

  setMouseTracking(true);

  setFont(font());

  m_foldingHighlightTimer.setSingleShot(true);
  m_foldingHighlightTimer.setInterval(300);
  connect(&m_foldingHighlightTimer, &QTimer::timeout, this, &IndicatorsBorder::highlightFolding);
}

IndicatorsBorder::LineNumberType IndicatorsBorder::getLineNumberType() const {
  return m_lineNumberType;
}

void IndicatorsBorder::setLineNumberType(IndicatorsBorder::LineNumberType p_type) {
  if (p_type == m_lineNumberType) {
    return;
  }

  m_lineNumberType = p_type;
}

QSize IndicatorsBorder::sizeHint() const { return QSize(borderWidth(), 0); }

const QFont &IndicatorsBorder::getFont() const { return m_font; }

void IndicatorsBorder::setFont(const QFont &p_font) {
  if (m_font == p_font && m_maxCharWidth > 0) {
    return;
  }

  m_font = p_font;

  const QFontMetricsF fm(m_font);
  m_maxCharWidth = 0.0;
  // Loop to determine the widest numeric character in the current font.
  // 48 is ascii '0'.
  for (int i = 48; i < 58; i++) {
    const qreal charWidth = ceil(fm.horizontalAdvance(QChar(i)));
    m_maxCharWidth = qMax(m_maxCharWidth, charWidth);
  }

  m_foldingMarkerWith = fm.height();

  m_needUpdateIndicatorsPosition = true;

  // Force to update line number area width.
  m_lineNumberCount = -1;
  requestUpdate();
}

void IndicatorsBorder::setForegroundColor(const QColor &p_color) { m_foregroundColor = p_color; }

void IndicatorsBorder::setBackgroundColor(const QColor &p_color) { m_backgroundColor = p_color; }

void IndicatorsBorder::setCurrentLineNumberForegroundColor(const QColor &p_color) {
  m_currentLineNumberForegroundColor = p_color;
}

void IndicatorsBorder::setFoldingColor(const QColor &p_color) { m_foldingColor = p_color; }

void IndicatorsBorder::setFoldedFoldingColor(const QColor &p_color) {
  m_foldedFoldingColor = p_color;
}

void IndicatorsBorder::setFoldingHighlightColor(const QColor &p_color) {
  m_foldingHighlightColor = p_color;
}

void IndicatorsBorder::requestUpdate() { QTimer::singleShot(0, this, SLOT(update())); }

int IndicatorsBorder::lineNumberWidth() {
  if (m_lineNumberType == LineNumberType::None) {
    m_lineNumberCount = 0;
    m_lineNumberWidth = 0;
    return 0;
  }

  int blockCount = m_interface->blockCount();
  if (blockCount == m_lineNumberCount) {
    return m_lineNumberWidth;
  }

  m_lineNumberCount = blockCount;
  const int digits = (int)ceil(log10((double)(m_lineNumberCount + 1)));
  m_lineNumberWidth = (int)ceil((digits + 1) * m_maxCharWidth);
  return m_lineNumberWidth;
}

void IndicatorsBorder::paintEvent(QPaintEvent *p_event) { paintBorder(p_event->rect()); }

void IndicatorsBorder::paintBorder(const QRect &p_rect) {
  // Line number width.
  const int oldLineNumberWidth = m_lineNumberWidth;
  const int newLineNumberWidth = lineNumberWidth();
  if (m_needUpdateIndicatorsPosition || (newLineNumberWidth != oldLineNumberWidth)) {
    m_needUpdateIndicatorsPosition = true;
    m_indicatorsPosition.clear();
  }

  QPainter painter(this);
  painter.setRenderHints(QPainter::TextAntialiasing);
  painter.setFont(m_font);

  // Paint background.
  painter.fillRect(p_rect, m_backgroundColor);

  const int top = m_interface->contentOffsetAtTop();
  const int bottom = p_rect.height() + top;
  const int currentBlockNumber = m_interface->cursorBlockNumber();
  auto block = m_interface->firstVisibleBlock();
  auto rect = m_interface->blockBoundingRect(block);
  // When drawing block i, this indicates how many invisible blocks from block i
  // to cursor block.
  int numOfInvisibleBlocksToCursorBlock = -1;
  int previousVisibleBlockNumber = block.blockNumber() - 1;

  // Paint the borader in chunks line by line.
  while (block.isValid() && rect.y() <= bottom) {
    Q_ASSERT(block.isVisible());

    // Painting coordinates.
    int x = 0;
    const int y = rect.y() - top;
    const int h = rect.height();
    const int blockNumber = block.blockNumber();

    Q_ASSERT(h != 0);

    // Add a left margin.
    x += c_separatorWidth;
    if (m_needUpdateIndicatorsPosition) {
      m_indicatorsPosition.append(IndicatorRightX(x, Indicators::None));
    }

    // Line number.
    if (m_lineNumberType != LineNumberType::None) {
      painter.save();
      QColor fg;
      const int distanceToCurrent = blockNumber - currentBlockNumber;
      if (distanceToCurrent == 0) {
        fg = m_currentLineNumberForegroundColor;
      } else {
        fg = m_foregroundColor;
      }

      painter.setPen(fg);
      painter.setBrush(fg);

      int number = blockNumber + 1;
      if (m_lineNumberType == LineNumberType::Relative) {
        // Relative number.
        if (distanceToCurrent == 0) {
          // Reset since we need to change the direction.
          numOfInvisibleBlocksToCursorBlock = -1;
        } else {
          if (m_interface->hasInvisibleBlocks()) {
            // Take care of invisible blocks.
            if (numOfInvisibleBlocksToCursorBlock == -1) {
              numOfInvisibleBlocksToCursorBlock = 0;
              // Go through all the blocks.
              if (distanceToCurrent > 0) {
                auto tb = m_interface->cursorBlock().next();
                while (tb.blockNumber() < blockNumber) {
                  if (!tb.isVisible()) {
                    ++numOfInvisibleBlocksToCursorBlock;
                  }
                  tb = tb.next();
                }
              } else {
                auto tb = block.next();
                while (tb.blockNumber() < currentBlockNumber) {
                  if (!tb.isVisible()) {
                    ++numOfInvisibleBlocksToCursorBlock;
                  }
                  tb = tb.next();
                }
              }
            } else if (blockNumber - 1 > previousVisibleBlockNumber) {
              if (distanceToCurrent > 0) {
                numOfInvisibleBlocksToCursorBlock += (blockNumber - previousVisibleBlockNumber - 1);
              } else {
                Q_ASSERT(numOfInvisibleBlocksToCursorBlock > 0);
                numOfInvisibleBlocksToCursorBlock -= (blockNumber - previousVisibleBlockNumber - 1);
              }
            }

            Q_ASSERT(numOfInvisibleBlocksToCursorBlock >= 0);
            number = abs(distanceToCurrent) - numOfInvisibleBlocksToCursorBlock;
          } else {
            number = abs(distanceToCurrent);
          }
        }
      }

      painter.drawText(x + m_maxCharWidth / 2, y, newLineNumberWidth - m_maxCharWidth, h,
                       Qt::TextDontClip | Qt::AlignRight | Qt::AlignTop, QString::number(number));

      x += newLineNumberWidth + c_separatorWidth;
      if (m_needUpdateIndicatorsPosition) {
        m_indicatorsPosition.append(IndicatorRightX(x, Indicators::LineNumber));
      }

      painter.restore();
    }

    // Folding markers.
    if (m_enableTextFolding) {
      if (m_currentFoldingRange && m_currentFoldingRange->second.contains(blockNumber)) {
        painter.fillRect(x, y, m_foldingMarkerWith, h, m_foldingHighlightColor);
      }

      auto foldingRanges = m_interface->textFolding().foldingRangesStartingOnBlock(blockNumber);
      bool hasFoldedFolding = false;
      for (const auto &range : foldingRanges) {
        if (range.second & TextFolding::FoldingRangeFlag::Folded) {
          hasFoldedFolding = true;
        }
      }

      if (!foldingRanges.isEmpty() || TextBlockData::get(block)->isMarkedAsFoldingStart()) {
        paintFoldingMarker(painter, x, y, m_foldingMarkerWith, h, hasFoldedFolding);
      }

      x += m_foldingMarkerWith;
      if (m_needUpdateIndicatorsPosition) {
        m_indicatorsPosition.append(IndicatorRightX(x, Indicators::FoldingMarker));
      }
    }

    if (m_needUpdateIndicatorsPosition) {
      m_needUpdateIndicatorsPosition = false;

      // Add an extra separator.
      x += c_separatorWidth;
      m_indicatorsPosition.append(IndicatorRightX(x, Indicators::None));

      // Now we know our needed space, update and paint again.
      updateGeometry();
      update();
      return;
    }

    previousVisibleBlockNumber = blockNumber;

    do {
      block = block.next();
    } while (block.isValid() && !block.isVisible());
    rect = m_interface->blockBoundingRect(block);
  }
}

void IndicatorsBorder::updateBorder() { update(); }

void IndicatorsBorder::resizeEvent(QResizeEvent *p_event) {
  QWidget::resizeEvent(p_event);

  update();
}

void IndicatorsBorder::paintFoldingMarker(QPainter &p_painter, int p_xOffset, int p_yOffset,
                                          int p_width, int p_height, bool p_folded) const {
  p_painter.save();

  QColor color = p_folded ? m_foldedFoldingColor : m_foldingColor;
  p_painter.setPen(color);
  p_painter.setBrush(color);

  auto font = p_painter.font();
  font.setBold(true);
  qreal ps = font.pointSizeF();
  if (ps > 0) {
    font.setPointSizeF(ps * 1.5);
  }
  p_painter.setFont(font);

  const QString markerText(p_folded ? "+" : "-");

  p_painter.drawText(p_xOffset, p_yOffset, p_width, p_height,
                     Qt::TextDontClip | Qt::AlignCenter | Qt::AlignVCenter, markerText);

  p_painter.restore();
}

void IndicatorsBorder::mousePressEvent(QMouseEvent *p_event) {
  auto block = yToBlock(p_event->y());
  if (block.isValid()) {
    m_lastClickedBlockNumber = block.blockNumber();
    auto indicator = positionToIndicator(p_event->pos());
    if (indicator == Indicators::LineNumber && p_event->button() == Qt::LeftButton &&
        !(p_event->modifiers() & Qt::ShiftModifier)) {
      // TODO: Let view begin to select lines.
    }

    QMouseEvent forwardEvent(QEvent::MouseButtonPress, QPoint(0, p_event->y()), p_event->button(),
                             p_event->buttons(), p_event->modifiers());
    m_interface->forwardMouseEvent(&forwardEvent);
    p_event->accept();
  } else {
    QWidget::mousePressEvent(p_event);
  }

  m_interface->setFocus();
}

IndicatorsBorder::Indicators IndicatorsBorder::positionToIndicator(const QPoint &p_pos) const {
  for (const auto &indic : m_indicatorsPosition) {
    if (p_pos.x() <= indic.first) {
      return indic.second;
    }
  }

  return Indicators::None;
}

void IndicatorsBorder::wheelEvent(QWheelEvent *p_event) {
  auto globalPos = p_event->globalPosition();
  globalPos.rx() += borderWidth();
  QWheelEvent forwardEvent(p_event->position(), globalPos, p_event->pixelDelta(),
                           p_event->angleDelta(), p_event->buttons(), p_event->modifiers(),
                           p_event->phase(), p_event->inverted(), p_event->source());
  m_interface->forwardWheelEvent(&forwardEvent);
  p_event->accept();
}

int IndicatorsBorder::borderWidth() const {
  int w = 1;
  if (!m_indicatorsPosition.isEmpty()) {
    w = m_indicatorsPosition.last().first;
  }

  return w;
}

void IndicatorsBorder::mouseReleaseEvent(QMouseEvent *p_event) {
  auto button = p_event->button();
  auto block = yToBlock(p_event->y());
  if (block.blockNumber() == m_lastClickedBlockNumber) {
    auto indicator = positionToIndicator(p_event->pos());
    if (indicator == Indicators::FoldingMarker) {
      if (button == Qt::LeftButton) {
        // Toggle folding range.
        if (m_currentFoldingRange) {
          auto id = m_currentFoldingRange->first;
          if (id == TextFolding::InvalidRangeId) {
            // A syntax folding.
            // New a non-persistent folding range.
            id = m_interface->textFolding().newFoldingRange(m_currentFoldingRange->second,
                                                            TextFolding::Folded);
            if (id == TextFolding::InvalidRangeId) {
              qWarning() << "failed to create a folding range based on syntax"
                         << m_currentFoldingRange->second.toString();
            }
          } else {
            m_interface->textFolding().toggleRange(id);
          }

          // Update current folding range highlight.
          m_currentFoldingHighlightBlockNumber = -1;
          kickOffFoldingHighlight(m_lastClickedBlockNumber);
        }
      }
    }
  }

  QMouseEvent forwardEvent(QEvent::MouseButtonRelease, QPoint(0, p_event->y()), button,
                           p_event->buttons(), p_event->modifiers());
  m_interface->forwardMouseEvent(&forwardEvent);
}

void IndicatorsBorder::mouseMoveEvent(QMouseEvent *p_event) {
  auto block = yToBlock(p_event->y());
  if (!block.isValid()) {
    clearFoldingHighlight();
  } else {
    auto indicator = positionToIndicator(p_event->pos());
    if (indicator == Indicators::FoldingMarker) {
      kickOffFoldingHighlight(block.blockNumber());
    } else {
      clearFoldingHighlight();
    }
  }

  QWidget::mouseMoveEvent(p_event);
}

QTextBlock IndicatorsBorder::yToBlock(int p_y) const {
  int contentYOffset = p_y + m_interface->contentOffsetAtTop();
  return m_interface->findBlockByYPosition(contentYOffset);
}

void IndicatorsBorder::clearFoldingHighlight() {
  m_foldingHighlightTimer.stop();

  m_currentFoldingHighlightBlockNumber = -1;
  m_currentFoldingRange.reset();

  requestUpdate();
}

void IndicatorsBorder::kickOffFoldingHighlight(int p_blockNumber) {
  if (p_blockNumber == m_currentFoldingHighlightBlockNumber ||
      p_blockNumber >= m_interface->blockCount()) {
    return;
  }

  m_currentFoldingHighlightBlockNumber = p_blockNumber;

  if (m_currentFoldingRange) {
    highlightFolding();
  } else if (!m_foldingHighlightTimer.isActive()) {
    m_foldingHighlightTimer.start();
  }
}

void IndicatorsBorder::highlightFolding() {
  const auto &tfolding = m_interface->textFolding();
  auto newFoldingRange = tfolding.leafFoldingRangeOnBlock(m_currentFoldingHighlightBlockNumber);

  if (!newFoldingRange) {
    // Check syntax highlight folding.
    // Search backwards for the first folding range containing current block.
    const int searchEndBlockNum = qMax(0, m_currentFoldingHighlightBlockNumber -
                                              c_maxNumberOfLinesToSearchBackwardsForSyntaxFolding);
    for (int blockNum = m_currentFoldingHighlightBlockNumber; blockNum >= searchEndBlockNum;
         --blockNum) {
      auto range = m_interface->fetchSyntaxFoldingRangeStartingOnBlock(blockNum);
      if (!range || !range->isValid()) {
        continue;
      }

      if (range->contains(m_currentFoldingHighlightBlockNumber)) {
        newFoldingRange = QSharedPointer<QPair<qint64, TextBlockRange>>::create(
            TextFolding::InvalidRangeId, *range);
        break;
      }
    }
  }

  if (newFoldingRange == m_currentFoldingRange) {
    return;
  } else {
    m_currentFoldingRange = newFoldingRange;
  }

  requestUpdate();
}

void IndicatorsBorder::leaveEvent(QEvent *p_event) {
  clearFoldingHighlight();

  QWidget::leaveEvent(p_event);
}

void IndicatorsBorder::setTextFoldingEnabled(bool p_enable) {
  m_enableTextFolding = p_enable;
  m_needUpdateIndicatorsPosition = true;
}
