#include "interactivepreviewhost.h"

#include <algorithm>
#include <climits>

#include <QApplication>
#include <QEvent>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextDocument>
#include <QTimer>
#include <QWidget>

#include <vtextedit/markdownhighlighter.h>
#include <vtextedit/theme.h>
#include <vtextedit/vmarkdowneditor.h>
#include <vtextedit/vtextedit.h>

#include "markdownastwalker.h"
#include "previewbuilder.h"
#include "previewfromast.h"
#include "previewlogging.h"
#include "tablepreviewwidget.h"

using namespace vte;

namespace {
const char *statusName(PreviewReplacementResult::Status p_status) {
  switch (p_status) {
  case PreviewReplacementResult::Accepted:
    return "Accepted";
  case PreviewReplacementResult::UnknownIdentity:
    return "UnknownIdentity";
  case PreviewReplacementResult::StaleSnapshot:
    return "StaleSnapshot";
  case PreviewReplacementResult::SourceMismatch:
    return "SourceMismatch";
  case PreviewReplacementResult::InvalidRange:
    return "InvalidRange";
  case PreviewReplacementResult::ReadOnly:
    return "ReadOnly";
  case PreviewReplacementResult::ParseFailure:
    return "ParseFailure";
  case PreviewReplacementResult::ElementCountMismatch:
    return "ElementCountMismatch";
  case PreviewReplacementResult::TypeMismatch:
    return "TypeMismatch";
  case PreviewReplacementResult::Deferred:
    return "Deferred";
  default:
    return "?";
  }
}
} // namespace

const char *InteractivePreviewHost::c_objectName = "vte_interactive_preview_host";

const char *InteractivePreviewHost::c_enabledTypeMaskProperty = "vte_preview_enabled_type_mask";

const char *InteractivePreviewHost::c_reconcileDeliveryCountProperty =
    "vte_preview_reconcile_deliveries";

const char *InteractivePreviewHost::c_foldRefreshCountProperty = "vte_preview_fold_refreshes";

const char *InteractivePreviewHost::c_owedWorkDrainCountProperty = "vte_preview_owed_work_drains";

const int InteractivePreviewHost::c_replacementRetryBudget = 8;

InteractivePreviewHost::BlockGuard::BlockGuard(InteractivePreviewHost *p_host, Reason p_reason)
    : m_host(p_host), m_reason(p_reason) {
  ++m_host->m_blockDepth;
  if (m_reason == Reason::FactoryCallback) {
    ++m_host->m_factoryCallbackDepth;
  }
}

InteractivePreviewHost::BlockGuard::~BlockGuard() {
  Q_ASSERT(m_host->m_blockDepth > 0);
  if (m_reason == Reason::FactoryCallback) {
    Q_ASSERT(m_host->m_factoryCallbackDepth > 0);
    --m_host->m_factoryCallbackDepth;
  }

  if (--m_host->m_blockDepth > 0) {
    return;
  }

  // Outermost guard: whatever was owed while the block was held can be
  // delivered now. This only arms a timer, so it is safe during unwinding.
  m_host->scheduleOwedWork();
}

// Exclusive offset after the last non-whitespace character.
//
// findSoleMatchingElement() accepts trailing whitespace after the matched
// element, so an accepted replacement's span and the parsed element's span can
// differ: the anchor keeps spanning the whole replacement while the folding
// region ends at the last non-blank line. Both the query and the restore have
// to describe the element, not the anchor.
static int trimmedEndOffset(const QString &p_text) {
  int end = p_text.size();
  while (end > 0 && p_text.at(end - 1).isSpace()) {
    --end;
  }
  return end;
}


static int typeOrder(PreviewElementType p_type) { return static_cast<int>(p_type); }

static int typeIndex(PreviewElementType p_type) {
  const int index = static_cast<int>(p_type);
  Q_ASSERT(index >= 0 && index < c_previewElementTypeCount);
  return index;
}

InteractivePreviewHost::InteractivePreviewHost(VMarkdownEditor *p_editor)
    : QObject(p_editor), m_editor(p_editor) {
  setObjectName(QLatin1String(c_objectName));

  registerPreviewMetaTypes();

  m_textEdit = m_editor->getTextEdit();
  m_doc = m_editor->document();
  m_layout = m_editor->documentLayout();

  connect(m_layout, &TextDocumentLayout::widgetPreviewGeometryChanged, this,
          &InteractivePreviewHost::syncWidgetGeometry);

  // The second of the two unblock edges. The first is the outermost BlockGuard.
  connect(m_layout, &TextDocumentLayout::becameIdle, this,
          &InteractivePreviewHost::scheduleOwedWork);

  if (m_textEdit) {
    connect(m_textEdit, &VTextEdit::resized, this, &InteractivePreviewHost::schedulePublish);
    // Read-only is not observable through any signal, so watch the event.
    m_textEdit->installEventFilter(this);
    // Scrolling only shifts the viewport mapping, so it takes the cheap path.
    if (auto vbar = m_textEdit->verticalScrollBar()) {
      connect(vbar, &QScrollBar::valueChanged, this, &InteractivePreviewHost::applyScrollOffset);
    }
    if (auto hbar = m_textEdit->horizontalScrollBar()) {
      connect(hbar, &QScrollBar::valueChanged, this, &InteractivePreviewHost::applyScrollOffset);
    }
  }

  // A preview's focusable widget is a descendant of the preview root, whose
  // FocusIn an event filter on the root never sees, hence the global watch.
  connect(qApp, &QApplication::focusChanged, this,
          &InteractivePreviewHost::handleApplicationFocusChanged);

  if (m_doc) {
    // The live anchors follow every edit, so resubmit the reservations as soon
    // as the event loop turns instead of waiting for the next parse.
    connect(m_doc, &QTextDocument::contentsChanged, this, [this]() {
      if (!m_items.isEmpty()) {
        schedulePublish();
      }
    });
  }

  // Built-in renderers use priority 0 so applications can override them.
  m_tableFactory = new TablePreviewWidgetFactory(this);
  connect(m_tableFactory.data(), &TablePreviewWidgetFactory::focusEscapeRequested, this,
          [this](TablePreviewWidget *p_widget, FocusEscapeDirection p_direction) {
            handleFocusEscape(p_widget, p_direction);
          });
  connect(m_tableFactory.data(), &TablePreviewWidgetFactory::undoRequested, this,
          [this](TablePreviewWidget *p_widget) { handleSheetUndo(p_widget); });
  connect(m_tableFactory.data(), &TablePreviewWidgetFactory::redoRequested, this,
          [this](TablePreviewWidget *p_widget) { handleSheetRedo(p_widget); });
  registerFactory(m_tableFactory.data(), 0);

  publishEnabledTypeMask();
}

InteractivePreviewHost::~InteractivePreviewHost() {
  removeAllItems(true);
  // Nothing may survive the editor, including removals whose deferred deletion
  // has not been delivered yet.
  flushPendingDeletions();
}

QWidget *InteractivePreviewHost::viewport() const {
  return m_textEdit ? m_textEdit->viewport() : nullptr;
}

// ---------------------------------------------------------------------------
// Factories
// ---------------------------------------------------------------------------

bool InteractivePreviewHost::registerFactory(PreviewWidgetFactory *p_factory, int p_priority) {
  // Reject reentrant registration: a factory must not mutate the registry from
  // inside its own callback, and reparenting itself must not recurse either.
  if (!p_factory || m_reconciling || inFactoryCallback() || m_mutatingFactories) {
    return false;
  }

  for (const auto &entry : m_factories) {
    if (entry.m_factory.data() == p_factory) {
      return false;
    }
  }

  m_mutatingFactories = true;

  FactoryEntry entry;
  entry.m_factory = p_factory;
  entry.m_priority = p_priority;
  entry.m_order = m_nextFactoryOrder++;
  m_factories.append(entry);

  // Ownership transfers to the host.
  p_factory->setParent(this);

  m_mutatingFactories = false;

  qCDebug(previewHostLog) << "registered factory" << p_factory->metaObject()->className()
                          << "priority" << p_priority << "- now" << m_factories.size()
                          << "factory(ies)";

  publishEnabledTypeMask();
  reconcileLater();
  return true;
}

bool InteractivePreviewHost::unregisterFactory(PreviewWidgetFactory *p_factory) {
  if (!p_factory) {
    return false;
  }

  for (int i = 0; i < m_factories.size(); ++i) {
    if (m_factories[i].m_factory.data() != p_factory) {
      continue;
    }

    // Erase first so a reentrant callback cannot resolve it anymore.
    m_factories.removeAt(i);

    p_factory->deleteLater();

    qCDebug(previewHostLog) << "unregistered factory" << p_factory->metaObject()->className()
                            << "- now" << m_factories.size() << "factory(ies)";

    publishEnabledTypeMask();
    reconcileLater();
    return true;
  }

  return false;
}

QVector<InteractivePreviewHost::FactoryEntry> InteractivePreviewHost::orderedFactories() const {
  QVector<FactoryEntry> result;
  result.reserve(m_factories.size());
  for (const auto &entry : m_factories) {
    if (!entry.m_factory.isNull()) {
      result.append(entry);
    }
  }

  std::stable_sort(result.begin(), result.end(),
                   [](const FactoryEntry &p_a, const FactoryEntry &p_b) {
                     if (p_a.m_priority != p_b.m_priority) {
                       // Descending explicit priority.
                       return p_a.m_priority > p_b.m_priority;
                     }
                     return p_a.m_order < p_b.m_order;
                   });

  return result;
}

bool InteractivePreviewHost::isFactoryActive(PreviewWidgetFactory *p_factory) const {
  for (const auto &entry : m_factories) {
    if (entry.m_factory.data() == p_factory) {
      return true;
    }
  }

  return false;
}

bool InteractivePreviewHost::isTypeClaimable(PreviewElementType p_type) {
  // supportedTypes() is application code and may unregister a factory (or
  // register another one) from inside the call, which mutates m_factories.
  // Iterate a snapshot and mark the span as a callback, exactly as
  // createWidgetFor() does; without the guard a reentrant registerFactory()
  // would even be accepted and could reallocate the vector under us.
  const auto factories = orderedFactories();
  for (const auto &entry : factories) {
    QPointer<PreviewWidgetFactory> factory = entry.m_factory;
    if (factory.isNull() || !isFactoryActive(factory.data())) {
      continue;
    }

    QVector<PreviewElementType> supported;
    {
      BlockGuard guard(this, BlockGuard::Reason::FactoryCallback);
      supported = factory->supportedTypes();
    }

    // The factory may have been deactivated or destroyed by its own callback.
    if (factory.isNull() || !isFactoryActive(factory.data())) {
      continue;
    }

    if (supported.contains(p_type)) {
      return true;
    }
  }

  return false;
}

void InteractivePreviewHost::publishEnabledTypeMask() {
  auto highlighter = m_editor ? m_editor->getHighlighter() : nullptr;
  if (!highlighter) {
    return;
  }

  int mask = 0;
  if (m_enabled) {
    for (int i = 0; i < c_previewElementTypeCount; ++i) {
      const auto type = static_cast<PreviewElementType>(i);
      // Building a snapshot only pays off when something can render it.
      if (m_typeEnabled[i] && isTypeClaimable(type)) {
        mask |= (1 << i);
      }
    }
  }

  highlighter->setProperty(c_enabledTypeMaskProperty, mask);

  if (previewHostLog().isDebugEnabled()) {
    QStringList enabled;
    for (int i = 0; i < c_previewElementTypeCount; ++i) {
      const auto type = static_cast<PreviewElementType>(i);
      if (mask & (1 << i)) {
        enabled << QLatin1String(previewTypeName(type));
      } else if (m_typeEnabled[i]) {
        // Configured on, but nothing can render it.
        enabled << (QLatin1String(previewTypeName(type)) + QLatin1String("(unclaimed)"));
      }
    }

    qCDebug(previewHostLog) << "enabled type mask" << Qt::hex << mask << Qt::dec << "-"
                            << (enabled.isEmpty() ? QStringLiteral("nothing")
                                                  : enabled.join(QLatin1String(", ")));
  }
}

