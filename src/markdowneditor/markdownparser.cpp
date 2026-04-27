#include "markdownparser.h"

#include "cmarkadapter.h"

using namespace vte;
using namespace vte::md;

void MarkdownParseResult::parse(QAtomicInt &p_stop, bool p_fast) {
  if (p_fast) {
    return;
  }

  parseImageRegions(p_stop);

  parseHeaderRegions(p_stop);

  parseFencedCodeBlockRegions(p_stop);

  parseInlineEquationRegions(p_stop);

  parseDisplayFormulaRegions(p_stop);

  parseHRuleRegions(p_stop);

  parseTableRegions(p_stop);

  parseTableHeaderRegions(p_stop);

  parseTableBorderRegions(p_stop);
}

void MarkdownParseResult::parseImageRegions(QAtomicInt &p_stop) {
  parseRegions(p_stop, 3, m_imageRegions, false);
}

void MarkdownParseResult::parseHeaderRegions(QAtomicInt &p_stop) {
  // From Qt5.7, the capacity is preserved.
  m_headerRegions.clear();
  if (isEmpty()) {
    return;
  }

  int hx[6] = {12, 13, 14, 15, 16, 17};
  for (int i = 0; i < 6; ++i) {
    HighlightElement *elem = m_elements[hx[i]];
    while (elem != NULL) {
      if (elem->end <= elem->pos) {
        elem = elem->next;
        continue;
      }

      if (p_stop.loadAcquire() == 1) {
        return;
      }

      m_headerRegions.push_back(ElementRegion(m_offset + elem->pos, m_offset + elem->end));
      elem = elem->next;
    }
  }

  if (p_stop.loadAcquire() == 1) {
    return;
  }

  std::sort(m_headerRegions.begin(), m_headerRegions.end());
}

void MarkdownParseResult::parseFencedCodeBlockRegions(QAtomicInt &p_stop) {
  m_codeBlockRegions.clear();
  if (isEmpty()) {
    return;
  }

  HighlightElement *elem = m_elements[23];
  while (elem != NULL) {
    if (elem->end <= elem->pos) {
      elem = elem->next;
      continue;
    }

    if (p_stop.loadAcquire() == 1) {
      return;
    }

    if (!m_codeBlockRegions.contains(m_offset + elem->pos)) {
      m_codeBlockRegions.insert(m_offset + elem->pos,
                                ElementRegion(m_offset + elem->pos, m_offset + elem->end));
    }

    elem = elem->next;
  }
}

void MarkdownParseResult::parseInlineEquationRegions(QAtomicInt &p_stop) {
  parseRegions(p_stop, 28, m_inlineEquationRegions, false);
}

void MarkdownParseResult::parseDisplayFormulaRegions(QAtomicInt &p_stop) {
  parseRegions(p_stop, 27, m_displayFormulaRegions, true);
}

void MarkdownParseResult::parseHRuleRegions(QAtomicInt &p_stop) {
  parseRegions(p_stop, 21, m_hruleRegions, false);
}

void MarkdownParseResult::parseTableRegions(QAtomicInt &p_stop) {
  parseRegions(p_stop, 30, m_tableRegions, true);
}

void MarkdownParseResult::parseTableHeaderRegions(QAtomicInt &p_stop) {
  parseRegions(p_stop, 31, m_tableHeaderRegions, true);
}

void MarkdownParseResult::parseTableBorderRegions(QAtomicInt &p_stop) {
  parseRegions(p_stop, 32, m_tableBorderRegions, true);
}

void MarkdownParseResult::parseRegions(QAtomicInt &p_stop, int p_type,
                                  QVector<ElementRegion> &p_result, bool p_sort) {
  p_result.clear();
  if (isEmpty()) {
    return;
  }

  HighlightElement *elem = m_elements[p_type];
  while (elem != NULL) {
    if (elem->end <= elem->pos) {
      elem = elem->next;
      continue;
    }

    if (p_stop.loadAcquire() == 1) {
      return;
    }

    p_result.push_back(ElementRegion(m_offset + elem->pos, m_offset + elem->end));
    elem = elem->next;
  }

  if (p_sort && p_stop.loadAcquire() != 1) {
    std::sort(p_result.begin(), p_result.end());
  }
}

MarkdownParserWorker::MarkdownParserWorker(QObject *p_parent) : QThread(p_parent) {}

void MarkdownParserWorker::prepareParse(const QSharedPointer<MarkdownParseConfig> &p_config) {
  Q_ASSERT(m_parseConfig.isNull());

  m_state = WorkerState::Busy;
  m_parseConfig = p_config;
}

void MarkdownParserWorker::reset() {
  m_parseConfig.reset();
  m_parseResult.reset();
  m_stop.storeRelaxed(0);
  m_state = WorkerState::Idle;
}

void MarkdownParserWorker::stop() { m_stop.storeRelaxed(1); }

