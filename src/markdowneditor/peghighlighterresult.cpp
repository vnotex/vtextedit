#include "peghighlighterresult.h"

#include <QDebug>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextDocument>

#include <vtextedit/markdownutils.h>
#include <vtextedit/pegmarkdownhighlighter.h>

using namespace vte;

PegHighlighterFastResult::PegHighlighterFastResult(
    const PegMarkdownHighlighter *p_peg, const QSharedPointer<peg::PegParseResult> &p_result)
    : m_timeStamp(p_result->m_timeStamp) {
  PegHighlighterResult::parseBlocksHighlights(m_blocksHighlights, p_peg, p_result);
}

PegHighlighterResult::PegHighlighterResult(const PegMarkdownHighlighter *p_peg,
                                           const QSharedPointer<peg::PegParseResult> &p_result,
                                           TimeStamp p_curTimeStamp,
                                           const ContentsChange &p_lastContentsChange)
    : m_timeStamp(p_result->m_timeStamp), m_numOfBlocks(p_result->m_numOfBlocks) {
  // TODO: use @p_curTimeStamp and @p_lastContentsChange to fix the position of
  // results. Now we will ignore unmatched results in PegMarkdownHighlighter to
  // avoid blinking.
  Q_UNUSED(p_curTimeStamp);
  Q_UNUSED(p_lastContentsChange);

  parseBlocksHighlights(m_blocksHighlights, p_peg, p_result);

  // Implicit sharing.
  m_imageRegions = p_result->m_imageRegions;
  m_headerRegions = p_result->m_headerRegions;

  parseFencedCodeBlocks(p_peg, p_result);

  parseMathBlock(p_peg, p_result);

  // ATTENTION: if we want to handle HRule blocks specificly, uncomment this
  // line to fill the m_hruleBlocks. parseHRuleBlocks(p_peg, p_result);

  parseTableBlocks(p_result);
}

void PegHighlighterResult::parseBlocksHighlights(
    QVector<QVector<peg::HLUnit>> &p_blocksHighlights, const PegMarkdownHighlighter *p_peg,
    const QSharedPointer<peg::PegParseResult> &p_result) {
  if (p_result->isEmpty()) {
    p_blocksHighlights.clear();
    p_blocksHighlights.resize(p_result->m_numOfBlocks);
    return;
  }

  p_blocksHighlights.resize(p_result->m_numOfBlocks);

  int offset = p_result->m_offset;
  const auto doc = p_peg->document();
  auto pmhResult = p_result->m_pmhElements;
  const auto numOfStyles = peg::PegParser::getNumberOfStyles();
  for (int i = 0; i < numOfStyles; ++i) {
    pmh_element *elem_cursor = pmhResult[i];
    while (elem_cursor != NULL) {
      // elem_cursor->pos and elem_cursor->end is the start
      // and end position of the element in document.
      if (elem_cursor->end <= elem_cursor->pos) {
        elem_cursor = elem_cursor->next;
        continue;
      }

      parseBlocksHighlightOne(p_blocksHighlights, doc, offset + elem_cursor->pos,
                              offset + elem_cursor->end, i);
      elem_cursor = elem_cursor->next;
    }
  }

  // Sort p_blocksHighlights.
  for (int i = 0; i < p_blocksHighlights.size(); ++i) {
    if (p_blocksHighlights[i].size() > 1) {
      std::sort(p_blocksHighlights[i].begin(), p_blocksHighlights[i].end(), peg::HLUnitLess());
    }
  }
}

