#ifndef VTEXTEDIT_PREVIEWWIDGET_H
#define VTEXTEDIT_PREVIEWWIDGET_H

#include "vtextedit_export.h"

#include <QFont>
#include <QObject>
#include <QRectF>
#include <QScopedPointer>
#include <QSize>
#include <QSizeF>
#include <QVector>
#include <QWidget>

#include <vtextedit/preview.h>

namespace vte {
class InteractivePreviewHost;
class PreviewWidgetContextPrivate;
class PreviewWidgetPrivate;

// Per-widget bridge to the editor.
//
// The host creates the context before invoking any factory and keeps it alive
// for the whole lifetime of the widget it belongs to. All rectangles are in
// document coordinates; QWidget geometry stays viewport local and is managed
// by the host.
class VTEXTEDIT_EXPORT PreviewWidgetContext : public QObject {
  Q_OBJECT
public:
  ~PreviewWidgetContext() Q_DECL_OVERRIDE;

  // Stable identity of the preview this context belongs to.
  quint64 identity() const;

  // The snapshot currently bound to this context. Never null while the widget
  // is alive.
  QSharedPointer<const Preview> preview() const;

  // Union of the visible text layout rectangles over the whole source range.
  QRectF sourceTextRect() const;

  // The rectangle the preview may occupy at most.
  QRectF availableContentRect() const;

  QSize viewportSize() const;

  // The rectangle actually assigned by the layout. Null when the preview is
  // not currently laid out (folded away, disabled, etc.).
  QRectF assignedPreviewRect() const;

  // Ask the editor to replace the whole source of this preview.
  // The outcome is reported asynchronously through replacementFinished().
  void requestSourceReplacement(const QString &p_replacementMarkdown);

signals:
  // Any of sourceTextRect(), availableContentRect(), viewportSize() or
  // assignedPreviewRect() changed.
  void geometryContextChanged();

  void replacementFinished(const vte::PreviewReplacementResult &p_result);

  // Internal: emitted by requestSourceReplacement() for the host to consume.
  // Applications should not connect to or emit this signal.
  void sourceReplacementRequested(quint64 p_identity, quint64 p_revision,
                                  const QString &p_expectedSource,
                                  const QString &p_replacementMarkdown);

private:
  friend class InteractivePreviewHost;

  explicit PreviewWidgetContext(quint64 p_identity, QObject *p_parent = nullptr);

  void setPreview(const QSharedPointer<const Preview> &p_preview);

  void notifyReplacementFinished(const PreviewReplacementResult &p_result);

  // Returns true if anything changed.
  bool setGeometryContext(const QRectF &p_sourceTextRect, const QRectF &p_availableContentRect,
                          const QSize &p_viewportSize, const QRectF &p_assignedRect);

  QScopedPointer<PreviewWidgetContextPrivate> m_d;
};

// Base class every interactive preview renderer must derive from.
class VTEXTEDIT_EXPORT PreviewWidget : public QWidget {
  Q_OBJECT
public:
  explicit PreviewWidget(PreviewWidgetContext *p_context, QWidget *p_parent = nullptr);

  ~PreviewWidget() Q_DECL_OVERRIDE;

  // Element types this instance can render. Validated by the host after
  // construction.
  virtual QVector<PreviewElementType> supportedTypes() const = 0;

  // Bind (or rebind) a snapshot. Return false to make the host drop this
  // instance and fall through to the next factory.
  virtual bool setPreview(const QSharedPointer<const Preview> &p_preview) = 0;

  // Minimum share of the available content width this renderer wants to occupy,
  // as a fraction in [0, 1]. 0 (the default) means "natural size". Only honored
  // for previews placed on their own band (PreviewPlacement::BlockAfterSource);
  // an inline preview is always bound to the text span it replaces.
  virtual qreal preferredWidthFraction() const;

  // Collapse any selection this renderer holds. Called by the host when focus
  // returns to the text editor. Default implementation does nothing.
  virtual void clearSelection();

  PreviewWidgetContext *previewContext() const;

private:
  QScopedPointer<PreviewWidgetPrivate> m_d;
};

// Creates PreviewWidget instances for the element types it advertises.
//
// Ownership is transferred to the editor on successful registration.
class VTEXTEDIT_EXPORT PreviewWidgetFactory : public QObject {
  Q_OBJECT
public:
  explicit PreviewWidgetFactory(QObject *p_parent = nullptr);

  ~PreviewWidgetFactory() Q_DECL_OVERRIDE;

  // Advertised before construction. The host only calls createWidget() for
  // types listed here.
  virtual QVector<PreviewElementType> supportedTypes() const = 0;

  // Return nullptr to decline; the host then tries the next factory.
  virtual PreviewWidget *createWidget(PreviewWidgetContext *p_context,
                                      const QSharedPointer<const Preview> &p_preview,
                                      QWidget *p_parent) = 0;
};

// Optional companion interface to PreviewWidgetFactory: how tall an element's
// widget WOULD be, computed without constructing it.
//
// This is what makes lazy realization possible. Without an estimate the host
// has no height to reserve for an element until its widget exists, so it must
// build every widget in the document up front - which for a file with hundreds
// of tables is seconds of work and hundreds of live QTextDocuments, almost all
// of them off screen.
//
// A separate interface rather than a new virtual on PreviewWidgetFactory,
// deliberately: adding a virtual to an exported polymorphic class is source
// compatible but NOT binary compatible, and it would break every out-of-tree
// factory compiled against an earlier VTextEdit. Discovery is by
// qobject_cast, so a factory which does not implement this keeps exactly
// today's eager behaviour.
//
// Implement it as a second base of a PreviewWidgetFactory subclass and declare
// it with Q_INTERFACES(vte::PreviewSizeEstimator).
class VTEXTEDIT_EXPORT PreviewSizeEstimator {
public:
  virtual ~PreviewSizeEstimator();

  // The size the widget for @p_preview is expected to report when it is given
  // @p_widthBasis of horizontal room and rendered in @p_font.
  //
  // Return an INVALID QSizeF to say "measure me properly": the host then
  // realizes the widget eagerly, as it always did. That is the supported
  // answer for an element whose height cannot be predicted cheaply, and it is
  // better than a wrong estimate, which shows up to the user as the document
  // jumping when the widget is finally built.
  //
  // Must be cheap - it is called for every element of the document on every
  // publication - and must not touch the editor's document.
  virtual QSizeF estimatedSize(const QSharedPointer<const Preview> &p_preview, qreal p_widthBasis,
                               const QFont &p_font) const = 0;
};
} // namespace vte

Q_DECLARE_INTERFACE(vte::PreviewSizeEstimator, "org.vnotex.vtextedit.PreviewSizeEstimator/1.0")

#endif // VTEXTEDIT_PREVIEWWIDGET_H
