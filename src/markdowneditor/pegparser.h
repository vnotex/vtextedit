#ifndef PEGPARSER_H
#define PEGPARSER_H

#include <QObject>

#include <QAtomicInt>
#include <QSharedPointer>
#include <QThread>
#include <QVector>

#include <vtextedit/global.h>

#include <vtextedit/pegmarkdownhighlighterdata.h>

extern "C" {
#include <pmh_parser.h>
}

namespace vte {
namespace peg {
struct PegParseConfig {
  TimeStamp m_timeStamp = 0;

  QByteArray m_data;

  int m_numOfBlocks = 0;

  // Offset of m_data in the document.
  int m_offset = 0;

  int m_extensions = pmh_EXT_NONE;

  // Fast parse.
  bool m_fast = false;

  QString toString() const {
    return QStringLiteral("PegParseConfig ts %1 data %2 blocks %3")
        .arg(m_timeStamp)
        .arg(m_data.size())
        .arg(m_numOfBlocks);
  }
};

struct PegParseResult {
  PegParseResult(const QSharedPointer<PegParseConfig> &p_config)
      : m_timeStamp(p_config->m_timeStamp), m_numOfBlocks(p_config->m_numOfBlocks),
        m_offset(p_config->m_offset), m_pmhElements(NULL) {}

  ~PegParseResult() { clearPmhElements(); }

  void clearPmhElements() {
    if (m_pmhElements) {
      pmh_free_elements(m_pmhElements);
      m_pmhElements = NULL;
    }
  }

  bool operator<(const PegParseResult &p_other) const { return m_timeStamp < p_other.m_timeStamp; }

  QString toString() const { return QStringLiteral("PegParseResult ts %1").arg(m_timeStamp); }

  bool isEmpty() const { return !m_pmhElements; }

  // Parse m_pmhElements.
  void parse(QAtomicInt &p_stop, bool p_fast);

  TimeStamp m_timeStamp = 0;

  int m_numOfBlocks = 0;

  int m_offset = 0;

  pmh_element **m_pmhElements = nullptr;

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

private:
  void parseImageRegions(QAtomicInt &p_stop);

  void parseHeaderRegions(QAtomicInt &p_stop);

  void parseFencedCodeBlockRegions(QAtomicInt &p_stop);

  void parseInlineEquationRegions(QAtomicInt &p_stop);

  void parseDisplayFormulaRegions(QAtomicInt &p_stop);

  void parseHRuleRegions(QAtomicInt &p_stop);

  void parseTableRegions(QAtomicInt &p_stop);

  void parseTableHeaderRegions(QAtomicInt &p_stop);

  void parseTableBorderRegions(QAtomicInt &p_stop);

  void parseRegions(QAtomicInt &p_stop, pmh_element_type p_type, QVector<ElementRegion> &p_result,
                    bool p_sort = false);
};

class PegParserWorker : public QThread {
  Q_OBJECT
public:
  enum WorkerState { Idle, Busy, Cancelled, Finished };

  explicit PegParserWorker(QObject *p_parent = nullptr);

  void prepareParse(const QSharedPointer<PegParseConfig> &p_config);

  void reset();

  int state() const { return m_state; }

  TimeStamp workTimeStamp() const {
    if (m_parseConfig.isNull()) {
      return 0;
    }

    return m_parseConfig->m_timeStamp;
  }

  const QSharedPointer<PegParseConfig> &parseConfig() const { return m_parseConfig; }

  const QSharedPointer<PegParseResult> &parseResult() const { return m_parseResult; }

public slots:
  void stop();

protected:
  void run() Q_DECL_OVERRIDE;

private:
  QSharedPointer<PegParseResult> parseMarkdown(const QSharedPointer<PegParseConfig> &p_config,
                                               QAtomicInt &p_stop);

  bool isAskedToStop() const { return m_stop.loadAcquire() == 1; }

  QAtomicInt m_stop = 0;

  int m_state = WorkerState::Idle;

  QSharedPointer<PegParseConfig> m_parseConfig;

  QSharedPointer<PegParseResult> m_parseResult;
};

class PegParser : public QObject {
  Q_OBJECT
public:
  explicit PegParser(QObject *p_parent = nullptr);

  ~PegParser();

  QSharedPointer<PegParseResult> parse(const QSharedPointer<PegParseConfig> &p_config);

  void parseAsync(const QSharedPointer<PegParseConfig> &p_config);

  static QVector<ElementRegion> parseImageRegions(const QSharedPointer<PegParseConfig> &p_config);

  // MUST pmh_free_elements() the result.
  static pmh_element **parseMarkdownToElements(const QSharedPointer<PegParseConfig> &p_config);

  static int getNumberOfStyles();

signals:
  void parseResultReady(const QSharedPointer<PegParseResult> &p_result);

private slots:
  void handleWorkerFinished(PegParserWorker *p_worker);

private:
  void init();

  void clear();

  void pickWorker();

  void scheduleWork(PegParserWorker *p_worker, const QSharedPointer<PegParseConfig> &p_config);

  // Maintain a fixed number of workers to pick work.
  QVector<PegParserWorker *> m_workers;

  QSharedPointer<PegParseConfig> m_pendingWork;
};

} // namespace peg
} // namespace vte

#endif // PEGPARSER_H
