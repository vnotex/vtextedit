#include "markdownfoldingprovider.h"

#include <algorithm>

#include <QTextBlock>
#include <QTextDocument>

#include <vtextedit/markdownhighlighterdata.h>
#include <vtextedit/textrange.h>

#include <texteditor/textfolding.h>

namespace vte {

MarkdownFoldingProvider::MarkdownFoldingProvider(TextFolding *p_textFolding,
                                                 QTextDocument *p_document)
    : m_textFolding(p_textFolding), m_document(p_document)
{
}

void MarkdownFoldingProvider::updateFoldingRegions(const QVector<md::FoldingRegion> &p_regions)
{
  // 0. Drop the ranges the document has invalidated behind TextFolding's back.
  //
  // Replacing the whole source of an element in one edit - what the preview
  // write-back path does when a table cell is edited, and what a paste or an
  // undo over the element does - destroys the blocks a range spans. The range
  // survives that edit holding stale endpoints and stops matching the block it
  // is supposed to start on, so the gutter loses its folding marker. A parse
  // result always describes a settled document, which makes this the right
  // moment to re-validate.
  m_textFolding->checkAndUpdateFoldings();

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
        qint64 id = it.value();
        m_regionIdMap.erase(it);
        m_textFolding->removeFoldingRange(id);
      }
    }
  }

  // 6. Add the ranges which TextFolding does not hold, outermost-first.
  //
  // Being absent from the old set is not the only reason for a range to be
  // missing: TextFolding drops a range without notice once the blocks it spans
  // are replaced (checkAndUpdateFoldings()), which is what an in-place source
  // rewrite of a previewed element does. Such a range comes back from the
  // parser with an unchanged (startBlock, endBlock) pair, so keying purely on
  // the diff would leave it gone for good. Creation can also simply fail, e.g.
  // when the range is not well nested with an existing one; retrying it on the
  // next parse costs nothing.
  for (const auto &r : valid) {
    auto pair = qMakePair(r.m_startBlock, r.m_endBlock);
    auto it = m_regionIdMap.find(pair);
    if (it != m_regionIdMap.end()) {
      if (m_textFolding->hasRange(it.value())) {
        // Still live - keep it, along with its fold state.
        continue;
      }

      // Stale id: the range is gone from TextFolding.
      m_regionIdMap.erase(it);
    }

    TextBlockRange range(m_document->findBlockByNumber(r.m_startBlock),
                         m_document->findBlockByNumber(r.m_endBlock));
    qint64 id = m_textFolding->newFoldingRange(range, TextFolding::Persistent);
    if (id != TextFolding::InvalidRangeId) {
      m_regionIdMap.insert(pair, id);
    }
  }

  // 7. Unchanged pairs whose range is still live are left alone - that is what
  // preserves the fold state across re-parses.

  // 8. Update previous regions.
  m_previousRegions = valid;
}

void MarkdownFoldingProvider::clear()
{
  QVector<qint64> ids;
  ids.reserve(m_regionIdMap.size());
  for (auto it = m_regionIdMap.cbegin(); it != m_regionIdMap.cend(); ++it) {
    ids.append(it.value());
  }
  m_regionIdMap.clear();
  m_previousRegions.clear();
  for (qint64 id : ids) {
    m_textFolding->removeFoldingRange(id);
  }
}

void MarkdownFoldingProvider::resetState()
{
  m_regionIdMap.clear();
  m_previousRegions.clear();
}

} // namespace vte
