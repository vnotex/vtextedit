#ifndef PEGMARKDOWNHIGHLIGHTER_H
#define PEGMARKDOWNHIGHLIGHTER_H

#include <QElapsedTimer>
#include <QTextCharFormat>

#include <vtextedit/codeblockhighlighter.h>
#include <vtextedit/pegmarkdownhighlighterdata.h>
#include <vtextedit/vsyntaxhighlighter.h>

class QScrollBar;

namespace vte {
namespace peg {
class PegParser;
struct PegParseResult;
} // namespace peg

class PegHighlighterResult;
class PegHighlighterFastResult;
class Theme;
class PegHighlightBlockData;

class PegMarkdownHighlighterInterface {
public:
  virtual ~PegMarkdownHighlighterInterface() {}

  virtual QTextCursor textCursor() const = 0;

  virtual QPair<int, int> visibleBlockRange() const = 0;

  virtual void ensureCursorVisible() = 0;

  virtual QScrollBar *verticalScrollBar() const = 0;
};

struct ContentsChange {
  int m_position = 0;
  int m_charsRemoved = 0;
  int m_charsAdded = 0;
};

// Markdown syntax highlighter via Peg-Markdown-Highlight.
class VTEXTEDIT_EXPORT PegMarkdownHighlighter : public VSyntaxHighlighter {
  Q_OBJECT
public:
  PegMarkdownHighlighter(PegMarkdownHighlighterInterface *p_interface, QTextDocument *p_doc,
                         const QSharedPointer<Theme> &p_theme,
                         CodeBlockHighlighter *p_codeBlockHighlighter,
                         const QSharedPointer<peg::HighlighterConfig> &p_config);

  void setTheme(const QSharedPointer<Theme> &p_theme);

  const QSet<int> &getPossiblePreviewBlocks() const;

  void clearPossiblePreviewBlocks(const QVector<int> &p_blocksToClear);

  void addPossiblePreviewBlock(int p_blockNumber);

  const QVector<peg::ElementRegion> &getHeaderRegions() const;

  const QVector<peg::ElementRegion> &getImageRegions() const;

  const QVector<peg::FencedCodeBlock> &getCodeBlocks() const;

  void updateStylesFontSize(int p_delta);

  // Get code block style (may not contain the font size value).
  const QTextCharFormat &codeBlockStyle() const;

public slots:
  // Rehighlight sensitive blocks using current parse result, mainly
  // visible blocks.
  void rehighlightSensitiveBlocks();

  // Parse and rehighlight immediately.
  void updateHighlight();

signals:
  void highlightCompleted();

  // QVector is implicitly shared.
  void codeBlocksUpdated(TimeStamp p_timeStamp, const QVector<peg::FencedCodeBlock> &p_codeBlocks);

  // Emitted when image regions have been fetched from a new parsing result.
  void imageLinksUpdated(const QVector<peg::ElementRegion> &p_imageRegions);

  // Emitted when header regions have been fetched from a new parsing result.
  void headersUpdated(const QVector<peg::ElementRegion> &p_headerRegions);

  // Emitted when table blocks updated.
  void tableBlocksUpdated(const QVector<peg::TableBlock> &p_tableBlocks);

  // Emitted when math blocks updated.
  void mathBlocksUpdated(const QVector<peg::MathBlock> &p_mathBlocks);

protected:
  void highlightBlock(const QString &p_text) Q_DECL_OVERRIDE;

private slots:
  void handleParseResult(const QSharedPointer<peg::PegParseResult> &p_result);

  void startParse();

  void handleContentsChange(int p_position, int p_charsRemoved, int p_charsAdded);

  void handleCodeBlockHighlightResult(const CodeBlockHighlighter::HighlightResult &p_result);

private:
  // To avoid line height jitter and code block mess.
  bool preHighlightSingleFormatBlock(const QVector<QVector<peg::HLUnit>> &p_highlights,
                                     int p_blockNum, const QString &p_text, bool p_forced);

