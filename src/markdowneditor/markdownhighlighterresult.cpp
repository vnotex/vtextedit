#include "markdownhighlighterresult.h"

#include "foldingregionutils.h"
#include "previewbuilder.h"
#include "previewfromast.h"
#include "previewlogging.h"

#include <QDebug>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextDocument>

#include <vtextedit/markdownhighlighter.h>
#include <vtextedit/markdownutils.h>

using namespace vte;

MarkdownHighlighterFastResult::MarkdownHighlighterFastResult(
    const MarkdownHighlighter *p_peg, const QSharedPointer<md::MarkdownParseResult> &p_result)
    : m_timeStamp(p_result->m_timeStamp) {
  Q_UNUSED(p_peg);
  m_blocksHighlights = p_result->m_blocksHighlights;
}

MarkdownHighlighterResult::MarkdownHighlighterResult(
    const MarkdownHighlighter *p_peg, const QSharedPointer<md::MarkdownParseResult> &p_result,
    TimeStamp p_curTimeStamp, const ContentsChange &p_lastContentsChange)
    : m_timeStamp(p_result->m_timeStamp), m_numOfBlocks(p_result->m_numOfBlocks) {
  // TODO: use @p_curTimeStamp and @p_lastContentsChange to fix the position of
  // results. Now we will ignore unmatched results in MarkdownHighlighter to
  // avoid blinking.
  Q_UNUSED(p_curTimeStamp);
  Q_UNUSED(p_lastContentsChange);

  m_blocksHighlights = p_result->m_blocksHighlights;

  // Implicit sharing.
  m_headerRegions = p_result->m_headerRegions;
  m_foldingRegions = p_result->m_foldingRegions;

  parseFencedCodeBlocks(p_peg, p_result);

  parseMathBlock(p_peg, p_result);

  // ATTENTION: if we want to handle HRule blocks specificly, uncomment this
  // line to fill the m_hruleBlocks. parseHRuleBlocks(p_peg, p_result);

  parseTableBlocks(p_result);

  parseFoldingRegions(m_numOfBlocks);

  // Implicit sharing: keeping the raw typed data is nearly free, and lets the
  // snapshots be built later for exactly the enabled element types.
  m_imageElements = p_result->m_imageElements;
  m_codeElements = p_result->m_codeElements;
  m_mathElements = p_result->m_mathElements;
  m_tableElements = p_result->m_tableElements;
  m_headingElements = p_result->m_headingElements;

  // ATTENTION: build this from p_result, never from a member. The old
  // `m_imageRegions = p_result->m_imageRegions;` sat near the TOP of this
  // constructor, where `m_imageElements` is still empty; transcribing it there
  // against the member would have published an empty list on every parse and
  // the editor would show no image previews at all. Sourcing straight from
  // p_result makes publication independent of member assignment order.
  m_imageLinks = md::buildImageLinks(p_result->m_imageElements);
}

QVector<QSharedPointer<const Preview>>
MarkdownHighlighterResult::buildPreviews(const QTextDocument *p_doc, int p_typeMask,
                                         const QVector<QTextCharFormat> &p_styles) const {
  QVector<QSharedPointer<const Preview>> previews;
  if (!p_doc || p_typeMask == 0) {
    // The host publishes an empty mask when the feature is off or when no
    // registered factory claims any type, and then no snapshot work is done.
    qCDebug(previewSnapshotLog) << "no snapshots requested - document" << (p_doc != nullptr)
                                << "typeMask" << p_typeMask;
    return previews;
  }

  auto typeEnabled = [p_typeMask](PreviewElementType p_type) {
    return (p_typeMask & (1 << static_cast<int>(p_type))) != 0;
  };

  const quint64 revision = static_cast<quint64>(m_timeStamp);

  auto sourceOf = [p_doc](int p_start, int p_end) {
    return previewSourceText(p_doc, p_start, p_end);
  };

  if (typeEnabled(PreviewElementType::Image)) {
    for (const auto &image : m_imageElements) {
      const auto source = sourceOf(image.m_startPos, image.m_endPos);
      if (source.isEmpty()) {
        qCDebug(previewSnapshotLog) << "skipped image element with no source text at ["
                                    << image.m_startPos << "," << image.m_endPos << ")";
        continue;
      }
      previews.append(
          PreviewBuilder::createImage(revision, image.m_startPos, image.m_endPos, source,
                                      image.m_standalone ? PreviewPlacement::BlockAfterSource
                                                         : PreviewPlacement::InlineAboveLine,
                                      image.m_destination, image.m_alternateText, image.m_title));
    }
  }

  if (typeEnabled(PreviewElementType::Code)) {
    for (const auto &code : m_codeElements) {
      const auto source = sourceOf(code.m_startPos, code.m_endPos);
      if (source.isEmpty()) {
        qCDebug(previewSnapshotLog) << "skipped code element with no source text at ["
                                    << code.m_startPos << "," << code.m_endPos << ")";
        continue;
      }
      previews.append(PreviewBuilder::createCode(revision, code.m_startPos, code.m_endPos, source,
                                                 code.m_language, code.m_code));
    }
  }

  if (typeEnabled(PreviewElementType::Math)) {
    for (const auto &math : m_mathElements) {
      const auto source = sourceOf(math.m_startPos, math.m_endPos);
      if (source.isEmpty()) {
        qCDebug(previewSnapshotLog) << "skipped math element with no source text at ["
                                    << math.m_startPos << "," << math.m_endPos << ")";
        continue;
      }
      previews.append(PreviewBuilder::createMath(revision, math.m_startPos, math.m_endPos, source,
                                                 math.m_expression, math.m_display));
    }
  }

  if (typeEnabled(PreviewElementType::Table)) {
    for (const auto &table : m_tableElements) {
      const auto source = sourceOf(table.m_startPos, table.m_endPos);
      if (source.isEmpty()) {
        qCDebug(previewSnapshotLog) << "skipped table element with no source text at ["
                                    << table.m_startPos << "," << table.m_endPos << ")";
        continue;
      }

      previews.append(
          createTablePreview(revision, table.m_startPos, table.m_endPos, source, table, p_styles));
    }
  }

  std::sort(previews.begin(), previews.end(),
            [](const QSharedPointer<const Preview> &a, const QSharedPointer<const Preview> &b) {
              if (a->startPos() != b->startPos()) {
                return a->startPos() < b->startPos();
              }
              return a->endPos() < b->endPos();
            });

  if (previewSnapshotLog().isDebugEnabled()) {
    int counts[c_previewElementTypeCount] = {0};
    for (const auto &preview : previews) {
      ++counts[static_cast<int>(preview->type())];
    }

    qCDebug(previewSnapshotLog) << "built" << previews.size() << "snapshot(s) for revision"
                                << revision << "typeMask" << Qt::hex << p_typeMask << Qt::dec
                                << "- image" << counts[0] << "code" << counts[1] << "math"
                                << counts[2] << "table" << counts[3] << "(source elements: image"
                                << m_imageElements.size() << "code" << m_codeElements.size()
                                << "math" << m_mathElements.size() << "table"
                                << m_tableElements.size() << ")";

    for (const auto &preview : previews) {
      qCDebug(previewSnapshotLog) << "  " << previewTypeName(preview->type()) << "at ["
                                  << preview->startPos() << "," << preview->endPos()
                                  << ") placement" << previewPlacementName(preview->placement())
                                  << "source" << preview->sourceMarkdown().left(60);
    }
  }

  return previews;
}

