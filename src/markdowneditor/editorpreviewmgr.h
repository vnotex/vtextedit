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
};
} // namespace vte

#endif // EDITORPREVIEWMGR_H
