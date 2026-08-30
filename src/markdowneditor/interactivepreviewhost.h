#ifndef INTERACTIVEPREVIEWHOST_H
#define INTERACTIVEPREVIEWHOST_H

#include <QHash>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QRectF>
#include <QSet>
#include <QSharedPointer>
#include <QSizeF>
#include <QTextCursor>
#include <QVector>

#include <vtextedit/global.h>
#include <vtextedit/preview.h>
#include <vtextedit/previewwidget.h>

#include "markdownfoldingprovider.h"
#include "textdocumentlayout.h"

class QTextDocument;
class QScrollBar;

namespace vte {
class InputModeStatusWidget;
class TablePreviewWidget;
class TablePreviewWidgetFactory;
class VMarkdownEditor;
class VTextEdit;

// Defined in tablepreviewwidget.h. Only ever passed by value through plain
// member functions below, never through a signal or slot, so moc never has to
// see the definition.
enum class FocusEscapeDirection;

// Owns every interactive preview widget of one VMarkdownEditor.
//
// It is an internal QObject child of the editor so no exported class needs a
// new data member. Layout only ever learns identities and rectangles; widget
// ownership never leaves this host.
class InteractivePreviewHost : public QObject {
  Q_OBJECT
public:
  explicit InteractivePreviewHost(VMarkdownEditor *p_editor);

  ~InteractivePreviewHost() Q_DECL_OVERRIDE;

  // Object name used to locate the host from VMarkdownEditor.
  static const char *c_objectName;

  bool registerFactory(PreviewWidgetFactory *p_factory, int p_priority);

  bool unregisterFactory(PreviewWidgetFactory *p_factory);

  // Global switch, mirroring VMarkdownEditor::setInplacePreviewEnabled().
  void setEnabled(bool p_enabled);

  void setTypeEnabled(PreviewElementType p_type, bool p_enabled);

  // Whether a table sheet writes back a padded, column-aligned pipe table
  // instead of the compact one. Affects subsequent commits only: no existing
  // source is reformatted.
  //
  // The value is stored as well as forwarded. The built-in table factory is
  // constructed eagerly, so the stored copy is a defensive fallback for an
  // application which unregistered it, not a lazy-creation requirement.
  void setTableSourceAlignEnabled(bool p_enabled);

  bool isTypeEnabled(PreviewElementType p_type) const;

  // Dynamic property carrying the enabled element type mask to the
  // highlighter. A property avoids adding a data member to the exported
  // MarkdownHighlighter, which would break its ABI.
  static const char *c_enabledTypeMaskProperty;

  // Number of reconcile deliveries that have run, published as a dynamic
  // property on the host so a test can tell a delivered rebuild from a
  // delivery which never happened because the host was blocked. A blocked
  // delivery calls into no factory and no widget, so it is otherwise invisible
  // from outside.
  static const char *c_reconcileDeliveryCountProperty;

  // Number of preview driven fold evaluations that have actually run. Same
  // mechanism as the reconcile counter above: both are incremented past the
  // blocked check, so neither moves while the host is blocked.
  static const char *c_foldRefreshCountProperty;

  // Number of owed-work drains that have actually run, i.e. that were not
  // declined by the blocked check. A test uses it to tell one settled drain
  // from a zero timer spinning against a held block.
  static const char *c_owedWorkDrainCountProperty;

  // Performance counters, published through the same dynamic-property
  // mechanism as the three counters above.
  //
  // They are the deterministic gates of the preview benchmark: wall clock on a
  // shared CI runner is only ever logged, never asserted, so every claim the
  // benchmark makes about how much work a pass does has to be expressed as one
  // of these. Dynamic properties rather than getters because the benchmark
  // links the shared VTextEdit and this class is not exported.
  //
  // Each is a monotonically increasing int, reset as a group by writing
  // c_countersResetProperty (see below).

  // Items bound to a snapshot: one per item created, one per item updated in
  // place. Measures reconcile work, and after lazy realization it is the
  // denominator c_widgetsRealizedProperty is read against.
  static const char *c_previewsBoundProperty;

  // Widgets a factory actually constructed.
  static const char *c_widgetsRealizedProperty;

  // Widgets torn down by removeItem().
  static const char *c_widgetsDestroyedProperty;

  // publish() bodies that have run.
  static const char *c_publishesProperty;

  // QWidget::setGeometry() calls issued by the scroll placement pass. The
  // direct measure of "every scroll tick walks every item".
  static const char *c_geometrySetCallsProperty;

  // Times findIdentity() fell through the exact-anchor index into the linear
  // overlap scan. The measurement which decides whether that scan is a real
  // cost at all.
  static const char *c_identityFallbackHitsProperty;

