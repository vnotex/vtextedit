#include <vtextedit/vmarkdowneditor.h>

#include <inputmode/abstractinputmode.h>
#include <inputmode/inputmodemgr.h>
#include <vtextedit/markdowneditorconfig.h>
#include <vtextedit/markdownhighlighter.h>
#include <vtextedit/markdownutils.h>
#include <vtextedit/textblockdata.h>
#include <vtextedit/texteditutils.h>
#include <vtextedit/textutils.h>
#include <vtextedit/theme.h>
#include <vtextedit/vtextedit.h>

#include <texteditor/textfolding.h>

#include "documentresourcemgr.h"
#include "editormarkdownhighlighter.h"
#include "editorpreviewmgr.h"
#include "headingsourcenumberer.h"
#include "interactivepreviewhost.h"
#include "ksyntaxcodeblockhighlighter.h"
#include "markdownastwalker.h"
#include "markdownfoldingprovider.h"
#include "markdownhighlighterresult.h"
#include "mathblockhighlighter.h"
#include "previewbuilder.h"
#include "previewfromast.h"
#include "tablepreviewwidget.h"
#include "textdocumentlayout.h"
#include "webcodeblockhighlighter.h"

#include <QApplication>
#include <QDebug>
#include <QFontMetricsF>
#include <QHelpEvent>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QStringList>
#include <QTextBoundaryFinder>
#include <QTextDocument>
#include <QTextLayout>
#include <QThread>
#include <QTimer>
#include <QToolTip>

#include <algorithm>
#include <utility>

using namespace vte;

namespace vte {
class ListSourceWorker final : public QThread {
  Q_OBJECT
public:
  enum class Work { Baseline, Numbering };

  explicit ListSourceWorker(QObject *p_parent) : QThread(p_parent) {}

  void prepare(Work p_work, quint64 p_epoch, quint64 p_generation,
               const md::ListStructure &p_structure, const QByteArray &p_seed,
               const QHash<int, int> &p_listStarts) {
    Q_ASSERT(!isRunning());
    m_work = p_work;
    m_epoch = p_epoch;
    m_generation = p_generation;
    m_structure = p_structure;
    m_seed = p_seed;
    m_starts = p_listStarts;
    m_edits.clear();
    m_succeeded = false;
  }

  const md::ListStructure &structure() const { return m_structure; }
  const QVector<md::ListSourceEdit> &edits() const { return m_edits; }
  bool succeeded() const { return m_succeeded; }

protected:
  void run() Q_DECL_OVERRIDE {
    if (isInterruptionRequested()) {
      return;
    }
    if (m_work == Work::Baseline) {
      m_structure = md::parseListStructure(m_seed);
      m_seed.clear();
      m_succeeded = m_structure.m_valid && !isInterruptionRequested();
      return;
    }
    // The controller already compared values and digit widths. Decode only a
    // nonempty proposal, on this thread; the builder owns its two-tree check.
    const QString source = QString::fromUtf8(m_structure.m_source);
    if (isInterruptionRequested()) {
      return;
    }
    md::ListStructure after;
    const bool accepted = md::buildListNumberEdits(source, m_structure, m_starts, m_edits, after);
    if (!accepted || isInterruptionRequested()) {
      m_edits.clear();
      return;
    }
    m_structure = std::move(after);
    m_succeeded = true;
  }

private:
  Work m_work = Work::Baseline;
  quint64 m_epoch = 0;
  quint64 m_generation = 0;
  md::ListStructure m_structure;
  QByteArray m_seed;
  QHash<int, int> m_starts;
  QVector<md::ListSourceEdit> m_edits;
  bool m_succeeded = false;
};

// Scheduling and source positions belong to the editor, not to a preview sheet.
// This QObject child deliberately adds no state to an exported class.
class MarkdownSourceFormatter final : public QObject {
  Q_OBJECT
public:
  explicit MarkdownSourceFormatter(VMarkdownEditor *p_editor)
      : QObject(p_editor), m_editor(p_editor), m_doc(p_editor->document()),
        m_worker(new ListSourceWorker(this)) {
    setObjectName(QStringLiteral("vte_markdown_source_formatter"));
    m_timer.setSingleShot(true);
    m_timer.setTimerType(Qt::PreciseTimer);
    m_timer.setInterval(500);
    connect(&m_timer, &QTimer::timeout, this, &MarkdownSourceFormatter::attempt);
    connect(m_doc, &QTextDocument::contentsChange, this, &MarkdownSourceFormatter::contentsChange);
    connect(m_doc, &QTextDocument::contentsChanged, this,
            &MarkdownSourceFormatter::contentsChanged);
    connect(m_doc, &QTextDocument::undoCommandAdded, this, [this]() {
      if (!m_applying) {
        m_newUndoCommand = true;
      }
    });
    connect(p_editor->getHighlighter(), &MarkdownHighlighter::highlightCompleted, this,
            &MarkdownSourceFormatter::queueAttempt);
    connect(p_editor->documentLayout(), &TextDocumentLayout::becameIdle, this,
            &MarkdownSourceFormatter::queueAttempt);
    connect(m_worker, &QThread::finished, this, &MarkdownSourceFormatter::workerFinished);
    auto edit = p_editor->getTextEdit();
    connect(edit, &VTextEdit::openLineRequested, this,
            [this](VTextEdit::BlockInsertion p_placement, bool *p_handled) {
              if (!*p_handled) {
                *p_handled = handleListInsertion(p_placement);
              }
            });
    connect(edit, &QTextEdit::cursorPositionChanged, this, &MarkdownSourceFormatter::queueAttempt);
    connect(edit, &QTextEdit::selectionChanged, this, &MarkdownSourceFormatter::queueAttempt);
    edit->installEventFilter(this);
    edit->viewport()->installEventFilter(this);
    observeDocument();
  }

  ~MarkdownSourceFormatter() Q_DECL_OVERRIDE {
    m_timer.stop();
    m_worker->requestInterruption();
    m_worker->quit();
    m_worker->wait();
  }

  void setEnabled(bool p_tablesEnabled, bool p_listsEnabled);
  bool handleListInsertion(VTextEdit::BlockInsertion p_operation);
  bool takeListReturnSuppression();

protected:
  bool eventFilter(QObject *p_object, QEvent *p_event) Q_DECL_OVERRIDE {
    Q_UNUSED(p_object);
    if (p_event->type() == QEvent::InputMethod || p_event->type() == QEvent::FocusIn ||
        p_event->type() == QEvent::FocusOut) {
      queueAttempt();
    }
    return false;
  }

private:
  // Classification is read-only and precedes the single insertion transaction.
  struct ListInsertionPlan {
    QString m_prefix;
    int m_removeStart = -1;
    int m_removeEnd = -1;
    bool m_suppressFallback = false;
  };

  bool resolveListInsertion(QTextCursor p_origin, VTextEdit::BlockInsertion p_operation,
                            ListInsertionPlan &p_plan) const;

  struct Baseline {
    QTextBlock m_block;
    QString m_text;
  };

  struct Row {
    int m_position = 0;
    QString m_before;
    QString m_after;
    QVector<QString> m_cellsBefore;
    QVector<QString> m_cellsAfter;
    QVector<int> m_offsetsBefore;
    QVector<int> m_offsetsAfter;
    QVector<int> m_bordersBefore;
    QVector<int> m_bordersAfter;
    bool m_delimiter = false;
    bool map(int p_position, int &p_mapped) const;
  };

  struct Mutation {
    int m_position;
    int m_removed;
    int m_added;
  };

  struct SourceRun {
    int m_old;
    int m_current;
    int m_length;
  };

  struct Marker {
    QTextBlock m_block;
    int m_blockStart = 0;
    int m_blockLength = 0;
    int m_start = -1;
    int m_end = -1;
    int m_ordinal = 0;
    int m_directOrdinal = 0;
    int m_list = -1;
    int m_container = -1;
    int m_number = 0;
    QChar m_marker;
    QString m_prefix;
    QString m_spelling;
    bool m_valid = false;
  };

  struct ListBaseline {
    QVector<Marker> m_markers;
    QVector<md::ListInfo> m_lists;
    QVector<md::ListContainerInfo> m_containers;
    int m_characters = 0;
    bool m_valid = false;
  };

  struct SeedBlock {
    QTextBlock m_block;
    int m_position;
    int m_length;
  };

  struct Replacement {
    int m_position;
    QString m_before;
    QString m_after;
    int m_row = -1;
  };

  struct Endpoints {
    QVector<int> m_positions;
    bool m_overridden = false;
  };

  void observeDocument();
  void resetBaselines();
  QHash<int, int> baselinePositions() const;
  void captureBaseline(const QSet<int> &p_deferredBlocks = QSet<int>());
  void contentsChange(int p_position, int p_removed, int p_added);
  void contentsChanged();
  bool hasPending() const;
  void queueAttempt();
  void attempt();
  bool guarded() const;
  const MarkdownHighlighterResult *freshResult() const;

  void beginListEpoch();
  void startSeed();
  void releaseWorkerResult();
  void workerFinished();
  void bindListBaseline(const md::ListStructure &p_structure,
                        const QVector<SeedBlock> *p_seedBlocks = nullptr,
                        const QVector<Replacement> &p_afterEdits = {});
  bool survivingSource(QVector<SourceRun> &p_runs) const;
  QVector<Marker> currentMarkers(const md::ListStructure &p_structure,
                                 const QVector<SeedBlock> *p_seedBlocks = nullptr,
                                 const QVector<Replacement> &p_afterEdits = {}) const;
  QHash<int, int> changedLists(const md::ListStructure &p_structure,
                               QVector<SourceRun> &p_survivingSource) const;
  static bool needsNumbering(const md::ListStructure &p_structure, const QHash<int, int> &p_starts);
  static bool sameMarker(const Marker &p_left, const Marker &p_right);
  bool hasListMarkersInOpenFence(const MarkdownHighlighterResult &p_result,
                                 const QVector<SourceRun> &p_runs) const;
  void prepareLists(const MarkdownHighlighterResult &p_result);

  Endpoints endpoints() const;
  bool prepareTable(const md::TableElement &p_table, QVector<Row> &p_rows) const;
  void prepareTables(const QVector<md::TableElement> &p_tables, const Endpoints &p_endpoints,
                     QVector<Row> &p_rows, QSet<int> &p_deferred) const;
  bool apply(const QVector<Row> &p_rows, const QVector<md::ListSourceEdit> &p_listEdits,
             Endpoints p_endpoints, quint64 p_generation, int p_revision, bool p_tables,
             bool p_lists);

