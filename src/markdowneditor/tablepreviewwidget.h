#ifndef TABLEPREVIEWWIDGET_H
#define TABLEPREVIEWWIDGET_H

#include <QAbstractItemDelegate>
#include <QAbstractTableModel>
#include <QMetaObject>
#include <QPointer>
#include <QSet>
#include <QSize>
#include <QString>
#include <QStyleOptionViewItem>
#include <QTableView>
#include <QVector>

#include <vtextedit/preview.h>
#include <vtextedit/previewwidget.h>

class QLineEdit;
class QTimer;

namespace vte {
// Canonical Markdown serialization of an editable table sheet.
//
// The dialect targeted here is the one this cmark fork accepts: both the
// leading and the trailing pipe are mandatory.
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

// Editable sheet model over the raw Markdown cells of one table.
// Row 0 is the header row; the delimiter row stays metadata.
class TablePreviewModel : public QAbstractTableModel {
  Q_OBJECT
public:
  // Upper bounds on the sheet materialized for one table. cmark does not
  // truncate body rows to the header column count, so a single very wide row
  // would otherwise inflate the width of every other row and make the padded
  // matrix quadratic in the document size. Rows and columns are bounded
  // independently as well: the product alone still admits a 200000x1 sheet,
  // whose per-edit resizeRowsToContents() would block the GUI thread.
  static const int c_maxCells;

  static const int c_maxRows;

  static const int c_maxColumns;

  // Upper bound on the readable padding of one column. Longer cells are still
  // emitted in full - leftJustified() only pads - but they stop widening every
  // other row, which is what turns one very wide cell into a rows x width
  // blow-up the cell count limits above do not bound.
  static const int c_maxPaddedWidth;

  // Whether the normalized sheet of @p_table stays inside every bound above.
  static bool isWithinLimits(const TablePreview &p_table);

  // The number of cells the normalized sheet of @p_table would occupy.
  static qint64 normalizedCellCount(const TablePreview &p_table);

  // The width the normalized sheet of @p_table would have.
  static int normalizedColumnCount(const TablePreview &p_table);

  explicit TablePreviewModel(QObject *p_parent = nullptr);

  void setTable(const QSharedPointer<const TablePreview> &p_table);

  int rowCount(const QModelIndex &p_parent = QModelIndex()) const Q_DECL_OVERRIDE;

  int columnCount(const QModelIndex &p_parent = QModelIndex()) const Q_DECL_OVERRIDE;

  QVariant data(const QModelIndex &p_index, int p_role = Qt::DisplayRole) const Q_DECL_OVERRIDE;

  bool setData(const QModelIndex &p_index, const QVariant &p_value,
               int p_role = Qt::EditRole) Q_DECL_OVERRIDE;

  Qt::ItemFlags flags(const QModelIndex &p_index) const Q_DECL_OVERRIDE;

  // Whether the contents can be written back without changing what the table
  // renders to. A body row wider than the header declares would otherwise
  // promote its excess cells into the header and delimiter rows, silently
  // giving the table a column GFM currently ignores.
  bool isRoundTrippable() const;

  // Canonical Markdown of the current model contents.
  QString toMarkdown() const;

signals:
  // A cell actually changed value.
  void cellCommitted();

private:
  // Pad every row to the model width and expand the header/alignment rows.
  void normalize();

  QVector<QVector<QString>> m_cells;

  QVector<PreviewTableAlignment> m_alignments;

  QVector<QString> m_rowPrefixes;

  QString m_delimiterPrefix;

  int m_columnCount = 0;

  // Column count declared by the header and delimiter rows of the source.
  int m_declaredColumnCount = 0;
};

// Table view which wraps its cells, plans its own section geometry and only
// consumes wheel movement it can actually use.
class TablePreviewView : public QTableView {
  Q_OBJECT
public:
  static const QAbstractItemView::EditTriggers c_defaultEditTriggers;

  // Readable floor of one column, expressed in average characters of the
  // table font. Below this a compressed column stops being a column and
  // becomes a vertical ribbon of single letters.
  static const int c_minimumColumnCharacters;

  explicit TablePreviewView(QWidget *p_parent = nullptr);

  void setModel(QAbstractItemModel *p_model) Q_DECL_OVERRIDE;

  void setVisibleRows(int p_rows);

  // Intrinsic preferred size: the natural column widths, and the preferred
  // band measured against exactly those widths.
  QSize preferredSize() const;

