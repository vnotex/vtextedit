#include "markdownfoldingprovider.h"

#include <algorithm>

#include <QTextBlock>
#include <QTextDocument>

#include <vtextedit/markdownhighlighterdata.h>
#include <vtextedit/textrange.h>

#include <textfolding.h>

namespace vte {

MarkdownFoldingProvider::MarkdownFoldingProvider(TextFolding *p_textFolding,
                                                 QTextDocument *p_document)
    : m_textFolding(p_textFolding), m_document(p_document)
{
}

void MarkdownFoldingProvider::updateFoldingRegions(const QVector<md::FoldingRegion> &p_regions)
{
  // 1. Filter out regions that span fewer than 2 blocks.
  QVector<md::FoldingRegion> valid;
  valid.reserve(p_regions.size());
  for (const auto &r : p_regions) {
    if (r.m_endBlock - r.m_startBlock >= 1) {
      valid.append(r);
    }
  }

  // 2. Sort outermost-first: ascending startBlock, then descending size.
  std::sort(valid.begin(), valid.end(), [](const md::FoldingRegion &a, const md::FoldingRegion &b) {
    if (a.m_startBlock != b.m_startBlock) {
      return a.m_startBlock < b.m_startBlock;
    }
    return (a.m_endBlock - a.m_startBlock) > (b.m_endBlock - b.m_startBlock);
  });

  // 3. Build sets of (startBlock, endBlock) pairs.
  QSet<QPair<int, int>> newPairs;
  for (const auto &r : valid) {
    newPairs.insert(qMakePair(r.m_startBlock, r.m_endBlock));
  }

  QSet<QPair<int, int>> oldPairs;
  for (const auto &r : m_previousRegions) {
    oldPairs.insert(qMakePair(r.m_startBlock, r.m_endBlock));
  }

  // 5. Remove stale ranges (in old but not in new).
  for (const auto &pair : oldPairs) {
    if (!newPairs.contains(pair)) {
      auto it = m_regionIdMap.find(pair);
      if (it != m_regionIdMap.end()) {
        m_textFolding->removeFoldingRange(it.value());
        m_regionIdMap.erase(it);
      }
    }
  }

  // 6. Add new ranges (in new but not in old), outermost-first.
  for (const auto &r : valid) {
    auto pair = qMakePair(r.m_startBlock, r.m_endBlock);
    if (!oldPairs.contains(pair)) {
      TextBlockRange range(m_document->findBlockByNumber(r.m_startBlock),
                           m_document->findBlockByNumber(r.m_endBlock));
      qint64 id = m_textFolding->newFoldingRange(range, TextFolding::Persistent);
      if (id != TextFolding::InvalidRangeId) {
        m_regionIdMap.insert(pair, id);
      }
    }
  }

  // 7. Unchanged pairs: do nothing — preserves fold state.

  // 8. Update previous regions.
  m_previousRegions = valid;
}

void MarkdownFoldingProvider::clear()
{
  for (auto it = m_regionIdMap.begin(); it != m_regionIdMap.end(); ++it) {
    m_textFolding->removeFoldingRange(it.value());
  }
  m_regionIdMap.clear();
  m_previousRegions.clear();
}

} // namespace vte
