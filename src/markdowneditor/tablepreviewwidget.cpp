#include "tablepreviewwidget.h"

#include <QApplication>
#include <QFont>
#include <QHeaderView>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QStringList>
#include <QStyle>
#include <QStyleOption>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include "previewlogging.h"

using namespace vte;

// ---------------------------------------------------------------------------
// TablePreviewSerializer
// ---------------------------------------------------------------------------

QString TablePreviewSerializer::escapeCell(const QString &p_cell) {
  QString result;
  result.reserve(p_cell.size());

  int backslashes = 0;
  for (int i = 0; i < p_cell.size(); ++i) {
    const QChar ch = p_cell.at(i);
    if (ch == QLatin1Char('|') && (backslashes % 2) == 0) {
      result.append(QLatin1Char('\\'));
    }

    result.append(ch);

    if (ch == QLatin1Char('\\')) {
      ++backslashes;
    } else {
      backslashes = 0;
    }
  }

  return result;
}

static bool isContinuationPrefix(const QString &p_prefix) {
  for (int i = 0; i < p_prefix.size(); ++i) {
    const QChar ch = p_prefix.at(i);
    if (ch != QLatin1Char(' ') && ch != QLatin1Char('\t') && ch != QLatin1Char('>')) {
      return false;
    }
  }

  return true;
}

bool TablePreviewSerializer::arePrefixesSafe(const QVector<QString> &p_rowPrefixes,
                                             const QString &p_delimiterPrefix) {
  if (p_rowPrefixes.isEmpty()) {
    return false;
  }

  // Every row after the header shares the delimiter row's prefix, which must
  // not contain a list marker: repeating a marker would create new list items.
  if (!isContinuationPrefix(p_delimiterPrefix)) {
    return false;
  }

  for (int i = 1; i < p_rowPrefixes.size(); ++i) {
    if (p_rowPrefixes[i] != p_delimiterPrefix) {
      return false;
    }
  }

  return true;
}

static int alignmentMinimumWidth(PreviewTableAlignment p_alignment) {
  switch (p_alignment) {
  case PreviewTableAlignment::Left:
  case PreviewTableAlignment::Right:
    return 4;
  case PreviewTableAlignment::Center:
    return 5;
  default:
    return 3;
  }
}

static QString alignmentMarker(PreviewTableAlignment p_alignment, int p_width) {
  switch (p_alignment) {
  case PreviewTableAlignment::Left:
    return QLatin1Char(':') + QString(p_width - 1, QLatin1Char('-'));
  case PreviewTableAlignment::Right:
    return QString(p_width - 1, QLatin1Char('-')) + QLatin1Char(':');
  case PreviewTableAlignment::Center:
    return QLatin1Char(':') + QString(p_width - 2, QLatin1Char('-')) + QLatin1Char(':');
  default:
    return QString(p_width, QLatin1Char('-'));
  }
}

static bool hasLineSeparator(const QString &p_text) {
  for (int i = 0; i < p_text.size(); ++i) {
    const ushort code = p_text.at(i).unicode();
    if (code == '\n' || code == '\r' || code == 0x2028 || code == 0x2029) {
      return true;
    }
  }

  return false;
}

QString TablePreviewSerializer::serialize(const QVector<QVector<QString>> &p_cells,
                                          const QVector<PreviewTableAlignment> &p_alignments,
                                          const QVector<QString> &p_rowPrefixes,
                                          const QString &p_delimiterPrefix) {
  if (p_cells.isEmpty() || p_cells.size() != p_rowPrefixes.size()) {
    return QString();
  }

  if (!arePrefixesSafe(p_rowPrefixes, p_delimiterPrefix)) {
    return QString();
  }

  // The model width is the maximum of the header, the alignment row and every
  // body row: nothing is ever discarded.
  int columns = p_alignments.size();
  for (const auto &row : p_cells) {
    columns = qMax(columns, row.size());
  }

  if (columns <= 0) {
    return QString();
  }

  QVector<QString> escaped;
  escaped.reserve(p_cells.size() * columns);
  QVector<int> widths(columns, 0);
  for (const auto &row : p_cells) {
    for (int c = 0; c < columns; ++c) {
      const QString raw = c < row.size() ? row[c] : QString();
      if (hasLineSeparator(raw)) {
        return QString();
      }

      escaped.append(escapeCell(raw));
      // Column widths come from the raw cell text, so the readable padding
      // does not depend on how many pipes had to be escaped.
      widths[c] = qMax(widths[c], raw.size());
    }
  }

  for (int c = 0; c < columns; ++c) {
    const auto alignment =
        c < p_alignments.size() ? p_alignments[c] : PreviewTableAlignment::None;
    widths[c] = qMax(widths[c], alignmentMinimumWidth(alignment));
    // The alignment minimums are far below the cap, so the delimiter markers
    // are unaffected by the clamp.
    widths[c] = qMin(widths[c], TablePreviewModel::c_maxPaddedWidth);
  }

  auto emitRow = [&](const QString &p_prefix, int p_rowIdx) {
    QString line = p_prefix;
    line.append(QLatin1Char('|'));
    for (int c = 0; c < columns; ++c) {
      line.append(QLatin1Char(' '));
      line.append(escaped[p_rowIdx * columns + c].leftJustified(widths[c], QLatin1Char(' ')));
      line.append(QLatin1String(" |"));
    }
    return line;
  };

  QStringList lines;
  lines.append(emitRow(p_rowPrefixes[0], 0));

  {
    QString line = p_delimiterPrefix;
    line.append(QLatin1Char('|'));
    for (int c = 0; c < columns; ++c) {
      const auto alignment =
          c < p_alignments.size() ? p_alignments[c] : PreviewTableAlignment::None;
      line.append(QLatin1Char(' '));
      line.append(alignmentMarker(alignment, widths[c]));
      line.append(QLatin1String(" |"));
    }
    lines.append(line);
  }

  for (int r = 1; r < p_cells.size(); ++r) {
    lines.append(emitRow(p_rowPrefixes[r], r));
  }

  return lines.join(QLatin1Char('\n'));
}

