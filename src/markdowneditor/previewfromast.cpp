#include "previewfromast.h"

#include "previewbuilder.h"

using namespace vte;

PreviewTableAlignment vte::toPreviewAlignment(int p_cmarkAlignment) {
  switch (p_cmarkAlignment) {
  case 1:
    return PreviewTableAlignment::Left;
  case 2:
    return PreviewTableAlignment::Center;
  case 3:
    return PreviewTableAlignment::Right;
  default:
    return PreviewTableAlignment::None;
  }
}

QSharedPointer<const Preview> vte::createTablePreview(quint64 p_revision, int p_startPos,
                                                      int p_endPos, const QString &p_source,
                                                      const md::TableElement &p_element) {
  // The header comes first, the delimiter row is kept in its own field, and
  // the body rows follow. TablePreview::rowPrefixes() carries exactly one
  // entry per emitted row, in that order.
  QVector<QVector<QString>> cells;
  QVector<QString> rowPrefixes;
  QString delimiterPrefix;
  for (const auto &row : p_element.m_rows) {
    if (row.m_type == md::TableRowType::Delimiter) {
      delimiterPrefix = row.m_prefix;
      continue;
    }

    cells.append(row.m_cells);
    rowPrefixes.append(row.m_prefix);
  }

  QVector<PreviewTableAlignment> alignments;
  alignments.reserve(p_element.m_alignments.size());
  for (int alignment : p_element.m_alignments) {
    alignments.append(toPreviewAlignment(alignment));
  }

  return PreviewBuilder::createTable(p_revision, p_startPos, p_endPos, p_source,
                                     p_element.m_columns, cells, alignments, rowPrefixes,
                                     delimiterPrefix);
}