  VMarkdownEditor *m_editor;
  QTextDocument *m_doc;
  ListSourceWorker *m_worker;
  QTimer m_timer;
  QElapsedTimer m_idle;
  QVector<Baseline> m_baseline;
  QVector<Baseline> m_nextBaseline;
  ListBaseline m_listBaseline;
  QVector<Mutation> m_mutations;
  QVector<SeedBlock> m_seedBlocks;
  QByteArray m_seed;
  int m_seedCharacters = 0;
  quint64 m_generation = 0;
  quint64 m_epoch = 0;
  quint64 m_jobEpoch = 0;
  quint64 m_jobGeneration = 0;
  TimeStamp m_jobTimeStamp = 0;
  ListSourceWorker::Work m_jobWork = ListSourceWorker::Work::Baseline;
  int m_revision = 0;
  int m_characters = 1;
  int m_undoSteps = 0;
  int m_redoSteps = 0;
  bool m_enabled = false;
  bool m_listsEnabled = false;
  bool m_pending = false;
  bool m_listsPending = false;
  bool m_seedRequired = false;
  bool m_workerActive = false;
  bool m_numberingReady = false;
  bool m_preserveListBaseline = false;
  bool m_queued = false;
  bool m_applying = false;
  bool m_reset = false;
  bool m_sourceChanged = false;
  bool m_newUndoCommand = false;
  bool m_listReturnSuppressed = false;
};

void MarkdownSourceFormatter::setEnabled(bool p_tablesEnabled, bool p_listsEnabled) {
  if (m_enabled == p_tablesEnabled && m_listsEnabled == p_listsEnabled) {
    return;
  }
  if (m_enabled != p_tablesEnabled) {
    m_enabled = p_tablesEnabled;
    m_pending = false;
    m_baseline.clear();
    m_nextBaseline.clear();
    if (m_enabled) {
      captureBaseline();
    }
  }
  if (m_listsEnabled != p_listsEnabled) {
    m_listsEnabled = p_listsEnabled;
    beginListEpoch();
  }
  // A config change is not source activity. In particular it must neither
  // restart the other mode's deadline nor consume its undo/revision evidence.
  if (!hasPending()) {
    m_timer.stop();
  } else {
    queueAttempt();
  }
}

void MarkdownSourceFormatter::observeDocument() {
  m_revision = m_doc->revision();
  m_characters = m_doc->characterCount();
  m_undoSteps = m_doc->availableUndoSteps();
  m_redoSteps = m_doc->availableRedoSteps();
  m_newUndoCommand = false;
  m_sourceChanged = false;
}

void MarkdownSourceFormatter::resetBaselines() {
  ++m_generation;
  m_pending = false;
  m_timer.stop();
  m_baseline.clear();
  m_nextBaseline.clear();
  if (m_enabled) {
    captureBaseline();
  }
  beginListEpoch();
}

QHash<int, int> MarkdownSourceFormatter::baselinePositions() const {
  QHash<int, int> positions;
  positions.reserve(m_baseline.size());
  for (int i = 0; i < m_baseline.size(); ++i) {
    if (m_baseline[i].m_block.isValid()) {
      positions.insert(m_baseline[i].m_block.position(), i);
    }
  }
  return positions;
}

void MarkdownSourceFormatter::captureBaseline(const QSet<int> &p_deferredBlocks) {
  const auto positions = baselinePositions();
  m_nextBaseline.resize(0);
  m_nextBaseline.reserve(m_doc->blockCount());
  for (auto block = m_doc->begin(); block.isValid(); block = block.next()) {
    const int old = positions.value(block.position(), -1);
    const bool retained = old >= 0 && m_baseline[old].m_block == block;
    if (p_deferredBlocks.contains(block.blockNumber())) {
      if (retained) {
        m_nextBaseline.append(m_baseline[old]);
      }
      continue;
    }
    const QString text = block.text();
    if (retained && m_baseline[old].m_text == text) {
      m_nextBaseline.append(m_baseline[old]);
    } else {
      m_nextBaseline.append({block, text});
    }
  }
  m_baseline.swap(m_nextBaseline);
}

void MarkdownSourceFormatter::contentsChange(int p_position, int p_removed, int p_added) {
  const bool reset = p_position == 0 && p_added == 0 && p_removed > m_characters - 1 &&
                     m_doc->characterCount() == 1;
  if (reset) {
    m_reset = !m_doc->isUndoRedoEnabled();
    resetBaselines();
    observeDocument();
    return;
  }
  if (m_reset && (p_position != 0 || p_removed != 0 || p_added != m_doc->characterCount())) {
    m_reset = false;
  }
  // QTextDocument reports document-wide format changes as a replacement that
  // includes its terminal character. A cursor source replacement cannot cover
  // that character; clear/setPlainText resets were handled above. Treating this
  // as deleted source would erase every baseline marker during a rehighlight.
  if (!m_reset && p_position == 0 && p_removed == p_added && p_removed == m_characters &&
      p_added == m_doc->characterCount()) {
    if (!m_sourceChanged && !m_applying) {
      observeDocument();
    }
    return;
  }
  if ((p_removed != 0 || p_added != 0) && m_doc->revision() != m_revision) {
    // This slot never visits a marker, copies source or follows a block chain.
    // The shared apply path records its own exact edits separately.
    if (m_listsEnabled && !m_reset && !m_applying) {
      if (!m_mutations.isEmpty() && p_removed == 0 && m_mutations.last().m_removed == 0 &&
          p_position == m_mutations.last().m_position + m_mutations.last().m_added) {
        m_mutations.last().m_added += p_added;
      } else {
        m_mutations.append({p_position, p_removed, p_added});
      }
    }
    if (!m_applying) {
      ++m_generation;
      m_sourceChanged = true;
      if (m_workerActive && m_jobWork == ListSourceWorker::Work::Numbering) {
        m_worker->requestInterruption();
      }
    }
  }
  m_characters = m_doc->characterCount();
}

void MarkdownSourceFormatter::contentsChanged() {
  if (m_reset) {
    m_reset = false;
    resetBaselines();
    observeDocument();
    return;
  }
  if (m_applying || (!m_enabled && !m_listsEnabled)) {
    observeDocument();
    return;
  }
  if (!m_sourceChanged || m_doc->revision() == m_revision) {
    return;
  }
  const int undo = m_doc->availableUndoSteps();
  const int redo = m_doc->availableRedoSteps();
  const bool replay =
      m_doc->isUndoRedoEnabled() &&
      (undo < m_undoSteps || (undo > m_undoSteps && redo < m_redoSteps && !m_newUndoCommand));
  if (replay) {
    resetBaselines();
  } else {
    if (m_editor->isReadOnly()) {
      m_pending = false;
      if (m_enabled) {
        captureBaseline();
      }
    } else {
      m_pending = m_pending || m_enabled;
    }
    m_listsPending = m_listsPending || m_listsEnabled;
    m_idle.restart();
    m_timer.start();
  }
  observeDocument();
}

bool MarkdownSourceFormatter::hasPending() const {
  return m_pending || m_listsPending || m_seedRequired || m_numberingReady;
}

void MarkdownSourceFormatter::queueAttempt() {
  if (!hasPending() || m_applying || m_queued) {
    return;
  }
  m_queued = true;
  QTimer::singleShot(0, this, [this]() {
    m_queued = false;
    attempt();
  });
}

const MarkdownHighlighterResult *MarkdownSourceFormatter::freshResult() const {
  const auto highlighter = m_editor->getHighlighter();
  if (!highlighter->m_result || !highlighter->m_result->matched(highlighter->m_timeStamp)) {
    return nullptr;
  }
  return highlighter->m_result.data();
}

bool MarkdownSourceFormatter::guarded() const {
  if (m_editor->isReadOnly() || m_applying || !m_idle.isValid() || m_idle.elapsed() < 500) {
    return false;
  }
  auto layout = m_editor->documentLayout();
  if (layout->isBusy()) {
    layout->requestIdleNotification();
    return false;
  }
  auto edit = m_editor->getTextEdit();
  if (edit->isViewportWidgetFocused() ||
      !edit->getSelections().getAdditionalSelections().isEmpty()) {
    return false;
  }
  for (auto block = m_doc->begin(); block.isValid(); block = block.next()) {
    if (block.layout() && !block.layout()->preeditAreaText().isEmpty()) {
      return false;
    }
  }
  return true;
}

void MarkdownSourceFormatter::beginListEpoch() {
  ++m_epoch;
  m_listsPending = false;
  m_seedRequired = false;
  m_numberingReady = false;
  m_preserveListBaseline = false;
  m_seed.clear();
  m_seedBlocks.clear();
  m_mutations.clear();
  m_listBaseline = {};
  if (m_workerActive) {
    m_worker->requestInterruption();
  } else {
    releaseWorkerResult();
  }
  if (!m_listsEnabled) {
    return;
  }
  if (m_doc->isEmpty()) {
    m_listBaseline.m_valid = true;
    return;
  }
  if (const auto result = freshResult()) {
    if (result->m_listStructure.m_valid) {
      bindListBaseline(result->m_listStructure);
      return;
    }
  }
  // Once per epoch, before returning to the caller which may immediately edit.
  // Keep this seed (not a later highlight) until its old markers are bound.
  m_seed = m_doc->toPlainText().toUtf8();
  m_seedCharacters = m_doc->characterCount() - 1;
  m_seedBlocks.reserve(m_doc->blockCount());
  for (auto block = m_doc->begin(); block.isValid(); block = block.next()) {
    m_seedBlocks.append({block, block.position(), block.length()});
  }
  m_seedRequired = true;
  startSeed();
}

void MarkdownSourceFormatter::releaseWorkerResult() {
  Q_ASSERT(!m_workerActive);
  m_worker->prepare(ListSourceWorker::Work::Baseline, m_epoch, m_generation, {}, {}, {});
}

void MarkdownSourceFormatter::startSeed() {
  if (!m_listsEnabled || !m_seedRequired || m_workerActive) {
    return;
  }
  m_jobWork = ListSourceWorker::Work::Baseline;
  m_jobEpoch = m_epoch;
  m_jobGeneration = m_generation;
  m_worker->prepare(m_jobWork, m_jobEpoch, m_jobGeneration, {}, m_seed, {});
  m_seed.clear();
  m_workerActive = true;
  m_worker->start();
}

void MarkdownSourceFormatter::workerFinished() {
  m_workerActive = false;
  if (!m_listsEnabled || m_jobEpoch != m_epoch) {
    releaseWorkerResult();
    startSeed();
    queueAttempt();
    return;
  }
  if (m_jobWork == ListSourceWorker::Work::Baseline) {
    if (m_worker->succeeded()) {
      // Do not require the source generation to match: the seed intentionally
      // predates immediate edits. Its journal has not been cleared meanwhile.
      bindListBaseline(m_worker->structure(), &m_seedBlocks);
    }
    m_seedRequired = false;
    m_seedBlocks.clear();
    releaseWorkerResult();
  } else {
    const auto result = freshResult();
    if (m_jobGeneration == m_generation && result && result->m_timeStamp == m_jobTimeStamp) {
      if (m_worker->succeeded()) {
        m_numberingReady = true;
      } else {
        // A rejected batch is consumed. Only a later real structural change
        // can schedule it again; cursor/layout notifications cannot spin it.
        if (!m_preserveListBaseline) {
          bindListBaseline(result->m_listStructure);
        }
        m_listsPending = false;
        releaseWorkerResult();
      }
    } else {
      releaseWorkerResult();
    }
  }
  queueAttempt();
}

QVector<MarkdownSourceFormatter::Marker>
MarkdownSourceFormatter::currentMarkers(const md::ListStructure &p_structure,
                                        const QVector<SeedBlock> *p_seedBlocks,
                                        const QVector<Replacement> &p_afterEdits) const {
  QVector<Marker> markers;
  markers.reserve(p_structure.m_items.size());
  int previousBlock = -1;
  int cachedBlock = -1;
  int ordinal = 0;
  int sourceBlock = 0;
  int sourceLine = 0;
  int sourceEnd = -1;
  QString line;
  int editIndex = 0;
  int delta = 0;
  for (const auto &item : p_structure.m_items) {
    Marker marker;
    marker.m_list = item.m_list;
    marker.m_container = item.m_container;
    marker.m_number = item.m_sourceNumber;
    marker.m_marker = item.m_marker;
    marker.m_ordinal = item.m_startBlock == previousBlock ? ++ordinal : (ordinal = 0);
    if (item.m_startBlock < 0 || !item.m_sourceValid) {
      markers.append(std::move(marker));
      previousBlock = item.m_startBlock;
      continue;
    }
    int start = item.m_markerStart;
    int end = item.m_markerEnd;
    if (p_seedBlocks) {
      if (item.m_startBlock >= p_seedBlocks->size()) {
        markers.append(std::move(marker));
        previousBlock = item.m_startBlock;
        continue;
      }
      const auto &captured = (*p_seedBlocks)[item.m_startBlock];
      marker.m_block = captured.m_block;
      marker.m_blockStart = captured.m_position;
      marker.m_blockLength = captured.m_length;
      if (cachedBlock != item.m_startBlock) {
        while (sourceBlock < item.m_startBlock) {
          const int newline = p_structure.m_source.indexOf('\n', sourceLine);
          if (newline < 0) {
            sourceLine = p_structure.m_source.size();
            break;
          }
          sourceLine = newline + 1;
          ++sourceBlock;
        }
        sourceEnd = p_structure.m_source.indexOf('\n', sourceLine);
        if (sourceEnd < 0) {
          sourceEnd = p_structure.m_source.size();
        }
        line = QString::fromUtf8(p_structure.m_source.constData() + sourceLine,
                                 sourceEnd - sourceLine);
        cachedBlock = item.m_startBlock;
      }
    } else {
      marker.m_block = m_doc->findBlockByNumber(item.m_startBlock);
      if (!marker.m_block.isValid()) {
        markers.append(std::move(marker));
        previousBlock = item.m_startBlock;
        continue;
      }
      marker.m_blockStart = marker.m_block.position();
      marker.m_blockLength = marker.m_block.length();
      while (editIndex < p_afterEdits.size() &&
             p_afterEdits[editIndex].m_position + p_afterEdits[editIndex].m_before.size() < start) {
        delta += p_afterEdits[editIndex].m_after.size() - p_afterEdits[editIndex].m_before.size();
        ++editIndex;
      }
      // Table prefixes retain the exact marker spelling and offset. No list
      // marker lies in a table cell, so an enclosing row preserves this offset.
      start += delta;
      end += delta;
      if (cachedBlock != item.m_startBlock) {
        line = marker.m_block.text();
        cachedBlock = item.m_startBlock;
      }
    }
    const int offset = start - marker.m_blockStart;
    const int length = end - start;
    if (offset >= 0 && length > 0 && offset <= line.size() && length <= line.size() - offset) {
      marker.m_start = start;
      marker.m_end = end;
      marker.m_prefix = line.left(offset);
      marker.m_spelling = line.mid(offset, length);
      md::ListItemInfo scanned;
      marker.m_valid =
          md::scanListMarker(line, offset, scanned) && scanned.m_markerEnd == offset + length &&
          scanned.m_marker == item.m_marker && scanned.m_sourceNumber == item.m_sourceNumber;
    }
    markers.append(std::move(marker));
    previousBlock = item.m_startBlock;
  }
  for (const auto &list : p_structure.m_lists) {
    for (int i = 0; i < list.m_items.size(); ++i) {
      markers[list.m_items[i]].m_directOrdinal = i;
    }
  }
  return markers;
}

void MarkdownSourceFormatter::bindListBaseline(const md::ListStructure &p_structure,
                                               const QVector<SeedBlock> *p_seedBlocks,
                                               const QVector<Replacement> &p_afterEdits) {
  m_listBaseline.m_markers = currentMarkers(p_structure, p_seedBlocks, p_afterEdits);
  m_listBaseline.m_lists = p_structure.m_lists;
  m_listBaseline.m_containers = p_structure.m_containers;
  m_listBaseline.m_characters = p_seedBlocks ? m_seedCharacters : m_doc->characterCount() - 1;
  m_listBaseline.m_valid = p_structure.m_valid;
  if (!p_seedBlocks) {
    m_mutations.clear();
  }
}

bool MarkdownSourceFormatter::survivingSource(QVector<SourceRun> &p_runs) const {
  // Build an implicit piece tree from the coalesced journal, not one marker
  // update per key. Splitting/splicing costs O(log journal); the final ordered
  // run sweep applies suffix deltas to every unaffected marker just once.
  struct Pieces {
    struct Node {
      int left = 0;
      int right = 0;
      int old = -1;
      int length = 0;
      int total = 0;
      quint32 priority = 0;
    };
    QVector<Node> nodes{Node{}};
    quint32 state = 0x9e3779b9U;
    int size(int n) const { return nodes[n].total; }
    void update(int n) {
      if (n) {
        nodes[n].total = size(nodes[n].left) + nodes[n].length + size(nodes[n].right);
      }
    }
    int make(int old, int length) {
      if (length <= 0) {
        return 0;
      }
      state ^= state << 13;
      state ^= state >> 17;
      state ^= state << 5;
      nodes.append({0, 0, old, length, length, state});
      return nodes.size() - 1;
    }
    int merge(int left, int right) {
      if (!left || !right) {
        return left ? left : right;
      }
      if (nodes[left].priority < nodes[right].priority) {
        nodes[left].right = merge(nodes[left].right, right);
        update(left);
        return left;
      }
      nodes[right].left = merge(left, nodes[right].left);
      update(right);
      return right;
    }
    std::pair<int, int> split(int root, int position) {
      if (!root) {
        return {0, 0};
      }
      const int leftSize = size(nodes[root].left);
      if (position <= leftSize) {
        const auto parts = split(nodes[root].left, position);
        nodes[root].left = 0;
        update(root);
        return {parts.first, merge(parts.second, root)};
      }
      if (position >= leftSize + nodes[root].length) {
        const auto parts = split(nodes[root].right, position - leftSize - nodes[root].length);
        nodes[root].right = 0;
        update(root);
        return {merge(root, parts.first), parts.second};
      }
      const int cut = position - leftSize;
      const int right = nodes[root].right;
      const int old = nodes[root].old < 0 ? -1 : nodes[root].old + cut;
      const int fragment = make(old, nodes[root].length - cut);
      nodes[root].length = cut;
      nodes[root].right = 0;
      update(root);
      return {root, merge(fragment, right)};
    }
    void collect(int root, int &position, QVector<SourceRun> &runs) const {
      if (!root) {
        return;
      }
      const auto &node = nodes[root];
      collect(node.left, position, runs);
      if (node.old >= 0) {
        if (!runs.isEmpty() && runs.last().m_old + runs.last().m_length == node.old &&
            runs.last().m_current + runs.last().m_length == position) {
          runs.last().m_length += node.length;
        } else {
          runs.append({node.old, position, node.length});
        }
      }
      position += node.length;
      collect(node.right, position, runs);
    }
  } pieces;
  pieces.nodes.reserve(3 * m_mutations.size() + 2);
  int root = pieces.make(0, m_listBaseline.m_characters);
  for (const auto &change : m_mutations) {
    if (change.m_position < 0 || change.m_position > pieces.size(root) || change.m_removed < 0 ||
        change.m_removed > pieces.size(root) - change.m_position) {
      return false;
    }
    const auto before = pieces.split(root, change.m_position);
    const auto after = pieces.split(before.second, change.m_removed);
    root = pieces.merge(pieces.merge(before.first, pieces.make(-1, change.m_added)), after.second);
  }
  p_runs.reserve(m_mutations.size() + 1);
  int position = 0;
  pieces.collect(root, position, p_runs);
  return position == m_doc->characterCount() - 1;
}

bool MarkdownSourceFormatter::sameMarker(const Marker &p_left, const Marker &p_right) {
  return p_left.m_valid && p_right.m_valid && p_left.m_marker == p_right.m_marker &&
         p_left.m_spelling == p_right.m_spelling && p_left.m_prefix == p_right.m_prefix;
}

QHash<int, int> MarkdownSourceFormatter::changedLists(const md::ListStructure &p_structure,
                                                      QVector<SourceRun> &p_survivingSource) const {
  const auto current = currentMarkers(p_structure);
  const auto &old = m_listBaseline.m_markers;
  auto &runs = p_survivingSource;
  if (!survivingSource(runs)) {
    runs.clear();
    return {};
  }
  QVector<int> currentToOld(current.size(), -1);
  QVector<int> oldToCurrent(old.size(), -1);
  QVector<bool> broadRecovery(old.size(), false);
  QHash<quint64, int> liveMarkers;
  liveMarkers.reserve(current.size());
  const auto blockKey = [](int p_position, int p_ordinal) {
    return (quint64(quint32(p_position)) << 32) | quint32(p_ordinal);
  };
  for (int i = 0; i < current.size(); ++i) {
    if (current[i].m_valid) {
      liveMarkers.insert(blockKey(current[i].m_blockStart, current[i].m_ordinal), i);
    }
  }
  int run = 0;
  int next = 0;
  for (int i = 0; i < old.size(); ++i) {
    const auto &marker = old[i];
    if (!marker.m_valid) {
      continue;
    }
    while (run < runs.size() && runs[run].m_old + runs[run].m_length <= marker.m_start) {
      ++run;
    }
    if (run < runs.size() && runs[run].m_old < marker.m_end) {
      const int surviving = qMax(marker.m_start, runs[run].m_old);
      const int position = runs[run].m_current + surviving - runs[run].m_old;
      while (next < current.size() && (!current[next].m_valid || current[next].m_end <= position)) {
        ++next;
      }
      if (next < current.size() && current[next].m_start <= position && currentToOld[next] < 0) {
        currentToOld[next] = i;
        oldToCurrent[i] = next;
      }
    } else {
      // A real deletion can leave its first QTextBlock on the following item.
      // Never use that boundary handle as an identity. A *whole interior*
      // block surviving a removed span, however, proves Qt coalesced several
      // edits into one broad notification rather than deleting this block.
      const int gapStart = run ? runs[run - 1].m_old + runs[run - 1].m_length : 0;
      const int gapEnd = run < runs.size() ? runs[run].m_old : m_listBaseline.m_characters;
      broadRecovery[i] = marker.m_block.isValid() && marker.m_blockStart > gapStart &&
                         marker.m_blockStart + marker.m_blockLength - 1 <= gapEnd;
    }
  }
  for (int i = 0; i < old.size(); ++i) {
    if (!broadRecovery[i] || oldToCurrent[i] >= 0) {
      continue;
    }
    const int candidate =
        liveMarkers.value(blockKey(old[i].m_block.position(), old[i].m_ordinal), -1);
    if (candidate >= 0 && currentToOld[candidate] < 0 && sameMarker(old[i], current[candidate])) {
      oldToCurrent[i] = candidate;
      currentToOld[candidate] = i;
    }
  }
  // Reconcile displaced boundary handles from direct surviving neighbours.
  // Do not search equal text elsewhere: an identical newly pasted LIST with
  // no surviving anchor remains new. Each marker is considered at most twice.
  for (const auto &list : p_structure.m_lists) {
    int previous = -1;
    for (int item : list.m_items) {
      if (currentToOld[item] >= 0) {
        previous = currentToOld[item];
        continue;
      }
      if (previous < 0 || old[previous].m_list < 0) {
        continue;
      }
      const auto &siblings = m_listBaseline.m_lists[old[previous].m_list].m_items;
      const int ordinal = old[previous].m_directOrdinal + 1;
      if (ordinal < siblings.size()) {
        const int candidate = siblings[ordinal];
        if (oldToCurrent[candidate] < 0 && sameMarker(old[candidate], current[item])) {
          oldToCurrent[candidate] = item;
          currentToOld[item] = candidate;
          previous = candidate;
          continue;
        }
      }
      previous = -1;
    }
    int following = -1;
    for (int ordinal = list.m_items.size() - 1; ordinal >= 0; --ordinal) {
      const int item = list.m_items[ordinal];
      if (currentToOld[item] >= 0) {
        following = currentToOld[item];
        continue;
      }
      if (following < 0 || old[following].m_list < 0) {
        continue;
      }
      const auto &siblings = m_listBaseline.m_lists[old[following].m_list].m_items;
      const int previousOrdinal = old[following].m_directOrdinal - 1;
      if (previousOrdinal >= 0) {
        const int candidate = siblings[previousOrdinal];
        if (oldToCurrent[candidate] < 0 && sameMarker(old[candidate], current[item])) {
          oldToCurrent[candidate] = item;
          currentToOld[item] = candidate;
          following = candidate;
          continue;
        }
      }
      following = -1;
    }
  }

  const auto &containers = p_structure.m_containers;
  const auto &oldContainers = m_listBaseline.m_containers;
  QVector<int> containerToOld(containers.size(), -1);
  QVector<int> oldToContainer(oldContainers.size(), -1);
  for (int i = 0; i < current.size(); ++i) {
    if (currentToOld[i] < 0) {
      continue;
    }
    int now = current[i].m_container;
    int before = old[currentToOld[i]].m_container;
    while (now >= 0 && before >= 0) {
      if (containerToOld[now] == before && oldToContainer[before] == now) {
        break;
      }
      containerToOld[now] = containerToOld[now] == -1 ? before : -2;
      oldToContainer[before] = oldToContainer[before] == -1 ? now : -2;
      now = containers[now].m_parent;
      before = oldContainers[before].m_parent;
    }
  }
  QVector<int> sameContainer(containers.size(), -1);
  const auto sameAncestry = [&](auto &&self, int p_current, int p_old) -> bool {
    if (p_current < 0 || p_old < 0) {
      return p_current == p_old;
    }
    if (containerToOld[p_current] != p_old || oldToContainer[p_old] != p_current) {
      return false;
    }
    if (sameContainer[p_current] >= 0) {
      return sameContainer[p_current] != 0;
    }
    const auto &now = containers[p_current];
    const auto &before = oldContainers[p_old];
    const bool same = now.m_kind == before.m_kind && now.m_markerOffset == before.m_markerOffset &&
                      now.m_padding == before.m_padding &&
                      (now.m_kind != md::ListContainerInfo::Kind::Item ||
                       (now.m_item >= 0 && currentToOld[now.m_item] == before.m_item)) &&
                      self(self, now.m_parent, before.m_parent);
    sameContainer[p_current] = int(same);
    return same;
  };

  // One old list can split. Its earliest surviving direct item, not whichever
  // fragment happens to be visited first, decides which fragment keeps start.
  QVector<int> inheritor(m_listBaseline.m_lists.size(), -1);
  for (int i = 0; i < old.size(); ++i) {
    if (oldToCurrent[i] >= 0 && old[i].m_list >= 0 && inheritor[old[i].m_list] < 0) {
      inheritor[old[i].m_list] = current[oldToCurrent[i]].m_list;
    }
  }
  QHash<int, int> starts;
  for (int l = 0; l < p_structure.m_lists.size(); ++l) {
    const auto &list = p_structure.m_lists[l];
    if (!list.m_ordered || list.m_items.isEmpty()) {
      continue;
    }
    int contributor = -1;
    for (int item : list.m_items) {
      if (currentToOld[item] >= 0) {
        contributor = old[currentToOld[item]].m_list;
        break;
      }
    }
    bool changed = contributor < 0;
    if (!changed) {
      const auto &before = m_listBaseline.m_lists[contributor];
      changed = before.m_ordered != list.m_ordered || before.m_marker != list.m_marker ||
                before.m_items.size() != list.m_items.size() ||
                !sameAncestry(sameAncestry, list.m_parentContainer, before.m_parentContainer);
      if (!changed) {
        for (int ordinal = 0; ordinal < list.m_items.size(); ++ordinal) {
          const int item = list.m_items[ordinal];
          const int prior = before.m_items[ordinal];
          if (currentToOld[item] != prior || !sameMarker(old[prior], current[item]) ||
              !sameAncestry(sameAncestry, current[item].m_container, old[prior].m_container)) {
            changed = true;
            break;
          }
        }
      }
    }
    if (!changed) {
      continue;
    }
    const int first = list.m_items.first();
    const int oldFirst = currentToOld[first];
    int start = current[first].m_number;
    if (oldFirst >= 0 && contributor >= 0 && current[first].m_number == old[oldFirst].m_number) {
      start = inheritor[contributor] == l ? m_listBaseline.m_lists[contributor].m_startNumber : 1;
    }
    // A new earlier marker or an explicitly renumbered first survivor wins
    // over inheritance. Otherwise a newly split later fragment restarts at one.
    starts.insert(l, start);
  }
  return starts;
}

bool MarkdownSourceFormatter::needsNumbering(const md::ListStructure &p_structure,
                                             const QHash<int, int> &p_starts) {
  for (auto it = p_starts.cbegin(); it != p_starts.cend(); ++it) {
    const auto &list = p_structure.m_lists[it.key()];
    const int start = it.value();
    if (start < 0 || start > 999999999 || list.m_items.size() - 1 > 999999999 - start) {
      // Let the builder reject the connected unit; other lists may be valid.
      return true;
    }
    for (int ordinal = 0; ordinal < list.m_items.size(); ++ordinal) {
      const auto &item = p_structure.m_items[list.m_items[ordinal]];
      int width = 1;
      const int target = start + ordinal;
      for (int number = target; number >= 10; number /= 10) {
        ++width;
      }
      if (!item.m_sourceValid || item.m_sourceNumber != target ||
          item.m_markerEnd - item.m_markerStart - 1 != width) {
        return true;
      }
    }
  }
  return false;
}

bool MarkdownSourceFormatter::hasListMarkersInOpenFence(const MarkdownHighlighterResult &p_result,
                                                        const QVector<SourceRun> &p_runs) const {
  auto lastBlock = m_doc->lastBlock();
  if (lastBlock.length() == 1 && lastBlock.previous().isValid()) {
    lastBlock = lastBlock.previous();
  }
  if (p_result.m_codeBlocksState.value(lastBlock.blockNumber(), md::Normal) != md::CodeBlock) {
    return false;
  }
  const int lastClosedBlock =
      p_result.m_codeBlocks.isEmpty() ? -1 : p_result.m_codeBlocks.constLast().m_endBlock;
  int run = 0;
  for (const auto &marker : m_listBaseline.m_markers) {
    if (!marker.m_valid || marker.m_list < 0 ||
        m_listBaseline.m_lists[marker.m_list].m_ordered == false) {
      continue;
    }
    while (run < p_runs.size() && p_runs[run].m_old + p_runs[run].m_length <= marker.m_start) {
      ++run;
    }
    // Only unchanged source carries old identity. A deleted/retyped marker is
    // newly authored even if Qt reuses its block or its spelling is identical.
    if (run == p_runs.size() || p_runs[run].m_old > marker.m_start ||
        p_runs[run].m_old + p_runs[run].m_length < marker.m_end) {
      continue;
    }
    const int position = p_runs[run].m_current + marker.m_start - p_runs[run].m_old;
    const auto block = m_doc->findBlock(position);
    if (block.blockNumber() > lastClosedBlock &&
        p_result.m_codeBlocksState.value(block.blockNumber(), md::Normal) == md::CodeBlock) {
      return true;
    }
  }
  return false;
}

void MarkdownSourceFormatter::prepareLists(const MarkdownHighlighterResult &p_result) {
  if (!m_listsEnabled || !m_listsPending || m_seedRequired || m_workerActive || m_numberingReady ||
      !p_result.m_listStructure.m_valid) {
    return;
  }
  if (!m_listBaseline.m_valid) {
    // An invalid seed never authorizes guessed old membership. Establish a
    // sound baseline without rewriting; later structural edits may proceed.
    bindListBaseline(p_result.m_listStructure);
    m_listsPending = false;
    return;
  }
  // An opening fence can temporarily hide the later items of an existing
  // list. Keep their lineage until the fence closes instead of treating those
  // surviving markers as newly authored when they reappear in the AST.
  QVector<SourceRun> runs;
  auto starts = changedLists(p_result.m_listStructure, runs);
  m_preserveListBaseline = hasListMarkersInOpenFence(p_result, runs);
  const auto &structure = p_result.m_listStructure;
  QVector<int> roots(structure.m_lists.size(), -1);
  const auto rootOf = [&](auto &&self, int p_list) -> int {
    if (roots[p_list] >= 0) {
      return roots[p_list];
    }
    int parent = structure.m_lists[p_list].m_parentContainer;
    while (parent >= 0) {
      const auto &container = structure.m_containers[parent];
      if (container.m_kind == md::ListContainerInfo::Kind::Item && container.m_item >= 0) {
        return roots[p_list] = self(self, structure.m_items[container.m_item].m_list);
      }
      parent = container.m_parent;
    }
    return roots[p_list] = p_list;
  };
  QSet<int> rejected;
  for (auto it = starts.cbegin(); it != starts.cend(); ++it) {
    const auto &list = structure.m_lists[it.key()];
    bool valid = it.value() >= 0 && it.value() <= 999999999 &&
                 list.m_items.size() - 1 <= 999999999 - it.value();
    int previousEnd = -1;
    for (int index : list.m_items) {
      const auto &item = structure.m_items[index];
      valid = valid && item.m_sourceValid && item.m_prefixValid &&
              item.m_markerStart >= previousEnd && item.m_markerEnd > item.m_markerStart &&
              item.m_marker == list.m_marker;
      previousEnd = item.m_markerEnd;
    }
    if (!valid) {
      rejected.insert(rootOf(rootOf, it.key()));
    }
  }
  for (auto it = starts.begin(); it != starts.end();) {
    if (rejected.contains(rootOf(rootOf, it.key()))) {
      it = starts.erase(it);
    } else {
      ++it;
    }
  }
  if (!needsNumbering(p_result.m_listStructure, starts)) {
    if (!m_preserveListBaseline) {
      bindListBaseline(p_result.m_listStructure);
    }
    m_listsPending = false;
    return;
  }
  m_jobWork = ListSourceWorker::Work::Numbering;
  m_jobEpoch = m_epoch;
  m_jobGeneration = m_generation;
  m_jobTimeStamp = p_result.m_timeStamp;
  m_worker->prepare(m_jobWork, m_jobEpoch, m_jobGeneration, p_result.m_listStructure, {}, starts);
  m_workerActive = true;
  m_worker->start();
}

bool MarkdownSourceFormatter::prepareTable(const md::TableElement &p_table,
                                           QVector<Row> &p_rows) const {
  if (p_table.m_syntax != md::TableElement::Syntax::Markdown || p_table.m_columns <= 0 ||
      p_table.m_rows.size() < 2 || p_table.m_alignments.size() != p_table.m_columns) {
    return false;
  }
  const auto first = m_doc->findBlockByNumber(p_table.m_startBlock);
  if (!first.isValid() || first.position() != p_table.m_startPos) {
    return false;
  }
  const auto lines =
      previewSourceText(m_doc, p_table.m_startPos, p_table.m_endPos).split(QLatin1Char('\n'));
  if (lines.size() != p_table.m_rows.size()) {
    return false;
  }
  QVector<QVector<QString>> cells;
  QVector<QString> prefixes;
  QString delimiterPrefix;
  auto block = first;
  for (int r = 0; r < lines.size(); ++r, block = block.next()) {
    const auto &parsed = p_table.m_rows[r];
    Row row;
    row.m_position = block.position();
    row.m_before = lines[r];
    row.m_delimiter = r == 1;
    QString prefix;
    if (!block.isValid() || block.text() != row.m_before ||
        !md::splitTableRow(row.m_before, prefix, row.m_cellsBefore, &row.m_offsetsBefore,
                           &row.m_bordersBefore) ||
        prefix != parsed.m_prefix || row.m_cellsBefore != parsed.m_cells ||
        row.m_offsetsBefore != parsed.m_cellOffsets ||
        row.m_cellsBefore.size() > p_table.m_columns ||
        (r < 2 && row.m_cellsBefore.size() != p_table.m_columns)) {
      return false;
    }
    if (row.m_delimiter) {
      if (parsed.m_type != md::TableRowType::Delimiter) {
        return false;
      }
      delimiterPrefix = prefix;
    } else {
      if (parsed.m_type != (r == 0 ? md::TableRowType::Header : md::TableRowType::Data)) {
        return false;
      }
      for (const auto &cell : row.m_cellsBefore) {
        if (TablePreviewSerializer::escapeCell(cell) != cell) {
          return false;
        }
      }
      cells.append(row.m_cellsBefore);
      prefixes.append(prefix);
    }
    p_rows.append(std::move(row));
  }
  QVector<PreviewTableAlignment> alignments;
  alignments.reserve(p_table.m_columns);
  for (int alignment : p_table.m_alignments) {
    alignments.append(toPreviewAlignment(alignment));
  }
  // This also rejects unsafe prefixes and line separators. The raw-source
  // caller above rejects escape changes rather than authoring new Markdown.
  const QString output =
      TablePreviewSerializer::serialize(cells, alignments, prefixes, delimiterPrefix, true);
  const auto formatted = output.split(QLatin1Char('\n'));
  if (output.isEmpty() || formatted.size() != p_rows.size()) {
    return false;
  }
  for (int r = 0; r < p_rows.size(); ++r) {
    auto &row = p_rows[r];
    row.m_after = formatted[r];
    QString prefix;
    if (!md::splitTableRow(row.m_after, prefix, row.m_cellsAfter, &row.m_offsetsAfter,
                           &row.m_bordersAfter) ||
        prefix != p_table.m_rows[r].m_prefix || row.m_cellsAfter.size() != p_table.m_columns) {
      return false;
    }
    if (!row.m_delimiter) {
      auto normalized = row.m_cellsBefore;
      normalized.resize(p_table.m_columns);
      if (normalized != row.m_cellsAfter) {
        return false;
      }
    }
  }
  return true;
}

// Positions are UTF-16 boundaries, not display columns. A boundary belonging
// to retained cell text wins over the pipe immediately following that text.
bool MarkdownSourceFormatter::Row::map(int p_position, int &p_mapped) const {
  if (m_before == m_after || p_position < m_bordersBefore[0]) {
    p_mapped = p_position;
    return true;
  }
  for (int c = 0; c < m_cellsBefore.size(); ++c) {
    const auto &before = m_cellsBefore[c];
    if (before.isEmpty()) {
      continue;
    }
    const int offset = p_position - m_offsetsBefore[c];
    if (offset < 0 || offset > before.size()) {
      continue;
    }
    int mapped = offset;
    if (m_delimiter) {
      const auto &after = m_cellsAfter[c];
      const bool left = before.startsWith(QLatin1Char(':'));
      const bool right = before.endsWith(QLatin1Char(':'));
      if (left != after.startsWith(QLatin1Char(':')) || right != after.endsWith(QLatin1Char(':'))) {
        return false;
      }
      if (left && offset == 0) {
        mapped = 0;
      } else if (right && offset >= before.size() - 1) {
        mapped = after.size() - (before.size() - offset);
      } else {
        const int dashOffset = offset - int(left);
        const int dashCount = after.size() - int(left) - int(right);
        if (dashOffset < 0 || dashOffset > dashCount) {
          return false;
        }
        mapped = int(left) + dashOffset;
      }
    }
    p_mapped = m_offsetsAfter[c] + mapped;
    return true;
  }
  for (int pipe = 0; pipe < m_bordersBefore.size(); ++pipe) {
    if (p_position == m_bordersBefore[pipe]) {
      // The old closing pipe still closes the same field, even when missing
      // body fields are appended after it.
      p_mapped = m_bordersAfter[pipe];
      return true;
    }
  }
  if (p_position > m_bordersBefore.last()) {
    p_mapped = m_bordersAfter.last() + p_position - m_bordersBefore.last();
    return p_mapped <= m_after.size();
  }
  for (int c = 0; c < m_cellsBefore.size(); ++c) {
    if (p_position <= m_bordersBefore[c] || p_position >= m_bordersBefore[c + 1]) {
      continue;
    }
    if (m_cellsBefore[c].isEmpty()) {
      p_mapped = m_bordersAfter[c] + p_position - m_bordersBefore[c];
      return p_mapped < m_bordersAfter[c + 1];
    }
    if (p_position < m_offsetsBefore[c]) {
      p_mapped = m_offsetsAfter[c] - (m_offsetsBefore[c] - p_position);
      return p_mapped > m_bordersAfter[c];
    }
    const int endBefore = m_offsetsBefore[c] + m_cellsBefore[c].size();
    const int endAfter = m_offsetsAfter[c] + m_cellsAfter[c].size();
    p_mapped = endAfter + p_position - endBefore;
    return p_mapped < m_bordersAfter[c + 1];
  }
  return false;
}

void MarkdownSourceFormatter::prepareTables(const QVector<md::TableElement> &p_tables,
                                            const Endpoints &p_endpoints, QVector<Row> &p_rows,
                                            QSet<int> &p_deferred) const {
  const auto positions = baselinePositions();
  for (const auto &table : p_tables) {
    if (table.m_syntax != md::TableElement::Syntax::Markdown) {
      continue;
    }
    bool changed = false;
    int baselineStart = -1;
    auto block = m_doc->findBlockByNumber(table.m_startBlock);
    for (int r = 0; r < table.m_rows.size() && block.isValid(); ++r, block = block.next()) {
      const int old = positions.value(block.position(), -1);
      if (old < 0 || m_baseline[old].m_block != block || m_baseline[old].m_text != block.text()) {
        changed = true;
      } else if (r > 0 && baselineStart < 0) {
        baselineStart = old - r;
      }
    }
    if (changed && baselineStart >= 0 && table.m_rows.size() <= m_baseline.size() - baselineStart) {
      // Qt can leave a split header's old handle on preceding prose. A broad
      // edit-block notification cannot locate that split. Instead, a surviving
      // non-header row anchors the original baseline; every row must still
      // match exactly. A newly inserted identical table has no such anchor.
      bool sameRun = true;
      block = m_doc->findBlockByNumber(table.m_startBlock);
      for (int r = 0; r < table.m_rows.size(); ++r, block = block.next()) {
        const int old = positions.value(block.position(), -1);
        if (!block.isValid() || m_baseline[baselineStart + r].m_text != block.text() ||
            (old >= 0 && old != baselineStart + r)) {
          sameRun = false;
          break;
        }
      }
      changed = !sameRun;
    }
    if (!changed) {
      continue;
    }
    QVector<Row> tableRows;
    if (!prepareTable(table, tableRows)) {
      continue;
    }
    bool representable = true;
    for (const auto &row : tableRows) {
      for (int endpoint : p_endpoints.m_positions) {
        if (endpoint >= row.m_position && endpoint <= row.m_position + row.m_before.size()) {
          int mapped = 0;
          if (!row.map(endpoint - row.m_position, mapped)) {
            representable = false;
          }
        }
      }
    }
    if (!representable) {
      for (int r = 0; r < table.m_rows.size(); ++r) {
        p_deferred.insert(table.m_startBlock + r);
      }
      continue;
    }
    for (auto &row : tableRows) {
      if (row.m_before != row.m_after) {
        p_rows.append(std::move(row));
      }
    }
  }
}

MarkdownSourceFormatter::Endpoints MarkdownSourceFormatter::endpoints() const {
  const auto edit = m_editor->getTextEdit();
  const auto original = edit->textCursor();
  const auto selection = edit->getSelection();
  Endpoints result;
  result.m_overridden =
      selection.isValid() &&
      !(selection == VTextEdit::Selection(original.anchor(), original.position()));
  result.m_positions = {original.anchor(), original.position()};
  if (result.m_overridden) {
    result.m_positions.append(selection.start());
    result.m_positions.append(selection.end());
  }
  return result;
}

bool MarkdownSourceFormatter::apply(const QVector<Row> &p_rows,
                                    const QVector<md::ListSourceEdit> &p_listEdits,
                                    Endpoints p_endpoints, quint64 p_generation, int p_revision,
                                    bool p_tables, bool p_lists) {
  QVector<Replacement> replacements;
  replacements.reserve(p_rows.size() + p_listEdits.size());
  int row = 0;
  int list = 0;
  while (row < p_rows.size() || list < p_listEdits.size()) {
    if (list == p_listEdits.size() ||
        (row < p_rows.size() && p_rows[row].m_position < p_listEdits[list].m_start)) {
      const auto &item = p_rows[row];
      replacements.append({item.m_position, item.m_before, item.m_after, row});
      ++row;
    } else {
      const auto &item = p_listEdits[list++];
      if (item.m_end < item.m_start || item.m_end - item.m_start != item.m_before.size()) {
        return false;
      }
      replacements.append({item.m_start, item.m_before, item.m_after, -1});
    }
  }
  int previousEnd = -1;
  QTextCursor probe(m_doc);
  for (const auto &replacement : replacements) {
    const int end = replacement.m_position + replacement.m_before.size();
    if (replacement.m_position < previousEnd || replacement.m_position < 0 ||
        end > m_doc->characterCount() - 1) {
      return false;
    }
    probe.setPosition(replacement.m_position);
    probe.setPosition(end, QTextCursor::KeepAnchor);
    QString before = probe.selectedText();
    before.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
    if (before != replacement.m_before) {
      return false;
    }
    previousEnd = end;
  }
  // Every controller-owned endpoint is mapped against the immutable batch,
  // including endpoints set by external programmatic QTextCursor edits.
  for (auto &endpoint : p_endpoints.m_positions) {
    int delta = 0;
    for (const auto &replacement : replacements) {
      if (endpoint < replacement.m_position) {
        break;
      }
      const int end = replacement.m_position + replacement.m_before.size();
      if (endpoint < end || (replacement.m_row >= 0 && endpoint == end)) {
        int mapped = qBound(0, endpoint - replacement.m_position, int(replacement.m_after.size()));
        if (replacement.m_row >= 0 &&
            !p_rows[replacement.m_row].map(endpoint - replacement.m_position, mapped)) {
          return false;
        }
        endpoint = replacement.m_position + mapped;
        break;
      }
      delta += replacement.m_after.size() - replacement.m_before.size();
    }
    endpoint += delta;
  }
  if ((p_tables && !m_enabled) || (p_lists && !m_listsEnabled) || p_generation != m_generation ||
      p_revision != m_doc->revision() || !guarded()) {
    return false;
  }
  if (replacements.isEmpty()) {
    return true;
  }
  auto edit = m_editor->getTextEdit();
  const int horizontal = edit->horizontalScrollBar()->value();
  const int vertical = edit->verticalScrollBar()->value();
  {
    QScopedValueRollback<bool> applying(m_applying, true);
    QTextCursor cursor(m_doc);
    // Qt can reopen an edit block but cannot promote a bare insert into one.
    if (m_doc->isUndoRedoEnabled() && m_undoSteps > 0 &&
        m_undoSteps == m_doc->availableUndoSteps() && m_doc->availableRedoSteps() == 0) {
      cursor.joinPreviousEditBlock();
    } else {
      cursor.beginEditBlock();
    }
    for (int r = replacements.size() - 1; r >= 0; --r) {
      const auto &replacement = replacements[r];
      int prefix = 0;
      const int beforeSize = replacement.m_before.size();
      const int afterSize = replacement.m_after.size();
      while (prefix < beforeSize && prefix < afterSize &&
             replacement.m_before[prefix] == replacement.m_after[prefix]) {
        ++prefix;
      }
      int suffix = 0;
      while (suffix < beforeSize - prefix && suffix < afterSize - prefix &&
             replacement.m_before[beforeSize - suffix - 1] ==
                 replacement.m_after[afterSize - suffix - 1]) {
        ++suffix;
      }
      // Minimal edits also let Qt rebase independently-owned external cursors
      // without replacing their unchanged body text. Qt has no public API for
      // enumerating/reassigning another caller's QTextCursor endpoints.
      cursor.setPosition(replacement.m_position + prefix);
      cursor.setPosition(replacement.m_position + beforeSize - suffix, QTextCursor::KeepAnchor);
      if (m_listsEnabled && (!p_lists || m_preserveListBaseline)) {
        // Retained split lineage must follow our own edits too, including an
        // unrelated list normalized while a fence still hides its old sibling.
        m_mutations.append({replacement.m_position + prefix, beforeSize - prefix - suffix,
                            afterSize - prefix - suffix});
      }
      cursor.insertText(replacement.m_after.mid(prefix, afterSize - prefix - suffix));
    }
    cursor.endEditBlock();
    QTextCursor restored(m_doc);
    restored.setPosition(p_endpoints.m_positions[0]);
    restored.setPosition(p_endpoints.m_positions[1], QTextCursor::KeepAnchor);
    edit->setTextCursor(restored);
    if (p_endpoints.m_overridden) {
      edit->setOverriddenSelection(p_endpoints.m_positions[2], p_endpoints.m_positions[3]);
    }
    edit->horizontalScrollBar()->setValue(horizontal);
    edit->verticalScrollBar()->setValue(vertical);
  }
  ++m_generation;
  observeDocument();
  return true;
}

void MarkdownSourceFormatter::attempt() {
  if (m_applying || !hasPending()) {
    return;
  }
  startSeed();
  if (m_numberingReady && (m_jobEpoch != m_epoch || m_jobGeneration != m_generation)) {
    m_numberingReady = false;
    releaseWorkerResult();
  }
  if (!m_pending && !m_listsPending && !m_numberingReady) {
    return;
  }
  if (m_editor->isReadOnly()) {
    // Preserve numbering's structural baseline until editing resumes. Table
    // mode retains its existing read-only cancellation contract independently.
    if (m_pending) {
      m_pending = false;
      captureBaseline();
    }
    return;
  }
  if (m_idle.isValid() && m_idle.elapsed() < 500) {
    m_timer.start(500 - int(m_idle.elapsed()));
    return;
  }
  if (!guarded()) {
    return;
  }
  const auto result = freshResult();
  if (!result) {
    return;
  }
  if (m_numberingReady && result->m_timeStamp != m_jobTimeStamp) {
    m_numberingReady = false;
    releaseWorkerResult();
  }
  prepareLists(*result);
  if (m_listsEnabled && m_workerActive && m_jobEpoch == m_epoch &&
      m_jobWork == ListSourceWorker::Work::Numbering) {
    // Hold the table plan, not a stale copy of its rows, until the current
    // numbering job completes. Then disjoint work shares one transaction.
    return;
  }
  const auto generation = m_generation;
  const int revision = m_doc->revision();
  const auto positions = endpoints();
  QVector<Row> rows;
  QSet<int> deferred;
  const bool tables = m_enabled && m_pending;
  if (tables) {
    prepareTables(result->m_tableElements, positions, rows, deferred);
  }
  const bool lists = m_listsEnabled && m_numberingReady;
  const QVector<md::ListSourceEdit> empty;
  const auto &listEdits = lists ? m_worker->edits() : empty;
  bool overlap = false;
  int listIndex = 0;
  for (const auto &row : rows) {
    while (listIndex < listEdits.size() &&
           (listEdits[listIndex].m_end < row.m_position ||
            (listEdits[listIndex].m_end == row.m_position &&
             listEdits[listIndex].m_start < listEdits[listIndex].m_end))) {
      ++listIndex;
    }
    if (listIndex < listEdits.size() &&
        listEdits[listIndex].m_start < row.m_position + row.m_before.size()) {
      overlap = true;
      break;
    }
  }
  if (overlap) {
    // Keep every table's old dirty baseline. The candidate indentation changes
    // prefixes; only a later full parse may supply new table row positions.
    rows.clear();
  }
  if (!apply(rows, listEdits, positions, generation, revision, tables && !overlap, lists)) {
    return;
  }
  if (lists) {
    QVector<Replacement> tableChanges;
    tableChanges.reserve(rows.size());
    int delta = 0;
    listIndex = 0;
    for (const auto &row : rows) {
      while (listIndex < listEdits.size() && listEdits[listIndex].m_end <= row.m_position) {
        delta += listEdits[listIndex].m_after.size() - listEdits[listIndex].m_before.size();
        ++listIndex;
      }
      tableChanges.append({row.m_position + delta, row.m_before, row.m_after, -1});
    }
    // The verified candidate is already parsed. Disjoint table rewrites only
    // shift positions; no third parse is necessary to bind its live markers.
    if (!m_preserveListBaseline) {
      bindListBaseline(m_worker->structure(), nullptr, tableChanges);
    }
    m_listsPending = false;
    m_numberingReady = false;
    releaseWorkerResult();
  }
  if (tables && !overlap) {
    m_pending = !deferred.isEmpty();
    captureBaseline(deferred);
  }
  observeDocument();
  if (!hasPending()) {
    m_timer.stop();
  } else if (overlap) {
    // This queued attempt will wait for a fresh timestamp; it does not retry
    // on a timer or submit another numbering job for our own source edits.
    queueAttempt();
  }
}
bool MarkdownSourceFormatter::takeListReturnSuppression() {
  const bool suppressed = m_listReturnSuppressed;
  m_listReturnSuppressed = false;
  return suppressed;
}

bool MarkdownSourceFormatter::resolveListInsertion(QTextCursor p_origin,
                                                   VTextEdit::BlockInsertion p_operation,
                                                   ListInsertionPlan &p_plan) const {
  const bool split = p_operation == VTextEdit::BlockInsertion::Split;
  if (split && p_origin.hasSelection()) {
    return false;
  }
  if (!split) {
    // Open-line commands classify the original block, not the caret's column
    // or (for Above) the preceding block. This cursor is never installed live.
    p_origin.movePosition(QTextCursor::EndOfBlock);
  }
  const auto block = p_origin.block();
  const auto highlighter = m_editor->getHighlighter();
  const auto context = highlighter->getBlockContext(block.blockNumber());
  if (context.m_valid && context.m_inFencedCode) {
    p_plan.m_suppressFallback = true;
    return false;
  }
  const auto &result = highlighter->m_result;
  const bool fresh =
      result && result->matched(highlighter->m_timeStamp) && result->m_listStructure.m_valid;
  const QString line = block.text();
  md::ListItemInfo marker;
  QString prefix;
  int number = 0;
  bool empty = false;
  const int increment = p_operation == VTextEdit::BlockInsertion::Above ? 0 : 1;
  if (fresh) {
    // A negative full-AST query vetoes the post-hook's lexical fallback too.
    p_plan.m_suppressFallback = true;
    const auto &structure = result->m_listStructure;
    const int blockNumber = block.blockNumber();
    int itemIndex = -1;
    const auto paragraph =
        std::upper_bound(structure.m_paragraphs.cbegin(), structure.m_paragraphs.cend(),
                         blockNumber, [](int p_block, const md::ListParagraphInfo &p_paragraph) {
                           return p_block < p_paragraph.m_startBlock;
                         });
    if (paragraph != structure.m_paragraphs.cbegin()) {
      const auto &previous = *(paragraph - 1);
      if (blockNumber <= previous.m_endBlock) {
        itemIndex = previous.m_item;
      }
    }
    auto opening = std::upper_bound(
        structure.m_items.cbegin(), structure.m_items.cend(), blockNumber,
        [](int p_block, const md::ListItemInfo &p_item) { return p_block < p_item.m_startBlock; });
    // Multiple opening markers on one line are ordered outermost first.
    while (opening != structure.m_items.cbegin() && (opening - 1)->m_startBlock == blockNumber &&
           (opening - 1)->m_markerStart > p_origin.position()) {
      --opening;
    }
    if (opening != structure.m_items.cbegin()) {
      const int candidate = int(opening - structure.m_items.cbegin()) - 1;
      const auto &item = structure.m_items[candidate];
      if (item.m_startBlock == blockNumber) {
        if (!item.m_sourceValid || item.m_contentStart < block.position()) {
          return false;
        }
        // A marker-only parent can have children, but it is not an empty ITEM.
        // Nonparagraph opening content (code/HTML/heading/quote/table/math) is
        // not a list-body Return, even though its marker shares this line.
        if (candidate != itemIndex &&
            !line.mid(item.m_contentStart - block.position()).trimmed().isEmpty()) {
          return false;
        }
        itemIndex = candidate;
      }
    }
    if (itemIndex < 0 || itemIndex >= structure.m_items.size()) {
      return false;
    }
    const auto &item = structure.m_items[itemIndex];
    const int boundary = item.m_startBlock == blockNumber &&
                                 line.mid(item.m_markerEnd - block.position()).trimmed().isEmpty()
                             ? item.m_markerEnd
                             : item.m_contentStart;
    if (!item.m_sourceValid || p_origin.position() < boundary ||
        !md::listContinuationPrefix(structure, itemIndex, prefix)) {
      return false;
    }
    const auto firstBlock = m_doc->findBlockByNumber(item.m_startBlock);
    if (!firstBlock.isValid()) {
      return false;
    }
    const QString firstLine = item.m_startBlock == blockNumber ? line : firstBlock.text();
    if (!md::scanListMarker(firstLine, item.m_markerStart - firstBlock.position(), marker) ||
        marker.m_marker != item.m_marker || marker.m_sourceNumber != item.m_sourceNumber ||
        marker.m_markerEnd - marker.m_markerStart != item.m_markerEnd - item.m_markerStart) {
      return false;
    }
    empty = blockNumber == item.m_startBlock && marker.m_empty && item.m_empty;
    number = item.m_sourceNumber;
    const auto &list = structure.m_lists[item.m_list];
    if (m_listsEnabled && list.m_ordered) {
      const auto ordinal = std::lower_bound(list.m_items.cbegin(), list.m_items.cend(), itemIndex);
      const int nextOrdinal = int(ordinal - list.m_items.cbegin()) + increment;
      if (ordinal == list.m_items.cend() || *ordinal != itemIndex ||
          nextOrdinal > 999999999 - list.m_startNumber) {
        return false;
      }
      number = list.m_startNumber + nextOrdinal - increment;
    }
  } else {
    // The cold/stale path examines only the authored current line. In
    // particular it cannot infer markerless membership from an older tree.
    QString indent, quote, rest;
    int depth = 0;
    int start = 0;
    if (MarkdownUtils::isQuote(line, indent, quote, rest, depth)) {
      start = int(line.size() - rest.size()) + TextUtils::fetchIndentation(rest);
    } else {
      start = TextUtils::fetchIndentation(line);
    }
    if (!md::scanListMarker(line, start, marker) ||
        p_origin.positionInBlock() <
            (marker.m_empty ? marker.m_markerEnd : marker.m_contentStart)) {
      return false;
    }
    prefix = line.left(marker.m_markerStart);
    number = marker.m_sourceNumber;
    empty = marker.m_empty;
  }
  if (split && empty) {
    p_plan.m_removeStart = block.position() + marker.m_markerStart;
    p_plan.m_removeEnd = block.position() + line.size();
    return true;
  }
  const bool ordered = marker.m_marker == QLatin1Char('.') || marker.m_marker == QLatin1Char(')');
  if (ordered && number > 999999999 - increment) {
    p_plan.m_suppressFallback = true;
    return false;
  }
  if (ordered) {
    prefix += QString::number(number + increment);
  }
  prefix += marker.m_marker;
  prefix += QLatin1Char(' ');
  if (marker.m_task) {
    prefix += QStringLiteral("[ ] ");
  }
  p_plan.m_prefix = std::move(prefix);
  return true;
}

bool MarkdownSourceFormatter::handleListInsertion(VTextEdit::BlockInsertion p_operation) {
  auto edit = m_editor->getTextEdit();
  auto cursor = edit->textCursor();
  ListInsertionPlan plan;
  const bool resolved = resolveListInsertion(cursor, p_operation, plan);
  if (p_operation == VTextEdit::BlockInsertion::Split) {
    m_listReturnSuppressed = plan.m_suppressFallback;
  }
  if (!resolved) {
    return false;
  }

  cursor.beginEditBlock();
  if (plan.m_removeStart >= 0) {
    cursor.setPosition(plan.m_removeStart);
    cursor.setPosition(plan.m_removeEnd, QTextCursor::KeepAnchor);
    cursor.removeSelectedText();
  } else {
    if (p_operation == VTextEdit::BlockInsertion::Above) {
      cursor.movePosition(QTextCursor::StartOfBlock);
      const int position = cursor.position();
      cursor.insertBlock();
      // insertBlock leaves the cursor in the original block; the new block is
      // before it, even at document position zero.
      cursor.setPosition(position);
    } else {
      if (p_operation == VTextEdit::BlockInsertion::Below) {
        cursor.movePosition(QTextCursor::EndOfBlock);
      }
      cursor.insertBlock();
    }
    cursor.insertText(plan.m_prefix);
  }
  cursor.endEditBlock();
  edit->setTextCursor(cursor);
  return true;
}

} // namespace vte