// ---------------------------------------------------------------------------
// Enablement
// ---------------------------------------------------------------------------

void InteractivePreviewHost::setEnabled(bool p_enabled) {
  if (m_enabled == p_enabled) {
    return;
  }

  m_enabled = p_enabled;
  publishEnabledTypeMask();
  reconcileLater();
}

void InteractivePreviewHost::setTypeEnabled(PreviewElementType p_type, bool p_enabled) {
  const int idx = typeIndex(p_type);
  if (m_typeEnabled[idx] == p_enabled) {
    return;
  }

  m_typeEnabled[idx] = p_enabled;
  publishEnabledTypeMask();
  reconcileLater();
}

bool InteractivePreviewHost::isTypeEnabled(PreviewElementType p_type) const {
  return m_typeEnabled[typeIndex(p_type)];
}

// ---------------------------------------------------------------------------
// Reconciliation
// ---------------------------------------------------------------------------

void InteractivePreviewHost::flushDirtySheets() {
  // Identities are allocated monotonically and never reused, and a flush is
  // reentrant - it applies a document edit and calls schedulePublish() - so
  // every item is re-resolved by id rather than held across the call.
  const auto ids = m_items.keys();
  for (quint64 id : ids) {
    auto it = m_items.find(id);
    if (it == m_items.end()) {
      continue;
    }

    QPointer<TablePreviewWidget> sheet =
        qobject_cast<TablePreviewWidget *>(it.value().m_widget.data());
    if (sheet) {
      const auto outcome = sheet->flushNow();
      // A rebuild does not start while blocked, so this flush always runs on a
      // clean stack and can never be postponed. See removeItem().
      Q_ASSERT(outcome != TablePreviewWidget::FlushOutcome::Deferred);
      Q_UNUSED(outcome);
    }
  }
}

void InteractivePreviewHost::rebuildAll() {
  if (isBlocked()) {
    // Before flushDirtySheets() and before m_carriedItems is populated: a
    // flush which is postponed leaves the anchors describing pre-flush source,
    // and snapshotting those is exactly what the comment below warns against.
    // This is also what makes removeItem()'s mandatory flush structurally
    // unable to be answered with Deferred - a rebuild never starts blocked.
    m_reconcilePending = true;
    scheduleOwedWork();
    qCDebug(previewHostLog) << "postponed the rebuild - the host is blocked";
    return;
  }

  // A sheet with a pending write-back applies a document edit when it is
  // flushed, and insertText() over the anchored range collapses that anchor -
  // including the copy carried below, because a QTextCursor copy shares one
  // registered private with the original. Flushing first is what makes the
  // anchors snapshotted here the retargeted ones; without it the rebuild would
  // see a collapsed range and drop the element to the painted path.
  flushDirtySheets();

  // Carry the live state across the rebuild: m_lastPreviews holds the
  // positions of parse generation m_revision, which the document may have
  // moved since, whereas the anchors have been tracking every edit. An
  // accepted replacement may also have rebased the bound snapshot, which the
  // replayed generation snapshot no longer describes.
  //
  // Key on the generation snapshot, since that is what the replay hands to
  // createItem().
  m_carriedItems.clear();
  for (auto it = m_items.constBegin(); it != m_items.constEnd(); ++it) {
    const auto &item = it.value();
    const auto &key = item.m_generationPreview ? item.m_generationPreview : item.m_preview;
    if (!key) {
      continue;
    }

    CarriedItem carried;
    carried.m_anchor = item.m_anchor;
    carried.m_foldState = item.m_foldState;
    // Only a rebased snapshot is worth carrying; otherwise the replay already
    // hands back the very same object.
    if (item.m_preview && item.m_preview != key) {
      carried.m_bound = item.m_preview;
    }

    m_carriedItems.insert(key.data(), carried);
  }

  removeAllItems();
  updatePreviews(m_revision, m_lastPreviews);
  m_carriedItems.clear();

  // The enabled type mask may have grown, in which case the replayed
  // generation has no snapshots for the newly enabled types. Ask the
  // highlighter to republish; it re-emits synchronously when its result is
  // already current.
  if (auto highlighter = m_editor ? m_editor->getHighlighter() : nullptr) {
    highlighter->updateHighlight();
  }
}

void InteractivePreviewHost::reconcileLater() {
  m_reconcilePending = true;
  scheduleOwedWork();
}

void InteractivePreviewHost::scheduleFoldRefresh() {
  m_foldRefreshPending = true;
  scheduleOwedWork();
}

void InteractivePreviewHost::scheduleOwedWork() {
  if (m_owedWorkScheduled || m_drainingOwedWork) {
    // Exactly one delivery is armed at a time, and a running drain arms its
    // own follow-up once it has finished its ordered pass.
    return;
  }

  if (!m_replacementRetryPending && !m_hasDeferredGeneration && !m_reconcilePending &&
      !m_publishPending && !m_geometrySyncPending && !m_scrollApplyPending &&
      !m_foldRefreshPending && !m_cursorLineSyncPending) {
    return;
  }

  // Never arm a bare zero timer while blocked: a widget callback may run a
  // real nested event loop, which would keep delivering each newly armed timer
  // while the block is held by a stack frame that loop is holding open. There
  // are exactly two unblock edges - the outermost BlockGuard and the layout's
  // becameIdle() - and both come back here.
  if (isBlocked()) {
    if (m_layout && m_layout->isBusy()) {
      m_layout->requestIdleNotification();
    }

    return;
  }

  m_owedWorkScheduled = true;
  QTimer::singleShot(0, this, [this]() {
    m_owedWorkScheduled = false;
    drainOwedWork();
  });
}

void InteractivePreviewHost::drainOwedWork() {
  if (isBlocked()) {
    // Delivered from inside a nested event loop a callback opened. An unblock
    // hook re-arms it; running here would mutate the document mid-pass.
    scheduleOwedWork();
    return;
  }

  setProperty(c_owedWorkDrainCountProperty, ++m_owedWorkDrainCount);

  {
    QScopedValueRollback<bool> guard(m_drainingOwedWork, true);
    runOwedWorkSteps();
  }

  // Whatever a step re-owed, or declined because a pass was running, gets its
  // own delivery - one, not one per owed item.
  scheduleOwedWork();
}

// The highest ranking category which is currently owed *and* runnable, or
// OwedWork::None. A category gated on m_reconciling does not count while a
// reconciliation pass is running: it cannot be run, so it must not hold the
// steps below it back either.
InteractivePreviewHost::OwedWork InteractivePreviewHost::highestOwedWork() const {
  if (m_replacementRetryPending) {
    return OwedWork::ReplacementRetry;
  }

  if (m_hasDeferredGeneration && !m_reconciling) {
    return OwedWork::DeferredGeneration;
  }

  if (m_reconcilePending && !m_reconciling) {
    return OwedWork::Reconcile;
  }

  if (m_publishPending) {
    return OwedWork::Publish;
  }

  if (m_geometrySyncPending) {
    return OwedWork::GeometrySync;
  }

  if (m_scrollApplyPending) {
    return OwedWork::ScrollApply;
  }

  if (m_foldRefreshPending && !m_reconciling) {
    return OwedWork::FoldRefresh;
  }

  if (m_cursorLineSyncPending) {
    return OwedWork::CursorLineSync;
  }

  return OwedWork::None;
}

// Whether the drain must stop before the step ranking at @p_step.
//
// Every step calls into code which can owe new work, and a step which raised
// something ranking above what is about to run has invalidated the rest of
// this scan: the ordering below is load-bearing, so the drain stops and the
// single follow-up delivery restarts it at the new highest debt. Stopping
// rather than looping in place is deliberate - a step which merely re-owes
// itself would otherwise spin synchronously.
bool InteractivePreviewHost::stopOwedWorkBefore(OwedWork p_step) const {
  if (isBlocked()) {
    return true;
  }

  return highestOwedWork() < p_step;
}

// The order is load-bearing: a replacement must settle before the reconcile
// which would remove its item, and the item set must settle before geometry
// and folding observe it.
void InteractivePreviewHost::runOwedWorkSteps() {
  if (m_replacementRetryPending) {
    m_replacementRetryPending = false;
    retryDeferredReplacements();
  }

  if (stopOwedWorkBefore(OwedWork::DeferredGeneration)) {
    return;
  }

  if (m_hasDeferredGeneration && !m_reconciling) {
    replayDeferredGeneration();
  }

  if (stopOwedWorkBefore(OwedWork::Reconcile)) {
    return;
  }

  if (m_reconcilePending && !m_reconciling) {
    m_reconcilePending = false;
    setProperty(c_reconcileDeliveryCountProperty, ++m_reconcileDeliveryCount);
    // A factory set change may make a different factory win, so rebuild.
    rebuildAll();
  }

  if (stopOwedWorkBefore(OwedWork::Publish)) {
    return;
  }

  if (m_publishPending) {
    publish();
  }

  if (stopOwedWorkBefore(OwedWork::GeometrySync)) {
    return;
  }

  if (m_geometrySyncPending) {
    // The wrappers own the flag bookkeeping - a full sync subsuming a pending
    // scroll apply, in particular. Their blocked check is a no-op here.
    syncWidgetGeometry();
  }

  if (stopOwedWorkBefore(OwedWork::ScrollApply)) {
    return;
  }

  if (m_scrollApplyPending) {
    applyScrollOffset();
  }

  if (stopOwedWorkBefore(OwedWork::FoldRefresh)) {
    return;
  }

  if (m_foldRefreshPending && !m_reconciling) {
    m_foldRefreshPending = false;
    setProperty(c_foldRefreshCountProperty, ++m_foldRefreshCount);
    if (m_editor) {
      m_editor->applyPreviewFolding();
    }
  }

  if (stopOwedWorkBefore(OwedWork::CursorLineSync)) {
    return;
  }

  if (m_cursorLineSyncPending) {
    m_cursorLineSyncPending = false;
    syncCursorLineToItem(m_cursorLineSyncId);
  }
}

void InteractivePreviewHost::replayDeferredGeneration() {
  Q_ASSERT(m_hasDeferredGeneration);

  // Newer than what the items hold, so it wins. The replay does its own
  // publish.
  m_hasDeferredGeneration = false;
  const quint64 revision = m_deferredRevision;
  const auto previews = m_deferredPreviews;
  m_deferredPreviews.clear();
  updatePreviews(revision, previews);
}

void InteractivePreviewHost::dropDeferredGeneration() {
  if (!m_hasDeferredGeneration) {
    return;
  }

  qCDebug(previewHostLog) << "dropped the stashed revision" << m_deferredRevision
                          << "- it describes source the document no longer holds";
  m_hasDeferredGeneration = false;
  m_deferredPreviews.clear();
}

void InteractivePreviewHost::retryDeferredReplacements() {
  const auto identities = m_deferredReplacements.keys();
  for (quint64 id : identities) {
    // Re-resolved on every iteration: a flush is reentrant and can tear items
    // down, and it can add entries here again.
    const auto attempt = m_deferredReplacements.constFind(id);
    if (attempt == m_deferredReplacements.constEnd()) {
      continue;
    }

    const auto it = m_items.constFind(id);
    QPointer<TablePreviewWidget> sheet =
        it == m_items.constEnd() ? nullptr
                                 : qobject_cast<TablePreviewWidget *>(it.value().m_widget.data());
    if (!sheet) {
      // Gone, or owned by an application-defined widget. A third-party
      // request is deliberately not replayed here: that would deliver two
      // completions for one logical request. The widget owns its own retry.
      m_deferredReplacements.remove(id);
      continue;
    }

    if (attempt.value() >= c_replacementRetryBudget) {
      qCWarning(previewReplaceLog)
          << "gave up retrying the postponed replacement of identity" << id << "after"
          << c_replacementRetryBudget << "attempts - the sheet keeps its edit and its debounce";
      // Only the host's retry bookkeeping is dropped: never the item, never
      // its authority, never its debounce.
      m_deferredReplacements.remove(id);
      continue;
    }

    m_deferredReplacements.insert(id, attempt.value() + 1);

    const auto outcome = sheet->flushNow();
    if (outcome == TablePreviewWidget::FlushOutcome::Deferred) {
      // Still not applicable. The entry stays, with the attempt spent.
      m_replacementRetryPending = true;
    } else {
      m_deferredReplacements.remove(id);
    }

    // Every block is stack-scoped RAII and this runs at depth 0, so a block
    // entered by the flush is unwound before it returns. Asserting it rather
    // than branching on it keeps a future non-RAII block from silently taking
    // an untested path.
    Q_ASSERT(!isBlocked());
  }
}

