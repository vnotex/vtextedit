#ifndef PEGMARKDOWNHIGHLIGHTERDATA_H
#define PEGMARKDOWNHIGHLIGHTERDATA_H

#include <QTextCharFormat>

#include "vtextedit_export.h"
#include <vtextedit/global.h>

namespace vte {
namespace peg {
class HighlighterConfig {
public:
  // Whether enable math extension.
  bool m_mathExtEnabled = false;

  // Whether enable code block syntax highlight.
  bool m_codeBlockHighlightEnabled = true;
};

enum HighlightBlockState {
  // QTextBlock::userState() will return -1 if not defined.
  Normal = -1,

  // A fenced code block.
  CodeBlockStart,
  CodeBlock,
  CodeBlockEnd,
};

// One continuous region for a certain markdown highlight style
// within a QTextBlock.
struct HLUnit {
  bool operator==(const HLUnit &p_a) const {
    return start == p_a.start && length == p_a.length && styleIndex == p_a.styleIndex;
  }

  QString toString() const {
    return QStringLiteral("HLUnit %1 %2 %3").arg(start).arg(length).arg(styleIndex);
  }

  // Highlight offset @start and @length with style @styleIndex.
  unsigned long start = 0;
  unsigned long length = 0;
  unsigned int styleIndex = 0;
};

// One continuous region for a certain markdown highlight style
// within a QTextBlock.
struct HLUnitStyle {
  bool operator==(const HLUnitStyle &p_a) const {
    if (start != p_a.start || length != p_a.length) {
      return false;
    }

    return format == p_a.format;
  }

  // Highlight offset @start and @length.
  unsigned long start = 0;
  unsigned long length = 0;
  QTextCharFormat format;
};

struct HLUnitLess {
  bool operator()(const HLUnit &p_a, const HLUnit &p_b) const {
    if (p_a.start < p_b.start) {
      return true;
    } else if (p_a.start == p_b.start) {
      return p_a.length > p_b.length;
    } else {
      return false;
    }
  }
};

// Denote the region of a certain Markdown element.
struct ElementRegion {
  ElementRegion() = default;

  ElementRegion(int p_start, int p_end) : m_startPos(p_start), m_endPos(p_end) {}

  // Whether this region contains @p_pos.
  bool contains(int p_pos) const { return m_startPos <= p_pos && m_endPos > p_pos; }

  bool contains(const ElementRegion &p_reg) const {
    return m_startPos <= p_reg.m_startPos && m_endPos >= p_reg.m_endPos;
  }

  bool intersect(int p_start, int p_end) const {
    return !(p_end <= m_startPos || p_start >= m_endPos);
  }

  bool operator==(const ElementRegion &p_other) const {
    return (m_startPos == p_other.m_startPos && m_endPos == p_other.m_endPos);
  }

  bool operator<(const ElementRegion &p_other) const {
    if (m_startPos < p_other.m_startPos) {
      return true;
    } else if (m_startPos == p_other.m_startPos) {
      // If a < b is true, then b < a must be false.
      return m_endPos < p_other.m_endPos;
    } else {
      return false;
    }
  }

  QString toString() const { return QStringLiteral("[%1,%2)").arg(m_startPos).arg(m_endPos); }

  // The start position of the region in document.
  int m_startPos = 0;

  // The end position of the region in document.
  int m_endPos = 0;
};

// Fenced code block only.
struct VTEXTEDIT_EXPORT FencedCodeBlock {
  bool equalContent(const FencedCodeBlock &p_block) const {
    return p_block.m_lang == m_lang && p_block.m_text == m_text;
  }

  void updateNonContent(const FencedCodeBlock &p_block) {
    m_startPos = p_block.m_startPos;
    m_startBlock = p_block.m_startBlock;
    m_endBlock = p_block.m_endBlock;
  }

  QString toString() const {
    return QStringLiteral("codeblock startPos %1 [%2,%3] lang %4")
        .arg(m_startPos)
        .arg(m_startBlock)
        .arg(m_endBlock)
        .arg(m_lang);
  }

  bool contains(int p_blockNumber) const {
    return m_startBlock <= p_blockNumber && p_blockNumber <= m_endBlock;
  }

  bool hasHighlight(int p_blockNumber) const {
    if (!contains(p_blockNumber)) {
      return false;
    }

    if (m_highlights.isEmpty()) {
      return false;
    }

    Q_ASSERT(m_highlights.size() == (m_endBlock - m_startBlock + 1));
    return true;
  }

  const QVector<HLUnitStyle> &getHighlight(int p_blockNumber) const {
    Q_ASSERT(hasHighlight(p_blockNumber));
    return m_highlights[p_blockNumber - m_startBlock];
  }

  // Global position of the start.
  int m_startPos = 0;

  // Including the fences.
  int m_startBlock = 0;
  int m_endBlock = 0;

  QString m_lang;

  QString m_text;

  // Highlights for [m_startBlock, m_endBlock].
  QVector<QVector<HLUnitStyle>> m_highlights;
};

struct MathBlock {
  bool equalContent(const MathBlock &p_block) const { return m_text == p_block.m_text; }

  void updateNonContent(const MathBlock &p_block) {
    m_blockNumber = p_block.m_blockNumber;
    m_previewedAsBlock = p_block.m_previewedAsBlock;
    m_index = p_block.m_index;
    m_length = p_block.m_length;
  }

  // Block number for in-place preview.
  int m_blockNumber = -1;

  // Whether it should be previewed as block or not.
  bool m_previewedAsBlock = false;

  // Start index wihtin block with number m_blockNumber, including the start
  // mark.
  int m_index = -1;

  // Length of this mathjax in block with number m_blockNumber, including the
  // end mark.
  int m_length = -1;

  QString m_text;
};

struct VTEXTEDIT_EXPORT TableBlock {
  bool isValid() const { return m_startPos > -1 && m_endPos >= m_startPos; }

  void clear() {
    m_startPos = m_endPos = -1;
    m_borders.clear();
  }

  QString toString() const {
    return QStringLiteral("table [%1,%2) borders %3")
        .arg(m_startPos)
        .arg(m_endPos)
        .arg(m_borders.size());
  }

  int m_startPos = -1;
  int m_endPos = -1;

  // Global position of the table borders in ascending order.
  QVector<int> m_borders;
};
} // namespace peg
} // namespace vte

#endif // PEGMARKDOWNHIGHLIGHTERDATA_H
