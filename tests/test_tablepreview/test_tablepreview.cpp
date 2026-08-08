#include "test_tablepreview.h"

#include <QAbstractItemDelegate>
#include <QFontDatabase>
#include <QImage>
#include <QLineEdit>
#include <QPainter>
#include <QProxyStyle>
#include <QScrollBar>
#include <QSignalSpy>
#include <QStringList>
#include <QStyle>
#include <QStyleOption>
#include <QStyleOptionViewItem>

#include <vtextedit/preview.h>

#include "previewbuilder.h"
#include "tablepreviewwidget.h"

#include <QHeaderView>
#include <QLayout>

using namespace tests;
using namespace vte;

namespace {
QSharedPointer<const TablePreview>
makeTable(const QVector<QVector<QString>> &p_cells,
          const QVector<PreviewTableAlignment> &p_alignments,
          const QVector<QString> &p_rowPrefixes = QVector<QString>(),
          const QString &p_delimiterPrefix = QString()) {
  QVector<QString> prefixes = p_rowPrefixes;
  while (prefixes.size() < p_cells.size()) {
    prefixes.append(QString());
  }

  auto preview = PreviewBuilder::createTable(1, 0, 10, QStringLiteral("source"),
                                             p_alignments.size(), p_cells, p_alignments, prefixes,
                                             p_delimiterPrefix);
  return preview.staticCast<const TablePreview>();
}
} // namespace

// ---------------------------------------------------------------------------
// Serializer
// ---------------------------------------------------------------------------

void TestTablePreview::testEscapeCellParity() {
  // Unescaped pipe gets a backslash.
  QCOMPARE(TablePreviewSerializer::escapeCell(QStringLiteral("a|b")), QStringLiteral("a\\|b"));
  // Already escaped pipe is left alone.
  QCOMPARE(TablePreviewSerializer::escapeCell(QStringLiteral("a\\|b")), QStringLiteral("a\\|b"));
  // An even number of backslashes means the pipe is structural.
  QCOMPARE(TablePreviewSerializer::escapeCell(QStringLiteral("a\\\\|b")),
           QStringLiteral("a\\\\\\|b"));
  // Nothing to do.
  QCOMPARE(TablePreviewSerializer::escapeCell(QStringLiteral("plain")), QStringLiteral("plain"));
}

void TestTablePreview::testEscapeCellIdempotent() {
  const QStringList inputs{QStringLiteral("a|b"), QStringLiteral("a\\|b"),
                           QStringLiteral("|"),   QStringLiteral("\\\\|"),
                           QStringLiteral("a||b")};
  for (const auto &input : inputs) {
    const QString once = TablePreviewSerializer::escapeCell(input);
    QCOMPARE(TablePreviewSerializer::escapeCell(once), once);
  }
}

void TestTablePreview::testSerializeCanonical() {
  QVector<QVector<QString>> cells;
  cells.append({QStringLiteral("h1"), QStringLiteral("h2")});
  cells.append({QStringLiteral("a"), QStringLiteral("b")});

  const QVector<PreviewTableAlignment> alignments{PreviewTableAlignment::None,
                                                  PreviewTableAlignment::None};
  const QVector<QString> prefixes{QString(), QString()};

  const QString markdown =
      TablePreviewSerializer::serialize(cells, alignments, prefixes, QString());
  QCOMPARE(markdown, QStringLiteral("| h1  | h2  |\n| --- | --- |\n| a   | b   |"));
}

void TestTablePreview::testSerializeAlignments() {
  QVector<QVector<QString>> cells;
  cells.append({QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c"),
                QStringLiteral("d")});
  cells.append({QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3"),
                QStringLiteral("4")});

  const QVector<PreviewTableAlignment> alignments{
      PreviewTableAlignment::None, PreviewTableAlignment::Left, PreviewTableAlignment::Center,
      PreviewTableAlignment::Right};
  const QVector<QString> prefixes{QString(), QString()};

  const QString markdown =
      TablePreviewSerializer::serialize(cells, alignments, prefixes, QString());
  const QStringList lines = markdown.split(QLatin1Char('\n'));
  QCOMPARE(lines.size(), 3);
  QCOMPARE(lines[1], QStringLiteral("| --- | :--- | :---: | ---: |"));
  // Every column keeps at least three dashes.
  QCOMPARE(lines[0], QStringLiteral("| a   | b    | c     | d    |"));
}

void TestTablePreview::testSerializeRaggedRows() {
  QVector<QVector<QString>> cells;
  // Header narrower than a body row: the canonical form expands, never drops.
  cells.append({QStringLiteral("a")});
  cells.append({QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("z")});
  cells.append({QStringLiteral("only")});

  const QVector<PreviewTableAlignment> alignments{PreviewTableAlignment::None};
  const QVector<QString> prefixes{QString(), QString(), QString()};

  const QString markdown =
      TablePreviewSerializer::serialize(cells, alignments, prefixes, QString());
  const QStringList lines = markdown.split(QLatin1Char('\n'));
  QCOMPARE(lines.size(), 4);
  QCOMPARE(lines[0], QStringLiteral("| a    |     |     |"));
  QCOMPARE(lines[1], QStringLiteral("| ---- | --- | --- |"));
  QCOMPARE(lines[2], QStringLiteral("| x    | y   | z   |"));
  QCOMPARE(lines[3], QStringLiteral("| only |     |     |"));
}

void TestTablePreview::testSerializePreservesPrefixes() {
  QVector<QVector<QString>> cells;
  cells.append({QStringLiteral("a"), QStringLiteral("b")});
  cells.append({QStringLiteral("c"), QStringLiteral("d")});

  const QVector<PreviewTableAlignment> alignments{PreviewTableAlignment::None,
                                                  PreviewTableAlignment::None};

  // Block quote: every row shares the same prefix.
  {
    const QVector<QString> prefixes{QStringLiteral("> "), QStringLiteral("> ")};
    const QString markdown =
        TablePreviewSerializer::serialize(cells, alignments, prefixes, QStringLiteral("> "));
    for (const auto &line : markdown.split(QLatin1Char('\n'))) {
      QVERIFY(line.startsWith(QStringLiteral("> |")));
    }
  }

  // Nested list: the first row keeps the marker, the rest keep the indent.
  {
    const QVector<QString> prefixes{QStringLiteral("- "), QStringLiteral("  ")};
    const QString markdown =
        TablePreviewSerializer::serialize(cells, alignments, prefixes, QStringLiteral("  "));
    const QStringList lines = markdown.split(QLatin1Char('\n'));
    QCOMPARE(lines.size(), 3);
    QVERIFY(lines[0].startsWith(QStringLiteral("- |")));
    QVERIFY(lines[1].startsWith(QStringLiteral("  |")));
    QVERIFY(lines[2].startsWith(QStringLiteral("  |")));
  }
}

void TestTablePreview::testSerializeRejectsUnsafePrefixes() {
  QVector<QVector<QString>> cells;
  cells.append({QStringLiteral("a")});
  cells.append({QStringLiteral("b")});
  cells.append({QStringLiteral("c")});

  const QVector<PreviewTableAlignment> alignments{PreviewTableAlignment::None};

  // Heterogeneous continuation prefixes would corrupt the nesting.
  {
    const QVector<QString> prefixes{QString(), QStringLiteral("  "), QStringLiteral("    ")};
    QVERIFY(TablePreviewSerializer::serialize(cells, alignments, prefixes, QStringLiteral("  "))
                .isEmpty());
  }

  // A list marker repeated on continuation rows would create new list items.
  {
    const QVector<QString> prefixes{QStringLiteral("- "), QStringLiteral("- "),
                                    QStringLiteral("- ")};
    QVERIFY(TablePreviewSerializer::serialize(cells, alignments, prefixes, QStringLiteral("- "))
                .isEmpty());
  }
}

void TestTablePreview::testSerializeRejectsLineSeparators() {
  QVector<QVector<QString>> cells;
  cells.append({QStringLiteral("a")});
  cells.append({QStringLiteral("b\nc")});

  const QVector<PreviewTableAlignment> alignments{PreviewTableAlignment::None};
  const QVector<QString> prefixes{QString(), QString()};
  QVERIFY(TablePreviewSerializer::serialize(cells, alignments, prefixes, QString()).isEmpty());

  cells[1][0] = QString(QChar(0x2029));
  QVERIFY(TablePreviewSerializer::serialize(cells, alignments, prefixes, QString()).isEmpty());
}

void TestTablePreview::testSerializePreservesInlineMarkdown() {
  QVector<QVector<QString>> cells;
  cells.append({QStringLiteral("**bold**"), QStringLiteral("[l](u.md)")});
  cells.append({QStringLiteral("`code`"), QStringLiteral("a \\| b")});

  const QVector<PreviewTableAlignment> alignments{PreviewTableAlignment::None,
                                                  PreviewTableAlignment::None};
  const QVector<QString> prefixes{QString(), QString()};

  const QString markdown =
      TablePreviewSerializer::serialize(cells, alignments, prefixes, QString());
  QVERIFY(markdown.contains(QStringLiteral("**bold**")));
  QVERIFY(markdown.contains(QStringLiteral("[l](u.md)")));
  QVERIFY(markdown.contains(QStringLiteral("`code`")));
  // The already escaped pipe is not double escaped.
  QVERIFY(markdown.contains(QStringLiteral("a \\| b")));
  QVERIFY(!markdown.contains(QStringLiteral("a \\\\| b")));
}

