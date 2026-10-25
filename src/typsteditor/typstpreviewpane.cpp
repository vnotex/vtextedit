#include "typstpreviewpane.h"

#include <vtextedit/typsteditorconfig.h>

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPdfDocument>
#include <QPdfPageRenderer>
#include <QPdfSearchModel>
#include <QPdfSelection>
#include <QPointer>
#include <QResizeEvent>
#include <QScopeGuard>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolTip>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>

using namespace vte;

namespace {

constexpr qreal PageMargin = 16.0;
constexpr qreal PageGap = 16.0;
constexpr qreal MaximumExtent = 1.0e12;
constexpr int MaximumPages = 100000;
constexpr int MaximumRasterDimension = 8192;
constexpr int MaximumVisiblePages = 1024;
constexpr int MaximumCacheEntries = 2048;
constexpr int MaximumScrollValue = std::numeric_limits<int>::max() - 1;

bool finiteCoordinate(qreal p_value) { return std::isfinite(static_cast<double>(p_value)); }

int scrollMaximum(qreal p_extent) {
  return static_cast<int>(std::ceil(qMin(qMax(qreal(0), p_extent), qreal(MaximumScrollValue))));
}

int toScrollValue(qreal p_position, qreal p_extent) {
  if (p_extent <= 0) {
    return 0;
  }
  return qRound(qBound(qreal(0), p_position / p_extent, qreal(1)) * scrollMaximum(p_extent));
}

qreal fromScrollValue(int p_value, qreal p_extent) {
  const int maximum = scrollMaximum(p_extent);
  return maximum ? p_extent * (qreal(p_value) / maximum) : 0;
}

struct ViewAnchor {
  int m_page = 0;
  QPointF m_point;
  QPointF m_viewport = QPointF(PageMargin, PageMargin);
};

struct PageLayout {
  QVector<QRectF> m_pages;
  QSizeF m_extent;
  QPointF m_offset;
  qreal m_scale = 1;
  qreal m_zoom = 1;
  qreal m_dpr = 1;
  quint64 m_revision = 0;
};

struct RenderKey {
  int m_page = -1;
  QSize m_size;
  QPdfDocumentRenderOptions m_options;

  bool usableFor(const RenderKey &p_other) const {
    return m_page == p_other.m_page && m_options == p_other.m_options;
  }

  bool adequateFor(const RenderKey &p_other) const {
    return usableFor(p_other) && m_size.width() >= p_other.m_size.width() &&
           m_size.height() >= p_other.m_size.height();
  }

  qint64 pixels() const { return qint64(m_size.width()) * m_size.height(); }
};

struct RenderNeed {
  RenderKey m_key;
  bool m_firstPaint = false;
};

struct CachedImage {
  RenderKey m_key;
  QImage m_image;
  quint64 m_request = 0;
  quint64 m_used = 0;
};

// QObject parenting cannot express renderer -> search -> document -> device
// destruction. In particular the renderer destructor joins its worker thread.
struct PdfSlot {
  ~PdfSlot() {
    if (m_renderer) {
      QObject::disconnect(m_renderer.get(), nullptr, nullptr, nullptr);
      m_renderer.reset();
    }
    m_search.reset();
    if (m_document) {
      m_document->close();
      m_document.reset();
    }
    m_buffer.reset();
    m_pdf.clear();
    m_images.clear();
  }

  quint64 m_serial = 0;
  TypstSnapshot m_snapshot;
  QByteArray m_pdf;
  std::unique_ptr<QBuffer> m_buffer;
  std::unique_ptr<QPdfDocument> m_document;
  std::unique_ptr<QPdfPageRenderer> m_renderer;
  std::unique_ptr<QPdfSearchModel> m_search;
  QVector<QSizeF> m_pageSizes;
  qreal m_widestPage = 0;
  PageLayout m_layout;
  QVector<RenderNeed> m_needs;
  QVector<CachedImage> m_images;
  RenderKey m_active;
  quint64 m_request = 0;
  bool m_initialized = false;
  bool m_obsolete = false;
  bool m_failed = false;
  bool m_resourceFailure = false;
};

struct OwnedSelection {
  quint64 m_snapshot = 0;
  int m_page = -1;
  QPointF m_anchor;
  QPointF m_endpoint;
  QString m_text;
  QVector<QPolygonF> m_polygons;
};

} // namespace

class vte::TypstPreviewPanePrivate {
public:
  using ZoomMode = VTypstEditor::PdfZoomMode;
  enum class RenderPass { FirstPaint, Upgrade, Prefetch };

  TypstPreviewPanePrivate(TypstPreviewPane *p_q, const TypstEditorConfig &p_config)
      : q(p_q), m_maxPdfBytes(qMax(qint64(0), p_config.m_maxPdfBytes)),
        m_maxBytes(qMax(qint64(0), p_config.m_maxCacheBytes)),
        m_maxPixels(qMax(0, p_config.m_maxRasterPixels)), m_background(p_config.m_pdfBackground),
        m_pageBackground(p_config.getPreviewBackground()),
        m_themeStyleEnabled(p_config.m_enableThemeStyleInPreview),
        m_foreground(p_config.m_previewForeground),
        m_zoom(finiteCoordinate(p_config.m_initialZoom)
                   ? qBound(qreal(0.01), p_config.m_initialZoom, qreal(64))
                   : 1),
        m_notifiedZoom(m_zoom) {
    m_autoScroll.setInterval(30);
    QObject::connect(&m_autoScroll, &QTimer::timeout, q, [this]() { autoScroll(); });
    m_sourceFlash.setInterval(200);
    QObject::connect(&m_sourceFlash, &QTimer::timeout, q, [this]() {
      if (--m_sourceFlashPhases == 0) {
        q->clearSourceHighlight();
      } else {
        q->viewport()->update();
      }
    });
  }

  ~TypstPreviewPanePrivate() {
    m_closing = true;
    ++m_epoch;
    m_autoScroll.stop();
    m_sourceFlash.stop();
    // Stop all callbacks before any join. Never close a document underneath
    // a worker, including during final destruction with a render in flight.
    for (auto slot : allSlots()) {
      if (slot) {
        QObject::disconnect(slot->m_renderer.get(), nullptr, q, nullptr);
        QObject::disconnect(slot->m_document.get(), nullptr, q, nullptr);
        QObject::disconnect(slot->m_search.get(), nullptr, q, nullptr);
      }
    }
    m_pending.reset();
    m_retiring.reset();
    m_displayed.reset();
    for (auto &slot : m_resetRetiring) {
      slot.reset();
    }
  }

  std::array<PdfSlot *, 5> allSlots() const {
    return {{m_displayed.get(), m_pending.get(), m_retiring.get(), m_resetRetiring[0].get(),
             m_resetRetiring[1].get()}};
  }

  bool current(const TypstSnapshot &p_snapshot) const {
    return p_snapshot && p_snapshot->m_epoch == m_epoch && p_snapshot->m_generation == m_generation;
  }

  bool pendingUpdate() const {
    return bool(m_queuedSnapshot) || (m_pending && !m_pending->m_obsolete && !m_pending->m_failed);
  }

  void notifyUpdate() {
    const bool available = pendingUpdate();
    if (available != m_updateAvailable) {
      m_updateAvailable = available;
      emit q->updateAvailableChanged(available);
    }
    q->viewport()->update();
  }

  void resetForNewDocument() {
    m_queuedSnapshot.clear();
    m_queuedPdf.clear();
    // Keep renderer/document/device ownership until each in-flight serial completes.
    // No new slot is loaded until these two reset-only retirements have drained.
    auto retireForReset = [](std::unique_ptr<PdfSlot> &p_slot,
                             std::unique_ptr<PdfSlot> &p_retiring) {
      if (!p_slot) {
        return;
      }
      Q_ASSERT(!p_retiring);
      p_slot->m_obsolete = true;
      p_slot->m_needs.clear();
      p_slot->m_images.clear();
      p_retiring = std::move(p_slot);
    };
    retireForReset(m_displayed, m_resetRetiring[0]);
    retireForReset(m_pending, m_resetRetiring[1]);
    ++m_viewRevision;
    m_anchor = ViewAnchor();
    m_searchText.clear();
    m_searchIndex = -1;
    m_searchNavigatePending = false;
    m_searchBackwards = false;
    m_focusedPage = -1;
    m_pressedLink = -1;
    m_pressPosition = QPointF();
    m_pointer = QPointF();
    m_controlClick = false;
    m_error.clear();
    m_needsDirty = true;
    q->clearSourceHighlight();
    {
      const QSignalBlocker horizontal(q->horizontalScrollBar());
      const QSignalBlocker vertical(q->verticalScrollBar());
      q->horizontalScrollBar()->setRange(0, 0);
      q->verticalScrollBar()->setRange(0, 0);
    }
    q->viewport()->unsetCursor();
    clearSelection(false);
    notifyUpdate();
    schedule();
  }

  void report(const QString &p_message) {
    m_error = p_message;
    q->viewport()->update();
    emit q->errorOccurred(p_message);
  }

