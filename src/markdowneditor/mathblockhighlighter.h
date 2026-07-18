#ifndef MATHBLOCKHIGHLIGHTER_H
#define MATHBLOCKHIGHLIGHTER_H

#include <QObject>
#include <QVector>

#include <vtextedit/global.h>
#include <vtextedit/lrucache.h>
#include <vtextedit/markdownhighlighterdata.h>

namespace vte {
// Class to help highlighting the LaTeX source of display math ($$...$$).
//
// It mirrors WebCodeBlockHighlighter: the math source is sent to the web side
// where Prism highlights it as latex, and the returned HTML is parsed back into
// per-block highlight styles.
class MathBlockHighlighter : public QObject {
  Q_OBJECT
public:
  typedef QVector<QVector<md::HLUnitStyle>> HighlightStyles;

  struct HighlightResult {
    HighlightResult() = default;

    HighlightResult(TimeStamp p_timeStamp, int p_index, int p_startBlock)
        : m_timeStamp(p_timeStamp), m_index(p_index), m_startBlock(p_startBlock) {}

    TimeStamp m_timeStamp = 0;

    // Index in the display math blocks passed to highlight().
    int m_index = 0;

    // First block number of the display math range.
    int m_startBlock = -1;

    // Highlight styles for each line (block) within the display math range in
    // order.
    HighlightStyles m_highlights;
  };

  explicit MathBlockHighlighter(QObject *p_parent);

  // @p_mathBlocks: display math blocks only (m_previewedAsBlock == true).
  void highlight(TimeStamp p_timeStamp, const QVector<md::MathBlock> &p_mathBlocks);

  void handleExternalMathHighlightData(int p_idx, TimeStamp p_timeStamp, const QString &p_html);

signals:
  void externalMathHighlightRequested(int p_idx, TimeStamp p_timeStamp, const QString &p_text);

  void mathHighlightCompleted(const MathBlockHighlighter::HighlightResult &p_result);

private:
  struct CacheEntry {
    CacheEntry() = default;

    CacheEntry(TimeStamp p_timeStamp, const HighlightStyles &p_highlights)
        : m_timeStamp(p_timeStamp), m_highlights(p_highlights) {}

    bool isNull() const { return m_timeStamp == 0; }

    TimeStamp m_timeStamp = 0;
    HighlightStyles m_highlights;
  };

  // Compute the first block number of a display math block whose m_blockNumber
  // is the last block of the range.
  static int startBlockOf(const md::MathBlock &p_block);

  TimeStamp m_timeStamp = 0;

  QVector<md::MathBlock> m_mathBlocks;

  // Cache keyed by the exact math source text, so unchanged formulas are not
  // re-sent over the web bridge on every parse.
  LruCache<QString, CacheEntry> m_cache;
};
} // namespace vte

#endif // MATHBLOCKHIGHLIGHTER_H