  // The size the sheet settles on when its outer width is p_maxWidth,
  // including the horizontal scroll bar's height when even the column minimums
  // cannot fit. The host clamps the band to that width, so the bar the clamp
  // brings with it has to be reserved before the band is sized.
  QSize preferredSizeWithin(int p_maxWidth) const;

  QSize sizeHint() const Q_DECL_OVERRIDE;

  // Re-measure the contents and re-plan every section against the assigned
  // width. Supersedes resizeColumnsToContents(): it writes every section too,
  // but compressed into the width the sheet actually got, and it fits the
  // preferred band against exactly those widths afterwards.
  //
  // Fitting *every* row would walk the delegate over the whole model, and the
  // model is only bounded at c_maxRows, so on a large table it would cost
  // hundreds of thousands of size hints per reset - once per parse generation
  // while typing. Only the band feeds the geometry preferredSize() reports;
  // rows scrolled to later are fitted lazily by the row-fit pass.
  void layoutColumns();

  // Measuring walks every cell of the band through the delegate, and Qt
  // queries sizeHint() far more often than the contents change, so the result
  // is cached until something it depends on moves. Invalidating also arms the
  // host notification and drops the lazily fitted rows, whose heights were
  // measured against contents which no longer exist.
  void invalidatePreferredSize();

  // The readable floor one column is compressed to at most.
  int minimumColumnWidth() const;

  // Unhides the inherited overload set, which the three argument form below
  // would otherwise hide from callers of the one argument slot.
  using QTableView::edit;

signals:
  // The preferred geometry settled on something the host has not been told
  // about yet. Emitted only after an intrinsic invalidation, never for a
  // geometry-only pass, so applying the answer cannot feed back into it.
  void preferredGeometryChanged();

protected:
  void wheelEvent(QWheelEvent *p_event) Q_DECL_OVERRIDE;

  // A cell holds one line of plain text, so pointing at it is enough to start
  // editing it - the way clicking into a paragraph does. Qt would otherwise
  // need a second click or a double click, and would open the editor with the
  // whole value selected.
  void mousePressEvent(QMouseEvent *p_event) Q_DECL_OVERRIDE;

  // Overridden only to settle the editor once it exists, whichever trigger
  // brought it up.
  bool edit(const QModelIndex &p_index, EditTrigger p_trigger,
            QEvent *p_event) Q_DECL_OVERRIDE;

  void changeEvent(QEvent *p_event) Q_DECL_OVERRIDE;

  // Re-runs the layout, which is based on the viewport width. This is the hook
  // rather than resizeEvent() because the viewport also changes width on its
  // own: dropping the vertical scroll bar happens in the deferred geometry
  // pass, and on a widget which is not visible yet Qt only marks the resize
  // event as pending instead of delivering it. Missing that leaves the sheet
  // planned for a viewport it no longer has.
  void updateGeometries() Q_DECL_OVERRIDE;

  // Rows outside the preferred band still carry their default height, so
  // scrolling one into view is what asks for it to be fitted.
  void scrollContentsBy(int p_dx, int p_dy) Q_DECL_OVERRIDE;

private:
  // The cheap inputs the measurement depends on besides the cell contents.
  // Comparing them costs a handful of integer reads and catches the inherited
  // mutations which have no signal of their own (frame style, header minimum
  // section size, a swapped delegate), so the cache cannot go stale silently.
  struct PreferredSizeKey {
    int m_frame = -1;

    int m_minColumnWidth = -1;

    int m_minRowHeight = -1;

    int m_rows = -1;

    int m_columns = -1;

    int m_visibleRows = -1;

    // Qt adds a pixel to every section hint while the grid is drawn.
    bool m_showGrid = false;

    bool m_wordWrap = false;

    const QAbstractItemDelegate *m_delegate = nullptr;

    bool operator==(const PreferredSizeKey &p_other) const {
      return m_frame == p_other.m_frame && m_minColumnWidth == p_other.m_minColumnWidth &&
             m_minRowHeight == p_other.m_minRowHeight && m_rows == p_other.m_rows &&
             m_columns == p_other.m_columns && m_visibleRows == p_other.m_visibleRows &&
             m_showGrid == p_other.m_showGrid && m_wordWrap == p_other.m_wordWrap &&
             m_delegate == p_other.m_delegate;
    }
  };

  // One complete answer of the pure geometry solver. Nothing here touches the
  // live sections, so the same routine serves both the measurement the host
  // asks for and the layout pass which applies it.
  struct Solution {
    QVector<int> m_columnWidths;

