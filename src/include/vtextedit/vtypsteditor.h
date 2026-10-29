#ifndef VTEXTEDIT_VTYPSTEDITOR_H
#define VTEXTEDIT_VTYPSTEDITOR_H

#include <vtextedit/vtextedit_export.h>

#include <QByteArray>
#include <QMetaType>
#include <QPointF>
#include <QScopedPointer>
#include <QSharedPointer>
#include <QSize>
#include <QStringList>
#include <QUrl>
#include <QVector>
#include <QWidget>

class QMenu;
class QTextDocument;

namespace vte {
class TypstEditorConfig;
class VTextEditor;
struct TextEditorParameters;

// One evaluated, outlined heading; field names follow md::HeadingInfo.
struct TypstHeadingInfo {
  // Half-open LF/UTF-16 offsets within m_file, or -1/-1 when unmappable.
  int m_startPos = -1;
  int m_endPos = -1;
  // Resolved Typst nesting level, starting at one (not limited to six).
  int m_level = -1;
  // Plain text of the evaluated body, without generated numbering.
  QString m_title;
  // Project-relative slash path, or @package path; empty when unmappable.
  QString m_file;
};

// Snapshot identities are opaque. Actions resolve the original compiler record, not copied fields.
struct TypstResourceContext {
  enum class Surface { Source, InPlace, Preview };
  Surface m_surface = Surface::Source;
  quint64 m_epoch = 0;
  quint64 m_generation = 0;
  quint64 m_snapshot = 0;
  quint64 m_imageId = 0;
  QString m_imagePath;
  QSize m_imageSize;
  int m_width = 0;
  int m_height = 0;
  bool m_imageResizable = false;
  QString m_sourceFile;
  int m_sourceStart = -1;
  int m_sourceEnd = -1;
  QUrl m_linkUrl;
  int m_linkPage = -1;
  QPointF m_linkPoint;
  int m_linkIndex = -1;
  bool m_linkNavigable = false;
};

// One source document and compiler session, independent of the presentation mode.
class VTEXTEDIT_EXPORT VTypstEditor : public QWidget {
  Q_OBJECT
public:
  enum class Mode { Edit, Read };
  Q_ENUM(Mode)
  enum class CompileStatus { Idle, Compiling, Ready, Error, Unavailable };
  Q_ENUM(CompileStatus)
  enum class PdfZoomMode { Custom, Width, Page };
  Q_ENUM(PdfZoomMode)

  VTypstEditor(const QSharedPointer<TypstEditorConfig> &p_config,
               const QSharedPointer<TextEditorParameters> &p_parameters,
               QWidget *p_parent = nullptr);
  ~VTypstEditor() Q_DECL_OVERRIDE;

  VTextEditor *getTextEditor() const;
  QTextDocument *document() const;
  Mode mode() const;
  void setMode(Mode p_mode);
  // Runtime preferences for Edit; Read remains PDF-only and remembers both flags.
  // Defaults: Edit with in-place preview enabled and live PDF preview disabled.
  bool isInplacePreviewEnabled() const;
  void setInplacePreviewEnabled(bool p_enabled);
  bool isLivePreviewEnabled() const;
  void setLivePreviewEnabled(bool p_enabled);
  bool isReadOnly() const;
  void setReadOnly(bool p_readOnly);

  const TypstEditorConfig &getConfig() const;
  // Use this instead of the contained editor's setConfig to refresh Typst formats too.
  void setConfig(const QSharedPointer<TypstEditorConfig> &p_config);
  // Begin a different-document bind without replacing widgets or the native session.
  // Clears source/history/output; compilation stays suspended across all identity/text setters.
  void resetForNewDocument();
  // Call only after the incoming project, active file and validated source are installed.
  // An unsuccessful source load must leave the reset unfinished. No-op outside a reset.
  void finishDocumentReset();
  // Preserves the configured memory-only policy; use setConfig() to change that policy.
  void setProject(const QString &p_root, const QString &p_mainFile);
  QString activeFile() const;
  // Changes this buffer's identity, not its contents. The host supplies the new text.
  void setActiveFile(const QString &p_file);
  // Paths are project-root-relative. Updating the active overlay replaces its source buffer.
  void setFileOverlay(const QString &p_file, const QString &p_text);
  // The active buffer remains an overlay while open; switch identity before removing it.
  void removeFileOverlay(const QString &p_file);

