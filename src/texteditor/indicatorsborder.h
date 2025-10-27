#ifndef INDICATORSBORDER_H
#define INDICATORSBORDER_H

#include <QColor>
#include <QFont>
#include <QPair>
#include <QSharedPointer>
#include <QTextBlock>
#include <QTimer>
#include <QVector>
#include <QWidget>

namespace vte {
class TextFolding;
class TextBlockRange;

class IndicatorsBorderInterface {
public:
  virtual ~IndicatorsBorderInterface() {}

  // Return the block count.
  virtual int blockCount() const = 0;

  virtual QTextBlock firstVisibleBlock() const = 0;

  virtual int contentOffsetAtTop() const = 0;

  virtual QSize viewportSize() const = 0;

  virtual QRectF blockBoundingRect(const QTextBlock &p_block) const = 0;

  virtual int cursorBlockNumber() const = 0;

  virtual QTextBlock cursorBlock() const = 0;

  virtual bool hasInvisibleBlocks() const = 0;

  virtual TextFolding &textFolding() const = 0;

  virtual QTextBlock findBlockByYPosition(int p_y) const = 0;

  virtual QWidget *viewWidget() const = 0;

  virtual void forwardWheelEvent(QWheelEvent *p_event) = 0;

  virtual void forwardMouseEvent(QMouseEvent *p_event) = 0;

  virtual QSharedPointer<TextBlockRange>
  fetchSyntaxFoldingRangeStartingOnBlock(int p_blockNumber) = 0;

  virtual void setFocus() = 0;
};

class IndicatorsBorder : public QWidget {
  Q_OBJECT
public:
  enum class LineNumberType { None, Absolute, Relative };

  IndicatorsBorder(IndicatorsBorderInterface *p_interface,
                   IndicatorsBorder::LineNumberType p_lineNumberType, bool p_enableTextFolding,
                   QWidget *p_parent = nullptr);

  IndicatorsBorder::LineNumberType getLineNumberType() const;
  void setLineNumberType(IndicatorsBorder::LineNumberType p_type);

  QSize sizeHint() const Q_DECL_OVERRIDE;

  const QFont &getFont() const;
  void setFont(const QFont &p_font);

  void setForegroundColor(const QColor &p_color);

  void setBackgroundColor(const QColor &p_color);

  void setCurrentLineNumberForegroundColor(const QColor &p_color);

  void setFoldingColor(const QColor &p_color);

  void setFoldedFoldingColor(const QColor &p_color);

  void setFoldingHighlightColor(const QColor &p_color);

  int lineNumberWidth();

  void setTextFoldingEnabled(bool p_enable);

public slots:
  void updateBorder();

protected:
  void paintEvent(QPaintEvent *p_event) Q_DECL_OVERRIDE;

  void resizeEvent(QResizeEvent *p_event) Q_DECL_OVERRIDE;

  void mousePressEvent(QMouseEvent *p_event) Q_DECL_OVERRIDE;

  void mouseReleaseEvent(QMouseEvent *p_event) Q_DECL_OVERRIDE;

  void mouseMoveEvent(QMouseEvent *p_event) Q_DECL_OVERRIDE;

  void wheelEvent(QWheelEvent *p_event) Q_DECL_OVERRIDE;

  void leaveEvent(QEvent *p_event) Q_DECL_OVERRIDE;

private slots:
  void highlightFolding();

private:
  enum Indicators { None, LineNumber, FoldingMarker };

  void requestUpdate();

  void paintBorder(const QRect &p_rect);

  void paintFoldingMarker(QPainter &p_painter, int p_xOffset, int p_yOffset, int p_width,
                          int p_height, bool p_folded) const;

  IndicatorsBorder::Indicators positionToIndicator(const QPoint &p_pos) const;

  int borderWidth() const;

  // @p_y is the Y position in current widget coordinate.
  QTextBlock yToBlock(int p_y) const;

  void clearFoldingHighlight();

  void kickOffFoldingHighlight(int p_blockNumber);

  IndicatorsBorderInterface *m_interface = nullptr;

  LineNumberType m_lineNumberType = LineNumberType::None;

  // <X position of the right border of indicator, indicator type>.
  typedef QPair<int, IndicatorsBorder::Indicators> IndicatorRightX;
  QVector<IndicatorRightX> m_indicatorsPosition;

  bool m_needUpdateIndicatorsPosition = true;

  QColor m_backgroundColor{"#eeeeee"};
  QColor m_foregroundColor{"#aaaaaa"};
  QColor m_currentLineNumberForegroundColor{"#222222"};
  QColor m_foldingColor{"#6495ed"};
  QColor m_foldedFoldingColor{"#4169e1"};
  QColor m_foldingHighlightColor{"#a9c4f5"};

  QFont m_font;

  qreal m_maxCharWidth = 0.0;

  int m_lineNumberWidth = 0;

  int m_lineNumberCount = 0;

  const int c_separatorWidth = 2;

  bool m_enableTextFolding = false;

  int m_foldingMarkerWith = 0;

  int m_lastClickedBlockNumber = -1;

  int m_currentFoldingHighlightBlockNumber = -1;

  // <id, range>.
  QSharedPointer<QPair<qint64, TextBlockRange>> m_currentFoldingRange;

  // Anti-flicker.
  QTimer m_foldingHighlightTimer;

  static const int c_maxNumberOfLinesToSearchBackwardsForSyntaxFolding = 1024;
};
} // namespace vte

#endif // INDICATORSBORDER_H
