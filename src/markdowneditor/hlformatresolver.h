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
// The result holds exactly one run per applicable input unit, in input order.
// Runs may overlap; applying them in order reproduces the sequential
// setFormat() behavior of MarkdownHighlighter::highlightBlockOne(). Units
// whose styleIndex is out of range of @p_styles are skipped.
QVector<PreviewFormatRun> resolveFormatRuns(const QVector<HLUnit> &p_units,
                                            const QVector<QTextCharFormat> &p_styles);

} // namespace md
} // namespace vte

#endif // HLFORMATRESOLVER_H
