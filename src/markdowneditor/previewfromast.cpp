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

// Project an HTML table's source-order rows onto the rectangular logical grid.
//
// The scanner has already proved the grid tiles exactly (decision D-i), so this
// only has to place what it was given: the origin's text, formats and verbatim
// tag at its origin slot, and an EMPTY string at every covered slot -- which is
// exactly what TablePreview::cells() is documented to hold for an HTML table.
static void buildHtmlGrid(const md::TableElement &p_element, TableSnapshotData &p_data) {
  const int rows = p_data.m_gridRowCount;
  const int cols = p_data.m_gridColumnCount;

  QVector<QVector<QString>> cells(rows, QVector<QString>(cols));
  QVector<QVector<QVector<PreviewFormatRun>>> cellFormats(rows,
                                                          QVector<QVector<PreviewFormatRun>>(cols));
  QVector<QVector<QString>> cellTags(rows, QVector<QString>(cols));

  p_data.m_rowTags.resize(rows);

  for (int r = 0; r < rows && r < p_element.m_rows.size(); ++r) {
    const auto &row = p_element.m_rows.at(r);
    p_data.m_rowTags[r] = row.m_rowTag;

    for (int i = 0; i < row.m_cells.size(); ++i) {
      const int col = row.m_slotColumns.value(i, i);
      const int rowSpan = qMax(1, row.m_rowSpans.value(i, 1));
      const int colSpan = qMax(1, row.m_colSpans.value(i, 1));
      if (col < 0 || col >= cols) {
        continue;
      }

      cells[r][col] = row.m_cells.at(i);
      cellFormats[r][col] = p_data.m_cellFormats.value(r).value(i);
      cellTags[r][col] = row.m_cellTags.value(i);

      for (int dr = 0; dr < rowSpan && r + dr < rows; ++dr) {
        for (int dc = 0; dc < colSpan && col + dc < cols; ++dc) {
          auto &slot = p_data.m_slots[(r + dr) * cols + col + dc];
          slot.m_originRow = r;
          slot.m_originColumn = col;
          slot.m_rowSpan = rowSpan;
          slot.m_colSpan = colSpan;
        }
      }
    }
  }

  p_data.m_cells = cells;
  p_data.m_cellFormats = cellFormats;
  p_data.m_cellTags = cellTags;
}

QSharedPointer<const Preview> vte::createTablePreview(quint64 p_revision, int p_startPos,
                                                      int p_endPos, const QString &p_source,
                                                      const md::TableElement &p_element,
                                                      const QVector<QTextCharFormat> &p_styles) {
  TableSnapshotData data;
  data.m_syntax = p_element.m_syntax == md::TableElement::Syntax::Html
                      ? PreviewTableSyntax::Html
                      : PreviewTableSyntax::Markdown;
  data.m_markdownBacked = p_element.m_markdownBacked;
  data.m_hasHeaderRow = p_element.m_hasHeaderRow;
  data.m_columnCount = p_element.m_columns;
  data.m_openTag = p_element.m_openTag;

  // The header comes first, the delimiter row is kept in its own field, and
  // the body rows follow. TablePreview::rowPrefixes() carries exactly one
  // entry per emitted row, in that order. An HTML table emits no delimiter row
  // at all and carries no prefixes (D-a).
  for (const auto &row : p_element.m_rows) {
    if (row.m_type == md::TableRowType::Delimiter) {
      data.m_delimiterPrefix = row.m_prefix;
      continue;
    }

    data.m_cells.append(row.m_cells);
    data.m_rowPrefixes.append(row.m_prefix);

    // Same order and raggedness as the cells of this row.
    QVector<QVector<PreviewFormatRun>> rowFormats;
    rowFormats.reserve(row.m_cells.size());
    for (int c = 0; c < row.m_cells.size(); ++c) {
      rowFormats.append(md::resolveFormatRuns(row.m_cellHighlights.value(c), p_styles));
    }
    data.m_cellFormats.append(rowFormats);
  }

  data.m_alignments.reserve(p_element.m_alignments.size());
  for (int alignment : p_element.m_alignments) {
    data.m_alignments.append(toPreviewAlignment(alignment));
  }

  data.m_gridRowCount = p_element.m_rowCount;
  data.m_gridColumnCount = p_element.m_columnCount;
  if (data.m_gridRowCount <= 0 || data.m_gridColumnCount <= 0) {
    // Defensive: an element that never had its grid filled in falls back to
    // the ragged matrix, so a snapshot is never built with an empty grid.
    data.m_gridRowCount = data.m_cells.size();
    data.m_gridColumnCount = 0;
    for (const auto &row : data.m_cells) {
      data.m_gridColumnCount = qMax(data.m_gridColumnCount, row.size());
    }
  }

  const int rows = data.m_gridRowCount;
  const int cols = data.m_gridColumnCount;
  data.m_slots.resize(rows * cols);
  for (int r = 0; r < rows; ++r) {
    for (int c = 0; c < cols; ++c) {
      auto &slot = data.m_slots[r * cols + c];
      slot.m_originRow = r;
      slot.m_originColumn = c;
    }
  }

  if (data.m_syntax == PreviewTableSyntax::Html) {
    buildHtmlGrid(p_element, data);
  }

  return PreviewBuilder::createTable(p_revision, p_startPos, p_endPos, p_source, data);
}