  // rebuildAll() bodies that have run (a postponed rebuild does not count).
  static const char *c_rebuildAllsProperty;

  // Table sheet cells constructed, aggregated from the table layer. Process
  // wide, because the count originates in TablePreviewDocument, which has no
  // back pointer to a host; a benchmark drives one editor at a time.
  static const char *c_tableCellsBuiltProperty;

  // Full cmark parses performed for per-cell table syntax highlighting,
  // aggregated from the AST walker. Process wide for the same reason, and the
  // direct measure of what the walker's document-wide cell budget bounds.
  static const char *c_snippetParsesProperty;

  // Write any value to reset every counter above to zero, including the
  // process-wide cell counter. A write is only honoured when it lands on this
  // host, so the property write is also the reset point: anything already
  // armed and not yet delivered is counted into the next window, which is why
  // a benchmark scenario settles the event loop before resetting.
  //
  // The handler clears the property again, so writing the same value twice
  // performs two resets. Qt only notifies on a CHANGE, and without the clear
  // the second write in a row would be silently ignored.
  static const char *c_countersResetProperty;

  // Whether the host may not touch the editor's document right now: either a
  // layout pass is running, or an application-defined callback or a geometry
  // application is on the stack. Every mutating entry point tests this, and
  // whatever it declines is owed through scheduleOwedWork().
  bool isBlocked() const { return m_blockDepth > 0 || (m_layout && m_layout->isBusy()); }

  // Block extent, type and fold state of every element which has a widget.
  QVector<PreviewedRange> previewedRanges() const;

  // Write back what the folding provider decided for each identity. Identities
  // which are gone are ignored.
  void setPreviewFoldStates(const QVector<QPair<quint64, PreviewFoldState>> &p_states);

  // Coalesced, blocked-aware re-evaluation of the preview driven fold state.
  void scheduleFoldRefresh();

public slots:
  void updatePreviews(quint64 p_revision, const QVector<QSharedPointer<const Preview>> &p_previews);

  // Recompute preferred sizes, resubmit layout reservations and reposition
  // every widget. Coalesced through a zero timer.
  void schedulePublish();

protected:
  bool event(QEvent *p_event) Q_DECL_OVERRIDE;

  bool eventFilter(QObject *p_obj, QEvent *p_event) Q_DECL_OVERRIDE;

private slots:
  void handleSourceReplacementRequested(quint64 p_identity, quint64 p_revision,
                                        const QString &p_expectedSource,
                                        const QString &p_replacementMarkdown);

  // Full refresh: recompute the document rectangles and per-widget context.
  void syncWidgetGeometry();

  // Cheap path used while scrolling: only remaps cached document rectangles
  // onto the viewport.
  void applyScrollOffset();

  void publish();

  // Ask for a rebuild, stating whether the reason can WIDEN the set of element
  // types the highlighter is asked to build snapshots for.
  //
  // m_lastPreviews only ever holds snapshots for the types that were enabled
  // and claimable at the last publication, so a widening change - enabling a
  // type, registering a factory that newly claims one - cannot be served by
  // replaying it: the snapshots simply are not there, and the highlighter has
  // to build them. A NARROWING change - disabling a type, unregistering a
  // factory - is fully served by the replay, and asking for a reparse there is
  // pure waste on a document with hundreds of previews.
  //
  // Coalescing is an OR: if anything owed may widen, the combined rebuild may.
  void reconcileLater(bool p_mayWiden);

private:
  // Marks a span during which the host must not mutate the editor's document
  // and must not start a reconcile or a rebuild.
  //
  // Depth counted, so only the outermost destructor hands control back, and it
  // does so through the single scheduleOwedWork() routine rather than by hand
  // picking what to re-arm.
  class BlockGuard {
  public:
    enum class Reason {
      // An application-defined factory or widget callback is running, during
      // which the factory registry must not be mutated by reentrant
      // registration either.
      FactoryCallback,

      // Widget geometry is being applied. A custom widget can request a
      // replacement from geometryContextChanged, which is emitted
      // synchronously from here while the layout's own depth is already 0.
      GeometryApply,

      // A source replacement is being validated, applied and completed.
      //
      // A widget's commit is "in flight" from the moment it issues the
      // request until its completion returns, and a flush re-entered in that
      // window declines rather than issuing a second request against an
      // anchor the outer edit has collapsed. Marking the transaction is what
      // makes that window part of the blocked predicate, so a reconcile or a
      // removal delivered from a nested event loop inside it is postponed
      // instead of hitting a mandatory flush which can only decline.
      SourceReplacement
    };

    BlockGuard(InteractivePreviewHost *p_host, Reason p_reason);

    ~BlockGuard();

  private:
    InteractivePreviewHost *m_host = nullptr;

    Reason m_reason;
  };