    // Only the preferred band; the rows below it are fitted lazily.
    QVector<int> m_rowHeights;

    int m_viewportWidth = 0;

    int m_bandHeight = 0;

    // The planned columns do not fit, so a horizontal scroll bar appears and
    // eats into the band.
    bool m_horizontalScrollBar = false;
  };

  PreferredSizeKey currentPreferredSizeKey() const;

  // A style option carrying this view's font, palette, direction and state,
  // built once per measurement sweep because initViewItemOption() is far from
  // free and every cell of the band goes through it.
  QStyleOptionViewItem baseViewOption() const;

  // The delegate's hint for one cell. A width of -1 asks for the natural,
  // unwrapped size; anything else is the width the text may occupy.
  QSize cellHint(const QStyleOptionViewItem &p_base, int p_row, int p_column,
                 int p_width) const;

  // The section height one row needs when the columns have p_widths.
  int rowHeightFor(const QStyleOptionViewItem &p_base, int p_row,
                   const QVector<int> &p_widths) const;

  // What the vertical or horizontal bar actually takes from the viewport.
  // Answered from the live geometry once the sheet has been laid out with that
  // bar shown, because the arithmetic QAbstractScrollArea lays its children
  // out by is private and style dependent; until then, from the two rules that
  // arithmetic follows.
  int scrollBarExtent(Qt::Orientation p_orientation) const;

  // Learn the real chrome from the geometry the view just settled on, and flag
  // the answers the host already has when it disagrees with the estimate.
  void observeScrollBarChrome();

  int bandRowCount() const;

  // The open cell editor, if p_index has one. Qt keeps no public handle on it,
  // so it is found among the viewport's children by the geometry it was given
  // - which is exactly the cell's.
  QLineEdit *cellEditor(const QModelIndex &p_index) const;

  // Drop the selection Qt makes when the editor takes focus and put the caret
  // where the user pointed, or at the end when the editor was not opened by a
  // click. Selecting the whole value would turn the next keystroke into a
  // replace of the cell rather than an edit of it.
  void placeCursor(const QModelIndex &p_index, const QEvent *p_event);

  // Refresh the natural widths, the intrinsic band heights and the cached
  // preferred size when anything they depend on moved.
  void ensureMeasured() const;

  // Plan the sections for an outer width of p_outerWidth. A p_viewportWidth of
  // -1 derives the viewport from the outer width; passing an observed one is
  // how the live pass converges onto a style whose chrome does not match the
  // derivation. A non-positive p_outerWidth asks for the natural plan.
  Solution solveGeometry(int p_outerWidth, int p_viewportWidth) const;

  // The band a settled plan occupies at an outer width of p_width: the frame,
  // the rows, and whatever the horizontal bar takes when the columns cannot
  // fit. The single source of that arithmetic, because both the measurement
  // the host asks for and the layout pass seed the same memo with it.
  QSize constrainedSizeFor(const Solution &p_solution, int p_width) const;

  // Apply the plan - columns first, then the band rows, because Qt measures
  // wrapped rows against the current column widths.
  void applyLayout();

  // Const because the measurement notices inherited mutations which have no
  // signal of their own, and it has to be able to ask for a pass from there.
  void scheduleLayout() const;

  void scheduleRowFit();

  // Fit the rows around the viewport in bounded batches.
  void fitRowsAroundViewport();

  // Compare the settled geometry against what the host was last told, and
  // report a real change once.
  void notifyPreferredGeometry(const Solution &p_solution);

  int m_visibleRows = 10;

  // Only the connections this view made itself, so dropping a model cannot
  // tear down QTableView's own model connections.
  QVector<QMetaObject::Connection> m_modelConnections;

  mutable QSize m_cachedPreferredSize;

  // The per-column natural widths behind m_cachedPreferredSize, refreshed
  // under the same key guard. Reused by the solver because measuring walks the
  // delegate over the whole band and would otherwise run on every resize.
  mutable QVector<int> m_cachedColumnWidths;

  mutable QVector<int> m_cachedRowHeights;

  mutable PreferredSizeKey m_cachedPreferredSizeKey;

  mutable bool m_preferredSizeDirty = true;

  // preferredSizeWithin() is answered on every publish, so the constrained
  // answer is memoized per width alongside the intrinsic one.
  mutable int m_constrainedWidthKey = -1;

  mutable QSize m_constrainedSize;

