#include "tablepreviewwidget.h"

#include <algorithm>
#include <cmath>

#include <QApplication>
#include <QFont>
#include <QFontMetricsF>
#include <QHeaderView>
#include <QLineEdit>
#include <QPainter>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QStringList>
#include <QStyle>
#include <QStyleOption>
#include <QStyledItemDelegate>
#include <QTextLayout>
#include <QTextOption>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtMath>

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

// ---------------------------------------------------------------------------
// TablePreviewDelegate
// ---------------------------------------------------------------------------

namespace {
// Line width standing in for "no constraint". QTextLine works in 26.6 fixed
// point, so this stays far away from the overflow QFIXED_MAX guards against
// while being wider than any cell a Markdown table can hold.
const qreal c_unboundedLineWidth = 1 << 22;

// The horizontal inset QCommonStyle applies on each side of an item's text.
// Using the very same expression here is what keeps the delegate's own
// painting, its size hints and the column floor from disagreeing by a pixel.
int itemTextMargin(const QStyle *p_style, const QStyleOption &p_option, const QWidget *p_widget) {
  return p_style->pixelMetric(QStyle::PM_FocusFrameHMargin, &p_option, p_widget) + 1;
}

// Lay @p_layout out at @p_width and return the bounds it occupies. Only
// QTextLine::height() is accumulated: a QTextDocument would add paragraph and
// document margins, which is exactly the stale vertical space this sheet must
// not have.
QSizeF layoutLines(QTextLayout &p_layout, qreal p_width) {
  qreal height = 0;
  qreal widthUsed = 0;

  p_layout.beginLayout();
  forever {
    QTextLine line = p_layout.createLine();
    if (!line.isValid()) {
      break;
    }

    // An empty cell has one zero-length line. Mirroring QCommonStyle and
    // stopping here keeps such a row at the vertical header's minimum instead
    // of reserving a line box for text which is not there.
    if (line.textLength() == 0) {
      break;
    }

    line.setLineWidth(p_width);
    line.setPosition(QPointF(0, height));
    height += line.height();
    widthUsed = qMax(widthUsed, line.naturalTextWidth());
  }
  p_layout.endLayout();

  return QSizeF(widthUsed, height);
}

// Prepare @p_layout for one cell and lay it out into @p_width, or naturally
// when @p_width is not positive.
QSizeF layoutCellText(QTextLayout &p_layout, const QStyleOptionViewItem &p_option, int p_width) {
  QTextOption textOption;
  // Qt's own item delegates use QTextOption::WordWrap, which cannot break an
  // unbroken token and leaves it elided instead. A cell holding a long URL or
  // an identifier is exactly the case this sheet has to show in full.
  textOption.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
  textOption.setTextDirection(p_option.direction);
  textOption.setAlignment(QStyle::visualAlignment(p_option.direction, p_option.displayAlignment));

  p_layout.setText(p_option.text);
  p_layout.setFont(p_option.font);
  p_layout.setTextOption(textOption);

  return layoutLines(p_layout, p_width > 0 ? qreal(p_width) : c_unboundedLineWidth);
}

// Item delegate which wraps rather than elides.
//
// Only display painting and sizing are replaced; editor creation, the commit
// path and every data role stay with QStyledItemDelegate, so a cell is still
// edited as the single line of raw Markdown it is.
class TablePreviewDelegate : public QStyledItemDelegate {
public:
  explicit TablePreviewDelegate(QObject *p_parent = nullptr) : QStyledItemDelegate(p_parent) {}

  void paint(QPainter *p_painter, const QStyleOptionViewItem &p_option,
             const QModelIndex &p_index) const Q_DECL_OVERRIDE;