  // A registered factory. Removal erases the entry, so presence in
  // m_factories is what "still active" means.
  struct FactoryEntry {
    QPointer<PreviewWidgetFactory> m_factory;

    int m_priority = 0;

    // Registration order, used to break priority ties.
    int m_order = 0;
  };

  // One live element.
  //
  // An item is always BOUND: it has an identity, a snapshot, a live anchor, a
  // fold state and a reserved band. It is additionally REALIZED when a factory
  // has built a widget for it, which is exactly when m_widget is non-null -
  // there is no separate flag, because a widget destroyed behind the host's
  // back must read as unrealized and a bool would disagree with reality at
  // that moment.
  //
  // Realization is demand driven: an item whose band is nowhere near the
  // viewport stays bound, costing one estimate instead of a QTextDocument and
  // a QTextTable. See realizeItem().
  struct ActiveItem {
    quint64 m_id = 0;

    // The snapshot currently bound to the widget. It is normally the one the
    // parse generation delivered, but an accepted source replacement rebases
    // it onto the text which is now in the document.
    QSharedPointer<const Preview> m_preview;

    // The snapshot exactly as the parse generation delivered it. Only used to
    // key the anchors carried across a rebuild, which are looked up by the
    // pointers held in m_lastPreviews.
    QSharedPointer<const Preview> m_generationPreview;

    QPointer<PreviewWidgetContext> m_context;

    QPointer<PreviewWidget> m_widget;

    QPointer<PreviewWidgetFactory> m_factory;

    // Live anchor which follows the source through document edits.
    QTextCursor m_anchor;

    // Cached preferred size and the width basis it was measured at. Measuring
    // is expensive (the table sheet scans its contents), so it is redone only
    // when the widget asked for a new layout or the basis changed.
    //
    // For an item which is BOUND but not REALIZED this holds the factory's
    // estimate instead, computed by PreviewSizeEstimator without building
    // anything. m_sizeIsEstimate says which of the two it is.
    QSizeF m_measuredSize;

    qreal m_measuredWidthBasis = -1;

    bool m_measureDirty = true;

    // Whether m_measuredSize came from an estimator rather than from the real
    // widget. Realizing an item clears it, and the difference between the two
    // is what the scroll pass compensates the scrollbar by.
    bool m_sizeIsEstimate = false;

    // Last published document rectangle, so scrolling does not have to
    // recompute any scroll invariant geometry.
    QRectF m_documentRect;

    // Cached source text rectangle and the inputs it was computed from.
    // sourceTextRect() unions the layout rectangle of every visual line of the
    // source range, which is the expensive part of a geometry sync, and it is
    // only reported to application code. Any reflow of the source changes the
    // block's height or offset, and therefore the reserved band, so the band
    // plus the tracked range is a sufficient key.
    QRectF m_sourceTextRect;

    QRectF m_sourceRectBand;

    int m_sourceRectStart = -1;

    int m_sourceRectEnd = -1;

    // The initial fold state this element has been settled into, or Undecided
    // while no pass has seen it together with a live folding range. It lives on
    // the item and not on the folding range because a range is destroyed by the
    // very edit an in-place rewrite performs, while the item and its anchor
    // survive it.
    PreviewFoldState m_foldState = PreviewFoldState::Undecided;
  };

  // State of a live item carried across a rebuild. The anchor has been
  // tracking every edit since the parse generation, and the bound snapshot may
  // have been rebased by an accepted replacement, so replaying the generation
  // snapshot alone would resurrect superseded source.
  struct CarriedItem {
    QTextCursor m_anchor;

    QSharedPointer<const Preview> m_bound;

    PreviewFoldState m_foldState = PreviewFoldState::Undecided;
  };

  // A removed pair whose deferred deletion has not been delivered yet.
  struct PendingDeletion {
    QPointer<PreviewWidget> m_widget;

    QPointer<PreviewWidgetContext> m_context;
  };

  QWidget *viewport() const;

  // A sheet is handing the caret back to the editor. The destination is
  // resolved here rather than in the sheet: only this host holds a live anchor
  // for the source, and the snapshot positions a sheet knows go stale on any
  // unrelated edit.
  void handleFocusEscape(TablePreviewWidget *p_widget, FocusEscapeDirection p_direction);

  // A sheet relays undo/redo instead of handling it, because the inner
  // document has no undo stack: the granularity is one whole-table replacement
  // on the editor's own stack.
  void handleSheetUndo(TablePreviewWidget *p_widget);

  void handleSheetRedo(TablePreviewWidget *p_widget);

  // The identity of the item @p_widget belongs to, or 0.
  quint64 identityOf(const PreviewWidget *p_widget) const;