QVector<PreviewedRange> InteractivePreviewHost::previewedRanges() const {
  QVector<PreviewedRange> ranges;
  if (!m_doc) {
    return ranges;
  }

  const int limit = m_doc->characterCount() - 1;
  ranges.reserve(m_items.size());
  for (auto it = m_items.constBegin(); it != m_items.constEnd(); ++it) {
    const auto &item = it.value();
    if (!item.m_widget || !item.m_preview) {
      continue;
    }

    // A destructive edit over the source collapses the anchor, and the stale
    // item survives until the next parse removes it. Reporting an unvalidated
    // conversion of that would describe a range which is not there.
    if (item.m_anchor.isNull()) {
      continue;
    }

    const int start = item.m_anchor.selectionStart();
    const int end = item.m_anchor.selectionEnd();
    if (start < 0 || end <= start || end > limit) {
      continue;
    }

    const QTextBlock firstBlock = m_doc->findBlock(start);
    // The anchor spans [start, end), so the last character of the source is at
    // end - 1.
    const QTextBlock lastBlock = m_doc->findBlock(end - 1);
    if (!firstBlock.isValid() || !lastBlock.isValid() ||
        lastBlock.blockNumber() < firstBlock.blockNumber()) {
      continue;
    }

    PreviewedRange range;
    range.m_identity = item.m_id;
    range.m_startBlock = firstBlock.blockNumber();
    range.m_endBlock = lastBlock.blockNumber();
    range.m_type = item.m_preview->type();
    range.m_foldState = item.m_foldState;
    ranges.append(range);
  }

  return ranges;
}

void InteractivePreviewHost::setPreviewFoldStates(
    const QVector<QPair<quint64, PreviewFoldState>> &p_states) {
  for (const auto &state : p_states) {
    auto it = m_items.find(state.first);
    if (it == m_items.end()) {
      continue;
    }

    it.value().m_foldState = state.second;
  }
}


void InteractivePreviewHost::updatePreviews(
    quint64 p_revision, const QVector<QSharedPointer<const Preview>> &p_previews) {
  if (m_reconciling || isBlocked()) {
    // Re-entered from a nested event loop a widget callback opened, or
    // delivered from inside a layout pass. Stash the newest generation instead
    // of discarding it: nothing else would ever replay it, and every bound
    // snapshot would keep describing superseded source until the next document
    // edit produced another parse.
    //
    // The whole generation is deferred rather than the individual items it
    // would remove: a pass which cannot start is the structural reason
    // removeItem()'s mandatory flush is never answered with Deferred, and no
    // item is ever left in m_items in a "queued for removal" state.
    m_deferredRevision = p_revision;
    m_deferredPreviews = p_previews;
    m_hasDeferredGeneration = true;
    // m_hasDeferredGeneration is itself an owed-work item: the drain replays
    // it. Deliberately not m_reconcilePending, which means "a rebuild is
    // owed" - a deferred parse generation does not need every widget torn
    // down and recreated.
    scheduleOwedWork();
    qCDebug(previewHostLog) << "deferred revision" << p_revision << "-" << p_previews.size()
                            << "snapshot(s) delivered during a running pass";
    return;
  }

  m_reconciling = true;

  m_revision = p_revision;
  m_lastPreviews = p_previews;

  QVector<QSharedPointer<const Preview>> candidates;
  if (m_enabled) {
    candidates.reserve(p_previews.size());
    for (const auto &preview : p_previews) {
      if (preview && isTypeEnabled(preview->type())) {
        candidates.append(preview);
      }
    }
  }

  QSet<quint64> used;
  QVector<QPair<quint64, QSharedPointer<const Preview>>> matched;
  QVector<QSharedPointer<const Preview>> fresh;
  rebuildAnchorIndex();
  for (const auto &preview : candidates) {
    const quint64 id = findIdentity(preview, used);
    if (id) {
      used.insert(id);
      matched.append(qMakePair(id, preview));
    } else {
      fresh.append(preview);
    }
  }
  // Nothing below keeps the anchors still, so the index must not outlive the
  // matching loop.
  m_anchorIndex.clear();

  const auto existing = m_items.keys();
  int removed = 0;
  for (quint64 id : existing) {
    if (!used.contains(id)) {
      removeItem(id);
      ++removed;
    }
  }

  qCDebug(previewHostLog) << "reconcile revision" << p_revision << "-" << p_previews.size()
                          << "snapshot(s)," << candidates.size() << "enabled," << matched.size()
                          << "matched," << fresh.size() << "new," << removed << "removed (enabled"
                          << m_enabled << ")";

  for (const auto &pair : matched) {
    updateItem(pair.first, pair.second);
  }

  for (const auto &preview : fresh) {
    createItem(preview);
  }

  m_reconciling = false;

  // Replay a generation which arrived while this pass was running, unless a
  // postponed replacement outranks it: that replacement mutates the document,
  // which would make the stashed positions stale, and the reconcile the replay
  // performs would reach the flush that replacement still owes.
  if (m_hasDeferredGeneration && !m_replacementRetryPending) {
    replayDeferredGeneration();
    return;
  }

  // A registration or enablement change made during the pass is owed a
  // rebuild, and the owed-work machine declined to arm while m_reconciling was
  // true. The same holds for a fold refresh and for a replay left to the drain.
  scheduleOwedWork();

  qCDebug(previewHostLog) << "reconcile done -" << m_items.size() << "live item(s)";

  publish();
}

void InteractivePreviewHost::rebuildAnchorIndex() {
  m_anchorIndex.clear();
  m_anchorIndex.reserve(m_items.size());
  for (auto it = m_items.constBegin(); it != m_items.constEnd(); ++it) {
    const auto &item = it.value();
    m_anchorIndex[qMakePair(item.m_anchor.selectionStart(), item.m_anchor.selectionEnd())].append(
        item.m_id);
  }
}

quint64 InteractivePreviewHost::findIdentity(const QSharedPointer<const Preview> &p_preview,
                                             const QSet<quint64> &p_used) const {
  // Fast path: the anchor has been tracking every edit since the last
  // generation, so an element which merely moved with the text still reports
  // exactly its anchor's range. That is the overwhelmingly common case, and
  // resolving it by scanning every live item is what makes this quadratic.
  const auto exact =
      m_anchorIndex.constFind(qMakePair(p_preview->startPos(), p_preview->endPos()));
  if (exact != m_anchorIndex.constEnd()) {
    for (quint64 id : exact.value()) {
      if (p_used.contains(id)) {
        continue;
      }

      const auto item = m_items.constFind(id);
      // A type change never retains an identity.
      if (item == m_items.constEnd() || !item.value().m_preview ||
          item.value().m_preview->type() != p_preview->type()) {
        continue;
      }

      return id;
    }
  }

  quint64 best = 0;
  int bestOverlap = 0;
  int bestDistance = INT_MAX;
  bool ambiguous = false;

  for (auto it = m_items.constBegin(); it != m_items.constEnd(); ++it) {
    const auto &item = it.value();
    if (p_used.contains(item.m_id)) {
      continue;
    }

    // A type change never retains an identity.
    if (!item.m_preview || item.m_preview->type() != p_preview->type()) {
      continue;
    }

    const int start = item.m_anchor.selectionStart();
    const int end = item.m_anchor.selectionEnd();
    if (start == p_preview->startPos() && end == p_preview->endPos()) {
      return item.m_id;
    }

    const int overlap = qMin(end, p_preview->endPos()) - qMax(start, p_preview->startPos());
    if (overlap <= 0) {
      continue;
    }

    const int distance = qAbs(start - p_preview->startPos());
    if (overlap > bestOverlap) {
      bestOverlap = overlap;
      bestDistance = distance;
      best = item.m_id;
      ambiguous = false;
    } else if (overlap == bestOverlap) {
      if (distance < bestDistance) {
        bestDistance = distance;
        best = item.m_id;
        ambiguous = false;
      } else if (distance == bestDistance) {
        ambiguous = true;
      }
    }
  }

  // Allocate a new identity on ambiguous overlap instead of guessing.
  return ambiguous ? 0 : best;
}

QTextCursor InteractivePreviewHost::makeAnchor(int p_startPos, int p_endPos) const {
  // rebuildAll() replays a parse generation whose positions the document may
  // have outgrown since; every sibling consumer bound-checks, and setPosition()
  // would otherwise warn and leave a collapsed cursor behind, which the rest of
  // the host would treat as a live source range.
  const int limit = m_doc ? m_doc->characterCount() - 1 : -1;
  if (p_startPos < 0 || p_endPos <= p_startPos || p_endPos > limit) {
    return QTextCursor();
  }

  QTextCursor cursor(m_doc);
  cursor.setPosition(p_startPos);
  cursor.setPosition(p_endPos, QTextCursor::KeepAnchor);
  return cursor;
}

PreviewWidget *
InteractivePreviewHost::createWidgetFor(const QSharedPointer<const Preview> &p_preview,
                                        PreviewWidgetContext *p_context,
                                        PreviewWidgetFactory **p_usedFactory) {
  *p_usedFactory = nullptr;

  // Iterate over a snapshot: a factory may unregister itself (or a sibling)
  // from inside any of the callbacks below, which mutates m_factories.
  const auto factories = orderedFactories();
  for (const auto &entry : factories) {
    QPointer<PreviewWidgetFactory> factory = entry.m_factory;
    if (factory.isNull() || !isFactoryActive(factory.data())) {
      continue;
    }

    QVector<PreviewElementType> supported;
    {
      BlockGuard guard(this, BlockGuard::Reason::FactoryCallback);
      supported = factory->supportedTypes();
    }

    // The factory may have been deactivated or destroyed by its own callback.
    if (factory.isNull() || !isFactoryActive(factory.data()) ||
        !supported.contains(p_preview->type())) {
      continue;
    }

    QPointer<PreviewWidget> widget;
    {
      BlockGuard guard(this, BlockGuard::Reason::FactoryCallback);
      widget = factory->createWidget(p_context, p_preview, viewport());
    }

    if (widget.isNull()) {
      continue;
    }

    if (factory.isNull() || !isFactoryActive(factory.data())) {
      delete widget.data();
      continue;
    }

    bool widgetSupports = false;
    {
      BlockGuard guard(this, BlockGuard::Reason::FactoryCallback);
      widgetSupports = widget->supportedTypes().contains(p_preview->type());
    }

    if (widget.isNull()) {
      continue;
    }

    if (!widgetSupports || factory.isNull() || !isFactoryActive(factory.data())) {
      delete widget.data();
      continue;
    }

    bool accepted = false;
    {
      BlockGuard guard(this, BlockGuard::Reason::FactoryCallback);
      accepted = widget->setPreview(p_preview);
    }

    if (widget.isNull()) {
      continue;
    }

    if (!accepted || factory.isNull() || !isFactoryActive(factory.data())) {
      delete widget.data();
      continue;
    }

    *p_usedFactory = factory.data();
    return widget.data();
  }

  return nullptr;
}