void PegHighlighterResult::parseBlocksHighlightOne(
    QVector<QVector<peg::HLUnit>> &p_blocksHighlights, const QTextDocument *p_doc,
    unsigned long p_pos, unsigned long p_end, int p_styleIndex) {
  // When the the highlight element is at the end of document, @p_end will
  // equals to the characterCount.
  unsigned int nrChar = (unsigned int)p_doc->characterCount();
  if (p_end >= nrChar && nrChar > 0) {
    p_end = nrChar - 1;
  }

  QTextBlock block = p_doc->findBlock(p_pos);
  int startBlockNum = block.blockNumber();
  int endBlockNum = p_doc->findBlock(p_end - 1).blockNumber();
  if (endBlockNum >= p_blocksHighlights.size()) {
    endBlockNum = p_blocksHighlights.size() - 1;
  }

  while (block.isValid()) {
    int blockNum = block.blockNumber();
    if (blockNum > endBlockNum) {
      break;
    }

    int blockStartPos = block.position();
    peg::HLUnit unit;
    if (blockNum == startBlockNum) {
      unit.start = p_pos - blockStartPos;
      unit.length =
          (startBlockNum == endBlockNum) ? (p_end - p_pos) : (block.length() - unit.start);
    } else if (blockNum == endBlockNum) {
      unit.start = 0;
      unit.length = p_end - blockStartPos;
    } else {
      unit.start = 0;
      unit.length = block.length();
    }

    unit.styleIndex = p_styleIndex;

    Q_ASSERT(unit.length > 0);

    if (unit.length > 0) {
      p_blocksHighlights[blockNum].append(unit);
    }

    block = block.next();
  }
}

#if 0
void PegHighlighterResult::parseBlocksElementRegionOne(QHash<int, QVector<VElementRegion>> &p_regs,
                                                       const QTextDocument *p_doc,
                                                       unsigned long p_pos,
                                                       unsigned long p_end)
{
    // When the the highlight element is at the end of document, @p_end will equals
    // to the characterCount.
    unsigned int nrChar = (unsigned int)p_doc->characterCount();
    if (p_end >= nrChar && nrChar > 0) {
        p_end = nrChar - 1;
    }

    QTextBlock block = p_doc->findBlock(p_pos);
    int startBlockNum = block.blockNumber();
    int endBlockNum = p_doc->findBlock(p_end - 1).blockNumber();
    if (endBlockNum >= p_regs.size()) {
        endBlockNum = p_regs.size() - 1;
    }

    while (block.isValid())
    {
        int blockNum = block.blockNumber();
        if (blockNum > endBlockNum) {
            break;
        }

        int blockStartPos = block.position();
        QVector<VElementRegion> &regs = p_regs[blockNum];
        int start, end;
        if (blockNum == startBlockNum) {
            start = p_pos - blockStartPos;
            end = (startBlockNum == endBlockNum) ? (p_end - blockStartPos)
                                                 : block.length();
        } else if (blockNum == endBlockNum) {
            start = 0;
            end = p_end - blockStartPos;
        } else {
            start = 0;
            end = block.length();
        }

        regs.append(VElementRegion(start, end));
    }
}
#endif

void PegHighlighterResult::parseFencedCodeBlocks(
    const PegMarkdownHighlighter *p_peg, const QSharedPointer<peg::PegParseResult> &p_result) {
  const auto &regs = p_result->m_codeBlockRegions;
  QRegularExpression codeBlockStartExp(MarkdownUtils::c_fencedCodeBlockStartRegExp);
  QRegularExpression codeBlockEndExp(MarkdownUtils::c_fencedCodeBlockEndRegExp);

  Q_ASSERT(m_codeBlocks.isEmpty());
  const auto doc = p_peg->document();
  peg::FencedCodeBlock item;
  bool inBlock = false;
  QString marker;
  for (auto it = regs.begin(); it != regs.end(); ++it) {
    QTextBlock block = doc->findBlock(it.value().m_startPos);
    int lastBlock = doc->findBlock(it.value().m_endPos - 1).blockNumber();
    if (lastBlock >= p_result->m_numOfBlocks) {
      lastBlock = p_result->m_numOfBlocks - 1;
    }

    while (block.isValid()) {
      int blockNumber = block.blockNumber();
      if (blockNumber > lastBlock) {
        break;
      }

      peg::HighlightBlockState state = peg::HighlightBlockState::Normal;
      QString text = block.text();
      if (inBlock) {
        item.m_text = item.m_text + "\n" + text;
        auto match = codeBlockEndExp.match(text);
        if (match.hasMatch() && marker == match.captured(2)) {
          // End block.
          inBlock = false;
          marker.clear();

          state = peg::HighlightBlockState::CodeBlockEnd;
          item.m_endBlock = blockNumber;
          m_codeBlocks.append(item);
        } else {
          // Within code block.
          state = peg::HighlightBlockState::CodeBlock;
        }
      } else {
        auto match = codeBlockStartExp.match(text);
        if (match.hasMatch()) {
          // Start block.
          inBlock = true;
          marker = match.captured(2);

          state = peg::HighlightBlockState::CodeBlockStart;
          item.m_startBlock = blockNumber;
          item.m_startPos = block.position();
          item.m_text = text;
          item.m_lang = match.captured(3).trimmed();
        }
      }

      if (state != peg::HighlightBlockState::Normal) {
        m_codeBlocksState.insert(blockNumber, state);
      }

      block = block.next();
    }
  }
}