  // The identity of the item owning @p_focus, which is normally not the
  // PreviewWidget itself but a focusable descendant of it (the table sheet, for
  // one). Returns 0 when the widget belongs to no item of this host.
  quint64 identityForFocusWidget(QWidget *p_focus) const;

  // Collapse the selection of every live preview widget. Called when focus
  // returns to the text editor, so a leftover preview highlight does not
  // compete with the editor's own selection.
  void clearPreviewSelections();

  // Focus moved somewhere in the application. Watched globally rather than
  // through an event filter on the preview root, which never sees a
  // descendant's FocusIn.
  void handleApplicationFocusChanged(QWidget *p_old, QWidget *p_now);

  // Move the editor's text cursor onto the first block of @p_id's live source
  // range, so the cursor-line extra selection and the gutter's current line
  // follow the focused preview. Runs from the owed-work drain, so everything is
  // re-validated here.
  void syncCursorLineToItem(quint64 p_id);

  // Deliver @p_bar's current value to the connections which missed it while
  // the bar's signals were blocked. See the call site in
  // syncCursorLineToItem().
  static void resyncScrollBar(QScrollBar *p_bar);

  // The same, for a range which changed while the bar's signals were blocked.
  static void resyncScrollBarRange(QScrollBar *p_bar);

  // A snapshot of the active factories in resolution order. Returned by value
  // because a factory may mutate the registry from inside its own callback.
  QVector<FactoryEntry> orderedFactories() const;

  bool isFactoryActive(PreviewWidgetFactory *p_factory) const;

  // Whether any active factory advertises @p_type. Used to skip the whole
  // widget construction dance for elements nothing can render.
  //
  // Not const: it calls into application code, which may mutate the registry.
  bool isTypeClaimable(PreviewElementType p_type);

  // Push the enabled type mask to the highlighter so it only builds the
  // snapshots which can actually be used.
  void publishEnabledTypeMask();

  // Push the editor's read-only state down to the built-in renderers, so a
  // sheet never offers an edit the host would reject at commit time.
  void applyReadOnly();

  // Give every preview widget the editor's font.
  //
  // An application usually sets the editor font through a style sheet. That
  // font reaches VTextEdit itself, but the viewport carries a style sheet of
  // its own, so a widget parented to the viewport keeps the application
  // default in QWidget::font() while still being *painted* with the inherited
  // style sheet font. A widget measuring itself from its own font metrics then
  // reserves a band far too small for what is drawn into it.
  void applyEditorFont();

  // The theme's generic text font, at the editor's current (zoomed) size.
  //
  // Taken from the theme rather than from VTextEdit::font(), which only
  // carries the themed font once the style sheet has been polished.
  QFont editorFont() const;

  // Last read-only state reported to the log, so the per-publish call does not
  // repeat itself.
  bool m_lastLoggedReadOnly = false;

  // Mirrored onto m_tableFactory whenever it exists.
  bool m_tableSourceAlign = false;

  // Returns 0 when no existing identity can safely be reused.
  quint64 findIdentity(const QSharedPointer<const Preview> &p_preview,
                       const QSet<quint64> &p_used) const;

  // Rebuild the exact-anchor index findIdentity() resolves its fast path
  // through. Valid only for the span of one matching loop, during which no
  // item is created or removed.
  void rebuildAnchorIndex();

  void createItem(const QSharedPointer<const Preview> &p_preview,
                  PreviewFoldState p_carried = PreviewFoldState::Undecided);

  void updateItem(quint64 p_id, const QSharedPointer<const Preview> &p_preview);

  void removeItem(quint64 p_id);

  void removeItem(quint64 p_id, bool p_synchronous);

  // Destroy every active preview. During host destruction the widgets are
  // deleted synchronously so nothing survives the editor without an owner.
  void removeAllItems(bool p_synchronous = false);

  void prunePendingDeletions();

  // Destroy everything whose deferred deletion is still outstanding.
  void flushPendingDeletions();

  // Try every compatible factory in order. Returns nullptr when none claims.
  PreviewWidget *createWidgetFor(const QSharedPointer<const Preview> &p_preview,
                                 PreviewWidgetContext *p_context,
                                 PreviewWidgetFactory **p_usedFactory);

  QSizeF preferredSize(PreviewWidget *p_widget, PreviewPlacement p_placement, int p_startPos,
                       int p_endPos) const;

  // The horizontal room an element will be given, which is both the input to
  // its measurement and the key the cached measurement is stored under.
  //
  // One helper because there are two callers - publish() and the measurement
  // realizeVisibleItems() performs - and they MUST agree: the second caches a
  // basis alongside a size and then clears m_measureDirty, so a basis that
  // does not describe the size it was stored with would suppress the
  // re-measurement that would have corrected it.
  qreal widthBasisFor(const ActiveItem &p_item, qreal p_availableWidth) const;