  void schedule() {
    if (m_scheduled || m_closing) {
      return;
    }
    m_scheduled = true;
    QTimer::singleShot(0, q, [this]() {
      m_scheduled = false;
      service();
      // Publication must finish all render-slot work before a consumer can
      // synchronously change the camera or its resource configuration.
      notifyZoomChanged();
    });
  }

  PdfSlot *slotBySerial(quint64 p_serial) const {
    for (auto slot : allSlots()) {
      if (slot && slot->m_serial == p_serial) {
        return slot;
      }
    }
    return nullptr;
  }

  qint64 pdfBytes() const {
    qint64 result = m_queuedPdf.size();
    for (auto slot : allSlots()) {
      if (slot) {
        result += slot->m_pdf.size();
      }
    }
    return result;
  }

  qint64 imagePixels() const {
    qint64 result = 0;
    for (auto slot : allSlots()) {
      if (slot) {
        if (slot->m_request) {
          result += slot->m_active.pixels();
        }
        for (const auto &image : slot->m_images) {
          result += image.m_key.pixels();
        }
      }
    }
    return result;
  }

  qint64 usedBytes() const {
    qint64 result = pdfBytes();
    for (auto slot : allSlots()) {
      if (slot) {
        if (slot->m_request) {
          result += slot->m_active.pixels() * 4;
        }
        for (const auto &image : slot->m_images) {
          result += static_cast<qint64>(image.m_image.sizeInBytes());
        }
      }
    }
    return result;
  }

  QVector<int> visiblePages(const PageLayout &p_layout) const {
    QVector<int> result;
    const QRectF visible(p_layout.m_offset, q->viewport()->size());
    auto first =
        std::lower_bound(p_layout.m_pages.cbegin(), p_layout.m_pages.cend(), visible.top(),
                         [](const QRectF &p_rect, qreal p_y) { return p_rect.bottom() < p_y; });
    for (auto it = first; it != p_layout.m_pages.cend() && it->top() <= visible.bottom(); ++it) {
      if (it->intersects(visible)) {
        result.append(static_cast<int>(it - p_layout.m_pages.cbegin()));
        if (result.size() > MaximumVisiblePages) {
          break;
        }
      }
    }
    return result;
  }

  int bestImageIndex(const PdfSlot &p_slot, int p_page) const {
    const RenderKey *wanted = nullptr;
    for (const auto &need : p_slot.m_needs) {
      if (need.m_key.m_page == p_page) {
        wanted = &need.m_key;
        break;
      }
    }
    int result = -1;
    bool bestAdequate = false;
    qint64 bestPixels = 0;
    for (int i = 0; i < p_slot.m_images.size(); ++i) {
      const auto &key = p_slot.m_images.at(i).m_key;
      if (key.m_page != p_page || (wanted && !key.usableFor(*wanted))) {
        continue;
      }
      const bool adequate = wanted && key.adequateFor(*wanted);
      const qint64 pixels = key.pixels();
      if (result < 0 || (adequate && !bestAdequate) ||
          (adequate == bestAdequate && (adequate ? pixels < bestPixels : pixels > bestPixels))) {
        result = i;
        bestAdequate = adequate;
        bestPixels = pixels;
      }
    }
    return result;
  }

  bool pinned(const PdfSlot &p_slot, int p_index) const {
    if (&p_slot == m_retiring.get() || p_slot.m_obsolete) {
      return false;
    }
    const auto &key = p_slot.m_images.at(p_index).m_key;
    if (&p_slot == m_displayed.get()) {
      return p_slot.m_layout.m_pages.at(key.m_page)
                 .intersects(QRectF(p_slot.m_layout.m_offset, q->viewport()->size())) &&
             bestImageIndex(p_slot, key.m_page) == p_index;
    }
    for (const auto &need : p_slot.m_needs) {
      if (need.m_firstPaint && key.usableFor(need.m_key)) {
        return bestImageIndex(p_slot, key.m_page) == p_index;
      }
    }
    return false;
  }

  bool reserve(qint64 p_bytes, qint64 p_pixels, bool p_evict = true) {
    for (;;) {
      int entries = 0;
      for (auto slot : allSlots()) {
        if (slot) {
          entries += slot->m_images.size() + (slot->m_request ? 1 : 0);
        }
      }
      if (p_bytes <= m_maxBytes - usedBytes() && p_pixels <= m_maxPixels - imagePixels() &&
          entries + (p_pixels > 0 ? 1 : 0) <= MaximumCacheEntries) {
        return true;
      }
      if (!p_evict) {
        return false;
      }
      PdfSlot *oldestSlot = nullptr;
      int oldestIndex = -1;
      quint64 oldestUse = std::numeric_limits<quint64>::max();
      for (auto slot : allSlots()) {
        if (!slot) {
          continue;
        }
        for (int i = 0; i < slot->m_images.size(); ++i) {
          if (!pinned(*slot, i) && slot->m_images.at(i).m_used < oldestUse) {
            oldestSlot = slot;
            oldestIndex = i;
            oldestUse = slot->m_images.at(i).m_used;
          }
        }
      }
      if (!oldestSlot) {
        return false;
      }
      oldestSlot->m_images.removeAt(oldestIndex);
    }
  }

  QPointF maximumOffset(const PageLayout &p_layout) const {
    return QPointF(qMax(qreal(0), p_layout.m_extent.width() - q->viewport()->width()),
                   qMax(qreal(0), p_layout.m_extent.height() - q->viewport()->height()));
  }

  QPointF clampOffset(const PageLayout &p_layout, const QPointF &p_offset) const {
    const QPointF maximum = maximumOffset(p_layout);
    return QPointF(qBound(qreal(0), p_offset.x(), maximum.x()),
                   qBound(qreal(0), p_offset.y(), maximum.y()));
  }

  int nearestPage(const PageLayout &p_layout, const QPointF &p_viewportPoint) const {
    if (p_layout.m_pages.isEmpty()) {
      return -1;
    }
    const qreal y = p_viewportPoint.y() + p_layout.m_offset.y();
    auto it =
        std::lower_bound(p_layout.m_pages.cbegin(), p_layout.m_pages.cend(), y,
                         [](const QRectF &p_rect, qreal p_y) { return p_rect.bottom() < p_y; });
    if (it == p_layout.m_pages.cend()) {
      return p_layout.m_pages.size() - 1;
    }
    int page = static_cast<int>(it - p_layout.m_pages.cbegin());
    if (page > 0 && y < it->top() && y - p_layout.m_pages.at(page - 1).bottom() < it->top() - y) {
      --page;
    }
    return page;
  }

  QPointF pagePoint(const PdfSlot &p_slot, int p_page, const QPointF &p_position) const {
    const QPointF point =
        (p_position + p_slot.m_layout.m_offset - p_slot.m_layout.m_pages.at(p_page).topLeft()) /
        p_slot.m_layout.m_scale;
    const QSizeF size = p_slot.m_pageSizes.at(p_page);
    return QPointF(qBound(qreal(0), point.x(), size.width()),
                   qBound(qreal(0), point.y(), size.height()));
  }

  int hitPage(const QPointF &p_position) const {
    if (!m_displayed) {
      return -1;
    }
    const int page = nearestPage(m_displayed->m_layout, p_position);
    return page >= 0 && m_displayed->m_layout.m_pages.at(page).contains(
                            p_position + m_displayed->m_layout.m_offset)
               ? page
               : -1;
  }

  ViewAnchor anchorAt(const QPointF &p_position) const {
    ViewAnchor result;
    if (m_displayed && !m_displayed->m_layout.m_pages.isEmpty()) {
      result.m_page = nearestPage(m_displayed->m_layout, p_position);
      result.m_point = pagePoint(*m_displayed, result.m_page, p_position);
      const auto &layout = m_displayed->m_layout;
      result.m_viewport = layout.m_pages.at(result.m_page).topLeft() +
                          result.m_point * layout.m_scale - layout.m_offset;
    }
    return result;
  }

  void saveAnchor() {
    if (m_displayed) {
      m_anchor = anchorAt(QPointF(q->viewport()->width() / 2.0, q->viewport()->height() / 2.0));
    }
  }

