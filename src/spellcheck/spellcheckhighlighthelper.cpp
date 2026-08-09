#include "spellcheckhighlighthelper.h"

#include <QDebug>

#include <texteditor/blockspellcheckdata.h>
#include <vtextedit/spellchecker.h>
#include <vtextedit/textblockdata.h>

#include <languagefilter_p.h>

using namespace vte;

bool SpellCheckHighlightHelper::checkBlock(const QTextBlock &p_block, const QString &p_text,
                                           bool p_autoDetectEnabled) {
  if (p_text.length() < 2) {
    return false;
  }

  // Check if cache is valid.
  auto data = TextBlockData::get(p_block);
  auto spellData = data->getBlockSpellCheckData();
  if (spellData && spellData->isValid(p_block.revision())) {
    return true;
  }

  auto &speller = SpellChecker::getInst();
  if (!speller.isValid()) {
    return false;
  }

  if (!spellData) {
    spellData.reset(new BlockSpellCheckData());
    data->setBlockSpellCheckData(spellData);
  } else {
    spellData->clear();
  }
  spellData->m_revision = p_block.revision();

  auto filter = speller.languageFilter();
  filter->setBuffer(p_text);
  while (filter->hasNext()) {
    if (!filter->isSpellcheckable()) {
      continue;
    }

    const auto sentence = filter->next();
    if (p_autoDetectEnabled) {
      const auto lang = filter->language();
      if (lang.isEmpty()) {
        continue;
      }

      if (lang != speller.currentLanguage()) {
        speller.setCurrentLanguage(lang);
      }
    }

    auto tokenizer = speller.wordTokenizer();
    tokenizer->setBuffer(sentence.toString());
    const int offset = sentence.position();
    while (tokenizer->hasNext()) {
      const auto token = tokenizer->next();

      if (!tokenizer->isSpellcheckable()) {
        continue;
      }

      auto word = token.toString();
      // Remove the ending _.
      if (word.endsWith(QLatin1Char('_'))) {
        word.chop(1);
      }

      if (speller.isMisspelled(word)) {
        // Found one.
        spellData->addMisspell(token.position() + offset, token.length());
      }
    }
  }

  return true;
}
