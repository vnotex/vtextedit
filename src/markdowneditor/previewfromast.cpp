#include "previewfromast.h"

#include "hlformatresolver.h"
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
                                                      const md::TableElement &p_element,
                                                      const QVector<QTextCharFormat> &p_styles) {
  // The header comes first, the delimiter row is kept in its own field, and
  // the body rows follow. TablePreview::rowPrefixes() carries exactly one
  // entry per emitted row, in that order.
  QVector<QVector<QString>> cells;
  QVector<QVector<QVector<PreviewFormatRun>>> cellFormats;
  QVector<QString> rowPrefixes;
  QString delimiterPrefix;
  for (const auto &row : p_element.m_rows) {
    if (row.m_type == md::TableRowType::Delimiter) {
      delimiterPrefix = row.m_prefix;
      continue;
    }

    cells.append(row.m_cells);
    rowPrefixes.append(row.m_prefix);

    // Same order and raggedness as the cells of this row.
    QVector<QVector<PreviewFormatRun>> rowFormats;
    rowFormats.reserve(row.m_cells.size());
    for (int c = 0; c < row.m_cells.size(); ++c) {
      rowFormats.append(md::resolveFormatRuns(row.m_cellHighlights.value(c), p_styles));
    }
    cellFormats.append(rowFormats);
  }

  QVector<PreviewTableAlignment> alignments;
  alignments.reserve(p_element.m_alignments.size());
  for (int alignment : p_element.m_alignments) {
    alignments.append(toPreviewAlignment(alignment));
  }

  return PreviewBuilder::createTable(p_revision, p_startPos, p_endPos, p_source,
                                     p_element.m_columns, cells, alignments, rowPrefixes,
                                     delimiterPrefix, cellFormats);
}