void TestTablePreview::testSerializeCapsPadding() {
  // The size limits bound the cell *count*, never the cell text length, so one
  // very wide cell would otherwise pad every other row up to its width and
  // amplify the output by the row count.
  const int rows = 50;
  const QString wide(500, QLatin1Char('w'));

  QVector<QVector<QString>> cells;
  cells.append({QStringLiteral("h"), QStringLiteral("h2")});
  cells.append({wide, QStringLiteral("x")});
  for (int i = 2; i < rows; ++i) {
    cells.append({QStringLiteral("a"), QStringLiteral("b")});
  }

  const QVector<PreviewTableAlignment> alignments{PreviewTableAlignment::None,
                                                  PreviewTableAlignment::None};
  QVector<QString> prefixes;
  for (int i = 0; i < rows; ++i) {
    prefixes.append(QString());
  }

  const QString markdown =
      TablePreviewSerializer::serialize(cells, alignments, prefixes, QString());
  QVERIFY(!markdown.isEmpty());

  // leftJustified() only pads, so the wide cell is still emitted in full.
  QVERIFY(markdown.contains(wide));

  // Every other row is padded to the cap, not to the wide cell's width.
  const QStringList lines = markdown.split(QLatin1Char('\n'));
  QCOMPARE(lines.size(), rows + 1);
  for (int i = 0; i < lines.size(); ++i) {
    if (lines[i].contains(wide)) {
      continue;
    }

    QVERIFY2(lines[i].size() < 2 * TablePreviewModel::c_maxPaddedWidth,
             qPrintable(QStringLiteral("line %1 is %2 characters wide")
                            .arg(i)
                            .arg(lines[i].size())));
  }

  // Which bounds the whole output: without the cap it would be rows x 500.
  QVERIFY2(markdown.size() < wide.size() + rows * 3 * TablePreviewModel::c_maxPaddedWidth,
           qPrintable(QStringLiteral("serialized %1 characters").arg(markdown.size())));
}

// ---------------------------------------------------------------------------
// Model
// ---------------------------------------------------------------------------

void TestTablePreview::testModelNormalization() {
  QVector<QVector<QString>> cells;
  cells.append({QStringLiteral("a"), QStringLiteral("b")});
  cells.append({QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("z")});
  cells.append({QStringLiteral("only")});

  TablePreviewModel model;
  model.setTable(makeTable(cells, {PreviewTableAlignment::None, PreviewTableAlignment::None}));

  QCOMPARE(model.rowCount(), 3);
  // The model width is the maximum of header, alignment and every body row.
  QCOMPARE(model.columnCount(), 3);
  QCOMPARE(model.data(model.index(0, 2)).toString(), QString());
  QCOMPARE(model.data(model.index(1, 2)).toString(), QStringLiteral("z"));
  QCOMPARE(model.data(model.index(2, 0)).toString(), QStringLiteral("only"));
}

void TestTablePreview::testModelNoOpCommit() {
  QVector<QVector<QString>> cells;
  cells.append({QStringLiteral("a")});
  cells.append({QStringLiteral("b")});

  TablePreviewModel model;
  model.setTable(makeTable(cells, {PreviewTableAlignment::None}));

  QSignalSpy spy(&model, &TablePreviewModel::cellCommitted);
  QVERIFY(!model.setData(model.index(1, 0), QStringLiteral("b")));
  QCOMPARE(spy.count(), 0);
}

void TestTablePreview::testModelCommitEmitsOnRealChange() {
  QVector<QVector<QString>> cells;
  cells.append({QStringLiteral("a")});
  cells.append({QStringLiteral("b")});

  TablePreviewModel model;
  model.setTable(makeTable(cells, {PreviewTableAlignment::None}));

  QSignalSpy spy(&model, &TablePreviewModel::cellCommitted);
  QVERIFY(model.setData(model.index(1, 0), QStringLiteral("changed")));
  QCOMPARE(spy.count(), 1);
  QCOMPARE(model.data(model.index(1, 0)).toString(), QStringLiteral("changed"));
}