VMarkdownEditor::VMarkdownEditor(const QSharedPointer<MarkdownEditorConfig> &p_config,
                                 const QSharedPointer<TextEditorParameters> &p_paras,
                                 QWidget *p_parent)
    : VTextEditor(p_config->m_textEditorConfig, p_paras, p_parent), m_config(p_config) {
  setupDocumentLayout();

  setupSyntaxHighlighter();

  setupPreviewMgr();

  // Setup folding provider.
  m_foldingProvider.reset(new MarkdownFoldingProvider(getTextFolding(), document()));
  connect(getHighlighter(), &MarkdownHighlighter::foldingRegionsUpdated, this,
          [this](const QVector<md::FoldingRegion> &p_regions) {
            m_foldingProvider->updateFoldingRegions(p_regions);
            // Never evaluate the fold state synchronously here:
            // MarkdownHighlighter::completeHighlight() emits
            // foldingRegionsUpdated *before* previewElementsUpdated, so at this
            // instant the host still describes the previous generation.
            if (auto host = interactivePreviewHost()) {
              host->scheduleFoldRefresh();
            }
          });

  // Reset provider state when TextFolding is externally cleared
  // (e.g., during document replacement detected by hardClear).
  connect(getTextFolding(), &TextFolding::foldingRangesChanged, this, [this]() {
    if (getTextFolding()->isEmpty()) {
      m_foldingProvider->resetState();
    }

    // A manual fold or unfold from the gutter has to be written back
    // onto the preview item which owns that region.
    if (auto host = interactivePreviewHost()) {
      host->scheduleFoldRefresh();
    }
  });

  m_textEdit->viewport()->installEventFilter(this);

  // Hook keys.
  connect(m_textEdit, &VTextEdit::preKeyReturn, this, &VMarkdownEditor::preKeyReturn);
  connect(m_textEdit, &VTextEdit::postKeyReturn, this, &VMarkdownEditor::postKeyReturn);
  connect(m_textEdit, &VTextEdit::preKeyTab, this, &VMarkdownEditor::preKeyTab);
  connect(m_textEdit, &VTextEdit::preKeyBacktab, this, &VMarkdownEditor::preKeyBacktab);

  new MarkdownSourceFormatter(this);
  new HeadingSourceNumberer(this);
  updateFromConfig();

  // Trigger update of stuffs after init.
  m_textEdit->setText("");
}