#if 0
void MarkdownHighlighterResult::parseBlocksElementRegionOne(QHash<int, QVector<VElementRegion>> &p_regs,
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

void MarkdownHighlighterResult::parseFencedCodeBlocks(
    const MarkdownHighlighter *p_peg, const QSharedPointer<md::MarkdownParseResult> &p_result) {
  const auto &regs = p_result->m_codeBlockRegions;
  QRegularExpression codeBlockStartExp(MarkdownUtils::c_fencedCodeBlockStartRegExp);
  QRegularExpression codeBlockEndExp(MarkdownUtils::c_fencedCodeBlockEndRegExp);

  Q_ASSERT(m_codeBlocks.isEmpty());
  const auto doc = p_peg->document();
  md::FencedCodeBlock item;
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

      md::HighlightBlockState state = md::HighlightBlockState::Normal;
      QString text = block.text();
      if (inBlock) {
        item.m_text = item.m_text + "\n" + text;
        auto match = codeBlockEndExp.match(text);
        if (match.hasMatch() && marker == match.captured(2)) {
          // End block.
          inBlock = false;
          marker.clear();

          state = md::HighlightBlockState::CodeBlockEnd;
          item.m_endBlock = blockNumber;
          m_codeBlocks.append(item);
        } else {
          // Within code block.
          state = md::HighlightBlockState::CodeBlock;
        }
      } else {
        auto match = codeBlockStartExp.match(text);
        if (match.hasMatch()) {
          // Start block.
          inBlock = true;
          marker = match.captured(2);

          state = md::HighlightBlockState::CodeBlockStart;
          item.m_startBlock = blockNumber;
          item.m_startPos = block.position();
          item.m_text = text;
          item.m_lang = match.captured(3).trimmed();
        }
      }

      if (state != md::HighlightBlockState::Normal) {
        m_codeBlocksState.insert(blockNumber, state);
      }

      block = block.next();
    }
  }
}

void MarkdownHighlighterResult::parseTableBlocks(
    const QSharedPointer<md::MarkdownParseResult> &p_result) {
  const auto &tableRegs = p_result->m_tableRegions;
  const auto &headerRegs = p_result->m_tableHeaderRegions;
  const auto &borderRegs = p_result->m_tableBorderRegions;

  md::TableBlock item;
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

void MarkdownHighlighterResult::parseMathBlock(
    const MarkdownHighlighter *p_peg, const QSharedPointer<md::MarkdownParseResult> &p_result) {
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

    md::MathBlock item;
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
  md::MathBlock item;
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

void MarkdownHighlighterResult::parseHRuleBlocks(
    const MarkdownHighlighter *p_peg, const QSharedPointer<md::MarkdownParseResult> &p_result) {
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

bool MarkdownHighlighterResult::isCodeBlockHighlightEmpty() const {
  bool allEmpty = true;
  for (const auto &block : m_codeBlocks) {
    if (!block.m_highlights.isEmpty()) {
      allEmpty = false;
      break;
    }
  }

  return allEmpty;
}

const QVector<md::HLUnitStyle> &
MarkdownHighlighterResult::getCodeBlockHighlight(int p_blockNumber) const {
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

void MarkdownHighlighterResult::setCodeBlockHighlights(
    int p_index, const QVector<QVector<md::HLUnitStyle>> &p_highlights) {
  Q_ASSERT(p_index >= 0 && p_index < m_codeBlocks.size());
  m_codeBlocks[p_index].m_highlights = p_highlights;
}

const QVector<md::HLUnitStyle> &
MarkdownHighlighterResult::getMathHighlight(int p_blockNumber) const {
  auto it = m_mathBlockHighlights.find(p_blockNumber);
  if (it != m_mathBlockHighlights.end()) {
    return it.value();
  }
  return m_dummyHighlight;
}

void MarkdownHighlighterResult::parseFoldingRegions(int p_numOfBlocks) {
  md::computeHeadingSections(m_foldingRegions, p_numOfBlocks);
}
