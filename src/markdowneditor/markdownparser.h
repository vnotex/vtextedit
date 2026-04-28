#ifndef MARKDOWNPARSER_H
#define MARKDOWNPARSER_H

#include <QObject>

#include <QAtomicInt>
#include <QSharedPointer>
#include <QThread>
#include <QVector>

#include <vtextedit/global.h>

#include <vtextedit/markdownhighlighterdata.h>

#include "markdownastwalker.h"

namespace vte {
namespace md {
struct MarkdownParseConfig {
  TimeStamp m_timeStamp = 0;

  QByteArray m_data;

  int m_numOfBlocks = 0;

  // Offset of m_data in the document.
  int m_offset = 0;

  int m_extensions = 0;

  // Fast parse.
  bool m_fast = false;

  QString toString() const {
    return QStringLiteral("MarkdownParseConfig ts %1 data %2 blocks %3")
        .arg(m_timeStamp)
        .arg(m_data.size())
        .arg(m_numOfBlocks);
  }
};

struct MarkdownParseResult {
  MarkdownParseResult(const QSharedPointer<MarkdownParseConfig> &p_config)
      : m_timeStamp(p_config->m_timeStamp), m_numOfBlocks(p_config->m_numOfBlocks),
        m_offset(p_config->m_offset) {}

  ~MarkdownParseResult() = default;

  bool operator<(const MarkdownParseResult &p_other) const { return m_timeStamp < p_other.m_timeStamp; }

  QString toString() const { return QStringLiteral("MarkdownParseResult ts %1").arg(m_timeStamp); }

  bool isEmpty() const { return m_blocksHighlights.isEmpty(); }

  TimeStamp m_timeStamp = 0;

  int m_numOfBlocks = 0;

  int m_offset = 0;

  QVector<QVector<HLUnit>> m_blocksHighlights;

  // All image link regions.
  QVector<ElementRegion> m_imageRegions;

  // All header regions.
  // Sorted by start position.
  QVector<ElementRegion> m_headerRegions;

  // Fenced code block regions.
  // Ordered by start position in ascending order.
  QMap<int, ElementRegion> m_codeBlockRegions;

  // All $ $ inline equation regions.
  QVector<ElementRegion> m_inlineEquationRegions;

  // All $$ $$ display formula regions.
  // Sorted by start position.
  QVector<ElementRegion> m_displayFormulaRegions;

  // HRule regions.
  QVector<ElementRegion> m_hruleRegions;

  // All table regions.
  // Sorted by start position.
  QVector<ElementRegion> m_tableRegions;

  // All table header regions.
  QVector<ElementRegion> m_tableHeaderRegions;

  // All table border regions.
  QVector<ElementRegion> m_tableBorderRegions;

  // Folding regions (headings, code blocks, blockquotes, etc.).
  QVector<FoldingRegion> m_foldingRegions;
};

class MarkdownParserWorker : public QThread {
  Q_OBJECT
public:
  enum WorkerState { Idle, Busy, Cancelled, Finished };

  explicit MarkdownParserWorker(QObject *p_parent = nullptr);

  void prepareParse(const QSharedPointer<MarkdownParseConfig> &p_config);

  void reset();

  int state() const { return m_state; }

  TimeStamp workTimeStamp() const {
    if (m_parseConfig.isNull()) {
      return 0;
    }

    return m_parseConfig->m_timeStamp;
  }

  const QSharedPointer<MarkdownParseConfig> &parseConfig() const { return m_parseConfig; }

  const QSharedPointer<MarkdownParseResult> &parseResult() const { return m_parseResult; }

public slots:
  void stop();

protected:
  void run() Q_DECL_OVERRIDE;

private:
  QSharedPointer<MarkdownParseResult> parseMarkdown(const QSharedPointer<MarkdownParseConfig> &p_config,
                                                QAtomicInt &p_stop);

  bool isAskedToStop() const { return m_stop.loadAcquire() == 1; }

  QAtomicInt m_stop = 0;

  int m_state = WorkerState::Idle;

  QSharedPointer<MarkdownParseConfig> m_parseConfig;

  QSharedPointer<MarkdownParseResult> m_parseResult;
};

class MarkdownParser : public QObject {
  Q_OBJECT
public:
  explicit MarkdownParser(QObject *p_parent = nullptr);

  ~MarkdownParser();

  QSharedPointer<MarkdownParseResult> parse(const QSharedPointer<MarkdownParseConfig> &p_config);

  void parseAsync(const QSharedPointer<MarkdownParseConfig> &p_config);

  static QVector<ElementRegion> parseImageRegions(const QSharedPointer<MarkdownParseConfig> &p_config);

  static int getNumberOfStyles();

signals:
  void parseResultReady(const QSharedPointer<MarkdownParseResult> &p_result);

private slots:
  void handleWorkerFinished(MarkdownParserWorker *p_worker);

private:
  void init();

  void clear();

  void pickWorker();

  void scheduleWork(MarkdownParserWorker *p_worker, const QSharedPointer<MarkdownParseConfig> &p_config);

  // Maintain a fixed number of workers to pick work.
  QVector<MarkdownParserWorker *> m_workers;

  QSharedPointer<MarkdownParseConfig> m_pendingWork;
};

} // namespace md
} // namespace vte

#endif // MARKDOWNPARSER_H
