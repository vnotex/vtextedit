#include "tablepreviewwidget.h"

#include <QApplication>
#include <QAbstractTextDocumentLayout>
#include <QAction>
#include <QActionGroup>
#include <QContextMenuEvent>
#include <QFocusEvent>
#include <QFont>
#include <QGuiApplication>
#include <QInputMethod>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPalette>
#include <QScopedValueRollback>
#include <QStringList>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextLayout>
#include <QTextTable>
#include <QTextTableCell>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtMath>

#include <functional>

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

// The delimiter markers are emitted at their minimum readable length: the
// serializer writes a compact table, so nothing is padded out to a column
// width.
static QString alignmentMarker(PreviewTableAlignment p_alignment) {
  switch (p_alignment) {
  case PreviewTableAlignment::Left:
    return QStringLiteral(":---");
  case PreviewTableAlignment::Right:
    return QStringLiteral("---:");
  case PreviewTableAlignment::Center:
    return QStringLiteral(":---:");
  default:
    return QStringLiteral("---");
  }
}

// Whether @p_code is one of the separators a table row can never carry: the
// serializer emits one source line per row, and every one of these would end
// that line early.
static bool isLineSeparator(ushort p_code) {
  return p_code == '\n' || p_code == '\r' || p_code == 0x2028 || p_code == 0x2029;
}

