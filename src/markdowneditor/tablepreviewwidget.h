#ifndef TABLEPREVIEWWIDGET_H
#define TABLEPREVIEWWIDGET_H

#include <QPointer>
#include <QScopedPointer>
#include <QSize>
#include <QString>
#include <QTextEdit>
#include <QVector>

#include <vtextedit/preview.h>
#include <vtextedit/previewwidget.h>

class QTextDocument;
class QTextTable;
class QTimer;

namespace vte {
// Why a sheet is handing the caret back to the editor, and where the caret
// should land when it gets there.
//
// The destination is deliberately not a document position: a sheet only knows
// the snapshot coordinates of its own source, and those go stale on any
// unrelated edit. Only the host holds a live anchor, so it resolves the
// position at delivery time.
enum class FocusEscapeDirection {
  // Escape: the editor takes the focus back and the caret stays where it was.
  Keep,

  // Up out of the first row. The Markdown source is rendered above the sheet
  // (PreviewPlacement::BlockAfterSource), so "up" is the end of the source.
  Up,

  // Down out of the last row, i.e. just past the source's own line.
  Down
};

// Canonical Markdown serialization of an editable table sheet.
//
// The dialect targeted here is the one this cmark fork accepts: both the
// leading and the trailing pipe are mandatory.
//
// The canonical form is compact: every cell is emitted with exactly one space
// inside each border and columns are never padded to a common width, so a
// single long cell cannot amplify the source by the row count.
class TablePreviewSerializer {
public:
  // Escape a '|' only when it is preceded by an even number of backslashes.
  // Idempotent: already escaped pipes are left untouched.
  static QString escapeCell(const QString &p_cell);

  // Whether the row prefixes can be reproduced without corrupting nesting.
  // The first row may carry a list marker; every following row (including the
  // delimiter row) must share one identical whitespace/block-quote prefix.
  static bool arePrefixesSafe(const QVector<QString> &p_rowPrefixes,
                              const QString &p_delimiterPrefix);

  // Returns an empty string when the table cannot be serialized safely.
  static QString serialize(const QVector<QVector<QString>> &p_cells,
                           const QVector<PreviewTableAlignment> &p_alignments,
                           const QVector<QString> &p_rowPrefixes,
                           const QString &p_delimiterPrefix);
};

// The rich text document behind one editable sheet: a QTextDocument holding a
// single QTextTable, plus the metadata a QTextTable cannot carry.
//
// The document is the single source of truth. There is no parallel cell
// matrix: the cells are derived from the table on demand, so a caret sitting
// in a half-typed cell can never disagree with what is about to be serialized.
class TablePreviewDocument {
public:
  // Upper bound on the number of cells one sheet materializes.
  //
  // Measured, not guessed. A QTextTable is not virtualized: every cell is a
  // QTextBlock, the sheet renders at its full natural height, and Qt relays
  // the whole table out on any change inside it. On a release build the cost
  // is linear in the cell count and independent of the shape - at a constant
  // 300 cells, 300x1, 30x10 and 1x300 all land within 10.6 to 17.4 ms - at
  // roughly 0.055 ms per cell for the first layout, for a width reflow and,
  // crucially, for every single keystroke:
  //
  //     cells     first layout   reflow   one keystroke
  //       200            10 ms    10 ms         10.6 ms
  //       300            16 ms    16 ms         16.5 ms
  //       500            28 ms    29 ms         29.1 ms
  //      2000           136 ms   140 ms          138 ms
  //    200000         36702 ms 36367 ms        36386 ms
  //
  // 300 cells is therefore the last shape whose keystroke still costs about
  // one 16 ms frame rather than a multiple of it. The previous QTableView
  // sheet could afford 200000 because it was virtualized and fitted only the
  // rows it actually showed; this substrate cannot, so the bound is roughly
  // three orders of magnitude lower. A table over it is left to the static
  // source rendering, exactly as before.
  static const int c_maxCells;

  // Upper bound on the number of columns.
  //
  // Not a latency bound - at a constant cell count the shape does not affect
  // the layout cost at all - but it still binds on its own for a short table:
  // a one-row 250-column table is 250 cells, inside the bound above, and is
  // refused here. Inherited unchanged from the QTableView sheet.
  //
  // There is deliberately no matching row bound: with c_maxCells at 300, more
  // than 300 rows already means more than 300 cells for any table with at
  // least one column, so a row bound could never decide anything.
  static const int c_maxColumns;

