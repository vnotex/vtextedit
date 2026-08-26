#ifndef PREVIEWBUILDER_H
#define PREVIEWBUILDER_H

#include <vtextedit/preview.h>

class QTextDocument;

namespace vte {
// Extract [p_start, p_end) from @p_doc with paragraph separators (U+2029)
// normalized to '\n'. Returns an empty string for an unresolvable range.
QString previewSourceText(const QTextDocument *p_doc, int p_start, int p_end);

// Everything a table snapshot carries beyond the common Preview fields.
//
// A struct rather than a long parameter list because the two views of a table
// -- the historical ragged matrix and the rectangular logical grid -- must be
// filled together, by exactly one converter (createTablePreview()), so the
// parse path and the rebase path cannot diverge.
struct TableSnapshotData {
  // --- The historical matrix view; contracts unchanged. ---

  // Width declared by the header/delimiter rows.
  int m_columnCount = 0;

  // Row major, possibly RAGGED. For an HTML table this is the grid projected
  // row-major with an empty string at every covered slot.
  QVector<QVector<QString>> m_cells;

  QVector<PreviewTableAlignment> m_alignments;

  QVector<QString> m_rowPrefixes;

  QString m_delimiterPrefix;

  QVector<QVector<QVector<PreviewFormatRun>>> m_cellFormats;

  // --- The logical grid; always rectangular, always tiles exactly. ---

  PreviewTableSyntax m_syntax = PreviewTableSyntax::Markdown;

  bool m_markdownBacked = true;

  bool m_hasHeaderRow = true;

  int m_gridRowCount = 0;

  int m_gridColumnCount = 0;

  // Row major, m_gridRowCount * m_gridColumnCount entries.
  QVector<PreviewTableSlot> m_slots;

  // --- Verbatim source tags; empty for the Markdown syntax (D-g). ---

  QString m_openTag;

  // One per grid row.
  QVector<QString> m_rowTags;

  // Row major over the grid; empty at a covered slot.
  QVector<QVector<QString>> m_cellTags;
};

// Internal factory for immutable Preview snapshots.
//
// Snapshot construction is deliberately private to the library: only the
// built-in parser adapter produces them, so applications can never fabricate
// a snapshot which does not correspond to real document source.
class PreviewBuilder {
public:
  static QSharedPointer<const Preview>
  createImage(quint64 p_revision, int p_startPos, int p_endPos, const QString &p_source,
              PreviewPlacement p_placement, const QString &p_destination,
              const QString &p_alternateText, const QString &p_title);

  static QSharedPointer<const Preview> createCode(quint64 p_revision, int p_startPos, int p_endPos,
                                                  const QString &p_source,
                                                  const QString &p_language, const QString &p_code);

  static QSharedPointer<const Preview> createMath(quint64 p_revision, int p_startPos, int p_endPos,
                                                  const QString &p_source,
                                                  const QString &p_expression, bool p_displayMath);

  // @p_cellFormats is row major and parallel to @p_cells; pass an empty vector
  // when no resolved highlighting is available.
  //
  // The MARKDOWN-only convenience overload: it derives the 1x1 logical grid
  // itself, normalizing to the widest row exactly as TablePreviewDocument does.
  static QSharedPointer<const Preview>
  createTable(quint64 p_revision, int p_startPos, int p_endPos, const QString &p_source,
              int p_columnCount, const QVector<QVector<QString>> &p_cells,
              const QVector<PreviewTableAlignment> &p_alignments,
              const QVector<QString> &p_rowPrefixes, const QString &p_delimiterPrefix,
              const QVector<QVector<QVector<PreviewFormatRun>>> &p_cellFormats);

  static QSharedPointer<const Preview> createTable(quint64 p_revision, int p_startPos, int p_endPos,
                                                   const QString &p_source,
                                                   const TableSnapshotData &p_data);

private:
  PreviewBuilder() = delete;
};
} // namespace vte

#endif // PREVIEWBUILDER_H