// ---------------------------------------------------------------------------
// TablePreviewModel
// ---------------------------------------------------------------------------

TablePreviewModel::TablePreviewModel(QObject *p_parent) : QAbstractTableModel(p_parent) {}

const int TablePreviewModel::c_maxCells = 200000;

const int TablePreviewModel::c_maxRows = 2000;

const int TablePreviewModel::c_maxColumns = 200;

const int TablePreviewModel::c_maxPaddedWidth = 80;

int TablePreviewModel::normalizedColumnCount(const TablePreview &p_table) {
  int columns = p_table.alignments().size();
  for (const auto &row : p_table.cells()) {
    columns = qMax(columns, row.size());
  }

  return columns;
}

qint64 TablePreviewModel::normalizedCellCount(const TablePreview &p_table) {
  return static_cast<qint64>(normalizedColumnCount(p_table)) *
         static_cast<qint64>(p_table.cells().size());
}

bool TablePreviewModel::isWithinLimits(const TablePreview &p_table) {
  return p_table.cells().size() <= c_maxRows && normalizedColumnCount(p_table) <= c_maxColumns &&
         normalizedCellCount(p_table) <= c_maxCells;
}

void TablePreviewModel::setTable(const QSharedPointer<const TablePreview> &p_table) {
  beginResetModel();
  if (p_table) {
    m_cells = p_table->cells();
    m_alignments = p_table->alignments();
    m_rowPrefixes = p_table->rowPrefixes();
    m_delimiterPrefix = p_table->delimiterPrefix();
    m_declaredColumnCount = p_table->columnCount();
  } else {
    m_cells.clear();
    m_alignments.clear();
    m_rowPrefixes.clear();
    m_delimiterPrefix.clear();
    m_declaredColumnCount = 0;
  }

  normalize();
  endResetModel();
}

void TablePreviewModel::normalize() {
  m_columnCount = m_alignments.size();
  for (const auto &row : m_cells) {
    m_columnCount = qMax(m_columnCount, row.size());
  }

  for (auto &row : m_cells) {
    while (row.size() < m_columnCount) {
      row.append(QString());
    }
  }

  while (m_alignments.size() < m_columnCount) {
    m_alignments.append(PreviewTableAlignment::None);
  }
}

int TablePreviewModel::rowCount(const QModelIndex &p_parent) const {
  return p_parent.isValid() ? 0 : m_cells.size();
}

int TablePreviewModel::columnCount(const QModelIndex &p_parent) const {
  return p_parent.isValid() ? 0 : m_columnCount;
}

QVariant TablePreviewModel::data(const QModelIndex &p_index, int p_role) const {
  if (!p_index.isValid() || p_index.row() >= m_cells.size() ||
      p_index.column() >= m_columnCount) {
    return QVariant();
  }

  switch (p_role) {
  case Qt::DisplayRole:
  case Qt::EditRole:
  case Qt::ToolTipRole:
    return m_cells[p_index.row()].value(p_index.column());

  case Qt::TextAlignmentRole: {
    const auto alignment = m_alignments.value(p_index.column(), PreviewTableAlignment::None);
    switch (alignment) {
    case PreviewTableAlignment::Center:
      return static_cast<int>(Qt::AlignCenter);
    case PreviewTableAlignment::Right:
      return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
    default:
      return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
    }
  }

  case Qt::FontRole:
    if (p_index.row() == 0) {
      QFont font;
      font.setBold(true);
      return font;
    }
    return QVariant();

  default:
    return QVariant();
  }
}