  // Whether the normalized sheet of @p_table stays inside every bound above.
  static bool isWithinLimits(const TablePreview &p_table);

  // The number of cells the normalized sheet of @p_table would occupy.
  static qint64 normalizedCellCount(const TablePreview &p_table);

  // The width the normalized sheet of @p_table would have.
  static int normalizedColumnCount(const TablePreview &p_table);

  TablePreviewDocument();

  ~TablePreviewDocument();

  // Never null. Not parented, so whoever renders it must not outlive this.
  QTextDocument *document() const;

  // Null until the first setTable().
  QTextTable *table() const;

  // Rebuild the document from @p_table, normalizing the ragged matrix exactly
  // as the source snapshot describes it: every row padded to the widest row,
  // the alignments extended with PreviewTableAlignment::None.
  void setTable(const QSharedPointer<const TablePreview> &p_table);

  int rowCount() const;

  int columnCount() const;

  // The cell matrix, derived from the live table.
  QVector<QVector<QString>> cells() const;

  // Whether the contents can be written back without changing what the table
  // renders to. A body row wider than the header declares would otherwise
  // promote its excess cells into the header and delimiter rows, silently
  // giving the table a column GFM currently ignores.
  bool isRoundTrippable() const;

  // Whether the document still holds a table at all.
  //
  // The last line of defence. Every mutation is supposed to be confined to one
  // cell, because removing a selection which crosses a frame boundary removes
  // the frame - and this object would then be holding a dangling QTextTable.
  // Re-resolved from the document rather than compared against the cached
  // pointer, which is exactly what dangles in the case this detects.
  bool isIntact() const;

  // Canonical Markdown of the current document contents.
  QString toMarkdown() const;

  // Re-apply the per-cell formats - the column alignment and the header
  // weight - without touching a single character of cell text.
  //
  // A font, palette or style change has to be answered without rebuilding the
  // document: rebuilding would destroy the caret and any selection, which for
  // a theme switch during typing is a silent loss of the user's place.
  void applyCellFormats();

  // The one colour the table itself owns. Separate from applyCellFormats()
  // because a rebuild has just written every per-cell format and only this is
  // still owed.
  void applyPalette(const QPalette &p_palette);

private:
  void build();

  // The single writer of one cell's formats, so build() and applyCellFormats()
  // cannot drift apart.
  void applyCellFormat(int p_row, int p_column);

  // The block format one column's cells carry.
  Qt::Alignment blockAlignment(int p_column) const;

  QScopedPointer<QTextDocument> m_doc;

  // Owned by m_doc.
  QTextTable *m_table = nullptr;

  // The raw matrix of the bound snapshot, normalized. Only used to build the
  // document; cells() reads the document itself afterwards.
  QVector<QVector<QString>> m_source;

  QVector<PreviewTableAlignment> m_alignments;

  QVector<QString> m_rowPrefixes;

  QString m_delimiterPrefix;

  int m_rowCount = 0;

  int m_columnCount = 0;

  // Column count declared by the header and delimiter rows of the source.
  int m_declaredColumnCount = 0;
};

// The QTextEdit the sheet is rendered and edited in.
//
// One caret roams every cell, there is no edit mode, and wrapping plus
// height-for-width come from Qt's rich text layout rather than a bespoke
// solver. It never scrolls internally: both bars are off, the sheet renders at
// its full natural height and every wheel movement belongs to the editor
// underneath.
class TablePreviewSheet : public QTextEdit {
  Q_OBJECT
public:
  explicit TablePreviewSheet(QWidget *p_parent = nullptr);

  // The document this sheet renders. Not owned.
  void setTableDocument(TablePreviewDocument *p_document);

  // The height the sheet needs when its *outer* width is p_outerWidth. The
  // conversion to a document text width is not optional: assigning the outer
  // width straight to QTextDocument::setTextWidth() under-measures, and with
  // the scroll bars off the clipped content would be unreachable.
  int heightForWidth(int p_outerWidth) const Q_DECL_OVERRIDE;

  bool hasHeightForWidth() const Q_DECL_OVERRIDE;

  // Width 0 on purpose: preferredWidthFraction() is 1.0, so the host resolves
  // the band to the full available content width.
  QSize sizeHint() const Q_DECL_OVERRIDE;

  QSize minimumSizeHint() const Q_DECL_OVERRIDE;

  // Row major index of the cell holding the caret, or -1 when it is outside
  // the table.
  int currentCellIndex() const;