VMarkdownEditor::~VMarkdownEditor() {
  delete findChild<HeadingSourceNumberer *>(QStringLiteral("vte_heading_source_numberer"),
                                            Qt::FindDirectChildrenOnly);
  delete findChild<MarkdownSourceFormatter *>(QStringLiteral("vte_markdown_source_formatter"),
                                              Qt::FindDirectChildrenOnly);
  // The host is an ordinary QObject child, and QObject destroys its children
  // in creation order - which puts m_textEdit, its viewport and every preview
  // widget parented to it *before* the host. Its destructor asks a dirty sheet
  // to write itself back before the identity is dropped, and that needs the
  // document, the anchors and the widgets to still exist. So destroy it here,
  // while they do.
  delete interactivePreviewHost();
}

void VMarkdownEditor::setSyntax(const QString &p_syntax) {
  // Just ignore it.
  Q_UNUSED(p_syntax);
}

QString VMarkdownEditor::getSyntax() const { return QStringLiteral("richmarkdown"); }

void VMarkdownEditor::setHeadingSectionNumberProvider(HeadingSectionNumberProvider p_provider) {
  if (auto numberer = findChild<HeadingSourceNumberer *>(
          QStringLiteral("vte_heading_source_numberer"), Qt::FindDirectChildrenOnly)) {
    numberer->setProvider(std::move(p_provider));
  }
}

