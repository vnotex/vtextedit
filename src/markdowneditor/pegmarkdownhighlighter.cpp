#include <vtextedit/pegmarkdownhighlighter.h>

#include <QDebug>
#include <QScrollBar>
#include <QTextDocument>
#include <QTimer>

#include <spellcheck/spellcheckhighlighthelper.h>
#include <texteditor/blockspellcheckdata.h>
#include <vtextedit/previewdata.h>
#include <vtextedit/textblockdata.h>
#include <vtextedit/texteditutils.h>
#include <vtextedit/textutils.h>
#include <vtextedit/theme.h>

#include "peghighlightblockdata.h"
#include "peghighlighterresult.h"
#include "pegparser.h"

#define LARGE_BLOCK_NUMBER 1000

using namespace vte;

PegMarkdownHighlighter::PegMarkdownHighlighter(
    PegMarkdownHighlighterInterface *p_interface, QTextDocument *p_doc,
    const QSharedPointer<Theme> &p_theme, CodeBlockHighlighter *p_codeBlockHighlighter,
    const QSharedPointer<peg::HighlighterConfig> &p_config)
    : VSyntaxHighlighter(p_doc), m_interface(p_interface), m_config(p_config),
      m_codeBlockHighlighter(p_codeBlockHighlighter),
      m_parserExts(pmh_EXT_NOTES | pmh_EXT_STRIKE | pmh_EXT_FRONTMATTER | pmh_EXT_MARK |
                   pmh_EXT_TABLE) {
  setTheme(p_theme);

  if (p_config->m_mathExtEnabled) {
    m_parserExts |= (pmh_EXT_MATH | pmh_EXT_MATH_RAW);
  }

  m_parser = new peg::PegParser(this);
  connect(m_parser, &peg::PegParser::parseResultReady, this,
          &PegMarkdownHighlighter::handleParseResult);

  m_result.reset(new PegHighlighterResult());
  m_fastResult.reset(new PegHighlighterFastResult());

  m_parseTimer = new QTimer(this);
  m_parseTimer->setSingleShot(true);
  m_parseTimer->setInterval(m_parseInterval);
  connect(m_parseTimer, &QTimer::timeout, this, &PegMarkdownHighlighter::startParse);

  m_fastParseTimer = new QTimer(this);
  m_fastParseTimer->setSingleShot(true);
  m_fastParseTimer->setInterval(m_fastParseInterval);
  connect(m_fastParseTimer, &QTimer::timeout, this, [this]() {
    startFastParse(m_lastContentsChange.m_position, m_lastContentsChange.m_charsRemoved,
                   m_lastContentsChange.m_charsAdded);
  });

  m_rehighlightTimer = new QTimer(this);
  m_rehighlightTimer->setSingleShot(true);
  m_rehighlightTimer->setInterval(10);
  connect(m_rehighlightTimer, &QTimer::timeout, this, &PegMarkdownHighlighter::rehighlightBlocks);

  m_scrollRehighlightTimer = new QTimer(this);
  m_scrollRehighlightTimer->setSingleShot(true);
  m_scrollRehighlightTimer->setInterval(5);
  connect(m_scrollRehighlightTimer, &QTimer::timeout, this, [this]() {
    if (m_result->m_numOfBlocks > LARGE_BLOCK_NUMBER) {
      rehighlightSensitiveBlocks();
    }
  });
  connect(m_interface->verticalScrollBar(), &QScrollBar::valueChanged, m_scrollRehighlightTimer,
          static_cast<void (QTimer::*)()>(&QTimer::start));

  m_contentChangeTime.start();
  connect(document(), &QTextDocument::contentsChange, this,
          &PegMarkdownHighlighter::handleContentsChange);

  connect(m_codeBlockHighlighter, &CodeBlockHighlighter::codeBlockHighlightCompleted, this,
          &PegMarkdownHighlighter::handleCodeBlockHighlightResult);
}

