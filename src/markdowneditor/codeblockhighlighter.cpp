#include <vtextedit/codeblockhighlighter.h>

using namespace vte;

CodeBlockHighlighter::CodeBlockHighlighter(QObject *p_parent)
    : QObject(p_parent), m_cache(50, CacheEntry()) {}

void CodeBlockHighlighter::highlight(TimeStamp p_timeStamp,
                                     const QVector<peg::FencedCodeBlock> &p_codeBlocks) {
  m_timeStamp = p_timeStamp;
  // It is OK since QVector is implicitly shared.
  m_codeBlocks = p_codeBlocks;

  m_cache.setCapacityHint(m_codeBlocks.size());
  for (int idx = 0; idx < m_codeBlocks.size(); ++idx) {
    auto &entry = m_cache.get(m_codeBlocks[idx].m_text);
    if (!entry.isNull()) {
      // Cache hits.
      entry.m_timeStamp = m_timeStamp;
      HighlightResult result(m_timeStamp, idx);
      result.m_highlights = entry.m_highlights;
      emit codeBlockHighlightCompleted(result);
    } else {
      highlightInternal(idx);
    }
  }
}

void CodeBlockHighlighter::finishHighlightOne(const HighlightResult &p_result) {
  addToCache(p_result);

  emit codeBlockHighlightCompleted(p_result);
}

void CodeBlockHighlighter::addToCache(const HighlightResult &p_result) {
  m_cache.set(m_codeBlocks[p_result.m_index].m_text,
              CacheEntry(p_result.m_timeStamp, p_result.m_highlights));
}
