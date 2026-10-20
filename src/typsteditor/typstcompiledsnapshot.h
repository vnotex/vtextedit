#ifndef VTE_TYPSTCOMPILEDSNAPSHOT_H
#define VTE_TYPSTCOMPILEDSNAPSHOT_H

#include <vtextedit/vtypsteditor.h>

#include <QMetaType>
#include <QPointF>
#include <QRectF>
#include <QSharedPointer>
#include <QSizeF>
#include <QStringList>
#include <QUrl>
#include <QVector>
#include <limits>

namespace vte {

struct TypstSourceRange {
  QString m_file;
  int m_start = -1;
  int m_end = -1;
  bool isValid() const { return !m_file.isEmpty() && m_start >= 0 && m_end >= m_start; }
};

struct TypstHighlight {
  TypstSourceRange m_source;
  QString m_kind;
};

struct TypstDiagnostic {
  TypstSourceRange m_source;
  QString m_severity;
  QString m_message;
};

struct TypstPreviewRegion {
  int m_page = -1;
  QRectF m_rect;
};

struct TypstPreviewOccurrence {
  quint64 m_id = 0;
  quint64 m_parent = 0;
  QString m_kind;
  QString m_status;
  bool m_block = false;
  TypstSourceRange m_source;
  QVector<TypstPreviewRegion> m_regions;
};

struct TypstLink {
  int m_page = -1;
  QRectF m_rect;
  int m_targetPage = -1;
  QPointF m_target;
  QUrl m_url;
};

struct TypstImageResize {
  int m_argumentsEnd = -1;
  bool m_trailingComma = false;
  int m_widthStart = -1;
  int m_widthEnd = -1;
  int m_heightStart = -1;
  int m_heightEnd = -1;
  double m_widthPoints = 0;
  double m_heightPoints = 0;
};

struct TypstImageResource {
  quint64 m_id = 0;
  int m_previewIndex = -1;
  TypstSourceRange m_source;
  QString m_path;
  QSize m_size;
  TypstImageResize m_resize;
};

struct TypstSourceLink {
  TypstSourceRange m_source;
  TypstLink m_link;
};

struct TypstCompiledSnapshot {
  quint64 m_epoch = 0;
  quint64 m_generation = 0;
  quint64 m_id = 0;
  QString m_activeFile;
  QVector<QSizeF> m_pages;
  QVector<TypstPreviewOccurrence> m_previews;
  QVector<TypstLink> m_links;
  QVector<TypstImageResource> m_images;
  QVector<TypstSourceLink> m_sourceLinks;
  QVector<TypstHeadingInfo> m_headings;
  QVector<TypstDiagnostic> m_diagnostics;
  QVector<TypstHighlight> m_highlights;
  QStringList m_dependencies;
};

using TypstSnapshot = QSharedPointer<const TypstCompiledSnapshot>;

inline const TypstImageResource *typstImage(const TypstSnapshot &p_snapshot, quint64 p_id) {
  if (p_snapshot && p_id) {
    for (const auto &image : p_snapshot->m_images) {
      if (image.m_id == p_id)
        return &image;
    }
  }
  return nullptr;
}

inline TypstResourceContext typstResourceContext(const TypstSnapshot &p_snapshot,
                                                 TypstResourceContext::Surface p_surface,
                                                 quint64 p_image, int p_link) {
  TypstResourceContext context;
  context.m_surface = p_surface;
  if (!p_snapshot)
    return context;
  context.m_epoch = p_snapshot->m_epoch;
  context.m_generation = p_snapshot->m_generation;
  context.m_snapshot = p_snapshot->m_id;
  if (const auto image = typstImage(p_snapshot, p_image)) {
    context.m_imageId = image->m_id;
    context.m_imagePath = image->m_path;
    context.m_imageSize = image->m_size;
    context.m_width = qRound(image->m_resize.m_widthPoints * 96.0 / 72.0);
    context.m_height = qRound(image->m_resize.m_heightPoints * 96.0 / 72.0);
    context.m_imageResizable = image->m_resize.m_argumentsEnd >= 0;
    context.m_sourceFile = image->m_source.m_file;
    context.m_sourceStart = image->m_source.m_start;
    context.m_sourceEnd = image->m_source.m_end;
  }
  const TypstLink *link = nullptr;
  if (p_surface == TypstResourceContext::Surface::Source) {
    if (p_link >= 0 && p_link < p_snapshot->m_sourceLinks.size()) {
      const auto &sourceLink = p_snapshot->m_sourceLinks[p_link];
      link = &sourceLink.m_link;
      if (!context.m_imageId) {
        context.m_sourceFile = sourceLink.m_source.m_file;
        context.m_sourceStart = sourceLink.m_source.m_start;
        context.m_sourceEnd = sourceLink.m_source.m_end;
      }
    }
  } else if (p_link >= 0 && p_link < p_snapshot->m_links.size()) {
    link = &p_snapshot->m_links[p_link];
  }
  if (link) {
    context.m_linkIndex = p_link;
    context.m_linkUrl = link->m_url;
    context.m_linkPage = link->m_targetPage;
    context.m_linkPoint = link->m_target;
  }
  return context;
}

// Geometry is compiler-authored, including transformed/clipped nested image occurrences.
inline TypstResourceContext typstResourceAt(const TypstSnapshot &p_snapshot,
                                            TypstResourceContext::Surface p_surface, int p_page,
                                            const QPointF &p_point) {
  quint64 image = 0;
  int link = -1;
  qreal smallest = std::numeric_limits<qreal>::max();
  if (p_snapshot) {
    for (const auto &resource : p_snapshot->m_images) {
      const auto &occurrence = p_snapshot->m_previews[resource.m_previewIndex];
      for (const auto &region : occurrence.m_regions) {
        const qreal area = region.m_rect.width() * region.m_rect.height();
        if (region.m_page == p_page && region.m_rect.contains(p_point) && area <= smallest) {
          image = occurrence.m_id;
          smallest = area;
        }
      }
    }
    for (int i = p_snapshot->m_links.size() - 1; i >= 0; --i) {
      const auto &record = p_snapshot->m_links[i];
      if (record.m_page == p_page && record.m_rect.contains(p_point)) {
        link = i;
        break;
      }
    }
  }
  return typstResourceContext(p_snapshot, p_surface, image, link);
}

} // namespace vte

Q_DECLARE_METATYPE(vte::TypstSnapshot)

#endif // VTE_TYPSTCOMPILEDSNAPSHOT_H
