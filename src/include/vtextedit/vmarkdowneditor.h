#ifndef VTEXTEDIT_VMARKDOWNEDITOR_H
#define VTEXTEDIT_VMARKDOWNEDITOR_H

#include <vtextedit/preview.h>
#include <vtextedit/vtexteditor.h>

#include <QHash>

namespace vte {
class MarkdownHighlighter;
class EditorMarkdownHighlighter;
class DocumentResourceMgr;
class TextDocumentLayout;
class EditorPreviewMgr;
class PreviewMgr;
class MarkdownEditorConfig;
class WebCodeBlockHighlighter;
class MathBlockHighlighter;
class MarkdownFoldingProvider;
class PreviewWidgetFactory;
class InteractivePreviewHost;

class VTEXTEDIT_EXPORT VMarkdownEditor : public VTextEditor {
  Q_OBJECT

  friend class EditorPreviewMgr;
  friend class InteractivePreviewHost;

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

  MarkdownHighlighter *getHighlighter() const;

  PreviewMgr *getPreviewMgr() const;

  void setConfig(const QSharedPointer<MarkdownEditorConfig> &p_config);

  void zoom(int p_delta) Q_DECL_OVERRIDE;

  // Temporarily enable/disable in-place preview without affecting the preview
  // sources.
  void setInplacePreviewEnabled(bool p_enabled);

  // Register an interactive preview renderer.
  //
  // Factories are tried by descending explicit priority, then by registration
  // order. The built-in renderers use priority 0, so an application can
  // override them with any higher priority.
  //
  // Rejects null, duplicate and reentrant registration without taking
  // ownership. On success the factory is reparented to an internal host and is
  // destroyed together with this editor.
  bool registerPreviewWidgetFactory(PreviewWidgetFactory *p_factory, int p_priority = 0);

  // Deactivate and destroy a previously registered factory. Ownership never
  // returns to the caller: the pointer is invalid once this returns true.
  bool unregisterPreviewWidgetFactory(PreviewWidgetFactory *p_factory);

  static void setExternalCodeBlockHighlihgtStyles(const ExternalCodeBlockHighlightStyles &p_styles);

public slots:
  // Used when using WebCodeBlockHighlighter.
  void handleExternalCodeBlockHighlightData(int p_idx, TimeStamp p_timeStamp,
                                            const QString &p_html);

  // Used for display math ($$...$$) source highlight.
  void handleExternalMathHighlightData(int p_idx, TimeStamp p_timeStamp, const QString &p_html);

signals:
  // Used when using WebCodeBlockHighlighter.
  void externalCodeBlockHighlightRequested(int p_idx, TimeStamp p_timeStamp, const QString &p_text);

  // Used for display math ($$...$$) source highlight.
  void externalMathHighlightRequested(int p_idx, TimeStamp p_timeStamp, const QString &p_text);

protected:
  bool eventFilter(QObject *p_obj, QEvent *p_event) Q_DECL_OVERRIDE;

private:
  void setupDocumentLayout();

  void setupSyntaxHighlighter();

  void setupPreviewMgr();

  // Locate the internal interactive preview host. Returns nullptr before it
  // has been created.
  InteractivePreviewHost *interactivePreviewHost() const;

  void updateFromConfig();

  // Re-evaluate the preview driven fold state of every foldable region and
  // write the outcome back onto the interactive preview items. Always called
  // through InteractivePreviewHost::scheduleFoldRefresh(): the highlighter
  // emits foldingRegionsUpdated *before* previewElementsUpdated, so a
  // synchronous evaluation would read the previous generation's previews.
  void applyPreviewFolding();

  // Re-create, already folded, the range an accepted in-place rewrite just
  // destroyed. Called by the host from inside the rewrite, so the source never
  // visibly expands.
  void restoreFoldAfterPreviewRewrite(PreviewElementType p_type, int p_startBlock, int p_endBlock);

  // Whether the region [p_startBlock, p_endBlock] of type @p_type has a live
  // folding range, and if so whether it is folded right now. False means there
  // is no such range, which is not the same as unfolded.
  bool tryPreviewSourceFolded(PreviewElementType p_type, int p_startBlock, int p_endBlock,
                              bool *p_folded) const;

  // Return true if @p_event is handled.
  bool handleKeyPressEvent(QKeyEvent *p_event);

  void preKeyReturn(int p_modifiers, bool *p_changed, bool *p_handled);

  void postKeyReturn(int p_modifiers);

  void preKeyTab(int p_modifiers, bool *p_handled);

  void preKeyBacktab(int p_modifiers, bool *p_handled);

  void updateInplacePreviewSources();

  void applyLineSpacing();

  void updateSpaceWidth();

  QScopedPointer<EditorMarkdownHighlighter> m_highlighterInterface;

  QScopedPointer<DocumentResourceMgr> m_resourceMgr;

  QScopedPointer<EditorPreviewMgr> m_previewMgrInterface;

  // Managed by QObject.
  PreviewMgr *m_previewMgr = nullptr;

  QSharedPointer<MarkdownEditorConfig> m_config;

  bool m_inplacePreviewEnabled = true;

  QScopedPointer<MarkdownFoldingProvider> m_foldingProvider;

  // Managed by QObject.
  WebCodeBlockHighlighter *m_webCodeBlockHighlighter = nullptr;

  // Managed by QObject.
  MathBlockHighlighter *m_mathBlockHighlighter = nullptr;
};
} // namespace vte

#endif
