#include "typstpreviewhost.h"

#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QEvent>
#include <QFontMetricsF>
#include <QMap>
#include <QMenu>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPointer>
#include <QScopeGuard>
#include <QScrollBar>
#include <QSet>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QWheelEvent>
#include <QWidget>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>

#include "typstsourceeditor.h"
#include <markdowneditor/textdocumentlayout.h>
#include <texteditor/textfolding.h>
#include <vtextedit/typsteditorconfig.h>
#include <vtextedit/vtextedit.h>

using namespace vte;

namespace {
constexpr qreal c_padding = 6;

int typeOrder(const QString &p_kind) {
  if (p_kind == QLatin1String("figure"))
    return 0;
  if (p_kind == QLatin1String("table"))
    return 1;
  if (p_kind == QLatin1String("image"))
    return 2;
  return 3;
}

QString kindLabel(const QString &p_kind) {
  if (p_kind == QLatin1String("figure"))
    return TypstPreviewHost::tr("Figure");
  if (p_kind == QLatin1String("table"))
    return TypstPreviewHost::tr("Table");
  if (p_kind == QLatin1String("image"))
    return TypstPreviewHost::tr("Image");
  return TypstPreviewHost::tr("Equation");
}

QString statusLabel(const QString &p_status) {
  if (p_status == QLatin1String("empty"))
    return TypstPreviewHost::tr("No visible output");
  if (p_status == QLatin1String("unmapped"))
    return TypstPreviewHost::tr("Generated or unmapped output");
  return TypstPreviewHost::tr("Output bounds unavailable");
}

bool validRect(const QRectF &p_rect) {
  return std::isfinite(p_rect.x()) && std::isfinite(p_rect.y()) && std::isfinite(p_rect.width()) &&
         std::isfinite(p_rect.height()) && p_rect.width() > 0 && p_rect.height() > 0;
}

struct PageImage {
  QImage m_image;
  qreal m_scale = 0;
  quint64 m_use = 0;
};

struct Part {
  const TypstPreviewRegion *m_region = nullptr;
  const QString *m_status = nullptr;
  QRectF m_imageRect;
  qreal m_scale = 1;
  qreal m_maxBottom = 0;
};

bool isInlinePreview(const TypstPreviewOccurrence &p_occurrence) {
  return p_occurrence.m_kind == QLatin1String("equation") && !p_occurrence.m_block;
}

QVector<Part> partsFor(const QVector<TypstPreviewOccurrence> &p_occurrences, qreal p_width,
                       qreal p_dpiScale, qreal p_maxHeight, const QFont &p_font, QSizeF *p_size,
                       bool p_measureOnly = false) {
  QVector<Part> parts;
  if (!p_measureOnly) {
    qsizetype count = 0;
    for (const auto &occurrence : p_occurrences)
      count += qMax<qsizetype>(1, occurrence.m_regions.size());
    parts.reserve(count);
  }
  const bool inlinePreview = isInlinePreview(p_occurrences.first());
  const qreal innerWidth = qMax<qreal>(1, p_width - 2 * c_padding);
  qreal left = c_padding;
  qreal top = c_padding;
  qreal height = c_padding;
  qreal statusHeight = 0;
  bool hasFragments = false;
  for (const auto &occurrence : p_occurrences) {
    qreal columnWidth = 0;
    if (occurrence.m_regions.isEmpty()) {
      if (!statusHeight)
        statusHeight = qCeil(QFontMetricsF(p_font).height());
      columnWidth = inlinePreview
                        ? QFontMetricsF(p_font).horizontalAdvance(statusLabel(occurrence.m_status))
                        : innerWidth;
      if (!p_measureOnly) {
        parts.append(
            {nullptr, &occurrence.m_status, QRectF(left, top, columnWidth, statusHeight), 1});
      }
      top += statusHeight + c_padding;
    } else {
      hasFragments |= occurrence.m_regions.size() > 1;
      for (const auto &region : occurrence.m_regions) {
        qreal scale =
            qMin(p_dpiScale, qMax<qreal>(1, p_maxHeight - 2 * c_padding) / region.m_rect.height());
        if (!inlinePreview)
          scale = qMin(scale, innerWidth / region.m_rect.width());
        const QRectF rect(left, top, region.m_rect.width() * scale, region.m_rect.height() * scale);
        if (!p_measureOnly)
          parts.append({&region, nullptr, rect, scale});
        columnWidth = qMax(columnWidth, rect.width());
        top += rect.height() + c_padding;
      }
    }
    height = qMax(height, top);
    if (inlinePreview) {
      // Each evaluation is a column; page fragments within it stay sequential.
      left += columnWidth + c_padding;
      top = c_padding;
    }
  }
  *p_size = QSizeF(p_width, height);
  if (inlinePreview) {
    // Fit the whole row, including its gaps, without wrapping or upscaling.
    const qreal fit = qMin<qreal>(1, innerWidth / qMax<qreal>(1, left - 2 * c_padding));
    *p_size = QSizeF(qMin(p_width, (left - 2 * c_padding) * fit + 2 * c_padding),
                     (height - 2 * c_padding) * fit + 2 * c_padding);
    if (fit < 1) {
      const QPointF padding(c_padding, c_padding);
      for (auto &part : parts) {
        part.m_imageRect = QRectF(padding + (part.m_imageRect.topLeft() - padding) * fit,
                                  part.m_imageRect.size() * fit);
        part.m_scale *= fit;
      }
    }
    if (hasFragments && p_occurrences.size() > 1) {
      std::stable_sort(parts.begin(), parts.end(), [](const Part &a, const Part &b) {
        return a.m_imageRect.top() < b.m_imageRect.top();
      });
    }
  }
  // Neighboring columns can have different heights. Index their prefix maximum,
  // not individual bottoms, so top clipping cannot skip a taller earlier part.
  qreal maxBottom = 0;
  for (auto &part : parts) {
    maxBottom = qMax(maxBottom, part.m_imageRect.bottom());
    part.m_maxBottom = maxBottom;
  }
  return parts;
}

void appendPartNeeds(const QVector<Part> &p_parts, const QRectF &p_clip, QHash<int, qreal> &p_needs,
                     qreal p_dpr) {
  auto part =
      std::lower_bound(p_parts.constBegin(), p_parts.constEnd(), p_clip.top(),
                       [](const Part &p_part, qreal p_top) { return p_part.m_maxBottom < p_top; });
  for (; part != p_parts.constEnd() && part->m_imageRect.top() <= p_clip.bottom(); ++part) {
    if (part->m_region && part->m_imageRect.intersects(p_clip))
      p_needs[part->m_region->m_page] =
          qMax(p_needs.value(part->m_region->m_page), part->m_scale * p_dpr);
  }
}

// One viewport-sized widget paints the visible slice of a reservation. Parts
// reference item-owned records; the host destroys widgets before rebuilding items.
class PreviewCard : public QWidget {
public:
  PreviewCard(QWidget *p_parent, const QVector<TypstPreviewOccurrence> *p_occurrences,
              qreal p_maxHeight)
      : QWidget(p_parent), m_occurrences(p_occurrences), m_maxHeight(p_maxHeight) {
    setObjectName(QStringLiteral("typstPreviewCard"));
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_NoMousePropagation);
    const QString title = kindLabel(m_occurrences->first().m_kind);
    setToolTip(title);
    setAccessibleName(title);
  }

