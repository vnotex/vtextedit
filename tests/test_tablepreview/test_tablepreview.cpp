#include "test_tablepreview.h"

#include <QSignalSpy>
#include <QStringList>

#include <vtextedit/preview.h>

#include "previewbuilder.h"
#include "tablepreviewwidget.h"

#include <QHeaderView>

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
  // shows. Only the visible window is fitted - the reserved geometry has to
  // stay correct anyway.
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
  QVERIFY2(hint >= windowHeight,
           qPrintable(QStringLiteral("preferred height %1 < window height %2")
                          .arg(hint)
                          .arg(windowHeight)));
  QVERIFY2(hint < 2 * windowHeight + 40,
           qPrintable(QStringLiteral("preferred height %1 covers more than the window (%2)")
                          .arg(hint)
                          .arg(windowHeight)));
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

QTEST_MAIN(tests::TestTablePreview)