  // The width of p_option.rect is read as the section width the cell has to
  // fit into; a non-positive one asks for the natural, unwrapped size.
  QSize sizeHint(const QStyleOptionViewItem &p_option,
                 const QModelIndex &p_index) const Q_DECL_OVERRIDE;
};

void TablePreviewDelegate::paint(QPainter *p_painter, const QStyleOptionViewItem &p_option,
                                 const QModelIndex &p_index) const {
  QStyleOptionViewItem option = p_option;
  initStyleOption(&option, p_index);

  const QWidget *widget = option.widget;
  QStyle *style = widget ? widget->style() : QApplication::style();

  p_painter->save();
  // Intersect rather than replace: the view has already clipped to the region
  // it is repainting, and dropping that would let a cell paint over its
  // neighbours.
  p_painter->setClipRect(option.rect, Qt::IntersectClip);

  // The native panel first: background, alternating rows, selection and hover
  // all come from the style, so a themed sheet keeps looking like one.
  style->drawPrimitive(QStyle::PE_PanelItemViewItem, &option, p_painter, widget);

  QPalette::ColorGroup group =
      option.state & QStyle::State_Enabled ? QPalette::Normal : QPalette::Disabled;
  if (group == QPalette::Normal && !(option.state & QStyle::State_Active)) {
    group = QPalette::Inactive;
  }

  if (!option.text.isEmpty()) {
    p_painter->setPen(option.palette.color(group, option.state & QStyle::State_Selected
                                                      ? QPalette::HighlightedText
                                                      : QPalette::Text));

    // The sheet carries neither decorations nor check indicators, so the text
    // occupies the whole item minus the style's own horizontal inset. Taking
    // SE_ItemViewItemText and insetting it again would count that inset twice.
    const int margin = itemTextMargin(style, option, widget);
    const QRect textRect = option.rect.adjusted(margin, 0, -margin, 0);

    QTextLayout layout;
    const QSizeF bounds = layoutCellText(layout, option, textRect.width());
    // Horizontal alignment lives inside the layout, which is why the layout
    // rectangle spans the full text width; only the vertical placement of the
    // whole block is decided here.
    const QRect layoutRect =
        QStyle::alignedRect(option.direction, option.displayAlignment,
                            QSize(textRect.width(), qCeil(bounds.height())), textRect);
    layout.draw(p_painter, layoutRect.topLeft());
  }

  if (option.state & QStyle::State_HasFocus) {
    QStyleOptionFocusRect focus;
    focus.QStyleOption::operator=(option);
    focus.rect = style->subElementRect(QStyle::SE_ItemViewItemFocusRect, &option, widget);
    focus.state |= QStyle::State_KeyboardFocusChange;
    focus.state |= QStyle::State_Item;
    focus.backgroundColor = option.palette.color(
        group, option.state & QStyle::State_Selected ? QPalette::Highlight : QPalette::Window);
    style->drawPrimitive(QStyle::PE_FrameFocusRect, &focus, p_painter, widget);
  }

  p_painter->restore();
}

QSize TablePreviewDelegate::sizeHint(const QStyleOptionViewItem &p_option,
                                     const QModelIndex &p_index) const {
  QStyleOptionViewItem option = p_option;
  initStyleOption(&option, p_index);

  const QWidget *widget = option.widget;
  const QStyle *style = widget ? widget->style() : QApplication::style();
  const int margin = itemTextMargin(style, option, widget);

  int available = -1;
  if (option.rect.width() > 0) {
    // A section narrower than its own padding still has to lay out, otherwise
    // the wrap loop would never place a character.
    available = qMax(1, option.rect.width() - 2 * margin);
  }

  QTextLayout layout;
  const QSizeF bounds = layoutCellText(layout, option, available);
  return QSize(qCeil(bounds.width()) + 2 * margin, qCeil(bounds.height()));
}
} // namespace

// ---------------------------------------------------------------------------
// TablePreviewView
// ---------------------------------------------------------------------------

namespace {
// How often one layout pass may re-run against a viewport its own writes
// resized. Two passes are what dropping the vertical scroll bar costs; the
// third is slack.
const int c_maxLayoutPasses = 3;

// Bounds on one lazy row-fitting pass. Enough to cover a viewport in a couple
// of event loop turns, small enough that a table at the model limits cannot
// stall the GUI thread while the user drags the scroll bar.
const int c_rowFitBatch = 8;

const int c_rowFitRounds = 8;

// Rows fitted on either side of the viewport, so a small scroll step never
// shows an unfitted row.
const int c_rowFitOverscan = 2;

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

// Where a mouse event happened, in the receiving widget's coordinates.
QPoint mousePosition(const QMouseEvent &p_event) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  return p_event.position().toPoint();
#else
  return p_event.pos();
#endif
}