  void setContentGeometry(qreal p_width, const QPointF &p_offset) {
    const bool widthChanged = !qFuzzyCompare(m_contentWidth + 1, p_width + 1);
    if (widthChanged) {
      m_contentWidth = p_width;
      QSizeF size;
      m_parts = partsFor(*m_occurrences, p_width, logicalDpiX() / 72.0, m_maxHeight, font(), &size);
    }
    if (widthChanged || m_contentOffset != p_offset) {
      m_contentOffset = p_offset;
      update();
    }
  }

  void appendNeeds(QHash<int, qreal> &p_needs, qreal p_dpr) const {
    appendPartNeeds(m_parts, QRectF(rect()).translated(m_contentOffset), p_needs, p_dpr);
  }

  std::function<const PageImage *(int)> m_pageImage;
  std::function<bool(int)> m_failed;
  TypstSnapshot m_snapshot;
  QPointer<TypstPreviewHost> m_host;

  TypstResourceContext resourceAt(const QPointF &p_position, int *p_page = nullptr,
                                  QPointF *p_point = nullptr) const {
    const auto position = p_position + m_contentOffset;
    for (const auto &part : m_parts) {
      if (!part.m_region || !part.m_imageRect.contains(position))
        continue;
      const auto point =
          part.m_region->m_rect.topLeft() + (position - part.m_imageRect.topLeft()) / part.m_scale;
      if (p_page)
        *p_page = part.m_region->m_page;
      if (p_point)
        *p_point = point;
      return typstResourceAt(m_snapshot, TypstResourceContext::Surface::InPlace,
                             part.m_region->m_page, point);
    }
    return {};
  }

protected:
  void contextMenuEvent(QContextMenuEvent *p_event) override {
    const auto context = resourceAt(p_event->pos());
    if (!m_host || (!context.m_imageId && context.m_linkIndex < 0)) {
      p_event->ignore();
      return;
    }
    const QPointer<PreviewCard> alive(this);
    const QPointer<TypstPreviewHost> host(m_host);
    // Frame replacement may run inside native menu activation. Keep the popup
    // alive until exec() unwinds, but close it when its card is destroyed.
    const QPointer<QMenu> menu(new QMenu(parentWidget()));
    const auto cleanup = qScopeGuard([menu]() { delete menu.data(); });
    connect(host, &QObject::destroyed, menu, &QMenu::close);
    connect(this, &QObject::destroyed, menu, &QMenu::close);
    p_event->accept();
    emit host->resourceContextMenuRequested(context, menu);
    if (alive && alive->isVisible() && alive->isEnabled() && host && menu && !menu->isEmpty())
      menu->exec(p_event->globalPos());
  }

  void mouseReleaseEvent(QMouseEvent *p_event) override {
    if (p_event->button() != Qt::LeftButton || !m_host)
      return;
    int page = -1;
    QPointF point;
    const auto context = resourceAt(p_event->position(), &page, &point);
    const auto host = m_host;
    const auto snapshot = m_snapshot;
    p_event->accept();
    if (context.m_linkIndex >= 0)
      emit host->linkActivated(context);
    else if (page >= 0 && snapshot)
      emit host->sourceLocationRequested(snapshot->m_id, page, point);
  }

  void mouseDoubleClickEvent(QMouseEvent *p_event) override {
    const auto context = resourceAt(p_event->position());
    if (p_event->button() == Qt::LeftButton && m_host && context.m_imageId &&
        context.m_linkIndex < 0) {
      p_event->accept();
      emit m_host->imageActivated(context);
    }
  }

  void paintEvent(QPaintEvent *p_event) override {
    QPainter painter(this);
    painter.translate(-m_contentOffset);
    const QRectF clip = QRectF(p_event->rect()).translated(m_contentOffset);
    for (int i = firstPart(clip.top()); i < m_parts.size(); ++i) {
      const auto &part = m_parts[i];
      const auto &imageRect = part.m_imageRect;
      if (imageRect.top() > clip.bottom())
        break;
      if (!imageRect.intersects(clip))
        continue;
      if (!part.m_region) {
        painter.save();
        painter.translate(imageRect.topLeft());
        painter.scale(part.m_scale, part.m_scale);
        painter.setPen(palette().text().color());
        painter.drawText(QRectF(QPointF(), imageRect.size() / part.m_scale),
                         Qt::AlignLeft | Qt::AlignVCenter, statusLabel(*part.m_status));
        painter.restore();
        continue;
      }
      const PageImage *page = m_pageImage ? m_pageImage(part.m_region->m_page) : nullptr;
      if (page && !page->m_image.isNull()) {
        const auto &r = part.m_region->m_rect;
        const QRect crop(QPoint(int(std::floor(r.left() * page->m_scale)),
                                int(std::floor(r.top() * page->m_scale))),
                         QPoint(int(std::ceil(r.right() * page->m_scale)) - 1,
                                int(std::ceil(r.bottom() * page->m_scale)) - 1));
        const QRect bounded = crop.intersected(page->m_image.rect());
        if (!bounded.isEmpty()) {
          const QRectF points(bounded.x() / page->m_scale, bounded.y() / page->m_scale,
                              bounded.width() / page->m_scale, bounded.height() / page->m_scale);
          const QRectF target(imageRect.x() + (points.x() - r.x()) * part.m_scale,
                              imageRect.y() + (points.y() - r.y()) * part.m_scale,
                              points.width() * part.m_scale, points.height() * part.m_scale);
          painter.setRenderHint(QPainter::SmoothPixmapTransform);
          painter.drawImage(target, page->m_image, bounded);
        }
      } else {
        painter.setPen(palette().text().color());
        painter.drawText(imageRect, Qt::AlignCenter,
                         m_failed && m_failed(part.m_region->m_page)
                             ? TypstPreviewHost::tr("Preview unavailable")
                             : TypstPreviewHost::tr("Loading preview…"));
      }
    }
  }