  void highlightBlockOne(const QVector<QVector<peg::HLUnit>> &p_highlights, int p_blockNum,
                         QVector<peg::HLUnit> &p_cache);

  void highlightBlockOne(const QVector<peg::HLUnit> &p_units);

  bool isFastParseBlock(int p_blockNum) const;

  void clearFastParseResult();

  TimeStamp nextCodeBlockTimeStamp();

  void appendSingleFormatBlocks(const QVector<QVector<peg::HLUnit>> &p_highlights);

  void clearAllBlocksUserDataAndState(const QSharedPointer<PegHighlighterResult> &p_result);

  void clearBlockUserData(const QSharedPointer<PegHighlighterResult> &p_result,
                          QTextBlock &p_block);

  void updateAllBlocksUserDataAndState(const QSharedPointer<PegHighlighterResult> &p_result);

  void updateCodeBlocks(const QSharedPointer<PegHighlighterResult> &p_result);

  void rehighlightBlocks();

  void rehighlightBlocksLater();

  bool rehighlightBlockRange(int p_first, int p_last);

  void completeHighlight(QSharedPointer<PegHighlighterResult> p_result);

  bool isMathEnabled() const;

  void startFastParse(int p_position, int p_charsRemoved, int p_charsAdded);

  void getFastParseBlockRange(int p_position, int p_charsRemoved, int p_charsAdded,
                              int &p_firstBlock, int &p_lastBlock) const;

  void processFastParseResult(const QSharedPointer<peg::PegParseResult> &p_result);

  // Highlight fenced code block according to CodeBlockHighlighter result.
  void highlightCodeBlock(const QSharedPointer<PegHighlighterResult> &p_result, int p_blockNum,
                          QVector<peg::HLUnitStyle> &p_cache);

  void highlightCodeBlock(const QVector<peg::HLUnitStyle> &p_units);

  void formatCodeBlockLeadingSpaces(const QString &p_text);

  static bool isEmptyCodeBlockHighlights(const QVector<QVector<peg::HLUnitStyle>> &p_highlights);

  PegMarkdownHighlighterInterface *m_interface = nullptr;

  QSharedPointer<peg::HighlighterConfig> m_config;

  // Managed by QObject.
  CodeBlockHighlighter *m_codeBlockHighlighter = nullptr;

  // Increased sequence on contents change.
  TimeStamp m_timeStamp = 0;

  TimeStamp m_codeBlockTimeStamp = 0;

  // Extensions for peg parser.
  // Init it in the CPP to avoid including extra header.
  int m_parserExts = 0;

  // Timer interval to trigger a new parse.
  int m_parseInterval = 150;

  // Timer to trigger a new parse.
  // Managed by QObject.
  QTimer *m_parseTimer = nullptr;

  // Timer interval to trigger a new fast parse.
  int m_fastParseInterval = 50;

  // Managed by QObject.
  QTimer *m_fastParseTimer = nullptr;

  // Managed by QObject.
  peg::PegParser *m_parser = nullptr;

  QSharedPointer<PegHighlighterResult> m_result;

  QSharedPointer<PegHighlighterFastResult> m_fastResult;

  // Block range of fast parse, inclusive.
  QPair<int, int> m_fastParseBlocks = {-1, -1};

  // Blocks have only one format set which occupies the whole block.
  QSet<int> m_singleFormatBlocks;

  QSharedPointer<Theme> m_theme;

  // Highlight styles of size pmh_NUM_LANG_TYPES.
  // Index by pmh_element_type.
  QVector<QTextCharFormat> m_styles;

  // Time since last content change.
  QElapsedTimer m_contentChangeTime;

  bool m_notifyHighlightComplete = false;

  // Managed by QObject.
  QTimer *m_rehighlightTimer = nullptr;

  // Managed by QObject.
  QTimer *m_scrollRehighlightTimer = nullptr;

  // Block number of those blocks which possible contains previewed image.
  QSet<int> m_possiblePreviewBlocks;

  ContentsChange m_lastContentsChange;
};
} // namespace vte

#endif // PEGMARKDOWNHIGHLIGHTER_H
