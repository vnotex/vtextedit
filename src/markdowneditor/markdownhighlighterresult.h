#ifndef MARKDOWNHIGHLIGHTERRESULT_H
#define MARKDOWNHIGHLIGHTERRESULT_H

#include <QSet>
#include <QVector>

#include <vtextedit/global.h>

#include "markdownparser.h"
#include <vtextedit/markdownhighlighterdata.h>

class QTextDocument;

namespace vte {
class MarkdownHighlighter;
struct ContentsChange;

class MarkdownHighlighterFastResult {
public:
  MarkdownHighlighterFastResult() = default;

  MarkdownHighlighterFastResult(const MarkdownHighlighter *p_peg,
                           const QSharedPointer<md::MarkdownParseResult> &p_result);

  bool matched(TimeStamp p_timeStamp) const { return m_timeStamp == p_timeStamp; }

  void clear() { m_blocksHighlights.clear(); }

  TimeStamp m_timeStamp = 0;

  // Highlights of all blocks.
  QVector<QVector<md::HLUnit>> m_blocksHighlights;
};

class MarkdownHighlighterResult {
public:
  MarkdownHighlighterResult() = default;

  // TODO: handle p_result->m_offset which is 0 for now.
  MarkdownHighlighterResult(const MarkdownHighlighter *p_peg,
                       const QSharedPointer<md::MarkdownParseResult> &p_result,
                       TimeStamp p_curTimeStamp, const ContentsChange &p_lastContentsChange);

  bool matched(TimeStamp p_timeStamp) const { return m_timeStamp == p_timeStamp; }

  bool isCodeBlockHighlightEmpty() const;

  const QVector<md::HLUnitStyle> &getCodeBlockHighlight(int p_blockNumber) const;

  void setCodeBlockHighlights(int p_index, const QVector<QVector<md::HLUnitStyle>> &p_highlights);

  TimeStamp m_timeStamp = 0;

  int m_numOfBlocks = 0;

  // Highlights of all blocks.
  QVector<QVector<md::HLUnit>> m_blocksHighlights;

  // Whether the code block highlight results of this result have been received.
  bool m_codeBlockHighlightReceived = false;

  // All image link regions.
  QVector<md::ElementRegion> m_imageRegions;

  // All header regions.
  // Sorted by start position.
  QVector<md::ElementRegion> m_headerRegions;

  // All fenced code blocks.
  QVector<md::FencedCodeBlock> m_codeBlocks;

  // Time stamp for code block highlight.
  TimeStamp m_codeBlockTimeStamp = 0;

  // Indexed by block number.
  QHash<int, md::HighlightBlockState> m_codeBlocksState;

  int m_numOfCodeBlockHighlightsToRecv = 0;

  // All math blocks.
  QVector<md::MathBlock> m_mathBlocks;

  QSet<int> m_hruleBlocks;

  // All table blocks.
  // Sorted by start position ascendingly.
  QVector<md::TableBlock> m_tableBlocks;

  QVector<md::HLUnitStyle> m_dummyHighlight;

private:
  // Parse fenced code blocks from parse results.
  void parseFencedCodeBlocks(const MarkdownHighlighter *p_peg,
                             const QSharedPointer<md::MarkdownParseResult> &p_result);

  // Parse math blocks from parse results.
  void parseMathBlock(const MarkdownHighlighter *p_peg,
                      const QSharedPointer<md::MarkdownParseResult> &p_result);

  // Parse HRule blocks from parse results.
  void parseHRuleBlocks(const MarkdownHighlighter *p_peg,
                        const QSharedPointer<md::MarkdownParseResult> &p_result);

  // Parse table blocks from parse results.
  void parseTableBlocks(const QSharedPointer<md::MarkdownParseResult> &p_result);

#if 0
        void parseBlocksElementRegionOne(QHash<int, QVector<md::ElementRegion>> &p_regs,
                                         const QTextDocument *p_doc,
                                         unsigned long p_pos,
                                         unsigned long p_end);
#endif
};
} // namespace vte
#endif // MARKDOWNHIGHLIGHTERRESULT_H