  // The highest ranking active factory which advertises @p_type, or nullptr.
  // Advertising is not the same as claiming - a factory may still decline
  // createWidget() - so this is only ever used to find an estimator and to
  // decide whether it is worth binding an item at all.
  //
  // Not const: supportedTypes() is application code and runs under a
  // BlockGuard, exactly as isTypeClaimable() does.
  PreviewWidgetFactory *estimatingFactoryFor(PreviewElementType p_type);

  // The band to reserve for an item which has no widget, or an invalid QSizeF
  // when no factory offers an estimate. An invalid answer is what keeps a
  // third-party factory on today's eager path.
  QSizeF estimatedSize(const ActiveItem &p_item, qreal p_widthBasis);

  // What one realization attempt did to the item. Deliberately three-valued
  // rather than a bool: "it did not get a widget" and "the item set changed"
  // are different questions, and answering the second with the first is an
  // infinite loop.
  //
  // A StillBound outcome means the attempt failed but left the item exactly as
  // it was. Treating that as a change would make realizeVisibleItems() owe a
  // publication, whose drain re-runs the scroll pass, which re-collects the
  // same in-band candidate and fails identically - a zero timer spinning
  // forever on the GUI thread.
  enum class RealizeOutcome {
    // A widget was built and attached.
    Realized,

    // Every factory declined, so the item was removed and the element went
    // back to the painted fallback. The item set changed.
    Removed,

    // The attempt failed - a callback destroyed the context or the widget - and
    // the item is untouched. NOTHING changed, so nothing may be owed for it.
    StillBound
  };

  // Build the widget for a bound item. The single realization transition.
  //
  // Every failure - the factory was destroyed or unregistered by its own
  // callback, it declined, its widget refused setPreview(), or the item was
  // torn down from inside a callback - is reported through the outcome above.
  // Nothing is ever left half realized.
  RealizeOutcome realizeItem(quint64 p_id);

  // Realize every bound item whose reserved band is within one viewport height
  // of the visible area, and record what the correction owes the scrollbar.
  //
  // Called from the scroll placement pass. It deliberately does NOT use the
  // geometry it is standing on afterwards: a realization changes the item's
  // preferred height, and that height only reaches the document through a new
  // publication and the relayout it triggers, which moves every later block.
  // Returns true when something was realized, i.e. when the caller must bail
  // out and let the owed-work drain re-run it against settled geometry.
  bool realizeVisibleItems();

  // Apply the scrollbar correction realizeVisibleItems() accumulated, clamped
  // against the range the relayout has since produced. Runs from the owed-work
  // drain, after publication and geometry sync, which is the first moment the
  // new range exists.
  void applyRealizationScrollCompensation();

  // One entry of the band-ordered geometry index. See m_geometryOrder.
  struct GeometryEntry {
    // Top of the item's reserved band, in document coordinates.
    qreal m_top = 0;

    // The largest band bottom among this entry and every entry before it.
    //
    // Bottoms are NOT monotonic in top order - two previews sharing one inline
    // band, or a tall block preview followed by a short one, both break it -
    // so a binary search on bottom is only sound against this running maximum.
    qreal m_maxBottomSoFar = 0;

    quint64 m_id = 0;
  };

  // Every REALIZED item, ordered by band top with the identity as a stable
  // tie-breaker, and carrying the prefix maximum of the band bottoms.
  //
  // publish() sorts its specs by IDENTITY, and m_items is a hash, so before
  // this there was no band-ordered sequence anywhere and every scroll tick had
  // to walk all N items to place the handful that are visible. Rebuilt once
  // per full geometry sync - which is where the rectangles change - and only
  // read by the placement pass.
  QVector<GeometryEntry> m_geometryOrder;

  // The same, over every BOUND but UNREALIZED item.
  //
  // A separate index rather than one combined one, because the two passes ask
  // different questions over disjoint sets: placement walks the realized items
  // inside the viewport, realization walks the unrealized ones inside a wider
  // band. Indexing only the realized half would leave the realization scan
  // walking the whole item hash per scroll tick - and in a large document the
  // unrealized half IS the large one, so that is the scan that matters.
  QVector<GeometryEntry> m_realizationOrder;

  // The identities the last placement pass positioned and left visible.
  //
  // An item which has left the visible interval must still be hidden, or it
  // stays painted at the viewport coordinates it had when it was last placed.
  // Keeping the set is what makes "just exited" cost the same as "just
  // entered" instead of costing a full walk.
  QSet<quint64> m_placedIds;