void PegHighlighterResult::parseTableBlocks(const QSharedPointer<peg::PegParseResult> &p_result) {
  const auto &tableRegs = p_result->m_tableRegions;
  const auto &headerRegs = p_result->m_tableHeaderRegions;
  const auto &borderRegs = p_result->m_tableBorderRegions;

  peg::TableBlock item;
  int headerIdx = 0, borderIdx = 0;
  for (int tableIdx = 0; tableIdx < tableRegs.size(); ++tableIdx) {
    const auto &reg = tableRegs[tableIdx];
    if (headerIdx < headerRegs.size()) {
      if (reg.contains(headerRegs[headerIdx])) {
        // A new table.
        if (item.isValid()) {
          // Save previous table.
          m_tableBlocks.append(item);

          auto &table = m_tableBlocks.back();
          // Fill borders.
          for (; borderIdx < borderRegs.size(); ++borderIdx) {
            if (borderRegs[borderIdx].m_startPos >= table.m_startPos &&
                borderRegs[borderIdx].m_endPos <= table.m_endPos) {
              table.m_borders.append(borderRegs[borderIdx].m_startPos);
            } else {
              break;
            }
          }
        }

        item.clear();
        item.m_startPos = reg.m_startPos;
        item.m_endPos = reg.m_endPos;

        ++headerIdx;
        continue;
      }
    }

    // Continue previous table.
    item.m_endPos = reg.m_endPos;
  }

  if (item.isValid()) {
    // Another table.
    m_tableBlocks.append(item);

    // Fill borders.
    auto &table = m_tableBlocks.back();
    for (; borderIdx < borderRegs.size(); ++borderIdx) {
      if (borderRegs[borderIdx].m_startPos >= table.m_startPos &&
          borderRegs[borderIdx].m_endPos <= table.m_endPos) {
        table.m_borders.append(borderRegs[borderIdx].m_startPos);
      } else {
        break;
      }
    }
  }
}

static bool isDisplayFormulaRawEnd(const QString &p_text) {
  static QRegularExpression regex("\\\\end\\{[^{}\\s\\r\\n]+\\}$");
  if (p_text.indexOf(regex) > -1) {
    return true;
  }

  return false;
}

