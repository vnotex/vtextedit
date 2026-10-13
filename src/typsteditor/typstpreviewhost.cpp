#include "typstpreviewhost.h"

#include <QCoreApplication>
#include <QEvent>
#include <QFontMetricsF>
#include <QMap>
#include <QPainter>
#include <QPaintEvent>
#include <QPointer>
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
        parts.append({nullptr, &occurrence.m_status,
                      QRectF(left, top, columnWidth, statusHeight), 1});
      }
      top += statusHeight + c_padding;
    } else {
      hasFragments |= occurrence.m_regions.size() > 1;
      for (const auto &region : occurrence.m_regions) {
        qreal scale = qMin(p_dpiScale, qMax<qreal>(1, p_maxHeight - 2 * c_padding) /
                                          region.m_rect.height());
        if (!inlinePreview)
          scale = qMin(scale, innerWidth / region.m_rect.width());
        const QRectF rect(left, top, region.m_rect.width() * scale,
                          region.m_rect.height() * scale);
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
    const QRectF clip = QRectF(rect()).translated(m_contentOffset);
    for (int i = firstPart(clip.top()); i < m_parts.size(); ++i) {
      const auto &part = m_parts[i];
      if (part.m_imageRect.top() > clip.bottom())
        break;
      if (part.m_region && part.m_imageRect.intersects(clip)) {
        p_needs[part.m_region->m_page] =
            qMax(p_needs.value(part.m_region->m_page), part.m_scale * p_dpr);
      }
    }
  }

  std::function<const PageImage *(int)> m_pageImage;
  std::function<bool(int)> m_failed;

protected:
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
    // Clicks remain passive, but the editor owns all preview scrolling.
    QWheelEvent forwarded(p_event->position() + QPointF(pos()), p_event->globalPosition(),
                           p_event->pixelDelta(), p_event->angleDelta(), p_event->buttons(),
                           p_event->modifiers(), p_event->phase(), p_event->inverted(),
                           p_event->source(), p_event->pointingDevice());
    QCoreApplication::sendEvent(parentWidget(), &forwarded);
    p_event->setAccepted(forwarded.isAccepted());
  }