static bool hasLineSeparator(const QString &p_text) {
  for (int i = 0; i < p_text.size(); ++i) {
    if (isLineSeparator(p_text.at(i).unicode())) {
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
  for (const auto &row : p_cells) {
    for (int c = 0; c < columns; ++c) {
      const QString raw = c < row.size() ? row[c] : QString();
      if (hasLineSeparator(raw)) {
        return QString();
      }

      escaped.append(escapeCell(raw));
    }
  }

  auto emitRow = [&](const QString &p_prefix, int p_rowIdx) {
    QString line = p_prefix;
    line.append(QLatin1Char('|'));
    for (int c = 0; c < columns; ++c) {
      line.append(QLatin1Char(' '));
      line.append(escaped[p_rowIdx * columns + c]);
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
      line.append(alignmentMarker(alignment));
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
// TablePreviewDocument
// ---------------------------------------------------------------------------

// See the header for how these were measured.
const int TablePreviewDocument::c_maxCells = 300;

const int TablePreviewDocument::c_maxColumns = 200;

namespace {
// Padding inside one cell, in pixels. Small enough that a compact table does
// not turn into a spreadsheet, large enough that the text does not touch the
// border.
const qreal c_cellPadding = 3;

// The margin the document keeps around the table. The table itself spans the
// whole remaining width, so this is the only gap between the border and the
// edge of the band.
const qreal c_documentMargin = 2;
} // namespace

int TablePreviewDocument::normalizedColumnCount(const TablePreview &p_table) {
  int columns = p_table.alignments().size();
  for (const auto &row : p_table.cells()) {
    columns = qMax(columns, row.size());
  }

  return columns;
}

qint64 TablePreviewDocument::normalizedCellCount(const TablePreview &p_table) {
  return static_cast<qint64>(normalizedColumnCount(p_table)) *
         static_cast<qint64>(p_table.cells().size());
}

bool TablePreviewDocument::isWithinLimits(const TablePreview &p_table) {
  return normalizedColumnCount(p_table) <= c_maxColumns &&
         normalizedCellCount(p_table) <= c_maxCells;
}

TablePreviewDocument::TablePreviewDocument() : m_doc(new QTextDocument()) {
  // The sheet's undo granularity is one whole-table replacement on the
  // editor's own stack, so the inner document must not accumulate a second,
  // invisible one.
  m_doc->setUndoRedoEnabled(false);
  m_doc->setDocumentMargin(c_documentMargin);

  QTextOption option = m_doc->defaultTextOption();
  // WordWrap alone cannot break an unbroken token and would leave a long URL
  // or identifier overflowing its column, which with no horizontal scrolling
  // means unreachable.
  option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
  m_doc->setDefaultTextOption(option);
}

TablePreviewDocument::~TablePreviewDocument() = default;

QTextDocument *TablePreviewDocument::document() const { return m_doc.data(); }

QTextTable *TablePreviewDocument::table() const { return m_table; }

int TablePreviewDocument::rowCount() const { return m_rowCount; }

int TablePreviewDocument::columnCount() const { return m_columnCount; }

Qt::Alignment TablePreviewDocument::blockAlignment(int p_column) const {
  switch (m_alignments.value(p_column, PreviewTableAlignment::None)) {
  case PreviewTableAlignment::Center:
    return Qt::AlignHCenter;
  case PreviewTableAlignment::Right:
    return Qt::AlignRight;
  default:
    return Qt::AlignLeft;
  }
}

void TablePreviewDocument::setTable(const QSharedPointer<const TablePreview> &p_table) {
  if (p_table) {
    m_source = p_table->cells();
    m_alignments = p_table->alignments();
    m_rowPrefixes = p_table->rowPrefixes();
    m_delimiterPrefix = p_table->delimiterPrefix();
    m_declaredColumnCount = p_table->columnCount();
  } else {
    m_source.clear();
    m_alignments.clear();
    m_rowPrefixes.clear();
    m_delimiterPrefix.clear();
    m_declaredColumnCount = 0;
  }

  // Normalize exactly as the snapshot describes it: pad every row to the
  // widest one and extend the alignments. Nothing is ever discarded, so a body
  // row wider than the header stays visible - it is isRoundTrippable() which
  // then refuses to write it back.
  m_columnCount = m_alignments.size();
  for (const auto &row : m_source) {
    m_columnCount = qMax(m_columnCount, row.size());
  }

  for (auto &row : m_source) {
    while (row.size() < m_columnCount) {
      row.append(QString());
    }
  }

  while (m_alignments.size() < m_columnCount) {
    m_alignments.append(PreviewTableAlignment::None);
  }

  m_rowCount = m_source.size();

  build();
}

void TablePreviewDocument::build() {
  m_table = nullptr;
  m_doc->clear();
  // clear() rebuilds the document, which restores the default undo behavior.
  m_doc->setUndoRedoEnabled(false);

  if (m_rowCount <= 0 || m_columnCount <= 0) {
    return;
  }

  QTextTableFormat format;
  // The band is the sheet's whole width, and the column widths are owned by
  // Qt: every column is a VariableLength constraint, so the layout shares the
  // width out by content instead of the retired proportional-compression pass.
  format.setWidth(QTextLength(QTextLength::PercentageLength, 100));
  format.setColumnWidthConstraints(
      QVector<QTextLength>(m_columnCount, QTextLength(QTextLength::VariableLength, 0)));
  format.setCellPadding(c_cellPadding);
  format.setCellSpacing(0);
  format.setBorder(1);
  format.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
  format.setHeaderRowCount(1);
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
  // One shared line between neighbouring cells rather than two abutting ones.
  format.setBorderCollapse(true);
#endif

  QTextCursor cursor(m_doc.data());
  // One edit block for the whole build: QTextDocumentLayout relayouts the
  // frame an edit lands in, and the frame here is the table.
  cursor.beginEditBlock();
  m_table = cursor.insertTable(m_rowCount, m_columnCount, format);
  if (!m_table) {
    cursor.endEditBlock();
    return;
  }

  for (int r = 0; r < m_rowCount; ++r) {
    const auto &row = m_source.at(r);
    for (int c = 0; c < m_columnCount; ++c) {
      QTextTableCell cell = m_table->cellAt(r, c);
      if (!cell.isValid()) {
        continue;
      }

      const QString text = row.value(c);
      if (!text.isEmpty()) {
        cell.firstCursorPosition().insertText(text);
      }

      // The same writer applyCellFormats() uses, so the two cannot disagree
      // about what a cell looks like.
      applyCellFormat(r, c);
    }
  }

  // A QTextDocument's root frame always ends with a block, so the table is
  // necessarily followed by an empty paragraph. At the editor's font that
  // would add a whole empty line under the sheet and give the caret a resting
  // place outside every cell; shrink it to a single pixel instead. The caret
  // is kept out of it by TablePreviewSheet::clampCursorIntoTable().
  QTextCursor tail(m_doc.data());
  tail.movePosition(QTextCursor::End);
  QTextCharFormat tailChar;
  tailChar.setFontPointSize(1);
  tail.setCharFormat(tailChar);

  QTextBlockFormat tailBlock = tail.blockFormat();
  tailBlock.setTopMargin(0);
  tailBlock.setBottomMargin(0);
  tailBlock.setLineHeight(1, QTextBlockFormat::FixedHeight);
  tail.setBlockFormat(tailBlock);

  cursor.endEditBlock();
}

QVector<QVector<QString>> TablePreviewDocument::cells() const {
  QVector<QVector<QString>> matrix;
  if (!m_table) {
    return matrix;
  }

  const int rows = m_table->rows();
  const int columns = m_table->columns();
  matrix.reserve(rows);

  for (int r = 0; r < rows; ++r) {
    QVector<QString> row;
    row.reserve(columns);
    for (int c = 0; c < columns; ++c) {
      const QTextTableCell cell = m_table->cellAt(r, c);
      if (!cell.isValid()) {
        row.append(QString());
        continue;
      }

      QTextCursor cursor = cell.firstCursorPosition();
      cursor.setPosition(cell.lastCursorPosition().position(), QTextCursor::KeepAnchor);
      // selectedText() reports a block boundary as U+2029, which the
      // serializer rejects. A cell can only ever hold one block - Enter never
      // inserts one (it either does nothing or appends a whole row) and every
      // paste is sanitized - so this is defence in depth rather than an
      // expected shape.
      row.append(cursor.selectedText());
    }

    matrix.append(row);
  }

  return matrix;
}

bool TablePreviewDocument::isRoundTrippable() const {
  if (m_rowCount <= 0 || m_declaredColumnCount <= 0) {
    qCDebug(previewTableLog) << "not round-trippable: rows" << m_rowCount << "declared columns"
                             << m_declaredColumnCount;
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

bool TablePreviewDocument::isIntact() const {
  if (m_rowCount <= 0 || m_columnCount <= 0) {
    // Nothing was ever built, so nothing can have been taken apart.
    return true;
  }

  if (!m_doc || !m_doc->rootFrame()) {
    return false;
  }

  // Deliberately re-resolved instead of compared against m_table: that pointer
  // is what dangles in exactly the case this detects. The document only ever
  // holds the one table this object built, so "a table is still there" is the
  // whole question.
  for (auto frame : m_doc->rootFrame()->childFrames()) {
    if (qobject_cast<QTextTable *>(frame)) {
      return true;
    }
  }

  return false;
}

QString TablePreviewDocument::toMarkdown() const {
  if (!isRoundTrippable()) {
    return QString();
  }

  return TablePreviewSerializer::serialize(cells(), m_alignments, m_rowPrefixes,
                                           m_delimiterPrefix);
}

void TablePreviewDocument::applyCellFormat(int p_row, int p_column) {
  if (!m_table) {
    return;
  }

  const QTextTableCell cell = m_table->cellAt(p_row, p_column);
  if (!cell.isValid()) {
    return;
  }

  QTextCursor cursor = cell.firstCursorPosition();
  cursor.setPosition(cell.lastCursorPosition().position(), QTextCursor::KeepAnchor);

  QTextBlockFormat blockFormat = cursor.blockFormat();
  blockFormat.setAlignment(blockAlignment(p_column));
  cursor.setBlockFormat(blockFormat);

  if (p_row != 0) {
    return;
  }

  // The header is bold as a character format, not as Markdown: a cell holds
  // its raw source, so '**' here would be two literal asterisks. Both halves
  // are needed - the block's char format so a cell which is empty in the
  // source is still bold once it is typed into, and the merge so text which is
  // already there is restyled.
  QTextCharFormat headerFormat;
  headerFormat.setFontWeight(QFont::Bold);
  cursor.setBlockCharFormat(headerFormat);
  cursor.mergeCharFormat(headerFormat);
}

void TablePreviewDocument::applyCellFormats() {
  if (!m_table) {
    return;
  }

  const int rows = m_table->rows();
  const int columns = m_table->columns();
  for (int r = 0; r < rows; ++r) {
    for (int c = 0; c < columns; ++c) {
      applyCellFormat(r, c);
    }
  }
}

bool TablePreviewDocument::canAppendRow() const {
  if (!m_table || m_columnCount <= 0 || m_rowCount <= 0) {
    return false;
  }

  // qint64 for the same reason normalizedCellCount() uses it: the product of
  // two ints is what is being bounded here, and it must not be the overflow
  // which decides the answer.
  return qint64(m_rowCount + 1) * qint64(m_columnCount) <= qint64(c_maxCells);
}

bool TablePreviewDocument::appendRow() { return insertRow(m_rowCount); }

bool TablePreviewDocument::canInsertRow() const {
  // Where the row goes does not change the cell count, so this is exactly the
  // append bound.
  return canAppendRow();
}

bool TablePreviewDocument::canInsertColumn() const {
  if (!m_table || m_columnCount <= 0 || m_rowCount <= 0) {
    return false;
  }

  if (m_columnCount + 1 > c_maxColumns) {
    return false;
  }

  // qint64 for the same reason canAppendRow() uses it.
  return qint64(m_rowCount) * qint64(m_columnCount + 1) <= qint64(c_maxCells);
}

bool TablePreviewDocument::canDeleteRow(int p_row) const {
  // Row 0 is the header: see the header file for why it is not an ordinary
  // row. The table may shrink to a header-only table but not to nothing.
  return m_table && p_row > 0 && p_row < m_rowCount && m_rowCount > 1;
}

bool TablePreviewDocument::canDeleteColumn(int p_column) const {
  return m_table && p_column >= 0 && p_column < m_columnCount && m_columnCount > 1;
}

PreviewTableAlignment TablePreviewDocument::columnAlignment(int p_column) const {
  return m_alignments.value(p_column, PreviewTableAlignment::None);
}

void TablePreviewDocument::rewriteColumnConstraints() {
  if (!m_table) {
    return;
  }

  QTextTableFormat format = m_table->format().toTableFormat();
  format.setColumnWidthConstraints(
      QVector<QTextLength>(m_columnCount, QTextLength(QTextLength::VariableLength, 0)));
  m_table->setFormat(format);
}

bool TablePreviewDocument::insertRow(int p_row) {
  // The whole precondition runs before the edit block opens: QTextTable clamps
  // an out-of-range index silently while QVector::insert() asserts on one, and
  // the two diverging is exactly the drift the serializer answers by throwing
  // the edit away. Row 0 is refused because the header is not an ordinary row.
  if (!m_table || p_row <= 0 || p_row > m_rowCount || !canInsertRow()) {
    return false;
  }

  // One edit block for the whole insert, as in build(): the row, its prefix
  // and its formats have to reach TablePreviewWidget::handleContentsChanged()
  // as a single change. That slot re-runs isIntact() and rebuilds from source
  // when the table looks gone, so an intermediate state observed halfway
  // through would risk throwing the user's edit away.
  QTextCursor cursor(m_doc.data());
  cursor.beginEditBlock();

  if (p_row == m_rowCount) {
    // Appending is what appendRows() is for; insertRows() at the row count is
    // not the documented way to grow at the bottom.
    m_table->appendRows(1);
  } else {
    m_table->insertRows(p_row, 1);
  }

  // The prefix vector is per row, and the serializer refuses a matrix whose
  // size disagrees with it. m_delimiterPrefix is what arePrefixesSafe()
  // requires of every row after the header, so it is the only prefix a new
  // body row may carry.
  m_rowPrefixes.insert(p_row, m_delimiterPrefix);

  // Derived from the table rather than incremented, exactly as build() derives
  // it, so the cached count cannot drift if the Qt call ever refuses.
  m_rowCount = m_table->rows();

  for (int c = 0; c < m_columnCount; ++c) {
    // A new row carries no per-cell format, so its cells would otherwise be
    // left-aligned regardless of the column's alignment.
    applyCellFormat(p_row, c);
  }

  cursor.endEditBlock();
  return true;
}

bool TablePreviewDocument::removeRow(int p_row) {
  if (!canDeleteRow(p_row)) {
    return false;
  }

  QTextCursor cursor(m_doc.data());
  cursor.beginEditBlock();

  m_table->removeRows(p_row, 1);
  m_rowPrefixes.remove(p_row);
  m_rowCount = m_table->rows();

  cursor.endEditBlock();
  return true;
}

bool TablePreviewDocument::insertColumn(int p_column) {
  if (!m_table || p_column < 0 || p_column > m_columnCount || !canInsertColumn()) {
    return false;
  }

  QTextCursor cursor(m_doc.data());
  cursor.beginEditBlock();

  if (p_column == m_columnCount) {
    m_table->appendColumns(1);
  } else {
    m_table->insertColumns(p_column, 1);
  }

  // One alignment per column, which the delimiter row is written from.
  m_alignments.insert(p_column, PreviewTableAlignment::None);
  m_columnCount = m_table->columns();
  // The width the document now intends to serialize as. Leaving this behind
  // makes isRoundTrippable() refuse, toMarkdown() empty and
  // flushPendingCommit() restore the source - the column would vanish at
  // commit time rather than be refused visibly.
  m_declaredColumnCount = m_columnCount;
  rewriteColumnConstraints();

  // Not just the new column: every column right of it has shifted, so its
  // cells now carry the previous neighbour's alignment.
  applyCellFormats();

  cursor.endEditBlock();
  return true;
}

bool TablePreviewDocument::removeColumn(int p_column) {
  if (!canDeleteColumn(p_column)) {
    return false;
  }

  QTextCursor cursor(m_doc.data());
  cursor.beginEditBlock();

  m_table->removeColumns(p_column, 1);
  m_alignments.remove(p_column);
  m_columnCount = m_table->columns();
  m_declaredColumnCount = m_columnCount;
  rewriteColumnConstraints();
  applyCellFormats();

  cursor.endEditBlock();
  return true;
}

bool TablePreviewDocument::setColumnAlignment(int p_column, PreviewTableAlignment p_alignment) {
  if (!m_table || p_column < 0 || p_column >= m_columnCount) {
    return false;
  }

  if (m_alignments.value(p_column) == p_alignment) {
    // Unchanged: refused so an idempotent click does not arm a commit.
    return false;
  }

  m_alignments[p_column] = p_alignment;

  QTextCursor cursor(m_doc.data());
  cursor.beginEditBlock();

  const int rows = m_table->rows();
  for (int r = 0; r < rows; ++r) {
    applyCellFormat(r, p_column);
  }

  cursor.endEditBlock();
  return true;
}

void TablePreviewDocument::applyPalette(const QPalette &p_palette) {
  if (!m_table) {
    return;
  }

  QTextTableFormat format = m_table->format().toTableFormat();
  // The only colour the table itself owns: everything else (text, selection,
  // the band behind it) is resolved from the palette by the paint path.
  format.setBorderBrush(p_palette.brush(QPalette::Mid));
  m_table->setFormat(format);
}

// ---------------------------------------------------------------------------
// TablePreviewSheet
// ---------------------------------------------------------------------------

namespace {
// Whether @p_text holds anything a cell could keep once every line separator
// has been taken out of it.
bool hasCellContent(const QString &p_text) {
  for (int i = 0; i < p_text.size(); ++i) {
    if (!isLineSeparator(p_text.at(i).unicode())) {
      return true;
    }
  }

  return false;
}

// Every separator the serializer rejects, collapsed into a single space, so a
// pasted paragraph still lands in the cell as the one line a table row is.
QString sanitizeForCell(const QString &p_text) {
  QString result;
  result.reserve(p_text.size());

  bool afterSeparator = false;
  for (int i = 0; i < p_text.size(); ++i) {
    const QChar ch = p_text.at(i);
    if (isLineSeparator(ch.unicode())) {
      // One space for a whole run, so a CRLF pair and a blank line between two
      // paragraphs both come out as a single gap.
      if (!afterSeparator) {
        result.append(QLatin1Char(' '));
      }
      afterSeparator = true;
      continue;
    }

    afterSeparator = false;
    result.append(ch);
  }

  return result;
}
} // namespace

TablePreviewSheet::TablePreviewSheet(QWidget *p_parent) : QTextEdit(p_parent) {
  setFrameShape(QFrame::NoFrame);
  setLineWrapMode(QTextEdit::WidgetWidth);
  // The sheet renders at its full natural height, so there is never anything
  // to scroll to: the editor underneath owns the whole vertical axis.
  setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setAcceptRichText(false);
  // WordWrap alone cannot break an unbroken token and would leave a long URL
  // or identifier overflowing its column, which with no horizontal scrolling
  // means unreachable. QTextEdit applies its own mode to whatever document it
  // is given, so this has to be stated here as well as on the document.
  setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
  // Tab is intercepted in keyPressEvent() before Qt's focus chain sees it.
  setTabChangesFocus(false);
  setFocusPolicy(Qt::StrongFocus);

  QSizePolicy policy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  policy.setHeightForWidth(true);
  setSizePolicy(policy);

  connect(this, &QTextEdit::cursorPositionChanged, this,
          &TablePreviewSheet::handleCursorPositionChanged);
  connect(this, &QTextEdit::selectionChanged, this,
          &TablePreviewSheet::handleSelectionChanged);
}

void TablePreviewSheet::setTableDocument(TablePreviewDocument *p_document) {
  m_document = p_document;
  if (!p_document || !p_document->document()) {
    return;
  }

  // QTextEdit only takes ownership of a document it parented itself, and this
  // one belongs to TablePreviewDocument.
  setDocument(p_document->document());
  // QTextEdit pushes its own undo setting onto whatever document it is given,
  // and this one must not accumulate a second, invisible undo stack: the
  // granularity the user sees is one whole-table replacement on the editor's.
  setUndoRedoEnabled(false);

  if (auto layout = p_document->document()->documentLayout()) {
    connect(layout, &QAbstractTextDocumentLayout::documentSizeChanged, this,
            &TablePreviewSheet::handleDocumentSizeChanged);
  }
}

int TablePreviewSheet::horizontalChrome() const {
  // What the frame and the viewport margins really take. Read from the live
  // geometry once there is one, because a style may inset the viewport by more
  // than the frame width alone; both scroll bars are off, so neither can
  // inflate this.
  const int live = viewport() ? width() - viewport()->width() : 0;
  return live > 0 ? live : frameWidth() * 2;
}

int TablePreviewSheet::verticalChrome() const {
  const int live = viewport() ? height() - viewport()->height() : 0;
  return live > 0 ? live : frameWidth() * 2;
}

bool TablePreviewSheet::hasHeightForWidth() const { return true; }

int TablePreviewSheet::heightForWidth(int p_outerWidth) const {
  auto doc = document();
  if (!doc) {
    return 0;
  }

  // The width conversion is not optional. p_outerWidth is the width the host
  // is about to assign to the *widget*; the document lays out inside the
  // viewport, so assigning it straight to setTextWidth() would measure a
  // narrower band than the sheet gets - and with the scroll bars off, the
  // content clipped by that mistake would be unreachable.
  const int chrome = horizontalChrome();
  const qreal textWidth = p_outerWidth > chrome ? qreal(p_outerWidth - chrome) : qreal(-1);

  // Laying the document out synchronously emits documentSizeChanged, and
  // handing that straight back to the host is how a measurement loop starts.
  QScopedValueRollback<bool> guard(m_measuring, true);

  if (textWidth > 0 && !qFuzzyCompare(doc->textWidth() + 1, textWidth + 1)) {
    doc->setTextWidth(textWidth);
  }

  const int height = qCeil(doc->documentLayout()->documentSize().height()) + verticalChrome();

  // The probed width is deliberately left on the document. The host measures
  // at the width it is about to assign - preferredSize() feeds the same value
  // into the reservation, which becomes the widget's geometry in the same call
  // stack, with no paint in between - so restoring the previous width here
  // would only pay for a second full relayout and then a third from the
  // resizeEvent. At the measured cost per cell that is the single most
  // expensive thing this function could do on an interactive resize.
  return height;
}

QSize TablePreviewSheet::sizeHint() const {
  // Width 0 on purpose: TablePreviewWidget::preferredWidthFraction() is 1.0,
  // so the host resolves the band to the full available content width rather
  // than to a natural width the sheet would then have to be re-measured
  // against. Only the height carries information here, and even that is a
  // fallback - the host asks heightForWidth() whenever it has a width.
  return QSize(0, heightForWidth(width()));
}

QSize TablePreviewSheet::minimumSizeHint() const {
  // QAbstractScrollArea's own minimum reserves room for scroll bars and a few
  // characters of text, which would stop the band from ever being narrow.
  return QSize(0, 0);
}

int TablePreviewSheet::currentCellIndex() const {
  const QTextCursor cursor = textCursor();
  QTextTable *table = cursor.currentTable();
  if (!table) {
    return -1;
  }

  const QTextTableCell cell = table->cellAt(cursor);
  if (!cell.isValid()) {
    return -1;
  }

  return cell.row() * table->columns() + cell.column();
}

void TablePreviewSheet::commitPreedit() {
  // QInputMethod has no receiver: it acts on the application's focus object,
  // exactly as reset() does. A flush runs on background sheets too - the host
  // flushes every sheet it removes, and every focus-out and debounce timeout
  // lands here - so committing from a sheet which does not own the focus would
  // finalize the composition of whatever widget really does. A sheet without
  // the focus has no composition of its own to lose either.
  if (QGuiApplication::focusObject() != this) {
    return;
  }

  if (auto method = QGuiApplication::inputMethod()) {
    // The preedit is not in the document yet, so serializing across one would
    // silently drop whatever the user has already typed into it.
    method->commit();
  }
}

void TablePreviewSheet::cancelComposition() {
  // QInputMethod has no receiver: it acts on the application's focus object.
  // Resetting from a sheet which does not own the focus would cancel - or on
  // some platforms commit - the composition of whatever widget really does,
  // and a background sheet is revoked on every preview rebuild.
  if (QGuiApplication::focusObject() != this) {
    return;
  }

  // Collapse first. Installing a changed preedit makes QWidgetTextControl
  // remove the current selection before anything else, and here that would be
  // a real deletion - both for a synchronous callback from the reset below and
  // for the explicit clear further down.
  QTextCursor collapsed = textCursor();
  if (collapsed.hasSelection()) {
    collapsed.setPosition(collapsed.position());
    setTextCursor(collapsed);
  }

  const int caret = textCursor().position();

  {
    // The Windows input context hands the composition back as a *commit* event
    // before it cancels the native context, and other platforms send an empty
    // clearing event instead. Both re-enter this sheet - which is still
    // writable, deliberately, so the clear below can reach the control - and
    // would change the document, which is the opposite of cancelling.
    QScopedValueRollback<bool> guard(m_cancellingComposition, true);
    if (auto method = QGuiApplication::inputMethod()) {
      method->reset();
    }
  }

  // reset() only tells the platform. What the text control was already given
  // lives in the block's QTextLayout, and nothing above clears that, so a
  // platform which sends no clearing event of its own would leave the preedit
  // rendered. Delegated straight to the base, so the guard above does not
  // swallow it.
  const QTextBlock block = textCursor().block();
  if (block.isValid() && block.layout() && !block.layout()->preeditAreaText().isEmpty()) {
    QInputMethodEvent clear;
    QTextEdit::inputMethodEvent(&clear);
  }

  QTextCursor restored = textCursor();
  restored.setPosition(qBound(0, caret, qMax(0, document()->characterCount() - 1)));
  setTextCursor(restored);
  clampCursorIntoTable();
}

QPalette TablePreviewSheet::applyPalette() {
  // The host propagates the editor font and nothing else, so every colour has
  // to come from this sheet's own effective palette. Re-seed it from the
  // parent on every pass: setPalette() marks the palette as explicitly set,
  // which would otherwise stop a later theme change from reaching the sheet.
  QPalette effective = parentWidget() ? parentWidget()->palette() : QApplication::palette();
  // The band belongs to the editor. A sheet which fills its own background
  // would paint an opaque rectangle over whatever the editor draws there.
  effective.setBrush(QPalette::Base, Qt::transparent);
  setPalette(effective);
  if (viewport()) {
    viewport()->setAutoFillBackground(false);
  }

  if (m_document) {
    m_document->applyPalette(effective);
  }

  return effective;
}

void TablePreviewSheet::refreshPalette() {
  if (m_applyingFormats) {
    return;
  }

  QScopedValueRollback<bool> guard(m_applyingFormats, true);
  applyPalette();
}

void TablePreviewSheet::refreshFormats() {
  if (m_applyingFormats) {
    return;
  }

  QScopedValueRollback<bool> guard(m_applyingFormats, true);
  applyPalette();

  if (m_document) {
    m_document->applyCellFormats();
  }
}

void TablePreviewSheet::clampCursorIntoTable() {
  QTextTable *table = m_document ? m_document->table() : nullptr;
  if (!table) {
    return;
  }

  QTextCursor cursor = textCursor();
  if (cursor.currentTable() == table) {
    return;
  }

  // Everything outside the table is the shrunken block a QTextDocument always
  // keeps after one, so the nearest cell is either the first or the last.
  const QTextTableCell cell = cursor.position() <= table->firstPosition()
                                  ? table->cellAt(0, 0)
                                  : table->cellAt(table->rows() - 1, table->columns() - 1);
  if (!cell.isValid()) {
    return;
  }

  setTextCursor(cursor.position() <= table->firstPosition() ? cell.firstCursorPosition()
                                                            : cell.lastCursorPosition());
}

void TablePreviewSheet::clampSelectionIntoOneCell() {
  QTextTable *table = m_document ? m_document->table() : nullptr;
  if (!table) {
    return;
  }

  QTextCursor cursor = textCursor();
  if (!cursor.hasSelection()) {
    return;
  }

  const QTextTableCell anchorCell = table->cellAt(cursor.anchor());
  const QTextTableCell caretCell = table->cellAt(cursor.position());
  if (caretCell.isValid() && anchorCell.isValid() && anchorCell == caretCell) {
    return;
  }

  // Keep the end the caret is at; the selection is what has to shrink.
  QTextTableCell target = caretCell.isValid() ? caretCell : anchorCell;
  if (!target.isValid() && m_lastCellIndex >= 0 && table->columns() > 0) {
    // Neither end is in a cell at all, which is what Ctrl+A produces: the
    // anchor lands before the table and the caret in the block after it. Fall
    // back to the cell the caret was last seen in.
    target = table->cellAt(m_lastCellIndex / table->columns(),
                           m_lastCellIndex % table->columns());
  }

  if (!target.isValid()) {
    clampCursorIntoTable();
    return;
  }

  const int first = target.firstPosition();
  const int last = target.lastPosition();
  int anchor = first;
  int caret = last;
  if (caretCell.isValid() || anchorCell.isValid()) {
    anchor = qBound(first, cursor.anchor(), last);
    caret = qBound(first, cursor.position(), last);
  }
  // Otherwise the whole cell, which is what "select all" means once it can
  // only mean "inside this cell".

  cursor.setPosition(anchor);
  cursor.setPosition(caret, QTextCursor::KeepAnchor);
  setTextCursor(cursor);
}

void TablePreviewSheet::clearSelection() {
  QTextCursor cursor = textCursor();
  if (!cursor.hasSelection()) {
    return;
  }

  // Collapse onto the caret. The caret position is already valid, so no
  // clamping is needed - and must not happen, since it would move the caret.
  cursor.setPosition(cursor.position());
  setTextCursor(cursor);
}

void TablePreviewSheet::handleCursorPositionChanged() {
  if (!m_clampingCursor) {
    // Both clamps rewrite the cursor, which re-enters this slot.
    QScopedValueRollback<bool> guard(m_clampingCursor, true);
    clampCursorIntoTable();
    clampSelectionIntoOneCell();
  }

  const int index = currentCellIndex();
  if (index == m_lastCellIndex) {
    return;
  }

  const bool leftACell = m_lastCellIndex >= 0;
  m_lastCellIndex = index;
  if (leftACell) {
    emit cellLeft();
  }
}

void TablePreviewSheet::handleSelectionChanged() {
  if (m_clampingCursor) {
    return;
  }

  // Only the selection: collapsing the caret here would undo a legitimate
  // in-cell selection the user is still dragging out.
  QScopedValueRollback<bool> guard(m_clampingCursor, true);
  clampSelectionIntoOneCell();
}

void TablePreviewSheet::handleDocumentSizeChanged() {
  if (m_measuring || m_applyingGeometry || m_applyingFormats) {
    // Either this sheet is measuring itself, or it is answering a geometry the
    // host has just chosen. Reporting either one back would feed the
    // measurement into itself.
    return;
  }

  const int outerWidth = width();
  const int height = heightForWidth(outerWidth);
  if (outerWidth == m_notifiedOuterWidth && height == m_notifiedHeight) {
    // The host caches its measurements and re-publishes on a layout request,
    // so an unchanged answer has to terminate here rather than bounce.
    return;
  }

  m_notifiedOuterWidth = outerWidth;
  m_notifiedHeight = height;

  qCDebug(previewTableLog) << "the sheet settled on" << height << "at width" << outerWidth;
  emit preferredGeometryChanged();
}

bool TablePreviewSheet::moveToAdjacentCell(bool p_forward) {
  QTextTable *table = m_document ? m_document->table() : nullptr;
  if (!table || table->rows() <= 0 || table->columns() <= 0) {
    return false;
  }

  // Leaving a cell commits it, so whatever the input method still holds has to
  // land in the document first.
  commitPreedit();
  clampCursorIntoTable();

  QTextCursor cursor = textCursor();
  if (!cursor.movePosition(p_forward ? QTextCursor::NextCell : QTextCursor::PreviousCell)) {
    // Wrap: past the last cell is the first one, and before the first is the
    // last.
    const QTextTableCell target = p_forward
                                      ? table->cellAt(0, 0)
                                      : table->cellAt(table->rows() - 1, table->columns() - 1);
    if (!target.isValid()) {
      return false;
    }

    cursor = target.firstCursorPosition();
  }

  setTextCursor(cursor);
  return true;
}

bool TablePreviewSheet::deleteWithinCell(const QKeyEvent *p_event) {
  QTextTable *table = m_document ? m_document->table() : nullptr;
  if (!table) {
    return false;
  }

  const bool completeLine = p_event->matches(QKeySequence::DeleteCompleteLine);
  QTextCursor::MoveOperation operation = QTextCursor::NoMove;
  if (p_event->matches(QKeySequence::DeleteEndOfWord)) {
    operation = QTextCursor::NextWord;
  } else if (p_event->matches(QKeySequence::DeleteStartOfWord)) {
    operation = QTextCursor::PreviousWord;
  } else if (p_event->matches(QKeySequence::DeleteEndOfLine)) {
    operation = QTextCursor::EndOfLine;
  } else if (!completeLine) {
    return false;
  }

  clampCursorIntoTable();
  clampSelectionIntoOneCell();

  QTextCursor cursor = textCursor();
  const QTextTableCell cell = table->cellAt(cursor.position());
  if (!cell.isValid()) {
    // Swallowed rather than handed to the base, which would delete outside the
    // table.
    return true;
  }

  const int first = cell.firstPosition();
  const int last = cell.lastPosition();

  if (!cursor.hasSelection()) {
    if (completeLine) {
      // A cell is one block, so "the whole line" is the whole cell.
      cursor.setPosition(first);
      cursor.setPosition(last, QTextCursor::KeepAnchor);
    } else {
      cursor.movePosition(operation, QTextCursor::KeepAnchor);
    }
  }

  // Whatever the move reached for, the deletion stays inside the cell.
  const int anchor = qBound(first, cursor.anchor(), last);
  const int position = qBound(first, cursor.position(), last);

  cursor.setPosition(anchor);
  if (anchor != position) {
    cursor.setPosition(position, QTextCursor::KeepAnchor);
    cursor.removeSelectedText();
  }

  setTextCursor(cursor);
  return true;
}

bool TablePreviewSheet::isAtTopEdge() const {
  QTextCursor probe = textCursor();
  if (!probe.currentTable()) {
    return true;
  }

  // Ask the layout rather than counting rows: a wrapped cell has several
  // visual lines and only the first one is an edge.
  if (!probe.movePosition(QTextCursor::Up)) {
    return true;
  }

  return probe.currentTable() == nullptr;
}

bool TablePreviewSheet::isAtBottomEdge() const {
  QTextCursor probe = textCursor();
  if (!probe.currentTable()) {
    return true;
  }

  if (!probe.movePosition(QTextCursor::Down)) {
    return true;
  }

  return probe.currentTable() == nullptr;
}

bool TablePreviewSheet::appendRowFromLastCell() {
  // Everything below the accept point is a preflight: it must not touch the
  // document or the cursor, because a refused Enter has to remain the inert
  // swallow it has always been.
  if (isReadOnly()) {
    // applyEditability() is the single writer of this flag and already folds in
    // the editor's read-only state, a revoked authority and a table which is
    // not round-trippable, so this one test covers all of them.
    return false;
  }

  QTextTable *table = m_document ? m_document->table() : nullptr;
  if (!table || table->rows() <= 0 || table->columns() <= 0) {
    return false;
  }

  const auto isInLastCell = [](const QTextTable *p_table, const QTextCursor &p_cursor) {
    const QTextTableCell cell = p_table->cellAt(p_cursor.position());
    return cell.isValid() && cell.row() == p_table->rows() - 1 &&
           cell.column() == p_table->columns() - 1;
  };

  if (!isInLastCell(table, textCursor()) || !m_document->canAppendRow()) {
    return false;
  }

  // Accepted from here on, so the side effects may run. The preedit has to
  // land in the document before the row is added, otherwise the composition
  // would be committed into a cell the caret has already left.
  commitPreedit();
  clampCursorIntoTable();
  clearSelection();

  // commitPreedit() can insert text synchronously, which moves the caret and
  // changes the document, so both conditions are re-resolved rather than
  // trusted from before it ran.
  table = m_document->table();
  if (!table || !isInLastCell(table, textCursor()) || !m_document->canAppendRow()) {
    return false;
  }

  if (!m_document->appendRow()) {
    return false;
  }

  const QTextTableCell target = table->cellAt(table->rows() - 1, 0);
  if (!target.isValid()) {
    return false;
  }

  // Moving the caret out of the previous cell is what emits cellLeft(), and
  // therefore what commits the row the user has just finished typing.
  setTextCursor(target.firstCursorPosition());
  return true;
}

void TablePreviewSheet::keyPressEvent(QKeyEvent *p_event) {
  // Undo and redo belong to the editor: the inner document has no undo stack
  // precisely so the granularity is one whole-table replacement. Both are
  // relayed rather than forwarded, because a pending edit has to be written
  // back before the editor's stack is moved.
  if (p_event->matches(QKeySequence::Undo)) {
    emit undoRequested();
    p_event->accept();
    return;
  }

  if (p_event->matches(QKeySequence::Redo)) {
    emit redoRequested();
    p_event->accept();
    return;
  }

  const Qt::KeyboardModifiers modifiers = p_event->modifiers();
  switch (p_event->key()) {
  case Qt::Key_Return:
  case Qt::Key_Enter:
    // One cell is one line: a table row is a single source line, and every
    // separator that could end it is rejected by the serializer. So Enter
    // never inserts anything - but in the last cell of the last row it means
    // "one more row", the way it does in every other table editor.
    //
    // KeypadModifier is masked off rather than compared away: the keypad's
    // Enter is the very key Qt::Key_Enter stands for, and native events carry
    // that modifier, so requiring Qt::NoModifier would make the branch
    // unreachable for it. Every semantic modifier still swallows.
    if (!(modifiers & ~Qt::KeypadModifier)) {
      appendRowFromLastCell();
    }
    // Accepted either way, so Enter is never handed to QTextEdit, which would
    // split the cell into two blocks.
    p_event->accept();
    return;

  case Qt::Key_Tab:
    if (!modifiers.testFlag(Qt::ControlModifier) && moveToAdjacentCell(true)) {
      p_event->accept();
      return;
    }
    break;

  case Qt::Key_Backtab:
    if (!modifiers.testFlag(Qt::ControlModifier) && moveToAdjacentCell(false)) {
      p_event->accept();
      return;
    }
    break;

  case Qt::Key_Escape:
    commitPreedit();
    emit focusEscapeRequested(FocusEscapeDirection::Keep);
    p_event->accept();
    return;

  case Qt::Key_Up:
    if (modifiers == Qt::NoModifier && isAtTopEdge()) {
      commitPreedit();
      emit focusEscapeRequested(FocusEscapeDirection::Up);
      p_event->accept();
      return;
    }
    break;

  case Qt::Key_Down:
    if (modifiers == Qt::NoModifier && isAtBottomEdge()) {
      commitPreedit();
      emit focusEscapeRequested(FocusEscapeDirection::Down);
      p_event->accept();
      return;
    }
    break;

  default:
    break;
  }

  // Defence in depth. The selection is already confined whenever the cursor
  // moves, but a mutating key is the one thing which must never see a
  // selection which crosses a cell boundary.
  if (!isReadOnly()) {
    clampSelectionIntoOneCell();

    // The delete shortcuts are the exception the clamps cannot cover: they
    // build their selection and remove it in one base-handler call. See
    // deleteWithinCell().
    if (deleteWithinCell(p_event)) {
      p_event->accept();
      return;
    }
  }

  QTextEdit::keyPressEvent(p_event);
}

void TablePreviewSheet::wheelEvent(QWheelEvent *p_event) {
  // Nothing to scroll and nothing to zoom: the movement is the editor's.
  p_event->ignore();
}

void TablePreviewSheet::focusInEvent(QFocusEvent *p_event) {
  QTextEdit::focusInEvent(p_event);
  clampCursorIntoTable();
  m_lastCellIndex = currentCellIndex();
}

void TablePreviewSheet::focusOutEvent(QFocusEvent *p_event) {
  commitPreedit();
  QTextEdit::focusOutEvent(p_event);
  emit focusLost();
}

void TablePreviewSheet::mousePressEvent(QMouseEvent *p_event) {
  // The base handler puts the caret at the exact character under the pointer,
  // which is the whole point of this substrate; it can also park it in the
  // block after the table when the click lands below the last row.
  QTextEdit::mousePressEvent(p_event);
  clampCursorIntoTable();
}

void TablePreviewSheet::focusCell(int p_row, int p_column) {
  QTextTable *table = m_document ? m_document->table() : nullptr;
  if (!table || table->rows() <= 0 || table->columns() <= 0) {
    return;
  }

  // Clamped rather than trusted: after a delete the index the caret came from
  // may no longer exist, and removeRows()/removeColumns() can park the caret
  // in the trailing block a QTextDocument always keeps after a table.
  const QTextTableCell cell = table->cellAt(qBound(0, p_row, table->rows() - 1),
                                            qBound(0, p_column, table->columns() - 1));
  if (cell.isValid()) {
    setTextCursor(cell.firstCursorPosition());
  }

  clampCursorIntoTable();
  clampSelectionIntoOneCell();
}

QMenu *TablePreviewSheet::buildTableMenu(QMenu *p_parent) {
  QTextTable *table = m_document ? m_document->table() : nullptr;
  if (!table) {
    return nullptr;
  }

  const QTextTableCell cell = table->cellAt(textCursor());
  if (!cell.isValid()) {
    // No cell to be relative to, so "this row" and "this column" mean nothing.
    return nullptr;
  }

  const int row = cell.row();
  const int column = cell.column();
  // isReadOnly() is the single test which covers all of it: applyEditability()
  // already folds the editor's read-only state, a revoked authority and a
  // table which is not round-trippable into this one flag.
  const bool writable = !isReadOnly();

  auto menu = new QMenu(tr("Table"), p_parent);
  menu->setObjectName(QStringLiteral("TablePreviewTableMenu"));

  auto addOperation = [this, menu, writable](const QString &p_text, const QString &p_name,
                                             bool p_allowed, std::function<void()> p_operation) {
    QAction *action = menu->addAction(p_text);
    action->setObjectName(p_name);
    action->setEnabled(writable && p_allowed);
    connect(action, &QAction::triggered, this, p_operation);
    return action;
  };

  addOperation(tr("Insert Row Above"), QStringLiteral("InsertRowAbove"),
               row > 0 && m_document->canInsertRow(), [this, row, column]() {
                 if (m_document->insertRow(row)) {
                   focusCell(row, column);
                 }
               });

  addOperation(tr("Insert Row Below"), QStringLiteral("InsertRowBelow"),
               m_document->canInsertRow(), [this, row, column]() {
                 if (m_document->insertRow(row + 1)) {
                   focusCell(row + 1, column);
                 }
               });

  addOperation(tr("Delete Row"), QStringLiteral("DeleteRow"), m_document->canDeleteRow(row),
               [this, row, column]() {
                 if (m_document->removeRow(row)) {
                   focusCell(row, column);
                 }
               });

  menu->addSeparator();

  addOperation(tr("Insert Column Left"), QStringLiteral("InsertColumnLeft"),
               m_document->canInsertColumn(), [this, row, column]() {
                 if (m_document->insertColumn(column)) {
                   focusCell(row, column);
                 }
               });

  addOperation(tr("Insert Column Right"), QStringLiteral("InsertColumnRight"),
               m_document->canInsertColumn(), [this, row, column]() {
                 if (m_document->insertColumn(column + 1)) {
                   focusCell(row, column + 1);
                 }
               });

  addOperation(tr("Delete Column"), QStringLiteral("DeleteColumn"),
               m_document->canDeleteColumn(column), [this, row, column]() {
                 if (m_document->removeColumn(column)) {
                   focusCell(row, column);
                 }
               });

  menu->addSeparator();

  auto alignmentMenu = menu->addMenu(tr("Alignment"));
  alignmentMenu->setObjectName(QStringLiteral("TablePreviewAlignmentMenu"));
  auto group = new QActionGroup(alignmentMenu);
  group->setExclusive(true);

  const PreviewTableAlignment current = m_document->columnAlignment(column);
  const struct {
    PreviewTableAlignment m_alignment;
    const char *m_name;
    QString m_text;
  } entries[] = {
      {PreviewTableAlignment::None, "AlignmentDefault", tr("Default")},
      {PreviewTableAlignment::Left, "AlignmentLeft", tr("Left")},
      {PreviewTableAlignment::Center, "AlignmentCenter", tr("Center")},
      {PreviewTableAlignment::Right, "AlignmentRight", tr("Right")},
  };

  for (const auto &entry : entries) {
    QAction *action = alignmentMenu->addAction(entry.m_text);
    action->setObjectName(QString::fromLatin1(entry.m_name));
    action->setCheckable(true);
    action->setChecked(entry.m_alignment == current);
    action->setEnabled(writable);
    group->addAction(action);

    const PreviewTableAlignment alignment = entry.m_alignment;
    connect(action, &QAction::triggered, this, [this, row, column, alignment]() {
      // A no-op returns false and arms nothing; a real change reaches the
      // commit machinery as the document change the re-formatting is.
      if (m_document->setColumnAlignment(column, alignment)) {
        focusCell(row, column);
      }
    });
  }

  return menu;
}

QMenu *TablePreviewSheet::createContextMenu(const QPoint &p_viewportPos) {
  QTextTable *table = m_document ? m_document->table() : nullptr;
  const QTextCursor clicked = cursorForPosition(p_viewportPos);
  // Deliberately not clamped: a click below the last row lands in the shrunken
  // block a QTextDocument always keeps after a table, and clamping that into
  // the nearest cell would offer row and column operations relative to a cell
  // the user never pointed at.
  const bool hitACell = table && !clicked.isNull() && clicked.currentTable() == table &&
                        table->cellAt(clicked).isValid();

  if (hitACell) {
    // "This row" and "this column" have to mean the cell the user pointed at,
    // not the one the caret happens to be in. A click inside an existing
    // selection is left alone, so a right click on a selection can still act
    // on it - that is what the standard menu's Cut and Copy are relative to.
    const QTextCursor caret = textCursor();
    const bool insideSelection = caret.hasSelection() &&
                                 clicked.position() >= qMin(caret.anchor(), caret.position()) &&
                                 clicked.position() <= qMax(caret.anchor(), caret.position());
    if (!insideSelection) {
      setTextCursor(clicked);
    }
  }

  // Null for a document which cannot offer one at all; the caller still gets a
  // menu, because the table operations below are appended to it.
  QMenu *menu = createStandardContextMenu(p_viewportPos);
  if (!menu) {
    menu = new QMenu(this);
  }

  if (hitACell) {
    if (QMenu *tableMenu = buildTableMenu(menu)) {
      // At the front, not appended: the table operations are what the sheet is
      // right-clicked for, and QTextEdit's standard menu ends with entries -
      // Select All among them - which would otherwise bury them.
      QAction *first = menu->actions().isEmpty() ? nullptr : menu->actions().first();
      if (first) {
        menu->insertMenu(first, tableMenu);
        menu->insertSeparator(first);
      } else {
        menu->addMenu(tableMenu);
      }
    }
  }

  return menu;
}

void TablePreviewSheet::contextMenuEvent(QContextMenuEvent *p_event) {
  // Already in viewport coordinates: the scroll area forwards the click which
  // landed on its viewport without retranslating it, and both scroll bars are
  // off, so there is no offset to add either.
  QScopedPointer<QMenu> menu(createContextMenu(p_event->pos()));
  menu->exec(p_event->globalPos());
  p_event->accept();
}

void TablePreviewSheet::resizeEvent(QResizeEvent *p_event) {
  // QTextEdit re-lays the document out for the new viewport width, which emits
  // documentSizeChanged for a size the host itself chose.
  QScopedValueRollback<bool> guard(m_applyingGeometry, true);
  QTextEdit::resizeEvent(p_event);
}

void TablePreviewSheet::inputMethodEvent(QInputMethodEvent *p_event) {
  if (!p_event) {
    QTextEdit::inputMethodEvent(p_event);
    return;
  }

  if (m_cancellingComposition) {
    // A platform callback from inside cancelComposition()'s reset(). Cancelling
    // is neither committing nor deleting, so it is discarded outright.
    qCDebug(previewTableLog) << "discarded an input method event raised by the reset";
    p_event->accept();
    return;
  }

  if (isReadOnly()) {
    // Qt does not enforce read-only on this path at all: a read-only QTextEdit
    // keeps Qt::TextSelectableByMouse, and QWidgetTextControl accepts an input
    // method event for a selectable control. Stripping the commit is not
    // enough either - installing a *changed* preedit removes the current
    // selection first, so even a commit-free event would delete cell text. So
    // the whole event is refused, and a composition which was still open when
    // the sheet became a viewer is cancelled by the reset in
    // TablePreviewWidget::applyEditability().
    qCDebug(previewTableLog) << "refused an input method event on a read-only sheet";
    p_event->accept();
    return;
  }

  // A Selection attribute is resolved by QWidgetTextControl in an intermediate
  // document state - after the current selection has been removed and after
  // the commit has been applied - which no pre-flight check can predict, and
  // it emits neither cursorPositionChanged() nor selectionChanged() when only
  // the anchor moves, so neither clamp would catch the result. Honoring one
  // which leaves the cell would hand a cross-frame selection to the next Cut
  // or Delete, and removing a selection across a frame boundary removes the
  // frame. So they are dropped rather than guessed at, and an ordinary
  // composition only needs the caret to follow the commit, which it does.
  bool droppedSelection = false;
  QList<QInputMethodEvent::Attribute> attributes;
  attributes.reserve(p_event->attributes().size());
  for (const auto &attribute : p_event->attributes()) {
    if (attribute.type == QInputMethodEvent::Selection) {
      droppedSelection = true;
      continue;
    }

    attributes.append(attribute);
  }

  // Forward everything except the commit, so the composition state stays
  // consistent while the document contents are left alone.
  auto forwardWithoutTheCommit = [this, p_event, &attributes]() {
    QInputMethodEvent filtered(p_event->preeditString(), attributes);
    QTextEdit::inputMethodEvent(&filtered);
    p_event->accept();
  };

  const QString commit = p_event->commitString();
  const int replacementLength = p_event->replacementLength();
  // An empty commit string with a replacement length is not a pure preedit -
  // that is how an input method deletes surrounding text.
  if (commit.isEmpty() && replacementLength == 0) {
    // A preedit is not in the document at all, so nothing else has to be
    // confined; normal handling has to be preserved so composition works.
    if (!droppedSelection) {
      QTextEdit::inputMethodEvent(p_event);
      return;
    }

    qCDebug(previewTableLog) << "dropped an input method selection";
    forwardWithoutTheCommit();
    return;
  }

  // Classify before touching the document. A payload which is nothing but
  // line separators is refused exactly as a paste is, and refusing it must not
  // delete what is selected - which is why this comes before the removal
  // below. The same separator policy otherwise: a cell holds one block.
  const QString sanitized =
      hasLineSeparator(commit)
          ? (hasCellContent(commit) ? sanitizeForCell(commit) : QString())
          : commit;
  if (sanitized.isEmpty() && replacementLength == 0) {
    qCDebug(previewTableLog) << "refused an input method commit which is nothing but line"
                             << "separators";
    forwardWithoutTheCommit();
    return;
  }

  if (sanitized != commit) {
    qCDebug(previewTableLog) << "sanitized a committed input method string";
  }

  // Everything below changes the document.
  clampCursorIntoTable();
  clampSelectionIntoOneCell();

  // Remove the - by now confined - selection here rather than letting the base
  // do it. QWidgetTextControl resolves replacementStart()/replacementLength()
  // against the cursor it is left with *after* that removal, so collapsing it
  // first is what makes the interval computed below the one it will really
  // use; clamping against the pre-removal caret would not bound anything.
  {
    QTextCursor selection = textCursor();
    if (selection.hasSelection()) {
      selection.removeSelectedText();
      setTextCursor(selection);
    }
  }

  QTextTable *table = m_document ? m_document->table() : nullptr;
  const QTextCursor cursor = textCursor();
  const QTextTableCell cell = table ? table->cellAt(cursor.position()) : QTextTableCell();
  if (!cell.isValid()) {
    // Nothing to anchor the replacement against, and letting it land outside
    // the table is exactly what must not happen.
    p_event->accept();
    return;
  }

  const int position = cursor.position();
  const int first = cell.firstPosition();
  const int last = cell.lastPosition();

  // The range the base will apply, now that the cursor is collapsed:
  // [position + replacementStart(), + replacementLength()). A negative start
  // or an overlong length would reach across the frame boundary.
  const int rawStart = position + p_event->replacementStart();
  const int start = qBound(first, rawStart, last);
  const int end = qBound(start, rawStart + replacementLength, last);

  QInputMethodEvent replacement(p_event->preeditString(), attributes);
  replacement.setCommitString(sanitized, start - position, end - start);
  QTextEdit::inputMethodEvent(&replacement);
  p_event->accept();
}

bool TablePreviewSheet::canInsertFromMimeData(const QMimeData *p_source) const {
  // Plain text only. A cell holds raw Markdown, so rich text would either be
  // flattened anyway or land in the document as structure a table row cannot
  // express.
  return p_source && p_source->hasText() && hasCellContent(p_source->text());
}

void TablePreviewSheet::insertFromMimeData(const QMimeData *p_source) {
  if (!p_source || isReadOnly()) {
    return;
  }

  // A drop puts the caret where it landed, which can be the block after the
  // table, and a paste replaces whatever is selected.
  clampCursorIntoTable();
  clampSelectionIntoOneCell();

  const QString text = p_source->text();
  if (!hasCellContent(text)) {
    qCDebug(previewTableLog) << "refused a payload which is nothing but line separators";
    return;
  }

  // Drops arrive here too, so one validator covers both.
  insertPlainText(sanitizeForCell(text));
}

// ---------------------------------------------------------------------------
// TablePreviewWidget
// ---------------------------------------------------------------------------

const qreal TablePreviewWidget::c_widthFraction = 1.0;

const int TablePreviewWidget::c_commitDebounceMs = 400;

TablePreviewWidget::TablePreviewWidget(PreviewWidgetContext *p_context, QWidget *p_parent)
    : PreviewWidget(p_context, p_parent), m_document(new TablePreviewDocument()) {
  auto layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  m_sheet = new TablePreviewSheet(this);
  m_sheet->setTableDocument(m_document.data());
  layout->addWidget(m_sheet);

  // The host only consults heightForWidth() when the policy advertises it, and
  // that is the only hook which sees the width the band actually gets.
  QSizePolicy policy = sizePolicy();
  policy.setHeightForWidth(true);
  setSizePolicy(policy);

  m_commitTimer = new QTimer(this);
  m_commitTimer->setSingleShot(true);
  m_commitTimer->setInterval(c_commitDebounceMs);
  connect(m_commitTimer, &QTimer::timeout, this, &TablePreviewWidget::handleCommitTimeout);

  connect(m_document->document(), &QTextDocument::contentsChanged, this,
          &TablePreviewWidget::handleContentsChanged);
  connect(m_sheet, &TablePreviewSheet::cellLeft, this, &TablePreviewWidget::handleCellLeft);
  connect(m_sheet, &TablePreviewSheet::focusLost, this, &TablePreviewWidget::handleFocusLost);
  connect(m_sheet, &TablePreviewSheet::focusEscapeRequested, this,
          &TablePreviewWidget::handleEscapeRequested);
  connect(m_sheet, &TablePreviewSheet::undoRequested, this,
          &TablePreviewWidget::handleUndoRequested);
  connect(m_sheet, &TablePreviewSheet::redoRequested, this,
          &TablePreviewWidget::handleRedoRequested);

  // The host's event filter watches this widget, not the sheet inside it, so
  // the sheet cannot reach it through updateGeometry() alone.
  connect(m_sheet, &TablePreviewSheet::preferredGeometryChanged, this,
          &TablePreviewWidget::handlePreferredGeometryChanged);

  if (p_context) {
    connect(p_context, &PreviewWidgetContext::replacementFinished, this,
            &TablePreviewWidget::handleReplacementFinished);
  }

  m_sheet->refreshFormats();
}

TablePreviewWidget::~TablePreviewWidget() {
  // The sheet renders the document m_document owns, and members are destroyed
  // before ~QObject tears the child widgets down. Drop the sheet here so it
  // can never lay out a document which is already gone.
  delete m_sheet;
  m_sheet = nullptr;
}

QVector<PreviewElementType> TablePreviewWidget::supportedTypes() const {
  return QVector<PreviewElementType>() << PreviewElementType::Table;
}

qreal TablePreviewWidget::preferredWidthFraction() const { return c_widthFraction; }

void TablePreviewWidget::clearSelection() {
  if (m_sheet) {
    m_sheet->clearSelection();
  }
}

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

  // A table too large to lay out interactively is left to the static source
  // rendering. A refused table never becomes an active item, so the factory
  // chain is re-walked on every parse generation: warning here would repeat
  // forever for a static, by-design condition.
  if (!TablePreviewDocument::isWithinLimits(*table)) {
    qCDebug(previewTableLog)
        << "refused an oversized table -" << table->cells().size() << "row(s) x"
        << TablePreviewDocument::normalizedColumnCount(*table) << "column(s) ="
        << TablePreviewDocument::normalizedCellCount(*table) << "cells; limits are"
        << TablePreviewDocument::c_maxCells << "cells and"
        << TablePreviewDocument::c_maxColumns << "columns";
    return false;
  }

  // Nothing authoritative changed: keep the live document, the caret and any
  // selection. Three things count as unchanged, and the third one is what
  // makes debounced editing safe:
  //
  //  - the same source as the snapshot currently bound,
  //  - what the document would serialize to right now,
  //  - the last Markdown this sheet successfully committed.
  //
  // Without the last one, committing A and then typing B before A's parse
  // echo arrives would rebuild the document from A and destroy both B and the
  // caret, because the incoming snapshot matches neither of the first two.
  if (m_table && m_authoritative) {
    const QString incoming = table->sourceMarkdown();
    bool unchanged = m_table->sourceMarkdown() == incoming;
    const char *reason = "identical source";

    if (!unchanged) {
      const QString current = m_document->toMarkdown();
      unchanged = !current.isEmpty() && current == incoming;
      reason = "the sheet's own contents";
    }

    if (!unchanged && !m_committedMarkdown.isEmpty() && m_committedMarkdown == incoming) {
      unchanged = true;
      reason = "echo of this sheet's own commit";
    }

    if (unchanged) {
      qCDebug(previewTableLog) << "bound an unchanged snapshot - kept the document and the caret"
                               << "(" << reason << ")";
      m_table = table;

      // The document may already hold edits newer than the commit this
      // snapshot echoes. They are still owed a write-back, so re-arm rather
      // than let them be stranded by a debounce which has already fired.
      if (m_editGeneration != m_committedGeneration) {
        armCommit();
      }

      return true;
    }
  }

  qCDebug(previewTableLog) << "rebuilding the sheet from" << table->rowCount() << "row(s) x"
                           << table->columnCount() << "declared column(s), source"
                           << table->sourceMarkdown().left(60);

  m_table = table;
  // The next snapshot is what a rejected sheet waits for.
  m_authoritative = true;
  resetFromSource();
  return true;
}

void TablePreviewWidget::setReadOnly(bool p_readOnly) {
  if (m_readOnly == p_readOnly) {
    return;
  }

  m_readOnly = p_readOnly;
  applyEditability();
}

void TablePreviewWidget::applyEditability() {
  if (!m_sheet) {
    return;
  }

  // Present the sheet as a viewer instead of silently swallowing edits which
  // the host would reject, or which could not be written back without changing
  // what the table renders to. Read-only still allows the caret, selection and
  // copy, which the retired NoEditTriggers did not.
  const bool roundTrippable = m_document->isRoundTrippable();
  const bool editable = !m_readOnly && roundTrippable && m_authoritative && !m_suppressed;
  qCDebug(previewTableLog) << "sheet is" << (editable ? "editable" : "viewer only")
                           << "- read-only" << m_readOnly << "round-trippable" << roundTrippable
                           << "authoritative" << m_authoritative << "suppressed" << m_suppressed;

  const bool wasReadOnly = m_sheet->isReadOnly();

  if (!wasReadOnly && !editable) {
    // While the sheet can still take the event: a viewer refuses every input
    // method event outright, so a composition which is still open has to be
    // cancelled here rather than left rendered over it.
    m_sheet->cancelComposition();
  }

  m_sheet->setReadOnly(!editable);

  if (wasReadOnly && editable) {
    // Defence in depth on the way back to writable: nothing in a viewer can
    // exploit a caret or a selection which left its cell, so re-establish both
    // invariants before an edit can.
    m_sheet->clampCursorIntoTable();
    m_sheet->clampSelectionIntoOneCell();
  }
}

void TablePreviewWidget::rebindFromContext() {
  // The context is the authoritative binding: an accepted replacement rebases
  // it onto the text which is now in the document, while a cached snapshot
  // still describes the pre-commit source.
  auto context = previewContext();
  if (!context) {
    return;
  }

  const auto bound = context->preview();
  if (bound && bound->type() == PreviewElementType::Table) {
    m_table = bound.staticCast<const TablePreview>();
  }
}

void TablePreviewWidget::resetFromSource() {
  // Restoring from a stale cache would undo a commit the host has already
  // applied, and the sheet would then serialize the reverted matrix over the
  // user's accepted change.
  rebindFromContext();

  {
    QScopedValueRollback<bool> guard(m_applyingSource, true);
    m_document->setTable(m_table);
    if (m_sheet) {
      // Only the palette: build() has just written every per-cell format, and
      // repeating that pass is O(cells) of pure duplicate work.
      m_sheet->refreshPalette();
    }
  }

  if (m_commitTimer) {
    m_commitTimer->stop();
  }

  // The document is the bound source again, so nothing is owed and no
  // self-commit is outstanding. The baseline is the source's *canonical* form
  // rather than the source itself: that is what a flush compares against, so
  // an edit which puts the cell back where it started - or a re-typed
  // identical value - stops there instead of rewriting the document with a
  // semantically identical table and pushing an undo step for it.
  m_editGeneration = 0;
  m_committedGeneration = 0;
  m_inFlightGeneration = 0;
  m_commitInFlight = false;
  m_inFlightMarkdown.clear();
  m_committedMarkdown = m_document->toMarkdown();

  applyEditability();
  updateGeometry();

  qCDebug(previewTableLog) << "sheet reset to" << m_document->rowCount() << "x"
                           << m_document->columnCount();
}

void TablePreviewWidget::armCommit() {
  if (m_suppressed || !m_commitTimer) {
    return;
  }

  m_commitTimer->start();
}

void TablePreviewWidget::handleContentsChanged() {
  if (m_applyingSource || m_suppressed) {
    return;
  }

  if (!m_document->isIntact()) {
    // Something took the table out of the document. Every guard which should
    // have made that impossible is upstream of here; this is the last one, and
    // rebuilding is the only safe answer - serializing a document which no
    // longer has a table would write an empty replacement, and the sheet would
    // be left holding a dangling QTextTable.
    qCWarning(previewTableLog) << "the sheet's table was removed from its document -"
                               << "rebuilding from the source";
    resetFromSource();
    return;
  }

  ++m_editGeneration;
  // Restarts, so a burst of keystrokes writes back once.
  armCommit();
}

void TablePreviewWidget::handleCellLeft() {
  // Leaving a cell commits it immediately: the debounce exists to coalesce
  // keystrokes inside one cell, not to hold an edit the user has visibly
  // finished.
  flushPendingCommit();
}

void TablePreviewWidget::handleFocusLost() { flushPendingCommit(); }

void TablePreviewWidget::handleCommitTimeout() { flushPendingCommit(); }

TablePreviewWidget::FlushOutcome TablePreviewWidget::flushPendingCommit() {
  if (m_suppressed || m_applyingSource || !m_table) {
    if (m_commitTimer) {
      m_commitTimer->stop();
    }

    return FlushOutcome::Settled;
  }

  if (m_commitInFlight) {
    // Re-entered while a request is on the wire. Issuing a second one would
    // target an anchor the outer edit has already collapsed, and would deliver
    // two completions for one logical commit. The timer is deliberately left
    // alone: the in-flight completion re-arms it when a newer generation is
    // still owed.
    qCDebug(previewTableLog) << "a commit is already in flight - not issuing a second one";
    return FlushOutcome::Deferred;
  }

  if (m_sheet) {
    // Before the generation check, never after it. A preedit is not in the
    // document yet, so it has not advanced the generation either: testing
    // first would report a composing sheet as clean, and the host's
    // pre-removal flush would then revoke authority and lose the composition.
    // Committing it is itself a document change, so the generation below is
    // read afterwards.
    m_sheet->commitPreedit();
  }

  // After the commit, which re-arms the debounce through contentsChanged.
  if (m_commitTimer) {
    m_commitTimer->stop();
  }

  if (m_editGeneration == m_committedGeneration) {
    // Nothing new since the last accepted commit.
    return FlushOutcome::Settled;
  }

  const QString markdown = m_document->toMarkdown();
  if (markdown.isEmpty()) {
    // Unsafe to rewrite: restore the source view.
    qCWarning(previewTableLog) << "the sheet was edited but cannot be serialized safely -"
                               << "restoring the source";
    resetFromSource();
    return FlushOutcome::Rejected;
  }

  auto context = previewContext();
  if (!context) {
    qCDebug(previewTableLog) << "the sheet was edited but has no context to write through";
    return FlushOutcome::Settled;
  }

  const quint64 generation = m_editGeneration;
  if (markdown == m_committedMarkdown) {
    // The document already holds what was last written - the edits cancelled
    // out, or a cell was re-typed with the value it had. Nothing to send, and
    // the generation is settled.
    m_committedGeneration = generation;
    return FlushOutcome::Settled;
  }

  m_inFlightMarkdown = markdown;
  m_inFlightGeneration = generation;
  m_commitInFlight = true;
  // Overwritten synchronously by the completion below. Only a request which
  // never reaches one - the host has no live identity for this sheet at all -
  // leaves this value standing, and that is a rejection.
  m_lastFlushOutcome = FlushOutcome::Rejected;

  qCDebug(previewTableLog) << "committing generation" << generation << "->" << markdown.left(80);
  context->requestSourceReplacement(markdown);

  // The host answers synchronously, so by now handleReplacementFinished() has
  // already run and recorded the outcome.
  if (m_committedGeneration >= generation) {
    return FlushOutcome::Settled;
  }

  return m_lastFlushOutcome;
}

void TablePreviewWidget::handleReplacementFinished(const vte::PreviewReplacementResult &p_result) {
  const bool wasInFlight = m_commitInFlight;
  m_commitInFlight = false;

  if (p_result.isAccepted()) {
    m_lastFlushOutcome = FlushOutcome::Settled;
    qCDebug(previewTableLog) << "the commit was accepted";
    m_authoritative = true;

    // The host has just rebased the context onto the text which is now in the
    // document. Leaving the cached snapshot describing the pre-commit source
    // would make setPreview()'s first test - "the incoming source equals the
    // bound one" - match an authoritative *revert* back to that very source,
    // which is precisely what the undo relay produces: the sheet would keep
    // rendering the committed table over a source which no longer holds it.
    rebindFromContext();

    if (wasInFlight) {
      m_committedMarkdown = m_inFlightMarkdown;
      // Deliberately the in-flight generation, never the current one: the user
      // may have typed again while this was on the wire, and that edit is
      // still owed a write-back of its own.
      m_committedGeneration = m_inFlightGeneration;
    }

    applyEditability();

    if (m_editGeneration != m_committedGeneration) {
      armCommit();
    }

    return;
  }

  switch (p_result.status()) {
  case PreviewReplacementResult::Deferred:
    // Nothing was touched, so nothing may be given up: this sheet is still
    // authoritative, still holds the edit and still owes the write-back. Only
    // the debounce is re-armed, which is what retries it.
    m_lastFlushOutcome = FlushOutcome::Deferred;
    qCDebug(previewTableLog) << "the commit was postponed - retrying after the debounce";
    armCommit();
    return;

  case PreviewReplacementResult::StaleSnapshot:
  case PreviewReplacementResult::SourceMismatch:
  case PreviewReplacementResult::InvalidRange:
  case PreviewReplacementResult::UnknownIdentity:
    // External source always wins and this snapshot no longer describes the
    // document. Stop presenting it as the truth and wait for the authoritative
    // snapshot instead of restoring stale values.
    // The host already warned about the rejection itself.
    m_lastFlushOutcome = FlushOutcome::Rejected;
    qCDebug(previewTableLog) << "this sheet is no longer authoritative - read-only until the"
                             << "next snapshot";
    m_authoritative = false;
    applyEditability();
    break;

  default:
    // The document was not touched, so the bound snapshot is still the source
    // of truth: discard the edit.
    m_lastFlushOutcome = FlushOutcome::Rejected;
    qCDebug(previewTableLog) << "the document was not touched - discarding the edit";
    resetFromSource();
    break;
  }
}

void TablePreviewWidget::handleEscapeRequested(vte::FocusEscapeDirection p_direction) {
  flushPendingCommit();
  emit focusEscapeRequested(p_direction);
}

void TablePreviewWidget::handleUndoRequested() {
  // Forwarding straight through would undo an unrelated earlier operation
  // whenever the debounce has not fired yet, and the pending table edit would
  // then still be committed on top of whatever the undo restored.
  if (m_editGeneration != m_committedGeneration) {
    if (flushPendingCommit() != FlushOutcome::Settled) {
      qCDebug(previewTableLog) << "the pending edit could not be committed - not undoing";
      return;
    }
  }

  emit undoRequested();
}

void TablePreviewWidget::handleRedoRequested() {
  if (m_editGeneration != m_committedGeneration) {
    // Committing a new edit necessarily clears the editor's redo stack, so
    // there is nothing left to redo afterwards. Drop the redo rather than
    // apply it to a stack the flush has just invalidated.
    qCDebug(previewTableLog) << "flushing a pending edit - the redo is dropped";
    flushPendingCommit();
    return;
  }

  emit redoRequested();
}

TablePreviewWidget::FlushOutcome TablePreviewWidget::flushNow() {
  if (m_suppressed) {
    return FlushOutcome::Settled;
  }

  qCDebug(previewTableLog) << "flushing while this sheet is still authoritative";
  return flushPendingCommit();
}

void TablePreviewWidget::revokeAuthority() {
  if (m_suppressed) {
    return;
  }

  m_suppressed = true;
  if (m_commitTimer) {
    m_commitTimer->stop();
  }

  qCDebug(previewTableLog) << "this sheet's authority has been revoked";
  applyEditability();
}

QSize TablePreviewWidget::sizeHint() const {
  // Width 0: see TablePreviewSheet::sizeHint().
  return m_sheet ? m_sheet->sizeHint() : QSize(0, 0);
}

bool TablePreviewWidget::hasHeightForWidth() const { return true; }

int TablePreviewWidget::heightForWidth(int p_width) const {
  // The layout has no margins, so the sheet's outer width is this widget's.
  return m_sheet ? m_sheet->heightForWidth(p_width) : 0;
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
  switch (p_event->type()) {
  case QEvent::FontChange:
  case QEvent::PaletteChange:
  case QEvent::StyleChange:
    if (m_sheet) {
      // Qt's style sheet machinery does not hand an application's editor font
      // down to a widget nested inside a styled ancestor, so the sheet would
      // keep measuring itself with the application default while being painted
      // with the inherited font. Forward it explicitly.
      if (p_event->type() == QEvent::FontChange && m_sheet->font() != font()) {
        m_sheet->setFont(font());
      }

      {
        // Formats only, never the cell text: rebuilding would destroy the
        // caret, which for a theme switch during typing is a silent loss of
        // the user's place. The guard keeps the format writes from being
        // mistaken for user edits.
        QScopedValueRollback<bool> guard(m_applyingSource, true);
        m_sheet->refreshFormats();
      }

      updateGeometry();
    }
    break;

  default:
    break;
  }

  PreviewWidget::changeEvent(p_event);
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
  widget->setReadOnly(m_readOnly);

  // Relay the sheet's requests upwards with the widget attached: only the host
  // can resolve which identity - and therefore which live anchor - they belong
  // to.
  connect(widget, &TablePreviewWidget::focusEscapeRequested, this,
          [this, widget](FocusEscapeDirection p_direction) {
            emit focusEscapeRequested(widget, p_direction);
          });
  connect(widget, &TablePreviewWidget::undoRequested, this,
          [this, widget]() { emit undoRequested(widget); });
  connect(widget, &TablePreviewWidget::redoRequested, this,
          [this, widget]() { emit redoRequested(widget); });

  // Every host rebuild destroys the previous sheets. setReadOnly() does
  // compact the list, but it early-returns when the value is unchanged, which
  // is the steady state - so without this the vector would grow for the whole
  // lifetime of the editor.
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