void PegHighlighterResult::parseMathBlock(const PegMarkdownHighlighter *p_peg,
                                          const QSharedPointer<peg::PegParseResult> &p_result) {
  const QTextDocument *doc = p_peg->document();

  // Inline equations.
  const auto &inlineRegs = p_result->m_inlineEquationRegions;

  for (auto it = inlineRegs.begin(); it != inlineRegs.end(); ++it) {
    const auto &r = *it;
    QTextBlock block = doc->findBlock(r.m_startPos);
    if (!block.isValid()) {
      continue;
    }

    // Inline equation MUST in one block.
    if (r.m_endPos - block.position() > block.length()) {
      continue;
    }

    peg::MathBlock item;
    item.m_blockNumber = block.blockNumber();
    item.m_previewedAsBlock = false;
    item.m_index = r.m_startPos - block.position();
    item.m_length = r.m_endPos - r.m_startPos;
    item.m_text = block.text().mid(item.m_index, item.m_length);
    m_mathBlocks.append(item);
  }

  // Display formulas.
  // One block may be split into several regions due to list indentation.
  const auto &formulaRegs = p_result->m_displayFormulaRegions;
  peg::MathBlock item;
  bool inBlock = false;
  QString marker("$$");
  QString rawMarkerStart("\\begin{");
  for (auto it = formulaRegs.begin(); it != formulaRegs.end(); ++it) {
    const auto &r = *it;
    QTextBlock block = doc->findBlock(r.m_startPos);
    int lastBlock = doc->findBlock(r.m_endPos - 1).blockNumber();
    if (lastBlock >= p_result->m_numOfBlocks) {
      lastBlock = p_result->m_numOfBlocks - 1;
    }

    while (block.isValid()) {
      int blockNum = block.blockNumber();
      if (blockNum > lastBlock) {
        break;
      }

      int pib = qMax(r.m_startPos - block.position(), 0);
      int length = qMin(r.m_endPos - block.position() - pib, block.length() - 1);
      QString text = block.text().mid(pib, length);
      if (inBlock) {
        item.m_text = item.m_text + "\n" + text;
        if (text.endsWith(marker) || (blockNum == lastBlock && isDisplayFormulaRawEnd(text))) {
          // End of block.
          inBlock = false;
          item.m_blockNumber = blockNum;
          item.m_index = pib;
          item.m_length = length;
          m_mathBlocks.append(item);
        }
      } else {
        if (!text.startsWith(marker) && !text.startsWith(rawMarkerStart)) {
          break;
        }

        if ((text.size() > 2 && text.endsWith(marker)) ||
            (blockNum == lastBlock && isDisplayFormulaRawEnd(text))) {
          // Within one block.
          item.m_blockNumber = blockNum;
          item.m_previewedAsBlock = true;
          item.m_index = pib;
          item.m_length = length;
          item.m_text = text;
          m_mathBlocks.append(item);
        } else {
          inBlock = true;
          item.m_previewedAsBlock = true;
          item.m_text = text;
        }
      }

      block = block.next();
    }
  }
}

void PegHighlighterResult::parseHRuleBlocks(const PegMarkdownHighlighter *p_peg,
                                            const QSharedPointer<peg::PegParseResult> &p_result) {
  const QTextDocument *doc = p_peg->document();
  const auto &regs = p_result->m_hruleRegions;

  for (auto it = regs.begin(); it != regs.end(); ++it) {
    QTextBlock block = doc->findBlock(it->m_startPos);
    int lastBlock = doc->findBlock(it->m_endPos - 1).blockNumber();
    if (lastBlock >= p_result->m_numOfBlocks) {
      lastBlock = p_result->m_numOfBlocks - 1;
    }

    while (block.isValid()) {
      int blockNumber = block.blockNumber();
      if (blockNumber > lastBlock) {
        break;
      }

      m_hruleBlocks.insert(blockNumber);

      block = block.next();
    }
  }
}

bool PegHighlighterResult::isCodeBlockHighlightEmpty() const {
  bool allEmpty = true;
  for (const auto &block : m_codeBlocks) {
    if (!block.m_highlights.isEmpty()) {
      allEmpty = false;
      break;
    }
  }

  return allEmpty;
}

const QVector<peg::HLUnitStyle> &
PegHighlighterResult::getCodeBlockHighlight(int p_blockNumber) const {
  // Binary search m_codeBlocks to get the highlight.
  int left = 0, right = m_codeBlocks.size() - 1;
  while (left <= right) {
    int mid = (left + right) / 2;
    if (m_codeBlocks[mid].contains(p_blockNumber)) {
      if (m_codeBlocks[mid].hasHighlight(p_blockNumber)) {
        return m_codeBlocks[mid].getHighlight(p_blockNumber);
      }
      break;
    } else if (m_codeBlocks[mid].m_endBlock < p_blockNumber) {
      left = mid + 1;
    } else {
      right = mid - 1;
    }
  }

  return m_dummyHighlight;
}

void PegHighlighterResult::setCodeBlockHighlights(
    int p_index, const QVector<QVector<peg::HLUnitStyle>> &p_highlights) {
  Q_ASSERT(p_index >= 0 && p_index < m_codeBlocks.size());
  m_codeBlocks[p_index].m_highlights = p_highlights;
}
