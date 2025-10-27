#include "plaintexthighlighter.h"

#include "blockspellcheckdata.h"
#include <spellcheck/spellcheckhighlighthelper.h>
#include <vtextedit/textblockdata.h>

using namespace vte;

PlainTextHighlighter::PlainTextHighlighter(QTextDocument *p_doc) : VSyntaxHighlighter(p_doc) {}

void PlainTextHighlighter::highlightBlock(const QString &p_text) {
  // Do spell check.
  if (!p_text.isEmpty() && m_spellCheckEnabled) {
    auto block = currentBlock();
    auto data = TextBlockData::get(block);
    bool ret = SpellCheckHighlightHelper::checkBlock(block, p_text, m_autoDetectLanguageEnabled);
    if (ret) {
      // Further check and highlight.
      auto spellData = data->getBlockSpellCheckData();
      if (spellData && spellData->isValid(block.revision()) && !spellData->isEmpty()) {
        VSyntaxHighlighter::highlightMisspell(spellData);
      }
    }
  }
}