void MarkdownParserWorker::run() {
  Q_ASSERT(m_state == WorkerState::Busy);

  m_parseResult = parseMarkdown(m_parseConfig, m_stop);

  if (isAskedToStop()) {
    m_state = WorkerState::Cancelled;
    return;
  }

  m_state = WorkerState::Finished;
}

QSharedPointer<MarkdownParseResult>
MarkdownParserWorker::parseMarkdown(const QSharedPointer<MarkdownParseConfig> &p_config, QAtomicInt &p_stop) {
  QSharedPointer<MarkdownParseResult> result(new MarkdownParseResult(p_config));

  if (p_config->m_data.isEmpty()) {
    return result;
  }

  result->m_elements = MarkdownParser::parseMarkdownToElements(p_config);

  if (p_stop.loadAcquire() == 1) {
    return result;
  }

  result->parse(p_stop, p_config->m_fast);

  return result;
}

#define NUM_OF_THREADS 2

MarkdownParser::MarkdownParser(QObject *p_parent) : QObject(p_parent) { init(); }

void MarkdownParser::init() {
  for (int i = 0; i < NUM_OF_THREADS; ++i) {
    MarkdownParserWorker *th = new MarkdownParserWorker(this);
    connect(th, &MarkdownParserWorker::finished, this, [this, th]() { handleWorkerFinished(th); });

    m_workers.append(th);
  }
}

void MarkdownParser::clear() {
  m_pendingWork.reset();

  for (auto const &th : m_workers) {
    th->quit();
    th->wait();

    delete th;
  }

  m_workers.clear();
}

MarkdownParser::~MarkdownParser() { clear(); }

void MarkdownParser::parseAsync(const QSharedPointer<MarkdownParseConfig> &p_config) {
  m_pendingWork = p_config;

  pickWorker();
}

QSharedPointer<MarkdownParseResult> MarkdownParser::parse(const QSharedPointer<MarkdownParseConfig> &p_config) {
  QSharedPointer<MarkdownParseResult> result(new MarkdownParseResult(p_config));

  if (p_config->m_data.isEmpty()) {
    return result;
  }

  result->m_elements = MarkdownParser::parseMarkdownToElements(p_config);

  QAtomicInt stop(0);
  result->parse(stop, p_config->m_fast);

  return result;
}

void MarkdownParser::handleWorkerFinished(MarkdownParserWorker *p_worker) {
  QSharedPointer<MarkdownParseResult> result;
  if (p_worker->state() == MarkdownParserWorker::WorkerState::Finished) {
    result = p_worker->parseResult();
  }

  p_worker->reset();

  pickWorker();

  if (!result.isNull()) {
    emit parseResultReady(result);
  }
}

void MarkdownParser::pickWorker() {
  if (m_pendingWork.isNull()) {
    return;
  }

  bool allBusy = true;
  for (auto th : m_workers) {
    if (th->state() == MarkdownParserWorker::WorkerState::Idle) {
      scheduleWork(th, m_pendingWork);
      m_pendingWork.reset();
      return;
    } else if (th->state() != MarkdownParserWorker::WorkerState::Busy) {
      allBusy = false;
    }
  }

  if (allBusy) {
    // Need to stop the worker with non-minimal timestamp.
    int idx = 0;
    TimeStamp minTS = m_workers[idx]->workTimeStamp();

    if (m_workers.size() > 1) {
      if (m_workers[1]->workTimeStamp() > minTS) {
        idx = 1;
      }
    }

    m_workers[idx]->stop();
  }
}

void MarkdownParser::scheduleWork(MarkdownParserWorker *p_worker,
                             const QSharedPointer<MarkdownParseConfig> &p_config) {
  Q_ASSERT(p_worker->state() == MarkdownParserWorker::WorkerState::Idle);

  p_worker->reset();
  p_worker->prepareParse(p_config);
  p_worker->start();
}

QVector<ElementRegion>
MarkdownParser::parseImageRegions(const QSharedPointer<MarkdownParseConfig> &p_config) {
  QVector<ElementRegion> regs;
  HighlightElement **res = MarkdownParser::parseMarkdownToElements(p_config);
  if (!res) {
    return regs;
  }

  int offset = p_config->m_offset;
  HighlightElement *elem = res[3];
  while (elem != NULL) {
    if (elem->end <= elem->pos) {
      elem = elem->next;
      continue;
    }

    regs.push_back(ElementRegion(offset + elem->pos, offset + elem->end));
    elem = elem->next;
  }

  freeHighlightElements(res, NUM_HIGHLIGHT_STYLES);

  return regs;
}

HighlightElement **MarkdownParser::parseMarkdownToElements(const QSharedPointer<MarkdownParseConfig> &p_config) {
  if (p_config->m_data.isEmpty()) {
    return nullptr;
  }

  return parseCmark(p_config->m_data);
}

int MarkdownParser::getNumberOfStyles() { return NUM_HIGHLIGHT_STYLES; }
