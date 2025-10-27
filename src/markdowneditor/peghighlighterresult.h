#ifndef PEGHIGHLIGHTERRESULT_H
#define PEGHIGHLIGHTERRESULT_H

#include <QSet>
#include <QVector>

#include <vtextedit/global.h>

#include "pegparser.h"
#include <vtextedit/pegmarkdownhighlighterdata.h>

class QTextDocument;

namespace vte {
class PegMarkdownHighlighter;
struct ContentsChange;

class PegHighlighterFastResult {
public:
  PegHighlighterFastResult() = default;

  PegHighlighterFastResult(const PegMarkdownHighlighter *p_peg,
                           const QSharedPointer<peg::PegParseResult> &p_result);

  bool matched(TimeStamp p_timeStamp) const { return m_timeStamp == p_timeStamp; }

  void clear() { m_blocksHighlights.clear(); }

  TimeStamp m_timeStamp = 0;

  // Highlights of all blocks.
  QVector<QVector<peg::HLUnit>> m_blocksHighlights;
};

class PegHighlighterResult {
public:
  PegHighlighterResult() = default;

  // TODO: handle p_result->m_offset which is 0 for now.
  PegHighlighterResult(const PegMarkdownHighlighter *p_peg,
                       const QSharedPointer<peg::PegParseResult> &p_result,
                       TimeStamp p_curTimeStamp, const ContentsChange &p_lastContentsChange);

  bool matched(TimeStamp p_timeStamp) const { return m_timeStamp == p_timeStamp; }

  bool isCodeBlockHighlightEmpty() const;

  const QVector<peg::HLUnitStyle> &getCodeBlockHighlight(int p_blockNumber) const;

  void setCodeBlockHighlights(int p_index, const QVector<QVector<peg::HLUnitStyle>> &p_highlights);

  // Parse highlight elements for all the blocks from parse results.
  static void parseBlocksHighlights(QVector<QVector<peg::HLUnit>> &p_blocksHighlights,
                                    const PegMarkdownHighlighter *p_peg,
                                    const QSharedPointer<peg::PegParseResult> &p_result);

  TimeStamp m_timeStamp = 0;

  int m_numOfBlocks = 0;

  // Highlights of all blocks.
  QVector<QVector<peg::HLUnit>> m_blocksHighlights;

  // Whether the code block highlight results of this result have been received.
  bool m_codeBlockHighlightReceived = false;

  // All image link regions.
  QVector<peg::ElementRegion> m_imageRegions;

  // All header regions.
  // Sorted by start position.
  QVector<peg::ElementRegion> m_headerRegions;

  // All fenced code blocks.
  QVector<peg::FencedCodeBlock> m_codeBlocks;

  // Time stamp for code block highlight.
  TimeStamp m_codeBlockTimeStamp = 0;

  // Indexed by block number.
  QHash<int, peg::HighlightBlockState> m_codeBlocksState;

  int m_numOfCodeBlockHighlightsToRecv = 0;

  // All math blocks.
  QVector<peg::MathBlock> m_mathBlocks;

  QSet<int> m_hruleBlocks;

  // All table blocks.
  // Sorted by start position ascendingly.
  QVector<peg::TableBlock> m_tableBlocks;

  QVector<peg::HLUnitStyle> m_dummyHighlight;

private:
  // Parse highlight elements for blocks from one parse result.
  static void parseBlocksHighlightOne(QVector<QVector<peg::HLUnit>> &p_blocksHighlights,
                                      const QTextDocument *p_doc, unsigned long p_pos,
                                      unsigned long p_end, int p_styleIndex);

  // Parse fenced code blocks from parse results.
  void parseFencedCodeBlocks(const PegMarkdownHighlighter *p_peg,
                             const QSharedPointer<peg::PegParseResult> &p_result);

  // Parse math blocks from parse results.
  void parseMathBlock(const PegMarkdownHighlighter *p_peg,
                      const QSharedPointer<peg::PegParseResult> &p_result);

  // Parse HRule blocks from parse results.
  void parseHRuleBlocks(const PegMarkdownHighlighter *p_peg,
                        const QSharedPointer<peg::PegParseResult> &p_result);

  // Parse table blocks from parse results.
  void parseTableBlocks(const QSharedPointer<peg::PegParseResult> &p_result);

#if 0
        void parseBlocksElementRegionOne(QHash<int, QVector<peg::ElementRegion>> &p_regs,
                                         const QTextDocument *p_doc,
                                         unsigned long p_pos,
                                         unsigned long p_end);
#endif
};
} // namespace vte
#endif // PEGHIGHLIGHTERRESULT_H
