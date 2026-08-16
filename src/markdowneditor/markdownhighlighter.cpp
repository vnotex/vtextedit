#include <vtextedit/markdownhighlighter.h>

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

#include "markdownastwalker.h"
#include "markdownhighlightblockdata.h"
#include "markdownhighlighterresult.h"
#include "markdownparser.h"
#include "markdownsyntaxstyles.h"
#include "mathblockhighlighter.h"
#include "interactivepreviewhost.h"

// Extension flags (replacing pmh_EXT_* constants).
// These are kept for config compatibility but cmark enables all extensions by default.
enum {
  EXT_NONE = 0,
  EXT_NOTES = 0x01,
  EXT_STRIKE = 0x02,
  EXT_FRONTMATTER = 0x04,
  EXT_MARK = 0x08,
  EXT_TABLE = 0x10,
  EXT_MATH = 0x20,
  EXT_MATH_RAW = 0x40,
};

#define LARGE_BLOCK_NUMBER 1000

using namespace vte;

MarkdownHighlighter::MarkdownHighlighter(
    MarkdownHighlighterInterface *p_interface, QTextDocument *p_doc,
    const QSharedPointer<Theme> &p_theme, CodeBlockHighlighter *p_codeBlockHighlighter,
    const QSharedPointer<md::HighlighterConfig> &p_config,
    MathBlockHighlighter *p_mathBlockHighlighter)
    : VSyntaxHighlighter(p_doc), m_interface(p_interface), m_config(p_config),
      m_codeBlockHighlighter(p_codeBlockHighlighter),
      m_mathBlockHighlighter(p_mathBlockHighlighter),
      m_parserExts(EXT_NOTES | EXT_STRIKE | EXT_FRONTMATTER | EXT_MARK |
                   EXT_TABLE) {
  setTheme(p_theme);

  if (p_config->m_mathExtEnabled) {
    m_parserExts |= (EXT_MATH | EXT_MATH_RAW);
  }

  m_parser = new md::MarkdownParser(this);
  connect(m_parser, &md::MarkdownParser::parseResultReady, this,
          &MarkdownHighlighter::handleParseResult);

  m_result.reset(new MarkdownHighlighterResult());
  m_fastResult.reset(new MarkdownHighlighterFastResult());

  m_parseTimer = new QTimer(this);
  m_parseTimer->setSingleShot(true);
  m_parseTimer->setInterval(m_parseInterval);
  connect(m_parseTimer, &QTimer::timeout, this, &MarkdownHighlighter::startParse);

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
  connect(m_rehighlightTimer, &QTimer::timeout, this, &MarkdownHighlighter::rehighlightBlocks);

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
          &MarkdownHighlighter::handleContentsChange);

  connect(m_codeBlockHighlighter, &CodeBlockHighlighter::codeBlockHighlightCompleted, this,
          &MarkdownHighlighter::handleCodeBlockHighlightResult);

  if (m_mathBlockHighlighter) {
    connect(m_mathBlockHighlighter, &MathBlockHighlighter::mathHighlightCompleted, this,
            [this](const MathBlockHighlighter::HighlightResult &p_result) {
              applyMathHighlightResult(p_result.m_timeStamp, p_result.m_startBlock,
                                       p_result.m_highlights);
            });
  }
}

