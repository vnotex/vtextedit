#ifndef TEXTDOCUMENTLAYOUTDATA_H
#define TEXTDOCUMENTLAYOUTDATA_H

#include <QColor>
#include <QPointF>
#include <QRectF>
#include <QSharedPointer>
#include <QString>

#include <vtextedit/textblockdata.h>

namespace vte {
// Denote the start and end position of a marker line.
struct Marker {
  QPointF m_start;
  QPointF m_end;
};

// How to draw a preview image.
struct ImagePaintData {
  // The rect to draw the image.
  QRectF m_rect;

  // Name of the image.
  QString m_name;

  // Forced background if valid.
  QColor m_backgroundColor;

  bool isValid() const { return !m_name.isEmpty(); }

  bool hasForcedBackground() const { return m_backgroundColor.isValid(); }
};

// Data about a block layout.
struct BlockLayoutData {
  void reset() {
    m_offset = -1;
    m_rect = QRectF();
    m_markers.clear();
    m_images.clear();
  }

  bool isNull() const { return m_rect.isNull(); }

  bool hasOffset() const { return m_offset > -1 && !m_rect.isNull(); }

  qreal top() const {
    Q_ASSERT(hasOffset());
    return m_offset;
  }

  qreal bottom() const {
    Q_ASSERT(hasOffset());
    return m_offset + m_rect.height();
  }

  static QSharedPointer<BlockLayoutData> get(const QTextBlock &p_block) {
    auto blockData = TextBlockData::get(p_block);
    auto data = blockData->getBlockLayoutData();
    if (!data) {
      data.reset(new BlockLayoutData());
      blockData->setBlockLayoutData(data);
    }
    return data;
  }

  // Y offset of this block.
  // -1 for invalid.
  qreal m_offset = -1;

  // The bounding rect of this block, including the margins.
  // Null for invalid.
  QRectF m_rect;

  // Markers to draw for this block.
  // Y is the offset within this block.
  QVector<Marker> m_markers;

  // Images to draw for this block.
  // Y is the offset within this block.
  QVector<ImagePaintData> m_images;
};

} // namespace vte
#endif // TEXTDOCUMENTLAYOUTDATA_H
