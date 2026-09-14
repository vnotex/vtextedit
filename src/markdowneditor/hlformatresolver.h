#ifndef HLFORMATRESOLVER_H
#define HLFORMATRESOLVER_H

#include <QTextCharFormat>
#include <QVector>

#include <vtextedit/markdownhighlighterdata.h>
#include <vtextedit/preview.h>

namespace vte {
namespace md {

// Turn ordered highlight units into effective character formats.
//
// Ordinary units produce one run each, in input order, retaining the sequential
// setFormat() merging behavior of MarkdownHighlighter::highlightBlockOne().
// Ordinary units whose styleIndex is out of range of @p_styles are skipped.
// Separate foreground-only @p_overlays are merged after ordinary styles, even
// when @p_styles is empty. Overlay pieces retain the final
// ordinary format at each range, including crossing overlaps. Input is sorted
// by start, then descending length (outer before inner); last covering overlay
// wins. Apply every returned run in order, since runs may overlap.
QVector<PreviewFormatRun> resolveFormatRuns(const QVector<HLUnit> &p_units,
                                            const QVector<QTextCharFormat> &p_styles,
                                            const QVector<HLUnitStyle> &p_overlays);

} // namespace md
} // namespace vte

#endif // HLFORMATRESOLVER_H