  // Commit whatever the input method is still holding in a preedit.
  //
  // A preedit is not in the document yet, so serializing across one would
  // silently drop the characters the user has already typed. A no-op unless
  // this sheet owns the input focus: QInputMethod acts on the application's
  // focus object, and a flush also runs on background sheets.
  void commitPreedit();

  // Cancel an open composition, on the platform and in the text control.
  //
  // Called just before the sheet becomes a viewer, which refuses every input
  // method event outright: a preedit still installed at that point would be
  // left rendered over a sheet which can never accept it, and the platform and
  // the control would start the next composition from different states.
  void cancelComposition();

  // Re-apply everything derived from the palette and the font, including the
  // per-cell formats.
  void refreshFormats();

  // Only the palette-derived parts. Used right after a rebuild, which has
  // already written every per-cell format.
  void refreshPalette();

  // Put the caret back inside the table when something parked it in the empty
  // block a QTextDocument always keeps after a table.
  void clampCursorIntoTable();

  // Confine a selection to the cell holding the caret.
  //
  // QTextCursor::removeSelectedText() over a range which crosses a frame
  // boundary removes the frame, so a Ctrl+A followed by one keystroke, a
  // Delete, a Cut, a paste or an internal drag would take the whole table out
  // of the document - and TablePreviewDocument would be left holding a
  // dangling QTextTable. Clamping the selection itself is the single gate
  // which covers every one of those paths, including the ones which come from
  // QTextEdit's own context menu and never pass through keyPressEvent(). The
  // retired QTableView sheet was SingleSelection for the same reason, so no
  // affordance is lost.
  void clampSelectionIntoOneCell();

signals:
  // The document's laid out size settled on something the host has not been
  // told about yet.
  void preferredGeometryChanged();

  void focusEscapeRequested(vte::FocusEscapeDirection p_direction);

  void undoRequested();

  void redoRequested();

  // The caret left the cell it was in, which commits that cell.
  void cellLeft();

  void focusLost();

protected:
  void keyPressEvent(QKeyEvent *p_event) Q_DECL_OVERRIDE;

  // Every wheel movement belongs to the editor underneath: the sheet has no
  // scroll bars and nothing it could scroll.
  void wheelEvent(QWheelEvent *p_event) Q_DECL_OVERRIDE;

  void focusInEvent(QFocusEvent *p_event) Q_DECL_OVERRIDE;

  void focusOutEvent(QFocusEvent *p_event) Q_DECL_OVERRIDE;

  void mousePressEvent(QMouseEvent *p_event) Q_DECL_OVERRIDE;

  void resizeEvent(QResizeEvent *p_event) Q_DECL_OVERRIDE;

  // A committed input method string is an insertion like any other: it can
  // carry a line separator, and both its replacement range and any Selection
  // attribute it carries are resolved by the base handler in an intermediate
  // document state, independently of the selection - so neither is covered by
  // the clamp and both are handled here.
  void inputMethodEvent(QInputMethodEvent *p_event) Q_DECL_OVERRIDE;

  // Plain text only, and never a line separator: the serializer rejects
  // '\n', '\r', U+2028 and U+2029, so a cell which contains one can no longer
  // be written back at all.
  bool canInsertFromMimeData(const QMimeData *p_source) const Q_DECL_OVERRIDE;

  void insertFromMimeData(const QMimeData *p_source) Q_DECL_OVERRIDE;

private slots:
  void handleDocumentSizeChanged();

  void handleCursorPositionChanged();

  // QTextEdit::selectAll() - and therefore Ctrl+A and the context menu's
  // Select All - moves the anchor without moving the caret's *position*, so it
  // emits selectionChanged() and no cursorPositionChanged() at all. The
  // selection clamp has to be reachable from both.
  void handleSelectionChanged();

private:
  // What the frame and the viewport margins take off the outer width and
  // height. Read from the live geometry once there is one, because a style may
  // inset the viewport by more than the frame width alone.
  int horizontalChrome() const;

  int verticalChrome() const;

  // Move the caret to the next or previous cell, wrapping at the last/first
  // one. Returns false when there is no table to move in.
  bool moveToAdjacentCell(bool p_forward);