void TestTablePreview::testModelAlignmentRole() {
  QVector<QVector<QString>> cells;
  cells.append({QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")});

  TablePreviewModel model;
  model.setTable(makeTable(cells, {PreviewTableAlignment::Left, PreviewTableAlignment::Center,
                                   PreviewTableAlignment::Right}));

  QCOMPARE(model.data(model.index(0, 0), Qt::TextAlignmentRole).toInt(),
           static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter));
  QCOMPARE(model.data(model.index(0, 1), Qt::TextAlignmentRole).toInt(),
           static_cast<int>(Qt::AlignCenter));
  QCOMPARE(model.data(model.index(0, 2), Qt::TextAlignmentRole).toInt(),
           static_cast<int>(Qt::AlignRight | Qt::AlignVCenter));
  // Row 0 is styled as the header.
  QVERIFY(model.data(model.index(0, 0), Qt::FontRole).value<QFont>().bold());
}

void TestTablePreview::testModelRoundTrip() {
  QVector<QVector<QString>> cells;
  cells.append({QStringLiteral("h1"), QStringLiteral("h2")});
  cells.append({QStringLiteral("a"), QStringLiteral("b")});

  TablePreviewModel model;
  model.setTable(makeTable(cells, {PreviewTableAlignment::None, PreviewTableAlignment::Right}));

  // The right aligned column is at least four characters wide (":" + "---").
  QCOMPARE(model.toMarkdown(),
           QStringLiteral("| h1  | h2   |\n| --- | ---: |\n| a   | b    |"));

  QVERIFY(model.setData(model.index(1, 1), QStringLiteral("wider value")));
  QCOMPARE(model.toMarkdown(), QStringLiteral("| h1  | h2          |\n"
                                              "| --- | ----------: |\n"
                                              "| a   | wider value |"));
}

// ---------------------------------------------------------------------------
// Widget
// ---------------------------------------------------------------------------

void TestTablePreview::testWidgetRejectsNonTable() {
  TablePreviewWidget widget(nullptr, nullptr);
  QCOMPARE(widget.supportedTypes(), QVector<PreviewElementType>{PreviewElementType::Table});

  auto code = PreviewBuilder::createCode(1, 0, 3, QStringLiteral("```\n```"), QString(),
                                         QString());
  QVERIFY(!widget.setPreview(code));
  QVERIFY(!widget.setPreview(QSharedPointer<const Preview>()));
}

void TestTablePreview::testWidgetVisibleRows() {
  QVector<QVector<QString>> cells;
  for (int i = 0; i < 20; ++i) {
    cells.append({QStringLiteral("row %1").arg(i)});
  }

  TablePreviewWidget widget(nullptr, nullptr);
  QVERIFY(widget.setPreview(makeTable(cells, {PreviewTableAlignment::None})));

  widget.setVisibleRows(20);
  const int tallHint = widget.sizeHint().height();
  widget.setVisibleRows(3);
  const int shortHint = widget.sizeHint().height();
  QVERIFY(shortHint < tallHint);

  // Consumed as at least one row.
  widget.setVisibleRows(0);
  QVERIFY(widget.sizeHint().height() > 0);
  QVERIFY(widget.sizeHint().height() <= shortHint);
}

void TestTablePreview::testLargeTableFitsVisibleRows() {
  // Fitting every row of a table this size costs hundreds of thousands of
  // delegate size hints, once per parse generation, for rows the sheet never
  // shows. Only the visible window is fitted eagerly - the reserved geometry
  // has to stay correct anyway, and the rest is fitted lazily when it is
  // actually scrolled to.
  QVector<QVector<QString>> cells;
  cells.append({QStringLiteral("header")});
  for (int i = 1; i < 800; ++i) {
    cells.append({QStringLiteral("row %1").arg(i)});
  }

  TablePreviewWidget widget(nullptr, nullptr);
  widget.setVisibleRows(5);
  QVERIFY(widget.setPreview(makeTable(cells, {PreviewTableAlignment::None})));

  auto view = widget.findChild<TablePreviewView *>();
  QVERIFY(view);
  QCOMPARE(view->model()->rowCount(), 800);

  // Every row of the window is fitted to its contents.
  for (int r = 0; r < 5; ++r) {
    QVERIFY2(view->rowHeight(r) > 0,
             qPrintable(QStringLiteral("row %1 has no height").arg(r)));
  }

  // And the sheet still reserves exactly that window plus the frame and the
  // scroll bar it needs for the rest.
  int windowHeight = 0;
  for (int r = 0; r < 5; ++r) {
    windowHeight += view->rowHeight(r);
  }

  const int hint = widget.sizeHint().height();
  QCOMPARE(hint, 2 * view->frameWidth() + windowHeight);

  // Nothing beyond the window was walked: a model-wide fit is exactly the
  // cost this window exists to avoid.
  const int defaultHeight = view->verticalHeader()->defaultSectionSize();
  QCOMPARE(view->rowHeight(400), defaultHeight);
  QCOMPARE(view->rowHeight(799), defaultHeight);
}

void TestTablePreview::testEscapedPipeDoesNotWidenColumn() {
  QVector<QVector<QString>> cells;
  cells.append({QStringLiteral("ab")});
  // Escaping adds a backslash to the emitted text, but the readable column
  // width is derived from the raw cell so the padding stays predictable.
  cells.append({QStringLiteral("a|b")});

  const QVector<PreviewTableAlignment> alignments{PreviewTableAlignment::None};
  const QVector<QString> prefixes{QString(), QString()};

  const QString markdown =
      TablePreviewSerializer::serialize(cells, alignments, prefixes, QString());
  const QStringList lines = markdown.split(QLatin1Char('\n'));
  QCOMPARE(lines.size(), 3);
  QCOMPARE(lines[0], QStringLiteral("| ab  |"));
  QCOMPARE(lines[1], QStringLiteral("| --- |"));
  QCOMPARE(lines[2], QStringLiteral("| a\\|b |"));

  // Round trip: the escaped pipe still parses back as one cell.
  QCOMPARE(TablePreviewSerializer::escapeCell(QStringLiteral("a|b")),
           QStringLiteral("a\\|b"));
}

void TestTablePreview::testWidgetRejectsOversizedTable() {
  // A single very wide row would otherwise inflate every other row when the
  // sheet is normalized, making the matrix quadratic in the document size.
  const int rows = 900;
  const int wideColumns = 900;

  QVector<QVector<QString>> cells;
  cells.append({QStringLiteral("h")});
  for (int i = 1; i < rows; ++i) {
    cells.append({QStringLiteral("r")});
  }
  QVector<QString> wideRow;
  for (int c = 0; c < wideColumns; ++c) {
    wideRow.append(QStringLiteral("w"));
  }
  cells.append(wideRow);

  auto table = makeTable(cells, {PreviewTableAlignment::None});
  QVERIFY(TablePreviewModel::normalizedCellCount(*table) > TablePreviewModel::c_maxCells);

  TablePreviewWidget widget(nullptr, nullptr);
  QVERIFY(!widget.setPreview(table));

  // A table within the budget is still accepted.
  QVector<QVector<QString>> small;
  small.append({QStringLiteral("a"), QStringLiteral("b")});
  small.append({QStringLiteral("c"), QStringLiteral("d")});
  auto smallTable =
      makeTable(small, {PreviewTableAlignment::None, PreviewTableAlignment::None});
  QVERIFY(TablePreviewModel::normalizedCellCount(*smallTable) <= TablePreviewModel::c_maxCells);
  QVERIFY(widget.setPreview(smallTable));
}

void TestTablePreview::testWidgetRejectsTooManyRowsOrColumns() {
  // The cell product alone still admits a very tall or a very wide sheet,
  // whose per-edit walk over every section would block the GUI thread.
  QVector<QVector<QString>> tall;
  for (int i = 0; i < TablePreviewModel::c_maxRows + 1; ++i) {
    tall.append({QStringLiteral("r")});
  }

  auto tallTable = makeTable(tall, {PreviewTableAlignment::None});
  QVERIFY(TablePreviewModel::normalizedCellCount(*tallTable) <= TablePreviewModel::c_maxCells);
  QVERIFY(!TablePreviewModel::isWithinLimits(*tallTable));

  TablePreviewWidget widget(nullptr, nullptr);
  QVERIFY(!widget.setPreview(tallTable));

  QVector<QString> wideRow;
  QVector<PreviewTableAlignment> alignments;
  for (int c = 0; c < TablePreviewModel::c_maxColumns + 1; ++c) {
    wideRow.append(QStringLiteral("w"));
    alignments.append(PreviewTableAlignment::None);
  }

  QVector<QVector<QString>> wide;
  wide.append(wideRow);
  wide.append(wideRow);

  auto wideTable = makeTable(wide, alignments);
  QVERIFY(TablePreviewModel::normalizedCellCount(*wideTable) <= TablePreviewModel::c_maxCells);
  QVERIFY(!TablePreviewModel::isWithinLimits(*wideTable));
  QVERIFY(!widget.setPreview(wideTable));
}

void TestTablePreview::testRaggedTableIsNotRoundTrippable() {
  QVector<QVector<QString>> cells;
  cells.append({QStringLiteral("h1"), QStringLiteral("h2")});
  cells.append({QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")});

  TablePreviewModel model;
  model.setTable(makeTable(cells, {PreviewTableAlignment::None, PreviewTableAlignment::None}));

  // The excess cell stays visible, so nothing is hidden from the user.
  QCOMPARE(model.columnCount(), 3);

  // Writing it back would promote it into the header and the delimiter row,
  // turning a cell GFM ignores today into a real column.
  QVERIFY(!model.isRoundTrippable());
  QVERIFY(model.toMarkdown().isEmpty());

  QVector<QVector<QString>> even;
  even.append({QStringLiteral("h1"), QStringLiteral("h2")});
  even.append({QStringLiteral("a"), QStringLiteral("b")});

  TablePreviewModel evenModel;
  evenModel.setTable(
      makeTable(even, {PreviewTableAlignment::None, PreviewTableAlignment::None}));
  QVERIFY(evenModel.isRoundTrippable());
  QVERIFY(!evenModel.toMarkdown().isEmpty());
}

void TestTablePreview::testPreferredSizeCacheTracksContents() {
  QVector<QVector<QString>> cells;
  cells.append({QStringLiteral("h")});
  cells.append({QStringLiteral("a")});

  TablePreviewWidget widget(nullptr, nullptr);
  QVERIFY(widget.setPreview(makeTable(cells, {PreviewTableAlignment::None})));

  const int narrow = widget.sizeHint().width();
  QVERIFY(narrow > 0);
  // Repeated queries are answered from the cache rather than remeasured.
  QCOMPARE(widget.sizeHint().width(), narrow);

  auto view = widget.findChild<TablePreviewView *>();
  QVERIFY(view);
  QVERIFY(view->model()->setData(view->model()->index(1, 0),
                                 QStringLiteral("a considerably longer cell value"),
                                 Qt::EditRole));

  // Content changes have to invalidate it.
  QVERIFY(widget.sizeHint().width() > narrow);
}

namespace {
// A two column sheet whose columns are deliberately very unequal, so an
// "equalize" distribution is distinguishable from a proportional one.
QSharedPointer<const TablePreview> makeUnequalTable() {
  QVector<QVector<QString>> cells;
  cells.append({QStringLiteral("a rather long header"), QStringLiteral("b")});
  cells.append({QStringLiteral("a rather long value"), QStringLiteral("c")});
  return makeTable(cells, {PreviewTableAlignment::None, PreviewTableAlignment::None});
}

int totalColumnWidth(TablePreviewView *p_view) {
  int total = 0;
  for (int c = 0; c < p_view->model()->columnCount(); ++c) {
    total += p_view->columnWidth(c);
  }

  return total;
}

// Let the queued layout pass run. Every geometry answer is computed by the
// pure solver, but the live sections are only written by that pass.
void settleWidget(TablePreviewWidget &p_widget) {
  QCoreApplication::processEvents();
  QTest::qWait(30);
  QCoreApplication::processEvents();
}

void resizeAndSettle(TablePreviewWidget &p_widget, int p_width, int p_height) {
  if (!p_widget.isVisible()) {
    // A hidden top level only marks its resize event as pending, so the view
    // inside it would keep the geometry of whatever size came before.
    // Off-screen is enough: the layout and the resize events are real either
    // way, and no window is opened.
    p_widget.setAttribute(Qt::WA_DontShowOnScreen, true);
    p_widget.show();
  }

  p_widget.resize(p_width, p_height);
  settleWidget(p_widget);
}

// Resize the way the host does: the sheet is given a width, and exactly the
// height it reports for it.
void resizeToWidth(TablePreviewWidget &p_widget, int p_width) {
  resizeAndSettle(p_widget, p_width, p_widget.heightForWidth(p_width));
}

QVector<int> columnWidths(TablePreviewView *p_view) {
  QVector<int> widths;
  for (int c = 0; c < p_view->model()->columnCount(); ++c) {
    widths.append(p_view->columnWidth(c));
  }

  return widths;
}

int totalRowHeight(TablePreviewView *p_view, int p_rows) {
  int total = 0;
  for (int r = 0; r < p_rows; ++r) {
    total += p_view->rowHeight(r);
  }

  return total;
}
} // namespace

void TestTablePreview::testColumnsFillTheAssignedWidth() {
  TablePreviewWidget widget(nullptr, nullptr);
  QVERIFY(widget.setPreview(makeUnequalTable()));

  auto view = widget.findChild<TablePreviewView *>();
  QVERIFY(view);
  QCOMPARE(view->model()->columnCount(), 2);

  // At exactly its natural width every column gets its natural width, which
  // is the baseline the proportional share below is measured against.
  const QSize hint = widget.sizeHint();
  resizeToWidth(widget, hint.width());

  const QVector<int> baseline = columnWidths(view);
  int baselineTotal = 0;
  for (int width : baseline) {
    baselineTotal += width;
  }
  QVERIFY(baseline.first() > baseline.last());
  QCOMPARE(baselineTotal, view->viewport()->width());

  // Twice the natural width: the surplus is what the plan has to spread. No
  // editor is involved, so this holds for any assigned geometry.
  resizeToWidth(widget, hint.width() * 2);

  const int viewportWidth = view->viewport()->width();
  QVERIFY(viewportWidth > baselineTotal);
  QCOMPARE(totalColumnWidth(view), viewportWidth);

  for (int c = 0; c < baseline.size(); ++c) {
    const int expected = qRound(qreal(baseline.at(c)) * viewportWidth / baselineTotal);
    QVERIFY2(qAbs(view->columnWidth(c) - expected) <= 2,
             qPrintable(QStringLiteral("column %1 is %2, not its proportional share %3")
                            .arg(c)
                            .arg(view->columnWidth(c))
                            .arg(expected)));
  }
}

void TestTablePreview::testColumnLayoutFollowsACellEdit() {
  TablePreviewWidget widget(nullptr, nullptr);
  QVERIFY(widget.setPreview(makeUnequalTable()));

  auto view = widget.findChild<TablePreviewView *>();
  QVERIFY(view);

  const QSize hint = widget.sizeHint();
  resizeToWidth(widget, hint.width() * 2);

  const int shortBefore = view->columnWidth(1);

  // A committed cell edit emits only dataChanged: the snapshot echoing it takes
  // the "unchanged" path, so nothing rebuilds the sheet. The columns still have
  // to be re-measured and re-planned, or a longer value stays wrapped inside a
  // column sized for the value it replaced.
  QVERIFY(view->model()->setData(view->model()->index(1, 1),
                                 QStringLiteral("a considerably longer cell value"),
                                 Qt::EditRole));
  settleWidget(widget);

  QVERIFY2(view->columnWidth(1) > shortBefore,
           qPrintable(QStringLiteral("the edited column stayed at %1 (was %2)")
                          .arg(view->columnWidth(1))
                          .arg(shortBefore)));
  QCOMPARE(totalColumnWidth(view), view->viewport()->width());
}

// ---------------------------------------------------------------------------
// Delegate
// ---------------------------------------------------------------------------

namespace {
// A sheet with one column and one very wrappable body cell.
TablePreviewView *buildSheet(TablePreviewWidget &p_widget,
                             const QVector<QVector<QString>> &p_cells,
                             const QVector<PreviewTableAlignment> &p_alignments) {
  if (!p_widget.setPreview(makeTable(p_cells, p_alignments))) {
    return nullptr;
  }

  return p_widget.findChild<TablePreviewView *>();
}

// The style option a cell is measured and painted with. A width of 0 asks the
// delegate for the natural, unwrapped size.
QStyleOptionViewItem cellOption(TablePreviewView *p_view, int p_width, int p_height = 0) {
  QStyleOptionViewItem option;
  option.initFrom(p_view);
  option.widget = p_view;
  option.font = p_view->font();
  option.fontMetrics = QFontMetrics(option.font);
  option.rect = QRect(0, 0, p_width, p_height);
  option.state |= QStyle::State_Enabled;
  return option;
}

QSize cellHint(TablePreviewView *p_view, int p_row, int p_column, int p_width) {
  const QModelIndex index = p_view->model()->index(p_row, p_column);
  return p_view->itemDelegate()->sizeHint(cellOption(p_view, p_width), index);
}



// What the horizontal bar really took, read off the live geometry rather than
// recomputed from the style metrics the sheet itself uses.
int horizontalBarReserve(TablePreviewView *p_view) {
  return p_view->height() - 2 * p_view->frameWidth() - p_view->viewport()->height();
}

// The bounding box of everything which is not the background.
QRect inkBounds(const QImage &p_image) {
  QRect bounds;
  for (int y = 0; y < p_image.height(); ++y) {
    for (int x = 0; x < p_image.width(); ++x) {
      if (p_image.pixel(x, y) != qRgb(255, 255, 255)) {
        bounds = bounds.isNull() ? QRect(x, y, 1, 1) : bounds.united(QRect(x, y, 1, 1));
      }
    }
  }

  return bounds;
}

int inkCount(const QImage &p_image) {
  int count = 0;
  for (int y = 0; y < p_image.height(); ++y) {
    for (int x = 0; x < p_image.width(); ++x) {
      if (p_image.pixel(x, y) != qRgb(255, 255, 255)) {
        ++count;
      }
    }
  }

  return count;
}

// Records the order in which the delegate reaches the style, and how much has
// already been painted at each step. Can also suppress the native panel, so a
// test of what the delegate itself paints is not confounded by whatever the
// platform style fills the cell with.
class RecordingStyle : public QProxyStyle {
public:
  void drawPrimitive(PrimitiveElement p_element, const QStyleOption *p_option,
                     QPainter *p_painter, const QWidget *p_widget) const Q_DECL_OVERRIDE {
    if (p_element == PE_PanelItemViewItem) {
      m_calls.append(QStringLiteral("panel"));
      m_inkAtPanel = m_image ? inkCount(*m_image) : -1;
      if (m_suppressPanel) {
        return;
      }
    } else if (p_element == PE_FrameFocusRect) {
      m_calls.append(QStringLiteral("focus"));
      m_inkAtFocus = m_image ? inkCount(*m_image) : -1;
    }

    QProxyStyle::drawPrimitive(p_element, p_option, p_painter, p_widget);
  }

  const QImage *m_image = nullptr;

  bool m_suppressPanel = false;

  mutable QStringList m_calls;

  mutable int m_inkAtPanel = -1;

  mutable int m_inkAtFocus = -1;
};

// The summed channels of everything which is not the background, which
// survives antialiasing where an exact colour match would not.
struct InkChannels {
  qint64 m_red = 0;

  qint64 m_green = 0;

  qint64 m_blue = 0;
};

InkChannels inkChannels(const QImage &p_image) {
  InkChannels channels;
  for (int y = 0; y < p_image.height(); ++y) {
    for (int x = 0; x < p_image.width(); ++x) {
      const QRgb pixel = p_image.pixel(x, y);
      if (pixel == qRgb(255, 255, 255)) {
        continue;
      }

      channels.m_red += qRed(pixel);
      channels.m_green += qGreen(pixel);
      channels.m_blue += qBlue(pixel);
    }
  }

  return channels;
}
} // namespace

void TestTablePreview::testDelegateWrapsOrdinaryText() {
  QVector<QVector<QString>> cells;
  cells.append({QStringLiteral("h")});
  cells.append({QStringLiteral("alpha beta gamma delta epsilon zeta")});

  TablePreviewWidget widget(nullptr, nullptr);
  auto view = buildSheet(widget, cells, {PreviewTableAlignment::None});
  QVERIFY(view);

  const QSize natural = cellHint(view, 1, 0, 0);
  QVERIFY(natural.width() > 0);
  QVERIFY(natural.height() > 0);

  const QSize wrapped = cellHint(view, 1, 0, natural.width() / 2);
  QVERIFY2(wrapped.width() <= natural.width() / 2,
           qPrintable(QStringLiteral("the cell did not fit its column: %1 > %2")
                          .arg(wrapped.width())
                          .arg(natural.width() / 2)));
  QVERIFY2(wrapped.height() >= 2 * natural.height(),
           qPrintable(QStringLiteral("half the width produced %1, not at least two lines of %2")
                          .arg(wrapped.height())
                          .arg(natural.height())));
}

void TestTablePreview::testDelegateWrapsAnUnbreakableToken() {
  // Qt's own item delegates wrap on word boundaries only, so a token with no
  // boundary in it stays on one line and is elided instead - which is exactly
  // the cell a Markdown table holds a URL or an identifier in.
  QVector<QVector<QString>> cells;
  cells.append({QStringLiteral("h")});
  cells.append({QString(64, QLatin1Char('w'))});

  TablePreviewWidget widget(nullptr, nullptr);
  auto view = buildSheet(widget, cells, {PreviewTableAlignment::None});
  QVERIFY(view);

  const QSize natural = cellHint(view, 1, 0, 0);
  const QSize wrapped = cellHint(view, 1, 0, natural.width() / 4);

  QVERIFY2(wrapped.height() >= 3 * natural.height(),
           qPrintable(QStringLiteral("the unbreakable token was not wrapped: %1 vs one line of %2")
                          .arg(wrapped.height())
                          .arg(natural.height())));
  QVERIFY(wrapped.width() <= natural.width() / 4);
}

void TestTablePreview::testDelegateAccumulatesLineHeightsOnly() {
  QVector<QVector<QString>> cells;
  cells.append({QStringLiteral("h")});
  cells.append({QString(64, QLatin1Char('w'))});

  TablePreviewWidget widget(nullptr, nullptr);
  auto view = buildSheet(widget, cells, {PreviewTableAlignment::None});
  QVERIFY(view);

  const QSize natural = cellHint(view, 1, 0, 0);
  QVERIFY(natural.height() > 0);

  // A QTextDocument would add its own document and paragraph margins here,
  // which is exactly the stale vertical space the sheet must not reserve. Only
  // whole line boxes may accumulate.
  for (int divisor = 2; divisor <= 5; ++divisor) {
    const QSize wrapped = cellHint(view, 1, 0, natural.width() / divisor);
    QVERIFY2(wrapped.height() % natural.height() == 0,
             qPrintable(QStringLiteral("height %1 is not a whole number of %2 pixel lines")
                            .arg(wrapped.height())
                            .arg(natural.height())));
  }
}

void TestTablePreview::testDelegateHonorsRolesAndDirection() {
  QVector<QVector<QString>> cells;
  cells.append({QStringLiteral("wwwwwwww"), QStringLiteral("right")});
  cells.append({QStringLiteral("wwwwwwww"), QStringLiteral("right")});

  TablePreviewWidget widget(nullptr, nullptr);
  auto view = buildSheet(widget, cells,
                         {PreviewTableAlignment::None, PreviewTableAlignment::Right});
  QVERIFY(view);

  // Row 0 carries a bold FontRole, so the very same text measures wider.
  QVERIFY2(cellHint(view, 0, 0, 0).width() > cellHint(view, 1, 0, 0).width(),
           "the header's bold font role was ignored");

  const int width = 200;
  const int height = 40;
  const QModelIndex right = view->model()->index(1, 1);

  // A right aligned column paints on the right of its cell.
  {
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    QStyleOptionViewItem option = cellOption(view, width, height);
    view->itemDelegate()->paint(&painter, option, right);
    painter.end();

    const QRect ink = inkBounds(image);
    QVERIFY(!ink.isNull());
    QVERIFY2(ink.center().x() > width / 2,
             qPrintable(QStringLiteral("the right aligned cell painted at %1 of %2")
                            .arg(ink.center().x())
                            .arg(width)));
  }

  // And a left aligned one in a right-to-left layout does the same, because
  // the visual alignment is what the delegate resolves.
  {
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    QStyleOptionViewItem option = cellOption(view, width, height);
    option.direction = Qt::RightToLeft;
    view->itemDelegate()->paint(&painter, option, view->model()->index(1, 0));
    painter.end();

    const QRect ink = inkBounds(image);
    QVERIFY(!ink.isNull());
    QVERIFY2(ink.center().x() > width / 2,
             qPrintable(QStringLiteral("the RTL cell painted at %1 of %2")
                            .arg(ink.center().x())
                            .arg(width)));
  }

  // The model asks for vertical centering, so a single line sits in the middle
  // of a band far taller than it is.
  {
    const int tall = 120;
    QImage image(width, tall, QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    view->itemDelegate()->paint(&painter, cellOption(view, width, tall),
                                view->model()->index(1, 0));
    painter.end();

    const QRect ink = inkBounds(image);
    QVERIFY(!ink.isNull());
    QVERIFY2(qAbs(ink.center().y() - tall / 2) <= 3,
             qPrintable(QStringLiteral("the line was not vertically centred: %1 of %2")
                            .arg(ink.center().y())
                            .arg(tall)));
  }
}

void TestTablePreview::testDelegateHonorsPaletteState() {
  // Declared first so it outlives the widget which is using it, and set to
  // paint no panel so the ink below is only ever the delegate's own text.
  RecordingStyle style;
  style.m_suppressPanel = true;

  QVector<QVector<QString>> cells;
  cells.append({QStringLiteral("h")});
  cells.append({QStringLiteral("MMMMMMMM")});

  TablePreviewWidget widget(nullptr, nullptr);
  auto view = buildSheet(widget, cells, {PreviewTableAlignment::None});
  QVERIFY(view);
  view->setStyle(&style);

  // One saturated channel per palette entry the delegate has to choose
  // between, so which one it resolved is readable straight off the pixels.
  QPalette palette = view->palette();
  palette.setColor(QPalette::Normal, QPalette::Text, Qt::red);
  palette.setColor(QPalette::Inactive, QPalette::Text, Qt::red);
  palette.setColor(QPalette::Disabled, QPalette::Text, Qt::green);
  palette.setColor(QPalette::Normal, QPalette::HighlightedText, Qt::blue);
  view->setPalette(palette);

  const QModelIndex index = view->model()->index(1, 0);

  auto painted = [&](QStyle::State p_state) {
    QImage image(200, 40, QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    QStyleOptionViewItem option = cellOption(view, image.width(), image.height());
    option.state = p_state;
    option.palette = palette;
    view->itemDelegate()->paint(&painter, option, index);
    painter.end();
    return inkChannels(image);
  };

  const QStyle::State active = QStyle::State_Enabled | QStyle::State_Active;

  const InkChannels normal = painted(active);
  QVERIFY2(normal.m_red > normal.m_green && normal.m_red > normal.m_blue,
           "an enabled cell was not painted with the normal text colour");

  // A cell without State_Enabled resolves the disabled group, which is what
  // makes a read-only sheet look like the viewer it is.
  const InkChannels disabled = painted(QStyle::State_Active);
  QVERIFY2(disabled.m_green > disabled.m_red && disabled.m_green > disabled.m_blue,
           "a disabled cell was not painted with the disabled text colour");

  // And a selected one takes the highlighted text colour rather than the
  // plain one.
  const InkChannels selected = painted(active | QStyle::State_Selected);
  QVERIFY2(selected.m_blue > selected.m_red && selected.m_blue > selected.m_green,
           "a selected cell was painted with the plain text colour");
}

void TestTablePreview::testDelegatePaintsPanelTextAndFocusInOrder() {
  // Declared first so it outlives the widget which is using it.
  RecordingStyle style;

  QVector<QVector<QString>> cells;
  cells.append({QStringLiteral("h")});
  cells.append({QStringLiteral("value")});

  TablePreviewWidget widget(nullptr, nullptr);
  auto view = buildSheet(widget, cells, {PreviewTableAlignment::None});
  QVERIFY(view);
  view->setStyle(&style);

  QImage image(120, 30, QImage::Format_RGB32);
  image.fill(Qt::white);
  style.m_image = &image;

  QPainter painter(&image);
  QStyleOptionViewItem option = cellOption(view, image.width(), image.height());
  option.state |= QStyle::State_HasFocus;
  view->itemDelegate()->paint(&painter, option, view->model()->index(1, 0));
  painter.end();

  QCOMPARE(style.m_calls, QStringList({QStringLiteral("panel"), QStringLiteral("focus")}));
  // The panel goes down on an untouched cell, and the focus frame on top of
  // text which has already been painted.
  QCOMPARE(style.m_inkAtPanel, 0);
  QVERIFY2(style.m_inkAtFocus > 0, "the focus frame was drawn before the text");
}

void TestTablePreview::testDelegatePreservesPainterState() {
  QVector<QVector<QString>> cells;
  cells.append({QStringLiteral("h")});
  cells.append({QStringLiteral("value")});

  TablePreviewWidget widget(nullptr, nullptr);
  auto view = buildSheet(widget, cells, {PreviewTableAlignment::None});
  QVERIFY(view);

  QImage image(200, 40, QImage::Format_RGB32);
  image.fill(Qt::white);

  QPainter painter(&image);
  painter.setPen(QPen(Qt::magenta, 3));
  painter.setBrush(Qt::cyan);
  painter.setClipRect(QRect(0, 0, 150, 40));

  const QPen pen = painter.pen();
  const QBrush brush = painter.brush();
  const QRegion clip = painter.clipRegion();

  QStyleOptionViewItem option = cellOption(view, 100, 40);
  option.rect.moveTo(20, 0);
  view->itemDelegate()->paint(&painter, option, view->model()->index(1, 0));

  QCOMPARE(painter.pen().color(), pen.color());
  QCOMPARE(painter.pen().width(), pen.width());
  QCOMPARE(painter.brush(), brush);
  QCOMPARE(painter.clipRegion(), clip);
  painter.end();
}

// ---------------------------------------------------------------------------
// Column planning
// ---------------------------------------------------------------------------

void TestTablePreview::testColumnFloorIsTwelveCharacters() {
  // A fixed pitch font, so "twelve average characters" is a width the test can
  // reason about without re-deriving the sheet's own formula from the same
  // metrics - which would pass for any wrong-but-mirrored floor.
  const QFont fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);

  QVector<QVector<QString>> cells;
  cells.append({QStringLiteral("h")});
  cells.append({QString(TablePreviewView::c_minimumColumnCharacters - 2, QLatin1Char('n'))});
  cells.append({QString(4 * TablePreviewView::c_minimumColumnCharacters, QLatin1Char('n'))});

  TablePreviewWidget widget(nullptr, nullptr);
  auto view = buildSheet(widget, cells, {PreviewTableAlignment::None});
  QVERIFY(view);
  view->setFont(fixed);

  const int floor = view->minimumColumnWidth();
  const int grid = view->showGrid() ? 1 : 0;

  // A header never assigns a section below its own minimum, so a floor under
  // that one could never be honoured.
  QVERIFY2(floor >= view->horizontalHeader()->minimumSectionSize(),
           qPrintable(QStringLiteral("the floor %1 is under the header minimum %2")
                          .arg(floor)
                          .arg(view->horizontalHeader()->minimumSectionSize())));

  // The property the floor exists for: a compressed column still shows about a
  // dozen characters on one line. Two characters of slack absorb the rounding
  // between a font's advertised average width and its actual advance.
  const int oneLine = cellHint(view, 0, 0, 0).height();
  QVERIFY(oneLine > 0);
  QCOMPARE(cellHint(view, 1, 0, floor - grid).height(), oneLine);

  // And it is a floor, not a licence to be arbitrarily wide: four times that
  // much text still has to wrap inside it.
  QVERIFY2(cellHint(view, 2, 0, floor - grid).height() >= 3 * oneLine,
           qPrintable(QStringLiteral("a %1 character cell did not wrap at the %2 pixel floor")
                          .arg(4 * TablePreviewView::c_minimumColumnCharacters)
                          .arg(floor)));

  // It is derived from the font rather than being a constant, so a larger
  // editor font moves it.
  QFont bigger = fixed;
  bigger.setPointSize(fixed.pointSize() + 8);
  view->setFont(bigger);
  QVERIFY2(view->minimumColumnWidth() > floor,
           qPrintable(QStringLiteral("the floor stayed at %1 when the font grew").arg(floor)));
}

void TestTablePreview::testColumnsCompressProportionally() {
  QVector<QVector<QString>> cells;
  cells.append({QString(60, QLatin1Char('w')), QString(24, QLatin1Char('m'))});
  cells.append({QString(60, QLatin1Char('w')), QString(24, QLatin1Char('m'))});

  TablePreviewWidget widget(nullptr, nullptr);
  auto view = buildSheet(widget, cells,
                         {PreviewTableAlignment::None, PreviewTableAlignment::None});
  QVERIFY(view);

  const int frame = 2 * view->frameWidth();
  const int minimum = view->minimumColumnWidth();

  resizeToWidth(widget, widget.sizeHint().width());
  const QVector<int> natural = columnWidths(view);
  const int naturalTotal = natural.at(0) + natural.at(1);
  QVERIFY(natural.at(0) > natural.at(1));
  QVERIFY2(natural.at(1) > minimum,
           "the narrow column has to start above the floor for compression to be visible");

  // Comfortably above the summed floors: both columns keep their proportions.
  const int roomy = qMax(naturalTotal * 4 / 5, 2 * minimum + 40);
  resizeToWidth(widget, roomy + frame);
  QCOMPARE(view->viewport()->width(), roomy);
  QCOMPARE(totalColumnWidth(view), roomy);
  for (int c = 0; c < 2; ++c) {
    const int expected = qRound(qreal(natural.at(c)) * roomy / naturalTotal);
    QVERIFY2(qAbs(view->columnWidth(c) - expected) <= 2,
             qPrintable(QStringLiteral("column %1 is %2, not its compressed share %3")
                            .arg(c)
                            .arg(view->columnWidth(c))
                            .arg(expected)));
  }

  // Tight enough that the narrow column would be pushed under the floor: it
  // is frozen there and the wide one absorbs the rest, still filling exactly.
  const int tight = 2 * minimum + 20;
  resizeToWidth(widget, tight + frame);
  QCOMPARE(view->viewport()->width(), tight);
  QCOMPARE(view->columnWidth(1), minimum);
  QCOMPARE(view->columnWidth(0), tight - minimum);
  QCOMPARE(totalColumnWidth(view), tight);
}

void TestTablePreview::testColumnsStopAtTheFloorAndScroll() {
  QVector<QVector<QString>> header;
  QVector<QString> row;
  QVector<PreviewTableAlignment> alignments;
  for (int c = 0; c < 6; ++c) {
    row.append(QStringLiteral("c%1").arg(c));
    alignments.append(PreviewTableAlignment::None);
  }
  header.append(row);
  header.append(row);

  TablePreviewWidget widget(nullptr, nullptr);
  auto view = buildSheet(widget, header, alignments);
  QVERIFY(view);

  const int frame = 2 * view->frameWidth();
  const int minimum = view->minimumColumnWidth();

  // Narrower than the summed floors: the columns stop compressing and the
  // overflow becomes reachable by scrolling instead.
  const int viewportWidth = 4 * minimum;
  resizeToWidth(widget, viewportWidth + frame);

  for (int c = 0; c < 6; ++c) {
    QCOMPARE(view->columnWidth(c), minimum);
  }
  QVERIFY2(totalColumnWidth(view) > view->viewport()->width(),
           qPrintable(QStringLiteral("columns total %1 do not overflow the viewport %2")
                          .arg(totalColumnWidth(view))
                          .arg(view->viewport()->width())));

  // And the band reserves the bar the overflow brings with it.
  const int rows = totalRowHeight(view, view->model()->rowCount());
  QVERIFY(view->horizontalScrollBar()->isVisible());
  QCOMPARE(widget.heightForWidth(viewportWidth + frame),
           frame + rows + horizontalBarReserve(view));
  QCOMPARE(view->viewport()->height(), rows);
}

void TestTablePreview::testVerticalScrollBarChromeIsReserved() {
  QVector<QVector<QString>> cells;
  for (int i = 0; i < 20; ++i) {
    cells.append({QStringLiteral("cell")});
  }

  TablePreviewWidget widget(nullptr, nullptr);
  QVERIFY(widget.setPreview(makeTable(cells, {PreviewTableAlignment::None})));
  auto view = widget.findChild<TablePreviewView *>();
  QVERIFY(view);

  widget.setVisibleRows(20);
  const int withoutBar = widget.sizeHint().width();

  widget.setVisibleRows(5);
  const int withBar = widget.sizeHint().width();

  // Every cell is identical, so the natural columns are the same either way
  // and the whole difference is the bar the shortened window brings with it.
  // A style whose bars overlap the content takes nothing, so the reserve is
  // only ever "whatever the viewport really loses".
  QVERIFY(withBar >= withoutBar);

  // Until the sheet has been laid out with that bar shown it can only estimate
  // what the style makes it cost. Once it has, the reserve has to be the width
  // the live viewport really loses - which is what a style that draws its
  // frame around the contents alone, or one whose bars overlap them, gets
  // wrong when it is derived from the bar's size hint instead.
  resizeToWidth(widget, qMax(withBar, withoutBar));
  QVERIFY(view->verticalScrollBar()->isVisible());

  const int live = view->width() - 2 * view->frameWidth() - view->viewport()->width();
  QCOMPARE(widget.sizeHint().width() - withoutBar, live);
  QCOMPARE(totalColumnWidth(view), view->viewport()->width());
}

// ---------------------------------------------------------------------------
// Band geometry
// ---------------------------------------------------------------------------

namespace {
QSharedPointer<const TablePreview> makeWrappingTable() {
  QVector<QVector<QString>> cells;
  cells.append({QStringLiteral("component"), QStringLiteral("description")});
  cells.append({QStringLiteral("VTextEdit"),
                QStringLiteral("base edit widget with cursor, selection and input method")});
  cells.append({QStringLiteral("VTextEditor"),
                QStringLiteral("adds syntax highlight, Vi mode, folding and completion")});
  return makeTable(cells, {PreviewTableAlignment::None, PreviewTableAlignment::None});
}
} // namespace

void TestTablePreview::testPureHeightMatchesTheLiveRows() {
  TablePreviewWidget widget(nullptr, nullptr);
  QVERIFY(widget.setPreview(makeWrappingTable()));
  auto view = widget.findChild<TablePreviewView *>();
  QVERIFY(view);

  const int frame = 2 * view->frameWidth();
  const int minimum = view->minimumColumnWidth();

  for (int viewportWidth : {2 * minimum + 30, 3 * minimum, 5 * minimum}) {
    resizeToWidth(widget, viewportWidth + frame);

    const int rows = totalRowHeight(view, view->model()->rowCount());
    const int bar = horizontalBarReserve(view);
    QVERIFY2(widget.heightForWidth(viewportWidth + frame) == frame + rows + bar,
             qPrintable(QStringLiteral("pure height %1 does not match the live rows %2 at "
                                       "viewport %3")
                            .arg(widget.heightForWidth(viewportWidth + frame))
                            .arg(frame + rows + bar)
                            .arg(viewportWidth)));

    // No empty strip under the last row either.
    QCOMPARE(view->viewport()->height(), rows);
  }
}

void TestTablePreview::testInheritedMutationDropsTheMemo() {
  TablePreviewWidget widget(nullptr, nullptr);
  QVERIFY(widget.setPreview(makeWrappingTable()));

  auto view = widget.findChild<TablePreviewView *>();
  QVERIFY(view);

  const int width = 3 * view->minimumColumnWidth() + 2 * view->frameWidth();
  resizeToWidth(widget, width);

  QSignalSpy spy(view, &TablePreviewView::preferredGeometryChanged);
  const int before = widget.heightForWidth(width);

  // A header minimum well above the sheet's own floor is one of the inherited
  // mutations which carry no signal of their own: nothing tells the sheet it
  // happened, and the answer the host is holding is now wrong. Asked for the
  // very same width, the memo must not survive it.
  view->horizontalHeader()->setMinimumSectionSize(4 * view->minimumColumnWidth());

  const int after = widget.heightForWidth(width);
  QVERIFY2(after != before,
           qPrintable(QStringLiteral("the constrained memo survived a mutation it depends on: "
                                     "%1 for both")
                          .arg(before)));

  // And the sheet owes the host exactly one report of it.
  settleWidget(widget);
  QCOMPARE(spy.count(), 1);
  QCOMPARE(widget.heightForWidth(width), after);
}

void TestTablePreview::testConstrainedHeightTracksTheWidth() {
  TablePreviewWidget widget(nullptr, nullptr);
  QVERIFY(widget.setPreview(makeWrappingTable()));
  auto view = widget.findChild<TablePreviewView *>();
  QVERIFY(view);

  const int frame = 2 * view->frameWidth();
  const int minimum = view->minimumColumnWidth();
  const int narrow = 2 * minimum + 20 + frame;
  const int wide = widget.sizeHint().width();

  const int tall = widget.heightForWidth(narrow);
  const int compact = widget.heightForWidth(wide);
  QVERIFY2(tall > compact,
           qPrintable(QStringLiteral("the narrow sheet is not taller: %1 vs %2")
                          .arg(tall)
                          .arg(compact)));

  // The answer is memoized per width, so asking again may not drift, and
  // asking for the other width must not return the memoized one.
  QCOMPARE(widget.heightForWidth(narrow), tall);
  QCOMPARE(widget.heightForWidth(wide), compact);
  QCOMPARE(widget.heightForWidth(narrow), tall);
}

void TestTablePreview::testRowsGrowAndShrinkWithTheWidth() {
  TablePreviewWidget widget(nullptr, nullptr);
  QVERIFY(widget.setPreview(makeWrappingTable()));
  auto view = widget.findChild<TablePreviewView *>();
  QVERIFY(view);

  const int frame = 2 * view->frameWidth();
  const int minimum = view->minimumColumnWidth();

  resizeToWidth(widget, widget.sizeHint().width());
  const int roomy = totalRowHeight(view, view->model()->rowCount());

  resizeToWidth(widget, 2 * minimum + 20 + frame);
  const int cramped = totalRowHeight(view, view->model()->rowCount());
  QVERIFY2(cramped > roomy,
           qPrintable(QStringLiteral("the rows did not grow when the sheet narrowed: %1 vs %2")
                          .arg(cramped)
                          .arg(roomy)));

  // Widening again has to give the space back: a row which only records that
  // it was fitted once stays tall forever.
  resizeToWidth(widget, widget.sizeHint().width());
  QCOMPARE(totalRowHeight(view, view->model()->rowCount()), roomy);
}

// ---------------------------------------------------------------------------
// Lazy row fitting
// ---------------------------------------------------------------------------

namespace {
QSharedPointer<const TablePreview> makeTallWrappingTable(int p_rows) {
  QVector<QVector<QString>> cells;
  cells.append({QStringLiteral("header")});
  for (int i = 1; i < p_rows; ++i) {
    cells.append({QStringLiteral("row %1 with a value long enough to wrap over several lines")
                      .arg(i)});
  }

  return makeTable(cells, {PreviewTableAlignment::None});
}
} // namespace

void TestTablePreview::testDistantRowsAreFittedLazily() {
  TablePreviewWidget widget(nullptr, nullptr);
  widget.setVisibleRows(5);
  QVERIFY(widget.setPreview(makeTallWrappingTable(300)));

  auto view = widget.findChild<TablePreviewView *>();
  QVERIFY(view);

  const int minimum = view->minimumColumnWidth();
  resizeToWidth(widget, minimum + 10 + 2 * view->frameWidth());

  const int defaultHeight = view->verticalHeader()->defaultSectionSize();
  QVERIFY2(view->rowHeight(1) > defaultHeight,
           "the preferred band was not fitted against its planned column");
  QCOMPARE(view->rowHeight(200), defaultHeight);

  view->scrollTo(view->model()->index(200, 0), QAbstractItemView::PositionAtTop);
  for (int i = 0; i < 40 && view->rowHeight(200) == defaultHeight; ++i) {
    QTest::qWait(10);
    QCoreApplication::processEvents();
  }

  QVERIFY2(view->rowHeight(200) > defaultHeight,
           qPrintable(QStringLiteral("row 200 was never fitted: %1 vs default %2; first row %3, "
                                     "vbar %4/%5, viewport %6x%7")
                          .arg(view->rowHeight(200))
                          .arg(defaultHeight)
                          .arg(view->rowAt(0))
                          .arg(view->verticalScrollBar()->value())
                          .arg(view->verticalScrollBar()->maximum())
                          .arg(view->viewport()->width())
                          .arg(view->viewport()->height())));
  // Only what the viewport needed: fitting the whole model is the cost the
  // lazy pass exists to avoid.
  QCOMPARE(view->rowHeight(299), defaultHeight);
}

void TestTablePreview::testRowFittingKeepsTheAnchorNearTheEnd() {
  TablePreviewWidget widget(nullptr, nullptr);
  widget.setVisibleRows(5);
  QVERIFY(widget.setPreview(makeTallWrappingTable(300)));

  auto view = widget.findChild<TablePreviewView *>();
  QVERIFY(view);

  const int minimum = view->minimumColumnWidth();
  resizeToWidth(widget, minimum + 10 + 2 * view->frameWidth());

  auto vbar = view->verticalScrollBar();
  QVERIFY(vbar->isVisible());

  // The very end of the table, where the bar has the least room left. Fitting
  // the rows there grows the content under a range Qt has only *posted* an
  // update for, so a correction applied against the stale maximum would be
  // clamped away and the sheet would visibly jump.
  vbar->setValue(vbar->maximum());

  // Sampled before the event loop turns, so the fitting pass the scroll just
  // scheduled has not run yet and the rows around the anchor are still at
  // their default height. Sampling after a settle would hide the bug: by then
  // the range has caught up and the correction no longer clamps.
  const int anchorRow = view->rowAt(0);
  QVERIFY(anchorRow > 0);
  const int anchorBefore = view->rowViewportPosition(anchorRow);

  for (int i = 0; i < 40; ++i) {
    QTest::qWait(10);
    QCoreApplication::processEvents();
  }

  const int defaultHeight = view->verticalHeader()->defaultSectionSize();
  QVERIFY2(view->rowHeight(anchorRow) > defaultHeight,
           qPrintable(QStringLiteral("the anchor row %1 was never fitted").arg(anchorRow)));

  // Whatever grew, the row the user was looking at is still where it was.
  QVERIFY2(view->rowAt(0) == anchorRow &&
               qAbs(view->rowViewportPosition(anchorRow) - anchorBefore) <= 1,
           qPrintable(QStringLiteral("the sheet jumped: row %1 at %2 was row %3 at %4")
                          .arg(view->rowAt(0))
                          .arg(view->rowViewportPosition(view->rowAt(0)))
                          .arg(anchorRow)
                          .arg(anchorBefore)));
}

void TestTablePreview::testRowFittingIsBounded() {
  TablePreviewWidget widget(nullptr, nullptr);
  widget.setVisibleRows(5);
  QVERIFY(widget.setPreview(makeTallWrappingTable(300)));

  auto view = widget.findChild<TablePreviewView *>();
  QVERIFY(view);

  const int minimum = view->minimumColumnWidth();
  // Tall enough to show a lot of rows at once, so the interval the lazy pass
  // has to cover is far larger than one batch.
  resizeAndSettle(widget, minimum + 10 + 2 * view->frameWidth(), 2000);

  const int defaultHeight = view->verticalHeader()->defaultSectionSize();
  int fitted = 0;
  for (int r = 0; r < 300; ++r) {
    if (view->rowHeight(r) != defaultHeight) {
      ++fitted;
    }
  }

  // Whatever the viewport covers, the pass never walks the whole model.
  QVERIFY2(fitted < 300,
           qPrintable(QStringLiteral("every one of the %1 rows was fitted").arg(fitted)));
  QVERIFY(fitted >= 5);

  // And it converges: the rows the viewport does cover are all fitted once the
  // event loop has turned enough times.
  for (int i = 0; i < 40; ++i) {
    QTest::qWait(10);
    QCoreApplication::processEvents();
  }

  const int last = qMax(0, view->rowAt(view->viewport()->height() - 1));
  for (int r = 0; r <= last; ++r) {
    QVERIFY2(view->rowHeight(r) != defaultHeight,
             qPrintable(QStringLiteral("visible row %1 was never fitted").arg(r)));
  }

  int highest = -1;
  for (int r = 0; r < 300; ++r) {
    if (view->rowHeight(r) != defaultHeight) {
      highest = r;
    }
  }

  // The pass only ever looks at the interval the viewport covers plus its
  // overscan, and that interval can only shrink as the rows inside it grow. So
  // the work is bounded by what the viewport spanned while every row was still
  // at its default height - never by the size of the model.
  const int overscan = 2;
  const int reach = view->viewport()->height() / defaultHeight + 2 * overscan + 2;
  QVERIFY2(highest <= reach,
           qPrintable(QStringLiteral("row %1 was fitted, far past the %2 rows the viewport "
                                     "could ever have covered")
                          .arg(highest)
                          .arg(reach)));
}

// ---------------------------------------------------------------------------
// Host notification
// ---------------------------------------------------------------------------

namespace {
// Counts the layout requests the host's event filter would see.
class LayoutRequestCounter : public QObject {
public:
  bool eventFilter(QObject *p_object, QEvent *p_event) Q_DECL_OVERRIDE {
    if (p_event->type() == QEvent::LayoutRequest) {
      ++m_count;
    }

    return QObject::eventFilter(p_object, p_event);
  }

  int m_count = 0;
};
} // namespace

void TestTablePreview::testSettledGeometryNotifiesTheHost() {
  TablePreviewWidget widget(nullptr, nullptr);
  QVERIFY(widget.setPreview(makeWrappingTable()));

  auto view = widget.findChild<TablePreviewView *>();
  QVERIFY(view);

  resizeToWidth(widget, widget.sizeHint().width());

  QSignalSpy spy(view, &TablePreviewView::preferredGeometryChanged);
  LayoutRequestCounter counter;
  widget.installEventFilter(&counter);

  // Pure geometry queries answer the host; they must never tell it that
  // something moved, or the measurement would feed itself.
  for (int i = 0; i < 3; ++i) {
    widget.sizeHint();
    widget.heightForWidth(widget.width());
    widget.heightForWidth(widget.width() / 2);
  }
  settleWidget(widget);
  QCOMPARE(spy.count(), 0);
  QCOMPARE(counter.m_count, 0);

  // A cell edited through the model alone. Committing one through the sheet
  // would replace the source and mark the host item dirty by itself, which
  // would pass this test without the notification existing at all.
  QVERIFY(view->model()->setData(
      view->model()->index(1, 1),
      QStringLiteral("a considerably longer description which needs a good deal more room"),
      Qt::EditRole));
  settleWidget(widget);

  QCOMPARE(spy.count(), 1);
  // Coalesced into exactly one request: a burst of settlements must not queue
  // a burst of host measurements.
  QCOMPARE(counter.m_count, 1);

  // The settled signature is the baseline now, so nothing else is owed.
  settleWidget(widget);
  QCOMPARE(spy.count(), 1);
  QCOMPARE(counter.m_count, 1);
}

namespace {
// A sheet whose second row holds raw inline Markdown, shown wide enough that a
// click lands on a predictable character.
TablePreviewView *buildEditableSheet(TablePreviewWidget &p_widget) {
  QVector<QVector<QString>> cells;
  cells.append({QStringLiteral("Left"), QStringLiteral("Center"), QStringLiteral("Right")});
  cells.append({QStringLiteral("*italic*"), QStringLiteral("**bold**"),
                QStringLiteral("`code`")});

  auto view = buildSheet(p_widget, cells,
                         {PreviewTableAlignment::Left, PreviewTableAlignment::Center,
                          PreviewTableAlignment::Right});
  if (view) {
    resizeToWidth(p_widget, 800);
  }

  return view;
}

QLineEdit *openEditorAt(TablePreviewView *p_view, const QPoint &p_viewportPos) {
  QTest::mouseClick(p_view->viewport(), Qt::LeftButton, Qt::NoModifier, p_viewportPos);
  QCoreApplication::processEvents();
  const auto editors = p_view->viewport()->findChildren<QLineEdit *>();
  return editors.isEmpty() ? nullptr : editors.first();
}
} // namespace

void TestTablePreview::testCellEditorHoldsRawMarkdown() {
  TablePreviewWidget widget(nullptr, nullptr);
  auto view = buildEditableSheet(widget);
  QVERIFY(view);

  // A cell is edited as the single line of raw Markdown it is: the custom
  // delegate replaces display painting and sizing only, so the inherited
  // editor still has to come up carrying the untouched source.
  const QModelIndex index = view->model()->index(1, 1);
  auto editor = openEditorAt(view, view->visualRect(index).center());
  QVERIFY(editor);
  QCOMPARE(editor->text(), QStringLiteral("**bold**"));
  QVERIFY(editor->isVisible());

  // And it covers the cell it is editing rather than collapsing to nothing.
  const QRect cell = view->visualRect(index);
  QVERIFY2(editor->width() >= cell.width() - 4,
           qPrintable(QStringLiteral("the editor is %1 wide for a %2 wide cell")
                          .arg(editor->width())
                          .arg(cell.width())));
}

void TestTablePreview::testOneClickStartsEditing() {
  TablePreviewWidget widget(nullptr, nullptr);
  auto view = buildEditableSheet(widget);
  QVERIFY(view);

  // A single click, the way clicking into a paragraph of text starts editing
  // it. Qt's own sheets need a second click on an already selected cell, or a
  // double click.
  const QModelIndex index = view->model()->index(1, 0);
  QVERIFY(view->viewport()->findChildren<QLineEdit *>().isEmpty());

  auto editor = openEditorAt(view, view->visualRect(index).center());
  QVERIFY2(editor, "one click did not start editing");
  QCOMPARE(editor->text(), QStringLiteral("*italic*"));
}

void TestTablePreview::testClickPutsTheCaretUnderThePointer() {
  TablePreviewWidget widget(nullptr, nullptr);
  auto view = buildEditableSheet(widget);
  QVERIFY(view);

  const QModelIndex index = view->model()->index(1, 1);
  const QRect cell = view->visualRect(index);

  // Click near the left edge of the text rather than the middle of the cell,
  // so a caret which merely went to one end could not pass by accident.
  const QPoint pos(cell.left() + 6, cell.center().y());
  auto editor = openEditorAt(view, pos);
  QVERIFY(editor);

  // Nothing selected: the next keystroke edits the value instead of replacing
  // it, which is what selecting the whole cell would do.
  QVERIFY2(editor->selectedText().isEmpty(),
           qPrintable(QStringLiteral("the editor came up with %1 selected")
                          .arg(editor->selectedText())));

  // And the caret is where the pointer was, not at either end.
  const int expected = editor->cursorPositionAt(editor->mapFrom(view->viewport(), pos));
  QCOMPARE(editor->cursorPosition(), expected);
  QVERIFY2(editor->cursorPosition() < editor->text().size(),
           qPrintable(QStringLiteral("the caret went to the end (%1) instead of the click")
                          .arg(editor->cursorPosition())));

  // Typing now inserts at the caret rather than replacing the cell.
  QTest::keyClicks(editor, QStringLiteral("X"));
  QCOMPARE(editor->text().size(), QStringLiteral("**bold**").size() + 1);
  QVERIFY(editor->text().contains(QStringLiteral("bold")));
}

void TestTablePreview::testClickAtTheEndPutsTheCaretAtTheEnd() {
  TablePreviewWidget widget(nullptr, nullptr);
  auto view = buildEditableSheet(widget);
  QVERIFY(view);

  const QModelIndex index = view->model()->index(1, 1);
  const QRect cell = view->visualRect(index);

  // Clicking past the end of the text parks the caret after the last
  // character, the way a text editor does.
  auto editor = openEditorAt(view, QPoint(cell.right() - 2, cell.center().y()));
  QVERIFY(editor);
  QVERIFY(editor->selectedText().isEmpty());
  QCOMPARE(editor->cursorPosition(), editor->text().size());
}

void TestTablePreview::testReadOnlySheetIgnoresTheClick() {
  TablePreviewWidget widget(nullptr, nullptr);
  auto view = buildEditableSheet(widget);
  QVERIFY(view);

  view->setEditTriggers(QAbstractItemView::NoEditTriggers);

  const QModelIndex index = view->model()->index(1, 1);
  QVERIFY2(!openEditorAt(view, view->visualRect(index).center()),
           "a click opened an editor on a sheet which cannot be written back");
}
QTEST_MAIN(tests::TestTablePreview)
