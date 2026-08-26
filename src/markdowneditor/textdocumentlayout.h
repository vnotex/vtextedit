#ifndef VTEXTDOCUMENTLAYOUT_H
#define VTEXTDOCUMENTLAYOUT_H

#include <QAbstractTextDocumentLayout>
#include <QHash>
#include <QMap>
#include <QPair>
#include <QSize>
#include <QVector>

#include <vtextedit/orderedintset.h>
#include <vtextedit/preview.h>
#include <vtextedit/previewdata.h>

#include "textdocumentlayoutdata.h"

namespace vte {
class DocumentResourceMgr;
struct PreviewImageData;
class PreviewData;

// The painted preview sources map one to one onto the interactive element
// types; PreviewElementType::Table has no painted counterpart, and therefore
// neither claims nor suppresses one.
//
// One table, looked up in both directions, so the two consumers - the layout's
// claim filter and the folding provider's painted-preview probe - cannot drift
// apart. The static assertions below break the build when either enum grows,
// which is the point at which this table has to be revisited.
struct PreviewSourceMapping {
  PreviewData::Source m_source;

  PreviewElementType m_type;
};

inline const QVector<PreviewSourceMapping> &previewSourceMappings() {
  static_assert(PreviewData::MaxSource == 3,
                "a painted preview source was added - extend previewSourceMappings()");
  static_assert(c_previewElementTypeCount == 4,
                "a preview element type was added - extend previewSourceMappings()");

  static const QVector<PreviewSourceMapping> mappings{
      {PreviewData::ImageLink, PreviewElementType::Image},
      {PreviewData::CodeBlock, PreviewElementType::Code},
      {PreviewData::MathBlock, PreviewElementType::Math}};
  return mappings;
}

// The element type @p_source is painted for, if any.
inline bool previewSourceToElementType(PreviewData::Source p_source, PreviewElementType *p_type) {
  for (const auto &mapping : previewSourceMappings()) {
    if (mapping.m_source == p_source) {
      *p_type = mapping.m_type;
      return true;
    }
  }

  return false;
}

// The painted source which stands in for @p_type, if any.
inline bool previewElementTypeToSource(PreviewElementType p_type, PreviewData::Source *p_source) {
  for (const auto &mapping : previewSourceMappings()) {
    if (mapping.m_type == p_type) {
      *p_source = mapping.m_source;
      return true;
    }
  }

  return false;
}

class TextDocumentLayout : public QAbstractTextDocumentLayout {
  Q_OBJECT
public:
  // One interactive preview widget to reserve space for.
  struct WidgetPreviewSpec {
    bool operator==(const WidgetPreviewSpec &p_other) const {
      return m_id == p_other.m_id && m_startPos == p_other.m_startPos &&
             m_endPos == p_other.m_endPos && m_placement == p_other.m_placement &&
             m_typeOrder == p_other.m_typeOrder &&
             qFuzzyCompare(m_width + 1, p_other.m_width + 1) &&
             qFuzzyCompare(m_height + 1, p_other.m_height + 1);
    }

    bool operator!=(const WidgetPreviewSpec &p_other) const { return !(*this == p_other); }

    // Stable identity assigned by InteractivePreviewHost.
    quint64 m_id = 0;

    // Half-open document range of the source.
    int m_startPos = 0;
    int m_endPos = 0;

    PreviewPlacement m_placement = PreviewPlacement::BlockAfterSource;

    // Secondary stacking key, derived from the element type.
    int m_typeOrder = 0;

    // Preferred size in logical pixels.
    qreal m_width = 0;
    qreal m_height = 0;
  };

  // One element whose static painted preview is suppressed because an
  // interactive widget claimed it.
  struct PreviewClaim {
    bool operator==(const PreviewClaim &p_other) const {
      return m_startPos == p_other.m_startPos && m_endPos == p_other.m_endPos &&
             m_type == p_other.m_type;
    }

    bool operator!=(const PreviewClaim &p_other) const { return !(*this == p_other); }

    bool operator<(const PreviewClaim &p_other) const {
      if (m_startPos != p_other.m_startPos) {
        return m_startPos < p_other.m_startPos;
      }
      if (m_endPos != p_other.m_endPos) {
        return m_endPos < p_other.m_endPos;
      }
      return static_cast<int>(m_type) < static_cast<int>(p_other.m_type);
    }

    // Half-open document range of the claimed source.
    int m_startPos = 0;
    int m_endPos = 0;