  void wheelEvent(QWheelEvent *p_event) override {
    // The editor owns preview scrolling.
    QWheelEvent forwarded(p_event->position() + QPointF(pos()), p_event->globalPosition(),
                          p_event->pixelDelta(), p_event->angleDelta(), p_event->buttons(),
                          p_event->modifiers(), p_event->phase(), p_event->inverted(),
                          p_event->source(), p_event->pointingDevice());
    QCoreApplication::sendEvent(parentWidget(), &forwarded);
    p_event->setAccepted(forwarded.isAccepted());
  }

private:
  int firstPart(qreal p_top) const {
    return int(
        std::lower_bound(m_parts.constBegin(), m_parts.constEnd(), p_top,
                         [](const Part &p_part, qreal p_y) { return p_part.m_maxBottom < p_y; }) -
        m_parts.constBegin());
  }

  const QVector<TypstPreviewOccurrence> *m_occurrences;
  qreal m_maxHeight = 0;
  qreal m_contentWidth = 0;
  QPointF m_contentOffset;
  QVector<Part> m_parts;
};
} // namespace

class TypstPreviewHost::Private {
public:
  struct Item {
    quint64 m_id = 0;
    TypstSourceRange m_source;
    QVector<TypstPreviewOccurrence> m_occurrences;
    qreal m_preferredWidth = 0;
    QRectF m_rect;
    QPointer<PreviewCard> m_widget;
  };
  struct Geometry {
    int m_item = 0;
    qreal m_top = 0;
    qreal m_maxBottom = 0;
  };
  struct Frame {
    ~Frame() { destroyWidgets(); }
    void destroyWidgets() {
      for (auto &item : m_items)
        delete item.m_widget.data();
      m_visible.clear();
      m_recreate.clear();
    }
    TypstSnapshot m_snapshot;
    QVector<Item> m_items;
    QVector<Geometry> m_geometry;
    QVector<TextDocumentLayout::WidgetPreviewSpec> m_specs;
    QSet<int> m_visible;
    QSet<int> m_recreate;
    QHash<int, PageImage> m_images;
    QHash<int, qreal> m_needs;
    QHash<int, qreal> m_failed;
    qint64 m_bytes = 0;
    bool m_cancelled = false;
  };
  struct Anchor {
    QTextCursor m_cursor;
    int m_y = 0;
  };

  Private(TypstPreviewHost *p_host, TypstSourceEditor *p_editor, const TypstEditorConfig &p_config)
      : q(p_host), m_edit(p_editor->getTextEdit()), m_doc(p_editor->document()),
        m_layout(p_editor->typstLayout()), m_displayed(new Frame),
        m_maxHeight(qBound<qreal>(80, p_config.m_previewMaxHeight, 100000)),
        m_budget(qMax<qint64>(4, p_config.m_maxCacheBytes / 4)),
        m_maxPixels(qMax(1, p_config.m_maxRasterPixels)) {
    m_timer.setSingleShot(true);
    QObject::connect(&m_timer, &QTimer::timeout, q, [this]() { drain(); });
  }

  bool current(const Frame &p_frame) const {
    return p_frame.m_snapshot && p_frame.m_snapshot->m_epoch == m_epoch &&
           p_frame.m_snapshot->m_generation == m_generation && !p_frame.m_cancelled;
  }

  void schedule() {
    if (m_draining || m_measuring)
      return;
    if (m_layout->isBusy()) {
      m_layout->requestIdleNotification();
    } else if (!m_timer.isActive()) {
      m_timer.start(0);
    }
  }

  Anchor captureAnchor() const {
    Anchor anchor;
    anchor.m_cursor = m_edit->cursorForPosition(QPoint(0, 0));
    anchor.m_cursor.clearSelection();
    anchor.m_y = m_edit->cursorRect(anchor.m_cursor).top();
    return anchor;
  }

  void preserveAnchor() {
    if (!m_restoreAnchor) {
      m_anchor = m_layout->isBusy() ? m_lastAnchor : captureAnchor();
      m_restoreAnchor = true;
    }
  }

  void restoreAnchor(const Anchor &p_anchor) {
    if (p_anchor.m_cursor.isNull())
      return;
    auto cursor = p_anchor.m_cursor;
    auto block = cursor.block();
    if (!block.isVisible()) {
      while (block.isValid() && !block.isVisible())
        block = block.previous();
      if (!block.isValid())
        block = m_doc->begin();
      cursor.setPosition(block.position());
    }
    const qint64 delta = qint64(m_edit->cursorRect(cursor).top()) - p_anchor.m_y;
    auto bar = m_edit->verticalScrollBar();
    bar->setValue(
        int(qBound(qint64(bar->minimum()), bar->value() + delta, qint64(bar->maximum()))));
  }

