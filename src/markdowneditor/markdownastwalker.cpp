#include "markdownastwalker.h"
#include "cmarkadapter.h"

#include <algorithm>

#include <cmark.h>
#include <node.h>

// MarkdownSyntaxStyle ordinals (must match cmarkadapter.cpp).
enum {
  STYLE_LINK = 0,
  STYLE_AUTO_LINK_URL = 1,
  STYLE_AUTO_LINK_EMAIL = 2,
  STYLE_IMAGE = 3,
  STYLE_CODE = 4,
  STYLE_HTML = 5,
  STYLE_EMPH = 7,
  STYLE_STRONG = 8,
  STYLE_LIST_BULLET = 9,
  STYLE_LIST_ENUMERATOR = 10,
  STYLE_H1 = 12,
  STYLE_BLOCKQUOTE = 18,
  STYLE_VERBATIM = 19,
  STYLE_HTMLBLOCK = 20,
  STYLE_HRULE = 21,
  STYLE_FENCEDCODEBLOCK = 23,
  STYLE_NOTE = 24,
  STYLE_STRIKE = 25,
  STYLE_FRONTMATTER = 26,
  STYLE_DISPLAYFORMULA = 27,
  STYLE_INLINEEQUATION = 28,
  STYLE_MARK = 29,
  STYLE_TABLE = 30,
  STYLE_TABLEHEADER = 31,
};

