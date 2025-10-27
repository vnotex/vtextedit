#ifndef EDITORINDICATORSBORDER_H
#define EDITORINDICATORSBORDER_H

#include "indicatorsborder.h"

class QTextDocument;
class QAbstractTextDocumentLayout;

namespace vte {
class VTextEditor;
class VTextEdit;

class EditorIndicatorsBorder : public IndicatorsBorderInterface {
public:
  explicit EditorIndicatorsBorder(VTextEditor *p_editor);

  int blockCount() const Q_DECL_OVERRIDE;

  QTextBlock firstVisibleBlock() const Q_DECL_OVERRIDE;

  int contentOffsetAtTop() const Q_DECL_OVERRIDE;

  QSize viewportSize() const Q_DECL_OVERRIDE;

  QRectF blockBoundingRect(const QTextBlock &p_block) const Q_DECL_OVERRIDE;

  int cursorBlockNumber() const Q_DECL_OVERRIDE;

  QTextBlock cursorBlock() const Q_DECL_OVERRIDE;

  bool hasInvisibleBlocks() const Q_DECL_OVERRIDE;

  TextFolding &textFolding() const Q_DECL_OVERRIDE;

  QTextBlock findBlockByYPosition(int p_y) const Q_DECL_OVERRIDE;

  QWidget *viewWidget() const Q_DECL_OVERRIDE;

  void forwardWheelEvent(QWheelEvent *p_event) Q_DECL_OVERRIDE;

  void forwardMouseEvent(QMouseEvent *p_event) Q_DECL_OVERRIDE;

  QSharedPointer<TextBlockRange>
  fetchSyntaxFoldingRangeStartingOnBlock(int p_blockNumber) Q_DECL_OVERRIDE;

  void setFocus() Q_DECL_OVERRIDE;

private:
  QTextDocument *document() const;

  QAbstractTextDocumentLayout *documentLayout() const;

  VTextEditor *m_editor = nullptr;

  VTextEdit *m_textEdit = nullptr;
};
} // namespace vte

#endif // EDITORINDICATORSBORDER_H