// Fill @p_available exactly with widths proportional to @p_natural, never
// letting a column fall below @p_minimum.
//
// Compressing proportionally on its own would push a narrow column under the
// floor, and simply clamping it afterwards would overshoot the target. So the
// columns which hit the floor are frozen one round at a time and the rest are
// rescaled against what is left, which is the standard apportionment fixed
// point. The final integer split uses largest remainders so the sections cover
// the viewport to the pixel however the rounding fell.
QVector<int> planColumnWidths(const QVector<int> &p_natural, int p_minimum, int p_available) {
  const int count = p_natural.size();
  QVector<int> widths(count, p_minimum);
  if (count <= 0) {
    return widths;
  }

  // Not even the floors fit: hand out the floors and let the sheet scroll.
  if (p_available <= count * p_minimum) {
    return widths;
  }

  qint64 freeTotal = 0;
  for (int width : p_natural) {
    freeTotal += qMax(width, p_minimum);
  }
  if (freeTotal <= 0) {
    return widths;
  }

  QVector<bool> frozen(count, false);
  int remaining = p_available;
  bool changed = true;
  while (changed && freeTotal > 0) {
    changed = false;
    for (int c = 0; c < count; ++c) {
      if (frozen[c] || freeTotal <= 0) {
        continue;
      }

      const qreal scaled = qreal(remaining) * qMax(p_natural.at(c), p_minimum) / freeTotal;
      if (scaled < p_minimum) {
        frozen[c] = true;
        freeTotal -= qMax(p_natural.at(c), p_minimum);
        remaining -= p_minimum;
        changed = true;
      }
    }
  }

  QVector<QPair<double, int>> remainders;
  qint64 assigned = 0;
  for (int c = 0; c < count; ++c) {
    if (frozen[c]) {
      continue;
    }

    const double exact =
        freeTotal > 0 ? double(remaining) * qMax(p_natural.at(c), p_minimum) / freeTotal : 0.0;
    const int base = qMax(p_minimum, int(std::floor(exact)));
    widths[c] = base;
    assigned += base;
    remainders.append(qMakePair(exact - std::floor(exact), c));
  }

  if (remainders.isEmpty()) {
    // Every column froze, which the arithmetic above rules out. Keep the
    // contract anyway: the sections must cover the viewport.
    widths[count - 1] += p_available - count * p_minimum;
    return widths;
  }

  std::sort(remainders.begin(), remainders.end(),
            [](const QPair<double, int> &p_a, const QPair<double, int> &p_b) {
              return p_a.first != p_b.first ? p_a.first > p_b.first : p_a.second < p_b.second;
            });

  int leftover = remaining - int(assigned);
  for (int i = 0; leftover > 0; ++i, --leftover) {
    ++widths[remainders.at(i % remainders.size()).second];
  }

  // Only reachable through floating point drift, and only ever by a pixel or
  // two; take it back from the columns which rounded up last.
  for (int i = remainders.size() - 1; leftover < 0 && i >= 0; --i) {
    const int c = remainders.at(i).second;
    const int take = qMin(widths[c] - p_minimum, -leftover);
    widths[c] -= take;
    leftover += take;
  }

  return widths;
}
} // namespace

const QAbstractItemView::EditTriggers TablePreviewView::c_defaultEditTriggers =
    QAbstractItemView::DoubleClicked | QAbstractItemView::SelectedClicked |
    QAbstractItemView::EditKeyPressed | QAbstractItemView::AnyKeyPressed;

const int TablePreviewView::c_minimumColumnCharacters = 12;

TablePreviewView::TablePreviewView(QWidget *p_parent) : QTableView(p_parent) {
  horizontalHeader()->hide();
  verticalHeader()->hide();
  // Every section is written by the layout pass, including the last one, so
  // the stretch would only ever overwrite a planned width with a viewport it
  // was not planned against.
  horizontalHeader()->setStretchLastSection(false);
  setSelectionMode(QAbstractItemView::SingleSelection);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  // Per pixel, because per item cannot reveal the tail of a single column
  // wider than the viewport - which is exactly the cell that overflowed. The
  // vertical axis is per pixel too, so a tall wrapped row can be scrolled
  // through instead of jumping past it.
  setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
  setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  setEditTriggers(c_defaultEditTriggers);

  // A cell wider than its column is shown in full rather than elided, which
  // is what makes a row's height depend on the width its columns end up with.
  // The layout pass below is what keeps the two in step: columns are planned
  // first, and the rows are then fitted against exactly those widths.
  setWordWrap(true);
  setItemDelegate(new TablePreviewDelegate(this));

  m_layoutTimer = new QTimer(this);
  m_layoutTimer->setSingleShot(true);
  m_layoutTimer->setInterval(0);
  connect(m_layoutTimer, &QTimer::timeout, this, &TablePreviewView::applyLayout);

  m_rowFitTimer = new QTimer(this);
  m_rowFitTimer->setSingleShot(true);
  m_rowFitTimer->setInterval(0);
  connect(m_rowFitTimer, &QTimer::timeout, this, &TablePreviewView::fitRowsAroundViewport);
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
    scheduleLayout();
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
  key.m_minColumnWidth = minimumColumnWidth();
  key.m_minRowHeight = qMax(1, verticalHeader()->minimumSectionSize());
  key.m_rows = model() ? model()->rowCount() : 0;
  key.m_columns = model() ? model()->columnCount() : 0;
  key.m_visibleRows = m_visibleRows;
  key.m_showGrid = showGrid();
  key.m_wordWrap = wordWrap();
  key.m_delegate = itemDelegate();
  return key;
}

void TablePreviewView::invalidatePreferredSize() {
  m_preferredSizeDirty = true;
  m_constrainedWidthKey = -1;
  // Every lazily fitted row was measured against contents which may just have
  // changed, and row identities do not survive a reset or a reorder either.
  m_fittedRows.clear();
  m_fittedColumnWidths.clear();
  m_notificationArmed = true;
}

