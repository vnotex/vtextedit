#ifndef EDITORPREVIEWMGR_H
#define EDITORPREVIEWMGR_H

#include <vtextedit/previewmgr.h>

namespace vte {
class VMarkdownEditor;

class EditorPreviewMgr : public PreviewMgrInterface {
public:
  explicit EditorPreviewMgr(VMarkdownEditor *p_editor);

  QTextDocument *document() const Q_DECL_OVERRIDE;

  // Tab stop distance in pixels.
  int tabStopDistance() const Q_DECL_OVERRIDE;

  const QString &basePath() const Q_DECL_OVERRIDE;

  DocumentResourceMgr *documentResourceMgr() const Q_DECL_OVERRIDE;

  qreal scaleFactor() const Q_DECL_OVERRIDE;

  void addPossiblePreviewBlock(int p_blockNumber) Q_DECL_OVERRIDE;

  const QSet<int> &getPossiblePreviewBlocks() const Q_DECL_OVERRIDE;

  void clearPossiblePreviewBlocks(const QVector<int> &p_blocksToClear) Q_DECL_OVERRIDE;

  void relayout(const OrderedIntSet &p_blocks) Q_DECL_OVERRIDE;

  void ensureCursorVisible() Q_DECL_OVERRIDE;

private:
  VMarkdownEditor *m_editor = nullptr;

  // Whether the caret block was within the visible block range right before the
  // last relayout(). PreviewMgr::relayout() always calls relayout() immediately
  // before ensureCursorVisible(), which is what makes this flag describe the
  // state the caller is asking about. Defaults to true so that any path which
  // reaches ensureCursorVisible() WITHOUT a preceding relayout() keeps the old
  // unconditional behaviour.
  bool m_caretWasVisibleBeforeRelayout = true;

  // Re-entrancy guard for the scroll compensation: the setValue() at the end of
  // relayout() fires a scroll, which can drive InteractivePreviewHost
  // realization and, in turn, another preview publication.
  bool m_inScrollCompensation = false;
};
} // namespace vte

#endif // EDITORPREVIEWMGR_H