  bool buildLayout(PdfSlot &p_slot, const ViewAnchor &p_anchor) {
    PageLayout layout;
    const int anchorPage =
        qBound(0, p_anchor.m_page, static_cast<int>(p_slot.m_pageSizes.size()) - 1);
    const qreal widest = p_slot.m_widestPage;
    const qreal dpiScale = qMax(qreal(1), qreal(q->logicalDpiY())) / 72.0;
    layout.m_zoom = m_zoom;
    if (m_zoomMode == ZoomMode::Width) {
      layout.m_zoom =
          qMax(qreal(1), qreal(q->viewport()->width()) - 2 * PageMargin) / widest / dpiScale;
    } else if (m_zoomMode == ZoomMode::Page) {
      const QSizeF page = p_slot.m_pageSizes.at(anchorPage);
      layout.m_zoom =
          qMin(qMax(qreal(1), qreal(q->viewport()->width()) - 2 * PageMargin) / page.width(),
               qMax(qreal(1), qreal(q->viewport()->height()) - 2 * PageMargin) / page.height()) /
          dpiScale;
    }
    layout.m_scale = layout.m_zoom * dpiScale;
    layout.m_dpr = q->viewport()->devicePixelRatioF();
    layout.m_revision = m_viewRevision;
    const qreal width =
        qMax(qreal(q->viewport()->width()), widest * layout.m_scale + 2 * PageMargin);
    qreal y = PageMargin;
    if (!finiteCoordinate(width) || width > MaximumExtent || !finiteCoordinate(layout.m_scale) ||
        layout.m_scale <= 0) {
      return false;
    }
    if (!p_slot.m_layout.m_pages.isEmpty() && p_slot.m_layout.m_scale == layout.m_scale &&
        p_slot.m_layout.m_extent.width() == width) {
      // Scroll/anchor changes share the immutable geometry, not a new
      // allocation and traversal of every page in a long document.
      layout.m_pages = p_slot.m_layout.m_pages;
      layout.m_extent = p_slot.m_layout.m_extent;
    } else {
      layout.m_pages.reserve(p_slot.m_pageSizes.size());
      for (const auto &size : p_slot.m_pageSizes) {
        const QSizeF scaled = size * layout.m_scale;
        if (!finiteCoordinate(scaled.height()) || scaled.height() > MaximumExtent - y - PageGap) {
          return false;
        }
        layout.m_pages.append(QRectF(QPointF((width - scaled.width()) / 2, y), scaled));
        y += scaled.height() + PageGap;
      }
      layout.m_extent = QSizeF(width, y - PageGap + PageMargin);
    }
    const QSizeF anchorSize = p_slot.m_pageSizes.at(anchorPage);
    const QPointF point(qBound(qreal(0), p_anchor.m_point.x(), anchorSize.width()),
                        qBound(qreal(0), p_anchor.m_point.y(), anchorSize.height()));
    const QPointF view(qBound(qreal(0), p_anchor.m_viewport.x(), qreal(q->viewport()->width())),
                       qBound(qreal(0), p_anchor.m_viewport.y(), qreal(q->viewport()->height())));
    layout.m_offset = clampOffset(layout, layout.m_pages.at(anchorPage).topLeft() +
                                              point * layout.m_scale - view);
    p_slot.m_layout = std::move(layout);
    return true;
  }

  void updateScrollBars() {
    if (!m_displayed) {
      return;
    }
    const auto &layout = m_displayed->m_layout;
    const QPointF maximum = maximumOffset(layout);
    m_settingScrollBars = true;
    const QSignalBlocker horizontal(q->horizontalScrollBar());
    const QSignalBlocker vertical(q->verticalScrollBar());
    auto set = [](QScrollBar *p_bar, qreal p_extent, qreal p_position, int p_viewport) {
      p_bar->setRange(0, scrollMaximum(p_extent));
      const qreal ratio = p_extent > 0 ? scrollMaximum(p_extent) / p_extent : 1;
      p_bar->setPageStep(qMax(1, qRound(qMin(qreal(MaximumScrollValue), p_viewport * ratio))));
      p_bar->setSingleStep(qMax(1, qRound(qMin(qreal(MaximumScrollValue), 30 * ratio))));
      p_bar->setValue(toScrollValue(p_position, p_extent));
    };
    set(q->horizontalScrollBar(), maximum.x(), layout.m_offset.x(), q->viewport()->width());
    set(q->verticalScrollBar(), maximum.y(), layout.m_offset.y(), q->viewport()->height());
    m_settingScrollBars = false;
  }

  void setOffset(const QPointF &p_offset) {
    if (!m_displayed) {
      return;
    }
    const QPointF offset = clampOffset(m_displayed->m_layout, p_offset);
    if (offset == m_displayed->m_layout.m_offset) {
      return;
    }
    const bool verticalChanged = offset.y() != m_displayed->m_layout.m_offset.y();
    m_displayed->m_layout.m_offset = offset;
    ++m_viewRevision;
    m_displayed->m_layout.m_revision = m_viewRevision;
    updateScrollBars();
    saveAnchor();
    m_focusedPage = m_anchor.m_page;
    resumeResourceFailures();
    m_needsDirty = true;
    schedule();
    q->viewport()->update();
    if (verticalChanged) {
      emit q->verticalScrollChanged();
    }
  }

  void resumeResourceFailures() {
    for (auto slot : allSlots()) {
      if (slot && slot != m_retiring.get() && slot->m_resourceFailure && !slot->m_obsolete) {
        slot->m_failed = false;
        slot->m_resourceFailure = false;
        m_error.clear();
      }
    }
  }

  void viewChanged(bool p_relayout) {
    ++m_viewRevision;
    resumeResourceFailures();
    if (m_displayed && p_relayout) {
      if (!buildLayout(*m_displayed, m_anchor)) {
        report(TypstPreviewPane::tr("The PDF page layout exceeds the supported coordinate range."));
        return;
      }
      m_zoom = m_displayed->m_layout.m_zoom;
      updateScrollBars();
      saveAnchor();
    }
    m_needsDirty = true;
    schedule();
    q->viewport()->update();
  }

  void notifyZoomChanged() {
    if (m_servicing || (m_notifiedZoom == m_zoom && m_notifiedZoomMode == m_zoomMode)) {
      return;
    }
    m_notifiedZoom = m_zoom;
    m_notifiedZoomMode = m_zoomMode;
    emit q->zoomChanged(m_zoom, m_zoomMode);
  }

  void changeZoom(ZoomMode p_mode, qreal p_zoom, const QPointF &p_position) {
    if (!finiteCoordinate(p_zoom) || p_zoom <= 0) {
      report(TypstPreviewPane::tr("The PDF zoom must be a positive finite number."));
      return;
    }
    m_anchor = anchorAt(p_position);
    m_zoomMode = p_mode;
    m_zoom = qBound(qreal(0.01), p_zoom, qreal(64));
    viewChanged(true);
    notifyZoomChanged();
  }

  bool makeNeeds(PdfSlot &p_slot) {
    p_slot.m_needs.clear();
    QVector<int> pages = visiblePages(p_slot.m_layout);
    if (pages.size() > MaximumVisiblePages) {
      fail(p_slot,
           TypstPreviewPane::tr("Too many PDF pages are visible at this zoom. Increase the zoom."),
           true);
      return false;
    }
    const int visibleCount = pages.size();
    if (!pages.isEmpty()) {
      const int previous = pages.first() - 1;
      const int next = pages.last() + 1;
      if (next < p_slot.m_pageSizes.size()) {
        pages.append(next);
      }
      if (previous >= 0) {
        pages.append(previous);
      }
    }
    for (int i = 0; i < pages.size(); ++i) {
      const int page = pages.at(i);
      const QSizeF logical = p_slot.m_layout.m_pages.at(page).size();
      const qreal width = logical.width() * p_slot.m_layout.m_dpr;
      const qreal height = logical.height() * p_slot.m_layout.m_dpr;
      const qreal factor = qMin(qreal(1), qMin(MaximumRasterDimension / qMax(qreal(1), width),
                                               MaximumRasterDimension / qMax(qreal(1), height)));
      RenderNeed need;
      need.m_key.m_page = page;
      const int pixelWidth = qBound(
          1, static_cast<int>(std::ceil(qMin(qreal(MaximumRasterDimension), width * factor))),
          MaximumRasterDimension);
      const int pixelHeight = qBound(
          1, static_cast<int>(std::ceil(qMin(qreal(MaximumRasterDimension), height * factor))),
          MaximumRasterDimension);
      need.m_key.m_size = QSize(pixelWidth, pixelHeight);
      need.m_firstPaint = i < visibleCount;
      p_slot.m_needs.append(need);
    }
    return true;
  }

  void fail(PdfSlot &p_slot, const QString &p_message, bool p_resourceFailure = false) {
    if (!p_slot.m_failed) {
      p_slot.m_failed = true;
      p_slot.m_resourceFailure = p_resourceFailure;
      p_slot.m_needs.clear();
      report(p_message);
      notifyUpdate();
      schedule();
    }
  }

  void initialize(PdfSlot &p_slot) {
    const int count = p_slot.m_document->pageCount();
    if (count <= 0 || count > MaximumPages || count != p_slot.m_snapshot->m_pages.size()) {
      fail(p_slot, TypstPreviewPane::tr(
                       "The PDF page count is invalid or does not match its compiled snapshot."));
      return;
    }
    p_slot.m_pageSizes.reserve(count);
    for (int page = 0; page < count; ++page) {
      const QSizeF size = p_slot.m_document->pagePointSize(page);
      const QSizeF expected = p_slot.m_snapshot->m_pages.at(page);
      if (!finiteCoordinate(size.width()) || !finiteCoordinate(size.height()) ||
          size.width() <= 0 || size.height() <= 0 || !finiteCoordinate(expected.width()) ||
          !finiteCoordinate(expected.height()) || expected.width() <= 0 || expected.height() <= 0 ||
          std::abs(size.width() - expected.width()) > 0.1 ||
          std::abs(size.height() - expected.height()) > 0.1) {
        fail(p_slot,
             TypstPreviewPane::tr(
                 "PDF page %1 has invalid dimensions or differs from its compiled snapshot.")
                 .arg(page + 1));
        return;
      }
      p_slot.m_pageSizes.append(size);
      p_slot.m_widestPage = qMax(p_slot.m_widestPage, size.width());
    }
    p_slot.m_renderer->setRenderMode(QPdfPageRenderer::RenderMode::MultiThreaded);
    p_slot.m_renderer->setDocument(p_slot.m_document.get());
    p_slot.m_search->setDocument(p_slot.m_document.get());
    p_slot.m_initialized = true;
    m_needsDirty = true;
  }