  bool validSource(const TypstSourceRange &p_source) const {
    const int limit = m_doc->characterCount() - 1;
    if (!p_source.isValid() || p_source.m_file != m_activeFile ||
        p_source.m_start >= p_source.m_end || p_source.m_end > limit)
      return false;
    const auto split = [this, limit](int p_pos) {
      return p_pos > 0 && p_pos < limit && m_doc->characterAt(p_pos).isLowSurrogate() &&
             m_doc->characterAt(p_pos - 1).isHighSurrogate();
    };
    return !split(p_source.m_start) && !split(p_source.m_end);
  }

  void buildItems(Frame &p_frame) {
    if (!p_frame.m_snapshot)
      return;
    QVector<TypstPreviewOccurrence> candidates;
    for (const auto &record : p_frame.m_snapshot->m_previews) {
      if (!validSource(record.m_source) || !record.m_id ||
          (record.m_kind != QLatin1String("figure") && record.m_kind != QLatin1String("table") &&
           record.m_kind != QLatin1String("image") && record.m_kind != QLatin1String("equation")))
        continue;
      auto occurrence = record;
      occurrence.m_regions.clear();
      if (record.m_status == QLatin1String("ready")) {
        for (const auto &region : record.m_regions) {
          if (region.m_page < 0 || region.m_page >= p_frame.m_snapshot->m_pages.size() ||
              !validRect(region.m_rect))
            continue;
          const QRectF page(QPointF(), p_frame.m_snapshot->m_pages[region.m_page]);
          if (!validRect(page))
            continue;
          const QRectF rect = region.m_rect.intersected(page);
          if (validRect(rect))
            occurrence.m_regions.append({region.m_page, rect});
        }
      }
      candidates.append(occurrence);
    }
    // Choose outer ownership before source order. Equal source ranges collect
    // repeated evaluations; no source text or regex is involved.
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const TypstPreviewOccurrence &a, const TypstPreviewOccurrence &b) {
                       const int orderA = typeOrder(a.m_kind), orderB = typeOrder(b.m_kind);
                       if (orderA != orderB)
                         return orderA < orderB;
                       if (a.m_source.m_start != b.m_source.m_start)
                         return a.m_source.m_start < b.m_source.m_start;
                       return a.m_source.m_end > b.m_source.m_end;
                     });
    QMap<int, int> accepted;
    for (const auto &occurrence : candidates) {
      auto next = accepted.lowerBound(occurrence.m_source.m_start);
      if (next != accepted.end() && next.key() == occurrence.m_source.m_start) {
        auto &item = p_frame.m_items[next.value()];
        const auto &first = item.m_occurrences.first();
        if (item.m_source.m_end == occurrence.m_source.m_end && first.m_kind == occurrence.m_kind &&
            first.m_block == occurrence.m_block) {
          item.m_occurrences.append(occurrence);
        }
        continue;
      }
      if (next != accepted.end() && next.key() < occurrence.m_source.m_end)
        continue;
      if (next != accepted.begin()) {
        auto previous = next;
        --previous;
        if (p_frame.m_items[previous.value()].m_source.m_end > occurrence.m_source.m_start)
          continue;
      }
      Item item;
      item.m_id = occurrence.m_id;
      item.m_source = occurrence.m_source;
      item.m_occurrences.append(occurrence);
      accepted.insert(item.m_source.m_start, p_frame.m_items.size());
      p_frame.m_items.append(item);
    }
    std::sort(p_frame.m_items.begin(), p_frame.m_items.end(),
              [](const Item &a, const Item &b) { return a.m_source.m_start < b.m_source.m_start; });
  }

  void buildSpecs(Frame &p_frame) {
    auto &specs = p_frame.m_specs;
    specs.clear();
    if (m_enabled && p_frame.m_snapshot) {
      specs.reserve(p_frame.m_items.size());
      const qreal contentWidth = m_layout->availableContentWidth();
      const bool wrapped = m_doc->defaultTextOption().wrapMode() != QTextOption::NoWrap;
      qreal available = contentWidth;
      if (available <= 0)
        available = m_edit->viewport()->width() - 2 * m_doc->documentMargin();
      available = qMax<qreal>(1, available);
      const qreal dpiScale = m_edit->logicalDpiX() / 72.0;
      for (auto &item : p_frame.m_items) {
        if (!item.m_source.isValid())
          continue;
        const auto &first = item.m_occurrences.first();
        TextDocumentLayout::WidgetPreviewSpec spec;
        spec.m_id = item.m_id;
        spec.m_startPos = item.m_source.m_start;
        spec.m_endPos = item.m_source.m_end;
        spec.m_typeOrder = typeOrder(first.m_kind);
        spec.m_placement = isInlinePreview(first) ? PreviewPlacement::InlineAboveLine
                                                  : PreviewPlacement::BlockAfterSource;
        qreal width = available;
        qreal spanWidth = 0;
        if (spec.m_placement == PreviewPlacement::InlineAboveLine) {
          qreal startX = 0;
          spanWidth = m_layout->inlinePlacementWidth(spec.m_startPos, spec.m_endPos, &startX);
          if (wrapped && contentWidth > 0) {
            width = qMax<qreal>(1, available - qMax<qreal>(0, startX - m_doc->documentMargin()));
          }
        }
        QSizeF size;
        partsFor(item.m_occurrences, width, dpiScale, m_maxHeight, m_edit->font(), &size, true);
        spec.m_width = size.width();
        spec.m_height = size.height();
        item.m_preferredWidth = 0;
        if (spec.m_placement == PreviewPlacement::InlineAboveLine) {
          spec.m_fullWidth = spec.m_width > spanWidth;
          item.m_preferredWidth = spec.m_width;
        }
        specs.append(spec);
      }
    }
  }

  void rebuildGeometry(Frame &p_frame) {
    p_frame.m_geometry.clear();
    for (int i = 0; i < p_frame.m_items.size(); ++i) {
      auto &item = p_frame.m_items[i];
      item.m_rect = m_enabled ? m_layout->widgetPreviewRect(item.m_id) : QRectF();
      // The shared layout reserves a source span or an exclusive line band;
      // the inline image card itself needs only its measured content width.
      if (item.m_preferredWidth > 0)
        item.m_rect.setWidth(qMin(item.m_rect.width(), item.m_preferredWidth));
      if (!item.m_rect.isEmpty())
        p_frame.m_geometry.append({i, item.m_rect.top(), item.m_rect.bottom()});
    }
    std::sort(p_frame.m_geometry.begin(), p_frame.m_geometry.end(),
              [](const Geometry &a, const Geometry &b) { return a.m_top < b.m_top; });
    qreal bottom = 0;
    for (auto &entry : p_frame.m_geometry) {
      bottom = qMax(bottom, entry.m_maxBottom);
      entry.m_maxBottom = bottom;
    }
  }

  template <typename Visitor> void visitVisible(Frame &p_frame, Visitor p_visit) {
    auto viewport = m_edit->viewport();
    if (!m_enabled || !p_frame.m_snapshot || !viewport->isVisible())
      return;
    const QPoint offset(m_edit->horizontalScrollBar()->value(),
                        m_edit->verticalScrollBar()->value());
    const QRect view = viewport->rect().translated(offset);
    const auto first = std::lower_bound(
        p_frame.m_geometry.constBegin(), p_frame.m_geometry.constEnd(), view.top(),
        [](const Geometry &p_entry, qreal p_y) { return p_entry.m_maxBottom < p_y; });
    for (auto it = first; it != p_frame.m_geometry.constEnd() && it->m_top <= view.bottom(); ++it) {
      auto &item = p_frame.m_items[it->m_item];
      if (!item.m_rect.intersects(view))
        continue;
      const QRectF full = item.m_rect.translated(-offset);
      // Clip before integer conversion, including on multi-page reservations.
      const QRect target = full.intersected(QRectF(viewport->rect())).toAlignedRect();
      p_visit(it->m_item, item, target, full);
    }
  }

  void visibleWidgets() {
    auto &frame = *m_displayed;
    QSet<int> visible;
    frame.m_needs.clear();
    visitVisible(frame, [&](int p_index, Item &p_item, const QRect &p_target,
                            const QRectF &p_full) {
      if (!p_item.m_widget) {
        auto widget = new PreviewCard(m_edit->viewport(), &p_item.m_occurrences, m_maxHeight);
        widget->m_snapshot = frame.m_snapshot;
        widget->m_host = q;
        widget->setFont(m_edit->font());
        widget->setPalette(m_edit->palette());
        widget->m_pageImage = [this](int p_page) -> const PageImage * {
          auto image = m_displayed->m_images.find(p_page);
          if (image == m_displayed->m_images.end())
            return nullptr;
          image->m_use = ++m_use;
          return &image.value();
        };
        widget->m_failed = [this](int p_page) { return m_displayed->m_failed.contains(p_page); };
        p_item.m_widget = widget;
      }
      auto widget = p_item.m_widget.data();
      if (widget->geometry() != p_target)
        widget->setGeometry(p_target);
      widget->setContentGeometry(p_full.width(), QPointF(p_target.topLeft()) - p_full.topLeft());
      widget->show();
      widget->appendNeeds(frame.m_needs, m_edit->viewport()->devicePixelRatioF());
      visible.insert(p_index);
    });
    for (int index : frame.m_visible) {
      if (!visible.contains(index) && index < frame.m_items.size())
        delete frame.m_items[index].m_widget.data();
    }
    frame.m_visible = std::move(visible);
    constrainNeeds(frame);
  }

  void pendingNeeds() {
    auto &frame = *m_pending;
    frame.m_needs.clear();
    visitVisible(frame, [&](int, Item &p_item, const QRect &p_target, const QRectF &p_full) {
      QSizeF size;
      const auto parts = partsFor(p_item.m_occurrences, p_full.width(),
                                  m_edit->logicalDpiX() / 72.0, m_maxHeight, m_edit->font(), &size);
      const QRectF clip(QPointF(p_target.topLeft()) - p_full.topLeft(), p_target.size());
      appendPartNeeds(parts, clip, frame.m_needs, m_edit->viewport()->devicePixelRatioF());
    });
    constrainNeeds(frame);
  }

  qint64 cacheBudget(const Frame &p_frame) const {
    // Reserve room for a replacement without evicting the displayed pages.
    if (m_budget >= 8)
      return m_budget / 2;
    return &p_frame == m_pending.get() && m_displayed->m_bytes ? 0 : m_budget;
  }

  void constrainNeeds(Frame &p_frame) {
    const qreal pixels = std::floor(qMin<qreal>(
        m_maxPixels, cacheBudget(p_frame) / (4.0 * qMax(1, int(p_frame.m_needs.size())))));
    for (auto it = p_frame.m_needs.begin(); it != p_frame.m_needs.end(); ++it) {
      if (pixels < 1) {
        it.value() = 0;
        if (!p_frame.m_failed.contains(it.key()))
          m_errorPending = true;
        p_frame.m_failed.insert(it.key(), 0);
        continue;
      }
      const auto page = p_frame.m_snapshot->m_pages[it.key()];
      const qreal longSide = qMax(page.width(), page.height());
      const qreal ratio = qMin(page.width(), page.height()) / longSide;
      // Solve (longPixels + 1) * (shortPixels + 1) <= the share of
      // the cache. This includes outward rounding even on very thin pages.
      const qreal extent =
          pixels == 1
              ? 0.5
              : 2 * (pixels - 1) /
                    (1 + ratio + std::sqrt((1 + ratio) * (1 + ratio) + 4 * ratio * (pixels - 1)));
      it.value() = qMin(it.value(), qMin<qreal>(32768, extent) / longSide);
    }
  }

  void evict(Frame &p_frame, qint64 p_incoming = 0, int p_replace = -1) {
    auto replaced = p_frame.m_images.find(p_replace);
    if (replaced != p_frame.m_images.end()) {
      p_frame.m_bytes -= qint64(replaced->m_image.bytesPerLine()) * replaced->m_image.height();
      p_frame.m_images.erase(replaced);
    }
    while (!p_frame.m_images.isEmpty() && p_frame.m_bytes + p_incoming > cacheBudget(p_frame)) {
      auto oldest = p_frame.m_images.begin();
      for (auto it = p_frame.m_images.begin(); it != p_frame.m_images.end(); ++it) {
        if ((!p_frame.m_needs.contains(it.key()) && p_frame.m_needs.contains(oldest.key())) ||
            (p_frame.m_needs.contains(it.key()) == p_frame.m_needs.contains(oldest.key()) &&
             it->m_use < oldest->m_use))
          oldest = it;
      }
      p_frame.m_bytes -= qint64(oldest->m_image.bytesPerLine()) * oldest->m_image.height();
      p_frame.m_images.erase(oldest);
    }
  }

  bool pendingReady() const {
    if (!m_pending || !current(*m_pending))
      return false;
    for (auto it = m_pending->m_needs.constBegin(); it != m_pending->m_needs.constEnd(); ++it) {
      const auto image = m_pending->m_images.constFind(it.key());
      if (image != m_pending->m_images.constEnd() && image->m_scale >= it.value() * 0.999)
        continue;
      // Initial rendering may show a real failure. Never replace last-good pixels with it.
      if (m_displayed->m_images.isEmpty() && m_pending->m_failed.contains(it.key()) &&
          qFuzzyCompare(m_pending->m_failed.value(it.key()), it.value()))
        continue;
      return false;
    }
    return true;
  }

  void measurePending() {
    const auto anchor = captureAnchor();
    const int horizontal = m_edit->horizontalScrollBar()->value();
    m_measuring = true;
    m_pendingViewDirty = false;
    // Geometry comes from the same layout as the displayed cards, not source-position
    // guesses. No events or pending widgets are dispatched in this temporary pass.
    buildSpecs(*m_pending);
    auto width = m_edit->viewport()->width();
    m_layout->setWidgetPreviews(m_pending->m_specs);
    if (width != m_edit->viewport()->width()) {
      buildSpecs(*m_pending);
      m_layout->setWidgetPreviews(m_pending->m_specs);
    }
    restoreAnchor(anchor);
    m_edit->horizontalScrollBar()->setValue(horizontal);
    rebuildGeometry(*m_pending);
    pendingNeeds();
    if (pendingReady()) {
      // Reservations, source mapping and their own page images change together.
      m_displayed = std::move(m_pending);
      m_geometryDirty = false;
    } else {
      // Every non-publishing path restores the old reservations before any paint.
      m_layout->setWidgetPreviews(m_displayed->m_specs);
      restoreAnchor(anchor);
      m_edit->horizontalScrollBar()->setValue(horizontal);
    }
    m_measuring = false;
    if (!m_pending)
      emit q->snapshotPublished(m_displayed->m_snapshot);
  }

  void requestImage() {
    if (m_inFlight || !m_enabled || !m_edit->viewport()->isVisible())
      return;
    auto &frame = m_pending ? *m_pending : *m_displayed;
    if (!current(frame))
      return;
    auto pages = frame.m_needs.keys();
    std::sort(pages.begin(), pages.end());
    for (int page : pages) {
      const qreal scale = frame.m_needs.value(page);
      if (!(scale > 0) || !std::isfinite(scale))
        continue;
      const auto image = frame.m_images.constFind(page);
      if (image != frame.m_images.constEnd() && image->m_scale >= scale * 0.999)
        continue;
      if (frame.m_failed.contains(page) && qFuzzyCompare(frame.m_failed.value(page), scale))
        continue;
      frame.m_failed.remove(page);
      m_inFlight = true;
      m_requestEpoch = frame.m_snapshot->m_epoch;
      m_requestSnapshot = frame.m_snapshot->m_id;
      m_requestPage = page;
      m_requestScale = scale;
      emit q->rasterRequested(m_requestEpoch, m_requestSnapshot, page, scale);
      return;
    }
  }

  bool finishRequest(quint64 p_epoch, quint64 p_snapshot, int p_page, qreal p_scale) {
    if (!m_inFlight || p_epoch != m_requestEpoch || p_snapshot != m_requestSnapshot ||
        p_page != m_requestPage || !qFuzzyCompare(p_scale, m_requestScale))
      return false;
    m_inFlight = false;
    schedule();
    return true;
  }

  Frame *frameFor(quint64 p_epoch, quint64 p_snapshot) const {
    for (auto frame : {m_pending.get(), m_displayed.get()}) {
      if (frame && frame->m_snapshot && frame->m_snapshot->m_epoch == p_epoch &&
          frame->m_snapshot->m_id == p_snapshot)
        return frame;
    }
    return nullptr;
  }

  void drain() {
    if (m_layout->isBusy()) {
      m_layout->requestIdleNotification();
      return;
    }
    m_draining = true;
    for (int index : m_displayed->m_recreate) {
      if (index < m_displayed->m_items.size())
        delete m_displayed->m_items[index].m_widget.data();
    }
    m_displayed->m_recreate.clear();
    if (m_publish) {
      m_publish = false;
      if (!m_restoreAnchor)
        preserveAnchor();
      buildSpecs(*m_displayed);
      m_layout->setWidgetPreviews(m_displayed->m_specs);
      m_geometryDirty = true;
    }
    if (m_geometryDirty) {
      rebuildGeometry(*m_displayed);
      m_geometryDirty = false;
      m_pendingViewDirty = true;
    }
    if (m_restoreAnchor) {
      m_restoreAnchor = false;
      restoreAnchor(m_anchor);
    }
    const QPointer<TypstPreviewHost> alive(q);
    if (m_pending && m_enabled && m_edit->viewport()->isVisible() &&
        (m_pendingViewDirty || pendingReady()))
      measurePending();
    if (!alive)
      return;
    visibleWidgets();
    m_lastAnchor = captureAnchor();
    m_draining = false;
    if (m_publish || m_geometryDirty || !m_displayed->m_recreate.isEmpty())
      schedule();
    requestImage();
    if (!alive)
      return;
    if (m_errorPending) {
      m_errorPending = false;
      emit q->errorOccurred(TypstPreviewHost::tr("Preview unavailable"));
    }
  }

  TypstPreviewHost *q;
  VTextEdit *m_edit;
  QTextDocument *m_doc;
  TextDocumentLayout *m_layout;
  QTimer m_timer;
  std::unique_ptr<Frame> m_displayed;
  std::unique_ptr<Frame> m_pending;
  QString m_activeFile;
  Anchor m_anchor;
  Anchor m_lastAnchor;
  qreal m_maxHeight;
  qint64 m_budget;
  int m_maxPixels;
  quint64 m_use = 0;
  quint64 m_epoch = 0;
  quint64 m_generation = 0;
  bool m_enabled = true;
  bool m_draining = false;
  bool m_measuring = false;
  bool m_pendingViewDirty = false;
  bool m_publish = false;
  bool m_geometryDirty = false;
  bool m_restoreAnchor = false;
  bool m_inFlight = false;
  bool m_errorPending = false;
  quint64 m_requestEpoch = 0;
  quint64 m_requestSnapshot = 0;
  int m_requestPage = -1;
  qreal m_requestScale = 0;
};

