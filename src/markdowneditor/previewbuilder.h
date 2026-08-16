#ifndef PREVIEWBUILDER_H
#define PREVIEWBUILDER_H

#include <vtextedit/preview.h>

class QTextDocument;

namespace vte {
// Extract [p_start, p_end) from @p_doc with paragraph separators (U+2029)
// normalized to '\n'. Returns an empty string for an unresolvable range.
QString previewSourceText(const QTextDocument *p_doc, int p_start, int p_end);

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
                                                  const QString &p_language,
                                                  const QString &p_code);

  static QSharedPointer<const Preview> createMath(quint64 p_revision, int p_startPos, int p_endPos,
                                                  const QString &p_source,
                                                  const QString &p_expression,
                                                  bool p_displayMath);

  // @p_cellFormats is row major and parallel to @p_cells; pass an empty vector
  // when no resolved highlighting is available.
  static QSharedPointer<const Preview>
  createTable(quint64 p_revision, int p_startPos, int p_endPos, const QString &p_source,
              int p_columnCount, const QVector<QVector<QString>> &p_cells,
              const QVector<PreviewTableAlignment> &p_alignments,
              const QVector<QString> &p_rowPrefixes, const QString &p_delimiterPrefix,
              const QVector<QVector<QVector<PreviewFormatRun>>> &p_cellFormats);

private:
  PreviewBuilder() = delete;
};
} // namespace vte

#endif // PREVIEWBUILDER_H
