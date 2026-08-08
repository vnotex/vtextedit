#ifndef TABLEPREVIEWWIDGET_H
#define TABLEPREVIEWWIDGET_H

#include <QAbstractItemDelegate>
#include <QAbstractTableModel>
#include <QMetaObject>
#include <QPointer>
#include <QSize>
#include <QString>
#include <QTableView>
#include <QVector>

#include <vtextedit/preview.h>
#include <vtextedit/previewwidget.h>

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

// Table view which only consumes wheel movement it can actually use.
class TablePreviewView : public QTableView {
  Q_OBJECT
public:
  static const QAbstractItemView::EditTriggers c_defaultEditTriggers;

  explicit TablePreviewView(QWidget *p_parent = nullptr);

  void setModel(QAbstractItemModel *p_model) Q_DECL_OVERRIDE;

  void setVisibleRows(int p_rows);

  // Fit the rows the sheet can actually show.
  //
  // QTableView::resizeRowsToContents() walks *every* row through the delegate,
  // and the model is only bounded at c_maxRows, so on a large table it costs
  // hundreds of thousands of size hints per reset - once per parse generation
  // while typing. Only this window feeds the geometry preferredSize() reports;
  // rows scrolled to later keep the header's default height, which is correct
  // for the single-line cells a Markdown table can hold.
  void resizeVisibleRowsToContents();

  QSize preferredSize() const;

  // preferredSize(), plus the horizontal scroll bar's height when the sheet
  // will not fit in p_maxWidth. The host clamps the band to that width, so the
  // bar the clamp brings with it has to be reserved before the band is sized.
  QSize preferredSizeWithin(int p_maxWidth) const;

  QSize sizeHint() const Q_DECL_OVERRIDE;

  // Re-measure the contents and hand every column its share of the assigned
  // width. Supersedes resizeColumnsToContents(): it writes every section too,
  // but scaled to the width the sheet actually got.
  void layoutColumns();

  // Measuring walks every cell through the delegate, and Qt queries sizeHint()
  // far more often than the contents change, so the result is cached until
  // something it depends on moves.
  void invalidatePreferredSize();

protected:
  void wheelEvent(QWheelEvent *p_event) Q_DECL_OVERRIDE;

  void changeEvent(QEvent *p_event) Q_DECL_OVERRIDE;

  // Re-runs the distribution, which is based on the viewport width. This is
  // the hook rather than resizeEvent() because the viewport also changes width
  // on its own: dropping the vertical scroll bar happens in the deferred
  // geometry pass, and on a widget which is not visible yet Qt only marks the
  // resize event as pending instead of delivering it. Missing that leaves the
  // sheet distributed for a viewport it no longer has, with the stretched last
  // section quietly absorbing the difference.
  void updateGeometries() Q_DECL_OVERRIDE;

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

  PreferredSizeKey currentPreferredSizeKey() const;

  // Give every column its share of the assigned width.
  void distributeColumnWidths();

  // Coalesce a burst of model signals into one distribution pass, and keep the
  // pass out of the emission itself.
  void scheduleColumnLayout();

  int m_visibleRows = 10;

  // Only the connections this view made itself, so dropping a model cannot
  // tear down QTableView's own model connections.
  QVector<QMetaObject::Connection> m_modelConnections;

  mutable QSize m_cachedPreferredSize;

  // The per-column content widths behind m_cachedPreferredSize, refreshed
  // under the same key guard. Reused by the distribution because
  // sizeHintForColumn() walks the delegate and would otherwise run on every
  // resize event.
  mutable QVector<int> m_cachedColumnWidths;

  mutable PreferredSizeKey m_cachedPreferredSizeKey;

  mutable bool m_preferredSizeDirty = true;

  // setColumnWidth() can drop the vertical scroll bar, which resizes the
  // viewport and re-enters the geometry hooks below from inside the pass.
  bool m_distributingColumns = false;

  // Managed by QObject.
  QTimer *m_columnLayoutTimer = nullptr;
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

protected:
  void changeEvent(QEvent *p_event) Q_DECL_OVERRIDE;

private slots:
  void handleCellCommitted();

  void handleReplacementFinished(const vte::PreviewReplacementResult &p_result);

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