  void loadQueued() {
    if (!m_queuedSnapshot || m_pending || m_resetRetiring[0] || m_resetRetiring[1]) {
      return;
    }
    auto slot = std::make_unique<PdfSlot>();
    slot->m_serial = ++m_nextSerial;
    slot->m_snapshot = m_queuedSnapshot;
    slot->m_pdf = std::move(m_queuedPdf);
    m_queuedSnapshot.clear();
    slot->m_buffer = std::make_unique<QBuffer>();
    slot->m_buffer->setData(slot->m_pdf);
    slot->m_document = std::make_unique<QPdfDocument>();
    slot->m_renderer = std::make_unique<QPdfPageRenderer>();
    slot->m_search = std::make_unique<QPdfSearchModel>();
    const quint64 serial = slot->m_serial;
    QObject::connect(slot->m_document.get(), &QPdfDocument::statusChanged, q,
                     [this]() { schedule(); });
    QObject::connect(slot->m_renderer.get(), &QPdfPageRenderer::pageRendered, q,
                     [this, serial](int p_page, QSize p_size, const QImage &p_image,
                                    QPdfDocumentRenderOptions p_options, quint64 p_request) {
                       rendered(serial, p_page, p_size, p_image, p_options, p_request);
                     });
    auto searchChanged = [this, serial]() {
      if (m_displayed && m_displayed->m_serial == serial) {
        const int count = m_displayed->m_search->rowCount(QModelIndex());
        if (m_searchIndex >= count) {
          m_searchIndex = -1;
        }
        if (m_searchNavigatePending && count > 0) {
          m_searchNavigatePending = false;
          q->findNext(m_searchBackwards);
        }
        q->viewport()->update();
      }
    };
    QObject::connect(slot->m_search.get(), &QAbstractItemModel::modelReset, q, searchChanged);
    QObject::connect(slot->m_search.get(), &QAbstractItemModel::rowsInserted, q, searchChanged);
    QObject::connect(slot->m_search.get(), &QAbstractItemModel::rowsRemoved, q, searchChanged);
    QObject::connect(slot->m_search.get(), &QAbstractItemModel::dataChanged, q, searchChanged);
    m_pending = std::move(slot);
    if (!m_pending->m_buffer->open(QIODevice::ReadOnly)) {
      fail(*m_pending, TypstPreviewPane::tr("The in-memory PDF could not be opened."));
      return;
    }
    m_pending->m_document->load(m_pending->m_buffer.get());
  }

  void retire(std::unique_ptr<PdfSlot> &p_slot) {
    p_slot->m_obsolete = true;
    p_slot->m_needs.clear();
    p_slot->m_images.clear();
    if (!p_slot->m_request) {
      p_slot.reset();
    } else if (!m_retiring) {
      m_retiring = std::move(p_slot);
    }
  }

  void service() {
    if (m_closing) {
      return;
    }
    const QScopedValueRollback<bool> guard(m_servicing, true);
    for (auto &slot : m_resetRetiring) {
      if (slot && !slot->m_request) {
        slot.reset();
      }
    }
    if (m_retiring && !m_retiring->m_request) {
      m_retiring.reset();
    }
    if (m_pending &&
        (m_pending->m_obsolete || (m_pending->m_failed && !m_pending->m_resourceFailure) ||
         !current(m_pending->m_snapshot))) {
      retire(m_pending);
    }
    if (m_queuedSnapshot && !current(m_queuedSnapshot)) {
      m_queuedSnapshot.clear();
      m_queuedPdf.clear();
    }
    loadQueued();
    if (m_pending && !m_pending->m_obsolete && !m_pending->m_failed && !m_pending->m_initialized) {
      if (m_pending->m_document->status() == QPdfDocument::Status::Error) {
        fail(*m_pending, TypstPreviewPane::tr("Qt PDF could not load the preview (error %1). The "
                                              "previous preview has been retained.")
                             .arg(static_cast<int>(m_pending->m_document->error())));
      } else if (m_pending->m_document->status() == QPdfDocument::Status::Ready) {
        initialize(*m_pending);
      }
    }
    if (m_needsDirty) {
      m_needsDirty = false;
      if (m_displayed && !m_displayed->m_failed) {
        makeNeeds(*m_displayed);
      }
      if (m_pending && m_pending->m_initialized && !m_pending->m_failed && !m_pending->m_obsolete) {
        if (!buildLayout(*m_pending, m_anchor)) {
          fail(*m_pending,
               TypstPreviewPane::tr("The PDF page layout exceeds the supported coordinate range."));
        } else {
          makeNeeds(*m_pending);
        }
      }
    }
    // Missing visible pages precede upgrades in either live snapshot. Qt
    // itself never receives a queue, and look-ahead uses only spare capacity.
    for (auto slot : {m_displayed.get(), m_pending.get()}) {
      if (slot) {
        submit(*slot, RenderPass::FirstPaint);
      }
    }
    if (visibleNeedsSatisfied(false)) {
      for (auto slot : {m_displayed.get(), m_pending.get()}) {
        if (slot) {
          submit(*slot, RenderPass::Upgrade);
        }
      }
    }
    tryPublish();
    if (visibleNeedsSatisfied(true)) {
      for (auto slot : {m_displayed.get(), m_pending.get()}) {
        if (slot) {
          submit(*slot, RenderPass::Prefetch);
        }
      }
    }
    notifyUpdate();
  }

  bool visibleNeedsSatisfied(bool p_ideal) const {
    for (auto slot : {m_displayed.get(), m_pending.get()}) {
      if (!slot || slot->m_obsolete || slot->m_failed) {
        continue;
      }
      if (!slot->m_initialized) {
        return false;
      }
      for (const auto &need : slot->m_needs) {
        if (!need.m_firstPaint) {
          continue;
        }
        const int index = bestImageIndex(*slot, need.m_key.m_page);
        if (index < 0 || (p_ideal && !slot->m_images.at(index).m_key.adequateFor(need.m_key))) {
          return false;
        }
      }
    }
    return true;
  }

  bool capacityWillBeReleased() const {
    for (auto slot : allSlots()) {
      if (!slot) {
        continue;
      }
      if (slot == m_retiring.get() || slot->m_obsolete) {
        return true;
      }
      if (!slot->m_request) {
        continue;
      }
      bool visible = false;
      for (const auto &need : slot->m_needs) {
        if (need.m_firstPaint && slot->m_active.usableFor(need.m_key)) {
          visible = true;
          break;
        }
      }
      // An offscreen result is disposable; a replacement releases the old
      // same-page image after residency. A different visible first paint
      // will stay pinned and is not a reason to defer a necessary fallback.
      if (!visible || bestImageIndex(*slot, slot->m_active.m_page) >= 0) {
        return true;
      }
    }
    return false;
  }

  qint64 firstPaintBudget(const RenderKey &p_key) const {
    const qint64 spare =
        qMax(qint64(0), qMin(m_maxPixels - imagePixels(), (m_maxBytes - usedBytes()) / 4));
    qint64 missingPixels = 0;
    int missingPages = 0;
    for (auto slot : {m_displayed.get(), m_pending.get()}) {
      if (!slot || slot->m_obsolete || slot->m_failed || !slot->m_initialized) {
        continue;
      }
      for (const auto &need : slot->m_needs) {
        if (need.m_firstPaint && bestImageIndex(*slot, need.m_key.m_page) < 0 &&
            !(slot->m_request && slot->m_active.usableFor(need.m_key))) {
          ++missingPages;
          missingPixels += need.m_key.pixels();
        }
      }
    }
    if (!missingPages || spare < missingPages) {
      return 0;
    }
    // Leave at least one pixel for each remaining first paint, then divide
    // the actual unreserved capacity proportionally to the ideal sizes.
    return qMin(p_key.pixels(), 1 + static_cast<qint64>((spare - missingPages) *
                                                        (qreal(p_key.pixels()) / missingPixels)));
  }