  // Whether the two indices above describe the current item set.
  //
  // A bulk teardown sets this instead of erasing from the vectors per item:
  // removeItem() would otherwise linear-scan and shift an N-element vector for
  // each of N items, making removeAllItems() - which every widening reconcile
  // runs - quadratic in the number of previews.
  bool m_geometryOrderDirty = false;

  // Rebuild both band-ordered indices from the current item rectangles.
  void rebuildGeometryOrder();

  // Sort one index by band top and turn its bottoms into a running maximum.
  // Shared by both indices so the ordering and the prefix-maximum rule are
  // spelled once.
  static void finalizeGeometryIndex(QVector<GeometryEntry> &p_entries);

  // Drop @p_id from the indices, unless a bulk teardown has already invalidated
  // them wholesale.
  void forgetGeometryOrderEntry(quint64 p_id);

  QTextCursor makeAnchor(int p_startPos, int p_endPos) const;

  // Ask every live table sheet to write back what its debounce still owes,
  // while its context and its anchor are still authoritative.
  void flushDirtySheets();

  // Rebuild every item, carrying the live anchors across so a factory or
  // enablement change never re-anchors at superseded parse positions.
  void rebuildAll();

  // The owed-work categories, in the one order a drain runs them. The order
  // is load-bearing: a replacement must settle before the reconcile which
  // would remove its item, and the item set must settle before geometry and
  // folding observe it.
  enum class OwedWork {
    ReplacementRetry,
    DeferredGeneration,
    Reconcile,
    Publish,
    GeometrySync,
    ScrollApply,

    // Ranks below ScrollApply because it corrects the scrollbar against the
    // range the publication and the geometry sync above have just produced.
    // Running it any earlier would clamp against the old range.
    RealizationCompensation,

    FoldRefresh,

    // The cursor move must observe a settled item set and geometry, so it
    // ranks below everything which can still move or destroy an item.
    CursorLineSync,

    // Ranks below every real category, so "nothing is owed" never stops a
    // drain.
    None
  };

  // Arm exactly one delivery of everything currently owed.
  //
  // Nothing may arm a bare zero timer while the host is blocked: a widget
  // callback may run a real nested event loop, which would keep delivering
  // each newly armed timer while the block is held by a stack frame that loop
  // is holding open. So while blocked this only records the debt and, when the
  // layout is the blocker, asks it for the becameIdle() edge. The two unblock
  // edges - the outermost BlockGuard and becameIdle() - both come back here.
  void scheduleOwedWork();

  // Run everything owed, in the one fixed order which keeps the passes
  // consistent with each other. Re-owes itself the moment it is blocked again.
  void drainOwedWork();

  // The ordered body of one drain. Split out so the "a drain is running" flag
  // is cleared before the follow-up delivery is armed.
  void runOwedWorkSteps();

  // The highest ranking category which is currently owed and runnable.
  OwedWork highestOwedWork() const;

  // Whether the drain must stop before the step ranking at @p_step, because it
  // is blocked again or because a step just raised work ranking above it.
  bool stopOwedWorkBefore(OwedWork p_step) const;

  // Ask each sheet whose replacement was postponed to write itself back again.
  // Only the built-in table sheets: replaying a third-party widget's request
  // verbatim would deliver two completions for one logical request.
  void retryDeferredReplacements();

  // Hand the stashed parse generation back to updatePreviews(). One routine,
  // so the drain step and the tail of a finished pass cannot drift apart.
  void replayDeferredGeneration();

  // Discard the stashed parse generation, because it describes source the
  // document no longer holds. Only an applied document edit may do this: the
  // edit owes a parse of its own, which is what replaces the dropped one.
  void dropDeferredGeneration();

  // The bodies of a full geometry sync and of the scroll-only remap. Called
  // directly by the wrappers and by drainOwedWork(), never deferred, so
  // syncWidgetGeometry()'s intentional inner scroll apply is not self-deferred.
  void syncWidgetGeometryImpl();

  void applyScrollOffsetImpl();

  bool validateReplacement(const QSharedPointer<const Preview> &p_preview, int p_startPos,
                           int p_endPos, const QString &p_text,
                           PreviewReplacementResult::Status *p_status, QString *p_diagnostic,
                           QSharedPointer<const Preview> *p_rebased = nullptr) const;

  void finishReplacement(PreviewWidgetContext *p_context, PreviewReplacementResult &p_result,
                         PreviewReplacementResult::Status p_status, const QString &p_diagnostic);

  VMarkdownEditor *m_editor = nullptr;

  VTextEdit *m_textEdit = nullptr;

  QTextDocument *m_doc = nullptr;

  TextDocumentLayout *m_layout = nullptr;

  QVector<FactoryEntry> m_factories;

  int m_nextFactoryOrder = 0;

  QHash<quint64, ActiveItem> m_items;

