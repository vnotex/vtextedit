#ifndef VTEXTDOCUMENTLAYOUT_H
#define VTEXTDOCUMENTLAYOUT_H

#include <QAbstractTextDocumentLayout>
#include <QMap>
#include <QSize>
#include <QVector>

#include <vtextedit/orderedintset.h>

#include "textdocumentlayoutdata.h"

namespace vte {
class DocumentResourceMgr;
struct PreviewImageData;
class PreviewData;

class TextDocumentLayout : public QAbstractTextDocumentLayout {
  Q_OBJECT
public:
  TextDocumentLayout(QTextDocument *p_doc, DocumentResourceMgr *p_resourceMgr);

  void draw(QPainter *p_painter, const PaintContext &p_context) Q_DECL_OVERRIDE;

  int hitTest(const QPointF &p_point, Qt::HitTestAccuracy p_accuracy) const Q_DECL_OVERRIDE;

  int pageCount() const Q_DECL_OVERRIDE;

  QSizeF documentSize() const Q_DECL_OVERRIDE;

  QRectF frameBoundingRect(QTextFrame *p_frame) const Q_DECL_OVERRIDE;

  QRectF blockBoundingRect(const QTextBlock &p_block) const Q_DECL_OVERRIDE;

  void setCursorWidth(int p_width);

  int cursorWidth() const;

  qreal getLeadingSpaceOfLine() const;
  void setLeadingSpaceOfLine(qreal p_leading);

  // Return the block number which contains point @p_point.
  // If @p_point is at the border, returns the block below.
  int findBlockByPosition(const QPointF &p_point) const;

  void setConstrainPreviewWidthEnabled(bool p_enabled);

  void setPreviewEnabled(bool p_enabled);

  // Relayout all the blocks.
  void relayout();

  // Relayout @p_blocks.
  void relayout(const OrderedIntSet &p_blocks);

  void setPreviewMarkerForeground(const QColor &p_color);

  // Request update block by block number.
  void updateBlockByNumber(int p_blockNumber);

protected:
  void documentChanged(int p_from, int p_charsRemoved, int p_charsAdded) Q_DECL_OVERRIDE;

private:
  // Layout one block.
  // Only update the rect of the block. Offset is not updated yet.
  void layoutBlock(const QTextBlock &p_block);

  // Update the offset of @p_block.
  // @p_block has a valid layout.
  // After the call, all block before @p_block will have the correct layout and
  // offset.
  void updateOffsetBefore(const QTextBlock &p_block);

  // Update the offset of blocks after @p_block if they are layouted.
  void updateOffsetAfter(const QTextBlock &p_block);

  void updateOffset(const QTextBlock &p_block);

  void layoutBlockAndUpdateOffset(const QTextBlock &p_block);

  // Returns the total height of this block after layouting lines and inline
  // images.
  qreal layoutLines(const QTextBlock &p_block, QTextLayout *p_tl, QVector<Marker> &p_markers,
                    QVector<ImagePaintData> &p_images, qreal p_availableWidth, qreal p_height);

  // Layout inline image in a line.
  // @p_data: if NULL, means just layout a marker.
  // Returns the image height.
  void layoutInlineImage(const PreviewImageData *p_data, qreal p_heightInBlock,
                         qreal p_imageSpaceHeight, qreal p_xStart, qreal p_xEnd,
                         QVector<Marker> &p_markers, QVector<ImagePaintData> &p_images);

  // Get inline images belonging to @p_line from @p_data.
  // @p_index: image [0, p_index) has been drawn.
  // @p_images: contains all images and markers (NULL element indicates it
  // is just a placeholder for the marker.
  // Returns the maximum height of the images.
  qreal fetchInlineImagesForOneLine(const QVector<PreviewData *> &p_data, const QTextLine *p_line,
                                    qreal p_margin, int &p_index,
                                    QVector<const PreviewImageData *> &p_images,
                                    QVector<QPair<qreal, qreal>> &p_imageRange);

  // Clear the layout of @p_block.
  // Also clear all the offset behind this block.
  void clearBlockLayout(QTextBlock &p_block);

  // Update rect of a block.
  void finishBlockLayout(const QTextBlock &p_block, const QVector<Marker> &p_markers,
                         const QVector<ImagePaintData> &p_images);

  void updateDocumentSize();

  QVector<QTextLayout::FormatRange>
  formatRangeFromSelection(const QTextBlock &p_block, const QVector<Selection> &p_selections) const;

  // Get the block range [first, last] by rect @p_rect.
  // @p_rect: a clip region in document coordinates. If null, returns all the
  // blocks. Return [-1, -1] if no valid block range found.
  void blockRangeFromRect(const QRectF &p_rect, int &p_first, int &p_last) const;

  // Binary search to get the block range [first, last] by @p_rect.
  void blockRangeFromRectBS(const QRectF &p_rect, int &p_first, int &p_last) const;

  // Return a rect from the layout.
  // If @p_imageRect is not NULL and there is block image for this block, it
  // will be set to the rect of that image. Return a null rect if @p_block has
  // not been layouted.
  QRectF blockRectFromTextLayout(const QTextBlock &p_block, ImagePaintData *p_image = NULL);

  // Update document size when only @p_block is changed and the height
  // remains the same.
  void updateDocumentSizeWithOneBlockChanged(const QTextBlock &p_block);

  void adjustImagePaddingAndSize(const PreviewImageData *p_data, int p_maximumWidth, int &p_padding,
                                 QSize &p_size) const;

  // Draw preview of block @p_block.
  // @p_offset: the offset for the drawing of the block.
  void drawPreview(QPainter *p_painter, const QTextBlock &p_block, const QPointF &p_offset);

  void drawPreviewMarker(QPainter *p_painter, const QTextBlock &p_block, const QPointF &p_offset);

  void scaleSize(QSize &p_size, int p_width, int p_height);

  // Get text length in pixel.
  // @p_pos: position within the layout.
  int getTextWidthWithinTextLine(const QTextLayout *p_layout, int p_pos, int p_length);

  bool shouldBlockWrapLine(const QTextBlock &p_block) const;

  // Document margin on left/right/bottom.
  qreal m_margin = 0;

  // Maximum width of the contents.
  qreal m_width = 0;

  // The block number of the block which contains the m_width.
  int m_maximumWidthBlockNumber = -1;

  // Height of all the blocks of document.
  qreal m_height = 0;

  // Set the leading space of a line.
  qreal m_leadingSpaceOfLine = 0;

  // Block count of the document.
  int m_blockCount = 0;

  // Width of the cursor.
  int m_cursorWidth = 1;

  // Right margin for cursor.
  qreal m_cursorMargin = 4;

  DocumentResourceMgr *m_resourceMgr = nullptr;

  // Whether allow preview of block.
  bool m_previewEnabled = false;

  // Whether constrain the width of preview to the width of the page.
  bool m_constrainPreviewWidthEnabled = false;

  QColor m_previewMarkerForeground = {"#9575CD"};

  static const int c_markerThickness;

  static const int c_maxInlineImageHeight;

  // Padding of image preview for top and bottom.
  static const int c_imagePadding;
};

} // namespace vte
#endif // VTEXTDOCUMENTLAYOUT_H