bool TablePreviewModel::setData(const QModelIndex &p_index, const QVariant &p_value, int p_role) {
  if (p_role != Qt::EditRole || !p_index.isValid() || p_index.row() >= m_cells.size() ||
      p_index.column() >= m_columnCount) {
    return false;
  }

  const QString newValue = p_value.toString();
  if (m_cells[p_index.row()][p_index.column()] == newValue) {
    // Only actual value changes are committed.
    return false;
  }

  m_cells[p_index.row()][p_index.column()] = newValue;
  emit dataChanged(p_index, p_index);
  emit cellCommitted();
  return true;
}

Qt::ItemFlags TablePreviewModel::flags(const QModelIndex &p_index) const {
  if (!p_index.isValid()) {
    return Qt::NoItemFlags;
  }

  return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable;
}

bool TablePreviewModel::isRoundTrippable() const {
  if (m_cells.isEmpty() || m_declaredColumnCount <= 0) {
    qCDebug(previewTableLog) << "not round-trippable: rows" << m_cells.size()
                             << "declared columns" << m_declaredColumnCount;
    return false;
  }

  // Writing a wider matrix back would add the excess column to the header and
  // the delimiter row, turning cells GFM ignores today into real ones.
  if (m_columnCount != m_declaredColumnCount) {
    qCDebug(previewTableLog) << "not round-trippable: a row is wider than the header declares -"
                             << m_columnCount << "vs" << m_declaredColumnCount;
    return false;
  }

  if (!TablePreviewSerializer::arePrefixesSafe(m_rowPrefixes, m_delimiterPrefix)) {
    qCDebug(previewTableLog) << "not round-trippable: the block container prefixes cannot be"
                             << "reproduced - rows" << m_rowPrefixes << "delimiter"
                             << m_delimiterPrefix;
    return false;
  }

  return true;
}

QString TablePreviewModel::toMarkdown() const {
  if (!isRoundTrippable()) {
    return QString();
  }

  return TablePreviewSerializer::serialize(m_cells, m_alignments, m_rowPrefixes,
                                           m_delimiterPrefix);
}

// ---------------------------------------------------------------------------
// TablePreviewView
// ---------------------------------------------------------------------------

namespace {
// How often one distribution may re-run against a viewport its own writes
// resized. Two passes are what dropping the vertical scroll bar costs; the
// third is slack.
const int c_maxDistributionPasses = 3;

// A wheel movement can be consumed only while the bar it targets still has
// room in that direction; otherwise the editor underneath keeps scrolling.
bool canScroll(const QScrollBar *p_bar, int p_delta) {
  return p_bar && ((p_delta < 0 && p_bar->value() < p_bar->maximum()) ||
                   (p_delta > 0 && p_bar->value() > p_bar->minimum()));
}

// Whether a wheel movement asks the sheet to scroll sideways, and which axis
// carries the amount. A horizontal axis speaks for itself when it dominates -
// the same rule QAbstractScrollArea uses to pick a bar - and shift with a
// vertical one is how a plain wheel asks for horizontal movement. Only
// angleDelta() decides, because that is all QAbstractSlider acts on, so a
// movement Qt cannot scroll with cannot scroll a sheet either.
enum class HorizontalIntent { None, FromHorizontalAxis, FromVerticalAxis };

HorizontalIntent horizontalIntent(const QWheelEvent &p_event) {
  const QPoint delta = p_event.angleDelta();
  if (p_event.modifiers().testFlag(Qt::ShiftModifier) && delta.y() != 0 &&
      qAbs(delta.y()) >= qAbs(delta.x())) {
    return HorizontalIntent::FromVerticalAxis;
  }

  return qAbs(delta.x()) > qAbs(delta.y()) ? HorizontalIntent::FromHorizontalAxis
                                           : HorizontalIntent::None;
}

// The same movement carried on the horizontal axis, so a slider oriented that
// way acts on it.
QPoint onHorizontalAxis(const QPoint &p_delta, HorizontalIntent p_intent) {
  return QPoint(p_intent == HorizontalIntent::FromVerticalAxis ? p_delta.y() : p_delta.x(), 0);
}
} // namespace

const QAbstractItemView::EditTriggers TablePreviewView::c_defaultEditTriggers =
    QAbstractItemView::DoubleClicked | QAbstractItemView::SelectedClicked |
    QAbstractItemView::EditKeyPressed | QAbstractItemView::AnyKeyPressed;

