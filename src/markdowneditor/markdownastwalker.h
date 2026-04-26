#ifndef MARKDOWNASTWALKER_H
#define MARKDOWNASTWALKER_H

#include <QByteArray>
#include <QMap>
#include <QVector>

#include <vtextedit/markdownhighlighterdata.h>

namespace vte {
namespace md {

struct ASTWalkResult {
  QVector<QVector<HLUnit>> blocksHighlights; // indexed by block number
  QVector<ElementRegion> imageRegions;
  QVector<ElementRegion> headerRegions;
  QMap<int, ElementRegion> codeBlockRegions;
  QVector<ElementRegion> inlineEquationRegions;
  QVector<ElementRegion> displayFormulaRegions;
  QVector<ElementRegion> hruleRegions;
  QVector<ElementRegion> tableRegions;
  QVector<ElementRegion> tableHeaderRegions;
  QVector<ElementRegion> tableBorderRegions;
};

// Single-pass AST walker. Parses markdown with cmark, walks AST once,
// produces per-block HLUnits and region vectors directly.
// p_numBlocks: total blocks in document (sizes blocksHighlights vector)
// p_offset: QChar offset of text start in document (for region positions)
// p_startBlock: first block number of the sliced text (maps local line 0 -> global block p_startBlock)
// p_fast: if true, skip region collection (only produce blocksHighlights)
ASTWalkResult walkAndConvert(const QByteArray &p_utf8Text, int p_numBlocks,
                             int p_offset = 0, int p_startBlock = 0, bool p_fast = false);

} // namespace md
} // namespace vte

#endif // MARKDOWNASTWALKER_H
