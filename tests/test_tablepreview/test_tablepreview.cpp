#include "test_tablepreview.h"

#include <QAbstractTextDocumentLayout>
#include <QClipboard>
#include <QDropEvent>
#include <QFontDatabase>
#include <QImage>
#include <QInputMethodEvent>
#include <QMimeData>
#include <QPainter>
#include <QScrollBar>
#include <QSignalSpy>
#include <QStringList>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextLayout>
#include <QTextTable>
#include <QTextTableCell>

#include <vtextedit/preview.h>

#include "previewbuilder.h"
#include "tablepreviewwidget.h"

using namespace tests;
using namespace vte;

namespace vte {
// PreviewWidgetContext's constructor and the two setters the host drives it
// through are private to InteractivePreviewHost. The real host lives in a
// translation unit this target deliberately does not compile - it exists to
// reach the sheet's internals, which the black-box test_interactivepreview
// target cannot - so a stand-in of the same name is what lets a unit test own
// a context at all.
class InteractivePreviewHost {
public:
  static PreviewWidgetContext *createContext(quint64 p_identity, QObject *p_parent) {
    return new PreviewWidgetContext(p_identity, p_parent);
  }

  static void bind(PreviewWidgetContext *p_context,
                   const QSharedPointer<const Preview> &p_preview) {
    p_context->setPreview(p_preview);
  }

  static void finish(PreviewWidgetContext *p_context, const PreviewReplacementResult &p_result) {
    p_context->notifyReplacementFinished(p_result);
  }
};
} // namespace vte

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

// The same table, but with the source Markdown the serializer would emit for
// it. Only a snapshot whose source really describes its cells can exercise the
// echo path, which compares exactly that string.
QSharedPointer<const TablePreview>
makeSnapshot(const QVector<QVector<QString>> &p_cells,
             const QVector<PreviewTableAlignment> &p_alignments, quint64 p_revision = 1) {
  const QVector<QString> prefixes(p_cells.size(), QString());
  const QString source =
      TablePreviewSerializer::serialize(p_cells, p_alignments, prefixes, QString());
  auto preview = PreviewBuilder::createTable(p_revision, 0, source.size(), source,
                                             p_alignments.size(), p_cells, p_alignments, prefixes,
                                             QString());
  return preview.staticCast<const TablePreview>();
}

// Parse the canonical Markdown the serializer emits back into a snapshot, the
// way the editor's real parse generation would. Only the shape the serializer
// produces has to be understood: a leading and a trailing pipe on every line,
// the delimiter row second, and '|' inside a cell escaped as "\|". Cells
// holding a literal backslash are outside what these tests use.
QSharedPointer<const TablePreview> parseCanonical(const QString &p_markdown, quint64 p_revision) {
  const QStringList lines = p_markdown.split(QLatin1Char('\n'));
  if (lines.size() < 2) {
    return QSharedPointer<const TablePreview>();
  }

  auto splitRow = [](const QString &p_line) {
    QVector<QString> cells;
    const QString trimmed = p_line.trimmed();
    if (trimmed.size() < 2) {
      return cells;
    }

    const QString body = trimmed.mid(1, trimmed.size() - 2);
    QString current;
    for (int i = 0; i < body.size(); ++i) {
      const QChar ch = body.at(i);
      if (ch == QLatin1Char('\\') && i + 1 < body.size() &&
          body.at(i + 1) == QLatin1Char('|')) {
        current.append(QLatin1Char('|'));
        ++i;
        continue;
      }

      if (ch == QLatin1Char('|')) {
        cells.append(current.trimmed());
        current.clear();
        continue;
      }

      current.append(ch);
    }

    cells.append(current.trimmed());
    return cells;
  };

  QVector<PreviewTableAlignment> alignments;
  for (const auto &marker : splitRow(lines.at(1))) {
    const bool left = marker.startsWith(QLatin1Char(':'));
    const bool right = marker.endsWith(QLatin1Char(':'));
    if (left && right) {
      alignments.append(PreviewTableAlignment::Center);
    } else if (left) {
      alignments.append(PreviewTableAlignment::Left);
    } else if (right) {
      alignments.append(PreviewTableAlignment::Right);
    } else {
      alignments.append(PreviewTableAlignment::None);
    }
  }

  QVector<QVector<QString>> cells;
  cells.append(splitRow(lines.at(0)));
  for (int i = 2; i < lines.size(); ++i) {
    cells.append(splitRow(lines.at(i)));
  }

  const QVector<QString> prefixes(cells.size(), QString());
  auto preview = PreviewBuilder::createTable(p_revision, 0, p_markdown.size(), p_markdown,
                                             alignments.size(), cells, alignments, prefixes,
                                             QString());
  return preview.staticCast<const TablePreview>();
}

QTextTable *tableOf(const QTextDocument *p_doc) {
  if (!p_doc || !p_doc->rootFrame()) {
    return nullptr;
  }

  for (auto frame : p_doc->rootFrame()->childFrames()) {
    if (auto table = qobject_cast<QTextTable *>(frame)) {
      return table;
    }
  }

  return nullptr;
}

TablePreviewSheet *sheetOf(TablePreviewWidget &p_widget) {
  return p_widget.findChild<TablePreviewSheet *>();
}

QString cellText(const QTextDocument *p_doc, int p_row, int p_column) {
  QTextTable *table = tableOf(p_doc);
  if (!table) {
    return QString();
  }

  const QTextTableCell cell = table->cellAt(p_row, p_column);
  if (!cell.isValid()) {
    return QString();
  }

  QTextCursor cursor = cell.firstCursorPosition();
  cursor.setPosition(cell.lastCursorPosition().position(), QTextCursor::KeepAnchor);
  return cursor.selectedText();
}

// The rectangle one cell's text occupies, in viewport coordinates.
QRect cellRect(TablePreviewSheet *p_sheet, int p_row, int p_column) {
  QTextTable *table = tableOf(p_sheet->document());
  if (!table) {
    return QRect();
  }

  const QTextTableCell cell = table->cellAt(p_row, p_column);
  if (!cell.isValid()) {
    return QRect();
  }

  return p_sheet->cursorRect(cell.firstCursorPosition())
      .united(p_sheet->cursorRect(cell.lastCursorPosition()));
}

void putCaretIn(TablePreviewSheet *p_sheet, int p_row, int p_column, bool p_atEnd = true) {
  QTextTable *table = tableOf(p_sheet->document());
  QVERIFY2(table, "the sheet has no table");
  const QTextTableCell cell = table->cellAt(p_row, p_column);
  QVERIFY2(cell.isValid(), "no such cell");
  p_sheet->setTextCursor(p_atEnd ? cell.lastCursorPosition() : cell.firstCursorPosition());
}

// Type into a cell the way a keystroke does: the caret goes there first, so
// the cell-leave commit sees the same transition the user would produce.
void typeInto(TablePreviewSheet *p_sheet, int p_row, int p_column, const QString &p_text) {
  putCaretIn(p_sheet, p_row, p_column);
  QTextCursor cursor = p_sheet->textCursor();
  cursor.insertText(p_text);
}

void showOffScreen(TablePreviewWidget &p_widget, int p_width) {
  if (!p_widget.isVisible()) {
    // Off-screen is enough: the layout and the resize events are real either
    // way, and no window is opened.
    p_widget.setAttribute(Qt::WA_DontShowOnScreen, true);
    p_widget.show();
  }

  p_widget.resize(p_width, qMax(1, p_widget.heightForWidth(p_width)));
  QCoreApplication::processEvents();
}

void settle() {
  QCoreApplication::processEvents();
  QTest::qWait(30);
  QCoreApplication::processEvents();
}

void waitForCommit() {
  QTest::qWait(TablePreviewWidget::c_commitDebounceMs + 200);
  QCoreApplication::processEvents();
}

// Drives one sheet the way InteractivePreviewHost does: it owns the context,
// answers replacement requests and rebases the bound snapshot onto the text
// which would now be in the document - which is exactly the window the echo
// handling has to survive.
class SheetHarness : public QObject {
public:
  explicit SheetHarness(const QSharedPointer<const TablePreview> &p_table) {
    m_context = vte::InteractivePreviewHost::createContext(1, this);
    vte::InteractivePreviewHost::bind(m_context, p_table);

    connect(m_context, &PreviewWidgetContext::sourceReplacementRequested, this,
            &SheetHarness::handleRequest);

    m_widget = new TablePreviewWidget(m_context, nullptr);
    m_widget->setPreview(p_table);
  }

  ~SheetHarness() Q_DECL_OVERRIDE { delete m_widget; }

  TablePreviewWidget *widget() const { return m_widget; }

  TablePreviewSheet *sheet() const { return m_widget->findChild<TablePreviewSheet *>(); }

  // Deliver a parse generation exactly as InteractivePreviewHost::updateItem()
  // does: the context is rebound first, because resetFromSource() deliberately
  // re-reads it rather than trusting the sheet's cached snapshot.
  bool deliver(const QSharedPointer<const TablePreview> &p_table) {
    vte::InteractivePreviewHost::bind(m_context, p_table);
    return m_widget->setPreview(p_table);
  }

  int requestCount() const { return m_requests.size(); }

  QString lastRequest() const { return m_requests.isEmpty() ? QString() : m_requests.last(); }

  const QStringList &requests() const { return m_requests; }

  // The source the context is currently bound to, which is what the host
  // rebases on every accepted replacement.
  QString boundSource() const {
    const auto bound = m_context->preview();
    return bound ? bound->sourceMarkdown() : QString();
  }

  void setNextStatus(PreviewReplacementResult::Status p_status) { m_nextStatus = p_status; }

  // The snapshot a parse generation would deliver for the text the harness has
  // accepted so far.
  QSharedPointer<const TablePreview> echo() const {
    return m_accepted.isEmpty() ? QSharedPointer<const TablePreview>()
                                : parseCanonical(m_accepted.last(), 1);
  }

private:
  void handleRequest(quint64 p_identity, quint64 p_revision, const QString &p_expectedSource,
                     const QString &p_replacement) {
    Q_UNUSED(p_revision);
    Q_UNUSED(p_expectedSource);
    m_requests.append(p_replacement);

    PreviewReplacementResult result;
    result.setIdentity(p_identity);
    result.setStatus(m_nextStatus);

    if (m_nextStatus == PreviewReplacementResult::Accepted) {
      m_accepted.append(p_replacement);
      // The host rebases the bound snapshot immediately, before the next parse
      // generation, so a second request in that window is not a mismatch.
      vte::InteractivePreviewHost::bind(m_context, parseCanonical(p_replacement, 1));
    }

    vte::InteractivePreviewHost::finish(m_context, result);
  }

  PreviewWidgetContext *m_context = nullptr;

  TablePreviewWidget *m_widget = nullptr;

  QStringList m_requests;

  QStringList m_accepted;

  PreviewReplacementResult::Status m_nextStatus = PreviewReplacementResult::Accepted;
};

// A two by two table whose source really is its canonical Markdown.
QSharedPointer<const TablePreview> makeCommittableTable() {
  QVector<QVector<QString>> cells;
  cells.append({QStringLiteral("h1"), QStringLiteral("h2")});
  cells.append({QStringLiteral("a"), QStringLiteral("b")});
  return makeSnapshot(cells, {PreviewTableAlignment::None, PreviewTableAlignment::None});
}