TablePreviewView::TablePreviewView(QWidget *p_parent) : QTableView(p_parent) {
  horizontalHeader()->hide();
  verticalHeader()->hide();
  horizontalHeader()->setStretchLastSection(true);
  setSelectionMode(QAbstractItemView::SingleSelection);
  // A sheet clamped to the text column cannot show every column, and a
  // Markdown table carries no natural place to break a row, so the columns
  // keep their content widths and the overflow is reached by scrolling.
  // Per pixel, because per item cannot reveal the tail of a single column
  // wider than the viewport - which is exactly the cell that overflowed.
  setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
  setEditTriggers(c_defaultEditTriggers);

  // A Markdown table cell is single-line by construction, and wrapping is what
  // makes sizeHintForRow() depend on the assigned column widths. The height is
  // measured before the columns are distributed, so a wrapping row reserved a
  // band for text which then fits on one line, leaving an empty strip under
  // the sheet. Cells too wide for their column are elided instead, which is
  // what the horizontal scroll bar policy above already implies.
  setWordWrap(false);

  m_columnLayoutTimer = new QTimer(this);
  m_columnLayoutTimer->setSingleShot(true);
  m_columnLayoutTimer->setInterval(0);
  connect(m_columnLayoutTimer, &QTimer::timeout, this, &TablePreviewView::distributeColumnWidths);
}

void TablePreviewView::setModel(QAbstractItemModel *p_model) {
  // Drop only this view's own connections: QTableView keeps model connections
  // of its own which must survive.
  for (const auto &connection : m_modelConnections) {
    disconnect(connection);
  }
  m_modelConnections.clear();

  QTableView::setModel(p_model);
  invalidatePreferredSize();

  if (!p_model) {
    return;
  }

  // A committed cell edit only emits dataChanged: the parse generation which
  // echoes it takes the "unchanged snapshot" path, so resetFromSource() never
  // runs. Re-laying out explicitly keeps the edited column from holding the
  // proportions of a value it no longer has, instead of relying on Qt to
  // schedule an items layout for the changed size hint.
  auto invalidate = [this]() {
    invalidatePreferredSize();
    scheduleColumnLayout();
  };
  m_modelConnections
      << connect(p_model, &QAbstractItemModel::modelReset, this, invalidate)
      << connect(p_model, &QAbstractItemModel::layoutChanged, this, invalidate)
      << connect(p_model, &QAbstractItemModel::dataChanged, this, invalidate)
      << connect(p_model, &QAbstractItemModel::rowsInserted, this, invalidate)
      << connect(p_model, &QAbstractItemModel::rowsRemoved, this, invalidate)
      << connect(p_model, &QAbstractItemModel::rowsMoved, this, invalidate)
      << connect(p_model, &QAbstractItemModel::columnsInserted, this, invalidate)
      << connect(p_model, &QAbstractItemModel::columnsRemoved, this, invalidate)
      << connect(p_model, &QAbstractItemModel::columnsMoved, this, invalidate);
}

TablePreviewView::PreferredSizeKey TablePreviewView::currentPreferredSizeKey() const {
  PreferredSizeKey key;
  key.m_frame = frameWidth() * 2;
  key.m_minColumnWidth = qMax(1, horizontalHeader()->minimumSectionSize());
  key.m_minRowHeight = qMax(1, verticalHeader()->minimumSectionSize());
  key.m_rows = model() ? model()->rowCount() : 0;
  key.m_columns = model() ? model()->columnCount() : 0;
  key.m_visibleRows = m_visibleRows;
  key.m_showGrid = showGrid();
  key.m_wordWrap = wordWrap();
  key.m_delegate = itemDelegate();
  return key;
}

void TablePreviewView::invalidatePreferredSize() { m_preferredSizeDirty = true; }

void TablePreviewView::changeEvent(QEvent *p_event) {
  switch (p_event->type()) {
  case QEvent::FontChange:
  case QEvent::ApplicationFontChange:
  case QEvent::StyleChange:
  case QEvent::LayoutDirectionChange:
    // The measurement is derived from font and style metrics.
    invalidatePreferredSize();
    break;

  default:
    break;
  }

  QTableView::changeEvent(p_event);
}

void TablePreviewView::setVisibleRows(int p_rows) {
  m_visibleRows = qMax(1, p_rows);
  // The window may have grown over rows which were never fitted.
  resizeVisibleRowsToContents();
  invalidatePreferredSize();
  // The row window decides whether a vertical scroll bar is reserved, which
  // moves the width the columns have to share.
  distributeColumnWidths();
  updateGeometry();
}

void TablePreviewView::resizeVisibleRowsToContents() {
  if (!model()) {
    return;
  }

  const int rows = qMin(model()->rowCount(), qMax(1, m_visibleRows));
  for (int r = 0; r < rows; ++r) {
    resizeRowToContents(r);
  }
}