  // Perform one of the delete shortcuts here, bounded by the caret's cell.
  //
  // QWidgetTextControl answers these by building a selection *and* removing it
  // inside the same call, on its own cursor and without emitting a signal in
  // between - so a clamp before the base handler sees no selection yet and the
  // signal-driven clamps never run at all. Ctrl+Delete at the end of a cell
  // selects into the next one, and at the end of the last cell out of the
  // table entirely, where QTextCursor expands the range to the whole frame.
  // Returns true when @p_event was one of them and has been handled.
  bool deleteWithinCell(const QKeyEvent *p_event);

  // Whether the caret is on the first visual line of the first row, or the
  // last visual line of the last row - the two edges at which an arrow key
  // hands the caret back to the editor.
  bool isAtTopEdge() const;

  bool isAtBottomEdge() const;

  // Re-seed the sheet's palette from its parent and hand the derived colours
  // to the document. Returns the effective palette.
  QPalette applyPalette();

  // Not owned.
  TablePreviewDocument *m_document = nullptr;

  // heightForWidth() lays the live document out, which synchronously emits
  // documentSizeChanged. Reporting that back to the host is how a measurement
  // loop starts.
  mutable bool m_measuring = false;

  // Set while the sheet is applying a geometry the host chose, for the same
  // reason.
  bool m_applyingGeometry = false;

  // Set while refreshFormats() rewrites the character and block formats, which
  // is a document change the commit machinery must not see as an edit.
  bool m_applyingFormats = false;

  // The last (outer width, height) pair reported to the host. An unchanged
  // pass terminates here instead of looping against the host's own cache.
  int m_notifiedOuterWidth = -1;

  int m_notifiedHeight = -1;

  // Set while a clamp is rewriting the cursor, whose own
  // cursorPositionChanged would otherwise re-enter it.
  bool m_clampingCursor = false;

  // Set across QInputMethod::reset(), which several platforms answer by
  // sending this very widget a synchronous commit or clearing event.
  bool m_cancellingComposition = false;

  // The cell the caret was last seen in, so cellLeft() fires once per move
  // rather than once per keystroke.
  int m_lastCellIndex = -1;
};

class TablePreviewWidget : public PreviewWidget {
  Q_OBJECT
public:
  // Outcome of a write-back attempt.
  enum class FlushOutcome {
    // Nothing is owed anymore: the commit was accepted, there was nothing to
    // commit, or the sheet is not in a state where it may commit at all.
    Settled,

    // The host could not apply the request right now and nothing was touched.
    // The sheet keeps its authority, its edit and its debounce, and the write
    // back is retried later.
    Deferred,

    // The request was answered with a rejection.
    Rejected
  };

  // Share of the available content width a sheet spans. The whole band: the
  // column widths are owned by Qt's table layout now, which distributes
  // whatever width it is given, so there is no natural width to fall back to.
  static const qreal c_widthFraction;

  // Idle time after the last keystroke before the sheet writes itself back.
  static const int c_commitDebounceMs;

  TablePreviewWidget(PreviewWidgetContext *p_context, QWidget *p_parent);

  ~TablePreviewWidget() Q_DECL_OVERRIDE;

  QVector<PreviewElementType> supportedTypes() const Q_DECL_OVERRIDE;

  bool setPreview(const QSharedPointer<const Preview> &p_preview) Q_DECL_OVERRIDE;

  qreal preferredWidthFraction() const Q_DECL_OVERRIDE;

  // Mirror the editor's read-only state so the sheet cannot swallow edits the
  // host is going to reject anyway.
  void setReadOnly(bool p_readOnly);

  // Write back anything the debounce still owes, while this sheet's context
  // and anchor are still authoritative. The host calls it immediately before
  // it drops the identity, and again before it snapshots the live anchors for
  // a rebuild - an accepted flush retargets the anchor, and a copy taken
  // beforehand would have been collapsed by the very edit it applies.
  FlushOutcome flushNow();

  // The host has revoked this sheet's authority: the identity is about to be
  // erased, so every remaining commit path has to become a no-op. A late
  // timeout, a hide, a focus-out and the deferred deletion all pass through
  // here.
  void revokeAuthority();

  QSize sizeHint() const Q_DECL_OVERRIDE;

  bool hasHeightForWidth() const Q_DECL_OVERRIDE;

  int heightForWidth(int p_width) const Q_DECL_OVERRIDE;

  // The host's event filter watches this widget rather than the sheet inside
  // it, so the sheet's own layout request has to be posted here.
  bool event(QEvent *p_event) Q_DECL_OVERRIDE;

signals:
  void focusEscapeRequested(vte::FocusEscapeDirection p_direction);