void VMarkdownEditor::setHeadingSectionNumberingActive(bool p_active) {
  if (auto numberer = findChild<HeadingSourceNumberer *>(
          QStringLiteral("vte_heading_source_numberer"), Qt::FindDirectChildrenOnly)) {
    numberer->setActive(p_active);
  }
}

void VMarkdownEditor::setupSyntaxHighlighter() {
  m_highlighterInterface.reset(new EditorMarkdownHighlighter(this));
  CodeBlockHighlighter *codeBlockHighlighter = nullptr;
  if (m_config->m_webCodeBlockHighlighterEnabled) {
    m_webCodeBlockHighlighter = new WebCodeBlockHighlighter(this);
    connect(m_webCodeBlockHighlighter,
            &WebCodeBlockHighlighter::externalCodeBlockHighlightRequested, this,
            &VMarkdownEditor::externalCodeBlockHighlightRequested);

    codeBlockHighlighter = m_webCodeBlockHighlighter;
  } else {
    codeBlockHighlighter =
        new KSyntaxCodeBlockHighlighter(m_config->m_textEditorConfig->m_syntaxTheme, this);
  }
  auto highlighterConfig = QSharedPointer<md::HighlighterConfig>::create();
  highlighterConfig->m_mathExtEnabled = true;

  m_mathBlockHighlighter = new MathBlockHighlighter(this);
  connect(m_mathBlockHighlighter, &MathBlockHighlighter::externalMathHighlightRequested, this,
          &VMarkdownEditor::externalMathHighlightRequested);

  m_highlighter =
      new MarkdownHighlighter(m_highlighterInterface.data(), document(), theme(),
                              codeBlockHighlighter, highlighterConfig, m_mathBlockHighlighter);
  connect(getHighlighter(), &MarkdownHighlighter::listItemRangesUpdated, documentLayout(),
          &TextDocumentLayout::setListItemRanges);
  connect(getHighlighter(), &MarkdownHighlighter::concealRangesUpdated, this,
          [this](TimeStamp p_timeStamp, const QVector<md::ConcealRange> &p_ranges) {
            Q_UNUSED(p_timeStamp);
            applyConcealRanges(p_ranges);
          });
  updateSpellCheck();
  connect(getHighlighter(), &MarkdownHighlighter::highlightCompleted, this, [this]() {
    m_textEdit->updateCursorWidth();
    auto numberer = findChild<HeadingSourceNumberer *>(
        QStringLiteral("vte_heading_source_numberer"), Qt::FindDirectChildrenOnly);
    if (numberer && numberer->restoreViewportAfterHighlight()) {
      return;
    }
    if (m_textEdit->isViewportWidgetFocused()) {
      // An in-place preview widget holds the focus. This re-parse is most
      // likely the one its own write-back caused, and the editor's caret is
      // elsewhere - scrolling to it would yank the viewport away from the
      // widget the user is typing in.
      return;
    }

    m_textEdit->ensureCursorVisible();
    m_textEdit->checkCenterCursor();
  });
}