// A table whose second column wraps, so its height depends on the width.
QSharedPointer<const TablePreview> makeWrappingTable() {
  QVector<QVector<QString>> cells;
  cells.append({QStringLiteral("component"), QStringLiteral("description")});
  cells.append({QStringLiteral("VTextEdit"),
                QStringLiteral("base edit widget with cursor, selection and input method")});
  cells.append({QStringLiteral("VTextEditor"),
                QStringLiteral("adds syntax highlight, Vi mode, folding and completion")});
  return makeTable(cells, {PreviewTableAlignment::None, PreviewTableAlignment::None});
}

// A sheet holding raw inline Markdown, shown wide enough that a click lands on
// a predictable character.
TablePreviewWidget *buildEditableSheet(QScopedPointer<TablePreviewWidget> &p_holder) {
  QVector<QVector<QString>> cells;
  cells.append({QStringLiteral("Left"), QStringLiteral("Center"), QStringLiteral("Right")});
  cells.append({QStringLiteral("*italic*"), QStringLiteral("**bold**"), QStringLiteral("`code`")});

  p_holder.reset(new TablePreviewWidget(nullptr, nullptr));
  if (!p_holder->setPreview(makeTable(cells, {PreviewTableAlignment::Left,
                                              PreviewTableAlignment::Center,
                                              PreviewTableAlignment::Right}))) {
    return nullptr;
  }

  showOffScreen(*p_holder, 800);
  settle();
  return p_holder.data();
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
  QCOMPARE(markdown, QStringLiteral("| h1 | h2 |\n| --- | --- |\n| a | b |"));
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
  // Cells are emitted compactly, with a single space inside each border.
  QCOMPARE(lines[0], QStringLiteral("| a | b | c | d |"));
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
  QCOMPARE(lines[0], QStringLiteral("| a |  |  |"));
  QCOMPARE(lines[1], QStringLiteral("| --- | --- | --- |"));
  QCOMPARE(lines[2], QStringLiteral("| x | y | z |"));
  QCOMPARE(lines[3], QStringLiteral("| only |  |  |"));
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

void TestTablePreview::testEscapedPipeDoesNotWidenColumn() {
  QVector<QVector<QString>> cells;
  cells.append({QStringLiteral("ab")});
  // Escaping adds a backslash to the emitted text; the compact format never
  // pads, so no other row is affected either way.
  cells.append({QStringLiteral("a|b")});

  const QVector<PreviewTableAlignment> alignments{PreviewTableAlignment::None};
  const QVector<QString> prefixes{QString(), QString()};

  const QString markdown =
      TablePreviewSerializer::serialize(cells, alignments, prefixes, QString());
  const QStringList lines = markdown.split(QLatin1Char('\n'));
  QCOMPARE(lines.size(), 3);
  QCOMPARE(lines[0], QStringLiteral("| ab |"));
  QCOMPARE(lines[1], QStringLiteral("| --- |"));
  QCOMPARE(lines[2], QStringLiteral("| a\\|b |"));

  // Round trip: the escaped pipe still parses back as one cell.
  QCOMPARE(TablePreviewSerializer::escapeCell(QStringLiteral("a|b")), QStringLiteral("a\\|b"));
}

void TestTablePreview::testSerializeDoesNotPadOtherRows() {
  // The size limits bound the cell *count*, never the cell text length, so one
  // very wide cell must not be allowed to widen any other row.
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

  // The wide cell is emitted in full.
  QVERIFY(markdown.contains(wide));

  // No other row is padded at all: the compact format sizes every row by its
  // own content.
  const QStringList lines = markdown.split(QLatin1Char('\n'));
  QCOMPARE(lines.size(), rows + 1);
  for (int i = 0; i < lines.size(); ++i) {
    if (lines[i].contains(wide)) {
      continue;
    }

    QVERIFY2(lines[i].size() < 20, qPrintable(QStringLiteral("line %1 is %2 characters wide")
                                                  .arg(i)
                                                  .arg(lines[i].size())));
  }

  // Which bounds the whole output: with padding it would be rows x 500.
  QVERIFY2(markdown.size() < wide.size() + rows * 20,
           qPrintable(QStringLiteral("serialized %1 characters").arg(markdown.size())));
}

// ---------------------------------------------------------------------------
// Document
// ---------------------------------------------------------------------------

void TestTablePreview::testDocumentBuildsTheTable() {
  QVector<QVector<QString>> cells;
  cells.append({QStringLiteral("h1"), QStringLiteral("h2")});
  cells.append({QStringLiteral("a"), QStringLiteral("b")});
  cells.append({QStringLiteral("c"), QStringLiteral("d")});

  TablePreviewDocument document;
  document.setTable(makeTable(cells, {PreviewTableAlignment::None, PreviewTableAlignment::None}));

  QVERIFY(document.document());
  QTextTable *table = document.table();
  QVERIFY(table);
  QCOMPARE(table->rows(), 3);
  QCOMPARE(table->columns(), 2);
  QCOMPARE(document.rowCount(), 3);
  QCOMPARE(document.columnCount(), 2);

  // The whole band, shared out by Qt's own table layout.
  const QTextTableFormat format = table->format().toTableFormat();
  QCOMPARE(format.width().type(), QTextLength::PercentageLength);
  QCOMPARE(format.width().rawValue(), qreal(100));
  QCOMPARE(format.headerRowCount(), 1);
  for (const auto &constraint : format.columnWidthConstraints()) {
    QCOMPARE(constraint.type(), QTextLength::VariableLength);
  }

  // The inner document never accumulates an undo stack of its own: the
  // granularity the user sees is one whole-table replacement.
  QVERIFY(!document.document()->isUndoRedoEnabled());
}

void TestTablePreview::testDocumentNormalization() {
  QVector<QVector<QString>> cells;
  cells.append({QStringLiteral("a"), QStringLiteral("b")});
  cells.append({QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("z")});
  cells.append({QStringLiteral("only")});

  TablePreviewDocument document;
  document.setTable(makeTable(cells, {PreviewTableAlignment::None, PreviewTableAlignment::None}));

  QCOMPARE(document.rowCount(), 3);
  // The width is the maximum of the header, the alignment row and every body
  // row: nothing is ever discarded.
  QCOMPARE(document.columnCount(), 3);
  QCOMPARE(cellText(document.document(), 0, 2), QString());
  QCOMPARE(cellText(document.document(), 1, 2), QStringLiteral("z"));
  QCOMPARE(cellText(document.document(), 2, 0), QStringLiteral("only"));
}

void TestTablePreview::testHeaderRowIsBold() {
  QVector<QVector<QString>> cells;
  cells.append({QStringLiteral("head")});
  cells.append({QStringLiteral("body")});

  TablePreviewDocument document;
  document.setTable(makeTable(cells, {PreviewTableAlignment::None}));

  QTextTable *table = document.table();
  QVERIFY(table);

  // Bold is a character format, not Markdown: a cell holds its raw source, so
  // '**' here would be two literal asterisks.
  const QTextBlock header = table->cellAt(0, 0).firstCursorPosition().block();
  QVERIFY(header.begin() != header.end());
  QCOMPARE(header.begin().fragment().charFormat().fontWeight(), static_cast<int>(QFont::Bold));

  const QTextBlock body = table->cellAt(1, 0).firstCursorPosition().block();
  QVERIFY(body.begin() != body.end());
  QVERIFY(body.begin().fragment().charFormat().fontWeight() < QFont::Bold);
}

void TestTablePreview::testColumnAlignmentIsABlockFormat() {
  QVector<QVector<QString>> cells;
  cells.append({QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")});
  cells.append({QStringLiteral("d"), QStringLiteral("e"), QStringLiteral("f")});

  TablePreviewDocument document;
  document.setTable(makeTable(cells, {PreviewTableAlignment::Left, PreviewTableAlignment::Center,
                                      PreviewTableAlignment::Right}));

  QTextTable *table = document.table();
  QVERIFY(table);

  for (int row = 0; row < 2; ++row) {
    QCOMPARE(table->cellAt(row, 0).firstCursorPosition().blockFormat().alignment(),
             Qt::Alignment(Qt::AlignLeft));
    QCOMPARE(table->cellAt(row, 1).firstCursorPosition().blockFormat().alignment(),
             Qt::Alignment(Qt::AlignHCenter));
    QCOMPARE(table->cellAt(row, 2).firstCursorPosition().blockFormat().alignment(),
             Qt::Alignment(Qt::AlignRight));
  }
}

void TestTablePreview::testCellsHoldRawMarkdown() {
  QVector<QVector<QString>> cells;
  cells.append({QStringLiteral("Left"), QStringLiteral("Center")});
  cells.append({QStringLiteral("*italic*"), QStringLiteral("**bold**")});

  TablePreviewDocument document;
  document.setTable(makeTable(cells, {PreviewTableAlignment::None, PreviewTableAlignment::None}));

  // The cell shows and edits the untouched source; '**bold**' is eight literal
  // characters, not a bold word.
  QCOMPARE(cellText(document.document(), 1, 1), QStringLiteral("**bold**"));
  QCOMPARE(cellText(document.document(), 1, 0), QStringLiteral("*italic*"));
  QCOMPARE(document.cells().at(1).at(1), QStringLiteral("**bold**"));
}

void TestTablePreview::testCellWalkReturnsTheEditedText() {
  QVector<QVector<QString>> cells;
  cells.append({QStringLiteral("h1"), QStringLiteral("h2")});
  cells.append({QStringLiteral("a"), QStringLiteral("b")});

  TablePreviewDocument document;
  document.setTable(makeTable(cells, {PreviewTableAlignment::None, PreviewTableAlignment::None}));

  // The document is the single source of truth: there is no parallel matrix
  // which could disagree with what the caret just typed.
  QTextTable *table = document.table();
  QTextCursor cursor = table->cellAt(1, 1).lastCursorPosition();
  cursor.insertText(QStringLiteral("bcd"));

  QCOMPARE(document.cells().at(1).at(1), QStringLiteral("bbcd"));
  QCOMPARE(document.toMarkdown(), QStringLiteral("| h1 | h2 |\n"
                                                 "| --- | --- |\n"
                                                 "| a | bbcd |"));
}

void TestTablePreview::testDocumentRoundTrip() {
  QVector<QVector<QString>> cells;
  cells.append({QStringLiteral("h1"), QStringLiteral("h2")});
  cells.append({QStringLiteral("a"), QStringLiteral("b")});

  TablePreviewDocument document;
  document.setTable(makeTable(cells, {PreviewTableAlignment::None, PreviewTableAlignment::Right}));

  // The right aligned column keeps its ":" marker.
  QCOMPARE(document.toMarkdown(),
           QStringLiteral("| h1 | h2 |\n| --- | ---: |\n| a | b |"));

  QTextCursor cursor = document.table()->cellAt(1, 1).lastCursorPosition();
  cursor.insertText(QStringLiteral("etter value"));
  QCOMPARE(document.toMarkdown(), QStringLiteral("| h1 | h2 |\n"
                                                 "| --- | ---: |\n"
                                                 "| a | better value |"));
}

void TestTablePreview::testRaggedTableIsNotRoundTrippable() {
  QVector<QVector<QString>> cells;
  cells.append({QStringLiteral("h1"), QStringLiteral("h2")});
  cells.append({QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")});

  TablePreviewDocument document;
  document.setTable(makeTable(cells, {PreviewTableAlignment::None, PreviewTableAlignment::None}));

  // The excess cell stays visible, so nothing is hidden from the user.
  QCOMPARE(document.columnCount(), 3);

  // Writing it back would promote it into the header and the delimiter row,
  // turning a cell GFM ignores today into a real column.
  QVERIFY(!document.isRoundTrippable());
  QVERIFY(document.toMarkdown().isEmpty());

  QVector<QVector<QString>> even;
  even.append({QStringLiteral("h1"), QStringLiteral("h2")});
  even.append({QStringLiteral("a"), QStringLiteral("b")});

  TablePreviewDocument evenDocument;
  evenDocument.setTable(
      makeTable(even, {PreviewTableAlignment::None, PreviewTableAlignment::None}));
  QVERIFY(evenDocument.isRoundTrippable());
  QVERIFY(!evenDocument.toMarkdown().isEmpty());
}

void TestTablePreview::testFormatRefreshKeepsTheCaret() {
  QScopedPointer<TablePreviewWidget> holder;
  auto widget = buildEditableSheet(holder);
  QVERIFY(widget);
  auto sheet = sheetOf(*widget);
  QVERIFY(sheet);

  putCaretIn(sheet, 1, 1);
  const int caret = sheet->textCursor().position();
  const QString before = cellText(sheet->document(), 1, 1);

  // A theme or font change during typing must not rebuild the cells: that
  // would silently lose the user's place.
  QFont bigger = widget->font();
  bigger.setPointSize(bigger.pointSize() + 4);
  widget->setFont(bigger);
  settle();

  QCOMPARE(sheet->textCursor().position(), caret);
  QCOMPARE(cellText(sheet->document(), 1, 1), before);
  // And the derived formats are still in place.
  QTextTable *table = tableOf(sheet->document());
  QVERIFY(table);
  QCOMPARE(table->cellAt(1, 2).firstCursorPosition().blockFormat().alignment(),
           Qt::Alignment(Qt::AlignRight));
}

// ---------------------------------------------------------------------------
// Limits
// ---------------------------------------------------------------------------

void TestTablePreview::testWidgetRejectsNonTable() {
  TablePreviewWidget widget(nullptr, nullptr);
  QCOMPARE(widget.supportedTypes(), QVector<PreviewElementType>{PreviewElementType::Table});

  auto code = PreviewBuilder::createCode(1, 0, 3, QStringLiteral("```\n```"), QString(),
                                         QString());
  QVERIFY(!widget.setPreview(code));
  QVERIFY(!widget.setPreview(QSharedPointer<const Preview>()));
}

void TestTablePreview::testWidgetRejectsOversizedTable() {
  // A QTextTable is not virtualized: every cell is a QTextBlock, the sheet
  // renders at its full natural height and Qt relays the whole table out on
  // every keystroke. The bound is the measured 16 ms frame budget, so a table
  // past it is left to the static source rendering.
  const int columns = 10;
  const int rows = TablePreviewDocument::c_maxCells / columns + 1;

  QVector<QString> row;
  QVector<PreviewTableAlignment> alignments;
  for (int c = 0; c < columns; ++c) {
    row.append(QStringLiteral("c%1").arg(c));
    alignments.append(PreviewTableAlignment::None);
  }

  QVector<QVector<QString>> cells;
  for (int r = 0; r < rows; ++r) {
    cells.append(row);
  }

  auto table = makeTable(cells, alignments);
  QVERIFY(TablePreviewDocument::normalizedCellCount(*table) > TablePreviewDocument::c_maxCells);

  TablePreviewWidget widget(nullptr, nullptr);
  QVERIFY(!widget.setPreview(table));

  // A table within the budget is still accepted.
  QVector<QVector<QString>> small;
  small.append({QStringLiteral("a"), QStringLiteral("b")});
  small.append({QStringLiteral("c"), QStringLiteral("d")});
  auto smallTable = makeTable(small, {PreviewTableAlignment::None, PreviewTableAlignment::None});
  QVERIFY(TablePreviewDocument::normalizedCellCount(*smallTable) <=
          TablePreviewDocument::c_maxCells);
  QVERIFY(widget.setPreview(smallTable));
}

void TestTablePreview::testWidgetRejectsTooManyRowsOrColumns() {
  // There is no independent row bound: the cell bound already caps a
  // single-column table at 300 rows, so a row bound could never decide
  // anything. A very tall table is still refused - by the cell bound.
  QVector<QVector<QString>> tall;
  for (int i = 0; i < TablePreviewDocument::c_maxCells + 1; ++i) {
    tall.append({QStringLiteral("r")});
  }

  auto tallTable = makeTable(tall, {PreviewTableAlignment::None});
  QVERIFY(!TablePreviewDocument::isWithinLimits(*tallTable));

  TablePreviewWidget widget(nullptr, nullptr);
  QVERIFY(!widget.setPreview(tallTable));

  // The column bound, by contrast, still binds on its own: a one-row table
  // wide enough to trip it is well inside the cell bound.
  QVector<QString> wideRow;
  QVector<PreviewTableAlignment> alignments;
  for (int c = 0; c < TablePreviewDocument::c_maxColumns + 1; ++c) {
    wideRow.append(QStringLiteral("w"));
    alignments.append(PreviewTableAlignment::None);
  }

  QVector<QVector<QString>> wide;
  wide.append(wideRow);

  auto wideTable = makeTable(wide, alignments);
  QVERIFY(TablePreviewDocument::normalizedCellCount(*wideTable) <=
          TablePreviewDocument::c_maxCells);
  QVERIFY(!TablePreviewDocument::isWithinLimits(*wideTable));
  QVERIFY(!widget.setPreview(wideTable));
}

// ---------------------------------------------------------------------------
// Sheet geometry
// ---------------------------------------------------------------------------

void TestTablePreview::testHeightForWidthComesFromTheDocumentLayout() {
  TablePreviewWidget widget(nullptr, nullptr);
  QVERIFY(widget.setPreview(makeWrappingTable()));
  auto sheet = sheetOf(widget);
  QVERIFY(sheet);

  showOffScreen(widget, 600);
  settle();

  auto doc = sheet->document();
  const int chrome = sheet->width() - sheet->viewport()->width();
  QCOMPARE(qCeil(doc->documentLayout()->documentSize().height()) +
               (sheet->height() - sheet->viewport()->height()),
           widget.heightForWidth(widget.width()));

  // A narrower band wraps more, so it is taller. The bespoke row solver is
  // gone: this is Qt's own rich text layout answering.
  const int wide = widget.heightForWidth(600);
  const int narrow = widget.heightForWidth(300);
  QVERIFY2(narrow > wide, qPrintable(QStringLiteral("narrow %1 is not taller than wide %2")
                                         .arg(narrow)
                                         .arg(wide)));

  // Repeating a query may not drift, and asking for the other width must not
  // return the previous answer.
  QCOMPARE(widget.heightForWidth(600), wide);
  QCOMPARE(widget.heightForWidth(300), narrow);
  QCOMPARE(widget.heightForWidth(600), wide);
  Q_UNUSED(chrome);
}

void TestTablePreview::testHeightForWidthConvertsTheOuterWidth() {
  TablePreviewWidget widget(nullptr, nullptr);
  QVERIFY(widget.setPreview(makeWrappingTable()));
  auto sheet = sheetOf(widget);
  QVERIFY(sheet);

  showOffScreen(widget, 500);
  settle();

  // The outer width is the widget's; the document lays out inside the
  // viewport. Assigning the outer width straight to setTextWidth() would
  // measure a narrower band than the sheet gets, and with the scroll bars off
  // the content clipped by that mistake would be unreachable.
  const int chrome = sheet->width() - sheet->viewport()->width();
  widget.heightForWidth(widget.width());
  QCOMPARE(qRound(sheet->document()->textWidth()), sheet->width() - chrome);

  // Nothing is clipped: the whole document fits inside the height reported for
  // the width the sheet has.
  QVERIFY(sheet->verticalScrollBar());
  QCOMPARE(sheet->verticalScrollBar()->maximum(), 0);
  QCOMPARE(sheet->horizontalScrollBar()->maximum(), 0);
}

void TestTablePreview::testColumnsFillTheAssignedWidth() {
  TablePreviewWidget widget(nullptr, nullptr);
  QVERIFY(widget.setPreview(makeWrappingTable()));
  auto sheet = sheetOf(widget);
  QVERIFY(sheet);

  for (int width : {400, 700}) {
    showOffScreen(widget, width);
    settle();

    QTextTable *table = tableOf(sheet->document());
    QVERIFY(table);

    const QRectF rect = sheet->document()->documentLayout()->frameBoundingRect(table);
    const qreal available =
        sheet->document()->textWidth() - 2 * sheet->document()->documentMargin();
    QVERIFY2(qAbs(rect.width() - available) <= 2,
             qPrintable(QStringLiteral("the table is %1 wide inside %2 of band")
                            .arg(rect.width())
                            .arg(available)));

    // And every column really got a share of it.
    const QRect first = cellRect(sheet, 0, 0);
    const QRect second = cellRect(sheet, 0, 1);
    QVERIFY(first.isValid() && second.isValid());
    QVERIFY2(second.left() > first.left(),
             "the two columns were laid out on top of each other");
  }
}

void TestTablePreview::testSettledGeometryNotifiesTheHost() {
  TablePreviewWidget widget(nullptr, nullptr);
  QVERIFY(widget.setPreview(makeWrappingTable()));
  auto sheet = sheetOf(widget);
  QVERIFY(sheet);

  showOffScreen(widget, 500);
  settle();

  QSignalSpy spy(sheet, &TablePreviewSheet::preferredGeometryChanged);

  class LayoutRequestCounter : public QObject {
  public:
    bool eventFilter(QObject *p_object, QEvent *p_event) Q_DECL_OVERRIDE {
      if (p_event->type() == QEvent::LayoutRequest) {
        ++m_count;
      }

      return QObject::eventFilter(p_object, p_event);
    }

    int m_count = 0;
  } counter;
  widget.installEventFilter(&counter);

  // Pure geometry queries answer the host; they must never tell it that
  // something moved, or the measurement would feed itself.
  for (int i = 0; i < 3; ++i) {
    widget.sizeHint();
    widget.heightForWidth(widget.width());
    widget.heightForWidth(widget.width() / 2);
  }
  settle();
  QCOMPARE(spy.count(), 0);
  QCOMPARE(counter.m_count, 0);

  // A cell which grows enough to wrap changes the height the host reserved.
  typeInto(sheet, 1, 1,
           QStringLiteral(" and a good deal more text so the row has to grow another line or "
                          "two beyond what it needed before"));
  settle();

  QVERIFY2(spy.count() >= 1, "the sheet never told the host its geometry moved");
  // Coalesced: a burst of settlements must not queue a burst of host
  // measurements.
  QCOMPARE(counter.m_count, 1);

  // The settled signature is the baseline now, so nothing else is owed.
  const int settledCount = spy.count();
  settle();
  QCOMPARE(spy.count(), settledCount);
  QCOMPARE(counter.m_count, 1);
}

// ---------------------------------------------------------------------------
// Caret and navigation
// ---------------------------------------------------------------------------

void TestTablePreview::testClickPutsTheCaretUnderThePointer() {
  QScopedPointer<TablePreviewWidget> holder;
  auto widget = buildEditableSheet(holder);
  QVERIFY(widget);
  auto sheet = sheetOf(*widget);
  QVERIFY(sheet);

  const QRect cell = cellRect(sheet, 1, 1);
  QVERIFY(cell.isValid());

  // Near the left edge of the text rather than the middle of the cell, so a
  // caret which merely went to one end could not pass by accident.
  const QPoint pos(cell.left() + 2, cell.center().y());
  QTest::mouseClick(sheet->viewport(), Qt::LeftButton, Qt::NoModifier, pos);
  QCoreApplication::processEvents();

  const QTextCursor cursor = sheet->textCursor();
  QVERIFY2(cursor.currentTable(), "the click left the caret outside the table");
  const QTextTableCell hit = cursor.currentTable()->cellAt(cursor);
  QCOMPARE(hit.row(), 1);
  QCOMPARE(hit.column(), 1);

  // Nothing selected, and the caret is exactly where cursorForPosition() puts
  // it - the whole point of a rich text substrate.
  QVERIFY(cursor.selectedText().isEmpty());
  QCOMPARE(cursor.position(), sheet->cursorForPosition(pos).position());
  QVERIFY2(cursor.position() < hit.lastCursorPosition().position(),
           "the caret went to the end of the cell instead of the click");
}

void TestTablePreview::testClickAtTheEndPutsTheCaretAtTheEnd() {
  QScopedPointer<TablePreviewWidget> holder;
  auto widget = buildEditableSheet(holder);
  QVERIFY(widget);
  auto sheet = sheetOf(*widget);
  QVERIFY(sheet);

  const QRect cell = cellRect(sheet, 1, 1);
  QVERIFY(cell.isValid());

  // Clicking past the end of the text parks the caret after the last
  // character, the way a text editor does.
  QTest::mouseClick(sheet->viewport(), Qt::LeftButton, Qt::NoModifier,
                    QPoint(cell.right() + 1, cell.center().y()));
  QCoreApplication::processEvents();

  const QTextCursor cursor = sheet->textCursor();
  QVERIFY(cursor.currentTable());
  const QTextTableCell hit = cursor.currentTable()->cellAt(cursor);
  QCOMPARE(cursor.position(), hit.lastCursorPosition().position());
}

void TestTablePreview::testTypingNeedsNoEditMode() {
  QScopedPointer<TablePreviewWidget> holder;
  auto widget = buildEditableSheet(holder);
  QVERIFY(widget);
  auto sheet = sheetOf(*widget);
  QVERIFY(sheet);

  const QRect cell = cellRect(sheet, 1, 1);
  QTest::mouseClick(sheet->viewport(), Qt::LeftButton, Qt::NoModifier,
                    QPoint(cell.right() + 1, cell.center().y()));
  QCoreApplication::processEvents();

  // No editor is opened and no mode is entered: the click put the caret in the
  // cell and the next keystroke edits it.
  QTest::keyClicks(sheet, QStringLiteral("X"));
  QCOMPARE(cellText(sheet->document(), 1, 1), QStringLiteral("**bold**X"));
}

void TestTablePreview::testTheCaretNeverLeavesTheTable() {
  QScopedPointer<TablePreviewWidget> holder;
  auto widget = buildEditableSheet(holder);
  QVERIFY(widget);
  auto sheet = sheetOf(*widget);
  QVERIFY(sheet);

  // A QTextDocument always keeps a block after a table. Parking the caret
  // there would put it outside every cell, so it is clamped back in.
  QTextCursor tail(sheet->document());
  tail.movePosition(QTextCursor::End);
  sheet->setTextCursor(tail);

  QVERIFY2(sheet->textCursor().currentTable(),
           "the caret was left in the block after the table");
}

void TestTablePreview::testReadOnlySheetKeepsTheCaretButSwallowsTyping() {
  QScopedPointer<TablePreviewWidget> holder;
  auto widget = buildEditableSheet(holder);
  QVERIFY(widget);
  auto sheet = sheetOf(*widget);
  QVERIFY(sheet);

  widget->setReadOnly(true);
  QVERIFY(sheet->isReadOnly());

  const QRect cell = cellRect(sheet, 1, 1);
  QTest::mouseClick(sheet->viewport(), Qt::LeftButton, Qt::NoModifier, cell.center());
  QCoreApplication::processEvents();

  // Read-only still allows the caret, a selection and a copy - strictly more
  // than the retired NoEditTriggers did.
  QVERIFY2(sheet->textCursor().currentTable(), "a read-only sheet refused the caret");

  const QString before = cellText(sheet->document(), 1, 1);
  QTest::keyClicks(sheet, QStringLiteral("X"));
  QCOMPARE(cellText(sheet->document(), 1, 1), before);
}

void TestTablePreview::testTabWrapsBetweenCells() {
  QScopedPointer<TablePreviewWidget> holder;
  auto widget = buildEditableSheet(holder);
  QVERIFY(widget);
  auto sheet = sheetOf(*widget);
  QVERIFY(sheet);

  auto currentCell = [sheet]() {
    const QTextCursor cursor = sheet->textCursor();
    QTextTable *table = cursor.currentTable();
    return table ? qMakePair(table->cellAt(cursor).row(), table->cellAt(cursor).column())
                 : qMakePair(-1, -1);
  };

  putCaretIn(sheet, 0, 0);
  QTest::keyClick(sheet, Qt::Key_Tab);
  QCOMPARE(currentCell(), qMakePair(0, 1));

  QTest::keyClick(sheet, Qt::Key_Backtab);
  QCOMPARE(currentCell(), qMakePair(0, 0));

  // Shift-Tab out of the first cell wraps to the last one, and Tab out of the
  // last one wraps back.
  QTest::keyClick(sheet, Qt::Key_Backtab);
  QCOMPARE(currentCell(), qMakePair(1, 2));

  QTest::keyClick(sheet, Qt::Key_Tab);
  QCOMPARE(currentCell(), qMakePair(0, 0));

  // And Tab never reaches the focus chain: it was consumed before Qt's own
  // handling saw it, so the caret is still inside the table.
  QVERIFY(sheet->textCursor().currentTable());
}

void TestTablePreview::testArrowOutAtTheEdgesRequestsAFocusEscape() {
  QScopedPointer<TablePreviewWidget> holder;
  auto widget = buildEditableSheet(holder);
  QVERIFY(widget);
  auto sheet = sheetOf(*widget);
  QVERIFY(sheet);

  QSignalSpy spy(widget, &TablePreviewWidget::focusEscapeRequested);

  // Up out of the first row.
  putCaretIn(sheet, 0, 1, false);
  QTest::keyClick(sheet, Qt::Key_Up);
  QCOMPARE(spy.count(), 1);
  QCOMPARE(spy.last().at(0).value<FocusEscapeDirection>(), FocusEscapeDirection::Up);

  // Down out of the last row.
  putCaretIn(sheet, 1, 1);
  QTest::keyClick(sheet, Qt::Key_Down);
  QCOMPARE(spy.count(), 2);
  QCOMPARE(spy.last().at(0).value<FocusEscapeDirection>(), FocusEscapeDirection::Down);

  // Inside the table the arrows just move the caret.
  putCaretIn(sheet, 0, 1, false);
  QTest::keyClick(sheet, Qt::Key_Down);
  QCOMPARE(spy.count(), 2);
  QVERIFY(sheet->textCursor().currentTable());
}

void TestTablePreview::testEscapeRequestsAFocusEscape() {
  QScopedPointer<TablePreviewWidget> holder;
  auto widget = buildEditableSheet(holder);
  QVERIFY(widget);
  auto sheet = sheetOf(*widget);
  QVERIFY(sheet);

  QSignalSpy spy(widget, &TablePreviewWidget::focusEscapeRequested);

  putCaretIn(sheet, 1, 1);
  QTest::keyClick(sheet, Qt::Key_Escape);

  QCOMPARE(spy.count(), 1);
  // Keep: the editor takes the focus back and the caret stays where it was.
  QCOMPARE(spy.last().at(0).value<FocusEscapeDirection>(), FocusEscapeDirection::Keep);
}

// ---------------------------------------------------------------------------
// Content invariants
// ---------------------------------------------------------------------------

void TestTablePreview::testEnterIsSwallowed() {
  QScopedPointer<TablePreviewWidget> holder;
  auto widget = buildEditableSheet(holder);
  QVERIFY(widget);
  auto sheet = sheetOf(*widget);
  QVERIFY(sheet);

  const int blocks = sheet->document()->blockCount();
  putCaretIn(sheet, 1, 1);

  QTest::keyClick(sheet, Qt::Key_Return);
  QTest::keyClick(sheet, Qt::Key_Enter);

  // One cell is one line: a table row is a single source line, and the
  // serializer rejects every separator which could end it.
  QCOMPARE(sheet->document()->blockCount(), blocks);
  QCOMPARE(cellText(sheet->document(), 1, 1), QStringLiteral("**bold**"));
}

void TestTablePreview::testSelectAllCannotTakeTheTableApart() {
  QScopedPointer<TablePreviewWidget> holder;
  auto widget = buildEditableSheet(holder);
  QVERIFY(widget);
  auto sheet = sheetOf(*widget);
  QVERIFY(sheet);

  QTextTable *table = tableOf(sheet->document());
  QVERIFY(table);
  const int rows = table->rows();
  const int columns = table->columns();

  putCaretIn(sheet, 1, 1);
  sheet->selectAll();
  QCoreApplication::processEvents();

  // A selection which crosses a cell would take the whole table out of the
  // document on the very next keystroke, and the sheet would be left holding a
  // dangling QTextTable. It is confined to the caret's cell instead.
  const QTextCursor selection = sheet->textCursor();
  QVERIFY(selection.currentTable());
  const QTextTableCell anchorCell = table->cellAt(selection.anchor());
  const QTextTableCell caretCell = table->cellAt(selection.position());
  QVERIFY(anchorCell.isValid());
  QVERIFY(caretCell.isValid());
  QVERIFY(anchorCell == caretCell);

  QTest::keyClicks(sheet, QStringLiteral("X"));

  // The table is intact and only the one cell changed.
  QTextTable *after = tableOf(sheet->document());
  QVERIFY2(after == table, "the table was replaced or removed");
  QCOMPARE(after->rows(), rows);
  QCOMPARE(after->columns(), columns);
  QCOMPARE(cellText(sheet->document(), 1, 1), QStringLiteral("X"));
  QCOMPARE(cellText(sheet->document(), 1, 0), QStringLiteral("*italic*"));
  QCOMPARE(cellText(sheet->document(), 0, 0), QStringLiteral("Left"));
}

void TestTablePreview::testCutAndDeleteStayInsideOneCell() {
  QScopedPointer<TablePreviewWidget> holder;
  auto widget = buildEditableSheet(holder);
  QVERIFY(widget);
  auto sheet = sheetOf(*widget);
  QVERIFY(sheet);

  QTextTable *table = tableOf(sheet->document());
  QVERIFY(table);
  const int rows = table->rows();
  const int columns = table->columns();

  // Cut and the context menu's Delete never pass through keyPressEvent(), so
  // confining the selection itself is the only gate which covers them.
  putCaretIn(sheet, 1, 1);
  sheet->selectAll();
  sheet->cut();
  QCoreApplication::processEvents();

  QCOMPARE(tableOf(sheet->document()), table);
  QCOMPARE(table->rows(), rows);
  QCOMPARE(table->columns(), columns);
  QCOMPARE(cellText(sheet->document(), 1, 1), QString());
  QCOMPARE(cellText(sheet->document(), 1, 2), QStringLiteral("`code`"));

  putCaretIn(sheet, 1, 2);
  sheet->selectAll();
  QTest::keyClick(sheet, Qt::Key_Delete);
  QCoreApplication::processEvents();

  QCOMPARE(tableOf(sheet->document()), table);
  QCOMPARE(table->rows(), rows);
  QCOMPARE(table->columns(), columns);
  QCOMPARE(cellText(sheet->document(), 1, 2), QString());
  QCOMPARE(cellText(sheet->document(), 0, 0), QStringLiteral("Left"));
}

void TestTablePreview::testWordDeleteShortcutsStayInsideOneCell() {
  QScopedPointer<TablePreviewWidget> holder;
  auto widget = buildEditableSheet(holder);
  QVERIFY(widget);
  auto sheet = sheetOf(*widget);
  QVERIFY(sheet);

  QTextTable *table = tableOf(sheet->document());
  QVERIFY(table);
  const int rows = table->rows();
  const int columns = table->columns();

  // QWidgetTextControl answers these by building a selection *and* removing it
  // in one call, on its own cursor and with no signal in between - so the
  // clamps, which run either before the base handler or off a cursor signal,
  // never see the cross-cell range they can produce. Ctrl+Delete at the end of
  // the last cell reaches out of the table entirely, and QTextCursor then
  // expands the range to the whole frame.
  putCaretIn(sheet, 1, 2);
  QTest::keyClick(sheet, Qt::Key_Delete, Qt::ControlModifier);
  QCoreApplication::processEvents();

  QVERIFY2(tableOf(sheet->document()) == table, "Ctrl+Delete removed the table");
  QCOMPARE(table->rows(), rows);
  QCOMPARE(table->columns(), columns);
  QCOMPARE(cellText(sheet->document(), 1, 2), QStringLiteral("`code`"));

  // At the start of the first cell, Ctrl+Backspace reaches backwards out of
  // the table.
  QTextCursor toStart = sheet->textCursor();
  toStart.setPosition(table->cellAt(0, 0).firstPosition());
  sheet->setTextCursor(toStart);
  QTest::keyClick(sheet, Qt::Key_Backspace, Qt::ControlModifier);
  QCoreApplication::processEvents();

  QVERIFY2(tableOf(sheet->document()) == table, "Ctrl+Backspace removed the table");
  QCOMPARE(table->rows(), rows);
  QCOMPARE(table->columns(), columns);
  QCOMPARE(cellText(sheet->document(), 0, 0), QStringLiteral("Left"));

  // At a cell boundary, the delete stays in the cell the caret is in.
  putCaretIn(sheet, 1, 0);
  QTest::keyClick(sheet, Qt::Key_Delete, Qt::ControlModifier);
  QCoreApplication::processEvents();

  QCOMPARE(tableOf(sheet->document()), table);
  QCOMPARE(cellText(sheet->document(), 1, 1), QStringLiteral("**bold**"));

  // And inside a cell it still does its job.
  QTextCursor inside = sheet->textCursor();
  inside.setPosition(table->cellAt(1, 1).firstPosition());
  sheet->setTextCursor(inside);
  QTest::keyClick(sheet, Qt::Key_Delete, Qt::ControlModifier);
  QCoreApplication::processEvents();

  QCOMPARE(tableOf(sheet->document()), table);
  QVERIFY2(cellText(sheet->document(), 1, 1).size() < QStringLiteral("**bold**").size(),
           qPrintable(cellText(sheet->document(), 1, 1)));
  QCOMPARE(cellText(sheet->document(), 1, 2), QStringLiteral("`code`"));
}

void TestTablePreview::testPastedTextIsSanitized() {
  QScopedPointer<TablePreviewWidget> holder;
  auto widget = buildEditableSheet(holder);
  QVERIFY(widget);
  auto sheet = sheetOf(*widget);
  QVERIFY(sheet);

  putCaretIn(sheet, 1, 0, false);
  QTextCursor clear = sheet->textCursor();
  clear.select(QTextCursor::BlockUnderCursor);

  auto pasteInto = [&](const QString &p_payload) {
    putCaretIn(sheet, 1, 0);
    QTextCursor cursor = sheet->textCursor();
    cursor.setPosition(tableOf(sheet->document())->cellAt(1, 0).firstCursorPosition().position(),
                       QTextCursor::KeepAnchor);
    cursor.removeSelectedText();
    sheet->setTextCursor(cursor);

    auto data = new QMimeData();
    data->setText(p_payload);
    QApplication::clipboard()->setMimeData(data);
    sheet->paste();
    QCoreApplication::processEvents();
    return cellText(sheet->document(), 1, 0);
  };

  // A multi-line paste lands as one line, and one run of separators becomes
  // one space.
  QCOMPARE(pasteInto(QStringLiteral("one\ntwo")), QStringLiteral("one two"));
  QCOMPARE(pasteInto(QStringLiteral("one\r\ntwo")), QStringLiteral("one two"));
  QCOMPARE(pasteInto(QStringLiteral("one\n\n\ntwo")), QStringLiteral("one two"));

  // Including the separators QTextCursor::selectedText() itself produces.
  QCOMPARE(pasteInto(QStringLiteral("one") + QChar(0x2028) + QStringLiteral("two")),
           QStringLiteral("one two"));
  QCOMPARE(pasteInto(QStringLiteral("one") + QChar(0x2029) + QStringLiteral("two")),
           QStringLiteral("one two"));

  // A whole pasted table is one line too, so it can still be written back.
  const QString pastedTable =
      QStringLiteral("| a | b |\n| --- | --- |\n| c | d |");
  const QString landed = pasteInto(pastedTable);
  QVERIFY2(!landed.contains(QLatin1Char('\n')), qPrintable(landed));
  QVERIFY2(!landed.contains(QChar(0x2029)), qPrintable(landed));
  // One cell, one block: the paste added no structure of its own.
  QTextTable *table = tableOf(sheet->document());
  QVERIFY(table);
  QCOMPARE(table->rows(), 2);
  QCOMPARE(table->columns(), 3);
  QCOMPARE(table->cellAt(1, 0).lastCursorPosition().block().blockNumber(),
           table->cellAt(1, 0).firstCursorPosition().block().blockNumber());
}

void TestTablePreview::testPastedRichTextIsFlattened() {
  QScopedPointer<TablePreviewWidget> holder;
  auto widget = buildEditableSheet(holder);
  QVERIFY(widget);
  auto sheet = sheetOf(*widget);
  QVERIFY(sheet);

  putCaretIn(sheet, 1, 0);
  QTextCursor cursor = sheet->textCursor();
  cursor.setPosition(tableOf(sheet->document())->cellAt(1, 0).firstCursorPosition().position(),
                     QTextCursor::KeepAnchor);
  cursor.removeSelectedText();
  sheet->setTextCursor(cursor);

  auto data = new QMimeData();
  data->setHtml(QStringLiteral("<p><b>one</b></p><p>two</p>"));
  data->setText(QStringLiteral("one\ntwo"));
  QApplication::clipboard()->setMimeData(data);
  sheet->paste();
  QCoreApplication::processEvents();

  // The plain alternative is taken, sanitized, and no structure a table row
  // cannot express reaches the document.
  QCOMPARE(cellText(sheet->document(), 1, 0), QStringLiteral("one two"));
  QCOMPARE(tableOf(sheet->document())->rows(), 2);
}

void TestTablePreview::testCommittedImeSeparatorsAreSanitized() {
  QScopedPointer<TablePreviewWidget> holder;
  auto widget = buildEditableSheet(holder);
  QVERIFY(widget);
  auto sheet = sheetOf(*widget);
  QVERIFY(sheet);

  QTextTable *table = tableOf(sheet->document());
  QVERIFY(table);
  const int blocks = sheet->document()->blockCount();

  putCaretIn(sheet, 1, 0);

  // An input method commits whatever it composed, which is not filtered by
  // keyPressEvent() and not a MIME payload either.
  QInputMethodEvent event;
  event.setCommitString(QStringLiteral("one\ntwo") + QChar(0x2029) +
                        QStringLiteral("three"));
  QCoreApplication::sendEvent(sheet, &event);
  QCoreApplication::processEvents();

  QCOMPARE(cellText(sheet->document(), 1, 0),
           QStringLiteral("*italic*one two three"));
  QCOMPARE(sheet->document()->blockCount(), blocks);
  QCOMPARE(tableOf(sheet->document()), table);
}

void TestTablePreview::testImeReplacementRangesStayInsideOneCell() {
  QScopedPointer<TablePreviewWidget> holder;
  auto widget = buildEditableSheet(holder);
  QVERIFY(widget);
  auto sheet = sheetOf(*widget);
  QVERIFY(sheet);

  QTextTable *table = tableOf(sheet->document());
  QVERIFY(table);
  const int rows = table->rows();
  const int columns = table->columns();

  auto send = [sheet](const QString &p_commit, int p_start, int p_length,
                      const QList<QInputMethodEvent::Attribute> &p_attributes =
                          QList<QInputMethodEvent::Attribute>()) {
    QInputMethodEvent event(QString(), p_attributes);
    event.setCommitString(p_commit, p_start, p_length);
    QCoreApplication::sendEvent(sheet, &event);
    QCoreApplication::processEvents();
  };

  auto tableIsIntact = [&]() {
    return tableOf(sheet->document()) == table && table->rows() == rows &&
           table->columns() == columns;
  };

  auto selectionIsInsideOneCell = [&]() {
    const QTextCursor cursor = sheet->textCursor();
    if (!cursor.hasSelection()) {
      return cursor.currentTable() == table;
    }

    const QTextTableCell anchorCell = table->cellAt(cursor.anchor());
    const QTextTableCell caretCell = table->cellAt(cursor.position());
    return anchorCell.isValid() && caretCell.isValid() && anchorCell == caretCell;
  };

  // An input method resolves its replacement range from the cursor and not
  // from the selection, so the clamp on the selection alone does not bound it.
  // Reaching across the frame boundary is what would delete the table itself.

  // An empty commit string with a replacement length is a deletion, not a
  // preedit.
  putCaretIn(sheet, 1, 1);
  send(QString(), -200, 400);
  QVERIFY2(tableIsIntact(), "an empty-commit deletion took the table apart");
  // Confined to the cell, so it emptied that one and nothing else.
  QCOMPARE(cellText(sheet->document(), 1, 1), QString());
  QCOMPARE(cellText(sheet->document(), 1, 0), QStringLiteral("*italic*"));
  QCOMPARE(cellText(sheet->document(), 1, 2), QStringLiteral("`code`"));
  QCOMPARE(cellText(sheet->document(), 0, 0), QStringLiteral("Left"));

  // A replacement start far before the cell.
  putCaretIn(sheet, 1, 2);
  send(QStringLiteral("Z"), -500, 0);
  QVERIFY2(tableIsIntact(), "a negative replacement start took the table apart");
  QCOMPARE(cellText(sheet->document(), 1, 2), QStringLiteral("Z`code`"));
  QCOMPARE(cellText(sheet->document(), 0, 2), QStringLiteral("Right"));

  // A replacement length far past the end of the cell.
  putCaretIn(sheet, 0, 0);
  QTextCursor toStart = sheet->textCursor();
  toStart.setPosition(table->cellAt(0, 0).firstPosition());
  sheet->setTextCursor(toStart);
  send(QStringLiteral("H"), 0, 500);
  QVERIFY2(tableIsIntact(), "an overlong replacement length took the table apart");
  QCOMPARE(cellText(sheet->document(), 0, 0), QStringLiteral("H"));
  QCOMPARE(cellText(sheet->document(), 0, 1), QStringLiteral("Center"));

  // And a Selection attribute never reaches the base handler at all: it is
  // resolved after the selection has been removed and the commit applied, so
  // no pre-flight check could bound it, and it moves only the anchor - which
  // emits neither cursorPositionChanged() nor selectionChanged().
  putCaretIn(sheet, 1, 0);
  QList<QInputMethodEvent::Attribute> attributes;
  attributes.append(
      QInputMethodEvent::Attribute(QInputMethodEvent::Selection, -400, 800, QVariant()));
  send(QStringLiteral("Y"), 0, 0, attributes);
  QVERIFY2(tableIsIntact(), "an input method selection took the table apart");
  QCOMPARE(cellText(sheet->document(), 1, 0), QStringLiteral("*italic*Y"));
  QVERIFY2(sheet->textCursor().currentTable() == table,
           "an input method selection moved the caret out of the table");
  QVERIFY2(selectionIsInsideOneCell(), "an input method selection crossed a cell");

  // Including on an event which carries no commit and no replacement at all,
  // which is otherwise a pure preedit.
  putCaretIn(sheet, 1, 1);
  send(QString(), 0, 0, attributes);
  QVERIFY2(tableIsIntact(), "a selection-only input method event took the table apart");
  QVERIFY2(selectionIsInsideOneCell(), "a selection-only input method event crossed a cell");

  // A replacement range is resolved against the cursor the base handler is
  // left with *after* it removes the current selection, so clamping against
  // the caret alone would not bound it: with the whole cell selected and the
  // caret at its high end, a negative start would still reach out of it.
  putCaretIn(sheet, 1, 2);
  QTextCursor whole = sheet->textCursor();
  whole.setPosition(table->cellAt(1, 2).firstPosition());
  whole.setPosition(table->cellAt(1, 2).lastPosition(), QTextCursor::KeepAnchor);
  sheet->setTextCursor(whole);
  QVERIFY(sheet->textCursor().hasSelection());
  send(QStringLiteral("W"), -300, 0);
  QVERIFY2(tableIsIntact(), "a replacement over a selection took the table apart");
  QCOMPARE(cellText(sheet->document(), 1, 2), QStringLiteral("W"));
  // Its neighbours are exactly what the earlier cases left them: the first
  // case emptied (1, 1), and the Selection case appended to (1, 0).
  QCOMPARE(cellText(sheet->document(), 1, 1), QString());
  QCOMPARE(cellText(sheet->document(), 1, 0), QStringLiteral("*italic*Y"));
  QCOMPARE(cellText(sheet->document(), 0, 2), QStringLiteral("Right"));
}

void TestTablePreview::testAReadOnlySheetRefusesImeMutations() {
  QScopedPointer<TablePreviewWidget> holder;
  auto widget = buildEditableSheet(holder);
  QVERIFY(widget);
  auto sheet = sheetOf(*widget);
  QVERIFY(sheet);

  QTextTable *table = tableOf(sheet->document());
  QVERIFY(table);
  const int rows = table->rows();
  const int columns = table->columns();
  const QString before = sheet->document()->toPlainText();

  widget->setReadOnly(true);
  QVERIFY(sheet->isReadOnly());

  // Read-only is not enforced on this path by Qt itself: a read-only QTextEdit
  // keeps Qt::TextSelectableByMouse, and QWidgetTextControl accepts an input
  // method event for a selectable control - and installing a changed preedit
  // removes the current selection before anything else, so stripping the
  // commit alone would still delete cell text.
  const QTextTableCell cell = table->cellAt(1, 1);
  QTextCursor selection = sheet->textCursor();
  selection.setPosition(cell.firstPosition());
  selection.setPosition(cell.lastPosition(), QTextCursor::KeepAnchor);
  sheet->setTextCursor(selection);
  QVERIFY(sheet->textCursor().hasSelection());

  QList<QInputMethodEvent::Attribute> attributes;
  attributes.append(
      QInputMethodEvent::Attribute(QInputMethodEvent::Selection, -400, 800, QVariant()));

  // A commit with an oversized replacement range, plus a cross-cell selection.
  QInputMethodEvent oversized(QStringLiteral("preedit"), attributes);
  oversized.setCommitString(QStringLiteral("X"), -500, 900);
  QCoreApplication::sendEvent(sheet, &oversized);
  QCoreApplication::processEvents();

  QCOMPARE(tableOf(sheet->document()), table);
  QCOMPARE(table->rows(), rows);
  QCOMPARE(table->columns(), columns);
  QCOMPARE(sheet->document()->toPlainText(), before);
  QVERIFY2(sheet->textCursor().hasSelection(), "a read-only sheet lost its selection");

  // And a pure preedit, which is the shape whose selection removal is easiest
  // to miss because it carries no commit at all.
  QInputMethodEvent preedit(QStringLiteral("another preedit"),
                            QList<QInputMethodEvent::Attribute>());
  QCoreApplication::sendEvent(sheet, &preedit);
  QCoreApplication::processEvents();

  QCOMPARE(sheet->document()->toPlainText(), before);
  QVERIFY2(sheet->textCursor().hasSelection(), "a read-only preedit deleted the selection");
  QCOMPARE(cellText(sheet->document(), 1, 1), QStringLiteral("**bold**"));

  // And nothing was left behind for the writable sheet to act on either.
  widget->setReadOnly(false);
  QVERIFY(!sheet->isReadOnly());

  sheet->cut();
  QCoreApplication::processEvents();
  QCOMPARE(tableOf(sheet->document()), table);
  QCOMPARE(table->rows(), rows);
  QCOMPARE(table->columns(), columns);
  QCOMPARE(cellText(sheet->document(), 0, 0), QStringLiteral("Left"));
}

namespace {
// What the text control currently renders as an uncommitted composition.
QString preeditOf(const QTextEdit *p_edit) {
  const QTextBlock block = p_edit->textCursor().block();
  return block.isValid() && block.layout() ? block.layout()->preeditAreaText() : QString();
}

// A real, exposed and activated window, which is what it takes for a widget to
// become the application's focus object - and therefore the receiver
// QInputMethod acts on.
bool giveInputFocus(QWidget *p_window, QWidget *p_target) {
  p_window->show();
  if (!QTest::qWaitForWindowExposed(p_window)) {
    return false;
  }

  p_window->activateWindow();
  p_target->setFocus();
  for (int i = 0; i < 50 && QGuiApplication::focusObject() != p_target; ++i) {
    QTest::qWait(10);
    QCoreApplication::processEvents();
  }

  return QGuiApplication::focusObject() == p_target;
}
} // namespace

void TestTablePreview::testBecomingAViewerCancelsTheComposition() {
  QVector<QVector<QString>> cells;
  cells.append({QStringLiteral("Left"), QStringLiteral("Center")});
  cells.append({QStringLiteral("*italic*"), QStringLiteral("**bold**")});

  TablePreviewWidget widget(nullptr, nullptr);
  QVERIFY(widget.setPreview(
      makeTable(cells, {PreviewTableAlignment::None, PreviewTableAlignment::None})));
  widget.resize(600, 200);

  auto sheet = sheetOf(widget);
  QVERIFY(sheet);

  if (!giveInputFocus(&widget, sheet)) {
    QSKIP("the platform did not give the sheet the input focus");
  }

  putCaretIn(sheet, 1, 1);
  const QString before = sheet->document()->toPlainText();

  QInputMethodEvent composing(QStringLiteral("composing"),
                              QList<QInputMethodEvent::Attribute>());
  QCoreApplication::sendEvent(sheet, &composing);
  QCoreApplication::processEvents();
  QCOMPARE(preeditOf(sheet), QStringLiteral("composing"));

  // A viewer refuses every input method event, so the composition has to be
  // cancelled on the way in rather than left rendered over it forever.
  widget.setReadOnly(true);
  QVERIFY(sheet->isReadOnly());
  QCOMPARE(preeditOf(sheet), QString());
  // Cancelling is not deleting: nothing committed, nothing lost.
  QCOMPARE(sheet->document()->toPlainText(), before);
  QVERIFY(sheet->textCursor().currentTable());

  // And a fresh composition works once the sheet is writable again.
  widget.setReadOnly(false);
  QVERIFY(!sheet->isReadOnly());

  QInputMethodEvent again(QStringLiteral("again"), QList<QInputMethodEvent::Attribute>());
  QCoreApplication::sendEvent(sheet, &again);
  QCoreApplication::processEvents();
  QCOMPARE(preeditOf(sheet), QStringLiteral("again"));

  QInputMethodEvent commit;
  commit.setCommitString(QStringLiteral("!"));
  QCoreApplication::sendEvent(sheet, &commit);
  QCoreApplication::processEvents();
  QCOMPARE(cellText(sheet->document(), 1, 1), QStringLiteral("**bold**!"));
}

void TestTablePreview::testABackgroundSheetLeavesTheFocusedCompositionAlone() {
  QVector<QVector<QString>> cells;
  cells.append({QStringLiteral("Left"), QStringLiteral("Center")});
  cells.append({QStringLiteral("*italic*"), QStringLiteral("**bold**")});

  // Never shown, so it certainly does not own the input focus.
  TablePreviewWidget widget(nullptr, nullptr);
  QVERIFY(widget.setPreview(
      makeTable(cells, {PreviewTableAlignment::None, PreviewTableAlignment::None})));
  QVERIFY(sheetOf(widget));

  // Another widget owns the focus and is composing in it.
  QTextEdit other;
  other.resize(300, 100);
  other.setPlainText(QStringLiteral("elsewhere"));
  if (!giveInputFocus(&other, &other)) {
    QSKIP("the platform did not give the other widget the input focus");
  }

  QInputMethodEvent composing(QStringLiteral("composing"),
                              QList<QInputMethodEvent::Attribute>());
  QCoreApplication::sendEvent(&other, &composing);
  QCoreApplication::processEvents();
  QCOMPARE(preeditOf(&other), QStringLiteral("composing"));

  // QInputMethod has no receiver - it acts on the application's focus object -
  // so a background sheet becoming a viewer must not reach for it. A sheet is
  // revoked on every preview rebuild, so this is not a rare shape.
  //
  // Necessary rather than sufficient: a synthetic preedit does not put the
  // platform input context into a composing state, so an unscoped reset()
  // would have nothing to cancel here either. What this pins down is that the
  // transition leaves another widget's state alone.
  widget.setReadOnly(true);
  QCoreApplication::processEvents();

  QCOMPARE(preeditOf(&other), QStringLiteral("composing"));
  QCOMPARE(other.toPlainText(), QStringLiteral("elsewhere"));

  // The same applies to a flush, which reaches the input method through
  // commitPreedit(): the host flushes every sheet it removes, and most of
  // those are not the focused one.
  widget.setReadOnly(false);
  widget.flushNow();
  widget.revokeAuthority();
  QCoreApplication::processEvents();

  QCOMPARE(preeditOf(&other), QStringLiteral("composing"));
  QCOMPARE(other.toPlainText(), QStringLiteral("elsewhere"));
}

void TestTablePreview::testASeparatorOnlyImeCommitKeepsTheSelection() {
  QScopedPointer<TablePreviewWidget> holder;
  auto widget = buildEditableSheet(holder);
  QVERIFY(widget);
  auto sheet = sheetOf(*widget);
  QVERIFY(sheet);

  QTextTable *table = tableOf(sheet->document());
  QVERIFY(table);

  // Select the whole cell, then commit a composition which is nothing but line
  // separators. A cell cannot hold one, so the payload is refused - and
  // refusing it must not delete what was selected, exactly as refusing the
  // same paste does not.
  const QTextTableCell cell = table->cellAt(1, 1);
  QTextCursor selection = sheet->textCursor();
  selection.setPosition(cell.firstPosition());
  selection.setPosition(cell.lastPosition(), QTextCursor::KeepAnchor);
  sheet->setTextCursor(selection);
  QVERIFY(sheet->textCursor().hasSelection());

  QInputMethodEvent event;
  event.setCommitString(QStringLiteral("\n\r\n") + QChar(0x2029));
  QCoreApplication::sendEvent(sheet, &event);
  QCoreApplication::processEvents();

  QCOMPARE(cellText(sheet->document(), 1, 1), QStringLiteral("**bold**"));
  QVERIFY2(sheet->textCursor().hasSelection(), "the refused commit dropped the selection");
  QCOMPARE(tableOf(sheet->document()), table);
}

// The host calls this when the focus goes back to the text editor: the
// selection has to collapse onto the caret, without the caret moving and
// without the document changing.
void TestTablePreview::testClearSelectionCollapsesOntoTheCaret() {
  QScopedPointer<TablePreviewWidget> holder;
  auto widget = buildEditableSheet(holder);
  QVERIFY(widget);
  auto sheet = sheetOf(*widget);
  QVERIFY(sheet);

  QTextTable *table = tableOf(sheet->document());
  QVERIFY(table);

  const QTextTableCell cell = table->cellAt(1, 1);
  QTextCursor selection = sheet->textCursor();
  selection.setPosition(cell.firstPosition());
  selection.setPosition(cell.lastPosition(), QTextCursor::KeepAnchor);
  sheet->setTextCursor(selection);
  QVERIFY(sheet->textCursor().hasSelection());

  const int caret = sheet->textCursor().position();
  const QString before = sheet->document()->toPlainText();

  widget->clearSelection();

  QVERIFY2(!sheet->textCursor().hasSelection(), "the selection survived");
  QCOMPARE(sheet->textCursor().position(), caret);
  QCOMPARE(sheet->document()->toPlainText(), before);
  QCOMPARE(tableOf(sheet->document()), table);
}

void TestTablePreview::testClearSelectionIsANoOpWithoutASelection() {
  QScopedPointer<TablePreviewWidget> holder;
  auto widget = buildEditableSheet(holder);
  QVERIFY(widget);
  auto sheet = sheetOf(*widget);
  QVERIFY(sheet);

  putCaretIn(sheet, 1, 0);
  const int caret = sheet->textCursor().position();
  const QString before = sheet->document()->toPlainText();

  widget->clearSelection();

  QVERIFY(!sheet->textCursor().hasSelection());
  QCOMPARE(sheet->textCursor().position(), caret);
  QCOMPARE(sheet->document()->toPlainText(), before);
}

void TestTablePreview::testAPurelySeparatorPayloadIsRefused() {
  QScopedPointer<TablePreviewWidget> holder;
  auto widget = buildEditableSheet(holder);
  QVERIFY(widget);
  auto sheet = sheetOf(*widget);
  QVERIFY(sheet);

  putCaretIn(sheet, 1, 0);
  const QString before = cellText(sheet->document(), 1, 0);

  auto data = new QMimeData();
  data->setText(QStringLiteral("\n\r\n") + QChar(0x2029));
  QApplication::clipboard()->setMimeData(data);
  sheet->paste();
  QCoreApplication::processEvents();

  // Nothing a cell could keep, so nothing is inserted - not even the space a
  // sanitized run would have become.
  QCOMPARE(cellText(sheet->document(), 1, 0), before);
}

void TestTablePreview::testDroppedTextGoesThroughTheSameValidator() {
  QScopedPointer<TablePreviewWidget> holder;
  auto widget = buildEditableSheet(holder);
  QVERIFY(widget);
  auto sheet = sheetOf(*widget);
  QVERIFY(sheet);

  putCaretIn(sheet, 1, 0);
  QTextCursor cursor = sheet->textCursor();
  cursor.setPosition(tableOf(sheet->document())->cellAt(1, 0).firstCursorPosition().position(),
                     QTextCursor::KeepAnchor);
  cursor.removeSelectedText();
  sheet->setTextCursor(cursor);

  const QRect cell = cellRect(sheet, 1, 0);
  const QPointF pos = QPointF(cell.center());

  QMimeData data;
  data.setText(QStringLiteral("one\ntwo"));

  QDragEnterEvent enter(pos.toPoint(), Qt::CopyAction, &data, Qt::LeftButton, Qt::NoModifier);
  QCoreApplication::sendEvent(sheet->viewport(), &enter);
  QDragMoveEvent move(pos.toPoint(), Qt::CopyAction, &data, Qt::LeftButton, Qt::NoModifier);
  QCoreApplication::sendEvent(sheet->viewport(), &move);
  QDropEvent drop(pos, Qt::CopyAction, &data, Qt::LeftButton, Qt::NoModifier);
  QCoreApplication::sendEvent(sheet->viewport(), &drop);
  QCoreApplication::processEvents();

  // A drop routes through the very same validator as a paste, so it cannot
  // smuggle a separator past it either.
  QVERIFY2(!cellText(sheet->document(), 1, 0).contains(QLatin1Char('\n')),
           qPrintable(cellText(sheet->document(), 1, 0)));
  QCOMPARE(tableOf(sheet->document())->rows(), 2);
}

// ---------------------------------------------------------------------------
// Commit machine
// ---------------------------------------------------------------------------

void TestTablePreview::testDebouncedCommit() {
  SheetHarness harness(makeCommittableTable());
  auto sheet = harness.sheet();
  QVERIFY(sheet);

  typeInto(sheet, 1, 0, QStringLiteral("1"));
  typeInto(sheet, 1, 0, QStringLiteral("2"));
  typeInto(sheet, 1, 0, QStringLiteral("3"));

  // Still inside the idle window: a burst of keystrokes must not become a
  // burst of document replacements.
  QTest::qWait(TablePreviewWidget::c_commitDebounceMs / 4);
  QCoreApplication::processEvents();
  QCOMPARE(harness.requestCount(), 0);

  waitForCommit();
  QCOMPARE(harness.requestCount(), 1);
  QVERIFY2(harness.lastRequest().contains(QStringLiteral("a123")),
           qPrintable(harness.lastRequest()));

  // And nothing further is owed.
  waitForCommit();
  QCOMPARE(harness.requestCount(), 1);
}

void TestTablePreview::testCellLeaveFlushesImmediately() {
  SheetHarness harness(makeCommittableTable());
  auto sheet = harness.sheet();
  QVERIFY(sheet);

  typeInto(sheet, 1, 0, QStringLiteral("!"));
  QCOMPARE(harness.requestCount(), 0);

  // Leaving the cell is a finished edit, so it does not wait for the idle
  // window.
  putCaretIn(sheet, 1, 1);
  QCoreApplication::processEvents();
  QCOMPARE(harness.requestCount(), 1);
  QVERIFY(harness.lastRequest().contains(QStringLiteral("a!")));
}

void TestTablePreview::testFocusOutFlushesImmediately() {
  SheetHarness harness(makeCommittableTable());
  auto sheet = harness.sheet();
  QVERIFY(sheet);

  showOffScreen(*harness.widget(), 600);
  sheet->setFocus();
  typeInto(sheet, 1, 0, QStringLiteral("?"));
  QCOMPARE(harness.requestCount(), 0);

  QFocusEvent out(QEvent::FocusOut);
  QCoreApplication::sendEvent(sheet, &out);
  QCoreApplication::processEvents();

  QCOMPARE(harness.requestCount(), 1);
  QVERIFY(harness.lastRequest().contains(QStringLiteral("a?")));
}

void TestTablePreview::testEscapeFlushesImmediately() {
  SheetHarness harness(makeCommittableTable());
  auto sheet = harness.sheet();
  QVERIFY(sheet);

  showOffScreen(*harness.widget(), 600);
  typeInto(sheet, 1, 0, QStringLiteral("*"));
  QCOMPARE(harness.requestCount(), 0);

  QTest::keyClick(sheet, Qt::Key_Escape);
  QCoreApplication::processEvents();

  QCOMPARE(harness.requestCount(), 1);
  QVERIFY(harness.lastRequest().contains(QStringLiteral("a*")));
}

void TestTablePreview::testEchoOfACommitKeepsANewerEdit() {
  SheetHarness harness(makeCommittableTable());
  auto sheet = harness.sheet();
  QVERIFY(sheet);

  // Commit A.
  typeInto(sheet, 1, 0, QStringLiteral("A"));
  waitForCommit();
  QCOMPARE(harness.requestCount(), 1);
  const QString committedA = harness.lastRequest();

  // Type B into the same cell before A's parse echo arrives.
  typeInto(sheet, 1, 0, QStringLiteral("B"));
  const int caret = sheet->textCursor().position();
  QCOMPARE(cellText(sheet->document(), 1, 0), QStringLiteral("aAB"));

  // The echo of A. It matches neither the bound source nor what the document
  // would serialize to right now, so without the committed-Markdown baseline
  // the sheet would rebuild from A and destroy both B and the caret.
  auto echo = parseCanonical(committedA, 1);
  QVERIFY(echo);
  QVERIFY(harness.deliver(echo));

  QCOMPARE(cellText(sheet->document(), 1, 0), QStringLiteral("aAB"));
  QCOMPARE(sheet->textCursor().position(), caret);

  // And B is not stranded: the debounce is re-armed, so A+B is committed next.
  waitForCommit();
  QCOMPARE(harness.requestCount(), 2);
  QVERIFY2(harness.lastRequest().contains(QStringLiteral("aAB")),
           qPrintable(harness.lastRequest()));
}

void TestTablePreview::testARevertToThePreCommitSourceIsHonoured() {
  SheetHarness harness(makeCommittableTable());
  auto sheet = harness.sheet();
  QVERIFY(sheet);

  const QString original = harness.boundSource();
  QVERIFY(!original.isEmpty());

  // Commit an edit, the way the undo relay does before it hands the editor its
  // undo.
  typeInto(sheet, 1, 0, QStringLiteral("A"));
  waitForCommit();
  QCOMPARE(harness.requestCount(), 1);
  QCOMPARE(cellText(sheet->document(), 1, 0), QStringLiteral("aA"));

  // The editor then undoes it, so the next parse generation delivers exactly
  // the pre-commit source again. The sheet's cached snapshot must not still be
  // describing that same source, or this authoritative revert looks like an
  // unchanged snapshot and the sheet keeps rendering the committed table over
  // a document which no longer holds it.
  auto reverted = parseCanonical(original, 1);
  QVERIFY(reverted);
  QVERIFY(harness.deliver(reverted));

  QCOMPARE(cellText(sheet->document(), 1, 0), QStringLiteral("a"));

  // And the sheet is settled: nothing is owed, so it does not immediately
  // write the reverted edit back.
  waitForCommit();
  QCOMPARE(harness.requestCount(), 1);
}

void TestTablePreview::testRejectionMakesTheSheetReadOnly() {
  SheetHarness harness(makeCommittableTable());
  auto sheet = harness.sheet();
  QVERIFY(sheet);

  harness.setNextStatus(PreviewReplacementResult::SourceMismatch);
  typeInto(sheet, 1, 0, QStringLiteral("X"));
  waitForCommit();

  QCOMPARE(harness.requestCount(), 1);
  // External source always wins: the sheet stops presenting itself as the
  // truth and waits for an authoritative snapshot, rather than restoring stale
  // values over it.
  QVERIFY(sheet->isReadOnly());
  QCOMPARE(cellText(sheet->document(), 1, 0), QStringLiteral("aX"));

  // The next snapshot restores it.
  harness.setNextStatus(PreviewReplacementResult::Accepted);
  QVector<QVector<QString>> cells;
  cells.append({QStringLiteral("h1"), QStringLiteral("h2")});
  cells.append({QStringLiteral("z"), QStringLiteral("b")});
  QVERIFY(harness.deliver(
      makeSnapshot(cells, {PreviewTableAlignment::None, PreviewTableAlignment::None})));
  QVERIFY(!sheet->isReadOnly());
  QCOMPARE(cellText(sheet->document(), 1, 0), QStringLiteral("z"));
}

void TestTablePreview::testAnUntouchedDocumentDiscardsTheEdit() {
  SheetHarness harness(makeCommittableTable());
  auto sheet = harness.sheet();
  QVERIFY(sheet);

  harness.setNextStatus(PreviewReplacementResult::ParseFailure);
  typeInto(sheet, 1, 0, QStringLiteral("X"));
  waitForCommit();

  QCOMPARE(harness.requestCount(), 1);
  // The document was not touched, so the bound snapshot is still the source of
  // truth and the edit is discarded rather than left on screen.
  QCOMPARE(cellText(sheet->document(), 1, 0), QStringLiteral("a"));
  QVERIFY(!sheet->isReadOnly());
}

void TestTablePreview::testUndoFlushesBeforeItReachesTheEditor() {
  SheetHarness harness(makeCommittableTable());
  auto sheet = harness.sheet();
  QVERIFY(sheet);
  showOffScreen(*harness.widget(), 600);

  QSignalSpy undoSpy(harness.widget(), &TablePreviewWidget::undoRequested);

  // Pressed inside the idle window. Forwarding straight through would undo an
  // unrelated earlier operation and the pending table edit would then still be
  // committed on top of whatever the undo restored.
  typeInto(sheet, 1, 0, QStringLiteral("U"));
  QCOMPARE(harness.requestCount(), 0);

  QTest::keyClick(sheet, Qt::Key_Z, Qt::ControlModifier);
  QCoreApplication::processEvents();

  QCOMPARE(harness.requestCount(), 1);
  QCOMPARE(undoSpy.count(), 1);

  // Pressed after the debounce has already fired, nothing is owed and the undo
  // goes straight through.
  typeInto(sheet, 1, 1, QStringLiteral("V"));
  waitForCommit();
  QCOMPARE(harness.requestCount(), 2);

  QTest::keyClick(sheet, Qt::Key_Z, Qt::ControlModifier);
  QCoreApplication::processEvents();
  QCOMPARE(harness.requestCount(), 2);
  QCOMPARE(undoSpy.count(), 2);
}

void TestTablePreview::testRedoIsDroppedWhileDirty() {
  SheetHarness harness(makeCommittableTable());
  auto sheet = harness.sheet();
  QVERIFY(sheet);
  showOffScreen(*harness.widget(), 600);

  QSignalSpy redoSpy(harness.widget(), &TablePreviewWidget::redoRequested);

  // Ctrl+Shift+Z rather than Ctrl+Y: both are bound to QKeySequence::Redo on
  // Windows and on X11, but only this one is bound on every platform.
  const Qt::KeyboardModifiers redo = Qt::ControlModifier | Qt::ShiftModifier;

  // Nothing pending: the redo is the editor's.
  QTest::keyClick(sheet, Qt::Key_Z, redo);
  QCoreApplication::processEvents();
  QCOMPARE(redoSpy.count(), 1);

  // A pending edit necessarily clears the editor's redo stack when it is
  // flushed, so there is nothing left to redo afterwards.
  typeInto(sheet, 1, 0, QStringLiteral("R"));
  QTest::keyClick(sheet, Qt::Key_Z, redo);
  QCoreApplication::processEvents();

  QCOMPARE(harness.requestCount(), 1);
  QCOMPARE(redoSpy.count(), 1);
}

void TestTablePreview::testCommittedImeInputReachesTheFlush() {
  SheetHarness harness(makeCommittableTable());
  auto sheet = harness.sheet();
  QVERIFY(sheet);
  showOffScreen(*harness.widget(), 600);

  putCaretIn(sheet, 1, 0);

  QInputMethodEvent event;
  event.setCommitString(QStringLiteral("\u3042"));
  QCoreApplication::sendEvent(sheet, &event);
  QCoreApplication::processEvents();

  QCOMPARE(cellText(sheet->document(), 1, 0), QStringLiteral("a\u3042"));

  waitForCommit();
  QCOMPARE(harness.requestCount(), 1);
  QVERIFY2(harness.lastRequest().contains(QStringLiteral("a\u3042")),
           qPrintable(harness.lastRequest()));
}

void TestTablePreview::testAnActiveCompositionSurvivesTheRemovalFlush() {
  SheetHarness harness(makeCommittableTable());
  auto sheet = harness.sheet();
  QVERIFY(sheet);
  showOffScreen(*harness.widget(), 600);
  sheet->setFocus();

  putCaretIn(sheet, 1, 0);

  // A preedit is not in the document yet, so it has not advanced the edit
  // generation either. That is why the flush commits the composition *before*
  // it tests the generation: testing first would report a composing sheet as
  // clean, and the host's pre-removal protocol would revoke its authority and
  // lose it.
  //
  // Whether QInputMethod::commit() actually produces the commit event is a
  // platform input context concern, and the offscreen platform has none, so
  // what is exercised here is the rest of the path: a composition which has
  // reached the document through the platform's own commit event is still
  // owed a write-back at removal time, and the flush issues it.
  QList<QInputMethodEvent::Attribute> attributes;
  attributes.append(QInputMethodEvent::Attribute(QInputMethodEvent::Cursor, 2, 1, QVariant()));
  QInputMethodEvent preedit(QStringLiteral("\u3042\u3044"), attributes);
  QCoreApplication::sendEvent(sheet, &preedit);
  QCoreApplication::processEvents();
  QCOMPARE(harness.requestCount(), 0);

  QInputMethodEvent commit;
  commit.setCommitString(QStringLiteral("\u3042\u3044"));
  QCoreApplication::sendEvent(sheet, &commit);
  // Still inside the idle window.
  QCOMPARE(harness.requestCount(), 0);

  harness.widget()->flushNow();
  harness.widget()->revokeAuthority();

  QCOMPARE(harness.requestCount(), 1);
  QVERIFY2(harness.lastRequest().contains(QStringLiteral("a\u3042\u3044")),
           qPrintable(harness.lastRequest()));
}


void TestTablePreview::testRevokedAuthoritySilencesEveryCommit() {
  SheetHarness harness(makeCommittableTable());
  auto sheet = harness.sheet();
  QVERIFY(sheet);
  showOffScreen(*harness.widget(), 600);
  sheet->setFocus();

  typeInto(sheet, 1, 0, QStringLiteral("Q"));
  QCOMPARE(harness.requestCount(), 0);

  // The host is about to drop the identity: it flushes first, and everything
  // afterwards - a late timeout, the hide, the focus-out, the deferred
  // deletion - has to be a no-op, because the context and the anchor are no
  // longer authoritative.
  harness.widget()->flushNow();
  QCOMPARE(harness.requestCount(), 1);

  harness.widget()->revokeAuthority();
  QVERIFY(sheet->isReadOnly());

  typeInto(sheet, 1, 1, QStringLiteral("Z"));
  harness.widget()->hide();
  QFocusEvent out(QEvent::FocusOut);
  QCoreApplication::sendEvent(sheet, &out);
  waitForCommit();

  QCOMPARE(harness.requestCount(), 1);
}

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------

void TestTablePreview::testDarkPaletteReachesTheSheet() {
  QScopedPointer<TablePreviewWidget> holder;
  auto widget = buildEditableSheet(holder);
  QVERIFY(widget);
  auto sheet = sheetOf(*widget);
  QVERIFY(sheet);

  QPalette dark;
  dark.setColor(QPalette::Text, QColor(230, 230, 230));
  dark.setColor(QPalette::Base, QColor(30, 30, 30));
  dark.setColor(QPalette::Mid, QColor(90, 90, 90));
  dark.setColor(QPalette::Highlight, QColor(0, 90, 180));
  dark.setColor(QPalette::HighlightedText, QColor(255, 255, 255));
  widget->setPalette(dark);
  settle();

  // The host propagates only the editor font, so every colour has to be
  // resolved from the sheet's own effective palette.
  QCOMPARE(sheet->palette().color(QPalette::Text), QColor(230, 230, 230));
  QCOMPARE(sheet->palette().color(QPalette::Highlight), QColor(0, 90, 180));
  QCOMPARE(sheet->palette().color(QPalette::HighlightedText), QColor(255, 255, 255));

  // The band belongs to the editor: a sheet which filled its own background
  // would paint an opaque rectangle over whatever the editor draws there.
  QCOMPARE(sheet->palette().color(QPalette::Base).alpha(), 0);
  QVERIFY(!sheet->viewport()->autoFillBackground());

  // And the one colour the table itself owns follows too.
  QTextTable *table = tableOf(sheet->document());
  QVERIFY(table);
  QCOMPARE(table->format().toTableFormat().borderBrush().color(), QColor(90, 90, 90));

  // A later theme change still reaches the sheet: applying a palette must not
  // pin it to the one it was given.
  QPalette light = dark;
  light.setColor(QPalette::Text, QColor(20, 20, 20));
  light.setColor(QPalette::Mid, QColor(200, 200, 200));
  widget->setPalette(light);
  settle();

  QCOMPARE(sheet->palette().color(QPalette::Text), QColor(20, 20, 20));
  QCOMPARE(table->format().toTableFormat().borderBrush().color(), QColor(200, 200, 200));
}

QTEST_MAIN(tests::TestTablePreview)
