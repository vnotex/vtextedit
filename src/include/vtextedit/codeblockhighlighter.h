#ifndef CODEBLOCKHIGHLIGHTER_H
#define CODEBLOCKHIGHLIGHTER_H

#include <QObject>

#include <vtextedit/global.h>
#include <vtextedit/lrucache.h>
#include <vtextedit/pegmarkdownhighlighterdata.h>

namespace vte {
// Class to help highlighting code block.
class CodeBlockHighlighter : public QObject {
  Q_OBJECT
public:
  typedef QVector<QVector<peg::HLUnitStyle>> HighlightStyles;
  struct HighlightResult {
    HighlightResult() = default;

    HighlightResult(TimeStamp p_timeStamp, int p_index)
        : m_timeStamp(p_timeStamp), m_index(p_index) {}

    bool isEmpty() const {
      for (const auto &highlight : m_highlights) {
        if (!highlight.isEmpty()) {
          return false;
        }
      }
      return true;
    }

    TimeStamp m_timeStamp = 0;

    int m_index = 0;

    // Highlight styles for each line within the code block (including the start
    // and end mark) in order.
    HighlightStyles m_highlights;
  };

  struct CacheEntry {
    CacheEntry() = default;

    CacheEntry(TimeStamp p_timeStamp, const HighlightStyles &p_highlights)
        : m_timeStamp(p_timeStamp), m_highlights(p_highlights) {}

    bool isNull() const { return m_timeStamp == 0; }

    TimeStamp m_timeStamp = 0;
    HighlightStyles m_highlights;
  };

  explicit CodeBlockHighlighter(QObject *p_parent);

  virtual ~CodeBlockHighlighter() {}

  void highlight(TimeStamp p_timeStamp, const QVector<peg::FencedCodeBlock> &p_codeBlocks);

protected:
  // @p_idx Index in m_codeBlocks.
  virtual void highlightInternal(int p_idx) = 0;

  void finishHighlightOne(const HighlightResult &p_result);

  TimeStamp m_timeStamp = 0;

  QVector<peg::FencedCodeBlock> m_codeBlocks;

signals:
  void codeBlockHighlightCompleted(const CodeBlockHighlighter::HighlightResult &p_result);

private:
  void addToCache(const HighlightResult &p_result);

  LruCache<QString, CacheEntry> m_cache;
};
} // namespace vte

#endif // CODEBLOCKHIGHLIGHTER_H