void VMarkdownEditor::setupDocumentLayout() {
  m_resourceMgr.reset(new DocumentResourceMgr());

  auto docLayout = new TextDocumentLayout(document(), m_resourceMgr.data());
  docLayout->setPreviewEnabled(true);

  document()->setDocumentLayout(docLayout);

  const auto refreshListItemCursor = [this]() {
    documentLayout()->setListItemCursorPosition(
        m_textEdit->isViewportWidgetFocused() ? -1 : m_textEdit->textCursor().position());
  };
  connect(m_textEdit, &VTextEdit::cursorPositionChanged, this, refreshListItemCursor);
  connect(qApp, &QApplication::focusChanged, this, refreshListItemCursor);
  refreshListItemCursor();

  const auto refreshConcealCursor = [this]() {
    documentLayout()->setConcealCursorPosition(m_textEdit->textCursor().position());
  };
  connect(m_textEdit, &VTextEdit::cursorPositionChanged, this, refreshConcealCursor);
  connect(docLayout, &TextDocumentLayout::concealmentChanged, this, [this]() {
    m_textEdit->updateCursorWidth();
    if (!m_textEdit->isViewportWidgetFocused()) {
      m_textEdit->ensureCursorVisible();
      m_textEdit->checkCenterCursor();
    }
  });
  refreshConcealCursor();

  connect(m_textEdit, &VTextEdit::cursorWidthChanged, this,
          [this]() { documentLayout()->setCursorWidth(m_textEdit->cursorWidth()); });
}

TextDocumentLayout *VMarkdownEditor::documentLayout() const {
  return static_cast<TextDocumentLayout *>(document()->documentLayout());
}

void VMarkdownEditor::setupPreviewMgr() {
  m_previewMgrInterface.reset(new EditorPreviewMgr(this));
  m_previewMgr = new PreviewMgr(m_previewMgrInterface.data(), this);
  m_previewMgr->setPreviewEnabled(true);
  connect(getHighlighter(), &MarkdownHighlighter::imageLinksUpdated, m_previewMgr,
          &PreviewMgr::updateImageLinks);
  connect(m_previewMgr, &PreviewMgr::requestUpdateImageLinks, getHighlighter(),
          &MarkdownHighlighter::updateHighlight);

  // Interactive preview widgets. The host is an internal QObject child so no
  // exported class needs a new data member.
  auto host = new InteractivePreviewHost(this);
  connect(getHighlighter(), &MarkdownHighlighter::previewElementsUpdated, host,
          &InteractivePreviewHost::updatePreviews);
}

InteractivePreviewHost *VMarkdownEditor::interactivePreviewHost() const {
  return findChild<InteractivePreviewHost *>(QLatin1String(InteractivePreviewHost::c_objectName),
                                             Qt::FindDirectChildrenOnly);
}

bool VMarkdownEditor::registerPreviewWidgetFactory(PreviewWidgetFactory *p_factory,
                                                   int p_priority) {
  auto host = interactivePreviewHost();
  return host ? host->registerFactory(p_factory, p_priority) : false;
}

bool VMarkdownEditor::unregisterPreviewWidgetFactory(PreviewWidgetFactory *p_factory) {
  auto host = interactivePreviewHost();
  return host ? host->unregisterFactory(p_factory) : false;
}

QVector<PreviewWidgetLocation> VMarkdownEditor::getVisiblePreviewWidgetLocations() const {
  auto host = interactivePreviewHost();
  return host ? host->getVisiblePreviewWidgetLocations() : QVector<PreviewWidgetLocation>();
}

bool VMarkdownEditor::focusPreviewWidget(quint64 p_identity) {
  auto host = interactivePreviewHost();
  return host ? host->focusPreviewWidget(p_identity) : false;
}

bool VMarkdownEditor::completeImageInsertion(quint64 p_requestId, const QString &p_imageSource) {
  auto host = interactivePreviewHost();
  return host && host->completeImageInsertion(p_requestId, p_imageSource);
}

void VMarkdownEditor::cancelImageInsertion(quint64 p_requestId) {
  if (auto host = interactivePreviewHost()) {
    host->cancelImageInsertion(p_requestId);
  }
}