// Just use parse results to highlight block.
// Do not maintain block data and state here.
void MarkdownHighlighter::highlightBlock(const QString &p_text) {
  QSharedPointer<MarkdownHighlighterResult> result(m_result);

  QTextBlock block = currentBlock();
  int blockNum = block.blockNumber();

  const auto cstate = currentBlockState();
  bool isCodeBlock = cstate == md::HighlightBlockState::CodeBlock;
  bool isNewBlock = block.userData() == NULL;
  auto highlightData = MarkdownHighlightBlockData::get(block);

  // Fast parse can not cross multiple empty lines in code block, which
  // cause the wrong parse results.
  if (isNewBlock) {
    int pstate = previousBlockState();
    if (pstate == md::HighlightBlockState::CodeBlock ||
        pstate == md::HighlightBlockState::CodeBlockStart) {
      setCurrentBlockState(md::HighlightBlockState::CodeBlock);
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

  formatCodeBlockLeadingSpaces(p_text);
  if (isCodeBlock) {
    if (highlightData->getCodeBlockHighlightTimeStamp() == result->m_codeBlockTimeStamp ||
        !result->m_codeBlockHighlightReceived) {
      highlightCodeBlock(highlightData->getCodeBlockHighlight());
    } else {
      highlightData->clearCodeBlockHighlight();
      highlightCodeBlock(result, blockNum, highlightData->getCodeBlockHighlight());
      highlightData->setCodeBlockHighlightTimeStamp(result->m_codeBlockTimeStamp);
    }
  } else if (result->m_mathHighlightReceived && result->m_mathBlockNumbers.contains(blockNum)) {
    // Overlay display math ($$...$$) LaTeX source highlight.
    const auto &mathUnits = result->getMathHighlight(blockNum);
    highlightData->getMathHighlight() = mathUnits;
    highlightData->setMathHighlightTimeStamp(result->m_mathTimeStamp);
    highlightMathBlock(mathUnits);
  } else if (!highlightData->getMathHighlight().isEmpty()) {
    // The block is no longer display math. Drop the stale math cache so that a
    // future re-classification with identical token ranges is not mistaken for
    // "already applied" by isMathHighlightMatched() and skipped.
    highlightData->clearMathHighlight();
  }

  // Do spell check.
  const bool needSpellCheck = cstate != md::HighlightBlockState::CodeBlockStart &&
                              cstate != md::HighlightBlockState::CodeBlock &&
                              cstate != md::HighlightBlockState::CodeBlockEnd;
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

bool MarkdownHighlighter::preHighlightSingleFormatBlock(
    const QVector<QVector<md::HLUnit>> &p_highlights, int p_blockNum, const QString &p_text,
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

void MarkdownHighlighter::highlightBlockOne(const QVector<QVector<md::HLUnit>> &p_highlights,
                                               int p_blockNum, QVector<md::HLUnit> &p_cache) {
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

void MarkdownHighlighter::highlightBlockOne(const QVector<md::HLUnit> &p_units) {
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
void MarkdownHighlighter::handleContentsChange(int p_position, int p_charsRemoved,
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

void MarkdownHighlighter::startParse() {
  QSharedPointer<md::MarkdownParseConfig> config(new md::MarkdownParseConfig());
  config->m_timeStamp = m_timeStamp;
  config->m_data = document()->toPlainText().toUtf8();
  config->m_numOfBlocks = document()->blockCount();
  config->m_extensions = m_parserExts;

  m_parser->parseAsync(config);
}

void MarkdownHighlighter::startFastParse(int p_position, int p_charsRemoved, int p_charsAdded) {
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

  // Call walkAndConvert directly with correct p_startBlock so HLUnits
  // are at global block indices (firstBlockNum..lastBlockNum), not 0-based.
  QByteArray utf8Data = text.toUtf8();
  int docBlockCount = document()->blockCount();
  auto walkResult = md::walkAndConvert(utf8Data, docBlockCount, offset, firstBlockNum, true);

  QSharedPointer<md::MarkdownParseConfig> config(new md::MarkdownParseConfig());
  config->m_timeStamp = m_timeStamp;
  config->m_data = utf8Data;
  config->m_numOfBlocks = docBlockCount;
  config->m_offset = offset;
  config->m_extensions = m_parserExts;
  config->m_fast = true;

  QSharedPointer<md::MarkdownParseResult> parseRes(new md::MarkdownParseResult(config));
  parseRes->m_blocksHighlights = std::move(walkResult.blocksHighlights);

  processFastParseResult(parseRes);
}

void MarkdownHighlighter::processFastParseResult(
    const QSharedPointer<md::MarkdownParseResult> &p_result) {
  m_fastResult.reset(new MarkdownHighlighterFastResult(this, p_result));

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

void MarkdownHighlighter::updateHighlight() {
  m_parseTimer->stop();
  if (m_result->matched(m_timeStamp)) {
    // No need to parse again. Already the latest.
    updateCodeBlocks(m_result);
    updateMathBlocks(m_result);
    rehighlightBlocksLater();
    completeHighlight(m_result);
  } else {
    startParse();
  }
}

void MarkdownHighlighter::handleParseResult(
    const QSharedPointer<md::MarkdownParseResult> &p_result) {
  if (!m_result.isNull() && p_result->m_timeStamp != m_timeStamp) {
    // Directly skip non-matched results to avoid highlight noise.
    return;
  }

  clearFastParseResult();

  m_result.reset(new MarkdownHighlighterResult(this, p_result, m_timeStamp, m_lastContentsChange));

  m_result->m_codeBlockTimeStamp = nextCodeBlockTimeStamp();

  m_singleFormatBlocks.clear();
  appendSingleFormatBlocks(m_result->m_blocksHighlights);

  bool matched = m_result->matched(m_timeStamp);
  if (matched) {
    clearAllBlocksUserDataAndState(m_result);

    updateAllBlocksUserDataAndState(m_result);

    updateCodeBlocks(m_result);

    updateMathBlocks(m_result);
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

void MarkdownHighlighter::clearFastParseResult() {
  m_fastParseBlocks.first = -1;
  m_fastParseBlocks.second = -1;
  m_fastResult->clear();
}

void MarkdownHighlighter::appendSingleFormatBlocks(
    const QVector<QVector<md::HLUnit>> &p_highlights) {
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

void MarkdownHighlighter::clearAllBlocksUserDataAndState(
    const QSharedPointer<MarkdownHighlighterResult> &p_result) {
  QTextBlock block = document()->firstBlock();
  while (block.isValid()) {
    clearBlockUserData(p_result, block);

    block.setUserState(md::HighlightBlockState::Normal);

    block = block.next();
  }
}

void MarkdownHighlighter::clearBlockUserData(
    const QSharedPointer<MarkdownHighlighterResult> &p_result, QTextBlock &p_block) {
  Q_UNUSED(p_result);
  const int blockNum = p_block.blockNumber();
  auto data = TextBlockData::get(p_block);
  if (!data) {
    return;
  }

  MarkdownHighlightBlockData::get(p_block)->clearOnResultReady();

  if (BlockPreviewData::get(p_block)->getPreviewData().isEmpty()) {
    m_possiblePreviewBlocks.remove(blockNum);
  } else {
    m_possiblePreviewBlocks.insert(blockNum);
  }
}

void MarkdownHighlighter::updateAllBlocksUserDataAndState(
    const QSharedPointer<MarkdownHighlighterResult> &p_result) {
  auto doc = document();

  // Code blocks.
  const QHash<int, md::HighlightBlockState> &cbStates = p_result->m_codeBlocksState;
  for (auto it = cbStates.begin(); it != cbStates.end(); ++it) {
    QTextBlock block = doc->findBlockByNumber(it.key());
    if (!block.isValid()) {
      continue;
    }
    block.setUserState(it.value());
  }
}

void MarkdownHighlighter::updateCodeBlocks(
    const QSharedPointer<MarkdownHighlighterResult> &p_result) {
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

void MarkdownHighlighter::updateMathBlocks(
    const QSharedPointer<MarkdownHighlighterResult> &p_result) {
  if (isMathEnabled() && m_config->m_mathHighlightEnabled && m_mathBlockHighlighter) {
    auto doc = document();

    // Collect display math blocks ($$...$$) only.
    //
    // Phase 1 restriction: only handle top-level formulas whose source text
    // exactly equals the physical document block lines. parseMathBlock strips
    // any leading indentation (list-contained / indented formulas), which would
    // make the Prism token offsets no longer align with the block offsets, so
    // such formulas are skipped rather than mis-highlighted.
    QVector<md::MathBlock> displays;
    for (const auto &mb : p_result->m_mathBlocks) {
      if (!mb.m_previewedAsBlock) {
        continue;
      }

      const int endBlock = mb.m_blockNumber;
      const int lineCount = mb.m_text.count(QLatin1Char('\n')) + 1;
      const int startBlock = endBlock - (lineCount - 1);
      if (startBlock < 0) {
        continue;
      }

      QString physical;
      bool ok = true;
      for (int b = startBlock; b <= endBlock; ++b) {
        QTextBlock block = doc->findBlockByNumber(b);
        if (!block.isValid()) {
          ok = false;
          break;
        }
        if (b == startBlock) {
          physical = block.text();
        } else {
          physical += QLatin1Char('\n') + block.text();
        }
      }
      if (!ok || physical != mb.m_text) {
        continue;
      }

      for (int b = startBlock; b <= endBlock; ++b) {
        p_result->m_mathBlockNumbers.insert(b);
      }
      displays.append(mb);
    }

    if (!displays.isEmpty()) {
      p_result->m_numOfMathHighlightsToRecv = displays.size();
      m_mathBlockHighlighter->highlight(p_result->m_timeStamp, displays);
      return;
    }
  }

  p_result->m_mathHighlightReceived = true;
}

void MarkdownHighlighter::applyMathHighlightResult(
    TimeStamp p_timeStamp, int p_startBlock,
    const QVector<QVector<md::HLUnitStyle>> &p_highlights) {
  QSharedPointer<MarkdownHighlighterResult> result(m_result);
  if (!result->matched(p_timeStamp) || result->m_numOfMathHighlightsToRecv <= 0) {
    return;
  }

  for (int i = 0; i < p_highlights.size(); ++i) {
    if (p_highlights[i].isEmpty()) {
      continue;
    }
    const int blockNum = p_startBlock + i;
    if (blockNum < 0) {
      continue;
    }
    result->m_mathBlockHighlights.insert(blockNum, p_highlights[i]);
  }

  if (--result->m_numOfMathHighlightsToRecv <= 0) {
    result->m_mathTimeStamp = nextCodeBlockTimeStamp();
    result->m_mathHighlightReceived = true;
    rehighlightBlocksLater();
  }
}

void MarkdownHighlighter::rehighlightBlocks() {
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

void MarkdownHighlighter::rehighlightBlocksLater() { m_rehighlightTimer->start(); }

void MarkdownHighlighter::highlightCodeBlock(
    const QSharedPointer<MarkdownHighlighterResult> &p_result, int p_blockNum,
    QVector<md::HLUnitStyle> &p_cache) {
  p_cache.clear();
  const auto &units = p_result->getCodeBlockHighlight(p_blockNum);
  if (!units.isEmpty()) {
    p_cache.append(units);
    highlightCodeBlock(units);
  }
}

void MarkdownHighlighter::highlightCodeBlock(const QVector<md::HLUnitStyle> &p_units) {
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

void MarkdownHighlighter::highlightMathBlock(const QVector<md::HLUnitStyle> &p_units) {
  for (const auto &unit : p_units) {
    // Merge the LaTeX token format over the existing markdown formatting so we
    // do not clobber the base math styling.
    QTextCharFormat newFormat = format(unit.start);
    newFormat.merge(unit.format);
    setFormat(unit.start, unit.length, newFormat);
  }
}

TimeStamp MarkdownHighlighter::nextCodeBlockTimeStamp() { return ++m_codeBlockTimeStamp; }

bool MarkdownHighlighter::isFastParseBlock(int p_blockNum) const {
  return p_blockNum >= m_fastParseBlocks.first && p_blockNum <= m_fastParseBlocks.second;
}

void MarkdownHighlighter::setTheme(const QSharedPointer<Theme> &p_theme) {
  if (m_theme == p_theme) {
    return;
  }

  m_theme = p_theme;
  Q_ASSERT(m_theme);

  qDebug() << "use Markdown highlighter theme" << m_theme->name();

  // Init m_styles from theme.
  m_styles.clear();
  m_styles.resize(Theme::MarkdownSyntaxStyle::MaxMarkdownSyntaxStyle);

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

bool MarkdownHighlighter::rehighlightBlockRange(int p_first, int p_last) {
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
    auto highlightData = MarkdownHighlightBlockData::get(block);
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
      if (it != cbStates.end() && it.value() == md::HighlightBlockState::CodeBlock) {
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

    if (!needHL) {
      // Display math source highlight: rehighlight when the math styles have
      // been (re)received for this block.
      if (m_result->m_mathHighlightReceived && m_result->m_mathBlockNumbers.contains(blockNum)) {
        if (highlightData->getMathHighlightTimeStamp() != m_result->m_mathTimeStamp) {
          const auto &mathHighlights = m_result->getMathHighlight(blockNum);
          if (highlightData->isMathHighlightMatched(mathHighlights)) {
            updateTS = true;
          } else {
            needHL = true;
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
      highlightData->setMathHighlightTimeStamp(m_result->m_mathTimeStamp);
    }

    block = block.next();
  }

  return highlighted;
}

bool MarkdownHighlighter::isEmptyCodeBlockHighlights(
    const QVector<QVector<md::HLUnitStyle>> &p_highlights) {
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

void MarkdownHighlighter::completeHighlight(QSharedPointer<MarkdownHighlighterResult> p_result) {
  m_notifyHighlightComplete = true;

  if (isMathEnabled()) {
    emit mathBlocksUpdated(p_result->m_mathBlocks);
  }

  emit tableBlocksUpdated(p_result->m_tableBlocks);

  emit imageLinksUpdated(p_result->m_imageRegions);
  emit headersUpdated(p_result->m_headerRegions);
  emit foldingRegionsUpdated(p_result->m_foldingRegions);

  // Snapshots are built here rather than when the result is constructed, so a
  // runtime change of the enabled element types takes effect on the next
  // updateHighlight() without waiting for a full reparse. The mask is carried
  // as a dynamic property because this class is exported and must not grow a
  // data member.
  const QVariant maskValue = property(InteractivePreviewHost::c_enabledTypeMaskProperty);
  const int typeMask = maskValue.isValid() ? maskValue.toInt() : 0;
  emit previewElementsUpdated(static_cast<quint64>(p_result->m_timeStamp),
                              p_result->buildPreviews(document(), typeMask));
}

bool MarkdownHighlighter::isMathEnabled() const { return m_parserExts & EXT_MATH; }

void MarkdownHighlighter::rehighlightSensitiveBlocks() {
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

const QSet<int> &MarkdownHighlighter::getPossiblePreviewBlocks() const {
  return m_possiblePreviewBlocks;
}

void MarkdownHighlighter::clearPossiblePreviewBlocks(const QVector<int> &p_blocksToClear) {
  for (auto i : p_blocksToClear) {
    m_possiblePreviewBlocks.remove(i);
  }
}

void MarkdownHighlighter::addPossiblePreviewBlock(int p_blockNumber) {
  m_possiblePreviewBlocks.insert(p_blockNumber);
}

void MarkdownHighlighter::getFastParseBlockRange(int p_position, int p_charsRemoved,
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
    if (state == md::HighlightBlockState::CodeBlock ||
        state == md::HighlightBlockState::CodeBlockEnd) {
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
        if (preState != md::HighlightBlockState::CodeBlockStart &&
            preState != md::HighlightBlockState::CodeBlock) {
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
    case md::HighlightBlockState::CodeBlockStart:
      Q_FALLTHROUGH();
    case md::HighlightBlockState::CodeBlock:
      inCodeBlock = true;
      goto godown;

    case md::HighlightBlockState::CodeBlockEnd:
      inCodeBlock = false;
      break;

    default:
      break;
    }

    // Empty block.
    if (TextEditUtils::isEmptyBlock(nextBlock) && !inCodeBlock) {
      int nstate = nextBlock.userState();
      if (nstate != md::HighlightBlockState::CodeBlockStart &&
          nstate != md::HighlightBlockState::CodeBlock &&
          nstate != md::HighlightBlockState::CodeBlockEnd) {
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

const QVector<md::ElementRegion> &MarkdownHighlighter::getHeaderRegions() const {
  return m_result->m_headerRegions;
}

const QVector<md::ElementRegion> &MarkdownHighlighter::getImageRegions() const {
  return m_result->m_imageRegions;
}

const QVector<md::FencedCodeBlock> &MarkdownHighlighter::getCodeBlocks() const {
  return m_result->m_codeBlocks;
}

static int countQuoteUnits(const QVector<md::HLUnit> &p_units) {
  int depth = 0;
  for (const auto &unit : p_units) {
    if (static_cast<int>(unit.styleIndex) == STYLE_BLOCKQUOTE) {
      ++depth;
    }
  }
  return depth;
}

md::BlockContext MarkdownHighlighter::getBlockContext(int p_blockNumber) const {
  md::BlockContext ctx;
  if (p_blockNumber < 0) {
    return ctx;
  }

  const auto doc = document();

  // Brand-new blocks may carry a synchronously propagated code block state.
  if (doc) {
    const auto block = doc->findBlockByNumber(p_blockNumber);
    if (block.isValid()) {
      const int state = block.userState();
      if (state == md::CodeBlockStart || state == md::CodeBlock || state == md::CodeBlockEnd) {
        ctx.m_inFencedCode = true;
        ctx.m_valid = true;
      }
    }
  }

  // Block numbers are not rebased, so a block count mismatch makes the result
  // unusable.
  const bool fullUsable = !m_result.isNull() && doc &&
                          doc->blockCount() == m_result->m_numOfBlocks &&
                          p_blockNumber < m_result->m_blocksHighlights.size();
  const bool fullMatched = fullUsable && m_result->matched(m_timeStamp);

  if (fullUsable) {
    const auto state = m_result->m_codeBlocksState.value(p_blockNumber, md::Normal);
    if (state == md::CodeBlockStart || state == md::CodeBlock || state == md::CodeBlockEnd) {
      ctx.m_inFencedCode = true;
    }
    ctx.m_valid = true;
  }

  if (!m_fastResult.isNull() && m_fastResult->matched(m_timeStamp) &&
      isFastParseBlock(p_blockNumber) && p_blockNumber < m_fastResult->m_blocksHighlights.size()) {
    ctx.m_quoteDepth = countQuoteUnits(m_fastResult->m_blocksHighlights.at(p_blockNumber));
    ctx.m_fresh = true;
    ctx.m_valid = true;
  } else if (fullUsable) {
    ctx.m_quoteDepth = countQuoteUnits(m_result->m_blocksHighlights.at(p_blockNumber));
    ctx.m_fresh = fullMatched;
  }

  return ctx;
}

void MarkdownHighlighter::handleCodeBlockHighlightResult(
    const CodeBlockHighlighter::HighlightResult &p_result) {
  QSharedPointer<MarkdownHighlighterResult> result(m_result);
  if (!result->matched(p_result.m_timeStamp) || result->m_numOfCodeBlockHighlightsToRecv <= 0) {
    return;
  }

  if (!p_result.isEmpty()) {
    // Set it to MarkdownHighlighterResult.
    result->setCodeBlockHighlights(p_result.m_index, p_result.m_highlights);
  }

  if (--result->m_numOfCodeBlockHighlightsToRecv <= 0) {
    result->m_codeBlockTimeStamp = nextCodeBlockTimeStamp();
    result->m_codeBlockHighlightReceived = true;
    rehighlightBlocksLater();
  }
}

const QTextCharFormat &MarkdownHighlighter::codeBlockStyle() const {
  return m_styles[Theme::MarkdownSyntaxStyle::FENCEDCODEBLOCK];
}

void MarkdownHighlighter::formatCodeBlockLeadingSpaces(const QString &p_text) {
  // Brush the indentation spaces.
  const auto state = currentBlockState();
  if (state == md::HighlightBlockState::CodeBlockStart ||
      state == md::HighlightBlockState::CodeBlock ||
      state == md::HighlightBlockState::CodeBlockEnd) {
    int spaces = TextUtils::fetchIndentation(p_text);
    if (spaces > 0) {
      setFormat(0, spaces, codeBlockStyle());
    }
  }
}

void MarkdownHighlighter::updateStylesFontSize(int p_delta) {
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