private:
  int firstPart(qreal p_top) const {
    return int(std::lower_bound(m_parts.constBegin(), m_parts.constEnd(), p_top,
                                [](const Part &p_part, qreal p_y) {
                                  return p_part.m_maxBottom < p_y;
                                }) -
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
  struct Anchor {
    QTextCursor m_cursor;
    int m_y = 0;
  };

  Private(TypstPreviewHost *p_host, TypstSourceEditor *p_editor, const TypstEditorConfig &p_config)
      : q(p_host), m_edit(p_editor->getTextEdit()), m_doc(p_editor->document()),
        m_layout(p_editor->typstLayout()),
        m_maxHeight(qBound<qreal>(80, p_config.m_previewMaxHeight, 100000)),
        m_budget(qMax<qint64>(4, p_config.m_maxCacheBytes / 4)),
        m_maxPixels(qMax(1, p_config.m_maxRasterPixels)) {
    m_timer.setSingleShot(true);
    QObject::connect(&m_timer, &QTimer::timeout, q, [this]() { drain(); });
  }

  ~Private() { destroyWidgets(); }

  void destroyWidgets() {
    for (auto &item : m_items)
      delete item.m_widget.data();
    m_visible.clear();
  }

  void schedule() {
    if (m_draining)
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

  void restoreAnchor() {
    m_restoreAnchor = false;
    if (m_anchor.m_cursor.isNull())
      return;
    auto cursor = m_anchor.m_cursor;
    auto block = cursor.block();
    if (!block.isVisible()) {
      while (block.isValid() && !block.isVisible())
        block = block.previous();
      if (!block.isValid())
        block = m_doc->begin();
      cursor.setPosition(block.position());
    }
    const qint64 delta = qint64(m_edit->cursorRect(cursor).top()) - m_anchor.m_y;
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

  void buildItems() {
    destroyWidgets();
    m_items.clear();
    m_geometry.clear();
    if (!m_snapshot)
      return;
    QVector<TypstPreviewOccurrence> candidates;
    for (const auto &record : m_snapshot->m_previews) {
      if (!validSource(record.m_source) || !record.m_id ||
          (record.m_kind != QLatin1String("figure") && record.m_kind != QLatin1String("table") &&
           record.m_kind != QLatin1String("image") && record.m_kind != QLatin1String("equation")))
        continue;
      auto occurrence = record;
      occurrence.m_regions.clear();
      if (record.m_status == QLatin1String("ready")) {
        for (const auto &region : record.m_regions) {
          if (region.m_page < 0 || region.m_page >= m_snapshot->m_pages.size() ||
              !validRect(region.m_rect))
            continue;
          const QRectF page(QPointF(), m_snapshot->m_pages[region.m_page]);
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
        auto &item = m_items[next.value()];
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
        if (m_items[previous.value()].m_source.m_end > occurrence.m_source.m_start)
          continue;
      }
      Item item;
      item.m_id = occurrence.m_id;
      item.m_source = occurrence.m_source;
      item.m_occurrences.append(occurrence);
      accepted.insert(item.m_source.m_start, m_items.size());
      m_items.append(item);
    }
    std::sort(m_items.begin(), m_items.end(),
              [](const Item &a, const Item &b) { return a.m_source.m_start < b.m_source.m_start; });
  }

  void publish() {
    QVector<TextDocumentLayout::WidgetPreviewSpec> specs;
    if (m_enabled && m_snapshot) {
      specs.reserve(m_items.size());
      const qreal contentWidth = m_layout->availableContentWidth();
      const bool wrapped = m_doc->defaultTextOption().wrapMode() != QTextOption::NoWrap;
      qreal available = contentWidth;
      if (available <= 0)
        available = m_edit->viewport()->width() - 2 * m_doc->documentMargin();
      available = qMax<qreal>(1, available);
      const qreal dpiScale = m_edit->logicalDpiX() / 72.0;
      for (auto &item : m_items) {
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
    m_layout->setWidgetPreviews(specs);
  }

  void rebuildGeometry() {
    m_geometry.clear();
    for (int i = 0; i < m_items.size(); ++i) {
      auto &item = m_items[i];
      item.m_rect = m_enabled ? m_layout->widgetPreviewRect(item.m_id) : QRectF();
      // The shared layout reserves a source span or an exclusive line band;
      // the inline image card itself needs only its measured content width.
      if (item.m_preferredWidth > 0)
        item.m_rect.setWidth(qMin(item.m_rect.width(), item.m_preferredWidth));
      if (!item.m_rect.isEmpty())
        m_geometry.append({i, item.m_rect.top(), item.m_rect.bottom()});
    }
    std::sort(m_geometry.begin(), m_geometry.end(),
              [](const Geometry &a, const Geometry &b) { return a.m_top < b.m_top; });
    qreal bottom = 0;
    for (auto &entry : m_geometry) {
      bottom = qMax(bottom, entry.m_maxBottom);
      entry.m_maxBottom = bottom;
    }
  }

  PreviewCard *createWidget(int p_index) {
    auto &item = m_items[p_index];
    auto widget = new PreviewCard(m_edit->viewport(), &item.m_occurrences, m_maxHeight);
    widget->setFont(m_edit->font());
    widget->setPalette(m_edit->palette());
    widget->m_pageImage = [this](int p_page) -> const PageImage * {
      auto image = m_images.find(p_page);
      if (image == m_images.end())
        return nullptr;
      image->m_use = ++m_use;
      return &image.value();
    };
    widget->m_failed = [this](int p_page) { return m_failed.contains(p_page); };
    return widget;
  }

  void visibleWidgets() {
    QSet<int> visible;
    m_needs.clear();
    auto viewport = m_edit->viewport();
    if (m_enabled && m_snapshot && viewport->isVisible()) {
      const QPoint offset(m_edit->horizontalScrollBar()->value(),
                          m_edit->verticalScrollBar()->value());
      const QRect view = viewport->rect().translated(offset);
      const auto first =
          std::lower_bound(m_geometry.constBegin(), m_geometry.constEnd(), view.top(),
                           [](const Geometry &entry, qreal y) { return entry.m_maxBottom < y; });
      for (auto it = first; it != m_geometry.constEnd() && it->m_top <= view.bottom(); ++it) {
        auto &item = m_items[it->m_item];
        if (!item.m_rect.intersects(view))
          continue;
        if (!item.m_widget)
          item.m_widget = createWidget(it->m_item);
        auto widget = item.m_widget.data();
        const QRectF full = item.m_rect.translated(-offset);
        // Intersect before integer conversion: the sequence may be much taller
        // than a QWidget, but its painted surface never exceeds the viewport.
        const QRect target = full.intersected(QRectF(viewport->rect())).toAlignedRect();
        if (widget->geometry() != target)
          widget->setGeometry(target);
        widget->setContentGeometry(full.width(), QPointF(target.topLeft()) - full.topLeft());
        widget->show();
        widget->appendNeeds(m_needs, viewport->devicePixelRatioF());
        visible.insert(it->m_item);
      }
    }
    for (int index : m_visible) {
      if (!visible.contains(index) && index < m_items.size())
        delete m_items[index].m_widget.data();
    }
    m_visible = visible;
    constrainNeeds();
  }

  void constrainNeeds() {
    const qreal pixels =
        std::floor(qMin<qreal>(m_maxPixels, m_budget / (4.0 * qMax(1, int(m_needs.size())))));
    for (auto it = m_needs.begin(); it != m_needs.end(); ++it) {
      if (pixels < 1) {
        it.value() = 0;
        m_failed.insert(it.key(), 0);
        continue;
      }
      const auto page = m_snapshot->m_pages[it.key()];
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

  void evict(qint64 p_incoming = 0, int p_replace = -1) {
    auto replaced = m_images.find(p_replace);
    if (replaced != m_images.end()) {
      m_bytes -= qint64(replaced->m_image.bytesPerLine()) * replaced->m_image.height();
      m_images.erase(replaced);
    }
    while (!m_images.isEmpty() && m_bytes + p_incoming > m_budget) {
      auto oldest = m_images.begin();
      for (auto it = m_images.begin(); it != m_images.end(); ++it) {
        if ((!m_needs.contains(it.key()) && m_needs.contains(oldest.key())) ||
            (m_needs.contains(it.key()) == m_needs.contains(oldest.key()) &&
             it->m_use < oldest->m_use))
          oldest = it;
      }
      m_bytes -= qint64(oldest->m_image.bytesPerLine()) * oldest->m_image.height();
      m_images.erase(oldest);
    }
  }

  void requestImage() {
    if (m_inFlight || !m_snapshot)
      return;
    // Iteration is page ordered, making scheduling deterministic as well as coalesced.
    auto pages = m_needs.keys();
    std::sort(pages.begin(), pages.end());
    for (int page : pages) {
      const qreal scale = m_needs.value(page);
      if (!(scale > 0) || !std::isfinite(scale))
        continue;
      const auto image = m_images.constFind(page);
      if (image != m_images.constEnd() && image->m_scale >= scale * 0.999)
        continue;
      if (m_failed.contains(page) && qFuzzyCompare(m_failed.value(page), scale))
        continue;
      m_failed.remove(page);
      m_inFlight = true;
      m_requestSnapshot = m_snapshot->m_id;
      m_requestPage = page;
      m_requestScale = scale;
      emit q->rasterRequested(m_requestSnapshot, page, scale);
      return;
    }
  }

  void drain() {
    if (m_layout->isBusy()) {
      m_layout->requestIdleNotification();
      return;
    }
    m_draining = true;
    for (int index : m_recreate) {
      if (index < m_items.size())
        delete m_items[index].m_widget.data();
    }
    m_recreate.clear();
    if (m_publish) {
      m_publish = false;
      if (!m_restoreAnchor)
        preserveAnchor();
      publish();
      m_geometryDirty = true;
    }
    if (m_geometryDirty) {
      rebuildGeometry();
      m_geometryDirty = false;
    }
    if (m_restoreAnchor)
      restoreAnchor();
    visibleWidgets();
    m_lastAnchor = captureAnchor();
    m_draining = false;
    // A viewport resize caused by a newly visible editor scrollbar may have
    // changed the width basis during publication. Settle that demand once;
    // ordinary geometry notifications do not republish reservations.
    if (m_publish || m_geometryDirty || !m_recreate.isEmpty())
      schedule();
    requestImage();
  }

  TypstPreviewHost *q;
  VTextEdit *m_edit;
  QTextDocument *m_doc;
  TextDocumentLayout *m_layout;
  QTimer m_timer;
  TypstSnapshot m_snapshot;
  QString m_activeFile;
  QVector<Item> m_items;
  QVector<Geometry> m_geometry;
  QSet<int> m_visible;
  QSet<int> m_recreate;
  QHash<int, PageImage> m_images;
  QHash<int, qreal> m_needs;
  QHash<int, qreal> m_failed;
  Anchor m_anchor;
  Anchor m_lastAnchor;
  qreal m_maxHeight;
  qint64 m_budget;
  qint64 m_bytes = 0;
  int m_maxPixels;
  quint64 m_use = 0;
  bool m_enabled = true;
  bool m_draining = false;
  bool m_publish = false;
  bool m_geometryDirty = false;
  bool m_restoreAnchor = false;
  bool m_inFlight = false;
  quint64 m_requestSnapshot = 0;
  int m_requestPage = -1;
  qreal m_requestScale = 0;
};

TypstPreviewHost::TypstPreviewHost(TypstSourceEditor *p_editor, const TypstEditorConfig &p_config,
                                   QObject *p_parent)
    : QObject(p_parent), d(new Private(this, p_editor, p_config)) {
  connect(d->m_layout, &TextDocumentLayout::becameIdle, this, [this]() {
    // This signal runs while the outermost layout pass is unwinding.
    if (!d->m_timer.isActive())
      d->m_timer.start(0);
  });
  connect(d->m_layout, &TextDocumentLayout::widgetPreviewGeometryChanged, this, [this]() {
    d->m_geometryDirty = true;
    d->schedule();
  });
  connect(d->m_doc, &QTextDocument::contentsChange, this, [this](int, int p_removed, int p_added) {
    if (p_removed || p_added)
      clear();
  });
  connect(d->m_edit->verticalScrollBar(), &QScrollBar::valueChanged, this, [this]() {
    // A deliberate scroll supersedes an anchor saved before deferred work.
    if (!d->m_draining && !d->m_layout->isBusy() && !d->m_restoreAnchor)
      d->m_lastAnchor = d->captureAnchor();
    d->schedule();
  });
  connect(d->m_edit->horizontalScrollBar(), &QScrollBar::valueChanged, this,
          [this]() { d->schedule(); });
  connect(p_editor->getTextFolding(), &TextFolding::foldingRangesChanged, this, [this]() {
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

void TypstPreviewHost::setSnapshot(const TypstSnapshot &p_snapshot) {
  if (!p_snapshot) {
    clear();
    return;
  }
  if (d->m_snapshot && (p_snapshot->m_epoch < d->m_snapshot->m_epoch ||
                        (p_snapshot->m_epoch == d->m_snapshot->m_epoch &&
                         p_snapshot->m_generation < d->m_snapshot->m_generation)))
    return;
  if (d->m_snapshot == p_snapshot)
    return;
  d->preserveAnchor();
  d->destroyWidgets();
  d->m_snapshot = p_snapshot;
  if (d->m_activeFile.isEmpty())
    d->m_activeFile = p_snapshot->m_activeFile;
  d->m_images.clear();
  d->m_failed.clear();
  d->m_needs.clear();
  d->m_recreate.clear();
  d->m_bytes = 0;
  d->m_inFlight = false;
  d->buildItems();
  d->m_publish = true;
  d->schedule();
}

void TypstPreviewHost::clear() {
  if (!d->m_snapshot && d->m_items.isEmpty())
    return;
  d->preserveAnchor();
  // Hiding these non-editing, non-focusable widgets cannot write back source.
  // Reservations are removed only from the later idle drain.
  for (auto &item : d->m_items)
    if (item.m_widget)
      item.m_widget->hide();
  d->destroyWidgets();
  d->m_items.clear();
  d->m_geometry.clear();
  d->m_recreate.clear();
  d->m_snapshot.clear();
  d->m_images.clear();
  d->m_failed.clear();
  d->m_needs.clear();
  d->m_bytes = 0;
  d->m_inFlight = false;
  d->m_publish = true;
  d->schedule();
}

void TypstPreviewHost::setEnabled(bool p_enabled) {
  if (d->m_enabled == p_enabled)
    return;
  d->preserveAnchor();
  d->m_enabled = p_enabled;
  if (!p_enabled)
    d->destroyWidgets();
  d->m_publish = true;
  d->schedule();
}

void TypstPreviewHost::setActiveFile(const QString &p_file) {
  if (d->m_activeFile == p_file)
    return;
  clear();
  d->m_activeFile = p_file;
}

void TypstPreviewHost::setPageImage(quint64 p_snapshot, int p_page, qreal p_scale,
                                    const QImage &p_image) {
  if (!d->m_inFlight || p_snapshot != d->m_requestSnapshot || p_page != d->m_requestPage ||
      !qFuzzyCompare(p_scale, d->m_requestScale))
    return;
  d->m_inFlight = false;
  if (!d->m_snapshot || p_snapshot != d->m_snapshot->m_id)
    return;
  const qint64 bytes = qint64(p_image.bytesPerLine()) * p_image.height();
  const auto pageSize = d->m_snapshot->m_pages[p_page];
  if (p_image.isNull() || bytes > d->m_budget ||
      qint64(p_image.width()) * p_image.height() > d->m_maxPixels ||
      p_image.width() > qMax(1, qCeil(pageSize.width() * p_scale)) ||
      p_image.height() > qMax(1, qCeil(pageSize.height() * p_scale))) {
    d->m_failed.insert(p_page, p_scale);
  } else {
    d->evict(bytes, p_page);
    d->m_images.insert(p_page, {p_image, p_scale, ++d->m_use});
    d->m_bytes += bytes;
  }
  for (int index : d->m_visible) {
    if (index < d->m_items.size() && d->m_items[index].m_widget)
      d->m_items[index].m_widget->update();
  }
  d->schedule();
}

void TypstPreviewHost::applyConfig(const TypstEditorConfig &p_config) {
  d->preserveAnchor();
  d->m_maxHeight = qBound<qreal>(80, p_config.m_previewMaxHeight, 100000);
  d->m_budget = qMax<qint64>(4, p_config.m_maxCacheBytes / 4);
  d->m_maxPixels = qMax(1, p_config.m_maxRasterPixels);
  // Config changes are explicit rerender demand, including recovery after a
  // previous resource limit failure. Existing in-flight transport is bounded.
  d->m_failed.clear();
  d->m_images.clear();
  d->m_bytes = 0;
  for (int index : d->m_visible)
    d->m_recreate.insert(index);
  d->m_publish = true;
  d->schedule();
}

bool TypstPreviewHost::eventFilter(QObject *p_object, QEvent *p_event) {
  if (p_object == d->m_edit || p_object == d->m_edit->viewport()) {
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
      for (int index : d->m_visible)
        d->m_recreate.insert(index);
      d->schedule();
      break;
    case QEvent::Show:
    case QEvent::Hide:
      d->schedule();
      break;
    default:
      break;
    }
  }
  return QObject::eventFilter(p_object, p_event);
}
