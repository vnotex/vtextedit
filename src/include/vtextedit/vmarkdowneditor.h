#ifndef VTEXTEDIT_VMARKDOWNEDITOR_H
#define VTEXTEDIT_VMARKDOWNEDITOR_H

#include <vtextedit/vtexteditor.h>

#include <QHash>

namespace vte {
class PegMarkdownHighlighter;
class EditorPegMarkdownHighlighter;
class DocumentResourceMgr;
class TextDocumentLayout;
class EditorPreviewMgr;
class PreviewMgr;
class MarkdownEditorConfig;
class WebCodeBlockHighlighter;

class VTEXTEDIT_EXPORT VMarkdownEditor : public VTextEditor {
  Q_OBJECT

  friend class EditorPreviewMgr;

public:
  typedef QHash<QString, QTextCharFormat> ExternalCodeBlockHighlightStyles;

  VMarkdownEditor(const QSharedPointer<MarkdownEditorConfig> &p_config,
                  const QSharedPointer<TextEditorParameters> &p_paras, QWidget *p_parent = nullptr);

  virtual ~VMarkdownEditor();

  void setSyntax(const QString &p_syntax) Q_DECL_OVERRIDE;
  QString getSyntax() const Q_DECL_OVERRIDE;

  DocumentResourceMgr *getDocumentResourceMgr() const;

  const QPixmap *findImageFromDocumentResourceMgr(const QString &p_name) const;

  TextDocumentLayout *documentLayout() const;

  PegMarkdownHighlighter *getHighlighter() const;

  PreviewMgr *getPreviewMgr() const;

  void setConfig(const QSharedPointer<MarkdownEditorConfig> &p_config);

  void zoom(int p_delta) Q_DECL_OVERRIDE;

  // Temporarily enable/disable in-place preview without affecting the preview
  // sources.
  void setInplacePreviewEnabled(bool p_enabled);

  static void setExternalCodeBlockHighlihgtStyles(const ExternalCodeBlockHighlightStyles &p_styles);

public slots:
  // Used when using WebCodeBlockHighlighter.
  void handleExternalCodeBlockHighlightData(int p_idx, TimeStamp p_timeStamp,
                                            const QString &p_html);

signals:
  // Used when using WebCodeBlockHighlighter.
  void externalCodeBlockHighlightRequested(int p_idx, TimeStamp p_timeStamp, const QString &p_text);

protected:
  bool eventFilter(QObject *p_obj, QEvent *p_event) Q_DECL_OVERRIDE;

private:
  void setupDocumentLayout();

  void setupSyntaxHighlighter();

  void setupPreviewMgr();

  void updateFromConfig();

  // Return true if @p_event is handled.
  bool handleKeyPressEvent(QKeyEvent *p_event);

  void preKeyReturn(int p_modifiers, bool *p_changed, bool *p_handled);

  void postKeyReturn(int p_modifiers);

  void preKeyTab(int p_modifiers, bool *p_handled);

  void preKeyBacktab(int p_modifiers, bool *p_handled);

  void updateInplacePreviewSources();

  void updateSpaceWidth();

  QScopedPointer<EditorPegMarkdownHighlighter> m_highlighterInterface;

  QScopedPointer<DocumentResourceMgr> m_resourceMgr;

  QScopedPointer<EditorPreviewMgr> m_previewMgrInterface;

  // Managed by QObject.
  PreviewMgr *m_previewMgr = nullptr;

  QSharedPointer<MarkdownEditorConfig> m_config;

  bool m_inplacePreviewEnabled = true;

  // Managed by QObject.
  WebCodeBlockHighlighter *m_webCodeBlockHighlighter = nullptr;
};
} // namespace vte

#endif