void InteractivePreviewHost::createItem(const QSharedPointer<const Preview> &p_preview,
                                        PreviewFoldState p_carried) {
  auto *vp = viewport();
  if (!vp) {
    return;
  }

  // Nothing can render this element: leave it to the static painted path
  // instead of allocating a context and walking the factory chain per parse.
  if (!isTypeClaimable(p_preview->type())) {
    qCDebug(previewHostLog) << "no factory supports" << previewTypeName(p_preview->type())
                            << "- leaving it to the painted path";
    return;
  }

  // A rebuild reuses the state which has been tracking the document since the
  // parse generation: the widget must be bound to the source the anchor
  // actually spans, not to the superseded generation snapshot.
  //
  // Presence in m_carriedItems is meaningful in itself. A carried entry whose
  // cursor has collapsed means the source this item tracked was deleted; that
  // is not the same as having no carried state. Falling back to the replayed
  // generation's coordinates there would re-anchor the widget onto whatever
  // text now occupies those positions, which after a mid-document deletion can
  // still be perfectly in bounds.
  //
  // Both are resolved before the context and the widget are allocated, so
  // declining strands nothing.
  QSharedPointer<const Preview> bound = p_preview;
  QTextCursor anchor;
  bool carriedAnchor = false;
  bool carriedBinding = false;
  // A rebuild of the same logical element must not re-decide its initial fold
  // state. A carried map entry wins over the parameter; the two are
  // operationally disjoint, because m_carriedItems is only populated by
  // rebuildAll(), which removes every item before replaying.
  PreviewFoldState foldState = p_carried;

  const auto carried = m_carriedItems.constFind(p_preview.data());
  if (carried != m_carriedItems.constEnd()) {
    if (carried.value().m_bound) {
      bound = carried.value().m_bound;
      carriedBinding = true;
    }

    foldState = carried.value().m_foldState;

    anchor = carried.value().m_anchor;
    if (anchor.isNull() || anchor.selectionEnd() <= anchor.selectionStart()) {
      qCDebug(previewHostLog) << "the source carried across the rebuild is gone - leaving"
                              << previewTypeName(p_preview->type()) << "to the painted path";
      return;
    }
    carriedAnchor = true;
  } else {
    anchor = makeAnchor(p_preview->startPos(), p_preview->endPos());
    if (anchor.isNull()) {
      qCDebug(previewHostLog) << "no resolvable range for" << previewTypeName(p_preview->type())
                              << "at [" << p_preview->startPos() << "," << p_preview->endPos()
                              << ") - leaving it to the painted path";
      return;
    }
  }

  const quint64 id = m_nextIdentity++;

  auto context = new PreviewWidgetContext(id, this);
  context->setPreview(bound);
  connect(context, &PreviewWidgetContext::sourceReplacementRequested, this,
          &InteractivePreviewHost::handleSourceReplacementRequested);

  PreviewWidgetFactory *usedFactory = nullptr;
  PreviewWidget *widget = createWidgetFor(bound, context, &usedFactory);
  if (!widget) {
    // Nobody claims it: keep the static painted fallback (or source only).
    qCDebug(previewHostLog) << "no factory claimed" << previewTypeName(bound->type()) << "at ["
                            << bound->startPos() << "," << bound->endPos()
                            << ") - keeping the painted fallback";
    delete context;
    return;
  }

  widget->setParent(vp);
  // Measure and paint with the same font. See applyEditorFont().
  widget->setFont(editorFont());
  widget->hide();
  widget->installEventFilter(this);

  ActiveItem item;
  item.m_id = id;
  item.m_preview = bound;
  item.m_generationPreview = p_preview;
  item.m_context = context;
  item.m_widget = widget;
  item.m_factory = usedFactory;
  item.m_anchor = anchor;
  item.m_foldState = foldState;
  m_items.insert(id, item);

  qCDebug(previewHostLog) << "created item" << id << previewTypeName(bound->type()) << "at ["
                          << item.m_anchor.selectionStart() << ","
                          << item.m_anchor.selectionEnd() << ") widget"
                          << widget->metaObject()->className() << "factory"
                          << (usedFactory ? usedFactory->metaObject()->className() : "?")
                          << (carriedBinding
                                  ? "(rebuilt, carried the rebased source)"
                                  : (carriedAnchor ? "(rebuilt, carried the live anchor)" : ""));
}

void InteractivePreviewHost::updateItem(quint64 p_id,
                                        const QSharedPointer<const Preview> &p_preview) {
  auto it = m_items.find(p_id);
  if (it == m_items.end()) {
    return;
  }

  auto &item = it.value();

  // Re-delivery of the very same generation carries no new information, and
  // rebuildAll() forces one through updateHighlight(). Rebinding to it would
  // resurrect the source an accepted replacement already superseded and
  // re-anchor at that generation's stale positions.
  const bool sameGeneration = item.m_generationPreview == p_preview;
  const bool rebased = item.m_preview && item.m_preview != item.m_generationPreview;
  const bool keepBinding = sameGeneration && rebased;

  const auto bound = keepBinding ? item.m_preview : p_preview;

  // Resolve the new anchor before rebinding anything: a replayed generation
  // can describe a range the document has outgrown, and rebinding first would
  // leave the item quoting the new source over a collapsed range.
  QTextCursor anchor = item.m_anchor;
  if (!keepBinding) {
    anchor = makeAnchor(p_preview->startPos(), p_preview->endPos());
    if (anchor.isNull()) {
      qCDebug(previewHostLog) << "item" << p_id << previewTypeName(p_preview->type())
                              << "has no resolvable range at [" << p_preview->startPos() << ","
                              << p_preview->endPos() << ") - rebuilding it";
      // The rebuild is of the *same* logical element, so its settled fold
      // state is carried across it.
      const PreviewFoldState foldState = item.m_foldState;
      removeItem(p_id);
      createItem(p_preview, foldState);
      return;
    }
  }

  item.m_preview = bound;
  item.m_generationPreview = p_preview;
  item.m_anchor = anchor;

  if (item.m_context) {
    item.m_context->setPreview(bound);
  }

  // Update in place to preserve focus and editing state. The widget may
  // destroy itself or its factory from inside the callback, so re-validate
  // everything afterwards.
  QPointer<PreviewWidget> widget = item.m_widget;
  QPointer<PreviewWidgetFactory> factory = item.m_factory;
  bool accepted = false;
  if (!widget.isNull()) {
    BlockGuard guard(this, BlockGuard::Reason::FactoryCallback);
    accepted = widget->setPreview(bound);
  }

  if (accepted && !widget.isNull() && !factory.isNull() && isFactoryActive(factory.data())) {
    // The callback may have torn this item down. Identities are allocated
    // monotonically and never reused, so re-resolving by id is exact.
    it = m_items.find(p_id);
    if (it == m_items.end()) {
      return;
    }

    auto &live = it.value();
    // The bound snapshot changed, so the cached measurement is stale.
    live.m_measureDirty = true;
    qCDebug(previewHostLog) << "updated item" << p_id << previewTypeName(bound->type())
                            << "in place at [" << live.m_anchor.selectionStart() << ","
                            << live.m_anchor.selectionEnd() << ")"
                            << (keepBinding ? "- kept the rebased binding" : "");
    return;
  }

  // The instance refused the snapshot or vanished: rebuild through the
  // remaining factory chain, falling back to static rendering if none claims.
  qCDebug(previewHostLog) << "item" << p_id << previewTypeName(p_preview->type())
                          << "refused the snapshot or vanished - rebuilding it";
  // Rebuilding the renderer of an existing element is not a new initial
  // decision, so its settled fold state is carried across. Re-resolved by id
  // because the callback above may have torn the item down and a nested
  // refresh may have written a newer state onto it.
  PreviewFoldState foldState = PreviewFoldState::Undecided;
  {
    const auto live = m_items.constFind(p_id);
    if (live != m_items.constEnd()) {
      foldState = live.value().m_foldState;
    }
  }
  removeItem(p_id);
  createItem(p_preview, foldState);
}

void InteractivePreviewHost::removeItem(quint64 p_id) { removeItem(p_id, false); }

void InteractivePreviewHost::removeItem(quint64 p_id, bool p_synchronous) {
  // A removal flushes the sheet's owed write-back, and that flush is
  // mandatory: it is the last point at which the identity is still
  // authoritative. It may therefore never be answered with Deferred, which is
  // guaranteed structurally - updatePreviews() and rebuildAll() both decline
  // to start a pass while blocked, so every caller reaches here on a clean
  // stack. Destroying the host from inside a layout pass or a widget callback
  // is a programming error, and this is where it is caught.
  Q_ASSERT(!isBlocked());

  auto it = m_items.find(p_id);
  if (it == m_items.end()) {
    return;
  }

  // A sheet may still owe a debounced write-back, and its context, its
  // snapshot and its anchor are only authoritative while the identity is in
  // m_items. Flushing after the erase below would be rejected as an
  // UnknownIdentity and the edit silently lost - and the focus move further
  // down would trigger exactly that flush. So: flush first, then revoke, so a
  // late timeout, the hide, the focus-out and the deferred deletion all become
  // no-ops.
  QPointer<TablePreviewWidget> sheet =
      qobject_cast<TablePreviewWidget *>(it.value().m_widget.data());
  if (sheet) {
    const auto outcome = sheet->flushNow();
    // Mandatory: see the assertion at the top of this function.
    Q_ASSERT(outcome != TablePreviewWidget::FlushOutcome::Deferred);
    Q_UNUSED(outcome);
    if (sheet) {
      sheet->revokeAuthority();
    }

    // The flush applies a document edit and calls schedulePublish(), so it can
    // re-enter this host and tear the very item down. Identities are allocated
    // monotonically and never reused, so re-resolving by id is exact.
    it = m_items.find(p_id);
    if (it == m_items.end()) {
      return;
    }
  }

  ActiveItem item = it.value();
  m_items.erase(it);

  qCDebug(previewHostLog) << "removing item" << p_id
                          << (item.m_preview ? previewTypeName(item.m_preview->type()) : "?")
                          << (p_synchronous ? "(synchronously)" : "(deferred)");

  if (item.m_widget) {
    // Move focus back to the editor before destroying a focused widget.
    QWidget *focus = QApplication::focusWidget();
    if (m_textEdit && focus &&
        (focus == item.m_widget.data() || item.m_widget->isAncestorOf(focus))) {
      m_textEdit->setFocus();
    }

    item.m_widget->hide();
  }

  if (p_synchronous) {
    // Destroy the widget before its context so a widget destructor can still
    // reach the context.
    delete item.m_widget.data();
    delete item.m_context.data();
    return;
  }

  if (item.m_widget) {
    item.m_widget->setParent(nullptr);
    item.m_widget->deleteLater();
  }

  if (item.m_context) {
    item.m_context->deleteLater();
  }

  // Remember the pair: if the editor goes away before the deferred deletion is
  // delivered, the destructor must still tear it down.
  if (!item.m_widget.isNull() || !item.m_context.isNull()) {
    PendingDeletion pending;
    pending.m_widget = item.m_widget;
    pending.m_context = item.m_context;
    m_pendingDeletions.append(pending);
  }

  prunePendingDeletions();
}

void InteractivePreviewHost::prunePendingDeletions() {
  for (int i = m_pendingDeletions.size() - 1; i >= 0; --i) {
    if (m_pendingDeletions[i].m_widget.isNull() && m_pendingDeletions[i].m_context.isNull()) {
      m_pendingDeletions.removeAt(i);
    }
  }
}

void InteractivePreviewHost::flushPendingDeletions() {
  const auto pending = m_pendingDeletions;
  m_pendingDeletions.clear();
  for (const auto &item : pending) {
    delete item.m_widget.data();
    delete item.m_context.data();
  }
}

void InteractivePreviewHost::removeAllItems(bool p_synchronous) {
  const auto ids = m_items.keys();
  for (quint64 id : ids) {
    removeItem(id, p_synchronous);
  }
}

// ---------------------------------------------------------------------------
// Sheet relays
// ---------------------------------------------------------------------------

quint64 InteractivePreviewHost::identityOf(const PreviewWidget *p_widget) const {
  if (!p_widget) {
    return 0;
  }

  for (auto it = m_items.constBegin(); it != m_items.constEnd(); ++it) {
    if (it.value().m_widget.data() == p_widget) {
      return it.value().m_id;
    }
  }

  return 0;
}