  void submit(PdfSlot &p_slot, RenderPass p_pass) {
    if (p_slot.m_request || p_slot.m_failed || p_slot.m_obsolete || !p_slot.m_initialized) {
      return;
    }
    for (const auto &need : p_slot.m_needs) {
      if (need.m_firstPaint == (p_pass == RenderPass::Prefetch)) {
        continue;
      }
      const int index = bestImageIndex(p_slot, need.m_key.m_page);
      if ((index >= 0 && p_slot.m_images.at(index).m_key.adequateFor(need.m_key)) ||
          (p_pass == RenderPass::FirstPaint && index >= 0) ||
          (p_pass == RenderPass::Upgrade && index < 0)) {
        continue;
      }
      RenderKey key = need.m_key;
      if (!reserve(key.pixels() * 4, key.pixels(), p_pass != RenderPass::Prefetch)) {
        if (p_pass != RenderPass::FirstPaint) {
          continue;
        }
        if (capacityWillBeReleased()) {
          return;
        }
        const qint64 pixels = firstPaintBudget(key);
        if (pixels > 0) {
          const qreal factor = std::sqrt(qreal(pixels) / key.pixels());
          const int width = qMax(1, static_cast<int>(std::floor(key.m_size.width() * factor)));
          const int height = qMax(1, static_cast<int>(std::floor(key.m_size.height() * factor)));
          key.m_size = QSize(width, height);
          // At extreme aspect ratios the one-pixel short side can exceed
          // the area budget. Clamp the long side without dropping below 1.
          if (key.pixels() > pixels) {
            if (width >= height) {
              key.m_size.setWidth(static_cast<int>(qMax(qint64(1), pixels / height)));
            } else {
              key.m_size.setHeight(static_cast<int>(qMax(qint64(1), pixels / width)));
            }
          }
        }
        if (pixels < 1 || !reserve(key.pixels() * 4, key.pixels())) {
          fail(p_slot,
               TypstPreviewPane::tr(
                   "The PDF image cache is full. Increase the preview memory or pixel limit."),
               true);
          return;
        }
      }
      p_slot.m_active = key;
      p_slot.m_request = p_slot.m_renderer->requestPage(key.m_page, key.m_size, key.m_options);
      if (!p_slot.m_request) {
        fail(p_slot, TypstPreviewPane::tr("Qt PDF could not schedule page %1 for rendering.")
                         .arg(key.m_page + 1));
      }
      return;
    }
  }

  void rendered(quint64 p_serial, int p_page, const QSize &p_size, const QImage &p_image,
                QPdfDocumentRenderOptions p_options, quint64 p_request) {
    PdfSlot *slot = slotBySerial(p_serial);
    if (!slot || !slot->m_request || p_request != slot->m_request) {
      return;
    }
    const RenderKey key = slot->m_active;
    slot->m_request = 0;
    if (!slot->m_obsolete && !slot->m_failed) {
      if (p_page != key.m_page || p_size != key.m_size || p_options != key.m_options ||
          p_image.isNull() || p_image.size() != key.m_size ||
          static_cast<qint64>(p_image.sizeInBytes()) > key.pixels() * 4) {
        fail(*slot,
             TypstPreviewPane::tr(
                 "Qt PDF returned an invalid image for page %1. The preview was not replaced.")
                 .arg(key.m_page + 1));
      } else {
        for (const auto &need : slot->m_needs) {
          if (key.usableFor(need.m_key) && key.pixels() * 4 <= m_maxBytes - usedBytes() &&
              key.pixels() <= m_maxPixels - imagePixels()) {
            CachedImage image;
            image.m_key = key;
            image.m_image = p_image;
            image.m_request = p_request;
            image.m_used = ++m_clock;
            slot->m_images.append(std::move(image));
            // The reservation covered both images at peak. Only now may
            // the best resident variant replace its same-page fallback.
            const int best = bestImageIndex(*slot, key.m_page);
            for (int i = slot->m_images.size() - 1; i >= 0; --i) {
              if (i != best && slot->m_images.at(i).m_key.usableFor(key)) {
                slot->m_images.removeAt(i);
              }
            }
            break;
          }
        }
      }
    }
    // Deletion/publication never happens inside a renderer signal stack.
    schedule();
    q->viewport()->update();
  }

  void tryPublish() {
    if (!m_pending || m_pending->m_obsolete || m_pending->m_failed || !m_pending->m_initialized ||
        !current(m_pending->m_snapshot) || m_pressed || !m_selection.m_text.isEmpty() ||
        m_pending->m_layout.m_revision != m_viewRevision ||
        m_pending->m_layout.m_dpr != q->viewport()->devicePixelRatioF()) {
      return;
    }
    for (const auto &need : m_pending->m_needs) {
      if (need.m_firstPaint && bestImageIndex(*m_pending, need.m_key.m_page) < 0) {
        return;
      }
    }
    if (m_displayed && m_displayed->m_request && m_retiring) {
      return;
    }
    if (m_displayed) {
      retire(m_displayed);
    }
    q->clearSourceHighlight();
    m_displayed = std::move(m_pending);
    clearSelection(false);
    m_searchIndex = -1;
    m_searchNavigatePending = false;
    m_focusedPage =
        qBound(0, m_anchor.m_page, static_cast<int>(m_displayed->m_pageSizes.size()) - 1);
    m_zoom = m_displayed->m_layout.m_zoom;
    m_error.clear();
    updateScrollBars();
    saveAnchor();
    m_displayed->m_search->setSearchString(m_searchText);
    q->viewport()->update();
    notifyUpdate();
    // Publication frees the old snapshot's images/PDF (or starts its
    // retirement), so reconsider ideal upgrades without another view event.
    schedule();
    emit q->snapshotPublished(m_displayed->m_snapshot->m_id);
  }

  void acceptSnapshot(const TypstSnapshot &p_snapshot, const QByteArray &p_pdf) {
    if (!current(p_snapshot)) {
      return;
    }
    if ((m_displayed && m_displayed->m_snapshot->m_id == p_snapshot->m_id &&
         m_displayed->m_snapshot->m_epoch == p_snapshot->m_epoch) ||
        (m_pending && !m_pending->m_obsolete && m_pending->m_snapshot->m_id == p_snapshot->m_id &&
         m_pending->m_snapshot->m_epoch == p_snapshot->m_epoch)) {
      return;
    }
    if (p_pdf.isEmpty() || p_pdf.size() > m_maxPdfBytes || p_pdf.size() >= m_maxBytes ||
        m_maxPixels <= 0) {
      report(TypstPreviewPane::tr(
          "The PDF preview is empty or exceeds the configured PDF, memory, or pixel limit."));
      return;
    }
    m_queuedSnapshot.clear();
    m_queuedPdf.clear();
    if (m_pending) {
      m_pending->m_obsolete = true;
      m_pending->m_needs.clear();
      m_pending->m_images.clear();
      // Ready idle slots need no asynchronous retirement.
      retire(m_pending);
    }
    if (!reserve(qint64(p_pdf.size()) + 4, 1)) {
      report(TypstPreviewPane::tr("There is not enough preview memory to retain this PDF alongside "
                                  "the displayed and retiring snapshots."));
      notifyUpdate();
      return;
    }
    m_queuedSnapshot = p_snapshot;
    m_queuedPdf = p_pdf; // QByteArray shares immutable storage with the bridge.
    schedule();
    notifyUpdate();
  }

  void clearSelection(bool p_publish) {
    const bool changed = m_selection.m_page >= 0;
    m_selection = OwnedSelection();
    m_pressed = false;
    m_dragging = false;
    m_autoScroll.stop();
    if (changed) {
      emit q->selectionChanged();
    }
    q->viewport()->update();
    if (p_publish) {
      schedule();
    }
  }

  void selectTo(const QPointF &p_position) {
    if (!m_displayed || m_selection.m_page < 0 || !m_dragging) {
      return;
    }
    m_selection.m_endpoint = pagePoint(*m_displayed, m_selection.m_page, p_position);
    const QPdfSelection selection = m_displayed->m_document->getSelection(
        m_selection.m_page, m_selection.m_anchor, m_selection.m_endpoint);
    m_selection.m_text = selection.text();
    m_selection.m_polygons = selection.bounds();
    emit q->selectionChanged();
    q->viewport()->update();
  }

  int linkAt(int p_page, const QPointF &p_point) const {
    if (!m_displayed) {
      return -1;
    }
    const auto &links = m_displayed->m_snapshot->m_links;
    for (int i = links.size() - 1; i >= 0; --i) {
      const auto &link = links.at(i);
      const bool internal =
          link.m_targetPage >= 0 && link.m_targetPage < m_displayed->m_pageSizes.size() &&
          finiteCoordinate(link.m_target.x()) && finiteCoordinate(link.m_target.y());
      if (link.m_page == p_page && link.m_rect.contains(p_point) &&
          (internal || (!link.m_url.isEmpty() && link.m_url.isValid()))) {
        return i;
      }
    }
    return -1;
  }

  void autoScroll() {
    if (!m_pressed || !m_dragging || !m_displayed || m_selection.m_page < 0) {
      m_autoScroll.stop();
      return;
    }
    const auto &layout = m_displayed->m_layout;
    const QRectF page = layout.m_pages.at(m_selection.m_page);
    auto speed = [](qreal p_position, int p_extent) {
      if (p_position < 24) {
        return qMax(qreal(-60), (p_position - 24) / 2);
      }
      if (p_position > p_extent - 24) {
        return qMin(qreal(60), (p_position - p_extent + 24) / 2);
      }
      return qreal(0);
    };
    QPointF offset = layout.m_offset + QPointF(speed(m_pointer.x(), q->viewport()->width()),
                                               speed(m_pointer.y(), q->viewport()->height()));
    const qreal minimumX = qMax(qreal(0), page.left() - q->viewport()->width() + PageMargin);
    const qreal minimumY = qMax(qreal(0), page.top() - q->viewport()->height() + PageMargin);
    const QPointF maximum = maximumOffset(layout);
    const qreal maximumX = qMax(minimumX, qMin(maximum.x(), page.right() - PageMargin));
    const qreal maximumY = qMax(minimumY, qMin(maximum.y(), page.bottom() - PageMargin));
    offset.setX(qBound(minimumX, offset.x(), maximumX));
    offset.setY(qBound(minimumY, offset.y(), maximumY));
    setOffset(offset);
    selectTo(m_pointer);
  }