// Just use parse results to highlight block.
// Do not maintain block data and state here.
void PegMarkdownHighlighter::highlightBlock(const QString &p_text) {
  QSharedPointer<PegHighlighterResult> result(m_result);

  QTextBlock block = currentBlock();
  int blockNum = block.blockNumber();

  const auto cstate = currentBlockState();
  bool isCodeBlock = cstate == peg::HighlightBlockState::CodeBlock;
  bool isNewBlock = block.userData() == NULL;
  auto highlightData = PegHighlightBlockData::get(block);

  // Fast parse can not cross multiple empty lines in code block, which
  // cause the wrong parse results.
  if (isNewBlock) {
    int pstate = previousBlockState();
    if (pstate == peg::HighlightBlockState::CodeBlock ||
        pstate == peg::HighlightBlockState::CodeBlockStart) {
      setCurrentBlockState(peg::HighlightBlockState::CodeBlock);
      isCodeBlock = true;
    }
  }

  bool cacheValid = true;
  if (result->matched(m_timeStamp)) {
    if (preHighlightSingleFormatBlock(result->m_blocksHighlights, blockNum, p_text, isCodeBlock)) {
      cacheValid = false;
    } else if (highlightData->getHighlightTimeStamp() == m_timeStamp) {
      // Use the cache to highlight.
      highlightBlockOne(highlightData->getHighlight());
    } else {
      highlightBlockOne(result->m_blocksHighlights, blockNum, highlightData->getHighlight());
    }
  } else {
    // If fast result covers this block, we do not need to use the outdated one.
    if (isFastParseBlock(blockNum)) {
      if (!preHighlightSingleFormatBlock(m_fastResult->m_blocksHighlights, blockNum, p_text,
                                         isCodeBlock)) {
        if (m_fastResult->m_blocksHighlights.size() > blockNum) {
          highlightBlockOne(m_fastResult->m_blocksHighlights[blockNum]);
        }
      }

      cacheValid = false;
    } else {
      if (preHighlightSingleFormatBlock(result->m_blocksHighlights, blockNum, p_text,
                                        isCodeBlock)) {
        cacheValid = false;
      } else if (result->matched(highlightData->getHighlightTimeStamp())) {
        // Use the cache to highlight.
        highlightBlockOne(highlightData->getHighlight());
      } else {
        highlightBlockOne(result->m_blocksHighlights, blockNum, highlightData->getHighlight());
      }
    }
  }

  if (cacheValid) {
    highlightData->setHighlightTimeStamp(result->m_timeStamp);
  } else {
    highlightData->clearHighlight();
  }

  if (isCodeBlock) {
    formatCodeBlockLeadingSpaces(p_text);
    if (highlightData->getCodeBlockHighlightTimeStamp() == result->m_codeBlockTimeStamp ||
        !result->m_codeBlockHighlightReceived) {
      highlightCodeBlock(highlightData->getCodeBlockHighlight());
    } else {
      highlightData->clearCodeBlockHighlight();
      highlightCodeBlock(result, blockNum, highlightData->getCodeBlockHighlight());
      highlightData->setCodeBlockHighlightTimeStamp(result->m_codeBlockTimeStamp);
    }
  }

  // Do spell check.
  const bool needSpellCheck = cstate != peg::HighlightBlockState::CodeBlockStart &&
                              cstate != peg::HighlightBlockState::CodeBlock &&
                              cstate != peg::HighlightBlockState::CodeBlockEnd;
  if (needSpellCheck && !p_text.isEmpty() && m_spellCheckEnabled) {
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

static bool containSpecialChar(const QString &p_str) {
  Q_ASSERT(!p_str.isEmpty());
  QChar fi = p_str[0];
  QChar la = p_str[p_str.size() - 1];

  return fi == '#' || la == '`' || la == '$' || la == '~' || la == '*' || la == '_';
}

bool PegMarkdownHighlighter::preHighlightSingleFormatBlock(
    const QVector<QVector<peg::HLUnit>> &p_highlights, int p_blockNum, const QString &p_text,
    bool p_forced) {
  int sz = p_text.size();
  if (sz == 0) {
    return false;
  }

  if (p_highlights.size() <= p_blockNum) {
    return false;
  }

  if (!p_forced && !m_singleFormatBlocks.contains(p_blockNum)) {
    return false;
  }

  const auto &units = p_highlights[p_blockNum];
  if (units.size() == 1) {
    const auto &unit = units[0];
    if (unit.start == 0 && (int)unit.length < sz && (p_forced || containSpecialChar(p_text))) {
      setFormat(0, sz, m_styles[unit.styleIndex]);
      return true;
    }
  }

  return false;
}

void PegMarkdownHighlighter::highlightBlockOne(const QVector<QVector<peg::HLUnit>> &p_highlights,
                                               int p_blockNum, QVector<peg::HLUnit> &p_cache) {
  p_cache.clear();
  if (p_highlights.size() > p_blockNum) {
    // units are sorted by start position and length.
    const auto &units = p_highlights[p_blockNum];
    if (!units.isEmpty()) {
      p_cache.append(units);
      highlightBlockOne(units);
    }
  }
}

void PegMarkdownHighlighter::highlightBlockOne(const QVector<peg::HLUnit> &p_units) {
  for (int i = 0; i < p_units.size(); ++i) {
    const auto &unit = p_units[i];
    if (i == 0) {
      // No need to merge format.
      setFormat(unit.start, unit.length, m_styles[unit.styleIndex]);
    } else {
      QTextCharFormat newFormat = m_styles[unit.styleIndex];
      for (int j = i - 1; j >= 0; --j) {
        if (p_units[j].start + p_units[j].length <= unit.start) {
          // It won't affect current unit.
          continue;
        } else {
          // Merge the format.
          QTextCharFormat tmpFormat(newFormat);
          newFormat = m_styles[p_units[j].styleIndex];
          // tmpFormat takes precedence.
          newFormat.merge(tmpFormat);
        }
      }

      setFormat(unit.start, unit.length, newFormat);
    }
  }
}

#define KEY_PRESS_INTERVAL 50

// highlightBlock() will be called before this function.
void PegMarkdownHighlighter::handleContentsChange(int p_position, int p_charsRemoved,
                                                  int p_charsAdded) {
  Q_UNUSED(p_position);

  int interval = m_contentChangeTime.restart();

  if (p_charsRemoved == 0 && p_charsAdded == 0) {
    return;
  }

  ++m_timeStamp;

  m_parseTimer->stop();

  if (m_timeStamp > 2) {
    m_lastContentsChange.m_position = p_position;
    m_lastContentsChange.m_charsRemoved = p_charsRemoved;
    m_lastContentsChange.m_charsAdded = p_charsAdded;
    m_fastParseTimer->start(interval < KEY_PRESS_INTERVAL ? 100 : m_fastParseInterval);
  }

  // We still need a timer to start a complete parse.
  m_parseTimer->start(m_timeStamp == 2 ? 0 : m_parseInterval);
}

void PegMarkdownHighlighter::startParse() {
  QSharedPointer<peg::PegParseConfig> config(new peg::PegParseConfig());
  config->m_timeStamp = m_timeStamp;
  config->m_data = document()->toPlainText().toUtf8();
  config->m_numOfBlocks = document()->blockCount();
  config->m_extensions = m_parserExts;

  m_parser->parseAsync(config);
}

void PegMarkdownHighlighter::startFastParse(int p_position, int p_charsRemoved, int p_charsAdded) {
  // Get affected block range.
  int firstBlockNum, lastBlockNum;
  getFastParseBlockRange(p_position, p_charsRemoved, p_charsAdded, firstBlockNum, lastBlockNum);
  if (firstBlockNum == -1) {
    // We could not let m_fastResult NULL here.
    clearFastParseResult();
    m_fastParseInterval = 100;
    return;
  } else {
    m_fastParseInterval = (lastBlockNum - firstBlockNum) < 5 ? 0 : 30;
  }

  QString text;
  QTextBlock block = document()->findBlockByNumber(firstBlockNum);
  int offset = block.position();
  while (block.isValid()) {
    int blockNum = block.blockNumber();
    if (blockNum > lastBlockNum) {
      break;
    } else if (blockNum == firstBlockNum) {
      text = block.text();
    } else {
      text = text + "\n" + block.text();
    }

    block = block.next();
  }

  m_fastParseBlocks.first = firstBlockNum;
  m_fastParseBlocks.second = lastBlockNum;

  QSharedPointer<peg::PegParseConfig> config(new peg::PegParseConfig());
  config->m_timeStamp = m_timeStamp;
  config->m_data = text.toUtf8();
  config->m_numOfBlocks = document()->blockCount();
  config->m_offset = offset;
  config->m_extensions = m_parserExts;
  config->m_fast = true;

  QSharedPointer<peg::PegParseResult> parseRes = m_parser->parse(config);
  processFastParseResult(parseRes);
}

void PegMarkdownHighlighter::processFastParseResult(
    const QSharedPointer<peg::PegParseResult> &p_result) {
  m_fastResult.reset(new PegHighlighterFastResult(this, p_result));

  // Add additional single format blocks.
  appendSingleFormatBlocks(m_fastResult->m_blocksHighlights);

  if (!m_fastResult->matched(m_timeStamp) || m_result->matched(m_timeStamp)) {
    return;
  }

  auto doc = document();
  for (int i = m_fastParseBlocks.first; i <= m_fastParseBlocks.second; ++i) {
    QTextBlock block = doc->findBlockByNumber(i);
    rehighlightBlock(block);
  }
}

void PegMarkdownHighlighter::updateHighlight() {
  m_parseTimer->stop();
  if (m_result->matched(m_timeStamp)) {
    // No need to parse again. Already the latest.
    updateCodeBlocks(m_result);
    rehighlightBlocksLater();
    completeHighlight(m_result);
  } else {
    startParse();
  }
}

void PegMarkdownHighlighter::handleParseResult(
    const QSharedPointer<peg::PegParseResult> &p_result) {
  if (!m_result.isNull() && p_result->m_timeStamp != m_timeStamp) {
    // Directly skip non-matched results to avoid highlight noise.
    return;
  }

  clearFastParseResult();

  m_result.reset(new PegHighlighterResult(this, p_result, m_timeStamp, m_lastContentsChange));

  m_result->m_codeBlockTimeStamp = nextCodeBlockTimeStamp();

  m_singleFormatBlocks.clear();
  appendSingleFormatBlocks(m_result->m_blocksHighlights);

  bool matched = m_result->matched(m_timeStamp);
  if (matched) {
    clearAllBlocksUserDataAndState(m_result);

    updateAllBlocksUserDataAndState(m_result);

    updateCodeBlocks(m_result);
  }

  if (m_result->m_timeStamp == 2) {
    m_notifyHighlightComplete = true;
    rehighlightBlocks();
  } else {
    rehighlightBlocksLater();
  }

  if (matched) {
    completeHighlight(m_result);
  }
}

void PegMarkdownHighlighter::clearFastParseResult() {
  m_fastParseBlocks.first = -1;
  m_fastParseBlocks.second = -1;
  m_fastResult->clear();
}

void PegMarkdownHighlighter::appendSingleFormatBlocks(
    const QVector<QVector<peg::HLUnit>> &p_highlights) {
  auto doc = document();
  for (int i = 0; i < p_highlights.size(); ++i) {
    const auto &units = p_highlights[i];
    if (units.size() == 1) {
      const auto &unit = units[0];
      if (unit.start == 0 && unit.length > 0) {
        QTextBlock block = doc->findBlockByNumber(i);
        if (block.length() - 1 <= (int)unit.length) {
          m_singleFormatBlocks.insert(i);
        }
      }
    }
  }
}

void PegMarkdownHighlighter::clearAllBlocksUserDataAndState(
    const QSharedPointer<PegHighlighterResult> &p_result) {
  QTextBlock block = document()->firstBlock();
  while (block.isValid()) {
    clearBlockUserData(p_result, block);

    block.setUserState(peg::HighlightBlockState::Normal);

    block = block.next();
  }
}

void PegMarkdownHighlighter::clearBlockUserData(
    const QSharedPointer<PegHighlighterResult> &p_result, QTextBlock &p_block) {
  Q_UNUSED(p_result);
  const int blockNum = p_block.blockNumber();
  auto data = TextBlockData::get(p_block);
  if (!data) {
    return;
  }

  PegHighlightBlockData::get(p_block)->clearOnResultReady();

  if (BlockPreviewData::get(p_block)->getPreviewData().isEmpty()) {
    m_possiblePreviewBlocks.remove(blockNum);
  } else {
    m_possiblePreviewBlocks.insert(blockNum);
  }
}

void PegMarkdownHighlighter::updateAllBlocksUserDataAndState(
    const QSharedPointer<PegHighlighterResult> &p_result) {
  auto doc = document();

  // Code blocks.
  const QHash<int, peg::HighlightBlockState> &cbStates = p_result->m_codeBlocksState;
  for (auto it = cbStates.begin(); it != cbStates.end(); ++it) {
    QTextBlock block = doc->findBlockByNumber(it.key());
    if (!block.isValid()) {
      continue;
    }
    block.setUserState(it.value());
  }

  // Table blocks.
  for (const auto &tbb : p_result->m_tableBlocks) {
    auto block = doc->findBlock(tbb.m_startPos);
    if (!block.isValid()) {
      continue;
    }

    while (block.isValid() && block.position() < tbb.m_endPos) {
      PegHighlightBlockData::get(block)->setWrapLineEnabled(false);
      block = block.next();
    }
  }
}

void PegMarkdownHighlighter::updateCodeBlocks(
    const QSharedPointer<PegHighlighterResult> &p_result) {
  // Only need to receive code block highlights when it is empty.
  if (m_config->m_codeBlockHighlightEnabled && m_codeBlockHighlighter) {
    int cbSz = p_result->m_codeBlocks.size();
    if (cbSz > 0) {
      if (p_result->isCodeBlockHighlightEmpty()) {
        p_result->m_numOfCodeBlockHighlightsToRecv = cbSz;
      }
    } else {
      p_result->m_codeBlockHighlightReceived = true;
    }
    m_codeBlockHighlighter->highlight(p_result->m_timeStamp, p_result->m_codeBlocks);
  } else {
    p_result->m_codeBlockHighlightReceived = true;
  }

  emit codeBlocksUpdated(p_result->m_timeStamp, p_result->m_codeBlocks);
}

void PegMarkdownHighlighter::rehighlightBlocks() {
  if (m_result->m_numOfBlocks <= LARGE_BLOCK_NUMBER) {
    rehighlightBlockRange(0, m_result->m_numOfBlocks - 1);
  } else {
    rehighlightSensitiveBlocks();
  }

  if (m_notifyHighlightComplete) {
    m_notifyHighlightComplete = false;
    emit highlightCompleted();
  }
}

void PegMarkdownHighlighter::rehighlightBlocksLater() { m_rehighlightTimer->start(); }

void PegMarkdownHighlighter::highlightCodeBlock(
    const QSharedPointer<PegHighlighterResult> &p_result, int p_blockNum,
    QVector<peg::HLUnitStyle> &p_cache) {
  p_cache.clear();
  const auto &units = p_result->getCodeBlockHighlight(p_blockNum);
  if (!units.isEmpty()) {
    p_cache.append(units);
    highlightCodeBlock(units);
  }
}

void PegMarkdownHighlighter::highlightCodeBlock(const QVector<peg::HLUnitStyle> &p_units) {
  if (p_units.isEmpty()) {
    return;
  }

  for (int i = 0; i < p_units.size(); ++i) {
    const auto &unit = p_units[i];

    QTextCharFormat newFormat = codeBlockStyle();
    newFormat.merge(unit.format);
    for (int j = i - 1; j >= 0; --j) {
      if (p_units[j].start + p_units[j].length <= unit.start) {
        // It won't affect current unit.
        continue;
      } else {
        // Merge the format.
        QTextCharFormat tmpFormat(newFormat);
        newFormat = p_units[j].format;
        // tmpFormat takes precedence.
        newFormat.merge(tmpFormat);
      }
    }

    setFormat(unit.start, unit.length, newFormat);
  }
}

TimeStamp PegMarkdownHighlighter::nextCodeBlockTimeStamp() { return ++m_codeBlockTimeStamp; }

bool PegMarkdownHighlighter::isFastParseBlock(int p_blockNum) const {
  return p_blockNum >= m_fastParseBlocks.first && p_blockNum <= m_fastParseBlocks.second;
}

void PegMarkdownHighlighter::setTheme(const QSharedPointer<Theme> &p_theme) {
  if (m_theme == p_theme) {
    return;
  }

  m_theme = p_theme;
  Q_ASSERT(m_theme);

  qDebug() << "use Markdown highlighter theme" << m_theme->name();

  // Init m_styles from theme.
  m_styles.clear();
  m_styles.resize(pmh_NUM_LANG_TYPES);
  Q_ASSERT(pmh_NUM_LANG_TYPES <= Theme::MarkdownSyntaxStyle::MaxMarkdownSyntaxStyle);

  auto syntaxStyles = m_theme->markdownSyntaxStyles();
  if (!syntaxStyles) {
    qWarning() << "no Markdown syntax styles defined in theme" << m_theme->name();
  } else {
    Q_ASSERT(syntaxStyles->size() >= m_styles.size());
    for (int i = 0; i < m_styles.size(); ++i) {
      m_styles[i] = syntaxStyles->at(i).toTextCharFormat();
    }
  }
}

bool PegMarkdownHighlighter::rehighlightBlockRange(int p_first, int p_last) {
  bool highlighted = false;
  const auto &cbStates = m_result->m_codeBlocksState;
  const auto &hls = m_result->m_blocksHighlights;

  int nr = 0;
  QTextBlock block = document()->findBlockByNumber(p_first);
  while (block.isValid()) {
    int blockNum = block.blockNumber();
    if (blockNum > p_last) {
      break;
    }

    bool needHL = false;
    bool updateTS = false;
    auto highlightData = PegHighlightBlockData::get(block);
    if (highlightData->getHighlightTimeStamp() != m_result->m_timeStamp) {
      needHL = true;
      // Try to find cache.
      if (blockNum < hls.size()) {
        if (highlightData->isBlockHighlightMatched(hls[blockNum])) {
          needHL = false;
          updateTS = true;
        }
      }
    }

    if (!needHL) {
      // FIXME: what about a previous code block turn into a non-code block? For
      // now, they can be distinguished by block highlights.
      auto it = cbStates.find(blockNum);
      if (it != cbStates.end() && it.value() == peg::HighlightBlockState::CodeBlock) {
        if (highlightData->getCodeBlockHighlightTimeStamp() != m_result->m_codeBlockTimeStamp &&
            m_result->m_codeBlockHighlightReceived) {
          needHL = true;
          // Try to find cache.
          const auto &codeBlockHighlights = m_result->getCodeBlockHighlight(blockNum);
          if (highlightData->isCodeBlockHighlightMatched(codeBlockHighlights)) {
            needHL = false;
            updateTS = true;
          }
        }
      }
    }

    if (needHL) {
      highlighted = true;
      rehighlightBlock(block);
      ++nr;
    } else if (updateTS) {
      highlightData->setHighlightTimeStamp(m_result->m_timeStamp);
      highlightData->setCodeBlockHighlightTimeStamp(m_result->m_codeBlockTimeStamp);
    }

    block = block.next();
  }

  return highlighted;
}

bool PegMarkdownHighlighter::isEmptyCodeBlockHighlights(
    const QVector<QVector<peg::HLUnitStyle>> &p_highlights) {
  if (p_highlights.isEmpty()) {
    return true;
  }

  bool empty = true;
  for (int i = 0; i < p_highlights.size(); ++i) {
    if (!p_highlights[i].isEmpty()) {
      empty = false;
      break;
    }
  }

  return empty;
}

void PegMarkdownHighlighter::completeHighlight(QSharedPointer<PegHighlighterResult> p_result) {
  m_notifyHighlightComplete = true;

  if (isMathEnabled()) {
    emit mathBlocksUpdated(p_result->m_mathBlocks);
  }

  emit tableBlocksUpdated(p_result->m_tableBlocks);

  emit imageLinksUpdated(p_result->m_imageRegions);
  emit headersUpdated(p_result->m_headerRegions);
}

bool PegMarkdownHighlighter::isMathEnabled() const { return m_parserExts & pmh_EXT_MATH; }

void PegMarkdownHighlighter::rehighlightSensitiveBlocks() {
  QTextBlock cb = m_interface->textCursor().block();

  auto range = m_interface->visibleBlockRange();

  bool cursorVisible = cb.blockNumber() >= range.first && cb.blockNumber() <= range.second;

  // Include extra blocks.
  const int nrUpExtra = 5;
  const int nrDownExtra = 20;
  int first = qMax(0, range.first - nrUpExtra);
  int last = qMin(document()->blockCount() - 1, range.second + nrDownExtra);

  if (rehighlightBlockRange(first, last)) {
    if (cursorVisible) {
      m_interface->ensureCursorVisible();
    }
  }
}

const QSet<int> &PegMarkdownHighlighter::getPossiblePreviewBlocks() const {
  return m_possiblePreviewBlocks;
}

void PegMarkdownHighlighter::clearPossiblePreviewBlocks(const QVector<int> &p_blocksToClear) {
  for (auto i : p_blocksToClear) {
    m_possiblePreviewBlocks.remove(i);
  }
}

void PegMarkdownHighlighter::addPossiblePreviewBlock(int p_blockNumber) {
  m_possiblePreviewBlocks.insert(p_blockNumber);
}

void PegMarkdownHighlighter::getFastParseBlockRange(int p_position, int p_charsRemoved,
                                                    int p_charsAdded, int &p_firstBlock,
                                                    int &p_lastBlock) const {
  const int maxNumOfBlocks = 15;

  int charsChanged = p_charsRemoved + p_charsAdded;
  auto doc = document();
  QTextBlock firstBlock = doc->findBlock(p_position);

  // May be an invalid block.
  QTextBlock lastBlock = doc->findBlock(qMax(0, p_position + charsChanged));
  if (!lastBlock.isValid()) {
    lastBlock = doc->lastBlock();
  }

  int num = lastBlock.blockNumber() - firstBlock.blockNumber() + 1;
  if (num > maxNumOfBlocks) {
    p_firstBlock = p_lastBlock = -1;
    return;
  }

  // Look up.
  while (firstBlock.isValid() && num <= maxNumOfBlocks) {
    QTextBlock preBlock = firstBlock.previous();
    if (!preBlock.isValid()) {
      break;
    }

    // Check code block.
    int state = firstBlock.userState();
    if (state == peg::HighlightBlockState::CodeBlock ||
        state == peg::HighlightBlockState::CodeBlockEnd) {
      goto goup;
    }

    // Empty block.
    if (TextEditUtils::isEmptyBlock(firstBlock)) {
      goto goup;
    }

    if (TextEditUtils::fetchIndentation(firstBlock) < 4) {
      // If previous block is empty, then we could stop now.
      if (TextEditUtils::isEmptyBlock(preBlock)) {
        int preState = preBlock.userState();
        if (preState != peg::HighlightBlockState::CodeBlockStart &&
            preState != peg::HighlightBlockState::CodeBlock) {
          break;
        }
      }
    }

  goup:
    firstBlock = preBlock;
    ++num;
  }

  // Look down.
  bool inCodeBlock = false;
  while (lastBlock.isValid() && num <= maxNumOfBlocks) {
    QTextBlock nextBlock = lastBlock.next();
    if (!nextBlock.isValid()) {
      break;
    }

    // Check code block.
    switch (lastBlock.userState()) {
    case peg::HighlightBlockState::CodeBlockStart:
      Q_FALLTHROUGH();
    case peg::HighlightBlockState::CodeBlock:
      inCodeBlock = true;
      goto godown;

    case peg::HighlightBlockState::CodeBlockEnd:
      inCodeBlock = false;
      break;

    default:
      break;
    }

    // Empty block.
    if (TextEditUtils::isEmptyBlock(nextBlock) && !inCodeBlock) {
      int nstate = nextBlock.userState();
      if (nstate != peg::HighlightBlockState::CodeBlockStart &&
          nstate != peg::HighlightBlockState::CodeBlock &&
          nstate != peg::HighlightBlockState::CodeBlockEnd) {
        break;
      }
    }

  godown:
    lastBlock = nextBlock;
    ++num;
  }

  p_firstBlock = firstBlock.blockNumber();
  p_lastBlock = lastBlock.blockNumber();
  if (p_lastBlock < p_firstBlock) {
    p_lastBlock = p_firstBlock;
  } else if (p_lastBlock - p_firstBlock + 1 > maxNumOfBlocks) {
    p_firstBlock = p_lastBlock = -1;
  }
}

const QVector<peg::ElementRegion> &PegMarkdownHighlighter::getHeaderRegions() const {
  return m_result->m_headerRegions;
}

const QVector<peg::ElementRegion> &PegMarkdownHighlighter::getImageRegions() const {
  return m_result->m_imageRegions;
}

const QVector<peg::FencedCodeBlock> &PegMarkdownHighlighter::getCodeBlocks() const {
  return m_result->m_codeBlocks;
}

void PegMarkdownHighlighter::handleCodeBlockHighlightResult(
    const CodeBlockHighlighter::HighlightResult &p_result) {
  QSharedPointer<PegHighlighterResult> result(m_result);
  if (!result->matched(p_result.m_timeStamp) || result->m_numOfCodeBlockHighlightsToRecv <= 0) {
    return;
  }

  if (!p_result.isEmpty()) {
    // Set it to PegHighlighterResult.
    result->setCodeBlockHighlights(p_result.m_index, p_result.m_highlights);
  }

  if (--result->m_numOfCodeBlockHighlightsToRecv <= 0) {
    result->m_codeBlockTimeStamp = nextCodeBlockTimeStamp();
    result->m_codeBlockHighlightReceived = true;
    rehighlightBlocksLater();
  }
}

const QTextCharFormat &PegMarkdownHighlighter::codeBlockStyle() const {
  return m_styles[Theme::MarkdownSyntaxStyle::FENCEDCODEBLOCK];
}

void PegMarkdownHighlighter::formatCodeBlockLeadingSpaces(const QString &p_text) {
  // Brush the indentation spaces.
  if (currentBlockState() == peg::HighlightBlockState::CodeBlock) {
    int spaces = TextUtils::fetchIndentation(p_text);
    if (spaces > 0) {
      setFormat(0, spaces, codeBlockStyle());
    }
  }
}

void PegMarkdownHighlighter::updateStylesFontSize(int p_delta) {
  if (p_delta == 0) {
    return;
  }

  const int minSize = 2;

  for (auto &style : m_styles) {
    if (style.fontPointSize() == 0) {
      // It contains no font size format.
      continue;
    }

    int ptSize = qMax(minSize, static_cast<int>(style.fontPointSize() + p_delta));
    style.setFontPointSize(ptSize);
  }

  rehighlight();
}
