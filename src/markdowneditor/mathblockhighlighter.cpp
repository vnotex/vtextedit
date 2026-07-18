#include "mathblockhighlighter.h"

#include "webcodeblockhighlighter.h"

using namespace vte;

MathBlockHighlighter::MathBlockHighlighter(QObject *p_parent)
    : QObject(p_parent), m_cache(50, CacheEntry()) {}

int MathBlockHighlighter::startBlockOf(const md::MathBlock &p_block) {
  const int lineCount = p_block.m_text.count(QLatin1Char('\n')) + 1;
  return p_block.m_blockNumber - (lineCount - 1);
}

void MathBlockHighlighter::highlight(TimeStamp p_timeStamp,
                                     const QVector<md::MathBlock> &p_mathBlocks) {
  m_timeStamp = p_timeStamp;
  m_mathBlocks = p_mathBlocks;

  m_cache.setCapacityHint(m_mathBlocks.size());

  for (int idx = 0; idx < m_mathBlocks.size(); ++idx) {
    const auto &block = m_mathBlocks[idx];
    if (block.m_text.isEmpty()) {
      emit mathHighlightCompleted(HighlightResult(p_timeStamp, idx, startBlockOf(block)));
      continue;
    }

    auto &entry = m_cache.get(block.m_text);
    if (!entry.isNull()) {
      // Cache hit: reuse the previously parsed styles without a web round-trip.
      entry.m_timeStamp = m_timeStamp;
      HighlightResult result(p_timeStamp, idx, startBlockOf(block));
      result.m_highlights = entry.m_highlights;
      emit mathHighlightCompleted(result);
      continue;
    }

    emit externalMathHighlightRequested(idx, m_timeStamp, block.m_text);
  }
}

void MathBlockHighlighter::handleExternalMathHighlightData(int p_idx, TimeStamp p_timeStamp,
                                                           const QString &p_html) {
  if (m_timeStamp != p_timeStamp) {
    return;
  }

  if (p_idx < 0 || p_idx >= m_mathBlocks.size()) {
    return;
  }

  const auto &block = m_mathBlocks[p_idx];
  HighlightResult res(p_timeStamp, p_idx, startBlockOf(block));

  if (p_html.isEmpty()) {
    emit mathHighlightCompleted(res);
    return;
  }

  auto lines = block.m_text.split(QLatin1Char('\n'));
  res.m_highlights.resize(lines.size());

  // Match the Prism tokens against the actual math source lines. For top-level
  // display formulas the source lines equal the document block lines, so the
  // matched offsets are the correct in-block offsets.
  WebCodeBlockHighlighter::parseHtmlToStyles(p_html, lines, 0, res.m_highlights);

  if (m_timeStamp != p_timeStamp) {
    return;
  }

  m_cache.set(block.m_text, CacheEntry(p_timeStamp, res.m_highlights));

  emit mathHighlightCompleted(res);
}