  void showSearchDialog() {
    bool accepted = false;
    const QString text = QInputDialog::getText(q, TypstPreviewPane::tr("Find in preview"),
                                               TypstPreviewPane::tr("Search text:"),
                                               QLineEdit::Normal, m_searchText, &accepted);
    if (accepted) {
      q->setSearchText(text);
      q->findNext();
    }
  }

  void paint() {
    if (m_displayed && m_displayed->m_layout.m_dpr != q->viewport()->devicePixelRatioF()) {
      viewChanged(true);
    }
    QPainter painter(q->viewport());
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    if (m_background.isValid()) {
      painter.fillRect(q->viewport()->rect(), m_background);
    } else {
      painter.fillRect(q->viewport()->rect(), q->palette().brush(QPalette::Dark));
    }
    if (m_displayed) {
      auto &slot = *m_displayed;
      const auto pages = visiblePages(slot.m_layout);
      for (int page : pages) {
        const QRectF rect = slot.m_layout.m_pages.at(page).translated(-slot.m_layout.m_offset);
        painter.fillRect(rect, m_themeStyleEnabled && m_pageBackground.isValid()
                                   ? m_pageBackground
                                   : QColor(Qt::white));
        const int index = bestImageIndex(slot, page);
        if (index >= 0) {
          auto &image = slot.m_images[index];
          image.m_used = ++m_clock;
          painter.drawImage(rect, image.m_image, QRectF(image.m_image.rect()));
        } else {
          painter.save();
          painter.setPen(m_themeStyleEnabled && m_foreground.isValid() ? m_foreground
                                                                       : QColor(Qt::darkGray));
          painter.drawText(rect.intersected(q->viewport()->rect()), Qt::AlignCenter,
                           slot.m_failed
                               ? TypstPreviewPane::tr("Page could not be rendered")
                               : TypstPreviewPane::tr("Rendering page %1…").arg(page + 1));
          painter.restore();
        }
        painter.save();
        painter.setClipRect(rect);
        painter.translate(rect.topLeft());
        painter.scale(slot.m_layout.m_scale, slot.m_layout.m_scale);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(255, 205, 0, 85));
        const auto results = slot.m_search->resultsOnPage(page);
        for (const auto &result : results) {
          for (const auto &box : result.rectangles()) {
            painter.drawRect(box);
          }
        }
        if (m_searchIndex >= 0 && m_searchIndex < slot.m_search->rowCount(QModelIndex())) {
          const auto result = slot.m_search->resultAtIndex(m_searchIndex);
          if (result.page() == page) {
            painter.setBrush(QColor(255, 135, 0, 125));
            for (const auto &box : result.rectangles()) {
              painter.drawRect(box);
            }
          }
        }
        if (m_selection.m_snapshot == slot.m_snapshot->m_id && m_selection.m_page == page) {
          QColor color = q->palette().color(QPalette::Highlight);
          color.setAlpha(100);
          painter.setBrush(color);
          for (const auto &polygon : m_selection.m_polygons) {
            painter.drawPolygon(polygon);
          }
        }
        painter.restore();
        if (page == m_sourceFlashPage && m_sourceFlashPhases > 0 && m_sourceFlashPhases % 2 == 0) {
          const QPointF center = rect.topLeft() + m_sourceFlashPoint * slot.m_layout.m_scale;
          painter.save();
          painter.setClipRect(rect);
          painter.setRenderHint(QPainter::Antialiasing);
          QColor color = q->palette().color(QPalette::Highlight);
          painter.setPen(QPen(color, 2));
          color.setAlpha(80);
          painter.setBrush(color);
          painter.drawEllipse(center, 12, 12);
          painter.restore();
        }
      }
    }
    if (!m_error.isEmpty()) {
      const QRect bounds = q->viewport()->rect().adjusted(8, 8, -8, -8);
      const QRect text =
          painter.boundingRect(bounds, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, m_error);
      painter.fillRect(text.adjusted(-4, -3, 4, 3), q->palette().brush(QPalette::ToolTipBase));
      painter.setPen(q->palette().color(QPalette::ToolTipText));
      painter.drawText(bounds, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, m_error);
    }
  }

  TypstPreviewPane *q;
  qint64 m_maxPdfBytes;
  qint64 m_maxBytes;
  qint64 m_maxPixels;
  QColor m_background;
  QColor m_pageBackground;
  bool m_themeStyleEnabled;
  QColor m_foreground;
  std::unique_ptr<PdfSlot> m_displayed;
  std::unique_ptr<PdfSlot> m_pending;
  std::unique_ptr<PdfSlot> m_retiring;
  std::array<std::unique_ptr<PdfSlot>, 2> m_resetRetiring;
  TypstSnapshot m_queuedSnapshot;
  QByteArray m_queuedPdf;
  quint64 m_epoch = 0;
  quint64 m_generation = 0;
  quint64 m_nextSerial = 0;
  quint64 m_viewRevision = 1;
  quint64 m_clock = 0;
  ViewAnchor m_anchor;
  ZoomMode m_zoomMode = ZoomMode::Custom;
  qreal m_zoom;
  ZoomMode m_notifiedZoomMode = ZoomMode::Custom;
  qreal m_notifiedZoom;
  OwnedSelection m_selection;
  QString m_searchText;
  QString m_error;
  int m_searchIndex = -1;
  int m_focusedPage = -1;
  int m_pressedLink = -1;
  QPointF m_pressPosition;
  QPointF m_pointer;
  QTimer m_autoScroll;
  QTimer m_sourceFlash;
  int m_sourceFlashPhases = 0;
  int m_sourceFlashPage = -1;
  QPointF m_sourceFlashPoint;
  bool m_pressed = false;
  bool m_dragging = false;
  bool m_controlClick = false;
  bool m_scheduled = false;
  bool m_servicing = false;
  bool m_closing = false;
  bool m_needsDirty = true;
  bool m_settingScrollBars = false;
  bool m_updateAvailable = false;
  bool m_searchNavigatePending = false;
  bool m_searchBackwards = false;
};

TypstPreviewPane::TypstPreviewPane(const TypstEditorConfig &p_config, QWidget *p_parent)
    : QAbstractScrollArea(p_parent), m_d(new TypstPreviewPanePrivate(this, p_config)) {
  setFocusPolicy(Qt::StrongFocus);
  viewport()->setMouseTracking(true);
  viewport()->setAutoFillBackground(false);
  // Stable gutters prevent a page-count change from resizing the viewport
  // between first-paint preparation and atomic snapshot publication.
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
  setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
  setAccessibleName(tr("Typst PDF preview"));
}

TypstPreviewPane::~TypstPreviewPane() = default;

void TypstPreviewPane::resetForNewDocument() { m_d->resetForNewDocument(); }

void TypstPreviewPane::applyConfig(const TypstEditorConfig &p_config) {
  m_d->m_maxPdfBytes = qMax(qint64(0), p_config.m_maxPdfBytes);
  m_d->m_maxBytes = qMax(qint64(0), p_config.m_maxCacheBytes);
  m_d->m_maxPixels = qMax(0, p_config.m_maxRasterPixels);
  m_d->m_background = p_config.m_pdfBackground;
  m_d->m_pageBackground = p_config.getPreviewBackground();
  m_d->m_themeStyleEnabled = p_config.m_enableThemeStyleInPreview;
  m_d->m_foreground = p_config.m_previewForeground;
  if (m_d->m_queuedPdf.size() > m_d->m_maxPdfBytes) {
    m_d->m_queuedSnapshot.clear();
    m_d->m_queuedPdf.clear();
  }
  if (m_d->m_pending && m_d->m_pending->m_pdf.size() > m_d->m_maxPdfBytes) {
    m_d->m_pending->m_obsolete = true;
  }
  for (auto slot : m_d->allSlots()) {
    if (slot) {
      if (slot->m_resourceFailure) {
        slot->m_failed = false;
        slot->m_resourceFailure = false;
        m_d->m_error.clear();
      }
    }
  }
  // Lowering a limit may require evicting even visible images. Selection and
  // point-space geometry are independent of image residency and stay intact.
  if (!m_d->reserve(0, 0)) {
    for (auto slot : m_d->allSlots()) {
      if (slot) {
        slot->m_images.clear();
      }
    }
  }
  if (m_d->usedBytes() > m_d->m_maxBytes || m_d->imagePixels() > m_d->m_maxPixels ||
      (m_d->m_displayed && m_d->m_displayed->m_pdf.size() > m_d->m_maxPdfBytes)) {
    m_d->report(tr("The new preview limits are smaller than a retained PDF or active render. "
                   "The displayed document and selection are retained; increase the limits to "
                   "render more pages."));
  }
  m_d->m_needsDirty = true;
  m_d->schedule();
  m_d->notifyUpdate();
}