TypstPreviewHost::TypstPreviewHost(TypstSourceEditor *p_editor, const TypstEditorConfig &p_config,
                                   QObject *p_parent)
    : QObject(p_parent), d(new Private(this, p_editor, p_config)) {
  connect(d->m_layout, &TextDocumentLayout::becameIdle, this, [this]() {
    // This signal runs while the outermost layout pass is unwinding.
    if (!d->m_measuring && !d->m_timer.isActive())
      d->m_timer.start(0);
  });
  connect(d->m_layout, &TextDocumentLayout::widgetPreviewGeometryChanged, this, [this]() {
    if (d->m_measuring)
      return;
    d->m_geometryDirty = true;
    d->schedule();
  });
  connect(d->m_doc, &QTextDocument::contentsChange, this,
          [this](int p_position, int p_removed, int p_added) {
            if (!p_removed && !p_added)
              return;
            // Qt temporarily detaches its cursor registry while clear() inserts an empty
            // paragraph. Releasing anchors there leaves dangling entries in the saved registry.
            // The following removal notification is the safe reset point.
            if (!d->m_doc->isUndoRedoEnabled() && p_position == 0 && p_removed == 0 &&
                p_added == 1 && d->m_doc->characterCount() == 1)
              return;
            // clear()/setPlainText include the terminal paragraph in their removal/insertion.
            // Viewport cursors from the old document otherwise drift to the end of the new one.
            const bool cleared =
                p_position == 0 && p_removed > 0 && p_added == 0 && d->m_doc->characterCount() == 1;
            const bool loaded =
                p_position == 0 && p_removed == 0 && p_added == d->m_doc->characterCount();
            if (cleared || loaded) {
              d->m_anchor = {};
              d->m_lastAnchor = {};
              d->m_restoreAnchor = false;
            }
            d->m_pending.reset();
            const int removedEnd = p_position + p_removed;
            const int delta = p_added - p_removed;
            for (auto &item : d->m_displayed->m_items) {
              auto &source = item.m_source;
              if (!source.isValid())
                continue;
              if (p_removed && p_position <= source.m_start && removedEnd >= source.m_end) {
                // Keep slots stable: live cards hold pointers into their item.
                delete item.m_widget.data();
                source = {};
                continue;
              }
              if (source.m_end > p_position) {
                if (source.m_start >= removedEnd) {
                  source.m_start += delta;
                  source.m_end += delta;
                } else {
                  source.m_start = qMin(source.m_start, p_position);
                  source.m_end = qMax(p_position + p_added, source.m_end + delta);
                }
              }
            }
            // Remap reservations only when the document/layout stack is idle.
            // Surviving widgets and their page images remain available meanwhile.
            d->m_publish = true;
            d->schedule();
          });
  connect(d->m_edit->verticalScrollBar(), &QScrollBar::valueChanged, this, [this]() {
    if (d->m_measuring)
      return;
    d->m_pendingViewDirty = true;
    // A deliberate scroll supersedes an anchor saved before deferred work.
    if (!d->m_draining && !d->m_layout->isBusy() && !d->m_restoreAnchor)
      d->m_lastAnchor = d->captureAnchor();
    d->schedule();
  });
  connect(d->m_edit, &QTextEdit::cursorPositionChanged, this, [this]() {
    // Explicit caret navigation (including session restore) wins over deferred layout work.
    // Layout/range clamping does not move the caret, and our own restoration is guarded.
    if (d->m_enabled && d->m_restoreAnchor && !d->m_draining && !d->m_measuring &&
        !d->m_layout->isBusy()) {
      d->m_anchor.m_cursor = d->m_edit->textCursor();
      d->m_anchor.m_cursor.clearSelection();
      const auto rect = d->m_edit->cursorRect();
      d->m_anchor.m_y =
          qBound(0, rect.top(), qMax(0, d->m_edit->viewport()->height() - rect.height()));
      d->m_lastAnchor = d->m_anchor;
    }
  });
  connect(d->m_edit->horizontalScrollBar(), &QScrollBar::valueChanged, this, [this]() {
    if (d->m_measuring)
      return;
    d->m_pendingViewDirty = true;
    d->schedule();
  });
  connect(p_editor->getTextFolding(), &TextFolding::foldingRangesChanged, this, [this]() {
    if (d->m_measuring)
      return;
    if (!d->m_restoreAnchor) {
      d->m_anchor = d->m_lastAnchor;
      d->m_restoreAnchor = true;
    }
    d->m_geometryDirty = true;
    d->schedule();
  });
  d->m_edit->installEventFilter(this);
  d->m_edit->viewport()->installEventFilter(this);
  d->m_lastAnchor = d->captureAnchor();
}