QSize TablePreviewView::preferredSize() const {
  const PreferredSizeKey key = currentPreferredSizeKey();
  if (!m_preferredSizeDirty && key == m_cachedPreferredSizeKey) {
    return m_cachedPreferredSize;
  }

  const int frame = key.m_frame;
  int width = frame;
  int height = frame;

  if (model()) {
    // A header never assigns a section less than its minimum, so measuring the
    // bare content hint would reserve a band the view cannot lay out in.
    const int minColumnWidth = key.m_minColumnWidth;
    const int minRowHeight = key.m_minRowHeight;

    // Derive from the contents, never from the currently assigned geometry:
    // the last section stretches, so using columnWidth() here would feed the
    // assigned width straight back into the preferred width.
    m_cachedColumnWidths.resize(model()->columnCount());
    for (int c = 0; c < model()->columnCount(); ++c) {
      const int columnWidth = qMax(sizeHintForColumn(c), minColumnWidth);
      m_cachedColumnWidths[c] = columnWidth;
      width += columnWidth;
    }

    const int rows = qMin(model()->rowCount(), qMax(1, m_visibleRows));
    for (int r = 0; r < rows; ++r) {
      height += qMax(sizeHintForRow(r), minRowHeight);
    }

    if (model()->rowCount() > rows) {
      width += verticalScrollBar()->sizeHint().width();
    }
  } else {
    // Never leave widths behind which the distribution could consult with a
    // column count that no longer exists.
    m_cachedColumnWidths.clear();
  }

  m_cachedPreferredSize = QSize(qMax(width, frame + 1), qMax(height, frame + 1));
  m_cachedPreferredSizeKey = key;
  m_preferredSizeDirty = false;

  qCDebug(previewTableLog) << "measured the sheet ->" << m_cachedPreferredSize << "for"
                           << key.m_rows << "x" << key.m_columns << "cell(s), showing at most"
                           << key.m_visibleRows << "row(s); frame" << key.m_frame
                           << "minimum section" << key.m_minColumnWidth << "x"
                           << key.m_minRowHeight;

  return m_cachedPreferredSize;
}

QSize TablePreviewView::sizeHint() const { return preferredSize(); }

QSize TablePreviewView::preferredSizeWithin(int p_maxWidth) const {
  QSize size = preferredSize();
  if (p_maxWidth > 0 && size.width() > p_maxWidth) {
    // The sheet is about to be clamped, so a horizontal scroll bar appears.
    // Without reserving the height it takes from the viewport the band would
    // be exactly the rows tall and the bar would cover the last one. A style
    // whose bars float over the content takes nothing, and then reserving
    // would leave an empty strip instead.
    QStyleOption option;
    option.initFrom(this);
    const int overlap = style()->pixelMetric(QStyle::PM_ScrollView_ScrollBarOverlap, &option, this);
    size.rheight() += qMax(0, horizontalScrollBar()->sizeHint().height() - overlap);
  }

  return size;
}

void TablePreviewView::wheelEvent(QWheelEvent *p_event) {
  const Qt::KeyboardModifiers modifiers = p_event->modifiers();
  // Control is the editor's zoom gesture, which VTextEdit deliberately ignores
  // so the application can act on it. The sheet must not swallow it whatever
  // it could otherwise do with the movement.
  const HorizontalIntent intent = modifiers.testFlag(Qt::ControlModifier)
                                      ? HorizontalIntent::None
                                      : horizontalIntent(*p_event);
  if (intent != HorizontalIntent::None) {
    // Hand the movement to the bar itself. QTableView picks its target from
    // the event's dominant axis, and a slider only acts on the axis it is
    // oriented along, so a shift+wheel has to be turned into a horizontal
    // movement first. Going through the slider rather than setValue() keeps
    // its inversion handling and its accumulation of high resolution deltas.
    // A bar with no room left leaves the event ignored, so the fall through
    // still reaches the editor.
    const QPoint angleDelta = onHorizontalAxis(p_event->angleDelta(), intent);
    const QPoint pixelDelta = onHorizontalAxis(p_event->pixelDelta(), intent);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QWheelEvent horizontal(p_event->position(), p_event->globalPosition(), pixelDelta, angleDelta,
                           p_event->buttons(), modifiers, p_event->phase(), p_event->inverted(),
                           p_event->source());
#else
    QWheelEvent horizontal(QPointF(p_event->pos()), p_event->globalPosF(), pixelDelta, angleDelta,
                           p_event->buttons(), modifiers, p_event->phase(), p_event->inverted(),
                           p_event->source());
#endif
    horizontal.setAccepted(false);
    QCoreApplication::sendEvent(horizontalScrollBar(), &horizontal);
    if (horizontal.isAccepted()) {
      p_event->accept();
      return;
    }
  }

  const int delta = p_event->angleDelta().y();
  if (!canScroll(verticalScrollBar(), delta) || modifiers != Qt::NoModifier) {
    // Let Qt propagate the unconsumed movement up to the editor viewport.
    p_event->ignore();
    return;
  }

  QTableView::wheelEvent(p_event);
}

