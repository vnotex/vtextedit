#ifndef PLAINTEXTHIGHLIGHTER_H
#define PLAINTEXTHIGHLIGHTER_H

#include <vtextedit/vsyntaxhighlighter.h>

namespace vte {
// Highlighter for plain text.
// Only for spell check highlighting for now.
class PlainTextHighlighter : public VSyntaxHighlighter {
  Q_OBJECT
public:
  explicit PlainTextHighlighter(QTextDocument *p_doc);

protected:
  void highlightBlock(const QString &p_text) Q_DECL_OVERRIDE;
};
} // namespace vte

#endif // PLAINTEXTHIGHLIGHTER_H