TypstPreviewHost::~TypstPreviewHost() {
  d->m_timer.stop();
  disconnect(d->m_layout, nullptr, this, nullptr);
  // The outer editor destroys its host before its source editor/document.
  if (!d->m_layout->isBusy())
    d->m_layout->setWidgetPreviews({});
}

void TypstPreviewHost::setCurrentGeneration(quint64 p_epoch, quint64 p_generation) {
  d->m_epoch = p_epoch;
  d->m_generation = p_generation;
  if (d->m_pending && !d->current(*d->m_pending))
    d->m_pending.reset();
  d->m_errorPending = false;
  d->schedule();
}

void TypstPreviewHost::setSnapshot(const TypstSnapshot &p_snapshot) {
  if (!p_snapshot) {
    clear();
    return;
  }
  if (p_snapshot->m_epoch != d->m_epoch || p_snapshot->m_generation != d->m_generation ||
      d->m_displayed->m_snapshot == p_snapshot ||
      (d->m_pending && d->m_pending->m_snapshot == p_snapshot))
    return;
  if (d->m_activeFile.isEmpty())
    d->m_activeFile = p_snapshot->m_activeFile;
  d->m_pending.reset(new Private::Frame);
  d->m_pending->m_snapshot = p_snapshot;
  d->buildItems(*d->m_pending);
  d->m_pendingViewDirty = true;
  d->schedule();
}

