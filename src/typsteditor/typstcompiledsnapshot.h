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

struct TypstCompiledSnapshot {
  quint64 m_epoch = 0;
  quint64 m_generation = 0;
  quint64 m_id = 0;
  QString m_activeFile;
  QVector<QSizeF> m_pages;
  QVector<TypstPreviewOccurrence> m_previews;
  QVector<TypstLink> m_links;
  QVector<TypstHeadingInfo> m_headings;
  QVector<TypstDiagnostic> m_diagnostics;
  QVector<TypstHighlight> m_highlights;
  QStringList m_dependencies;
};

using TypstSnapshot = QSharedPointer<const TypstCompiledSnapshot>;

} // namespace vte

Q_DECLARE_METATYPE(vte::TypstSnapshot)

#endif // VTE_TYPSTCOMPILEDSNAPSHOT_H
