#include "markdownparser.h"

#include "markdownastwalker.h"

using namespace vte;
using namespace vte::md;

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

  auto walkResult = walkAndConvert(p_config->m_data, p_config->m_numOfBlocks,
                                   p_config->m_offset, 0, p_config->m_fast);

  if (p_stop.loadAcquire() == 1) {
    return result;
  }

  // Move walk results into parse result.
  result->m_blocksHighlights = std::move(walkResult.blocksHighlights);
  if (!p_config->m_fast) {
    result->m_imageRegions = std::move(walkResult.imageRegions);
    result->m_headerRegions = std::move(walkResult.headerRegions);
    result->m_codeBlockRegions = std::move(walkResult.codeBlockRegions);
    result->m_inlineEquationRegions = std::move(walkResult.inlineEquationRegions);
    result->m_displayFormulaRegions = std::move(walkResult.displayFormulaRegions);
    result->m_hruleRegions = std::move(walkResult.hruleRegions);
    result->m_tableRegions = std::move(walkResult.tableRegions);
    result->m_tableHeaderRegions = std::move(walkResult.tableHeaderRegions);
    result->m_tableBorderRegions = std::move(walkResult.tableBorderRegions);
    result->m_foldingRegions = std::move(walkResult.foldingRegions);
  }

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

  auto walkResult = walkAndConvert(p_config->m_data, p_config->m_numOfBlocks,
                                   p_config->m_offset, 0, p_config->m_fast);

  result->m_blocksHighlights = std::move(walkResult.blocksHighlights);
  if (!p_config->m_fast) {
    result->m_imageRegions = std::move(walkResult.imageRegions);
    result->m_headerRegions = std::move(walkResult.headerRegions);
    result->m_codeBlockRegions = std::move(walkResult.codeBlockRegions);
    result->m_inlineEquationRegions = std::move(walkResult.inlineEquationRegions);
    result->m_displayFormulaRegions = std::move(walkResult.displayFormulaRegions);
    result->m_hruleRegions = std::move(walkResult.hruleRegions);
    result->m_tableRegions = std::move(walkResult.tableRegions);
    result->m_tableHeaderRegions = std::move(walkResult.tableHeaderRegions);
    result->m_tableBorderRegions = std::move(walkResult.tableBorderRegions);
    result->m_foldingRegions = std::move(walkResult.foldingRegions);
  }

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
  if (p_config->m_data.isEmpty()) {
    return {};
  }

  auto walkResult = walkAndConvert(p_config->m_data, p_config->m_numOfBlocks,
                                   p_config->m_offset, 0, false);
  return walkResult.imageRegions;
}

int MarkdownParser::getNumberOfStyles() { return 33; }