void TablePreviewView::changeEvent(QEvent *p_event) {
  switch (p_event->type()) {
  case QEvent::StyleChange:
    // A different style lays its scroll bars out differently, so what the old
    // one was seen to take says nothing about the new one.
    m_observedVerticalChrome = -1;
    m_observedHorizontalChrome = -1;
    invalidatePreferredSize();
    scheduleLayout();
    break;

  case QEvent::FontChange:
  case QEvent::ApplicationFontChange:
  case QEvent::LayoutDirectionChange:
    // The measurement is derived from font and style metrics.
    invalidatePreferredSize();
    scheduleLayout();
    break;

  default:
    break;
  }

  QTableView::changeEvent(p_event);
}

void TablePreviewView::setVisibleRows(int p_rows) {
  m_visibleRows = qMax(1, p_rows);
  // The window may have grown over rows which were never fitted, and it also
  // decides whether a vertical scroll bar is reserved, which moves the width
  // the columns have to share.
  invalidatePreferredSize();
  applyLayout();
  updateGeometry();
}

int TablePreviewView::bandRowCount() const {
  if (!model()) {
    return 0;
  }

  return qMin(model()->rowCount(), qMax(1, m_visibleRows));
}

QStyleOptionViewItem TablePreviewView::baseViewOption() const {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  QStyleOptionViewItem option;
  initViewItemOption(&option);
#else
  QStyleOptionViewItem option = viewOptions();
#endif
  return option;
}

QSize TablePreviewView::cellHint(const QStyleOptionViewItem &p_base, int p_row, int p_column,
                                 int p_width) const {
  if (!model()) {
    return QSize();
  }

  const QModelIndex index = model()->index(p_row, p_column);
  if (!index.isValid()) {
    return QSize();
  }

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  const QAbstractItemDelegate *delegate = itemDelegateForIndex(index);
#else
  const QAbstractItemDelegate *delegate = itemDelegate(index);
#endif
  if (!delegate) {
    return QSize();
  }

  QStyleOptionViewItem option = p_base;
  option.rect = QRect(0, 0, qMax(0, p_width), 0);
  return delegate->sizeHint(option, index);
}

int TablePreviewView::rowHeightFor(const QStyleOptionViewItem &p_base, int p_row,
                                   const QVector<int> &p_widths) const {
  // QTableView's own accounting is asymmetric but exact: a cell is painted
  // into the section minus the grid line, while the section itself is the
  // tallest cell plus it.
  const int grid = showGrid() ? 1 : 0;
  int hint = 0;
  for (int c = 0; c < p_widths.size(); ++c) {
    hint = qMax(hint, cellHint(p_base, p_row, c, p_widths.at(c) - grid).height());
  }

  return qMax(qMax(1, verticalHeader()->minimumSectionSize()), hint + grid);
}

int TablePreviewView::minimumColumnWidth() const {
  const int headerMinimum = qMax(1, horizontalHeader()->minimumSectionSize());

  QStyleOption option;
  option.initFrom(this);
  const int margin = itemTextMargin(style(), option, this);
  const int grid = showGrid() ? 1 : 0;
  const int text = qCeil(c_minimumColumnCharacters * QFontMetricsF(font()).averageCharWidth());

  return qMax(headerMinimum, text + 2 * margin + grid);
}

int TablePreviewView::scrollBarExtent(Qt::Orientation p_orientation) const {
  const int observed =
      p_orientation == Qt::Vertical ? m_observedVerticalChrome : m_observedHorizontalChrome;
  if (observed >= 0) {
    return observed;
  }

  const QScrollBar *bar =
      p_orientation == Qt::Vertical ? verticalScrollBar() : horizontalScrollBar();
  if (!bar) {
    return 0;
  }

  QStyleOption option;
  option.initFrom(this);

  // The estimate until a pass has seen the real thing, following the two rules
  // QAbstractScrollArea lays its children out by: a bar which overlaps the
  // content takes none of the viewport's extent, and a frame drawn around the
  // contents only leaves the bar outside the frame, together with the style's
  // scroll view spacing.
  const int overlap = style()->pixelMetric(QStyle::PM_ScrollView_ScrollBarOverlap, &option, this);
  const QSize hint = bar->sizeHint();
  const int extent = p_orientation == Qt::Vertical ? hint.width() : hint.height();

  int consumed = overlap == 0 ? extent : 0;
  if (frameWidth() > 0 &&
      style()->styleHint(QStyle::SH_ScrollView_FrameOnlyAroundContents, &option, this)) {
    consumed +=
        overlap + style()->pixelMetric(QStyle::PM_ScrollView_ScrollBarSpacing, &option, this);
  }

  return qMax(0, consumed);
}