quint64 InteractivePreviewHost::identityForFocusWidget(QWidget *p_focus) const {
  if (!p_focus) {
    return 0;
  }

  for (auto it = m_items.constBegin(); it != m_items.constEnd(); ++it) {
    QWidget *widget = it.value().m_widget.data();
    if (widget && (widget == p_focus || widget->isAncestorOf(p_focus))) {
      return it.value().m_id;
    }
  }

  return 0;
}

void InteractivePreviewHost::clearPreviewSelections() {
  // Snapshot first: clearSelection() is application-defined and may destroy
  // widgets or mutate m_items, so no iterator may outlive the callback.
  QVector<QPointer<PreviewWidget>> widgets;
  widgets.reserve(m_items.size());
  for (auto it = m_items.constBegin(); it != m_items.constEnd(); ++it) {
    if (it.value().m_widget) {
      widgets.append(it.value().m_widget);
    }
  }

  if (widgets.isEmpty()) {
    return;
  }

  // A public extension point, like supportedTypes() and setPreview(): the
  // callback may request a source replacement, unregister a factory, delete
  // itself or spin a nested event loop.
  BlockGuard guard(this, BlockGuard::Reason::FactoryCallback);
  for (const auto &widget : widgets) {
    if (widget) {
      widget->clearSelection();
    }
  }
}

void InteractivePreviewHost::handleApplicationFocusChanged(QWidget *p_old, QWidget *p_now) {
  Q_UNUSED(p_old);

  const quint64 id = identityForFocusWidget(p_now);
  if (id == 0) {
    // The editor, another window, or nothing at all. handleFocusEscape() lands
    // here too, and must not have its caret moved back afterwards.
    m_focusedItemId = 0;
    if (m_textEdit && p_now && (p_now == m_textEdit || m_textEdit->isAncestorOf(p_now))) {
      clearPreviewSelections();
    }
    return;
  }

  if (id == m_focusedItemId) {
    // Focus only moved inside the same preview widget.
    return;
  }

  m_focusedItemId = id;

  // Owed, never posted directly: a bare queued invocation would bypass the
  // blocked-aware scheduler and could run inside a nested event loop a factory
  // or geometry callback opened.
  m_cursorLineSyncPending = true;
  m_cursorLineSyncId = id;
  scheduleOwedWork();
}

void InteractivePreviewHost::syncCursorLineToItem(quint64 p_id) {
  if (!m_textEdit || !m_doc || p_id == 0) {
    return;
  }

  // Identities are monotonic and never reused, so the lookup is exact even
  // though the item set may have changed since the focus arrived.
  if (p_id != m_focusedItemId) {
    return;
  }

  const auto it = m_items.constFind(p_id);
  if (it == m_items.constEnd()) {
    return;
  }

  QWidget *widget = it.value().m_widget.data();
  if (!widget) {
    return;
  }

  QWidget *focus = QApplication::focusWidget();
  if (!focus || (focus != widget && !widget->isAncestorOf(focus))) {
    return;
  }

  // Resolved from the live anchor, and validated exactly like
  // previewedRanges(): the deferral means the document may have changed
  // between the focus and this drain.
  const QTextCursor &anchor = it.value().m_anchor;
  if (anchor.isNull()) {
    return;
  }

  const int start = anchor.selectionStart();
  const int end = anchor.selectionEnd();
  const int limit = m_doc->characterCount() - 1;
  if (start < 0 || end <= start || end > limit) {
    return;
  }

  const QTextBlock firstBlock = m_doc->findBlock(start);
  const QTextBlock lastBlock = m_doc->findBlock(end - 1);
  if (!firstBlock.isValid() || !lastBlock.isValid() ||
      lastBlock.blockNumber() < firstBlock.blockNumber()) {
    return;
  }

  if (!firstBlock.isVisible()) {
    // VTextEdit::handleCursorPositionChange() would relocate the caret out of a
    // folded block, which is worse than leaving it alone.
    return;
  }

  const int currentBlock = m_textEdit->textCursor().blockNumber();
  if (currentBlock >= firstBlock.blockNumber() && currentBlock <= lastBlock.blockNumber()) {
    // Already inside the source range - a deliberate caret must not be moved.
    return;
  }

  QScrollBar *vbar = m_textEdit->verticalScrollBar();
  QScrollBar *hbar = m_textEdit->horizontalScrollBar();
  const int vvalue = vbar ? vbar->value() : 0;
  const int hvalue = hbar ? hbar->value() : 0;
  const QPair<int, int> vrange =
      vbar ? qMakePair(vbar->minimum(), vbar->maximum()) : qMakePair(0, 0);
  const QPair<int, int> hrange =
      hbar ? qMakePair(hbar->minimum(), hbar->maximum()) : qMakePair(0, 0);

  {
    // Both bars' valueChanged run applyScrollOffset(), which hides previews
    // outside the transient viewport; hiding the focused sheet would drop its
    // focus and trigger a write-back. QTextEdit::setTextCursor() ensures the
    // cursor is visible, so the intermediate value is observable.
    QSignalBlocker vblocker(vbar);
    QSignalBlocker hblocker(hbar);

    QTextCursor cursor = m_textEdit->textCursor();
    cursor.setPosition(firstBlock.position());
    m_textEdit->setTextCursor(cursor);

    // Restoration is only attempted here. A range which moved under the
    // blockers can stop setValue() landing on the saved value at all, and
    // everything the blockers swallowed is replayed once they are gone.
    if (vbar) {
      vbar->setValue(vvalue);
    }
    if (hbar) {
      hbar->setValue(hvalue);
    }
  }

  // Everything below is the repair for what the blockers swallowed.
  //
  // setTextCursor() reveals the caret, which relayouts, so a viewport which
  // has just grown shrinks the maximum. That range change is suppressed here,
  // and so is the clamping of the restore above it: setValue() cannot go back
  // to a value the new range no longer holds. Nothing observed either, because
  // a blocked bar emits neither rangeChanged nor valueChanged.
  //
  // The range goes first: ScrollBar answers rangeChanged by extending the
  // maximum, and the settled value has to be reported against the range that
  // extension leaves behind.
  const int vsettled = vbar ? vbar->value() : 0;
  const int hsettled = hbar ? hbar->value() : 0;

  if (vbar && qMakePair(vbar->minimum(), vbar->maximum()) != vrange) {
    resyncScrollBarRange(vbar);
  }
  if (hbar && qMakePair(hbar->minimum(), hbar->maximum()) != hrange) {
    resyncScrollBarRange(hbar);
  }

  // QAbstractScrollArea learns a new offset only from valueChanged, and
  // applyScrollOffset() - which places every preview widget - hangs off the
  // same signal. Left unreported, the scroll area's cached offset, the widgets
  // and the text QTextEdit paints translated by the bar's current value sit on
  // different offsets, and the previews are drawn over the source until an
  // unrelated scroll happens to resync them.
  //
  // Skipped when the range replay above moved the value itself: that change
  // was not blocked, so it has already been reported.
  if (vbar && vbar->value() == vsettled && vsettled != vvalue) {
    qCDebug(previewHostLog) << "cursor line sync settled the vertical scroll at" << vsettled
                            << "instead of" << vvalue << "- the range moved under the restore";
    resyncScrollBar(vbar);
  }
  if (hbar && hbar->value() == hsettled && hsettled != hvalue) {
    qCDebug(previewHostLog) << "cursor line sync settled the horizontal scroll at" << hsettled
                            << "instead of" << hvalue << "- the range moved under the restore";
    resyncScrollBar(hbar);
  }

  qCDebug(previewHostLog) << "item" << p_id << "took the focus, moved the cursor line to block"
                          << firstBlock.blockNumber();
}

void InteractivePreviewHost::resyncScrollBar(QScrollBar *p_bar) {
  if (!p_bar) {
    return;
  }

  // Report the value the bar actually settled on to the connections which
  // missed it while its signals were blocked: QAbstractScrollArea caches its
  // own offset in that slot, and applyScrollOffset() places every preview
  // widget from it.
  //
  // The signal is invoked rather than driven by stepping the value off and
  // back on. Two real transitions would publish an intermediate position, and
  // a preview sitting on the viewport edge can fall outside it there and be
  // hidden - which drops the focus out of a sheet and makes it write its
  // pending edit back, the very thing the blockers exist to prevent. It also
  // reports the correct state when the range collapsed to a single value, in
  // which case no transition is representable at all.
  QMetaObject::invokeMethod(p_bar, "valueChanged", Qt::DirectConnection,
                            Q_ARG(int, p_bar->value()));
}

void InteractivePreviewHost::resyncScrollBarRange(QScrollBar *p_bar) {
  if (!p_bar) {
    return;
  }

  // Same reasoning as resyncScrollBar(), for the range. VTextEdit's ScrollBar
  // answers rangeChanged by extending the maximum so the bottom of the content
  // can still be scrolled up; a range set while the bar was blocked never gets
  // that extension, and nothing revisits it until the range changes again.
  QMetaObject::invokeMethod(p_bar, "rangeChanged", Qt::DirectConnection,
                            Q_ARG(int, p_bar->minimum()), Q_ARG(int, p_bar->maximum()));
}

void InteractivePreviewHost::handleFocusEscape(TablePreviewWidget *p_widget,
                                               FocusEscapeDirection p_direction) {
  if (!m_textEdit) {
    return;
  }

  const quint64 id = identityOf(p_widget);
  const auto it = id ? m_items.constFind(id) : m_items.constEnd();
  bool movedCaret = false;
  if (it != m_items.constEnd() && p_direction != FocusEscapeDirection::Keep) {
    // Resolved from the live anchor, never from the snapshot's own start/end:
    // those describe the parse generation the sheet was bound in, which any
    // unrelated edit before the table has already moved.
    const int start = it.value().m_anchor.selectionStart();
    const int end = it.value().m_anchor.selectionEnd();
    if (start >= 0 && end > start) {
      // The source is rendered above the sheet
      // (PreviewPlacement::BlockAfterSource), so leaving upwards lands at the
      // end of the source and leaving downwards just past its line.
      const int limit = m_doc ? m_doc->characterCount() - 1 : end;
      const int target =
          p_direction == FocusEscapeDirection::Up ? end : qMin(end + 1, qMax(end, limit));

      QTextCursor cursor = m_textEdit->textCursor();
      cursor.setPosition(qBound(0, target, qMax(0, limit)));
      m_textEdit->setTextCursor(cursor);
      movedCaret = true;

      qCDebug(previewHostLog) << "item" << id << "handed the caret back to the editor at"
                              << cursor.position();
    }
  }

  m_textEdit->setFocus();

  if (movedCaret) {
    // The caret was moved above while the sheet still owned the focus, so
    // checkCenterCursor() declined then. Now that the editor has it back,
    // honour the configured centering. Escape (FocusEscapeDirection::Keep)
    // moves no caret, so it must not scroll anything either.
    m_textEdit->checkCenterCursor();
  }
}

void InteractivePreviewHost::handleSheetUndo(TablePreviewWidget *p_widget) {
  Q_UNUSED(p_widget);
  if (m_textEdit) {
    m_textEdit->undo();
  }
}