    PreviewElementType m_type = PreviewElementType::Image;
  };

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

  // Submit the complete set of interactive preview widgets to reserve space
  // for. Layout data only ever stores identities and rectangles, never widget
  // pointers.
  void setWidgetPreviews(const QVector<WidgetPreviewSpec> &p_specs);

  const QVector<WidgetPreviewSpec> &widgetPreviews() const;

  // The complete set of elements claimed by interactive widgets. Only a claim
  // whose type matches a painted preview's own source suppresses it, so a
  // table widget never hides an image nested inside its cells. Must be sorted.
  void setPreviewClaims(const QVector<PreviewClaim> &p_claims);

  // Final document rect of a laid out widget preview. Returns a null rect when
  // the widget is not currently laid out (folded away, removed, ...).
  QRectF widgetPreviewRect(quint64 p_id) const;

  // The width a preview may occupy at most, or 0 when unconstrained.
  qreal availableContentWidth() const;

  // Union of the visible text layout rects of [p_startPos, p_endPos) in
  // document coordinates.
  QRectF sourceTextRect(int p_startPos, int p_endPos) const;

  // The exact width an InlineAboveLine band gets for [p_startPos, p_endPos),
  // using the same visual-line selection rule as the layout itself. Returns 0
  // when no line claims the range.
  qreal inlinePlacementWidth(int p_startPos, int p_endPos) const;

  // Whether a layout pass is currently running.
  //
  // Qt does not support mutating a QTextDocument from inside
  // QAbstractTextDocumentLayout::documentChanged() or from anything reached
  // through it: QTextDocumentPrivate merges the nested edit into the still
  // pending change triple, so the nested documentChanged() receives a range
  // which no longer describes the edit, blocks fall off the offset chain and
  // updateDocumentSize() aborts. Every function which can reach widget visible
  // code holds a PassGuard, so a widget can ask this before applying an edit.
  bool isBusy() const { return m_passDepth > 0; }

  // Ask for one becameIdle() the next time the outermost pass unwinds. Off by
  // default so the signal stays away from the per-paint hot path: it only
  // fires when something was actually deferred during the pass.
  void requestIdleNotification() { m_idleNotificationOwed = true; }

signals:
  // Emitted whenever the geometry assigned to any interactive preview widget
  // changed, even when the overall document size did not.
  void widgetPreviewGeometryChanged();

  // The outermost layout pass has finished and a notification was owed. The
  // handler must only set flags and arm timers: it runs during unwinding.
  void becameIdle();

protected:
  void documentChanged(int p_from, int p_charsRemoved, int p_charsAdded) Q_DECL_OVERRIDE;

private:
  // Marks the span of one layout pass. Only the outermost destructor takes
  // the depth back to 0, so a nested guard can never report idle while an
  // outer pass is still running.
  class PassGuard {
  public:
    explicit PassGuard(TextDocumentLayout *p_layout) : m_layout(p_layout) {
      ++m_layout->m_passDepth;
    }

    ~PassGuard();

  private:
    TextDocumentLayout *m_layout = nullptr;
  };

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
  // @p_widgetMarkers: markers of the inline widget preview bands. Kept apart
  // from @p_markers because finishBlockLayout() asserts that @p_markers is
  // empty when the block carries a block-level painted image, while a widget
  // claim may coexist with such an image.
  qreal layoutLines(const QTextBlock &p_block, QTextLayout *p_tl, QVector<Marker> &p_markers,
                    QVector<ImagePaintData> &p_images, QVector<WidgetPaintData> &p_widgets,
                    QVector<Marker> &p_widgetMarkers, qreal p_availableWidth, qreal p_height);

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
  // NOTICE: the offsets of the blocks behind @p_block are NOT cleared, so they
  // keep describing the pre-clear geometry until an updateOffset() walk
  // reaches them. The caller must layout @p_block and update the offsets.
  void clearBlockLayout(QTextBlock &p_block);

  // Update rect of a block.
  // @p_widgetMarkers: markers of the inline widget preview bands, merged into
  // the block markers unconditionally (see layoutLines()).
  void finishBlockLayout(const QTextBlock &p_block, const QVector<Marker> &p_markers,
                         const QVector<ImagePaintData> &p_images,
                         const QVector<WidgetPaintData> &p_widgets,
                         const QVector<Marker> &p_widgetMarkers);

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
  QRectF blockRectFromTextLayout(const QTextBlock &p_block, ImagePaintData *p_image = NULL,
                                 QVector<WidgetPaintData> *p_widgets = NULL);

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

