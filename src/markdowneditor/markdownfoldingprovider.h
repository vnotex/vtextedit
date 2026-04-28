#ifndef MARKDOWNFOLDINGPROVIDER_H
#define MARKDOWNFOLDINGPROVIDER_H

#include <QHash>
#include <QPair>
#include <QVector>

namespace vte {
namespace md {
struct FoldingRegion;
}

class TextFolding;
} // namespace vte

class QTextDocument;

namespace vte {

class MarkdownFoldingProvider {
public:
  MarkdownFoldingProvider(TextFolding *p_textFolding, QTextDocument *p_document);

  void updateFoldingRegions(const QVector<md::FoldingRegion> &p_regions);

  void clear();

private:
  TextFolding *m_textFolding = nullptr;

  QTextDocument *m_document = nullptr;

  QVector<md::FoldingRegion> m_previousRegions;

  // Maps (startBlock, endBlock) to the TextFolding range ID.
  QHash<QPair<int, int>, qint64> m_regionIdMap;
};

} // namespace vte

#endif // MARKDOWNFOLDINGPROVIDER_H