void InteractivePreviewHost::handleSheetRedo(TablePreviewWidget *p_widget) {
  Q_UNUSED(p_widget);
  if (m_textEdit) {
    m_textEdit->redo();
  }
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

void InteractivePreviewHost::schedulePublish() {
  // QTextDocument::contentsChanged reaches this synchronously from inside a
  // document edit, i.e. from inside the layout pass that edit drives, so it
  // goes through the owed-work machine like everything else.
  m_publishPending = true;
  scheduleOwedWork();
}

QSizeF InteractivePreviewHost::preferredSize(PreviewWidget *p_widget, PreviewPlacement p_placement,
                                             int p_startPos, int p_endPos) const {
  const qreal maxWidth = m_layout->availableContentWidth();

  // Measure at the width the widget will actually be assigned, otherwise a
  // wrapping widget reserves a height it cannot honor.
  qreal width = -1;
  if (p_placement == PreviewPlacement::InlineAboveLine) {
    const qreal spanWidth = m_layout->inlinePlacementWidth(p_startPos, p_endPos);
    if (spanWidth > 0) {
      width = spanWidth;
    }
  }

  const QSize hint = p_widget->sizeHint();
  if (width < 0) {
    width = hint.width();
    // Only a preview on its own band may ask to span a share of the text
    // column, so a short table does not render as a small box at the margin.
    // The placement is what decides that, not the missing width above: an
    // inline span whose width could not be resolved lands here too, and
    // widening it would measure a height for a width the inline layout is
    // never going to assign.
    if (p_placement == PreviewPlacement::BlockAfterSource) {
      const qreal fraction = qBound<qreal>(0, p_widget->preferredWidthFraction(), 1);
      if (fraction > 0 && maxWidth > 0) {
        width = qMax(width, maxWidth * fraction);
      }
    }
  }

  if (maxWidth > 0) {
    width = qMin(width, maxWidth);
  }
  width = qMax<qreal>(0, width);

  qreal height = hint.height();
  if (p_widget->sizePolicy().hasHeightForWidth() && width > 0) {
    height = p_widget->heightForWidth(qRound(width));
  }

  return QSizeF(width, qMax<qreal>(0, height));
}

void InteractivePreviewHost::applyReadOnly() {
  if (!m_tableFactory) {
    return;
  }

  const bool readOnly = !m_textEdit || m_textEdit->isReadOnly();
  // This runs on every publish and every geometry sync, so only report the
  // transitions.
  if (readOnly != m_lastLoggedReadOnly) {
    m_lastLoggedReadOnly = readOnly;
    qCDebug(previewHostLog) << "editor is now" << (readOnly ? "read-only" : "writable");
  }

  m_tableFactory->setReadOnly(readOnly);
}

QFont InteractivePreviewHost::editorFont() const {
  QFont font = m_textEdit ? m_textEdit->font() : QApplication::font();

  auto theme = m_editor ? m_editor->theme() : nullptr;
  if (!theme) {
    return font;
  }

  // The generic text style, which MarkdownEditorConfig::overrideTextStyle()
  // has already replaced with the Markdown variant where the theme defines
  // one. This is the same style the editor itself is rendered with.
  const auto &fmt = theme->editorStyle(Theme::EditorStyle::Text);

  // Mirror VTextEditor::updateFromConfig(): the theme has already selected one
  // family, and that same value is what the editor's style sheet declares.
  // Deriving it a second way here is how the measured font and the painted
  // font drift apart.
  if (!fmt.m_fontFamily.isEmpty()) {
    font.setFamily(fmt.m_fontFamily);
  }

  // Follow the editor's zoom instead of the theme's base size.
  const int pointSize = m_editor->editorFontPointSize();
  if (pointSize > 0) {
    font.setPointSize(pointSize);
  } else if (fmt.m_fontPointSize > 0) {
    font.setPointSize(fmt.m_fontPointSize);
  }

  return font;
}

void InteractivePreviewHost::applyEditorFont() {
  if (!m_textEdit) {
    return;
  }

  const QFont font = editorFont();
  for (auto it = m_items.begin(); it != m_items.end(); ++it) {
    auto &item = it.value();
    if (item.m_widget.isNull() || item.m_widget->font() == font) {
      continue;
    }

    qCDebug(previewHostLog) << "pushing the editor font" << font.families() << font.pointSize()
                            << "onto item" << item.m_id;
    item.m_widget->setFont(font);
    // Everything about the measurement is font derived.
    item.m_measureDirty = true;
  }
}

void InteractivePreviewHost::publish() {
  // Whatever asked for a publication has been served by this run.
  m_publishPending = false;

  applyReadOnly();

  QVector<TextDocumentLayout::WidgetPreviewSpec> specs;
  QVector<TextDocumentLayout::PreviewClaim> claims;
  specs.reserve(m_items.size());
  claims.reserve(m_items.size());

  const qreal availableWidth = m_layout->availableContentWidth();

  for (auto it = m_items.begin(); it != m_items.end(); ++it) {
    auto &item = it.value();
    if (!item.m_widget || !item.m_preview) {
      continue;
    }

    const int start = item.m_anchor.selectionStart();
    const int end = item.m_anchor.selectionEnd();
    if (start < 0 || end <= start) {
      continue;
    }

    TextDocumentLayout::WidgetPreviewSpec spec;
    spec.m_id = item.m_id;
    spec.m_startPos = start;
    spec.m_endPos = end;
    spec.m_placement = item.m_preview->placement();
    spec.m_typeOrder = typeOrder(item.m_preview->type());

    // Re-measuring is expensive, so only do it when the widget asked for a new
    // layout or the width it will be given changed.
    qreal widthBasis = availableWidth;
    if (spec.m_placement == PreviewPlacement::InlineAboveLine) {
      const qreal spanWidth = m_layout->inlinePlacementWidth(start, end);
      if (spanWidth > 0) {
        widthBasis = availableWidth > 0 ? qMin(spanWidth, availableWidth) : spanWidth;
      }
    }

    if (item.m_measureDirty || !qFuzzyCompare(item.m_measuredWidthBasis + 1, widthBasis + 1)) {
      item.m_measuredSize = preferredSize(item.m_widget.data(), spec.m_placement, start, end);
      // Store the input constraint, which is what the guard is keyed on. The
      // width preferredSize() derives is the widget's own, so caching that
      // instead would never compare equal and the measurement would re-run on
      // every publish.
      item.m_measuredWidthBasis = widthBasis;
      item.m_measureDirty = false;

      qCDebug(previewLayoutLog)
          << "measured item" << item.m_id << previewTypeName(item.m_preview->type()) << "->"
          << item.m_measuredSize << "at width basis" << widthBasis;
    }

    spec.m_width = item.m_measuredSize.width();
    spec.m_height = item.m_measuredSize.height();
    specs.append(spec);

    TextDocumentLayout::PreviewClaim claim;
    claim.m_startPos = start;
    claim.m_endPos = end;
    claim.m_type = item.m_preview->type();
    claims.append(claim);
  }

  std::sort(claims.begin(), claims.end());
  std::sort(specs.begin(), specs.end(),
            [](const TextDocumentLayout::WidgetPreviewSpec &p_a,
               const TextDocumentLayout::WidgetPreviewSpec &p_b) { return p_a.m_id < p_b.m_id; });

  // Establish claims before the reservation relayout so no element is ever
  // rendered twice.
  qCDebug(previewLayoutLog) << "publishing" << specs.size() << "reservation(s) and"
                            << claims.size() << "claim(s), available width" << availableWidth;
  for (const auto &spec : specs) {
    qCDebug(previewLayoutLog) << "  spec" << spec.m_id << "[" << spec.m_startPos << ","
                              << spec.m_endPos << ") placement"
                              << previewPlacementName(spec.m_placement) << "size" << spec.m_width
                              << "x" << spec.m_height;
  }

  m_geometrySynced = false;
  m_layout->setPreviewClaims(claims);
  m_layout->setWidgetPreviews(specs);

  // Both relayouts emit widgetPreviewGeometryChanged when anything moved,
  // which is connected directly and has therefore already run the sync. Only
  // the unchanged case still needs one, and that is the case where a full
  // second pass would be pure waste.
  if (!m_geometrySynced) {
    syncWidgetGeometry();
  }

  // The previews the layout now holds are the ones the fold decision is made
  // from, so this is the point where a re-evaluation is owed.
  scheduleFoldRefresh();
}

void InteractivePreviewHost::syncWidgetGeometry() {
  if (isBlocked()) {
    // Deliberately without setting m_geometrySynced: publish()'s synchronous
    // fallback has to still run once the pass it is nested in has unwound,
    // otherwise no geometry would be applied for this publication at all.
    m_geometrySyncPending = true;
    qCDebug(previewLayoutLog) << "geometry sync deferred, layout busy" << m_layout->isBusy()
                              << "block depth" << m_blockDepth;
    scheduleOwedWork();
    return;
  }

  m_geometrySyncPending = false;
  // A full sync ends in a scroll apply of its own.
  m_scrollApplyPending = false;
  syncWidgetGeometryImpl();
}

void InteractivePreviewHost::syncWidgetGeometryImpl() {
  auto *vp = viewport();
  if (!vp) {
    return;
  }

  // setGeometryContext() below is delivered synchronously to application code,
  // which may request a source replacement from it while no layout pass is
  // running. That request has to be postponed too, and this is the only edge
  // which marks it.
  BlockGuard guard(this, BlockGuard::Reason::GeometryApply);

  m_geometrySynced = true;

  const qreal availableWidth = m_layout->availableContentWidth();
  applyReadOnly();

  qCDebug(previewLayoutLog) << "syncing" << m_items.size() << "item(s), available width"
                            << availableWidth << "enabled" << m_enabled;

  for (auto it = m_items.begin(); it != m_items.end(); ++it) {
    auto &item = it.value();
    if (!item.m_widget) {
      continue;
    }

    const QRectF docRect = m_layout->widgetPreviewRect(item.m_id);
    item.m_documentRect = m_enabled ? docRect : QRectF();

    if (item.m_context) {
      if (item.m_documentRect.isNull()) {
        item.m_context->setGeometryContext(QRectF(), QRectF(), vp->size(), QRectF());
      } else {
        const int start = item.m_anchor.selectionStart();
        const int end = item.m_anchor.selectionEnd();
        const QRectF available(docRect.left(), docRect.top(),
                               availableWidth > 0 ? availableWidth : docRect.width(),
                               docRect.height());

        // See ActiveItem::m_sourceTextRect: this union over every visual line
        // of the source is the expensive part of the pass, and it can only
        // have moved if the reserved band or the tracked range moved.
        if (start != item.m_sourceRectStart || end != item.m_sourceRectEnd ||
            docRect != item.m_sourceRectBand) {
          item.m_sourceTextRect = m_layout->sourceTextRect(start, end);
          item.m_sourceRectStart = start;
          item.m_sourceRectEnd = end;
          item.m_sourceRectBand = docRect;
        }

        item.m_context->setGeometryContext(item.m_sourceTextRect, available, vp->size(), docRect);

        // The band is reserved outside the source text, so any real overlap
        // means the published rect and the block offsets disagree - which is
        // exactly what "the preview is painted on top of the markdown" looks
        // like. Reported unconditionally because it is never expected.
        const QRectF overlap = item.m_sourceTextRect.intersected(docRect);
        if (overlap.width() > 1 && overlap.height() > 1) {
          qCWarning(previewLayoutLog)
              << "preview" << item.m_id << previewTypeName(item.m_preview->type())
              << "overlaps its own source text by" << overlap << ": widget at" << docRect
              << "source at" << item.m_sourceTextRect;
        }
      }
    }

    qCDebug(previewLayoutLog) << "  synced item" << item.m_id << "doc rect" << item.m_documentRect
                              << "source rect" << item.m_sourceTextRect;
  }

  applyScrollOffsetImpl();
}

void InteractivePreviewHost::applyScrollOffset() {
  if (isBlocked()) {
    m_scrollApplyPending = true;
    qCDebug(previewLayoutLog) << "scroll apply deferred, layout busy" << m_layout->isBusy()
                              << "block depth" << m_blockDepth;
    scheduleOwedWork();
    return;
  }

  m_scrollApplyPending = false;
  applyScrollOffsetImpl();
}

void InteractivePreviewHost::applyScrollOffsetImpl() {
  auto *vp = viewport();
  if (!vp) {
    return;
  }

  // hide(), show() and setGeometry() are delivered synchronously to the
  // widgets, and hiding a focused sheet makes it write its pending edit back
  // from inside this call.
  BlockGuard guard(this, BlockGuard::Reason::GeometryApply);

  // Everything below is scroll dependent only; the document rectangles were
  // computed by the last syncWidgetGeometry().
  const int hScroll = m_textEdit && m_textEdit->horizontalScrollBar()
                          ? m_textEdit->horizontalScrollBar()->value()
                          : 0;
  const int vScroll = m_textEdit && m_textEdit->verticalScrollBar()
                          ? m_textEdit->verticalScrollBar()->value()
                          : 0;
  const QRect viewportRect(QPoint(0, 0), vp->size());

  for (auto it = m_items.begin(); it != m_items.end(); ++it) {
    auto &item = it.value();
    if (!item.m_widget) {
      continue;
    }

    if (item.m_documentRect.isNull()) {
      // Folded, disabled or not laid out: keep the widget allocated but
      // hidden off-screen.
      item.m_widget->hide();
      continue;
    }

    const QRect targetRect = item.m_documentRect.translated(-hScroll, -vScroll).toAlignedRect();
    item.m_widget->setGeometry(targetRect);

    qCDebug(previewLayoutLog) << "  placed item" << item.m_id << "doc" << item.m_documentRect
                              << "scroll" << hScroll << vScroll << "-> viewport" << targetRect
                              << (targetRect.intersects(viewportRect) ? "visible" : "off-screen");

    if (targetRect.intersects(viewportRect)) {
      item.m_widget->show();
    } else {
      item.m_widget->hide();
    }
  }
}

bool InteractivePreviewHost::eventFilter(QObject *p_obj, QEvent *p_event) {
  if (p_obj == m_textEdit) {
    // QTextEdit::setReadOnly() emits no signal and touches no document, so
    // nothing else would ever re-run the read-only push. Without this a sheet
    // keeps its edit triggers for the whole read-only session and silently
    // swallows edits the host then rejects.
    if (p_event->type() == QEvent::ReadOnlyChange) {
      applyReadOnly();
      applyEditorFont();
      return QObject::eventFilter(p_obj, p_event);
    }

    // A theme change repaints the previews with a new font, so they have to be
    // re-measured against it.
    if (p_event->type() == QEvent::FontChange) {
      applyEditorFont();
      schedulePublish();
      return QObject::eventFilter(p_obj, p_event);
    }
  }

  if (p_event->type() == QEvent::LayoutRequest) {
    // LayoutRequest is posted, not sent, so it cannot be filtered by a flag
    // held across setGeometry(). Instead just invalidate the measurement: if
    // the preferred size did not actually change, setWidgetPreviews() sees an
    // unchanged spec set and the pass stops there.
    auto widget = qobject_cast<PreviewWidget *>(p_obj);
    if (widget) {
      for (auto it = m_items.begin(); it != m_items.end(); ++it) {
        if (it.value().m_widget.data() == widget) {
          it.value().m_measureDirty = true;
          schedulePublish();
          break;
        }
      }
    }
  }

  return QObject::eventFilter(p_obj, p_event);
}

// ---------------------------------------------------------------------------
// Replacement
// ---------------------------------------------------------------------------

void InteractivePreviewHost::finishReplacement(PreviewWidgetContext *p_context,
                                               PreviewReplacementResult &p_result,
                                               PreviewReplacementResult::Status p_status,
                                               const QString &p_diagnostic) {
  p_result.setStatus(p_status);
  p_result.setDiagnostic(p_diagnostic);

  // One line per request outcome, so a rejected rewrite is never silent.
  if (p_status == PreviewReplacementResult::Accepted) {
    qCDebug(previewReplaceLog) << "  ->" << statusName(p_status);
  } else if (p_status == PreviewReplacementResult::Deferred) {
    // An expected outcome, not a rejection: nothing was touched and the
    // request is retried.
    qCDebug(previewReplaceLog) << "  ->" << statusName(p_status) << ":" << p_diagnostic;
  } else {
    qCWarning(previewReplaceLog)
        << "replacement rejected for identity" << p_result.identity() << "-"
        << statusName(p_status) << ":" << p_diagnostic;
  }

  if (p_context) {
    p_context->notifyReplacementFinished(p_result);
  }
}

// Whether the reconstructed table matches the container prefixes of the
// snapshot it replaces, row for row. TablePreview::rowPrefixes() holds the
// header prefix followed by one prefix per body row; the delimiter row's
// prefix is stored separately.
static bool tablePrefixesMatch(const QSharedPointer<const TablePreview> &p_original,
                               const md::TableElement &p_candidate) {
  const auto &rowPrefixes = p_original->rowPrefixes();
  if (rowPrefixes.isEmpty() || p_candidate.m_rows.size() < 2) {
    return false;
  }

  if (p_candidate.m_rows[0].m_prefix != rowPrefixes.first() ||
      p_candidate.m_rows[1].m_prefix != p_original->delimiterPrefix()) {
    return false;
  }

  for (int i = 2; i < p_candidate.m_rows.size(); ++i) {
    // Candidate row i is body row i - 1 of the snapshot. Rows beyond the
    // snapshot must continue the last known body prefix.
    const int originalIdx = i - 1;
    const QString expected = originalIdx < rowPrefixes.size()
                                 ? rowPrefixes[originalIdx]
                                 : (rowPrefixes.size() > 1 ? rowPrefixes.last()
                                                           : p_original->delimiterPrefix());
    if (p_candidate.m_rows[i].m_prefix != expected) {
      return false;
    }
  }

  return true;
}

// Build the snapshot which describes @p_text once it sits at [@p_startPos,
// @p_startPos + @p_text.size()), from the element the validation walk already
// resolved.
//
// The revision is carried over unchanged: rebasing does not start a new parse
// generation, it only retargets the current one onto the text the document now
// holds. The placement is carried over too, because it is a property of the
// surrounding line, which a replacement of the element's own range cannot
// change, and the validation probe does not carry that surrounding text.
static QSharedPointer<const Preview> rebaseImage(const QSharedPointer<const Preview> &p_original,
                                                 int p_startPos, const QString &p_text,
                                                 const md::ImageElement &p_element) {
  return PreviewBuilder::createImage(p_original->revision(), p_startPos,
                                     p_startPos + p_text.size(), p_text, p_original->placement(),
                                     p_element.m_destination, p_element.m_alternateText,
                                     p_element.m_title);
}

static QSharedPointer<const Preview> rebaseCode(const QSharedPointer<const Preview> &p_original,
                                                int p_startPos, const QString &p_text,
                                                const md::CodeElement &p_element) {
  return PreviewBuilder::createCode(p_original->revision(), p_startPos,
                                    p_startPos + p_text.size(), p_text, p_element.m_language,
                                    p_element.m_code);
}

static QSharedPointer<const Preview> rebaseMath(const QSharedPointer<const Preview> &p_original,
                                                int p_startPos, const QString &p_text,
                                                const md::MathElement &p_element) {
  return PreviewBuilder::createMath(p_original->revision(), p_startPos,
                                    p_startPos + p_text.size(), p_text, p_element.m_expression,
                                    p_element.m_display);
}

static QSharedPointer<const Preview> rebaseTable(const QSharedPointer<const Preview> &p_original,
                                                 int p_startPos, const QString &p_text,
                                                 const md::TableElement &p_element) {
  return createTablePreview(p_original->revision(), p_startPos, p_startPos + p_text.size(),
                            p_text, p_element);
}

// Per-type dispatch onto the snapshot builders above. Returns a null pointer
// when @p_matchedIndex does not resolve, which the caller treats as "accepted
// but not rebased" exactly as before.
static QSharedPointer<const Preview>
rebaseFromElement(const QSharedPointer<const Preview> &p_original, int p_startPos,
                  const QString &p_text, const md::ASTWalkResult &p_walk, int p_matchedIndex) {
  if (p_matchedIndex < 0) {
    return QSharedPointer<const Preview>();
  }

  switch (p_original->type()) {
  case PreviewElementType::Image:
    if (p_matchedIndex < p_walk.imageElements.size()) {
      return rebaseImage(p_original, p_startPos, p_text, p_walk.imageElements[p_matchedIndex]);
    }
    break;

  case PreviewElementType::Code:
    if (p_matchedIndex < p_walk.codeElements.size()) {
      return rebaseCode(p_original, p_startPos, p_text, p_walk.codeElements[p_matchedIndex]);
    }
    break;

  case PreviewElementType::Math:
    if (p_matchedIndex < p_walk.mathElements.size()) {
      return rebaseMath(p_original, p_startPos, p_text, p_walk.mathElements[p_matchedIndex]);
    }
    break;

  case PreviewElementType::Table:
    if (p_matchedIndex < p_walk.tableElements.size()) {
      return rebaseTable(p_original, p_startPos, p_text, p_walk.tableElements[p_matchedIndex]);
    }
    break;

  default:
    break;
  }

  return QSharedPointer<const Preview>();
}

// Whether @p_text contains a '|' which is not backslash escaped. Mirrors the
// parity rule TablePreviewSerializer::escapeCell() applies.
static bool hasUnescapedPipe(const QString &p_text) {
  int backslashes = 0;
  for (int i = 0; i < p_text.size(); ++i) {
    const QChar ch = p_text.at(i);
    if (ch == QLatin1Char('|') && (backslashes % 2) == 0) {
      return true;
    }

    backslashes = ch == QLatin1Char('\\') ? backslashes + 1 : 0;
  }

  return false;
}

// The text cmark is asked to validate: the whole line the replacement would
// produce, i.e. the block container prefix which precedes the source, the
// replacement, and the text which follows the source on its line. Validating
// in context is what makes a candidate acceptable only under the original
// wrapper chain, and carrying the suffix is what stops a candidate from
// swallowing the text after it.
//
// Returns false, with the status filled in, when the replacement cannot be
// inserted at [@p_startPos, @p_endPos) at all.
static bool buildValidationProbe(const QTextDocument *p_doc, int p_startPos, int p_endPos,
                                 const QString &p_text, QString *p_probe, int *p_prefixLength,
                                 PreviewReplacementResult::Status *p_status,
                                 QString *p_diagnostic) {
  // QTextCursor::insertText() starts a new block on any of these, and cmark's
  // line table only splits on '\n', so anything else would be validated
  // against a different line structure than the one actually inserted.
  int firstBreak = -1;
  for (int i = 0; i < p_text.size(); ++i) {
    const ushort code = p_text.at(i).unicode();
    if (code == '\r' || code == 0x2028 || code == 0x2029) {
      *p_status = PreviewReplacementResult::ParseFailure;
      *p_diagnostic = QStringLiteral("replacement contains an unsupported line separator");
      return false;
    }
    if (code == '\n' && firstBreak < 0) {
      firstBreak = i;
    }
  }

  const QTextBlock startBlock = p_doc->findBlock(p_startPos);
  const QString linePrefix = previewSourceText(p_doc, startBlock.position(), p_startPos);

  const QTextBlock endBlock = p_doc->findBlock(qMax(p_startPos, p_endPos - 1));
  const int endOfBlock = endBlock.position() + endBlock.length() - 1;
  const QString lineSuffix = previewSourceText(p_doc, p_endPos, qMax(p_endPos, endOfBlock));
  if (firstBreak >= 0 && !lineSuffix.trimmed().isEmpty()) {
    *p_status = PreviewReplacementResult::ElementCountMismatch;
    *p_diagnostic =
        QStringLiteral("a multi-line replacement would split the text following the source");
    return false;
  }

  // A '|' is structural inside a table row, but a single line is not a table
  // to cmark - that needs the delimiter row - so the probe below cannot see
  // it. An unescaped pipe in the text retained around the source is the
  // evidence that the source sits in a row, and introducing another one would
  // split the cell and silently change the table's shape.
  if ((hasUnescapedPipe(linePrefix) || hasUnescapedPipe(lineSuffix)) && hasUnescapedPipe(p_text)) {
    *p_status = PreviewReplacementResult::ElementCountMismatch;
    *p_diagnostic =
        QStringLiteral("replacement adds an unescaped '|' to a line which already uses it "
                       "structurally");
    return false;
  }

  *p_probe = linePrefix + p_text + lineSuffix;
  *p_prefixLength = linePrefix.size();
  return true;
}

// Index, inside the vector of its own type, of the single element of @p_type
// which starts exactly where the replacement does and covers it entirely.
// Elements belonging to the retained line prefix are therefore ignored, and
// unrelated content appended by the replacement disqualifies it.
//
// @p_bodyEnd is the offset in @p_probe at which the replacement ends. A
// candidate may not reach past it - that would mean it swallowed the text
// which follows the source on its line - and everything between the candidate
// and it must be whitespace.
//
// Returns -1, with the status filled in, when there is not exactly one.
static int findSoleMatchingElement(const md::ASTWalkResult &p_walk, const QString &p_probe,
                                   int p_prefixLength, int p_bodyEnd, PreviewElementType p_type,
                                   PreviewReplacementResult::Status *p_status,
                                   QString *p_diagnostic) {
  int matching = 0;
  int otherTypes = 0;
  int matchedIndex = -1;

  auto scan = [&](PreviewElementType p_candidate, const auto &p_elements) {
    for (int i = 0; i < p_elements.size(); ++i) {
      const auto &element = p_elements[i];
      if (element.m_startPos != p_prefixLength || element.m_endPos <= element.m_startPos ||
          element.m_endPos > p_bodyEnd ||
          !p_probe.mid(element.m_endPos, p_bodyEnd - element.m_endPos).trimmed().isEmpty()) {
        continue;
      }

      if (p_candidate == p_type) {
        ++matching;
        matchedIndex = i;
      } else {
        ++otherTypes;
      }
    }
  };

  scan(PreviewElementType::Image, p_walk.imageElements);
  scan(PreviewElementType::Code, p_walk.codeElements);
  scan(PreviewElementType::Math, p_walk.mathElements);
  scan(PreviewElementType::Table, p_walk.tableElements);

  if (matching != 1) {
    if (matching == 0 && otherTypes > 0) {
      *p_status = PreviewReplacementResult::TypeMismatch;
      *p_diagnostic = QStringLiteral("replacement resolved to a different element type");
      return -1;
    }

    *p_status = PreviewReplacementResult::ElementCountMismatch;
    *p_diagnostic = QStringLiteral(
        "replacement must contain exactly one element of the same type, under the original "
        "container chain, and nothing else");
    return -1;
  }

  return matchedIndex;
}

bool InteractivePreviewHost::validateReplacement(const QSharedPointer<const Preview> &p_preview,
                                                 int p_startPos, int p_endPos,
                                                 const QString &p_text,
                                                 PreviewReplacementResult::Status *p_status,
                                                 QString *p_diagnostic,
                                                 QSharedPointer<const Preview> *p_rebased) const {
  if (p_rebased) {
    p_rebased->clear();
  }

  if (p_text.trimmed().isEmpty()) {
    *p_status = PreviewReplacementResult::ParseFailure;
    *p_diagnostic = QStringLiteral("empty replacement");
    return false;
  }

  const PreviewElementType type = p_preview->type();

  QString probe;
  int prefixLength = 0;
  if (!buildValidationProbe(m_doc, p_startPos, p_endPos, p_text, &probe, &prefixLength, p_status,
                            p_diagnostic)) {
    return false;
  }

  const QByteArray utf8 = probe.toUtf8();
  const int lines = probe.count(QLatin1Char('\n')) + 1;
  const auto walk = md::walkAndConvert(utf8, lines, 0, 0, false);

  const int matchedIndex = findSoleMatchingElement(walk, probe, prefixLength,
                                                   prefixLength + p_text.size(), type, p_status,
                                                   p_diagnostic);
  if (matchedIndex < 0) {
    return false;
  }

  if (type == PreviewElementType::Table) {
    // The candidate is the one the scan matched, i.e. anchored at the
    // replacement boundary and followed by nothing but whitespace. Its
    // container prefixes must be verified before anything is rebased onto it.
    const md::TableElement *candidate = matchedIndex < walk.tableElements.size()
                                            ? &walk.tableElements[matchedIndex]
                                            : nullptr;

    if (!candidate ||
        !tablePrefixesMatch(p_preview.staticCast<const TablePreview>(), *candidate)) {
      *p_status = PreviewReplacementResult::ElementCountMismatch;
      *p_diagnostic = QStringLiteral("replacement changes the table's block container prefixes");
      return false;
    }
  }

  if (p_rebased) {
    *p_rebased = rebaseFromElement(p_preview, p_startPos, p_text, walk, matchedIndex);
  }

  return true;
}

void InteractivePreviewHost::handleSourceReplacementRequested(
    quint64 p_identity, quint64 p_revision, const QString &p_expectedSource,
    const QString &p_replacementMarkdown) {
  PreviewReplacementResult result;
  result.setIdentity(p_identity);
  result.setRequestedRevision(p_revision);
  result.setCurrentRevision(m_revision);

  auto *context = qobject_cast<PreviewWidgetContext *>(sender());

  qCDebug(previewReplaceLog) << "replacement requested by identity" << p_identity << "revision"
                             << p_revision << "current" << m_revision << "- expected source"
                             << p_expectedSource.left(60) << "-> replacement"
                             << p_replacementMarkdown.left(60);

  // Before m_items.find() and before every validation gate below, so a
  // postponed request touches no host state at all. Qt does not support
  // mutating a QTextDocument from inside a layout pass, and the geometry a
  // widget callback is being handed is computed by one.
  if (isBlocked()) {
    if (!m_deferredReplacements.contains(p_identity)) {
      // Never clobber an attempt count which is already being spent.
      m_deferredReplacements.insert(p_identity, 0);
    }

    m_replacementRetryPending = true;
    scheduleOwedWork();
    finishReplacement(context, result, PreviewReplacementResult::Deferred,
                      QStringLiteral("the layout is mid-pass - the request was not applied"));
    return;
  }

  // The request is being answered now, so the host owes it no retry anymore,
  // whatever the outcome below turns out to be.
  m_deferredReplacements.remove(p_identity);

  // Everything from here to the completion is one transaction: the document
  // edit itself, and the completion which is what clears the requesting
  // sheet's "in flight" flag. A reconcile or a removal delivered inside it -
  // through a nested event loop a document callback opened, say - would reach
  // a mandatory flush which can only decline while that flag is set, so it is
  // postponed instead.
  BlockGuard transaction(this, BlockGuard::Reason::SourceReplacement);

  auto it = m_items.find(p_identity);
  if (it == m_items.end()) {
    finishReplacement(context, result, PreviewReplacementResult::UnknownIdentity,
                      QStringLiteral("no active preview owns this identity"));
    return;
  }

  auto &item = it.value();
  if (!context) {
    context = item.m_context.data();
  }

  result.setCurrentRevision(item.m_preview ? item.m_preview->revision() : m_revision);

  // Stale only when the widget targets a superseded snapshot; an unrelated
  // document revision elsewhere is not a conflict by itself.
  if (!item.m_preview || item.m_preview->revision() != p_revision) {
    finishReplacement(context, result, PreviewReplacementResult::StaleSnapshot,
                      QStringLiteral("the snapshot has been superseded"));
    return;
  }

  if (!m_textEdit || m_textEdit->isReadOnly()) {
    finishReplacement(context, result, PreviewReplacementResult::ReadOnly,
                      QStringLiteral("the editor is read-only"));
    return;
  }

  const int start = item.m_anchor.selectionStart();
  const int end = item.m_anchor.selectionEnd();
  if (start < 0 || end <= start || end > m_doc->characterCount() - 1) {
    finishReplacement(context, result, PreviewReplacementResult::InvalidRange,
                      QStringLiteral("the anchored range is no longer resolvable"));
    return;
  }

  const QString live = previewSourceText(m_doc, start, end);
  if (live != p_expectedSource || live != item.m_preview->sourceMarkdown()) {
    qCDebug(previewReplaceLog) << "  source mismatch - live" << live.left(60) << "expected"
                               << p_expectedSource.left(60) << "bound"
                               << item.m_preview->sourceMarkdown().left(60);
    finishReplacement(context, result, PreviewReplacementResult::SourceMismatch,
                      QStringLiteral("the live source no longer matches the snapshot"));
    return;
  }

  PreviewReplacementResult::Status status = PreviewReplacementResult::ParseFailure;
  QString diagnostic;
  QSharedPointer<const Preview> rebased;
  if (!validateReplacement(item.m_preview, start, end, p_replacementMarkdown, &status, &diagnostic,
                           &rebased)) {
    finishReplacement(context, result, status, diagnostic);
    return;
  }

  // Sample the live fold state of the element about to be destroyed.
  //
  // item.m_foldState must not be trusted here: only the queued refresh writes
  // it, so a manual fold or unfold made since the last delivery is not in it
  // yet. The live state is written back onto the item so the restore branch of
  // the auto-fold pass agrees with it afterwards.
  //
  // The *element's* extent is what both sides need, not the anchor's: trailing
  // whitespace inside a replacement is accepted but is not part of the parsed
  // element, so the folding region ends at the last non-blank line.
  const int firstBlock = m_doc->findBlock(start).blockNumber();
  const int liveSpan = trimmedEndOffset(live);
  bool folded = false;
  bool known = false;
  if (liveSpan > 0) {
    const int lastBlock = m_doc->findBlock(start + liveSpan - 1).blockNumber();
    known = m_editor->tryPreviewSourceFolded(item.m_preview->type(), firstBlock, lastBlock,
                                             &folded);
  }

  if (known) {
    item.m_foldState = folded ? PreviewFoldState::Folded : PreviewFoldState::Unfolded;
  }
  // known == false means there is no live range for this source at all - most
  // importantly while text folding is disabled, where every range was cleared.
  // The item's remembered state has to survive that untouched.

  QTextCursor cursor(m_doc);
  cursor.beginEditBlock();
  cursor.setPosition(start);
  cursor.setPosition(end, QTextCursor::KeepAnchor);
  cursor.insertText(p_replacementMarkdown);
  cursor.endEditBlock();

  // insertText() removes the selection first, which collapses this anchor, and
  // then advances it past the inserted text. Retarget it explicitly so the
  // identity survives until the next parse generation.
  item.m_anchor = makeAnchor(start, start + p_replacementMarkdown.size());
  item.m_measureDirty = true;

  // Rebind onto the text which is now in the document. Without this the widget
  // would keep quoting the pre-edit source as its expected source, so any
  // second request issued before the next parse generation lands would be
  // rejected as a SourceMismatch and the edit silently dropped.
  if (rebased) {
    item.m_preview = rebased;
    if (item.m_context) {
      item.m_context->setPreview(rebased);
    }
  }

  qCDebug(previewReplaceLog) << "  applied to [" << start << ","
                             << (start + p_replacementMarkdown.size()) << ")"
                             << (rebased ? "and rebased the bound snapshot"
                                         : "WITHOUT a rebased snapshot");

  // Restore the fold in this very event-loop turn. Waiting for the next parse
  // would let the source visibly expand and collapse again. The edit is
  // complete - endEditBlock() has returned - and no repaint can happen before
  // this returns, so folding here is safe; it dirties the layout, and the
  // schedulePublish() below resettles the widget geometry.
  const int span = trimmedEndOffset(p_replacementMarkdown);
  if (known && folded && span > 0) {
    m_editor->restoreFoldAfterPreviewRewrite(
        item.m_preview->type(), firstBlock, m_doc->findBlock(start + span - 1).blockNumber());
  }

  // The anchors moved: resubmit the reservations without waiting for the next
  // parse generation.
  schedulePublish();

  // A generation stashed before this edit describes the source the edit has
  // just replaced, and replaying it would re-anchor every element from here on
  // at pre-edit positions - the very hazard rebuildAll() carries m_carriedItems
  // to avoid. Dropping it is safe: the edit itself owes a parse, and until that
  // lands the items keep their live anchors and their bound snapshots.
  dropDeferredGeneration();

  finishReplacement(context, result, PreviewReplacementResult::Accepted, QString());
}
