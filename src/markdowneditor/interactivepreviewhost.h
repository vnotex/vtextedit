#ifndef INTERACTIVEPREVIEWHOST_H
#define INTERACTIVEPREVIEWHOST_H

#include <QHash>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QRectF>
#include <QSet>
#include <QSizeF>
#include <QTextCursor>
#include <QVector>

#include <vtextedit/preview.h>
#include <vtextedit/previewwidget.h>

#include "markdownfoldingprovider.h"
#include "textdocumentlayout.h"

class QTimer;
class QTextDocument;

namespace vte {
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

  bool isTypeEnabled(PreviewElementType p_type) const;

  // Dynamic property carrying the enabled element type mask to the
  // highlighter. A property avoids adding a data member to the exported
  // MarkdownHighlighter, which would break its ABI.
  static const char *c_enabledTypeMaskProperty;

  // Number of reconcile deliveries that have run, published as a dynamic
  // property on the host so a test can tell a consumed blocked delivery from a
  // re-armed spin. A blocked delivery calls into no factory and no widget, so
  // it is otherwise invisible from outside.
  static const char *c_reconcileDeliveryCountProperty;

  // Number of preview driven fold evaluations that have actually run. Same
  // mechanism as the reconcile counter above, but deliberately not the same
  // convention: that one counts every queued delivery, including the blocked
  // ones it declines, while this one is incremented past the guard and
  // therefore counts only the passes which ran. The two deferral tests rely on
  // that difference.
  static const char *c_foldRefreshCountProperty;

  // Block extent, type and fold state of every element which has a widget.
  QVector<PreviewedRange> previewedRanges() const;

  // Write back what the folding provider decided for each identity. Identities
  // which are gone are ignored.
  void setPreviewFoldStates(const QVector<QPair<quint64, PreviewFoldState>> &p_states);

  // Coalesced, blocked-aware re-evaluation of the preview driven fold state.
  void scheduleFoldRefresh();

public slots:
  void updatePreviews(quint64 p_revision,
                      const QVector<QSharedPointer<const Preview>> &p_previews);

  // Recompute preferred sizes, resubmit layout reservations and reposition
  // every widget. Coalesced through a zero timer.
  void schedulePublish();

protected:
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

  void reconcileLater();

private:
  // Marks the span of an application-defined virtual callback, during which
  // the factory registry must not be mutated by reentrant registration.
  class CallbackGuard {
  public:
    explicit CallbackGuard(InteractivePreviewHost *p_host) : m_host(p_host) {
      m_previous = m_host->m_inFactoryCallback;
      m_host->m_inFactoryCallback = true;
    }

    ~CallbackGuard();

  private:
    InteractivePreviewHost *m_host = nullptr;

    bool m_previous = false;
  };

  // A registered factory. Removal erases the entry, so presence in
  // m_factories is what "still active" means.
  struct FactoryEntry {
    QPointer<PreviewWidgetFactory> m_factory;

    int m_priority = 0;

    // Registration order, used to break priority ties.
    int m_order = 0;
  };

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
    QSizeF m_measuredSize;

    qreal m_measuredWidthBasis = -1;

    bool m_measureDirty = true;

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

  QTextCursor makeAnchor(int p_startPos, int p_endPos) const;

  // Ask every live table sheet to write back what its debounce still owes,
  // while its context and its anchor are still authoritative.
  void flushDirtySheets();

  // Rebuild every item, carrying the live anchors across so a factory or
  // enablement change never re-anchors at superseded parse positions.
  void rebuildAll();

  // Queue one delivery of the owed rebuild, unless one is already queued or a
  // callback is holding the host blocked. Arming while blocked would busy-spin
  // inside a nested event loop, because the blocking flag can only be cleared
  // by a stack frame that loop is holding open.
  void scheduleReconcileDelivery();

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

  bool m_reconcileScheduled = false;

  // Whether a preview driven fold re-evaluation is owed, and whether one is
  // already queued. Kept apart for the same reason as the reconcile pair: an
  // unblock hook has to be able to re-arm exactly one delivery.
  bool m_foldRefreshPending = false;

  bool m_foldRefreshScheduled = false;

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

  // Set while an application-defined factory or widget callback is running.
  bool m_inFactoryCallback = false;

  // Set while the factory registry itself is being mutated.
  bool m_mutatingFactories = false;

  // Set by syncWidgetGeometry() so publish() can tell whether the layout
  // already drove a sync through widgetPreviewGeometryChanged.
  bool m_geometrySynced = false;

  // Managed by QObject.
  QTimer *m_publishTimer = nullptr;

  // Managed by QObject. Owned by this host like any other factory, and an
  // application may unregister it, which destroys it. A QPointer so the
  // per-publish applyReadOnly() cannot dereference it afterwards.
  QPointer<TablePreviewWidgetFactory> m_tableFactory;
};
} // namespace vte

#endif // INTERACTIVEPREVIEWHOST_H
