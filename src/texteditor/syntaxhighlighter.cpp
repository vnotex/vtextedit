#include "syntaxhighlighter.h"

#include <QDebug>
#include <QTextDocument>

#include <FoldingRegion>
#include <Format>
#include <QRegularExpression>
#include <Repository>

#include "ksyntaxhighlighterwrapper.h"
#include <vtextedit/textblockdata.h>

#include "blockspellcheckdata.h"
#include <spellcheck/spellcheckhighlighthelper.h>

#include <utils/utils.h>

using namespace vte;

using Definition = KSyntaxHighlighting::Definition;
using Definitions = QList<Definition>;

static KSyntaxHighlighting::Repository *repository() {
  return KSyntaxHighlighterWrapper::repository();
}

SyntaxHighlighter::SyntaxHighlighter(QTextDocument *p_doc, const QString &p_theme,
                                     const QString &p_syntax)
    : VSyntaxHighlighter(p_doc) {
  auto def = KSyntaxHighlighterWrapper::definitionForSyntax(p_syntax);
  if (def.isValid()) {
    qDebug() << "use definition" << def.name() << "to highlight for syntax" << p_syntax;
    setDefinition(def);
  }

  KSyntaxHighlighting::Theme th;
  if (!p_theme.isEmpty()) {
    // Check if it is a file path.
    if (Utils::isFilePath(p_theme)) {
      th = repository()->themeFromFile(p_theme);
    } else {
      th = repository()->theme(p_theme);
    }
  }
  if (!th.isValid()) {
    th = repository()->defaultTheme();
  }
  setTheme(th);
  qDebug() << "use syntax highlighter theme" << th.name() << p_theme;
}

void SyntaxHighlighter::highlightBlock(const QString &p_text) {
  if (!definition().isValid()) {
    return;
  }

  auto block = currentBlock();

  auto data = TextBlockData::get(block);
  // Clean up.
  data->clearFoldings();
  data->setMarkedAsFoldingStart(false);
  Q_ASSERT(m_pendingFoldingStart.isEmpty());

  // State of previous block.
  auto state = data->getSyntaxState();
  state = highlightLine(p_text, state);

  // Check text folding.
  if (!m_pendingFoldingStart.isEmpty()) {
    data->setMarkedAsFoldingStart(true);
    m_pendingFoldingStart.clear();
  }

  // Do spell check.
  if (!p_text.isEmpty() && m_spellCheckEnabled) {
    bool ret = SpellCheckHighlightHelper::checkBlock(block, p_text, m_autoDetectLanguageEnabled);
    if (ret) {
      // Further check and highlight.
      auto spellData = data->getBlockSpellCheckData();
      if (spellData && spellData->isValid(block.revision()) && !spellData->isEmpty()) {
        VSyntaxHighlighter::highlightMisspell(spellData);
      }
    }
  }

  // Store the state.
  const auto nextBlock = block.next();
  if (nextBlock.isValid()) {
    auto nextData = TextBlockData::get(nextBlock);
    if (nextData->getSyntaxState() != state) {
      nextData->setSyntaxState(state);
      setCurrentBlockState(currentBlockState() ^ 1);
    }
  }
}

void SyntaxHighlighter::applyFormat(int p_offset, int p_length,
                                    const KSyntaxHighlighting::Format &p_format) {
  if (p_length == 0) {
    return;
  }

  QTextCharFormat tf;

  if (m_formatCache.contains(p_format.id())) {
    tf = m_formatCache.get(p_format.id());
  } else {
    tf = KSyntaxHighlighterWrapper::toTextCharFormat(theme(), p_format);
    m_formatCache.insert(p_format.id(), tf);
  }

  QSyntaxHighlighter::setFormat(p_offset, p_length, tf);
}

void SyntaxHighlighter::applyFolding(int p_offset, int p_length,
                                     KSyntaxHighlighting::FoldingRegion p_region) {
  if (!p_region.isValid()) {
    return;
  }
  auto block = currentBlock();
  auto data = TextBlockData::get(block);
  const bool isBegin = p_region.type() == KSyntaxHighlighting::FoldingRegion::Begin;
  const int foldingValue = isBegin ? int(p_region.id()) : -int(p_region.id());
  data->addFolding(p_offset + (isBegin ? 0 : p_length), foldingValue);

  Q_ASSERT(foldingValue != 0);
  if (isBegin) {
    ++m_pendingFoldingStart[foldingValue];
  } else {
    // For end region, decrease corresponding pending folding start.
    auto it = m_pendingFoldingStart.find(-foldingValue);
    if (it != m_pendingFoldingStart.end()) {
      if (it.value() > 1) {
        --(it.value());
      } else {
        m_pendingFoldingStart.erase(it);
      }
    }
  }
}

bool SyntaxHighlighter::isValidSyntax(const QString &p_syntax) {
  auto def = KSyntaxHighlighterWrapper::definitionForSyntax(p_syntax);
  return def.isValid();
}

bool SyntaxHighlighter::isSyntaxFoldingEnabled() const { return true; }