namespace vte {
namespace md {

static int numberWidth(int p_num)
{
  if (p_num <= 0) return 1;
  int w = 0;
  int n = p_num;
  while (n > 0) {
    n /= 10;
    ++w;
  }
  return w;
}

static void addHLUnit(ASTWalkResult &p_result, const LineOffsetTable &p_offsets,
                      int p_docStart, int p_docEnd, int p_style,
                      int p_startBlock, int p_numBlocks, int p_offset)
{
  // Compute 0-indexed line indices from document-local positions.
  // We need to figure out which lines this element spans.
  // docStart and docEnd are relative to the input text (not document-absolute).

  // Find the line for docStart: scan lineStartQCharOffset.
  int startLineIdx = -1;
  int endLineIdx = -1;
  int lc = p_offsets.lineCount();

  // Binary search for startLineIdx: largest lineIdx where lineStartQCharOffset <= docStart.
  {
    int lo = 0, hi = lc - 1;
    while (lo <= hi) {
      int mid = (lo + hi) / 2;
      if (p_offsets.lineStartQCharOffset(mid) <= p_docStart) {
        startLineIdx = mid;
        lo = mid + 1;
      } else {
        hi = mid - 1;
      }
    }
  }

  // Binary search for endLineIdx: largest lineIdx where lineStartQCharOffset < docEnd.
  {
    int lo = 0, hi = lc - 1;
    while (lo <= hi) {
      int mid = (lo + hi) / 2;
      if (p_offsets.lineStartQCharOffset(mid) < p_docEnd) {
        endLineIdx = mid;
        lo = mid + 1;
      } else {
        hi = mid - 1;
      }
    }
  }

  if (startLineIdx < 0 || endLineIdx < 0) {
    return;
  }

  if (startLineIdx == endLineIdx) {
    int blockNum = p_startBlock + startLineIdx;
    if (blockNum >= 0 && blockNum < p_numBlocks) {
      int lineStartQChar = p_offsets.lineStartQCharOffset(startLineIdx);
      HLUnit unit;
      unit.start = p_docStart - lineStartQChar;
      unit.length = p_docEnd - p_docStart;
      unit.styleIndex = p_style;
      p_result.blocksHighlights[blockNum].append(unit);
    }
  } else {
    for (int lineIdx = startLineIdx; lineIdx <= endLineIdx; ++lineIdx) {
      int blockNum = p_startBlock + lineIdx;
      if (blockNum < 0 || blockNum >= p_numBlocks) {
        continue;
      }
      int lineStartQChar = p_offsets.lineStartQCharOffset(lineIdx);
      int nextLineStartQChar = (lineIdx + 1 < lc)
                                   ? p_offsets.lineStartQCharOffset(lineIdx + 1)
                                   : p_docEnd;
      HLUnit unit;
      if (lineIdx == startLineIdx) {
        unit.start = p_docStart - lineStartQChar;
        unit.length = nextLineStartQChar - p_docStart;
      } else if (lineIdx == endLineIdx) {
        unit.start = 0;
        unit.length = p_docEnd - lineStartQChar;
      } else {
        unit.start = 0;
        unit.length = nextLineStartQChar - lineStartQChar;
      }
      unit.styleIndex = p_style;
      if (unit.length > 0) {
        p_result.blocksHighlights[blockNum].append(unit);
      }
    }
  }
}

static void addRegion(ASTWalkResult &p_result, int p_style,
                      int p_absStart, int p_absEnd)
{
  ElementRegion region(p_absStart, p_absEnd);
  switch (p_style) {
  case STYLE_IMAGE:
    p_result.imageRegions.append(region);
    break;
  case STYLE_H1:
  case STYLE_H1 + 1:
  case STYLE_H1 + 2:
  case STYLE_H1 + 3:
  case STYLE_H1 + 4:
  case STYLE_H1 + 5:
    p_result.headerRegions.append(region);
    break;
  case STYLE_FENCEDCODEBLOCK:
    if (!p_result.codeBlockRegions.contains(p_absStart)) {
      p_result.codeBlockRegions.insert(p_absStart, region);
    }
    break;
  case STYLE_INLINEEQUATION:
    p_result.inlineEquationRegions.append(region);
    break;
  case STYLE_DISPLAYFORMULA:
    p_result.displayFormulaRegions.append(region);
    break;
  case STYLE_HRULE:
    p_result.hruleRegions.append(region);
    break;
  case STYLE_TABLE:
    p_result.tableRegions.append(region);
    break;
  case STYLE_TABLEHEADER:
    p_result.tableHeaderRegions.append(region);
    break;
  default:
    break;
  }
}

static void addFoldingRegion(ASTWalkResult &p_result, int p_style,
                             int p_startBlock, int p_endBlock)
{
  FoldingRegion region;
  region.m_startBlock = p_startBlock;
  region.m_endBlock = p_endBlock;
  if (p_style >= STYLE_H1 && p_style <= STYLE_H1 + 5) {
    region.m_type = FoldingRegionType::Heading;
    region.m_level = p_style - STYLE_H1 + 1;
  } else if (p_style == STYLE_FENCEDCODEBLOCK) {
    region.m_type = FoldingRegionType::FencedCode;
  } else if (p_style == STYLE_BLOCKQUOTE) {
    region.m_type = FoldingRegionType::Blockquote;
  } else if (p_style == STYLE_TABLE) {
    region.m_type = FoldingRegionType::Table;
  } else if (p_style == STYLE_DISPLAYFORMULA) {
    region.m_type = FoldingRegionType::Math;
  } else if (p_style == STYLE_FRONTMATTER) {
    region.m_type = FoldingRegionType::FrontMatter;
  } else {
    return;
  }
  p_result.foldingRegions.append(region);
}

static void handleListDirect(cmark_node *p_listNode,
                             const LineOffsetTable &p_offsets,
                             ASTWalkResult &p_result,
                             int p_startBlock, int p_numBlocks)
{
  cmark_list_type listType = cmark_node_get_list_type(p_listNode);
  int startNum = cmark_node_get_list_start(p_listNode);

  int itemIdx = 0;
  for (cmark_node *item = cmark_node_first_child(p_listNode);
       item != nullptr;
       item = cmark_node_next(item)) {
    if (cmark_node_get_type(item) != CMARK_NODE_ITEM) {
      continue;
    }

    int sl = cmark_node_get_start_line(item);
    int sc = cmark_node_get_start_column(item);
    int docPos = p_offsets.toDocPosition(sl, sc);

    int lineIdx = sl - 1;
    int blockNum = p_startBlock + lineIdx;
    if (blockNum < 0 || blockNum >= p_numBlocks) {
      ++itemIdx;
      continue;
    }

    int lineStartQChar = p_offsets.lineStartQCharOffset(lineIdx);
    int style;
    int span;
    if (listType == CMARK_BULLET_LIST) {
      style = STYLE_LIST_BULLET;
      span = 1;
    } else {
      style = STYLE_LIST_ENUMERATOR;
      int num = startNum + itemIdx;
      span = numberWidth(num) + 1;
    }

    HLUnit unit;
    unit.start = docPos - lineStartQChar;
    unit.length = span;
    unit.styleIndex = style;
    p_result.blocksHighlights[blockNum].append(unit);
    ++itemIdx;
  }
}

ASTWalkResult walkAndConvert(const QByteArray &p_utf8Text, int p_numBlocks,
                             int p_offset, int p_startBlock, bool p_fast)
{
  ASTWalkResult result;
  result.blocksHighlights.resize(p_numBlocks);

  if (p_utf8Text.isEmpty()) {
    return result;
  }

  cmark_node *doc = cmark_parse_document(p_utf8Text.constData(),
                                         p_utf8Text.size(),
                                         CMARK_OPT_DEFAULT);
  if (!doc) {
    return result;
  }

  LineOffsetTable offsets(p_utf8Text);

  cmark_iter *iter = cmark_iter_new(doc);
  cmark_event_type ev;

  while ((ev = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
    cmark_node *node = cmark_iter_get_node(iter);
    cmark_node_type type = cmark_node_get_type(node);

    if (type == CMARK_NODE_DOCUMENT) {
      continue;
    }

    if (type == CMARK_NODE_SOFTBREAK) {
      continue;
    }

    if (type == CMARK_NODE_LIST && ev == CMARK_EVENT_ENTER) {
      handleListDirect(node, offsets, result, p_startBlock, p_numBlocks);
      continue;
    }

    bool isLeaf = cmark_node_is_leaf(node);
    if (!isLeaf && ev != CMARK_EVENT_ENTER) {
      continue;
    }

    int style = mapCmarkNodeToStyle(type, node);
    if (style < 0) {
      continue;
    }

    int sl = cmark_node_get_start_line(node);
    int sc = cmark_node_get_start_column(node);
    int el = cmark_node_get_end_line(node);
    int ec = cmark_node_get_end_column(node);

    if (sl == 0 && sc == 0 && el == 0 && ec == 0) {
      continue;
    }

    int docStart = offsets.toDocPosition(sl, sc);
    int docEnd = offsets.toDocPosition(el, ec) + 1;

    if (type == CMARK_NODE_FORMULA_INLINE) {
      docStart -= 1;
      docEnd += 1;
    }

    // Add per-block HLUnits.
    addHLUnit(result, offsets, docStart, docEnd, style,
              p_startBlock, p_numBlocks, p_offset);

    // Collect regions (only when not fast-parsing).
    if (!p_fast) {
      int absStart = p_offset + docStart;
      int absEnd = p_offset + docEnd;
      addRegion(result, style, absStart, absEnd);

      int startBlock = p_startBlock + (sl - 1);
      int endBlock = p_startBlock + (el - 1);
      addFoldingRegion(result, style, startBlock, endBlock);
    }
  }

  cmark_iter_free(iter);

  // Sort each block's HLUnits.
  for (auto &blockUnits : result.blocksHighlights) {
    if (blockUnits.size() > 1) {
      std::sort(blockUnits.begin(), blockUnits.end(), HLUnitLess());
    }
  }

  // Sort region vectors that need sorting.
  if (!p_fast) {
    std::sort(result.headerRegions.begin(), result.headerRegions.end());
    std::sort(result.displayFormulaRegions.begin(), result.displayFormulaRegions.end());
    std::sort(result.tableRegions.begin(), result.tableRegions.end());
    std::sort(result.tableHeaderRegions.begin(), result.tableHeaderRegions.end());
    std::sort(result.foldingRegions.begin(), result.foldingRegions.end(),
              [](const FoldingRegion &a, const FoldingRegion &b) {
                return a.m_startBlock < b.m_startBlock;
              });
  }

  cmark_node_free(doc);
  return result;
}

} // namespace md
} // namespace vte