void TablePreviewView::updateGeometries() {
  QTableView::updateGeometries();
  distributeColumnWidths();
}

void TablePreviewView::layoutColumns() {
  invalidatePreferredSize();
  distributeColumnWidths();
}

void TablePreviewView::scheduleColumnLayout() {
  if (m_columnLayoutTimer && !m_columnLayoutTimer->isActive()) {
    m_columnLayoutTimer->start();
  }
}

void TablePreviewView::distributeColumnWidths() {
  if (m_distributingColumns || !model() || model()->columnCount() <= 0) {
    return;
  }

  // Refreshes m_cachedColumnWidths; cheap when the cache is still valid.
  preferredSize();

  const int count = m_cachedColumnWidths.size();
  int total = 0;
  for (int w : m_cachedColumnWidths) {
    total += w;
  }
  if (count <= 0 || total <= 0) {
    return;
  }

  const int minColumnWidth = qMax(1, horizontalHeader()->minimumSectionSize());
  QScopedValueRollback<bool> guard(m_distributingColumns, true);

  // Writing the sections can drop the vertical scroll bar, which resizes the
  // viewport from inside setColumnWidth() - and the guard has to swallow the
  // re-entrant pass that comes with it. So re-run here against the width the
  // viewport settled on, otherwise the sheet keeps the proportions of a
  // viewport it no longer has and the stretched last section quietly absorbs
  // the difference. The scroll bar depends on the row heights only, so this
  // converges; the bound is there so a style with width-dependent scroll bars
  // cannot spin.
  for (int pass = 0; pass < c_maxDistributionPasses; ++pass) {
    const int available = viewport()->width();
    if (available <= total) {
      // No surplus: keep the content widths rather than squeezing the columns
      // into the band. The overflow is what the horizontal scroll bar exists
      // for, and squeezing would elide cells the user cannot then reach.
      for (int c = 0; c < count; ++c) {
        setColumnWidth(c, m_cachedColumnWidths.at(c));
      }
    } else {
      // Every section is written, including the last one. Letting the stretch
      // handle the remainder instead would leave the last section's stored
      // base size behind from whatever the previous contents were, and a one
      // column table would get no assignment at all.
      int assigned = 0;
      for (int c = 0; c < count - 1; ++c) {
        const int width =
            qMax(minColumnWidth, qRound(qreal(m_cachedColumnWidths.at(c)) * available / total));
        setColumnWidth(c, width);
        assigned += width;
      }

      // Cumulative remainder, so the sections cover the viewport exactly
      // however the per column rounding fell.
      setColumnWidth(count - 1, qMax(minColumnWidth, available - assigned));
    }

    if (viewport()->width() == available) {
      break;
    }
  }
}

// ---------------------------------------------------------------------------
// TablePreviewWidget
// ---------------------------------------------------------------------------

const qreal TablePreviewWidget::c_widthFraction = 0.9;

TablePreviewWidget::TablePreviewWidget(PreviewWidgetContext *p_context, QWidget *p_parent)
    : PreviewWidget(p_context, p_parent) {
  auto layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  m_model = new TablePreviewModel(this);
  m_view = new TablePreviewView(this);
  m_view->setModel(m_model);
  layout->addWidget(m_view);

  // The host only consults heightForWidth() when the policy advertises it, and
  // that is the only hook which sees the width the band actually gets.
  QSizePolicy policy = sizePolicy();
  policy.setHeightForWidth(true);
  setSizePolicy(policy);

  connect(m_model, &TablePreviewModel::cellCommitted, this,
          &TablePreviewWidget::handleCellCommitted);

  if (p_context) {
    connect(p_context, &PreviewWidgetContext::replacementFinished, this,
            &TablePreviewWidget::handleReplacementFinished);
  }
}

QVector<PreviewElementType> TablePreviewWidget::supportedTypes() const {
  return QVector<PreviewElementType>() << PreviewElementType::Table;
}

qreal TablePreviewWidget::preferredWidthFraction() const { return c_widthFraction; }