void TablePreviewView::observeScrollBarChrome() {
  const int frame = frameWidth() * 2;
  bool changed = false;

  // Only a bar which is actually shown says anything about what it costs.
  if (verticalScrollBar()->isVisible()) {
    const int chrome = qMax(0, width() - frame - viewport()->width());
    changed = changed || chrome != scrollBarExtent(Qt::Vertical);
    m_observedVerticalChrome = chrome;
  }

  if (horizontalScrollBar()->isVisible()) {
    const int chrome = qMax(0, height() - frame - viewport()->height());
    changed = changed || chrome != scrollBarExtent(Qt::Horizontal);
    m_observedHorizontalChrome = chrome;
  }

  if (!changed) {
    return;
  }

  // Every answer the host holds was derived from the estimate this just
  // replaced, so they all have to be measured again and reported.
  qCDebug(previewTableLog) << "the sheet's scroll bar chrome is really"
                           << m_observedVerticalChrome << "x" << m_observedHorizontalChrome;
  m_preferredSizeDirty = true;
  m_constrainedWidthKey = -1;
  m_notificationArmed = true;
}

void TablePreviewView::ensureMeasured() const {
  const PreferredSizeKey key = currentPreferredSizeKey();
  if (!m_preferredSizeDirty && key == m_cachedPreferredSizeKey) {
    return;
  }

  // A key which moved without an explicit invalidation is one of the inherited
  // mutations that have no signal of their own - a swapped delegate, the grid
  // being turned off, a new header minimum. They change what the sheet reports
  // and what its sections should be, so they owe a pass and a notification
  // just as a content change does.
  if (!m_preferredSizeDirty) {
    qCDebug(previewTableLog) << "the sheet was mutated behind its own back - re-planning";
    m_fittedRows.clear();
    m_fittedColumnWidths.clear();
    m_notificationArmed = true;
    scheduleLayout();
  }

  const int frame = key.m_frame;
  int width = frame;
  int height = frame;

  m_cachedColumnWidths.clear();
  m_cachedRowHeights.clear();

  if (model() && model()->columnCount() > 0 && model()->rowCount() > 0) {
    const QStyleOptionViewItem base = baseViewOption();
    const int grid = key.m_showGrid ? 1 : 0;
    const int columns = model()->columnCount();
    const int band = bandRowCount();

    // Derive from the contents, never from the currently assigned geometry:
    // the columns are planned against the width the sheet gets, so consulting
    // columnWidth() here would feed that width straight back into the natural
    // one. Only the band is measured - a row the sheet cannot show has no say
    // in the geometry it reserves.
    m_cachedColumnWidths.resize(columns);
    for (int c = 0; c < columns; ++c) {
      int hint = 0;
      for (int r = 0; r < band; ++r) {
        hint = qMax(hint, cellHint(base, r, c, -1).width());
      }

      m_cachedColumnWidths[c] = qMax(key.m_minColumnWidth, hint + grid);
      width += m_cachedColumnWidths.at(c);
    }

    m_cachedRowHeights.resize(band);
    for (int r = 0; r < band; ++r) {
      m_cachedRowHeights[r] = rowHeightFor(base, r, m_cachedColumnWidths);
      height += m_cachedRowHeights.at(r);
    }

    if (model()->rowCount() > band) {
      width += scrollBarExtent(Qt::Vertical);
    }
  }

  m_cachedPreferredSize = QSize(qMax(width, frame + 1), qMax(height, frame + 1));
  m_cachedPreferredSizeKey = key;
  m_preferredSizeDirty = false;
  m_constrainedWidthKey = -1;

  qCDebug(previewTableLog) << "measured the sheet ->" << m_cachedPreferredSize << "for"
                           << key.m_rows << "x" << key.m_columns << "cell(s), showing at most"
                           << key.m_visibleRows << "row(s); frame" << key.m_frame
                           << "minimum section" << key.m_minColumnWidth << "x"
                           << key.m_minRowHeight;
}

QSize TablePreviewView::preferredSize() const {
  ensureMeasured();
  return m_cachedPreferredSize;
}

QSize TablePreviewView::sizeHint() const { return preferredSize(); }

