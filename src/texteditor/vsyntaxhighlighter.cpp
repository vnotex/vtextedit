#include <vtextedit/vsyntaxhighlighter.h>

#include <QTextDocument>

#include "blockspellcheckdata.h"
#include <vtextedit/textblockdata.h>

using namespace vte;

VSyntaxHighlighter::VSyntaxHighlighter(QTextDocument *p_doc) : QSyntaxHighlighter(p_doc) {}

void VSyntaxHighlighter::highlightMisspell(const QSharedPointer<BlockSpellCheckData> &p_data) {
  for (const auto &seg : p_data->m_misspellings) {
    auto format = QSyntaxHighlighter::format(seg.m_offset);
    format.setFontUnderline(true);
    format.setUnderlineStyle(QTextCharFormat::SpellCheckUnderline);
    format.setUnderlineColor(Qt::red);
    setFormat(seg.m_offset, seg.m_length, format);
  }
}

void VSyntaxHighlighter::setSpellCheckEnabled(bool p_enabled) {
  if (m_spellCheckEnabled == p_enabled) {
    return;
  }
  m_spellCheckEnabled = p_enabled;
  refreshSpellCheck();
}

void VSyntaxHighlighter::setAutoDetectLanguageEnabled(bool p_enabled) {
  if (m_autoDetectLanguageEnabled == p_enabled) {
    return;
  }
  m_autoDetectLanguageEnabled = p_enabled;
  refreshSpellCheck();
}

void VSyntaxHighlighter::refreshSpellCheck() {
  // Clear all the spell check cache data.
  auto block = document()->firstBlock();
  while (block.isValid()) {
    auto data = TextBlockData::get(block);
    auto spellData = data->getBlockSpellCheckData();
    if (spellData) {
      spellData->clear();
    }

    block = block.next();
  }

  rehighlight();
}

void VSyntaxHighlighter::refreshBlockSpellCheck(const QTextBlock &p_block) {
  auto data = TextBlockData::get(p_block);
  auto spellData = data->getBlockSpellCheckData();
  if (spellData) {
    spellData->clear();
  }

  rehighlightBlock(p_block);
}

bool VSyntaxHighlighter::isSyntaxFoldingEnabled() const { return false; }