bool VMarkdownEditor::handleTypeAction(TypeAction p_action, const QVariant &p_data) {
  if (auto host = interactivePreviewHost()) {
    if (host->handleTypeAction(p_action, p_data)) {
      return true;
    }
  }

  if (isReadOnly()) {
    return true;
  }

  switch (p_action) {
  case TypeAction::TypeHeading: {
    if (p_data.userType() != QMetaType::Int) {
      return false;
    }

    const int level = p_data.toInt();
    if (level < 0 || level > 6) {
      return false;
    }

    enterInsertModeIfApplicable();
    MarkdownUtils::typeHeading(getTextEdit(), level);
    return true;
  }

  case TypeAction::TypeTodoList:
    if (p_data.userType() != QMetaType::Bool) {
      return false;
    }

    enterInsertModeIfApplicable();
    MarkdownUtils::typeTodoList(getTextEdit(), p_data.toBool());
    return true;

  case TypeAction::TypeLink: {
    if (p_data.userType() != QMetaType::QStringList) {
      return false;
    }

    const QStringList link = p_data.toStringList();
    if (link.size() != 2) {
      return false;
    }

    enterInsertModeIfApplicable();
    MarkdownUtils::typeLink(getTextEdit(), link[0], link[1]);
    return true;
  }

  case TypeAction::TypeImage:
  case TypeAction::TypeTable:
    return false;

  case TypeAction::TypeBold:
  case TypeAction::TypeItalic:
  case TypeAction::TypeStrikethrough:
  case TypeAction::TypeMark:
  case TypeAction::TypeUnorderedList:
  case TypeAction::TypeOrderedList:
  case TypeAction::TypeCode:
  case TypeAction::TypeCodeBlock:
  case TypeAction::TypeMath:
  case TypeAction::TypeMathBlock:
  case TypeAction::TypeQuote:
    if (p_data.isValid()) {
      return false;
    }
    break;
  default:
    return false;
  }

  enterInsertModeIfApplicable();
  switch (p_action) {
  case TypeAction::TypeBold:
    MarkdownUtils::typeBold(getTextEdit());
    break;
  case TypeAction::TypeItalic:
    MarkdownUtils::typeItalic(getTextEdit());
    break;
  case TypeAction::TypeStrikethrough:
    MarkdownUtils::typeStrikethrough(getTextEdit());
    break;
  case TypeAction::TypeMark:
    MarkdownUtils::typeMark(getTextEdit());
    break;
  case TypeAction::TypeUnorderedList:
    MarkdownUtils::typeUnorderedList(getTextEdit());
    break;
  case TypeAction::TypeOrderedList:
    MarkdownUtils::typeOrderedList(getTextEdit());
    break;
  case TypeAction::TypeCode:
    MarkdownUtils::typeCode(getTextEdit());
    break;
  case TypeAction::TypeCodeBlock:
    MarkdownUtils::typeCodeBlock(getTextEdit());
    break;
  case TypeAction::TypeMath:
    MarkdownUtils::typeMath(getTextEdit());
    break;
  case TypeAction::TypeMathBlock:
    MarkdownUtils::typeMathBlock(getTextEdit());
    break;
  case TypeAction::TypeQuote:
    MarkdownUtils::typeQuote(getTextEdit());
    break;
  case TypeAction::TypeHeading:
  case TypeAction::TypeTodoList:
  case TypeAction::TypeLink:
  case TypeAction::TypeImage:
  case TypeAction::TypeTable:
    Q_UNREACHABLE();
  }

  return true;
}

DocumentResourceMgr *VMarkdownEditor::getDocumentResourceMgr() const {
  return m_resourceMgr.data();
}

const QPixmap *VMarkdownEditor::findImageFromDocumentResourceMgr(const QString &p_name) const {
  return m_resourceMgr->findImage(p_name);
}

MarkdownHighlighter *VMarkdownEditor::getHighlighter() const {
  return static_cast<MarkdownHighlighter *>(m_highlighter);
}

PreviewMgr *VMarkdownEditor::getPreviewMgr() const { return m_previewMgr; }

void VMarkdownEditor::setConfig(const QSharedPointer<MarkdownEditorConfig> &p_config) {
  m_config = p_config;
  m_config->fillDefaultTheme();

  VTextEditor::setConfig(p_config->m_textEditorConfig);

  // The base VTextEditor::updateFromConfig only re-applies the syntax theme to a
  // SyntaxHighlighter; the MarkdownHighlighter is not one, so its styles stay
  // locked to the theme set at construction. Re-apply the new theme explicitly
  // and rehighlight so a theme switch refreshes editor colors.
  if (auto *hl = getHighlighter()) {
    hl->setTheme(theme());
    hl->rehighlight();
  }

  updateFromConfig();
}

void VMarkdownEditor::updateFromConfig() {
  Q_ASSERT(m_config);

  documentLayout()->setConstrainPreviewWidthEnabled(
      m_config->m_constrainInplacePreviewWidthEnabled);

  documentLayout()->setListItemDecorationColors(
      theme()->editorStyle(Theme::ListItemGuide).textColor(),
      theme()->editorStyle(Theme::ActiveListItem).backgroundColor());
  documentLayout()->setConcealFormat(theme()->editorStyle(Theme::ConcealedText).toTextCharFormat());
  applyConcealRanges(getHighlighter()->getConcealRanges());

  updateInplacePreviewSources();

  // Not ANDed with the text folding switch: the provider gates on
  // TextFolding::isEnabled() itself, which also covers the restore path. And
  // deliberately not retroactive - a region which has already been settled
  // keeps the state it was settled into.
  m_foldingProvider->setAutoFoldPreviewsEnabled(m_config->m_autoFoldPreviewedBlocksEnabled);

  // Also deliberately not retroactive: a table which is already in the
  // document keeps the shape it has until the user edits it.
  if (auto host = interactivePreviewHost()) {
    host->setTableSourceAlignEnabled(m_config->m_autoFormatTableSourceEnabled);
  }

  if (auto formatter = findChild<MarkdownSourceFormatter *>(
          QStringLiteral("vte_markdown_source_formatter"), Qt::FindDirectChildrenOnly)) {
    formatter->setEnabled(m_config->m_autoFormatTableSourceEnabled,
                          m_config->m_autoNumberOrderedListsEnabled);
  }

  applyLineSpacing();

  updateSpaceWidth();
}

void VMarkdownEditor::applyConcealRanges(const QVector<md::ConcealRange> &p_ranges) {
  auto *layout = documentLayout();
  if (!m_config->m_concealElements || p_ranges.isEmpty()) {
    layout->setConcealedRanges({});
    return;
  }

  auto *doc = document();
  const int documentEnd = doc->characterCount() - 1;
  const int minimumLength = qMax(9, m_config->m_concealLengthThreshold);
  QVector<TextDocumentLayout::ConcealSpec> specs;
  specs.reserve(p_ranges.size());
  QTextBlock previousBlock;
  QTextBoundaryFinder finder;
  for (const auto &range : p_ranges) {
    if (range.m_element == MarkdownConcealElement::None ||
        !m_config->m_concealElements.testFlag(range.m_element) || range.m_startPos < 0 ||
        range.m_endPos <= range.m_startPos || range.m_endPos > documentEnd) {
      continue;
    }
    const auto block = doc->findBlock(range.m_startPos);
    if (!block.isValid() || range.m_endPos - block.position() > block.length() - 1) {
      continue;
    }
    // Candidates are source ordered; build grapheme attributes only once per block.
    if (block != previousBlock) {
      finder = QTextBoundaryFinder(QTextBoundaryFinder::Grapheme, block.text());
      previousBlock = block;
    }
    const int start = range.m_startPos - block.position();
    const int end = range.m_endPos - block.position();
    finder.setPosition(end);
    if (!finder.isAtBoundary()) {
      continue;
    }
    finder.setPosition(start);
    if (!finder.isAtBoundary()) {
      continue;
    }
    int length = 0;
    while (finder.position() < end && length <= minimumLength) {
      if (finder.toNextBoundary() < 0) {
        break;
      }
      ++length;
    }
    if (length > minimumLength) {
      specs.append({block, start, end});
    }
  }
  if (!layout->setConcealedRanges(specs)) {
    // Never leave an old snapshot attached to a newly rejected parse result.
    layout->setConcealedRanges({});
  }
}

void VMarkdownEditor::applyPreviewFolding() {
  if (!m_foldingProvider) {
    return;
  }

  auto vbar = m_textEdit->verticalScrollBar();
  auto layout = documentLayout();

  // Auto-folding a previewed block HIDES its source, so the document gets
  // SHORTER - the mirror image of the growth EditorPreviewMgr::relayout()
  // compensates for, and a second, later geometry change that its anchor has
  // already been retired by the time this runs. QScrollBar keeps its value()
  // across the range update either way, so without an anchor here every fold
  // above the viewport slides the visible text UP.
  //
  // Anchor on the first visible block, exactly as the relayout path does.
  // blockBoundingRect() lazily repairs a stale block layout, so it must not be
  // called from inside a layout pass.
  int originValue = 0;
  int anchorBlockNumber = -1;
  int originRevision = 0;
  qreal anchorViewportY = 0;
  bool anchored = false;

  if (!m_inFoldScrollAnchor && vbar && layout && !layout->isBusy()) {
    originValue = vbar->value();
    // At the very top nothing above the viewport can displace the content.
    if (originValue != vbar->minimum()) {
      const auto anchorBlock = TextEditUtils::firstVisibleBlock(m_textEdit);
      if (anchorBlock.isValid()) {
        anchored = true;
        anchorBlockNumber = anchorBlock.blockNumber();
        anchorViewportY = layout->blockBoundingRect(anchorBlock).y() - originValue;
        originRevision = document()->revision();
      }
    }
  }

  auto host = interactivePreviewHost();
  const auto ranges = host ? host->previewedRanges() : QVector<PreviewedRange>();
  const auto states =
      m_foldingProvider->applyPreviewAutoFold(ranges, m_textEdit->textCursor().blockNumber());
  if (host) {
    host->setPreviewFoldStates(states);
  }

  if (!anchored) {
    return;
  }

  // applyPreviewAutoFold() reaches application code synchronously - foldRange()
  // emits foldingRangesChanged(), and the geometry that follows hands a context
  // to every preview widget - so NOTHING captured above may be trusted without
  // being re-established first. The provider re-resolves its own table across
  // the same boundary for exactly this reason.
  //
  // A replaced scrollbar or layout leaves the captured pointers dangling, and a
  // document edit makes anchorBlockNumber name a different line. Any of those
  // means the correction describes a document nobody is looking at any more;
  // drop it rather than guess.
  if (m_textEdit->verticalScrollBar() != vbar || documentLayout() != layout ||
      document()->revision() != originRevision || layout->isBusy()) {
    return;
  }

  // The same "user scrolled meanwhile" rule as
  // InteractivePreviewHost::applyRealizationScrollCompensation(): a newer
  // scroll always wins over an older correction.
  if (vbar->value() != originValue) {
    return;
  }

  auto anchorBlock = document()->findBlockByNumber(anchorBlockNumber);
  if (!anchorBlock.isValid()) {
    return;
  }

  // The anchor may have been folded away. TextFolding::setRangeFolded() keeps a
  // region's FIRST and last block visible, so the hidden content collapsed
  // upwards into a block above; walking backwards lands on it and keeps the
  // anchor at or above where the user was looking. Walking forward is only the
  // fallback for a document whose head is entirely hidden, which
  // blockBoundingRect() could not measure at all.
  if (!anchorBlock.isVisible()) {
    auto probe = anchorBlock.previous();
    while (probe.isValid() && !probe.isVisible()) {
      probe = probe.previous();
    }

    if (!probe.isValid()) {
      probe = anchorBlock.next();
      while (probe.isValid() && !probe.isVisible()) {
        probe = probe.next();
      }
    }

    if (!probe.isValid()) {
      return;
    }

    anchorBlock = probe;
  }

  const qreal newDocY = layout->blockBoundingRect(anchorBlock).y();
  const int target = qBound(vbar->minimum(), qRound(newDocY - anchorViewportY), vbar->maximum());
  if (target == originValue) {
    return;
  }

  m_inFoldScrollAnchor = true;
  vbar->setValue(target);
  m_inFoldScrollAnchor = false;
}

bool VMarkdownEditor::restoreFoldAfterPreviewRewrite(PreviewElementType p_type, int p_startBlock,
                                                     int p_endBlock) {
  if (m_foldingProvider &&
      m_foldingProvider->restoreFoldedRange(p_type, p_startBlock, p_endBlock)) {
    // The fold was destroyed and restored synchronously inside the rewrite's
    // turn. The folded-line background is an extra selection whose cursor was
    // dragged past the replacement by Qt, and the corrected list is only
    // scheduled behind a 200ms coalescing timer. Push it now so the stale
    // position never reaches the repaint which follows this call. Only owed
    // when a range was really created: the restore is a no-op for a replacement
    // which collapsed the element onto a single block, and then nothing changed.
    applyPendingExtraSelections();
    return true;
  }

  return false;
}

bool VMarkdownEditor::tryPreviewSourceFolded(PreviewElementType p_type, int p_startBlock,
                                             int p_endBlock, bool *p_folded) const {
  return m_foldingProvider
             ? m_foldingProvider->tryRegionFolded(p_type, p_startBlock, p_endBlock, p_folded)
             : false;
}

void VMarkdownEditor::setInplacePreviewEnabled(bool p_enabled) {
  if (m_inplacePreviewEnabled == p_enabled) {
    return;
  }

  m_inplacePreviewEnabled = p_enabled;
  updateInplacePreviewSources();
}

void VMarkdownEditor::updateInplacePreviewSources() {
  auto host = interactivePreviewHost();

  if (!m_inplacePreviewEnabled) {
    m_previewMgr->setPreviewEnabled(false);
    if (host) {
      host->setEnabled(false);
    }
    return;
  }

  if (host) {
    host->setEnabled(true);
    host->setTypeEnabled(PreviewElementType::Image,
                         m_config->m_inplacePreviewSources & MarkdownEditorConfig::ImageLink);
    host->setTypeEnabled(PreviewElementType::Code,
                         m_config->m_inplacePreviewSources & MarkdownEditorConfig::CodeBlock);
    host->setTypeEnabled(PreviewElementType::Math,
                         m_config->m_inplacePreviewSources & MarkdownEditorConfig::Math);
    host->setTypeEnabled(PreviewElementType::Table,
                         m_config->m_inplacePreviewSources & MarkdownEditorConfig::Table);
  }

  // The painted path only knows about image, code and math.
  const auto paintedSources = m_config->m_inplacePreviewSources &
                              (MarkdownEditorConfig::ImageLink | MarkdownEditorConfig::CodeBlock |
                               MarkdownEditorConfig::Math);
  if (paintedSources == (MarkdownEditorConfig::ImageLink | MarkdownEditorConfig::CodeBlock |
                         MarkdownEditorConfig::Math)) {
    m_previewMgr->setPreviewEnabled(true);
  } else {
    m_previewMgr->setPreviewEnabled(false);
    if (paintedSources & MarkdownEditorConfig::ImageLink) {
      m_previewMgr->setPreviewEnabled(PreviewData::Source::ImageLink, true);
    }
    if (paintedSources & MarkdownEditorConfig::CodeBlock) {
      m_previewMgr->setPreviewEnabled(PreviewData::Source::CodeBlock, true);
    }
    if (paintedSources & MarkdownEditorConfig::Math) {
      m_previewMgr->setPreviewEnabled(PreviewData::Source::MathBlock, true);
    }
  }
}