  // The item which currently owns the keyboard focus, or 0.
  quint64 m_focusedItemId = 0;

  // Live anchor ranges to the identities holding them, so the overwhelmingly
  // common "the element did not move relative to its anchor" case does not
  // scan every item. Without it, matching a generation is quadratic in the
  // number of previews the document holds. Several items can share a range
  // (different types), hence the list.
  QHash<QPair<int, int>, QVector<quint64>> m_anchorIndex;

  QVector<PendingDeletion> m_pendingDeletions;

  // Live state carried across a rebuild, keyed by the generation snapshot the
  // replay hands back to createItem().
  QHash<const Preview *, CarriedItem> m_carriedItems;

  quint64 m_nextIdentity = 1;

  quint64 m_revision = 0;

  // The last accepted snapshot generation, replayed when the factory set or
  // the enabled sources change.
  QVector<QSharedPointer<const Preview>> m_lastPreviews;

  bool m_enabled = true;

  bool m_typeEnabled[c_previewElementTypeCount] = {true, true, true, true};

  bool m_reconciling = false;

  // Whether a rebuild is owed. Kept separate from whether a delivery is
  // already queued, so an unblock hook can re-arm exactly one delivery.
  bool m_reconcilePending = false;

  // Whether the owed rebuild may need snapshots m_lastPreviews does not hold.
  // See reconcileLater(bool).
  bool m_reconcileMayWiden = false;

  // Whether a preview driven fold re-evaluation is owed.
  bool m_foldRefreshPending = false;

  // Whether a cursor-line sync is owed, and for which identity. A newer focus
  // overwrites the identity, so at most one cursor move runs per drain.
  bool m_cursorLineSyncPending = false;

  quint64 m_cursorLineSyncId = 0;

  // Published through c_foldRefreshCountProperty.
  int m_foldRefreshCount = 0;

  // A parse generation delivered while a pass was already running. A widget
  // callback may spin a nested event loop, which delivers the highlighter's
  // queued result; dropping it would leave every bound snapshot describing
  // superseded source until the next document edit.
  bool m_hasDeferredGeneration = false;

  quint64 m_deferredRevision = 0;

  QVector<QSharedPointer<const Preview>> m_deferredPreviews;

  // Published through c_reconcileDeliveryCountProperty.
  int m_reconcileDeliveryCount = 0;

  // Nesting depth of the host-side blocks. See isBlocked().
  int m_blockDepth = 0;

  // Nesting depth of the application-defined factory and widget callbacks
  // only. Kept apart from m_blockDepth so the reentrant-registration rejection
  // keeps meaning exactly "from inside a factory callback".
  int m_factoryCallbackDepth = 0;

  // Owed work, all coalescing. Each is set by an entry point which declined to
  // run while blocked and cleared by drainOwedWork().
  bool m_publishPending = false;

  bool m_geometrySyncPending = false;

  bool m_scrollApplyPending = false;

  bool m_replacementRetryPending = false;

  // Whether one drain is already armed, so an unblock edge arms exactly one.
  bool m_owedWorkScheduled = false;

  // Set while a drain is running its ordered steps, so the work those steps
  // owe is picked up by the drain itself or by its single follow-up delivery,
  // never by one timer per owed item.
  bool m_drainingOwedWork = false;

  // Identities whose replacement request was postponed, with the number of
  // retries already spent on each. Only host bookkeeping: exhausting the
  // budget drops the entry and nothing else.
  QHash<quint64, int> m_deferredReplacements;

  // Published through c_owedWorkDrainCountProperty.
  int m_owedWorkDrainCount = 0;

  // The scrollbar correction owed by realizations that have happened but whose
  // relayout has not been observed yet.
  //
  // Realizing an item above the viewport top replaces its estimated height
  // with the real one, which moves everything below it - including the text
  // the user is looking at - by the difference. Correcting inside the scroll
  // pass would use pre-relayout rectangles and could re-enter valueChanged
  // while the geometry guard is held, so the delta is accumulated here and
  // applied by the owed-work drain once the new range exists.
  //
  // Accumulated, not overwritten: one pass can realize several items, and a
  // second pass can run before the first correction has landed.
  bool m_realizationCompensationPending = false;

  qreal m_realizationCompensationDelta = 0;

  // The scrollbar value the accumulated correction was computed against. The
  // correction is only valid for that viewport position: if the bar has moved
  // by the time it is applied, a newer scroll has superseded it and it is
  // dropped rather than applied to a position nobody chose.
  int m_realizationCompensationOrigin = 0;

  // The performance counters published through the c_*Property names above.
  // Mutable because findIdentity() is const and its fallback is precisely the
  // thing being measured.
  int m_previewsBound = 0;

  int m_widgetsRealized = 0;

  int m_widgetsDestroyed = 0;

