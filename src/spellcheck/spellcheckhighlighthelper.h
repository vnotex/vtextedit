#ifndef SPELLCHECKHIGHLIGHTHELPER_H
#define SPELLCHECKHIGHLIGHTHELPER_H

#include <QTextBlock>

namespace vte {
class SpellCheckHighlightHelper {
public:
  SpellCheckHighlightHelper() = delete;

  // Fill the BlockSpellCheckData of @p_block.
  // Return true if spell check succeeded.
  static bool checkBlock(const QTextBlock &p_block, const QString &p_text,
                         bool p_autoDetectEnabled);
};
} // namespace vte

#endif // SPELLCHECKHIGHLIGHTHELPER_H