TypstSnapshot TypstPreviewHost::snapshot() const { return d->m_displayed->m_snapshot; }

void TypstPreviewHost::clear() {
  d->preserveAnchor();
  d->m_pending.reset();
  d->m_displayed.reset(new Private::Frame);
  // The outstanding request still completes through its epoch-qualified latch.
  d->m_publish = true;
  d->m_errorPending = false;
  d->schedule();
  emit snapshotPublished({});
}

void TypstPreviewHost::setEnabled(bool p_enabled) {
  if (d->m_enabled == p_enabled)
    return;
  d->preserveAnchor();
  d->m_enabled = p_enabled;
  if (!p_enabled)
    d->m_displayed->destroyWidgets();
  d->m_pendingViewDirty = true;
  d->m_publish = true;
  d->schedule();
}

void TypstPreviewHost::setActiveFile(const QString &p_file) {
  if (d->m_activeFile == p_file)
    return;
  clear();
  d->m_activeFile = p_file;
}

void TypstPreviewHost::setPageImage(quint64 p_epoch, quint64 p_snapshot, int p_page, qreal p_scale,
                                    const QImage &p_image) {
  if (!d->finishRequest(p_epoch, p_snapshot, p_page, p_scale))
    return;
  auto frame = d->frameFor(p_epoch, p_snapshot);
  if (!frame || !d->current(*frame))
    return;
  const qint64 bytes = qint64(p_image.bytesPerLine()) * p_image.height();
  const auto pageSize = frame->m_snapshot->m_pages[p_page];
  if (p_image.isNull() || bytes > d->cacheBudget(*frame) ||
      qint64(p_image.width()) * p_image.height() > d->m_maxPixels ||
      p_image.width() > qMax(1, qCeil(pageSize.width() * p_scale)) ||
      p_image.height() > qMax(1, qCeil(pageSize.height() * p_scale))) {
    frame->m_failed.insert(p_page, p_scale);
    d->m_errorPending = true;
  } else {
    d->evict(*frame, bytes, p_page);
    frame->m_images.insert(p_page, {p_image, p_scale, ++d->m_use});
    frame->m_bytes += bytes;
  }
  for (int index : frame->m_visible) {
    if (index < frame->m_items.size() && frame->m_items[index].m_widget)
      frame->m_items[index].m_widget->update();
  }
}