bool TablePreviewWidget::setPreview(const QSharedPointer<const Preview> &p_preview) {
  if (!p_preview || p_preview->type() != PreviewElementType::Table) {
    qCDebug(previewTableLog) << "refused a snapshot which is not a table";
    return false;
  }

  auto table = p_preview.staticCast<const TablePreview>();
  if (table->rowCount() <= 0) {
    qCDebug(previewTableLog) << "refused an empty table";
    return false;
  }

  // A table too large to materialize is left to the static source rendering
  // rather than driving a per-edit walk over every section. A refused table
  // never becomes an active item, so the factory chain is re-walked on every
  // parse generation: warning here would repeat forever for a static,
  // by-design condition.
  if (!TablePreviewModel::isWithinLimits(*table)) {
    qCDebug(previewTableLog)
        << "refused an oversized table -" << table->cells().size() << "row(s) x"
        << TablePreviewModel::normalizedColumnCount(*table) << "column(s) ="
        << TablePreviewModel::normalizedCellCount(*table) << "cells; limits are"
        << TablePreviewModel::c_maxRows << "x" << TablePreviewModel::c_maxColumns << "and"
        << TablePreviewModel::c_maxCells << "cells";
    return false;
  }

  // Nothing changed: keep the model, the selection and any open cell editor.
  // The snapshot which merely echoes this sheet's own accepted commit counts
  // as unchanged too, otherwise every committed cell edit would tear the
  // editing state down one parse generation later.
  if (m_table && m_view->isEnabled()) {
    bool unchanged = m_table->sourceMarkdown() == table->sourceMarkdown();
    const bool identical = unchanged;
    if (!unchanged) {
      const QString committed = m_model->toMarkdown();
      unchanged = !committed.isEmpty() && committed == table->sourceMarkdown();
    }

    if (unchanged) {
      qCDebug(previewTableLog) << "bound an unchanged snapshot - kept the model and the editing"
                               << "state" << (identical ? "(identical source)"
                                                        : "(echo of this sheet's own commit)");
      m_table = table;
      return true;
    }
  }

  qCDebug(previewTableLog) << "rebuilding the sheet from" << table->rowCount() << "row(s) x"
                           << table->columnCount() << "declared column(s), source"
                           << table->sourceMarkdown().left(60);

  m_table = table;
  resetFromSource();
  // setEnabled() tracks whether the bound snapshot is still authoritative;
  // read-only is expressed through the edit triggers instead.
  m_view->setEnabled(true);
  return true;
}

void TablePreviewWidget::setReadOnly(bool p_readOnly) {
  if (m_readOnly == p_readOnly) {
    return;
  }

  m_readOnly = p_readOnly;
  applyEditTriggers();
}

void TablePreviewWidget::applyEditTriggers() {
  // Present the sheet as a viewer instead of silently swallowing edits which
  // the host would reject, or which could not be written back without changing
  // what the table renders to.
  const bool roundTrippable = m_model->isRoundTrippable();
  const bool editable = !m_readOnly && roundTrippable;
  qCDebug(previewTableLog) << "edit triggers:" << (editable ? "editable" : "viewer only")
                           << "- read-only" << m_readOnly << "round-trippable" << roundTrippable;
  m_view->setEditTriggers(editable ? TablePreviewView::c_defaultEditTriggers
                                   : QAbstractItemView::NoEditTriggers);
}

void TablePreviewWidget::resetFromSource() {
  // The context is the authoritative binding: an accepted replacement rebases
  // it onto the text now in the document, while this cached snapshot still
  // describes the pre-commit source. Restoring from the cache would undo a
  // commit the host has already applied, and the sheet would then serialize
  // the reverted matrix over the user's accepted change.
  if (auto context = previewContext()) {
    const auto bound = context->preview();
    if (bound && bound->type() == PreviewElementType::Table) {
      m_table = bound.staticCast<const TablePreview>();
    }
  }

  m_applyingSource = true;
  m_model->setTable(m_table);
  m_view->layoutColumns();
  m_view->resizeVisibleRowsToContents();
  m_applyingSource = false;

  applyEditTriggers();
  updateGeometry();

  qCDebug(previewTableLog) << "sheet reset to" << m_model->rowCount() << "x"
                           << m_model->columnCount() << "- preferred size" << sizeHint();
}
void TablePreviewWidget::setVisibleRows(int p_rows) {
  m_view->setVisibleRows(p_rows);
  updateGeometry();
}

QSize TablePreviewWidget::sizeHint() const { return m_view->preferredSize(); }

bool TablePreviewWidget::hasHeightForWidth() const { return true; }

int TablePreviewWidget::heightForWidth(int p_width) const {
  // The host clamps the band to the text column, and a sheet wider than the
  // band gets a horizontal scroll bar which eats into the viewport. Reserving
  // its height here rather than in sizeHint() is what makes the reserve track
  // the clamp: the width passed in is the one the band ends up with, whereas
  // the widget's own geometry context still holds the previous layout pass at
  // measuring time.
  return m_view->preferredSizeWithin(p_width).height();
}