void TypstPreviewPane::setCurrentGeneration(quint64 p_epoch, quint64 p_generation) {
  clearSourceHighlight();
  m_d->m_epoch = p_epoch;
  m_d->m_generation = p_generation;
  if (m_d->m_pending && !m_d->current(m_d->m_pending->m_snapshot)) {
    m_d->m_pending->m_obsolete = true;
    m_d->m_pending->m_needs.clear();
  }
  if (m_d->m_queuedSnapshot && !m_d->current(m_d->m_queuedSnapshot)) {
    m_d->m_queuedSnapshot.clear();
    m_d->m_queuedPdf.clear();
  }
  m_d->schedule();
  m_d->notifyUpdate();
}

void TypstPreviewPane::setSnapshot(const TypstSnapshot &p_snapshot, const QByteArray &p_pdf) {
  m_d->acceptSnapshot(p_snapshot, p_pdf);
}

TypstSnapshot TypstPreviewPane::snapshot() const {
  return m_d->m_displayed ? m_d->m_displayed->m_snapshot : TypstSnapshot();
}

QString TypstPreviewPane::selectedText() const { return m_d->m_selection.m_text; }

bool TypstPreviewPane::hasSelection() const { return !m_d->m_selection.m_text.isEmpty(); }

bool TypstPreviewPane::hasPendingUpdate() const { return m_d->pendingUpdate(); }

void TypstPreviewPane::publishPending() { m_d->clearSelection(true); }

void TypstPreviewPane::clearSelection() { m_d->clearSelection(true); }

void TypstPreviewPane::copy() {
  if (hasSelection()) {
    QApplication::clipboard()->setText(selectedText());
  }
}

void TypstPreviewPane::selectPageText() {
  if (!m_d->m_displayed) {
    return;
  }
  auto &slot = *m_d->m_displayed;
  const int page = m_d->m_focusedPage >= 0 ? qBound(0, m_d->m_focusedPage,
                                                    static_cast<int>(slot.m_pageSizes.size()) - 1)
                                           : currentPage();
  if (page < 0) {
    return;
  }
  m_d->clearSelection(false);
  const QPdfSelection selection = slot.m_document->getAllText(page);
  m_d->m_selection.m_snapshot = slot.m_snapshot->m_id;
  m_d->m_selection.m_page = page;
  m_d->m_selection.m_endpoint =
      QPointF(slot.m_pageSizes.at(page).width(), slot.m_pageSizes.at(page).height());
  m_d->m_selection.m_text = selection.text();
  m_d->m_selection.m_polygons = selection.bounds();
  emit selectionChanged();
  viewport()->update();
  m_d->schedule();
}

void TypstPreviewPane::setZoom(qreal p_zoom) {
  m_d->changeZoom(TypstPreviewPanePrivate::ZoomMode::Custom, p_zoom,
                  QPointF(viewport()->width() / 2.0, viewport()->height() / 2.0));
}

qreal TypstPreviewPane::zoom() const { return m_d->m_zoom; }

VTypstEditor::PdfZoomMode TypstPreviewPane::zoomMode() const { return m_d->m_zoomMode; }

void TypstPreviewPane::fitWidth() {
  m_d->changeZoom(TypstPreviewPanePrivate::ZoomMode::Width, m_d->m_zoom,
                  QPointF(viewport()->width() / 2.0, viewport()->height() / 2.0));
}

void TypstPreviewPane::fitPage() {
  m_d->changeZoom(TypstPreviewPanePrivate::ZoomMode::Page, m_d->m_zoom,
                  QPointF(viewport()->width() / 2.0, viewport()->height() / 2.0));
}

void TypstPreviewPane::setSearchText(const QString &p_text) {
  if (m_d->m_searchText == p_text) {
    return;
  }
  m_d->m_searchText = p_text;
  m_d->m_searchIndex = -1;
  m_d->m_searchNavigatePending = false;
  if (m_d->m_displayed) {
    m_d->m_displayed->m_search->setSearchString(p_text);
  }
  viewport()->update();
}

void TypstPreviewPane::findNext(bool p_backwards) {
  if (!m_d->m_displayed || m_d->m_searchText.isEmpty()) {
    return;
  }
  auto &slot = *m_d->m_displayed;
  const int count = slot.m_search->rowCount(QModelIndex());
  if (!count) {
    m_d->m_searchNavigatePending = true;
    m_d->m_searchBackwards = p_backwards;
    return;
  }
  m_d->m_searchIndex = m_d->m_searchIndex < 0
                           ? (p_backwards ? count - 1 : 0)
                           : (m_d->m_searchIndex + (p_backwards ? count - 1 : 1)) % count;
  const auto result = slot.m_search->resultAtIndex(m_d->m_searchIndex);
  goToPage(result.page(), result.location());
}

void TypstPreviewPane::clearSourceHighlight() {
  const bool visible = m_d->m_sourceFlashPhases > 0 && m_d->m_sourceFlashPhases % 2 == 0;
  m_d->m_sourceFlash.stop();
  m_d->m_sourceFlashPhases = 0;
  m_d->m_sourceFlashPage = -1;
  if (visible) {
    viewport()->update();
  }
}

void TypstPreviewPane::flashSourceLocation(quint64 p_snapshot, int p_page, const QPointF &p_point) {
  clearSourceHighlight();
  if (!m_d->m_displayed || !m_d->current(m_d->m_displayed->m_snapshot) ||
      m_d->m_displayed->m_snapshot->m_id != p_snapshot || p_page < 0 ||
      p_page >= m_d->m_displayed->m_pageSizes.size() || !finiteCoordinate(p_point.x()) ||
      !finiteCoordinate(p_point.y())) {
    return;
  }
  const auto &layout = m_d->m_displayed->m_layout;
  const QRectF pagePoints(QPointF(), m_d->m_displayed->m_pageSizes.at(p_page));
  if (!pagePoints.contains(p_point)) {
    return;
  }
  const QPointF center = layout.m_pages.at(p_page).topLeft() + p_point * layout.m_scale;
  if (!QRectF(viewport()->rect()).adjusted(16, 16, -16, -16).contains(center - layout.m_offset)) {
    m_d->setOffset(center - QPointF(viewport()->width() / 2.0, viewport()->height() / 2.0));
  }
  m_d->m_sourceFlashPage = p_page;
  m_d->m_sourceFlashPoint = p_point;
  m_d->m_sourceFlashPhases = 6;
  m_d->m_sourceFlash.start();
  viewport()->update();
}

void TypstPreviewPane::goToPage(int p_page, const QPointF &p_point) {
  if (!m_d->m_displayed || p_page < 0 || p_page >= m_d->m_displayed->m_pageSizes.size() ||
      !finiteCoordinate(p_point.x()) || !finiteCoordinate(p_point.y())) {
    return;
  }
  auto &slot = *m_d->m_displayed;
  const qreal previousY = slot.m_layout.m_offset.y();
  const QSizeF size = slot.m_pageSizes.at(p_page);
  const QPointF point(qBound(qreal(0), p_point.x(), size.width()),
                      qBound(qreal(0), p_point.y(), size.height()));
  m_d->m_focusedPage = p_page;
  m_d->m_anchor.m_page = p_page;
  m_d->m_anchor.m_point = point;
  m_d->m_anchor.m_viewport = QPointF(PageMargin, PageMargin);
  m_d->viewChanged(true);
  if (slot.m_layout.m_offset.y() != previousY) {
    emit verticalScrollChanged();
  }
  m_d->notifyZoomChanged();
}

int TypstPreviewPane::currentPage() const {
  return m_d->m_displayed
             ? m_d->nearestPage(m_d->m_displayed->m_layout,
                                QPointF(viewport()->width() / 2.0, viewport()->height() / 2.0))
             : -1;
}

bool TypstPreviewPane::event(QEvent *p_event) {
  const bool result = QAbstractScrollArea::event(p_event);
  if (m_d && (p_event->type() == QEvent::ScreenChangeInternal
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
              || p_event->type() == QEvent::DevicePixelRatioChange
#endif
              )) {
    m_d->viewChanged(true);
    m_d->notifyZoomChanged();
  }
  return result;
}

void TypstPreviewPane::paintEvent(QPaintEvent *) {
  m_d->paint();
  m_d->notifyZoomChanged();
}

void TypstPreviewPane::resizeEvent(QResizeEvent *p_event) {
  QAbstractScrollArea::resizeEvent(p_event);
  m_d->viewChanged(true);
  m_d->notifyZoomChanged();
}

void TypstPreviewPane::scrollContentsBy(int p_dx, int p_dy) {
  if (!m_d || m_d->m_settingScrollBars || !m_d->m_displayed) {
    return;
  }
  const QPointF maximum = m_d->maximumOffset(m_d->m_displayed->m_layout);
  auto offset = m_d->m_displayed->m_layout.m_offset;
  // Do not quantize an unchanged axis through its integer scrollbar.
  if (p_dx)
    offset.setX(fromScrollValue(horizontalScrollBar()->value(), maximum.x()));
  if (p_dy)
    offset.setY(fromScrollValue(verticalScrollBar()->value(), maximum.y()));
  m_d->setOffset(offset);
}