bool VMarkdownEditor::eventFilter(QObject *p_obj, QEvent *p_event) {
  if (p_obj == m_textEdit->viewport() && p_event->type() == QEvent::ToolTip) {
    const auto helpEvent = static_cast<QHelpEvent *>(p_event);
    auto viewport = m_textEdit->viewport();
    const auto horizontalBar = m_textEdit->horizontalScrollBar();
    const QPoint scroll(m_textEdit->isRightToLeft()
                            ? horizontalBar->maximum() - horizontalBar->value()
                            : horizontalBar->value(),
                        m_textEdit->verticalScrollBar()->value());
    const int position = documentLayout()->hitTest(helpEvent->pos() + scroll, Qt::ExactHit);
    const auto block = position < 0 ? QTextBlock() : document()->findBlock(position);
    if (block.isValid() && block.isVisible()) {
      const auto data = BlockLayoutData::get(block);
      if (data->m_concealRevision == block.revision()) {
        const int column = position - block.position();
        for (const auto &range : data->m_concealedRanges) {
          if (column < range.m_start || column >= range.m_end) {
            continue;
          }
          // A submitted range may be revealed by the caret or IME, or rejected by shaping.
          const auto marker =
              std::find_if(data->m_concealMarkers.cbegin(), data->m_concealMarkers.cend(),
                           [&range](const ConcealPaintData &p_marker) {
                             return p_marker.m_hiddenStart == range.m_hiddenStart &&
                                    p_marker.m_hiddenEnd == range.m_hiddenEnd;
                           });
          if (marker == data->m_concealMarkers.cend()) {
            break;
          }
          const auto line = block.layout()->lineForTextPosition(column);
          const qreal start = line.cursorToX(qMax(range.m_start, line.textStart()));
          const qreal end = line.cursorToX(qMin(range.m_end, line.textStart() + line.textLength()));
          QRectF rect(qMin(start, end), line.y(), qAbs(end - start), line.height());
          if (line.lineNumber() ==
              block.layout()->lineForTextPosition(marker->m_hiddenStart).lineNumber()) {
            rect = rect.united(marker->m_rect);
          }
          rect.translate(-scroll.x(), data->top() - scroll.y());
          const auto text = block.text().mid(range.m_start, range.m_end - range.m_start);
          QToolTip::showText(helpEvent->globalPos(), Qt::convertFromPlainText(text), viewport,
                             rect.toAlignedRect());
          return true;
        }
      }
    }
    QToolTip::hideText();
    p_event->ignore();
    return true;
  }
  if (p_obj == m_textEdit) {
    switch (p_event->type()) {
    case QEvent::KeyPress:
      if (handleKeyPressEvent(static_cast<QKeyEvent *>(p_event))) {
        return true;
      }
      break;

    default:
      break;
    }
  }
  return VTextEditor::eventFilter(p_obj, p_event);
}

bool VMarkdownEditor::handleKeyPressEvent(QKeyEvent *p_event) {
  Q_UNUSED(p_event);
  return false;
}

void VMarkdownEditor::zoom(int p_delta) {
  const int preFontSize = editorFontPointSize();
  VTextEditor::zoom(p_delta);
  const int postFontSize = editorFontPointSize();

  if (preFontSize == postFontSize) {
    return;
  }

  getHighlighter()->updateStylesFontSize(postFontSize - preFontSize);

  updateSpaceWidth();
  applyLineSpacing();
}

void VMarkdownEditor::applyLineSpacing() {
  qreal multiplier = qMax(m_config->m_textEditorConfig->m_lineSpacing, 1.0);
  QFontMetricsF fmf(m_textEdit->font(), m_textEdit);
  const qreal leadingSpace = fmf.lineSpacing() * (multiplier - 1.0);

  if (qFuzzyCompare(documentLayout()->getLeadingSpaceOfLine() + 1.0, leadingSpace + 1.0)) {
    return;
  }

  documentLayout()->setLeadingSpaceOfLine(leadingSpace);
  documentLayout()->relayout();
}

void VMarkdownEditor::updateSpaceWidth() {
  const auto &codeBlockFormat = getHighlighter()->codeBlockStyle();
  auto font = codeBlockFormat.font();
  if (codeBlockFormat.fontPointSize() < 0.001) {
    font.setPointSize(editorFontPointSize());
  }

  QFontMetricsF fmf(font, m_textEdit);
  m_textEdit->setSpaceWidth(fmf.horizontalAdvance(QLatin1Char(' ')));
}

void VMarkdownEditor::preKeyReturn(int p_modifiers, bool *p_changed, bool *p_handled) {
  Q_ASSERT(!m_textEdit->isReadOnly());
  auto formatter = findChild<MarkdownSourceFormatter *>(
      QStringLiteral("vte_markdown_source_formatter"), Qt::FindDirectChildrenOnly);
  Q_ASSERT(formatter);
  formatter->takeListReturnSuppression();

  // Probe before splitting; only this Return may consume the quote context.
  m_returnBlockContext = getHighlighter()->getBlockContext(m_textEdit->textCursor().blockNumber());
  if (p_modifiers == Qt::ShiftModifier) {
    *p_changed = true;
    auto cursor = m_textEdit->textCursor();
    cursor.beginEditBlock();
    cursor.insertText(QStringLiteral("  "));
    cursor.endEditBlock();
    m_textEdit->setTextCursor(cursor);
  } else if (p_modifiers == Qt::NoModifier) {
    if (formatter->handleListInsertion(VTextEdit::BlockInsertion::Split)) {
      *p_changed = true;
      *p_handled = true;
      m_returnBlockContext = md::BlockContext();
      return;
    }
    if (m_textEdit->textCursor().hasSelection()) {
      // The base split retains the selection's first line, not necessarily
      // its active endpoint. Only the surviving source may drive continuation.
      m_returnBlockContext.m_fresh = false;
      m_returnBlockContext.m_quoteDepth = 0;
    }
  }
}

void VMarkdownEditor::postKeyReturn(int p_modifiers) {
  Q_ASSERT(!m_textEdit->isReadOnly());
  auto formatter = findChild<MarkdownSourceFormatter *>(
      QStringLiteral("vte_markdown_source_formatter"), Qt::FindDirectChildrenOnly);
  Q_ASSERT(formatter);
  const bool suppressList = formatter->takeListReturnSuppression();
  const auto blockContext = m_returnBlockContext;
  m_returnBlockContext = md::BlockContext();

  if (p_modifiers != Qt::NoModifier) {
    return;
  }

  auto cursor = m_textEdit->textCursor();

  const auto block = cursor.block();
  auto preBlock = block.previous();
  Q_ASSERT(preBlock.isValid());
  const auto preText = preBlock.text();

  if (blockContext.m_valid && blockContext.m_inFencedCode) {
    // Never continue markers inside a fence.
    return;
  }

  if (preText.isEmpty()) {
    return;
  }

  // Already indented by VTextEdit.

  QString indent, quotePrefix, rest;
  int textDepth = 0;
  bool quoted = MarkdownUtils::isQuote(preText, indent, quotePrefix, rest, textDepth);

  if (!quoted && blockContext.m_fresh && blockContext.m_quoteDepth > 0) {
    // Lazy continuation: the AST knows this line belongs to a quote although
    // the text carries no marker. Insert-causing, so it requires a fresh AST.
    quotePrefix = QStringLiteral("> ").repeated(blockContext.m_quoteDepth);
    // The leading indentation has already been copied into the new block by
    // AutoIndentHelper::autoIndent.
    rest = preText.mid(TextUtils::fetchIndentation(preText));
    quoted = true;
  }

  const QString &listSource = quoted ? rest : preText;

  QString marker;
  md::ListItemInfo listMarker;
  if (!suppressList &&
      md::scanListMarker(listSource, TextUtils::fetchIndentation(listSource), listMarker)) {
    const bool ordered =
        listMarker.m_marker == QLatin1Char('.') || listMarker.m_marker == QLatin1Char(')');
    if (!ordered || listMarker.m_sourceNumber < 999999999) {
      if (ordered) {
        marker = QString::number(listMarker.m_sourceNumber + 1);
      }
      marker += listMarker.m_marker;
      marker += QLatin1Char(' ');
      if (listMarker.m_task) {
        marker += QStringLiteral("[ ] ");
      }
    }
  }

  const QString innerIndent = quoted ? TextUtils::fetchIndentationSpaces(rest) : QString();
  const QString textToInsert = quotePrefix + innerIndent + marker;
  if (textToInsert.isEmpty()) {
    return;
  }

  cursor.joinPreviousEditBlock();
  cursor.insertText(textToInsert);
  cursor.endEditBlock();
  m_textEdit->setTextCursor(cursor);
}

void VMarkdownEditor::preKeyTab(int p_modifiers, bool *p_handled) {
  Q_ASSERT(!m_textEdit->isReadOnly());
  if (p_modifiers == Qt::NoModifier) {
    auto cursor = m_textEdit->textCursor();
    if (cursor.hasSelection()) {
      return;
    }

    const auto block = cursor.block();
    const auto text = block.text().left(cursor.positionInBlock());
    if (text.isEmpty()) {
      return;
    }

    QChar listMark;
    bool isEmpty = false;
    if (MarkdownUtils::isTodoList(text, listMark, isEmpty) ||
        MarkdownUtils::isUnorderedList(text, listMark, isEmpty)) {
      // Indent the empty todo/unordered list.
      if (isEmpty) {
        *p_handled = true;
        TextEditUtils::indentBlock(cursor, !m_textEdit->isTabExpanded(),
                                   m_textEdit->getTabStopWidthInSpaces(), false);
        m_textEdit->setTextCursor(cursor);
      }
      return;
    }

    QString listNumber;
    if (MarkdownUtils::isOrderedList(text, listNumber, isEmpty) && isEmpty) {
      *p_handled = true;
      // A non-one ordered marker cannot interrupt its parent's paragraph, so
      // asynchronous numbering cannot repair the missing nested list.
      const auto afterText = MarkdownUtils::setOrderedListNumber(text, 1);
      cursor.beginEditBlock();
      if (afterText != text) {
        cursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::KeepAnchor);
        cursor.insertText(afterText);
      }
      TextEditUtils::indentBlock(cursor, !m_textEdit->isTabExpanded(),
                                 m_textEdit->getTabStopWidthInSpaces(), false);
      cursor.endEditBlock();
      m_textEdit->setTextCursor(cursor);
      return;
    }
  }
}

void VMarkdownEditor::preKeyBacktab(int p_modifiers, bool *p_handled) {
  Q_ASSERT(!m_textEdit->isReadOnly());
  if (p_modifiers == Qt::ShiftModifier) {
    auto cursor = m_textEdit->textCursor();
    if (cursor.hasSelection()) {
      return;
    }

    const auto block = cursor.block();
    const auto text = block.text().left(cursor.positionInBlock());
    if (text.isEmpty()) {
      return;
    }

    QChar listMark;
    bool isEmpty = false;
    if (MarkdownUtils::isTodoList(text, listMark, isEmpty) ||
        MarkdownUtils::isUnorderedList(text, listMark, isEmpty)) {
      // Unindent the empty todo/unordered list.
      if (isEmpty) {
        *p_handled = true;
        TextEditUtils::unindentBlock(cursor, m_textEdit->getTabStopWidthInSpaces());
        m_textEdit->setTextCursor(cursor);
      }
      return;
    }

    QString listNumber;
    if (MarkdownUtils::isOrderedList(text, listNumber, isEmpty) && isEmpty) {
      *p_handled = true;

      cursor.beginEditBlock();

      // Unindent the empty ordered list.
      TextEditUtils::unindentBlock(cursor, m_textEdit->getTabStopWidthInSpaces());

      if (!m_config->m_autoNumberOrderedListsEnabled) {
        const auto newText = block.text().left(cursor.positionInBlock());
        Q_ASSERT(MarkdownUtils::isOrderedList(newText, listNumber, isEmpty));

        // Try to correct the list number.
        int newNumber = 1;
        {
          const auto preBlock = block.previous();
          if (preBlock.isValid()) {
            const auto preText = preBlock.text();
            if (TextUtils::fetchIndentation(preText) == TextUtils::fetchIndentation(newText)) {
              QString preListNumber;
              bool preIsEmpty = false;
              if (MarkdownUtils::isOrderedList(preText, preListNumber, preIsEmpty)) {
                newNumber = preListNumber.toInt() + 1;
              }
            }
          }
        }

        auto afterText = MarkdownUtils::setOrderedListNumber(newText, newNumber);
        if (afterText != newText) {
          cursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::KeepAnchor);
          cursor.insertText(afterText);
        }
      }
      cursor.endEditBlock();
      m_textEdit->setTextCursor(cursor);
      return;
    }
  }
}

void VMarkdownEditor::handleExternalCodeBlockHighlightData(int p_idx, TimeStamp p_timeStamp,
                                                           const QString &p_html) {
  Q_ASSERT(m_webCodeBlockHighlighter);
  m_webCodeBlockHighlighter->handleExternalCodeBlockHighlightData(p_idx, p_timeStamp, p_html);
}

void VMarkdownEditor::setExternalCodeBlockHighlihgtStyles(
    const ExternalCodeBlockHighlightStyles &p_styles) {
  WebCodeBlockHighlighter::setExternalCodeBlockHighlihgtStyles(p_styles);
}

void VMarkdownEditor::handleExternalMathHighlightData(int p_idx, TimeStamp p_timeStamp,
                                                      const QString &p_html) {
  Q_ASSERT(m_mathBlockHighlighter);
  m_mathBlockHighlighter->handleExternalMathHighlightData(p_idx, p_timeStamp, p_html);
}

#include "vmarkdowneditor.moc"
