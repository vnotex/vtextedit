#include <vtextedit/previewwidget.h>

#include <QPointer>

using namespace vte;

namespace vte {
class PreviewWidgetContextPrivate {
public:
  quint64 m_identity = 0;

  QSharedPointer<const Preview> m_preview;

  QRectF m_sourceTextRect;

  QRectF m_availableContentRect;

  QSize m_viewportSize;

  QRectF m_assignedPreviewRect;
};

class PreviewWidgetPrivate {
public:
  QPointer<PreviewWidgetContext> m_context;
};
} // namespace vte

PreviewWidgetContext::PreviewWidgetContext(quint64 p_identity, QObject *p_parent)
    : QObject(p_parent), m_d(new PreviewWidgetContextPrivate()) {
  m_d->m_identity = p_identity;
}

PreviewWidgetContext::~PreviewWidgetContext() {}

quint64 PreviewWidgetContext::identity() const { return m_d->m_identity; }

QSharedPointer<const Preview> PreviewWidgetContext::preview() const { return m_d->m_preview; }

QRectF PreviewWidgetContext::sourceTextRect() const { return m_d->m_sourceTextRect; }

QRectF PreviewWidgetContext::availableContentRect() const { return m_d->m_availableContentRect; }

QSize PreviewWidgetContext::viewportSize() const { return m_d->m_viewportSize; }

QRectF PreviewWidgetContext::assignedPreviewRect() const { return m_d->m_assignedPreviewRect; }

void PreviewWidgetContext::setPreview(const QSharedPointer<const Preview> &p_preview) {
  m_d->m_preview = p_preview;
}

void PreviewWidgetContext::notifyReplacementFinished(const PreviewReplacementResult &p_result) {
  emit replacementFinished(p_result);
}

bool PreviewWidgetContext::setGeometryContext(const QRectF &p_sourceTextRect,
                                              const QRectF &p_availableContentRect,
                                              const QSize &p_viewportSize,
                                              const QRectF &p_assignedRect) {
  if (m_d->m_sourceTextRect == p_sourceTextRect &&
      m_d->m_availableContentRect == p_availableContentRect &&
      m_d->m_viewportSize == p_viewportSize && m_d->m_assignedPreviewRect == p_assignedRect) {
    return false;
  }

  m_d->m_sourceTextRect = p_sourceTextRect;
  m_d->m_availableContentRect = p_availableContentRect;
  m_d->m_viewportSize = p_viewportSize;
  m_d->m_assignedPreviewRect = p_assignedRect;

  emit geometryContextChanged();
  return true;
}

void PreviewWidgetContext::requestSourceReplacement(const QString &p_replacementMarkdown) {
  if (!m_d->m_preview) {
    return;
  }

  emit sourceReplacementRequested(m_d->m_identity, m_d->m_preview->revision(),
                                  m_d->m_preview->sourceMarkdown(), p_replacementMarkdown);
}

PreviewWidget::PreviewWidget(PreviewWidgetContext *p_context, QWidget *p_parent)
    : QWidget(p_parent), m_d(new PreviewWidgetPrivate()) {
  m_d->m_context = p_context;
}

PreviewWidget::~PreviewWidget() {}

qreal PreviewWidget::preferredWidthFraction() const { return 0; }

void PreviewWidget::clearSelection() {}

PreviewWidgetContext *PreviewWidget::previewContext() const { return m_d->m_context.data(); }

PreviewWidgetFactory::PreviewWidgetFactory(QObject *p_parent) : QObject(p_parent) {}

PreviewWidgetFactory::~PreviewWidgetFactory() {}