void TypstPreviewPane::mousePressEvent(QMouseEvent *p_event) {
  if (p_event->button() != Qt::LeftButton || !m_d->m_displayed) {
    QAbstractScrollArea::mousePressEvent(p_event);
    return;
  }
  const QPointF position = p_event->position();
  const int page = m_d->hitPage(position);
  if (page < 0) {
    clearSelection();
    p_event->accept();
    return;
  }
  setFocus(Qt::MouseFocusReason);
  m_d->clearSelection(false);
  m_d->m_focusedPage = page;
  m_d->m_selection.m_snapshot = m_d->m_displayed->m_snapshot->m_id;
  m_d->m_selection.m_page = page;
  m_d->m_selection.m_anchor = m_d->pagePoint(*m_d->m_displayed, page, position);
  m_d->m_selection.m_endpoint = m_d->m_selection.m_anchor;
  m_d->m_pressed = true;
  m_d->m_controlClick = p_event->modifiers().testFlag(Qt::ControlModifier);
  m_d->m_pressPosition = position;
  m_d->m_pointer = position;
  m_d->m_pressedLink = m_d->linkAt(page, m_d->m_selection.m_anchor);
  p_event->accept();
}

void TypstPreviewPane::mouseMoveEvent(QMouseEvent *p_event) {
  const QPointF position = p_event->position();
  m_d->m_pointer = position;
  if (m_d->m_pressed) {
    if ((position - m_d->m_pressPosition).manhattanLength() >= QApplication::startDragDistance()) {
      m_d->m_dragging = true;
    }
    if (m_d->m_dragging && !m_d->m_controlClick) {
      m_d->selectTo(position);
      m_d->m_autoScroll.start();
    }
    p_event->accept();
    return;
  }
  const int page = m_d->hitPage(position);
  const bool link =
      page >= 0 && m_d->linkAt(page, m_d->pagePoint(*m_d->m_displayed, page, position)) >= 0;
  viewport()->setCursor(link ? Qt::PointingHandCursor
                             : (page >= 0 ? Qt::IBeamCursor : Qt::ArrowCursor));
  QAbstractScrollArea::mouseMoveEvent(p_event);
}

void TypstPreviewPane::mouseReleaseEvent(QMouseEvent *p_event) {
  if (p_event->button() != Qt::LeftButton || !m_d->m_pressed || !m_d->m_displayed) {
    QAbstractScrollArea::mouseReleaseEvent(p_event);
    return;
  }
  const QPointF position = p_event->position();
  const int page = m_d->m_selection.m_page;
  const QPointF point = m_d->pagePoint(*m_d->m_displayed, page, position);
  m_d->m_dragging = m_d->m_dragging || (position - m_d->m_pressPosition).manhattanLength() >=
                                           QApplication::startDragDistance();
  const bool click = !m_d->m_dragging;
  if (m_d->m_dragging && !m_d->m_controlClick) {
    m_d->selectTo(position);
  }
  m_d->m_pressed = false;
  m_d->m_dragging = false;
  m_d->m_autoScroll.stop();
  // Ordinary clicks navigate source; links and drag selection keep their own gestures.
  const QPointer<TypstPreviewPane> alive(this);
  if (click && m_d->hitPage(position) == page && (m_d->m_controlClick || m_d->m_pressedLink < 0)) {
    if (m_d->current(m_d->m_displayed->m_snapshot)) {
      emit sourceLocationRequested(m_d->m_displayed->m_snapshot->m_id, page, point);
    } else {
      QToolTip::showText(p_event->globalPos(),
                         tr("Source navigation is unavailable for an out-of-date preview."), this);
    }
  } else if (click && m_d->m_pressedLink >= 0 && m_d->hitPage(position) == page &&
             m_d->linkAt(page, point) == m_d->m_pressedLink) {
    const TypstLink link = m_d->m_displayed->m_snapshot->m_links.at(m_d->m_pressedLink);
    if (link.m_targetPage >= 0 && link.m_targetPage < m_d->m_displayed->m_pageSizes.size()) {
      goToPage(link.m_targetPage, link.m_target);
    } else if (!link.m_url.isEmpty()) {
      emit externalLinkRequested(link.m_url);
    }
  }
  if (alive)
    m_d->schedule();
  p_event->accept();
}

void TypstPreviewPane::mouseDoubleClickEvent(QMouseEvent *p_event) {
  const int page = m_d->hitPage(p_event->position());
  if (p_event->button() == Qt::LeftButton && page >= 0) {
    const auto snapshot = m_d->m_displayed->m_snapshot;
    const auto context =
        typstResourceAt(snapshot, TypstResourceContext::Surface::Preview, page,
                        m_d->pagePoint(*m_d->m_displayed, page, p_event->position()));
    if (context.m_imageId && context.m_linkIndex < 0) {
      m_d->m_pressed = false;
      m_d->m_dragging = false;
      m_d->m_autoScroll.stop();
      p_event->accept();
      emit imageActivated(context);
      return;
    }
  }
  QAbstractScrollArea::mouseDoubleClickEvent(p_event);
}

void TypstPreviewPane::keyPressEvent(QKeyEvent *p_event) {
  if (p_event->matches(QKeySequence::Copy)) {
    copy();
  } else if (p_event->key() == Qt::Key_Escape) {
    clearSelection();
  } else if (p_event->matches(QKeySequence::SelectAll)) {
    selectPageText();
  } else if (p_event->matches(QKeySequence::Find)) {
    m_d->showSearchDialog();
  } else if (p_event->matches(QKeySequence::FindNext) || p_event->key() == Qt::Key_F3) {
    findNext(p_event->modifiers().testFlag(Qt::ShiftModifier));
  } else if (p_event->matches(QKeySequence::FindPrevious)) {
    findNext(true);
  } else if (p_event->matches(QKeySequence::ZoomIn)) {
    setZoom(zoom() * 1.2);
  } else if (p_event->matches(QKeySequence::ZoomOut)) {
    setZoom(zoom() / 1.2);
  } else if (m_d->m_displayed) {
    const QPointF offset = m_d->m_displayed->m_layout.m_offset;
    switch (p_event->key()) {
    case Qt::Key_Home:
      goToPage(0);
      break;
    case Qt::Key_End:
      goToPage(m_d->m_displayed->m_pageSizes.size() - 1);
      break;
    case Qt::Key_PageUp:
      m_d->setOffset(offset - QPointF(0, viewport()->height() * 0.9));
      break;
    case Qt::Key_PageDown:
      m_d->setOffset(offset + QPointF(0, viewport()->height() * 0.9));
      break;
    case Qt::Key_Up:
      m_d->setOffset(offset - QPointF(0, 30));
      break;
    case Qt::Key_Down:
      m_d->setOffset(offset + QPointF(0, 30));
      break;
    case Qt::Key_Left:
      m_d->setOffset(offset - QPointF(30, 0));
      break;
    case Qt::Key_Right:
      m_d->setOffset(offset + QPointF(30, 0));
      break;
    default:
      QAbstractScrollArea::keyPressEvent(p_event);
      return;
    }
  } else {
    QAbstractScrollArea::keyPressEvent(p_event);
    return;
  }
  p_event->accept();
}

void TypstPreviewPane::wheelEvent(QWheelEvent *p_event) {
  if (p_event->modifiers().testFlag(Qt::ControlModifier)) {
    const int delta = p_event->angleDelta().y();
    if (delta) {
      m_d->changeZoom(TypstPreviewPanePrivate::ZoomMode::Custom,
                      zoom() * std::pow(1.2, delta / 120.0), p_event->position());
    }
    p_event->accept();
  } else {
    QAbstractScrollArea::wheelEvent(p_event);
  }
}

void TypstPreviewPane::contextMenuEvent(QContextMenuEvent *p_event) {
  const int page = m_d->hitPage(p_event->pos());
  TypstResourceContext context;
  if (page >= 0) {
    m_d->m_focusedPage = page;
    context = typstResourceAt(m_d->m_displayed->m_snapshot, TypstResourceContext::Surface::Preview,
                              page, m_d->pagePoint(*m_d->m_displayed, page, p_event->pos()));
  }
  const QPointer<TypstPreviewPane> alive(this);
  const QPointer<QMenu> menu(new QMenu(this));
  const auto cleanup = qScopeGuard([menu]() { delete menu.data(); });
  connect(this, &QObject::destroyed, menu, &QMenu::close);
  auto copyAction = menu->addAction(tr("Copy"), this, &TypstPreviewPane::copy);
  copyAction->setShortcut(QKeySequence::Copy);
  copyAction->setEnabled(hasSelection());
  auto selectAction =
      menu->addAction(tr("Select page text"), this, &TypstPreviewPane::selectPageText);
  selectAction->setEnabled(bool(m_d->m_displayed));
  auto clearAction =
      menu->addAction(tr("Clear selection"), this, &TypstPreviewPane::clearSelection);
  clearAction->setEnabled(hasSelection() || m_d->m_pressed);
  menu->addSeparator();
  menu->addAction(tr("Fit width"), this, &TypstPreviewPane::fitWidth);
  menu->addAction(tr("Fit page"), this, &TypstPreviewPane::fitPage);
  p_event->accept();
  emit resourceContextMenuRequested(context, menu);
  if (alive && alive->isVisible() && alive->isEnabled() && menu)
    menu->exec(p_event->globalPos());
}