  // Whether @p_line owns the block-local range [p_start, p_end) under the
  // inline placement rule, and if so its x span. A range crossing the line
  // boundary stays on the side owning at least half of it.
  // @p_positioned: false while the block is still being laid out, where
  // QTextLine::x() is still 0 and the document margin must be added by hand.
  // Shared by layoutLines() and inlinePlacementWidth() so the width a widget
  // is measured at always matches the width it is assigned.
  bool lineClaimsRange(const QTextLine &p_line, int p_start, int p_end, bool p_positioned,
                       qreal *p_startX, qreal *p_endX) const;

  // Rebuild the block-number to widget-spec index map when it went stale.
  void ensureWidgetPreviewMap();

  // Widget specs anchored to @p_block with the given placement, in stacking
  // order.
  QVector<const WidgetPreviewSpec *> widgetSpecsForBlock(const QTextBlock &p_block,
                                                         PreviewPlacement p_placement);

  // The block's preview data with claimed entries removed.
  QVector<PreviewData *> unclaimedPreviewData(const QTextBlock &p_block) const;

  // Whether [p_start, p_end) is covered by a claim of the same element type.
  bool isPreviewClaimed(int p_start, int p_end, PreviewElementType p_type) const;

  // Recompute the document rects of every laid out widget preview and notify
  // when anything changed.
  void updateWidgetPreviewGeometry();

  // Blocks whose painted preview or reservation can change between two sorted
  // claim/spec sets.
  void collectClaimDeltaBlocks(const QVector<PreviewClaim> &p_old,
                               const QVector<PreviewClaim> &p_new, OrderedIntSet &p_blocks);

  void collectBlocksForClaim(const PreviewClaim &p_claim, OrderedIntSet &p_blocks);

  void collectSpecDeltaBlocks(const QVector<WidgetPreviewSpec> &p_old,
                              const QVector<WidgetPreviewSpec> &p_new, OrderedIntSet &p_blocks);

  void collectBlocksForSpec(const WidgetPreviewSpec &p_spec, OrderedIntSet &p_blocks);

  // The block which carries @p_spec's reservation. Returns an invalid block
  // when the spec's positions no longer resolve.
  //
  // ensureWidgetPreviewMap() uses this to decide which block reserves the
  // band, and collectBlocksForSpec() to decide which blocks a spec change
  // relayouts. If the two ever disagreed, the reserving block would never be
  // relayouted and the published rectangle would stay stale, leaving the
  // widget over unrelated text - so the rule lives here once.
  QTextBlock blockForSpec(const WidgetPreviewSpec &p_spec) const;

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

  // Width used only to paint the cursor.
  int m_cursorWidth = 1;

  // Right margin for cursor.
  qreal m_cursorMargin = 4;

  DocumentResourceMgr *m_resourceMgr = nullptr;

  // Whether allow preview of block.
  bool m_previewEnabled = false;

  // Whether constrain the width of preview to the width of the page.
  bool m_constrainPreviewWidthEnabled = false;

  QColor m_previewMarkerForeground = {"#9575CD"};

  // Interactive preview widgets to reserve space for.
  QVector<WidgetPreviewSpec> m_widgetPreviews;

  // Block number to indices into m_widgetPreviews, in stacking order.
  QHash<int, QVector<int>> m_widgetPreviewsByBlock;

  // Document revision the block map was built for.
  int m_widgetPreviewMapRevision = -1;

  bool m_widgetPreviewMapDirty = true;

  // Final document rects of the laid out widget previews.
  QHash<quint64, QRectF> m_widgetPreviewGeometry;

  // Half-open document ranges whose static preview is suppressed.
  QVector<PreviewClaim> m_claimedPreviews;

  // Nesting depth of the running layout passes. See isBusy().
  int m_passDepth = 0;

  // Whether becameIdle() is owed when the outermost pass unwinds.
  bool m_idleNotificationOwed = false;

  static const int c_markerThickness;

  static const int c_maxInlineImageHeight;

  // Padding of image preview for top and bottom.
  static const int c_imagePadding;

  // Fixed geometry reserved for the cursor at the end of a block.
  static const int c_cursorGeometryWidth;

  // Vertical padding around an interactive preview widget.
  static const int c_widgetPreviewPadding;
};

} // namespace vte
#endif // VTEXTDOCUMENTLAYOUT_H