  CompileStatus compileStatus() const;
  QString compileMessage() const;
  QStringList diagnostics() const;
  // Whole compiled main document, in document order, excluding outlined:false.
  // Empty until a successful compile and immediately on input/compiler invalidation.
  // Borrowed until the next update; copy the QVector to retain a snapshot.
  const QVector<TypstHeadingInfo> &getHeadings() const;
  void retryCompilation();
  void showOutputAtCursor();

  // Host-owned PDF controls; the preview pane remains internal to this widget.
  qreal pdfZoom() const;
  PdfZoomMode pdfZoomMode() const;
  void setPdfZoom(qreal p_zoom);
  void fitPdfWidth();
  void fitPdfPage();
  void setPdfSearchText(const QString &p_text);
  void findNextPdfMatch(bool p_backwards = false);
  // Displayed output only: zero-based page, or -1 with count 0 before publication.
  int currentPdfPage() const;
  int pdfPageCount() const;
  void goToPdfPage(int p_page);
  bool hasPendingPdfUpdate() const;
  void publishPendingPdfUpdate();

  bool isResourceContextValid(const TypstResourceContext &p_context, bool p_forEdit = false) const;
  void requestImageData(const TypstResourceContext &p_context, quint64 p_requestId);
  bool resizeImage(const TypstResourceContext &p_context, int p_width, int p_height);
  // Resolve the captured snapshot, not mutable context URL/page fields. Direct cards
  // pass false; origin affects host URL routing only, never internal page navigation.
  void openContextLink(const TypstResourceContext &p_context, bool p_fromContextMenu = true);

signals:
  void modeChanged(vte::VTypstEditor::Mode p_mode);
  void inplacePreviewEnabledChanged(bool p_enabled);
  void livePreviewEnabledChanged(bool p_enabled);
  void compileStatusChanged(vte::VTypstEditor::CompileStatus p_status, const QString &p_message);
  void diagnosticsChanged(const QStringList &p_diagnostics);
  // Complete accepted outline, or empty when a nonempty outline is invalidated.
  void headingsUpdated(const QVector<vte::TypstHeadingInfo> &p_headings);
  // Actual preview camera state, including fit-mode changes at the same scale.
  void pdfZoomChanged(qreal p_zoom, vte::VTypstEditor::PdfZoomMode p_mode);
  // Re-query currentPdfPage()/pdfPageCount() after scrolling or PDF publication.
  void pdfNavigationChanged();
  void pdfUpdateAvailableChanged(bool p_available);
  // Imported-file navigation is delegated to the host. Offsets are LF/UTF-16, half-open.
  void sourceLocationRequested(const QString &p_file, int p_start, int p_end);
  // The host applies its URL opening/security policy; the editor never launches URLs.
  // Menu origin is explicit; direct pane, card and compiler navigation report false.
  void externalLinkRequested(const QUrl &p_url, bool p_fromContextMenu);
  void resourceContextMenuRequested(const vte::TypstResourceContext &p_context, QMenu *p_menu);
  void imageActivated(const vte::TypstResourceContext &p_context);
  void imageDataReady(quint64 p_requestId, const QByteArray &p_data, const QString &p_mime,
                      const QString &p_error);
  void resourceContextsInvalidated();

protected:
  void changeEvent(QEvent *p_event) Q_DECL_OVERRIDE;
  void hideEvent(QHideEvent *p_event) Q_DECL_OVERRIDE;

private:
  class Private;
  QScopedPointer<Private> d;
};

} // namespace vte

Q_DECLARE_METATYPE(vte::TypstHeadingInfo)
Q_DECLARE_METATYPE(vte::TypstResourceContext)

#endif // VTEXTEDIT_VTYPSTEDITOR_H