TablePreviewView::Solution TablePreviewView::solveGeometry(int p_outerWidth,
                                                           int p_viewportWidth) const {
  ensureMeasured();

  Solution solution;
  const int frame = frameWidth() * 2;
  if (!model() || model()->columnCount() <= 0 || model()->rowCount() <= 0) {
    solution.m_viewportWidth = qMax(0, p_outerWidth - frame);
    return solution;
  }

  const int band = bandRowCount();
  // Only ever consulted here: the band is what decides whether the rows below
  // it need a bar, and the bar is what the columns have to share the width
  // with.
  const bool verticalScrollBar = model()->rowCount() > band;

  int viewport = -1;
  if (p_viewportWidth >= 0) {
    viewport = p_viewportWidth;
  } else if (p_outerWidth > 0) {
    viewport = p_outerWidth - frame - (verticalScrollBar ? scrollBarExtent(Qt::Vertical) : 0);
    viewport = qMax(0, viewport);
  }

  const int minimum = minimumColumnWidth();
  if (viewport < 0) {
    // The natural plan: what the sheet would like to be before anything clamps
    // it. Whether the frame arrangement puts the scroll bars inside or outside
    // it (SH_ScrollView_FrameOnlyAroundContents) changes where they are drawn,
    // not how much of the outer width they consume, so one formula covers both.
    solution.m_columnWidths = m_cachedColumnWidths;
    int total = 0;
    for (int w : solution.m_columnWidths) {
      total += w;
    }
    solution.m_viewportWidth = total;
  } else {
    solution.m_columnWidths = planColumnWidths(m_cachedColumnWidths, minimum, viewport);
    solution.m_viewportWidth = viewport;
    solution.m_horizontalScrollBar = m_cachedColumnWidths.size() * minimum > viewport;
  }

  // Measure the band against exactly the widths just planned. Qt sizes a
  // wrapped row from the *current* sections, which is why nothing may fit a
  // row before its columns are decided.
  //
  // Whenever the plan came out at the natural widths - the whole
  // p_outerWidth <= 0 branch, and every viewport roomy enough not to compress
  // anything - ensureMeasured() has already measured the band against those
  // very widths. Walking every cell of the band through the delegate a second
  // time for the same answer is the single most expensive thing this solver
  // could do, and it runs once per parse generation while the user types.
  if (solution.m_columnWidths == m_cachedColumnWidths) {
    solution.m_rowHeights = m_cachedRowHeights;
    for (int height : solution.m_rowHeights) {
      solution.m_bandHeight += height;
    }
  } else {
    const QStyleOptionViewItem base = baseViewOption();
    solution.m_rowHeights.resize(band);
    for (int r = 0; r < band; ++r) {
      solution.m_rowHeights[r] = rowHeightFor(base, r, solution.m_columnWidths);
      solution.m_bandHeight += solution.m_rowHeights.at(r);
    }
  }

  return solution;
}

QSize TablePreviewView::constrainedSizeFor(const Solution &p_solution, int p_width) const {
  const int frame = frameWidth() * 2;
  int height = frame + p_solution.m_bandHeight;
  if (p_solution.m_horizontalScrollBar) {
    height += scrollBarExtent(Qt::Horizontal);
  }

  return QSize(qMin(preferredSize().width(), p_width), qMax(height, frame + 1));
}

QSize TablePreviewView::preferredSizeWithin(int p_maxWidth) const {
  if (p_maxWidth <= 0) {
    return preferredSize();
  }

  // Before the memo, never after: ensureMeasured() is what notices the
  // inherited mutations which drop it, and answering from a memo the sheet has
  // already outgrown is exactly the stale reservation this has to avoid.
  ensureMeasured();
  if (m_constrainedWidthKey == p_maxWidth) {
    return m_constrainedSize;
  }

  const Solution solution = solveGeometry(p_maxWidth, -1);
  m_constrainedSize = constrainedSizeFor(solution, p_maxWidth);
  m_constrainedWidthKey = p_maxWidth;
  return m_constrainedSize;
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
  if (m_layingOut || m_fittingRows) {
    return;
  }

  // Writing a section makes QTableView post a deferred geometry update for the
  // very sections the pass just wrote, and that update lands after the guard
  // above has been rolled back. Re-planning for it would walk every cell of
  // the band through the delegate again only to produce the identical plan.
  if (!m_preferredSizeDirty && viewport()->width() == m_plannedViewportWidth) {
    return;
  }

  scheduleLayout();
}

void TablePreviewView::mousePressEvent(QMouseEvent *p_event) {
  QTableView::mousePressEvent(p_event);

  // NoEditTriggers is how a read-only editor, and a table which cannot be
  // written back, turn editing off. Neither may be opened by a click either.
  if (p_event->button() != Qt::LeftButton || editTriggers() == QAbstractItemView::NoEditTriggers) {
    return;
  }

  const QModelIndex index = indexAt(mousePosition(*p_event));
  if (!index.isValid() || !index.flags().testFlag(Qt::ItemIsEditable)) {
    return;
  }

  // One click is enough, and the caret lands under the pointer. A second click
  // never reaches here: the editor covers its own cell by then.
  edit(index, QAbstractItemView::AllEditTriggers, p_event);
}