  void undoRequested();

  void redoRequested();

protected:
  void changeEvent(QEvent *p_event) Q_DECL_OVERRIDE;

private slots:
  void handleContentsChanged();

  void handleCellLeft();

  void handleFocusLost();

  void handleEscapeRequested(vte::FocusEscapeDirection p_direction);

  void handleUndoRequested();

  void handleRedoRequested();

  void handleReplacementFinished(const vte::PreviewReplacementResult &p_result);

  // The sheet settled on a geometry the host has not measured yet.
  void handlePreferredGeometryChanged();

  void handleCommitTimeout();

private:
  void resetFromSource();

  // Take the bound snapshot from the context, which is the authoritative
  // binding: an accepted replacement rebases it onto the text now in the
  // document, and the cached one still describes the pre-commit source.
  void rebindFromContext();

  // Restart the idle debounce.
  void armCommit();

  // Write the sheet back now. "Nothing to commit" is Settled; a request the
  // host postponed is Deferred, which the caller must not treat as a loss.
  FlushOutcome flushPendingCommit();

  // A sheet only offers editing when the host would accept the commit.
  void applyEditability();

  // Managed by QObject.
  TablePreviewSheet *m_sheet = nullptr;

  // Destroyed after m_sheet, which renders it. See the destructor.
  QScopedPointer<TablePreviewDocument> m_document;

  // Managed by QObject.
  QTimer *m_commitTimer = nullptr;

  QSharedPointer<const TablePreview> m_table;

  // Set while the sheet is being rewritten from the bound snapshot, so the
  // resulting document changes are not mistaken for user edits.
  bool m_applyingSource = false;

  bool m_readOnly = false;

  // Whether the bound snapshot still describes the document. Cleared by a
  // rejection the sheet cannot recover from, restored by the next snapshot.
  bool m_authoritative = true;

  // Set by revokeAuthority(). Terminal: nothing clears it.
  bool m_suppressed = false;

  // One posted layout request at a time, so a burst of settlements cannot
  // queue a burst of host measurements.
  bool m_layoutRequestPending = false;

  // Monotonic counter of accepted document changes. The commit machinery
  // compares generations rather than text, because the document can move on
  // while a replacement is in flight.
  quint64 m_editGeneration = 0;

  // The generation the last accepted commit carried.
  quint64 m_committedGeneration = 0;

  quint64 m_inFlightGeneration = 0;

  bool m_commitInFlight = false;

  // The outcome the last completion recorded, written synchronously by
  // handleReplacementFinished() so flushPendingCommit() can tell a postponed
  // request from a rejected one - the committed generation alone collapses
  // both into "not settled".
  FlushOutcome m_lastFlushOutcome = FlushOutcome::Settled;

  QString m_inFlightMarkdown;

  // The Markdown this sheet last knew the document to hold: the canonical form
  // of the bound source at bind time, and the accepted replacement after every
  // successful commit. It is both the "nothing changed" baseline a flush
  // compares against and the echo baseline setPreview() needs, so a parse
  // generation which merely replays this sheet's own commit is not mistaken
  // for an authoritative external change.
  QString m_committedMarkdown;
};

class TablePreviewWidgetFactory : public PreviewWidgetFactory {
  Q_OBJECT
public:
  explicit TablePreviewWidgetFactory(QObject *p_parent = nullptr);

  QVector<PreviewElementType> supportedTypes() const Q_DECL_OVERRIDE;

  PreviewWidget *createWidget(PreviewWidgetContext *p_context,
                              const QSharedPointer<const Preview> &p_preview,
                              QWidget *p_parent) Q_DECL_OVERRIDE;

  // Mirror the editor's read-only state onto every live sheet.
  void setReadOnly(bool p_readOnly);

signals:
  // Relayed from a live sheet, carrying the widget so the host can resolve the
  // identity - and therefore the live anchor - it belongs to.
  void focusEscapeRequested(vte::TablePreviewWidget *p_widget,
                            vte::FocusEscapeDirection p_direction);

  void undoRequested(vte::TablePreviewWidget *p_widget);

  void redoRequested(vte::TablePreviewWidget *p_widget);

private:
  // Drop the entries whose sheet has been destroyed.
  void pruneWidgets();

  bool m_readOnly = false;

  QVector<QPointer<TablePreviewWidget>> m_widgets;
};
} // namespace vte

#endif // TABLEPREVIEWWIDGET_H
