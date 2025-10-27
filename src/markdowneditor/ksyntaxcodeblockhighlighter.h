#ifndef KSYNTAXCODEBLOCKHIGHLIGHTER_H
#define KSYNTAXCODEBLOCKHIGHLIGHTER_H

#include <vtextedit/codeblockhighlighter.h>

#include <QSet>

#include <texteditor/formatcache.h>

namespace KSyntaxHighlighting {
class Format;
class FoldingRegion;
} // namespace KSyntaxHighlighting

namespace vte {
class KSyntaxHighlighterWrapper;

class KSyntaxCodeBlockHighlighter : public CodeBlockHighlighter {
  Q_OBJECT
public:
  // @p_theme: a theme file path or a theme name.
  KSyntaxCodeBlockHighlighter(const QString &p_theme, QObject *p_parent);

private:
  struct HighlightInfo {
    void startNewHighlight(int p_index, int p_numOfLines) {
      m_blockIndex = p_index;
      m_lineIndex = 0;
      m_lineCount = p_numOfLines;
      m_indentation = 0;

      m_highlights.clear();
    }

    void addHighlightUnit(const peg::HLUnitStyle &p_unit) {
      if (m_highlights.isEmpty()) {
        m_highlights.resize(m_lineCount);
      }

      Q_ASSERT(m_lineIndex < m_highlights.size());
      m_highlights[m_lineIndex].push_back(p_unit);
    }

    QString toString() const {
      return QStringLiteral("codeblock %1 line %2 indentation %3")
          .arg(m_blockIndex)
          .arg(m_lineIndex)
          .arg(m_indentation);
    }

    // Index in m_codeBlocks.
    int m_blockIndex = -1;

    // Index of line within one code block.
    int m_lineIndex = 0;

    int m_lineCount = 0;

    // Index of current line.
    int m_indentation = 0;

    // Highlight results for each line within the code block.
    HighlightStyles m_highlights;
  };

  void initExtraAndExcludedLangs();

  void highlightInternal(int p_idx) Q_DECL_OVERRIDE;

  void applyFormat(int p_offset, int p_length, const KSyntaxHighlighting::Format &p_format);

  // Managed by QObject.
  KSyntaxHighlighterWrapper *m_syntaxHighlighter = nullptr;

  HighlightInfo m_currentInfo;

  FormatCache m_formatCache;

  // To minimize the gap between read mode and edit mode syntax highlighting.
  static QHash<QString, QString> s_extraLangs;

  static QSet<QString> s_excludedLangs;
};
} // namespace vte

#endif // KSYNTAXCODEBLOCKHIGHLIGHTER_H