bool TablePreviewView::edit(const QModelIndex &p_index, EditTrigger p_trigger, QEvent *p_event) {
  const bool editing = QTableView::edit(p_index, p_trigger, p_event);
  if (editing) {
    // The base has already shown the editor and given it focus, so whatever
    // selection that produced is in place and can be replaced with a caret.
    placeCursor(p_index, p_event);
  }

  return editing;
}

QLineEdit *TablePreviewView::cellEditor(const QModelIndex &p_index) const {
  const QRect cell = visualRect(p_index);
  if (!cell.isValid()) {
    return nullptr;
  }

  for (auto editor : viewport()->findChildren<QLineEdit *>()) {
    if (editor->isVisible() && cell.intersects(editor->geometry())) {
      return editor;
    }
  }

  return nullptr;
}

void TablePreviewView::placeCursor(const QModelIndex &p_index, const QEvent *p_event) {
  auto editor = cellEditor(p_index);
  if (!editor) {
    return;
  }

  int position = editor->text().size();
  if (p_event && p_event->type() == QEvent::MouseButtonPress) {
    // The editor is a child of the viewport and covers the cell, so the click
    // it was opened by maps straight onto a character in it.
    const QPoint pos = mousePosition(*static_cast<const QMouseEvent *>(p_event));
    position = editor->cursorPositionAt(editor->mapFrom(viewport(), pos));
  }

  editor->deselect();
  editor->setCursorPosition(position);
}

void TablePreviewView::scrollContentsBy(int p_dx, int p_dy) {
  QTableView::scrollContentsBy(p_dx, p_dy);
  if (p_dy != 0 && !m_fittingRows) {
    scheduleRowFit();
  }
}

void TablePreviewView::layoutColumns() {
  invalidatePreferredSize();
  applyLayout();
}

void TablePreviewView::scheduleLayout() const {
  if (m_layoutTimer && !m_layoutTimer->isActive()) {
    m_layoutTimer->start();
  }
}

void TablePreviewView::scheduleRowFit() {
  if (m_rowFitTimer && !m_rowFitTimer->isActive()) {
    m_rowFitTimer->start();
  }
}

void TablePreviewView::applyLayout() {
  if (m_layingOut || !model() || model()->columnCount() <= 0 || model()->rowCount() <= 0) {
    return;
  }

  if (m_layoutTimer) {
    m_layoutTimer->stop();
  }

  QScopedValueRollback<bool> guard(m_layingOut, true);

  Solution solution;
  int observedViewport = -1;
  for (int pass = 0; pass < c_maxLayoutPasses; ++pass) {
    solution = solveGeometry(width(), observedViewport);

    // Columns first: Qt measures a wrapped row against the sections it finds,
    // so fitting rows before the widths are decided is what reserves a band
    // for text which then fits on fewer lines.
    for (int c = 0; c < solution.m_columnWidths.size(); ++c) {
      setColumnWidth(c, solution.m_columnWidths.at(c));
    }

    for (int r = 0; r < solution.m_rowHeights.size(); ++r) {
      setRowHeight(r, solution.m_rowHeights.at(r));
    }

    // Writing the sections can add or drop a scroll bar, which resizes the
    // viewport from underneath the plan. Ask Qt to settle, then compare: the
    // plan is only authoritative if the viewport it assumed is the one the
    // sheet actually has. The retry is bounded so an unusual style cannot spin.
    QTableView::updateGeometries();
    observeScrollBarChrome();

    const int actual = viewport()->width();
    if (actual == solution.m_viewportWidth || actual == observedViewport) {
      break;
    }

    observedViewport = actual;
  }

  m_plannedViewportWidth = viewport()->width();

  // Only a different plan makes the rows fitted for the previous one stale. A
  // pass which lands on the same widths - the common one, since most passes
  // answer a geometry update rather than a content change - would otherwise
  // throw away every lazily fitted row and measure it all over again.
  if (m_fittedColumnWidths != solution.m_columnWidths) {
    m_fittedColumnWidths = solution.m_columnWidths;
    m_fittedRows.clear();
  }

  for (int r = 0; r < solution.m_rowHeights.size(); ++r) {
    m_fittedRows.insert(r);
  }

  notifyPreferredGeometry(solution);

  // Whatever is on screen below the band still carries a default height.
  if (model()->rowCount() > solution.m_rowHeights.size()) {
    scheduleRowFit();
  }
}