  // setColumnWidth() can drop the vertical scroll bar, which resizes the
  // viewport and re-enters the geometry hooks below from inside the pass.
  bool m_layingOut = false;

  // setRowHeight() scrolls, which asks for another fitting pass from inside
  // the one already running.
  bool m_fittingRows = false;

  // The viewport width the live sections were last planned against. Writing a
  // section makes Qt post a geometry update for the very sections the pass
  // just wrote, and re-planning for that would only produce the same plan.
  int m_plannedViewportWidth = -1;

  // The chrome each bar was seen to take, or -1 while it is still estimated.
  int m_observedVerticalChrome = -1;

  int m_observedHorizontalChrome = -1;

  // Measurement bookkeeping, so an inherited mutation noticed from a const
  // query can drop it: the exact column widths m_fittedRows were measured
  // against, and the rows which are fitted for them. A row fitted for a
  // narrower plan is stale, not done.
  mutable QVector<int> m_fittedColumnWidths;

  mutable QSet<int> m_fittedRows;

  // Whether an intrinsic invalidation is still owed a notification.
  mutable bool m_notificationArmed = false;

  bool m_settlementEstablished = false;

  QSize m_settledPreferredSize;

  int m_settledConstrainedHeight = -1;

  // Managed by QObject.
  QTimer *m_layoutTimer = nullptr;

  // Managed by QObject.
  QTimer *m_rowFitTimer = nullptr;
};

class TablePreviewWidget : public PreviewWidget {
  Q_OBJECT
public:
  // Share of the available content width a sheet spans at least. Without it a
  // short table renders as a small box hugging the left margin, because the
  // host reserves a band exactly as wide as the natural contents.
  static const qreal c_widthFraction;

  TablePreviewWidget(PreviewWidgetContext *p_context, QWidget *p_parent);

  QVector<PreviewElementType> supportedTypes() const Q_DECL_OVERRIDE;

  bool setPreview(const QSharedPointer<const Preview> &p_preview) Q_DECL_OVERRIDE;

  qreal preferredWidthFraction() const Q_DECL_OVERRIDE;

  void setVisibleRows(int p_rows);

  // Mirror the editor's read-only state so the sheet cannot swallow edits the
  // host is going to reject anyway.
  void setReadOnly(bool p_readOnly);

  QSize sizeHint() const Q_DECL_OVERRIDE;

  bool hasHeightForWidth() const Q_DECL_OVERRIDE;

  int heightForWidth(int p_width) const Q_DECL_OVERRIDE;

  // The host's event filter watches this widget rather than the view inside
  // it, so the sheet's own layout request has to be posted here.
  bool event(QEvent *p_event) Q_DECL_OVERRIDE;

protected:
  void changeEvent(QEvent *p_event) Q_DECL_OVERRIDE;

private slots:
  void handleCellCommitted();

  void handleReplacementFinished(const vte::PreviewReplacementResult &p_result);

  // The sheet settled on a geometry the host has not measured yet.
  void handlePreferredGeometryChanged();

private:
  void resetFromSource();

  // A sheet only offers editing when the host would accept the commit.
  void applyEditTriggers();

  // Managed by QObject.
  TablePreviewView *m_view = nullptr;

  // Managed by QObject.
  TablePreviewModel *m_model = nullptr;

  QSharedPointer<const TablePreview> m_table;

  bool m_applyingSource = false;

  bool m_readOnly = false;

  // One posted layout request at a time, so a burst of settlements cannot
  // queue a burst of host measurements.
  bool m_layoutRequestPending = false;
};

class TablePreviewWidgetFactory : public PreviewWidgetFactory {
  Q_OBJECT
public:
  explicit TablePreviewWidgetFactory(QObject *p_parent = nullptr);

  QVector<PreviewElementType> supportedTypes() const Q_DECL_OVERRIDE;

  PreviewWidget *createWidget(PreviewWidgetContext *p_context,
                              const QSharedPointer<const Preview> &p_preview,
                              QWidget *p_parent) Q_DECL_OVERRIDE;

  void setVisibleRows(int p_rows);

  // Mirror the editor's read-only state onto every live sheet.
  void setReadOnly(bool p_readOnly);

private:
  // Drop the entries whose sheet has been destroyed.
  void pruneWidgets();

  int m_visibleRows = 10;

  bool m_readOnly = false;

  QVector<QPointer<TablePreviewWidget>> m_widgets;
};
} // namespace vte

#endif // TABLEPREVIEWWIDGET_H
