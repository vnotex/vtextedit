#ifndef VTEXTEDIT_PREVIEWWIDGET_H
#define VTEXTEDIT_PREVIEWWIDGET_H

#include "vtextedit_export.h"

#include <QObject>
#include <QRectF>
#include <QScopedPointer>
#include <QSize>
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
} // namespace vte

#endif // VTEXTEDIT_PREVIEWWIDGET_H