void TablePreviewView::fitRowsAroundViewport() {
  if (m_fittingRows || m_layingOut || !model() || model()->columnCount() <= 0 ||
      model()->rowCount() <= 0) {
    return;
  }

  if (m_fittedColumnWidths.size() != model()->columnCount()) {
    // No authoritative plan yet, and fitting against sections the pass is
    // about to rewrite would only have to be undone.
    scheduleLayout();
    return;
  }

  QScopedValueRollback<bool> guard(m_fittingRows, true);
  const QStyleOptionViewItem base = baseViewOption();
  const int rows = model()->rowCount();
  auto vbar = verticalScrollBar();

  for (int round = 0; round < c_rowFitRounds; ++round) {
    // Recomputed every round: fitting a row moves the ones below it, so the
    // interval the viewport covers is not the one it covered before.
    int first = rowAt(0);
    if (first < 0) {
      first = 0;
    }
    int last = rowAt(qMax(0, viewport()->height() - 1));
    if (last < 0) {
      last = rows - 1;
    }
    first = qMax(0, first - c_rowFitOverscan);
    last = qMin(rows - 1, last + c_rowFitOverscan);

    QVector<int> pending;
    for (int r = first; r <= last && pending.size() < c_rowFitBatch; ++r) {
      if (!m_fittedRows.contains(r)) {
        pending.append(r);
      }
    }

    if (pending.isEmpty()) {
      return;
    }

    // Keep whatever the user is looking at where it is: the rows being fitted
    // change the content height above the viewport as well as inside it.
    const int anchorRow = qBound(0, rowAt(0), rows - 1);
    const int anchorBefore = rowViewportPosition(anchorRow);

    for (int r : pending) {
      setRowHeight(r, rowHeightFor(base, r, m_fittedColumnWidths));
      m_fittedRows.insert(r);
    }

    // setRowHeight() only *posts* the scroll bar's range update, so without
    // this the bar would still be bounded by the content height the rows just
    // grew past, and the correction below would be clamped away - which is a
    // visible jump for anyone fitting rows near the end of a long table.
    QTableView::updateGeometries();

    if (vbar) {
      const int anchorAfter = rowViewportPosition(anchorRow);
      vbar->setValue(vbar->value() + (anchorAfter - anchorBefore));
    }
  }

  // Bounded work per pass; the rest continues on the next event loop turn.
  scheduleRowFit();
}

void TablePreviewView::notifyPreferredGeometry(const Solution &p_solution) {
  const QSize preferred = preferredSize();
  const QSize constrained =
      constrainedSizeFor(p_solution, width() > 0 ? width() : preferred.width());

  // The pass just measured the band against the width the sheet actually has,
  // so seed the memo rather than measuring it all over again. Taken after the
  // intrinsic measurement, which drops the memo when it has to re-run.
  if (width() > 0) {
    m_constrainedSize = constrained;
    m_constrainedWidthKey = width();
  }

  const bool armed = m_notificationArmed;
  const bool changed = preferred != m_settledPreferredSize ||
                       constrained.height() != m_settledConstrainedHeight;

  m_notificationArmed = false;
  m_settledPreferredSize = preferred;
  m_settledConstrainedHeight = constrained.height();

  if (!m_settlementEstablished) {
    // The first settlement is the baseline the host measured on its own.
    m_settlementEstablished = true;
    return;
  }

  // A geometry-only pass answers a width the host chose; telling it about the
  // answer it asked for is how a measurement loop starts.
  if (!armed || !changed) {
    return;
  }

  qCDebug(previewTableLog) << "the sheet settled on" << preferred << "and" << constrained
                           << "at width" << width();
  emit preferredGeometryChanged();
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

  // The host's event filter watches this widget, not the view inside it, so
  // the sheet cannot reach it through updateGeometry() alone.
  connect(m_view, &TablePreviewView::preferredGeometryChanged, this,
          &TablePreviewWidget::handlePreferredGeometryChanged);

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
  // Plans the columns and fits the band against them, in that order.
  m_view->layoutColumns();
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
  // The host clamps the band to the text column, and a sheet whose columns
  // cannot even be compressed to their readable floor gets a horizontal
  // scroll bar which eats into the viewport. Reserving its height here rather
  // than in sizeHint() is what makes the reserve track the clamp: the width
  // passed in is the one the band ends up with, whereas the widget's own
  // geometry context still holds the previous layout pass at measuring time.
  return m_view->preferredSizeWithin(p_width).height();
}

bool TablePreviewWidget::event(QEvent *p_event) {
  if (p_event->type() == QEvent::LayoutRequest) {
    // Whatever queued this has been delivered; the next settlement may queue
    // another one.
    m_layoutRequestPending = false;
  }

  return PreviewWidget::event(p_event);
}

void TablePreviewWidget::handlePreferredGeometryChanged() {
  if (m_layoutRequestPending) {
    return;
  }

  // updateGeometry() posts a layout request to the *parent*, which the host
  // does not watch. Posting one here is what makes the host drop its cached
  // measurement and re-publish the reservation.
  m_layoutRequestPending = true;
  updateGeometry();
  QCoreApplication::postEvent(this, new QEvent(QEvent::LayoutRequest));
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