void TypstPreviewHost::cancelRaster(quint64 p_epoch, quint64 p_snapshot, int p_page,
                                    qreal p_scale) {
  if (!d->finishRequest(p_epoch, p_snapshot, p_page, p_scale))
    return;
  // Superseded transport is not a failed render and must never start a retry loop.
  auto frame = d->frameFor(p_epoch, p_snapshot);
  if (frame)
    frame->m_cancelled = true;
  if (frame == d->m_pending.get())
    d->m_pending.reset();
}

void TypstPreviewHost::applyConfig(const TypstEditorConfig &p_config) {
  d->preserveAnchor();
  d->m_maxHeight = qBound<qreal>(80, p_config.m_previewMaxHeight, 100000);
  d->m_budget = qMax<qint64>(4, p_config.m_maxCacheBytes / 4);
  d->m_maxPixels = qMax(1, p_config.m_maxRasterPixels);
  for (auto frame : {d->m_displayed.get(), d->m_pending.get()}) {
    if (!frame)
      continue;
    frame->m_failed.clear();
    for (auto it = frame->m_images.begin(); it != frame->m_images.end();) {
      if (qint64(it->m_image.width()) * it->m_image.height() > d->m_maxPixels) {
        frame->m_bytes -= qint64(it->m_image.bytesPerLine()) * it->m_image.height();
        it = frame->m_images.erase(it);
      } else {
        ++it;
      }
    }
    d->evict(*frame);
    for (int index : frame->m_visible)
      frame->m_recreate.insert(index);
  }
  d->m_pendingViewDirty = true;
  d->m_publish = true;
  d->schedule();
}

bool TypstPreviewHost::eventFilter(QObject *p_object, QEvent *p_event) {
  // Disabled cards must not restore a pre-navigation anchor when source is shown/resized.
  if (d->m_enabled && !d->m_measuring &&
      (p_object == d->m_edit || p_object == d->m_edit->viewport())) {
    switch (p_event->type()) {
    case QEvent::Resize:
    case QEvent::FontChange:
    case QEvent::StyleChange:
    case QEvent::PaletteChange:
    case QEvent::ScreenChangeInternal:
      if (!d->m_restoreAnchor) {
        d->m_anchor = d->m_lastAnchor;
        d->m_restoreAnchor = true;
      }
      d->m_publish = true;
      for (int index : d->m_displayed->m_visible)
        d->m_displayed->m_recreate.insert(index);
      d->m_pendingViewDirty = true;
      d->schedule();
      break;
    case QEvent::Show:
    case QEvent::Hide:
      d->m_pendingViewDirty = true;
      d->schedule();
      break;
    default:
      break;
    }
  }
  return QObject::eventFilter(p_object, p_event);
}