void TablePreviewWidget::changeEvent(QEvent *p_event) {
  if (p_event->type() == QEvent::FontChange && m_view) {
    // Qt's style sheet machinery does not hand an application's editor font
    // down to a view nested inside a styled ancestor, so the view would keep
    // measuring itself with the application default while being painted with
    // the inherited font. Forward it explicitly and re-fit.
    if (m_view->font() != font()) {
      m_view->setFont(font());
      m_view->layoutColumns();
      m_view->resizeVisibleRowsToContents();
      updateGeometry();
    }
  }

  PreviewWidget::changeEvent(p_event);
}

void TablePreviewWidget::handleCellCommitted() {
  if (m_applyingSource || !m_table) {
    return;
  }

  const QString markdown = m_model->toMarkdown();
  if (markdown.isEmpty()) {
    // Unsafe to rewrite: restore the source view.
    qCWarning(previewTableLog) << "a cell was committed but the sheet cannot be serialized"
                               << "safely - restoring the source";
    resetFromSource();
    return;
  }

  auto context = previewContext();
  if (!context) {
    qCDebug(previewTableLog) << "a cell was committed but the sheet has no context to write"
                             << "through";
    return;
  }

  qCDebug(previewTableLog) << "committing a cell edit ->" << markdown.left(80);
  context->requestSourceReplacement(markdown);
}

void TablePreviewWidget::handleReplacementFinished(const vte::PreviewReplacementResult &p_result) {
  if (p_result.isAccepted()) {
    qCDebug(previewTableLog) << "the commit was accepted";
    m_view->setEnabled(true);
    return;
  }

  switch (p_result.status()) {
  case PreviewReplacementResult::StaleSnapshot:
  case PreviewReplacementResult::SourceMismatch:
  case PreviewReplacementResult::InvalidRange:
  case PreviewReplacementResult::UnknownIdentity:
    // External source always wins and this snapshot no longer describes the
    // document. Stop presenting it as the truth and wait for the authoritative
    // snapshot instead of restoring stale values.
    // The host already warned about the rejection itself.
    qCDebug(previewTableLog) << "this sheet is no longer authoritative - disabling it until the"
                             << "next snapshot";
    m_view->setEnabled(false);
    break;

  default:
    // The document was not touched, so the bound snapshot is still the source
    // of truth: discard the edit.
    qCDebug(previewTableLog) << "the document was not touched - discarding the edit";
    resetFromSource();
    break;
  }
}

// ---------------------------------------------------------------------------
// TablePreviewWidgetFactory
// ---------------------------------------------------------------------------

TablePreviewWidgetFactory::TablePreviewWidgetFactory(QObject *p_parent)
    : PreviewWidgetFactory(p_parent) {}

QVector<PreviewElementType> TablePreviewWidgetFactory::supportedTypes() const {
  return QVector<PreviewElementType>() << PreviewElementType::Table;
}

PreviewWidget *
TablePreviewWidgetFactory::createWidget(PreviewWidgetContext *p_context,
                                        const QSharedPointer<const Preview> &p_preview,
                                        QWidget *p_parent) {
  if (!p_preview || p_preview->type() != PreviewElementType::Table) {
    return nullptr;
  }

  auto widget = new TablePreviewWidget(p_context, p_parent);
  widget->setVisibleRows(m_visibleRows);
  widget->setReadOnly(m_readOnly);

  // Every host rebuild destroys the previous sheets. setVisibleRows() and
  // setReadOnly() do compact the list, but both early-return when the value is
  // unchanged, which is the steady state - so without this the vector would
  // grow for the whole lifetime of the editor.
  pruneWidgets();
  m_widgets.append(QPointer<TablePreviewWidget>(widget));
  return widget;
}

void TablePreviewWidgetFactory::pruneWidgets() {
  for (int i = m_widgets.size() - 1; i >= 0; --i) {
    if (m_widgets[i].isNull()) {
      m_widgets.removeAt(i);
    }
  }
}

void TablePreviewWidgetFactory::setVisibleRows(int p_rows) {
  const int rows = qMax(1, p_rows);
  if (m_visibleRows == rows) {
    return;
  }

  m_visibleRows = rows;

  pruneWidgets();
  for (auto &widget : m_widgets) {
    widget->setVisibleRows(rows);
  }
}

void TablePreviewWidgetFactory::setReadOnly(bool p_readOnly) {
  if (m_readOnly == p_readOnly) {
    return;
  }

  m_readOnly = p_readOnly;

  pruneWidgets();
  for (auto &widget : m_widgets) {
    widget->setReadOnly(p_readOnly);
  }
}