  int m_publishes = 0;

  int m_geometrySetCalls = 0;

  mutable int m_identityFallbackHits = 0;

  int m_rebuildAlls = 0;

  // Write every performance counter out to its dynamic property. Called at the
  // end of each pass which can have moved one, so a benchmark reading them
  // from a settled event loop always sees the whole window.
  void publishCounters();

  // The value of m_geometrySetCalls at the last publishCounters(), used by the
  // placement pass to skip publishing on a tick that placed nothing.
  //
  // The placement pass runs per scroll tick and must not pay nine QVariant
  // allocations and nine synchronous property-change dispatches for
  // diagnostics that did not change. It cannot simply skip publishing either:
  // pure scrolling owes no work, so no drain runs, and the counters would go
  // stale for the whole gesture.
  int m_publishedGeometrySetCalls = -1;

  // Zero every counter, including the process-wide table cell counter.
  void resetCounters();

  // Maximum number of times the host retries one postponed replacement on a
  // built-in sheet's behalf. The sheet's own debounce is the backstop after
  // that.
  static const int c_replacementRetryBudget;

  // Whether an application-defined factory or widget callback is running.
  bool inFactoryCallback() const { return m_factoryCallbackDepth > 0; }

  // Set while the factory registry itself is being mutated.
  bool m_mutatingFactories = false;

  // Set by syncWidgetGeometry() so publish() can tell whether the layout
  // already drove a sync through widgetPreviewGeometryChanged.
  bool m_geometrySynced = false;

  // Managed by QObject. Owned by this host like any other factory, and an
  // application may unregister it, which destroys it. A QPointer so the
  // per-publish applyReadOnly() cannot dereference it afterwards.
  QPointer<TablePreviewWidgetFactory> m_tableFactory;

  // --- Input modes inside the built-in table sheets (decisions D4, D6, D8). ---

  // Push the editor's configured input mode down to the built-in sheets, so
  // typing inside a previewed table behaves like typing outside one.
  //
  // Sourced from VTextEditor::modeChanged() plus a query of the editor's mode,
  // deliberately without a new signal (decision D8): modeChanged() carries no
  // argument and also fires on every Vi submode transition, so it is filtered
  // by the queried InputMode here.
  void applyInputMode();

  // The FOCUS DOMAIN owning @p_focus: the item identity, or 0 for the editor,
  // another window, or nothing.
  //
  // A domain is larger than the preview widget. A Vi command bar belongs to a
  // sheet's mode but is mounted in the EDITOR's status slot, so it is not a
  // descendant of the preview widget and identityForFocusWidget() answers 0
  // for it. Treating that as "the sheet lost the focus" would restore the
  // editor's mode and its status widget while the bar the user is typing in
  // still belongs to the sheet's.
  quint64 focusDomainFor(QWidget *p_focus) const;

  // Run the ordered, idempotent focus transition. Covers editor -> sheet,
  // sheet A -> sheet B, sheet -> its own status widget and back, sheet or
  // status widget -> editor, and everything -> nothing.
  void applyFocusDomain(QWidget *p_now);

  // Lend the editor's single status slot to @p_id's sheet, or take it back
  // when @p_id owns no sheet or no status widget (decision D4).
  //
  // Idempotent, and safe to call while the outgoing mode is being destroyed -
  // which is exactly when the sheet emits inputModeStatusWidgetChanged.
  void publishInputModeStatusWidget(quint64 p_id);

  // The table widget of @p_id, or null when @p_id has none.
  TablePreviewWidget *tableWidgetOf(quint64 p_id) const;

  // The last InputMode pushed down, so the noisy modeChanged() is filtered.
  InputMode m_lastInputMode = InputMode::NormalMode;

  bool m_inputModePushed = false;

  // The focus domain, which is an item identity or 0. Distinct from
  // m_focusedItemId, which is strictly the widget's own identity and drives
  // the cursor-line sync.
  quint64 m_focusDomainId = 0;

  // The identity whose status widget currently occupies the editor's slot, and
  // the widget itself - which is what tells the command bar apart from
  // anything else outside the preview widget.
  //
  // 0 whenever the slot holds the editor's own widget, INCLUDING while a sheet
  // is focused whose mode has no status widget at all (Normal and vscode have
  // none). So it answers "what is mounted", never "which sheet has the focus";
  // that is m_focusDomainId.
  quint64 m_inputModeStatusOwnerId = 0;

  QSharedPointer<InputModeStatusWidget> m_publishedStatusWidget;

  QPointer<QWidget> m_publishedStatusHostWidget;

  QMetaObject::Connection m_publishedStatusFocusConnection;
};
} // namespace vte

#endif // INTERACTIVEPREVIEWHOST_H
