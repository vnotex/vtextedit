#ifndef PEGHIGHLIGHTBLOCKDATA_H
#define PEGHIGHLIGHTBLOCKDATA_H

#include <QVector>

#include <vtextedit/textblockdata.h>

#include <vtextedit/pegmarkdownhighlighterdata.h>

namespace vte {
class PegHighlightBlockData {
public:
  TimeStamp getHighlightTimeStamp() const { return m_highlightTimeStamp; }

  void setHighlightTimeStamp(TimeStamp p_ts) { m_highlightTimeStamp = p_ts; }

  const QVector<peg::HLUnit> &getHighlight() const { return m_highlight; }

  QVector<peg::HLUnit> &getHighlight() { return m_highlight; }

  void clearHighlight() {
    m_highlightTimeStamp = 0;
    m_highlight.clear();
  }

  TimeStamp getCodeBlockHighlightTimeStamp() const { return m_codeBlockHighlightTimeStamp; }

  void setCodeBlockHighlightTimeStamp(TimeStamp p_ts) { m_codeBlockHighlightTimeStamp = p_ts; }

  const QVector<peg::HLUnitStyle> &getCodeBlockHighlight() const { return m_codeBlockHighlight; }

  QVector<peg::HLUnitStyle> &getCodeBlockHighlight() { return m_codeBlockHighlight; }

  void clearCodeBlockHighlight() {
    m_codeBlockHighlightTimeStamp = 0;
    m_codeBlockHighlight.clear();
  }

  bool isBlockHighlightMatched(const QVector<peg::HLUnit> &p_highlight) const {
    if (m_highlightTimeStamp == 0 || p_highlight.size() != m_highlight.size()) {
      return false;
    }

    for (int i = 0; i < p_highlight.size(); ++i) {
      if (!(p_highlight[i] == m_highlight[i])) {
        return false;
      }
    }

    return true;
  }

  bool isCodeBlockHighlightMatched(const QVector<peg::HLUnitStyle> &p_highlight) const {
    if (p_highlight.size() != m_codeBlockHighlight.size()) {
      return false;
    }

    for (int i = 0; i < p_highlight.size(); ++i) {
      if (!(p_highlight[i] == m_codeBlockHighlight[i])) {
        return false;
      }
    }

    return true;
  }

  int getCodeBlockIndentation() const { return m_codeBlockIndentation; }

  void setCodeBlockIndentation(int p_indentation) { m_codeBlockIndentation = p_indentation; }

  bool getWrapLineEnabled() const { return m_wrapLineEnabled; }

  void setWrapLineEnabled(bool p_enabled) { m_wrapLineEnabled = p_enabled; }

  // Clear user data on parse result ready.
  void clearOnResultReady() {
    m_codeBlockIndentation = -1;
    m_wrapLineEnabled = true;
  }

  static QSharedPointer<PegHighlightBlockData> get(const QTextBlock &p_block) {
    auto blockData = TextBlockData::get(p_block);
    auto highlightData = blockData->getPegHighlightBlockData();
    if (!highlightData) {
      highlightData.reset(new PegHighlightBlockData());
      blockData->setPegHighlightBlockData(highlightData);
    }
    return highlightData;
  }

private:
  // TimeStamp of the highlight result which has been applied to this block.
  TimeStamp m_highlightTimeStamp = 0;

  // Highlight cache for this block.
  QVector<peg::HLUnit> m_highlight;

  TimeStamp m_codeBlockHighlightTimeStamp = 0;

  QVector<peg::HLUnitStyle> m_codeBlockHighlight;

  // Indentation of the this code block if this block is a fenced code block.
  int m_codeBlockIndentation = -1;

  // Whether wrap this block to the width of editor.
  bool m_wrapLineEnabled = true;
};
} // namespace vte

#endif // PEGHIGHLIGHTBLOCKDATA_H
