#ifndef SYNTAXHIGHLIGHTER_H
#define SYNTAXHIGHLIGHTER_H

#include <vtextedit/vsyntaxhighlighter.h>

#include <AbstractHighlighter>
#include <Definition>
#include <QHash>
#include <QSharedPointer>

#include "formatcache.h"

namespace vte {
struct BlockSpellCheckData;

class SyntaxHighlighter : public VSyntaxHighlighter,
                          public KSyntaxHighlighting::AbstractHighlighter {
  Q_OBJECT
  Q_INTERFACES(KSyntaxHighlighting::AbstractHighlighter)
public:
  // @p_theme: a theme file path or a theme name.
  SyntaxHighlighter(QTextDocument *p_doc, const QString &p_theme, const QString &p_syntax);

  bool isSyntaxFoldingEnabled() const Q_DECL_OVERRIDE;

  static bool isValidSyntax(const QString &p_syntax);

protected:
  void highlightBlock(const QString &p_text) Q_DECL_OVERRIDE;

  void applyFormat(int p_offset, int p_length,
                   const KSyntaxHighlighting::Format &p_format) Q_DECL_OVERRIDE;

  void applyFolding(int p_offset, int p_length,
                    KSyntaxHighlighting::FoldingRegion p_region) Q_DECL_OVERRIDE;

private:
  // Will be set and cleared within highlightBlock().
  QHash<int, int> m_pendingFoldingStart;

  FormatCache m_formatCache;
};
} // namespace vte

#endif // SYNTAXHIGHLIGHTER_H
