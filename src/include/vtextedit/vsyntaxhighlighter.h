#ifndef VSYNTAXHIGHLIGHTER_H
#define VSYNTAXHIGHLIGHTER_H

#include <QSharedPointer>
#include <QSyntaxHighlighter>

namespace vte {
struct BlockSpellCheckData;

class VSyntaxHighlighter : public QSyntaxHighlighter {
  Q_OBJECT
public:
  explicit VSyntaxHighlighter(QTextDocument *p_doc);

  virtual ~VSyntaxHighlighter() = default;

  void setSpellCheckEnabled(bool p_enabled);

  void setAutoDetectLanguageEnabled(bool p_enabled);

  virtual bool isSyntaxFoldingEnabled() const;

  void refreshSpellCheck();

  void refreshBlockSpellCheck(const QTextBlock &p_block);

protected:
  void highlightMisspell(const QSharedPointer<BlockSpellCheckData> &p_data);

  bool m_spellCheckEnabled = false;

  bool m_autoDetectLanguageEnabled = false;
};
} // namespace vte

#endif // VSYNTAXHIGHLIGHTER_H
