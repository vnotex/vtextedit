#ifndef MARKDOWNHIGHLIGHTERRESULT_H
#define MARKDOWNHIGHLIGHTERRESULT_H

#include <QHash>
#include <QSet>
#include <QVector>

#include <vtextedit/global.h>

#include "markdownparser.h"
#include <vtextedit/markdownhighlighterdata.h>
#include <vtextedit/preview.h>

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

  // Return the display math source highlight for @p_blockNumber, or an empty
  // vector if there is none.
  const QVector<md::HLUnitStyle> &getMathHighlight(int p_blockNumber) const;

  TimeStamp m_timeStamp = 0;

  int m_numOfBlocks = 0;

  // Highlights of all blocks.
  QVector<QVector<md::HLUnit>> m_blocksHighlights;

  // Whether the code block highlight results of this result have been received.
  bool m_codeBlockHighlightReceived = false;

  // All image links, with their declared `=WxH` size. Order follows the
  // walker's sorted image elements.
  QVector<md::ImageLinkInfo> m_imageLinks;

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

  // Block numbers belonging to a display math ($$...$$) source range.
  QSet<int> m_mathBlockNumbers;

  // Display math source highlight, indexed by block number.
  QHash<int, QVector<md::HLUnitStyle>> m_mathBlockHighlights;

  // Time stamp for display math source highlight.
  TimeStamp m_mathTimeStamp = 0;

  // Whether the display math highlight results of this result have been received.
  bool m_mathHighlightReceived = false;

  int m_numOfMathHighlightsToRecv = 0;

  QSet<int> m_hruleBlocks;

  // All table blocks.
  // Sorted by start position ascendingly.
  QVector<md::TableBlock> m_tableBlocks;

  // Folding regions (heading sections, code blocks, blockquotes, etc.).
  QVector<md::FoldingRegion> m_foldingRegions;

  // Typed element data of every parsed element which can be rendered as an
  // interactive preview. Kept raw (implicitly shared with the parse result)
  // so that immutable snapshots can be built on demand for exactly the
  // element types which are currently enabled and renderable.
  QVector<md::ImageElement> m_imageElements;
  QVector<md::CodeElement> m_codeElements;
  QVector<md::MathElement> m_mathElements;
  QVector<md::TableElement> m_tableElements;

  // Build immutable preview snapshots for the element types set in
  // @p_typeMask (bit i corresponds to PreviewElementType value i).
  // @p_styles maps HLUnit::styleIndex to a concrete format and is used to
  // resolve the per-cell syntax runs of table snapshots.
  QVector<QSharedPointer<const Preview>>
  buildPreviews(const QTextDocument *p_doc, int p_typeMask,
                const QVector<QTextCharFormat> &p_styles) const;

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

  // Parse folding regions from parse results and compute heading sections.
  void parseFoldingRegions(int p_numOfBlocks);

#if 0
        void parseBlocksElementRegionOne(QHash<int, QVector<md::ElementRegion>> &p_regs,
                                         const QTextDocument *p_doc,
                                         unsigned long p_pos,
                                         unsigned long p_end);
#endif
};
} // namespace vte
#endif // MARKDOWNHIGHLIGHTERRESULT_H
