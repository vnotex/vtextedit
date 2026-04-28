#ifndef FOLDINGREGIONUTILS_H
#define FOLDINGREGIONUTILS_H

#include <QVector>

#include <algorithm>

#include <vtextedit/markdownhighlighterdata.h>

namespace vte {
namespace md {

// Compute heading section ranges from single-line heading regions.
// Headings are expanded into sections ending before the next same-or-higher-level heading
// (or end of document). Sections spanning less than 2 blocks are removed.
// Headings fully inside a blockquote region are also removed.
inline void computeHeadingSections(QVector<FoldingRegion> &p_regions, int p_numOfBlocks) {
  if (p_regions.isEmpty() || p_numOfBlocks <= 0) {
    return;
  }

  // Separate headings from non-headings.
  QVector<FoldingRegion> headings;
  QVector<FoldingRegion> others;
  for (const auto &r : p_regions) {
    if (r.m_type == Heading) {
      headings.append(r);
    } else {
      others.append(r);
    }
  }

  if (headings.isEmpty()) {
    return;
  }

  // Sort headings by m_startBlock ascending.
  std::sort(headings.begin(), headings.end(),
            [](const FoldingRegion &a, const FoldingRegion &b) {
              return a.m_startBlock < b.m_startBlock;
            });

  // Compute heading section ranges.
  for (int i = 0; i < headings.size(); ++i) {
    int endBlock = p_numOfBlocks - 1;
    // Find next heading with same or higher level (lower or equal m_level value).
    for (int j = i + 1; j < headings.size(); ++j) {
      if (headings[j].m_level <= headings[i].m_level) {
        endBlock = headings[j].m_startBlock - 1;
        break;
      }
    }
    headings[i].m_endBlock = endBlock;
  }

  // Collect blockquote regions for filtering.
  QVector<FoldingRegion> blockquotes;
  for (const auto &r : others) {
    if (r.m_type == Blockquote) {
      blockquotes.append(r);
    }
  }

  // Filter out headings that are too small or inside a blockquote.
  QVector<FoldingRegion> validHeadings;
  for (const auto &h : headings) {
    if (h.m_endBlock - h.m_startBlock < 1) {
      continue;
    }

    bool insideBlockquote = false;
    for (const auto &bq : blockquotes) {
      if (h.m_startBlock >= bq.m_startBlock && h.m_endBlock <= bq.m_endBlock) {
        insideBlockquote = true;
        break;
      }
    }
    if (insideBlockquote) {
      continue;
    }

    validHeadings.append(h);
  }

  // Recombine: valid heading sections + non-heading regions.
  p_regions = others;
  p_regions.append(validHeadings);

  // Sort by m_startBlock ascending.
  std::sort(p_regions.begin(), p_regions.end(),
            [](const FoldingRegion &a, const FoldingRegion &b) {
              return a.m_startBlock < b.m_startBlock;
            });
}

} // namespace md
} // namespace vte

#endif // FOLDINGREGIONUTILS_H
