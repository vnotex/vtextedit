#include "test_interactivepreview.h"

#include <algorithm>
#include <limits>

#include <QAbstractTextDocumentLayout>
#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QEventLoop>
#include <QFocusEvent>
#include <QMenu>
#include <QPointer>
#include <QScrollBar>
#include <QSignalSpy>
#include <QSizePolicy>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextFrame>
#include <QTextTable>
#include <QTextTableCell>
#include <QTimer>
#include <QVBoxLayout>

#include <inputmode/abstractinputmode.h>
#include <texteditor/inputmodestatuswidget.h>
#include <vtextedit/markdowneditorconfig.h>
#include <vtextedit/markdownhighlighter.h>
#include <vtextedit/texteditorconfig.h>
#include <vtextedit/theme.h>
#include <vtextedit/vmarkdowneditor.h>
#include <vtextedit/vtextedit.h>

using namespace tests;
using namespace vte;

// ---------------------------------------------------------------------------
// Test doubles
// ---------------------------------------------------------------------------

RecordingPreviewWidget::RecordingPreviewWidget(PreviewWidgetContext *p_context, QWidget *p_parent,
                                               const QVector<PreviewElementType> &p_types,
                                               const QSize &p_hint)
    : PreviewWidget(p_context, p_parent), m_types(p_types), m_hint(p_hint) {
  if (p_context) {
    connect(p_context, &PreviewWidgetContext::replacementFinished, this,
            &RecordingPreviewWidget::handleReplacementFinished);
    connect(p_context, &PreviewWidgetContext::geometryContextChanged, this,
            &RecordingPreviewWidget::handleGeometryContextChanged);
  }
}

QVector<PreviewElementType> RecordingPreviewWidget::supportedTypes() const { return m_types; }

bool RecordingPreviewWidget::setPreview(const QSharedPointer<const vte::Preview> &p_preview) {
  if (m_refuseNextSetPreview) {
    m_refuseNextSetPreview = false;
    return false;
  }

  if (!p_preview || !m_types.contains(p_preview->type())) {
    return false;
  }

  m_preview = p_preview;
  ++m_setPreviewCount;

  if (m_selfDestructOnUpdate && m_setPreviewCount > 1) {
    // The host must survive a widget destroying itself from inside its own
    // callback and still return a coherent result.
    delete this;
    return true;
  }

  if (m_spinOnNextSetPreview) {
    // A renderer opening a modal dialog runs a real nested event loop here,
    // which keeps delivering the host's own queued timers while this callback
    // is still on the stack.
    m_spinOnNextSetPreview = false;
    ++m_spinCount;
    if (m_deliveryCounterSource) {
      m_deliveriesBeforeSpin =
          m_deliveryCounterSource->property("vte_preview_reconcile_deliveries").toInt();
    }

    if (m_foldCounterSource) {
      m_foldRefreshesBeforeSpin =
          m_foldCounterSource->property("vte_preview_fold_refreshes").toInt();
    }

    QEventLoop loop;
    QTimer::singleShot(30, this, [this]() {
      if (m_duringSpin) {
        m_duringSpin();
      }
    });
    // Bounded, so a failing assertion cannot hang the suite.
    QTimer::singleShot(60, &loop, [&loop]() { loop.quit(); });
    loop.exec();

    if (m_deliveryCounterSource) {
      m_deliveriesAfterSpin =
          m_deliveryCounterSource->property("vte_preview_reconcile_deliveries").toInt();
    }
  }

  return true;
}

QSize RecordingPreviewWidget::sizeHint() const {
  ++m_sizeHintCount;
  return m_hint;
}

qreal RecordingPreviewWidget::preferredWidthFraction() const { return m_widthFraction; }

bool RecordingPreviewWidget::hasHeightForWidth() const { return m_wrapping; }

int RecordingPreviewWidget::heightForWidth(int p_width) const {
  if (!m_wrapping || p_width <= 0) {
    return m_hint.height();
  }

  // A fixed area: the narrower the widget, the taller it has to be.
  const int area = m_hint.width() * m_hint.height();
  return qMax(1, area / p_width);
}

void RecordingPreviewWidget::handleReplacementFinished(const PreviewReplacementResult &p_result) {
  m_lastResult = p_result;
  m_results.append(p_result.status());
  ++m_resultCount;

  if (m_spinOnNextReplacementFinished) {
    m_spinOnNextReplacementFinished = false;
    if (m_duringReplacementSpin) {
      m_duringReplacementSpin();
    }
  }
}

void RecordingPreviewWidget::clearSelection() {
  ++m_clearSelectionCount;

  if (m_requestOnClearSelection.isEmpty()) {
    return;
  }

  // Application code reached from inside the host's focus handling. The
  // request must not touch the document from here.
  const QString markdown = m_requestOnClearSelection;
  m_requestOnClearSelection.clear();
  if (auto context = previewContext()) {
    context->requestSourceReplacement(markdown);
  }

  if (m_documentSource) {
    m_documentDuringClearSelection = m_documentSource();
  }
}

void RecordingPreviewWidget::handleGeometryContextChanged() {
  ++m_geometryContextCount;

  if (m_requestOnGeometryContext.isEmpty()) {
    return;
  }

  // Application code reached from inside the host's geometry application. The
  // request must not touch the document from here.
  const QString markdown = m_requestOnGeometryContext;
  m_requestOnGeometryContext.clear();
  if (auto context = previewContext()) {
    context->requestSourceReplacement(markdown);
  }
}

RecordingPreviewFactory::RecordingPreviewFactory(const QVector<PreviewElementType> &p_types,
                                                 QObject *p_parent)
    : PreviewWidgetFactory(p_parent), m_types(p_types) {}

QVector<PreviewElementType> RecordingPreviewFactory::supportedTypes() const {
  auto self = const_cast<RecordingPreviewFactory *>(this);
  ++self->m_supportedTypesCount;
  if (m_unregisterSelfIn) {
    auto editor = m_unregisterSelfIn;
    self->m_unregisterSelfIn = nullptr;
    editor->unregisterPreviewWidgetFactory(self);
  }

  if (m_registerReentrantlyInSupportedTypes) {
    auto editor = m_registerReentrantlyInSupportedTypes;
    self->m_registerReentrantlyInSupportedTypes = nullptr;
    auto nested = new RecordingPreviewFactory(m_types);
    self->m_reentrantRegistrationAccepted = editor->registerPreviewWidgetFactory(nested, 20);
    if (!self->m_reentrantRegistrationAccepted) {
      delete nested;
    }
  }

  return m_types;
}

PreviewWidget *RecordingPreviewFactory::createWidget(PreviewWidgetContext *p_context,
                                                     const QSharedPointer<const Preview> &p_preview,
                                                     QWidget *p_parent) {
  Q_UNUSED(p_preview);
  ++m_createCount;
  if (m_createCountSink) {
    ++(*m_createCountSink);
  }

  if (m_registerReentrantlyIn) {
    auto editor = m_registerReentrantlyIn;
    m_registerReentrantlyIn = nullptr;
    auto nested = new RecordingPreviewFactory(m_types);
    m_reentrantRegistrationAccepted = editor->registerPreviewWidgetFactory(nested, 20);
    if (!m_reentrantRegistrationAccepted) {
      delete nested;
    }
  }

  if (m_decline) {
    return nullptr;
  }

  auto types = m_refuseSetPreview ? QVector<PreviewElementType>() : m_types;
  auto widget = new RecordingPreviewWidget(p_context, p_parent, types, m_hint);
  widget->m_wrapping = m_wrapping;
  widget->m_widthFraction = m_widthFraction;
  if (m_wrapping) {
    QSizePolicy policy = widget->sizePolicy();
    policy.setHeightForWidth(true);
    widget->setSizePolicy(policy);
  }
  m_widgets.append(widget);
  return widget;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {
const char *c_table = "| h1 | h2 |\n| --- | --- |\n| a | b |\n";

QSharedPointer<MarkdownEditorConfig> makeConfig() {
  auto textConfig = QSharedPointer<TextEditorConfig>::create();
  auto config = QSharedPointer<MarkdownEditorConfig>::create(textConfig);
  // The table sheet can rewrite the document, so it is opt-in.
  config->m_inplacePreviewSources |= MarkdownEditorConfig::Table;
  return config;
}

// Drive one full parse generation and let the host settle.
void settle(VMarkdownEditor &p_editor) {
  auto highlighter = p_editor.getHighlighter();
  QSignalSpy completed(highlighter, &MarkdownHighlighter::highlightCompleted);
  highlighter->updateHighlight();
  QTRY_VERIFY(completed.count() > 0);
  QTest::qWait(30);
  QCoreApplication::processEvents();
}

void setTextAndSettle(VMarkdownEditor &p_editor, const QString &p_text) {
  // Shown, deliberately.
  //
  // Interactive previews are realized on VIEWPORT DEMAND: a hidden editor - a
  // background tab, or one never shown - keeps its elements bound (they own
  // their bands and still fold their source) but builds no widgets, because
  // building hundreds of sheets nobody can see is the whole cost this design
  // exists to avoid. Every test below that inspects a live preview widget
  // therefore needs an editor with a real viewport.
  //
  // Guarded rather than unconditional so a test which sized and exposed the
  // editor itself keeps the geometry it chose.
  if (!p_editor.isVisible()) {
    p_editor.resize(600, 400);
    p_editor.show();
    QTest::qWaitForWindowExposed(&p_editor);
  }

  p_editor.setText(p_text);
  settle(p_editor);
}

QList<PreviewWidget *> previewWidgets(VMarkdownEditor &p_editor) {
  return p_editor.getTextEdit()->viewport()->findChildren<PreviewWidget *>(
      QString(), Qt::FindDirectChildrenOnly);
}

PreviewWidget *singlePreviewWidget(VMarkdownEditor &p_editor) {
  const auto widgets = previewWidgets(p_editor);
  return widgets.size() == 1 ? widgets.first() : nullptr;
}

// The host is an internal QObject child, reachable only by its object name.
QObject *previewHost(VMarkdownEditor &p_editor) {
  return p_editor.findChild<QObject *>(QStringLiteral("vte_interactive_preview_host"));
}

int reconcileDeliveries(VMarkdownEditor &p_editor) {
  auto host = previewHost(p_editor);
  return host ? host->property("vte_preview_reconcile_deliveries").toInt() : -1;
}

// ---------------------------------------------------------------------------
// The built-in table sheet, reached through the public widget API only
// ---------------------------------------------------------------------------

// The sheet is a QTextEdit hosting one QTextTable. This target deliberately
// cannot reach the internal header, so everything below is expressed through
// Qt's own rich text API.

// TablePreviewWidget::c_commitDebounceMs: the idle window after the last
// keystroke before a sheet writes itself back.
const int c_commitDebounceMs = 400;

QTextEdit *sheetView(PreviewWidget *p_widget) {
  return p_widget ? p_widget->findChild<QTextEdit *>() : nullptr;
}

QTextTable *sheetTable(QTextEdit *p_sheet) {
  if (!p_sheet || !p_sheet->document() || !p_sheet->document()->rootFrame()) {
    return nullptr;
  }

  for (auto frame : p_sheet->document()->rootFrame()->childFrames()) {
    if (auto table = qobject_cast<QTextTable *>(frame)) {
      return table;
    }
  }

  return nullptr;
}

// The raw Markdown one cell holds.
QString sheetCell(QTextEdit *p_sheet, int p_row, int p_column) {
  QTextTable *table = sheetTable(p_sheet);
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

// The laid out content width of every column, taken from the block each header
// cell holds. Qt's table layout owns the widths now, so this is the only place
// they exist.
QVector<qreal> columnWidths(QTextEdit *p_sheet) {
  QVector<qreal> widths;
  QTextTable *table = sheetTable(p_sheet);
  if (!table) {
    return widths;
  }

  auto layout = p_sheet->document()->documentLayout();
  for (int c = 0; c < table->columns(); ++c) {
    const QTextBlock block = table->cellAt(0, c).firstCursorPosition().block();
    widths.append(layout->blockBoundingRect(block).width());
  }

  return widths;
}

// The height of every row, i.e. of its tallest cell.
QVector<qreal> rowHeights(QTextEdit *p_sheet) {
  QVector<qreal> heights;
  QTextTable *table = sheetTable(p_sheet);
  if (!table) {
    return heights;
  }

  auto layout = p_sheet->document()->documentLayout();
  for (int r = 0; r < table->rows(); ++r) {
    qreal height = 0;
    for (int c = 0; c < table->columns(); ++c) {
      const QTextBlock block = table->cellAt(r, c).firstCursorPosition().block();
      height = qMax(height, layout->blockBoundingRect(block).height());
    }

    heights.append(height);
  }

  return heights;
}

qreal totalRowHeight(QTextEdit *p_sheet) {
  qreal total = 0;
  for (qreal height : rowHeights(p_sheet)) {
    total += height;
  }

  return total;
}

// The width the whole table was laid out at.
qreal tableWidth(QTextEdit *p_sheet) {
  QTextTable *table = sheetTable(p_sheet);
  return table ? p_sheet->document()->documentLayout()->frameBoundingRect(table).width() : 0;
}

// The rectangle a cell occupies in viewport coordinates.
QRect sheetCellRect(QTextEdit *p_sheet, int p_row, int p_column) {
  QTextTable *table = sheetTable(p_sheet);
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

// Right-click one cell and trigger a table action by object name.
//
// The sheet shows its menu with exec(), which spins its own event loop, so the
// action is triggered from a timer running inside it. Object names are the
// only handle this target has: it deliberately cannot include the sheet's
// header, and the labels are translated.
bool triggerTableAction(QTextEdit *p_sheet, int p_row, int p_column, const char *p_name) {
  const QRect rect = sheetCellRect(p_sheet, p_row, p_column);
  if (!rect.isValid()) {
    return false;
  }

  bool triggered = false;
  int attempts = 0;
  QTimer timer;
  timer.setInterval(5);
  QObject::connect(&timer, &QTimer::timeout, [&]() {
    auto popup = qobject_cast<QMenu *>(QApplication::activePopupWidget());
    if (!popup) {
      if (++attempts > 400) {
        timer.stop();
      }

      return;
    }

    timer.stop();
    if (QAction *action = popup->findChild<QAction *>(QString::fromLatin1(p_name))) {
      triggered = action->isEnabled();
      if (triggered) {
        action->trigger();
      }
    }

    popup->close();
  });
  timer.start();

  const QPoint pos = rect.center();
  QContextMenuEvent event(QContextMenuEvent::Mouse, pos, p_sheet->viewport()->mapToGlobal(pos));
  // To the viewport, which is where a real right click lands.
  QCoreApplication::sendEvent(p_sheet->viewport(), &event);
  timer.stop();
  QCoreApplication::processEvents();
  return triggered;
}

void putCaretIn(QTextEdit *p_sheet, int p_row, int p_column) {
  QTextTable *table = sheetTable(p_sheet);
  if (!table) {
    return;
  }

  p_sheet->setTextCursor(table->cellAt(p_row, p_column).lastCursorPosition());
}

// Replace a cell's whole contents the way selecting it and typing does.
void editCell(QTextEdit *p_sheet, int p_row, int p_column, const QString &p_text) {
  QTextTable *table = sheetTable(p_sheet);
  if (!table) {
    return;
  }

  const QTextTableCell cell = table->cellAt(p_row, p_column);
  QTextCursor cursor = cell.firstCursorPosition();
  cursor.setPosition(cell.lastCursorPosition().position(), QTextCursor::KeepAnchor);
  p_sheet->setTextCursor(cursor);
  cursor.insertText(p_text);
}

// Write the pending edit back now. The sheet debounces on a 400 ms idle timer,
// and losing the focus is one of the triggers which flushes it immediately.
void flushSheet(QTextEdit *p_sheet) {
  QFocusEvent out(QEvent::FocusOut);
  QCoreApplication::sendEvent(p_sheet, &out);
  QCoreApplication::processEvents();
}

// Collects the warnings Qt emits while it is installed, so a test can assert
// that a particular diagnostic was *not* produced.
class WarningRecorder {
public:
  WarningRecorder() {
    Q_ASSERT(!s_active);
    s_active = this;
    m_previous = qInstallMessageHandler(&WarningRecorder::handle);
  }

  ~WarningRecorder() {
    qInstallMessageHandler(m_previous);
    s_active = nullptr;
  }

  bool contains(const QString &p_needle) const {
    for (const auto &message : m_messages) {
      if (message.contains(p_needle)) {
        return true;
      }
    }

    return false;
  }

  QStringList m_messages;

private:
  static void handle(QtMsgType p_type, const QMessageLogContext &p_context,
                     const QString &p_message) {
    if (s_active) {
      s_active->m_messages.append(p_message);
      if (s_active->m_previous) {
        s_active->m_previous(p_type, p_context, p_message);
      }
    }
  }

  QtMessageHandler m_previous = nullptr;

  static WarningRecorder *s_active;
};

WarningRecorder *WarningRecorder::s_active = nullptr;
} // namespace

// ---------------------------------------------------------------------------
// Built-in renderer
// ---------------------------------------------------------------------------

void TestInteractivePreview::testBuiltinTableWidgetCreated() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  setTextAndSettle(editor, QLatin1String(c_table));

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  QCOMPARE(widget->supportedTypes(), QVector<PreviewElementType>{PreviewElementType::Table});

  auto context = widget->previewContext();
  QVERIFY(context);
  QVERIFY(context->preview());
  QCOMPARE(context->preview()->type(), PreviewElementType::Table);
  QCOMPARE(context->preview()->startPos(), 0);
}

namespace {
// A table whose two columns have visibly different widths, so the compact and
// the aligned write-back cannot be confused for one another.
const char *c_raggedTable = "| h1 | header2 |\n| --- | --- |\n| a | b |\n";

const char *c_compactCommit = "| h1 | header2 |\n| --- | --- |\n| z | b |";

const char *c_alignedCommit = "| h1  | header2 |\n| --- | ------- |\n| z   | b       |";

// Type @p_text into the first body cell of the single sheet and write it back
// now, then return what the editor's document holds.
QString commitFirstCell(VMarkdownEditor &p_editor, const QString &p_text) {
  auto sheet = sheetView(singlePreviewWidget(p_editor));
  if (!sheet) {
    return QString();
  }

  editCell(sheet, 1, 0, p_text);
  flushSheet(sheet);
  settle(p_editor);
  return p_editor.document()->toPlainText();
}
} // namespace

// MarkdownEditorConfig::m_alignTableSourceEnabled reaches the sheet through the
// host and the built-in factory, and only ever changes what a SUBSEQUENT commit
// writes.
void TestInteractivePreview::testTableSourceAlignOptionThreading() {
  // 1. The default config commits the compact form.
  {
    VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
    setTextAndSettle(editor, QLatin1String(c_raggedTable));
    QVERIFY2(commitFirstCell(editor, QStringLiteral("z")).contains(QLatin1String(c_compactCommit)),
             qPrintable(editor.document()->toPlainText()));
  }

  // 2. A config with the option on up front commits the padded form.
  {
    auto config = makeConfig();
    config->m_alignTableSourceEnabled = true;
    VMarkdownEditor editor(config, QSharedPointer<TextEditorParameters>::create());
    setTextAndSettle(editor, QLatin1String(c_raggedTable));
    QVERIFY2(commitFirstCell(editor, QStringLiteral("z")).contains(QLatin1String(c_alignedCommit)),
             qPrintable(editor.document()->toPlainText()));
  }

  // 3-6. Flipping it on a live sheet.
  auto config = makeConfig();
  VMarkdownEditor editor(config, QSharedPointer<TextEditorParameters>::create());
  setTextAndSettle(editor, QLatin1String(c_raggedTable));

  const QString before = editor.document()->toPlainText();
  config->m_alignTableSourceEnabled = true;
  editor.setConfig(config);
  settle(editor);

  // 3. Existing source is never reformatted on its own.
  QCOMPARE(editor.document()->toPlainText(), before);

  // Nor is it by an edit which cancels out: the commit path asks whether
  // anything really changed in the shape its baseline was recorded in, so a
  // cell re-typed with the value it had still settles without a commit rather
  // than reformatting the table behind the user's back.
  {
    auto sheet = sheetView(singlePreviewWidget(editor));
    QVERIFY(sheet);
    editCell(sheet, 1, 0, QStringLiteral("aa"));
    editCell(sheet, 1, 0, QStringLiteral("a"));
    flushSheet(sheet);
    settle(editor);
    QCOMPARE(editor.document()->toPlainText(), before);
  }

  // And the same holds for the other ordering, which the flip-time state
  // cannot see: the edit is already pending, and only cancels out, when the
  // option changes underneath it.
  {
    config->m_alignTableSourceEnabled = false;
    editor.setConfig(config);
    settle(editor);
    QCOMPARE(editor.document()->toPlainText(), before);

    auto sheet = sheetView(singlePreviewWidget(editor));
    QVERIFY(sheet);
    editCell(sheet, 1, 0, QStringLiteral("aa"));
    editCell(sheet, 1, 0, QStringLiteral("a"));

    // Still inside the debounce: the sheet is dirty by generation, settled by
    // content.
    config->m_alignTableSourceEnabled = true;
    editor.setConfig(config);
    settle(editor);

    auto rebound = sheetView(singlePreviewWidget(editor));
    QVERIFY(rebound);
    flushSheet(rebound);
    settle(editor);
    QCOMPARE(editor.document()->toPlainText(), before);
  }

  // 4. The next edit on that very sheet commits the padded form.
  QVERIFY2(commitFirstCell(editor, QStringLiteral("z")).contains(QLatin1String(c_alignedCommit)),
           qPrintable(editor.document()->toPlainText()));

  // 5. A sheet created after the change inherits the setting.
  setTextAndSettle(editor, QLatin1String(c_raggedTable));
  QVERIFY2(commitFirstCell(editor, QStringLiteral("z")).contains(QLatin1String(c_alignedCommit)),
           qPrintable(editor.document()->toPlainText()));

  // 6. And flipping it back affects subsequent commits only - on the very same
  // live sheet, without replacing the editor's contents first.
  config->m_alignTableSourceEnabled = false;
  editor.setConfig(config);
  settle(editor);
  QVERIFY2(editor.document()->toPlainText().contains(QLatin1String(c_alignedCommit)),
           "turning the option off rewrote source by itself");

  const QString compactAfterFlipBack = QStringLiteral("| h1 | header2 |\n| --- | --- |\n| y | b |");
  QVERIFY2(commitFirstCell(editor, QStringLiteral("y")).contains(compactAfterFlipBack),
           qPrintable(editor.document()->toPlainText()));
}

// The host validates a write-back by RE-PARSING it, so the padded source has to
// be understood by the production cmark/AST path exactly as the compact one is:
// same cells, same alignments, same block prefixes. A sheet which still binds
// and still shows the same cells after the commit is that round trip.
void TestInteractivePreview::testAlignedCommitSurvivesTheRealParser() {
  auto config = makeConfig();
  config->m_alignTableSourceEnabled = true;
  VMarkdownEditor editor(config, QSharedPointer<TextEditorParameters>::create());

  // Every alignment, an escaped pipe, a CJK cell measured two columns per
  // character, and a blockquote prefix on every row.
  const QString source = QStringLiteral("> | left | center | right | \u4E2D\u6587 |\n"
                                        "> | :--- | :---: | ---: | --- |\n"
                                        "> | a | b | c\\|d | e |\n");
  setTextAndSettle(editor, source);

  auto sheet = sheetView(singlePreviewWidget(editor));
  QVERIFY(sheet);
  // A cell holds RAW Markdown, so the escape is part of it and the serializer's
  // escaping is idempotent over it.
  QCOMPARE(sheetCell(sheet, 1, 2), QStringLiteral("c\\|d"));

  editCell(sheet, 1, 0, QStringLiteral("a much wider value"));
  flushSheet(sheet);
  settle(editor);

  const QString written = editor.document()->toPlainText();
  QVERIFY2(written.contains(QStringLiteral("> | a much wider value |   b    |  c\\|d | e    |")),
           qPrintable(written));
  // The delimiter row grew with the columns and kept its markers at the edges.
  QVERIFY2(written.contains(QStringLiteral("> | :----------------- | :----: | ----: | ---- |")),
           qPrintable(written));

  // The re-parse accepted it: a sheet is bound again, over the same cells and
  // with the blockquote prefix still carried.
  auto rebound = sheetView(singlePreviewWidget(editor));
  QVERIFY(rebound);
  QCOMPARE(sheetCell(rebound, 0, 0), QStringLiteral("left"));
  QCOMPARE(sheetCell(rebound, 1, 0), QStringLiteral("a much wider value"));
  QCOMPARE(sheetCell(rebound, 1, 2), QStringLiteral("c\\|d"));
  QCOMPARE(sheetCell(rebound, 0, 3), QStringLiteral("\u4E2D\u6587"));

  // And a second commit is stable rather than growing the padding again.
  const QString beforeSecond = editor.document()->toPlainText();
  editCell(rebound, 1, 1, QStringLiteral("b"));
  flushSheet(rebound);
  settle(editor);
  QCOMPARE(editor.document()->toPlainText(), beforeSecond);
}

namespace {
// The char format of the character at cell-local offset @p_offset.
QTextCharFormat sheetFormatAt(QTextEdit *p_sheet, int p_row, int p_column, int p_offset) {
  QTextTable *table = sheetTable(p_sheet);
  if (!table) {
    return QTextCharFormat();
  }

  QTextCursor cursor(p_sheet->document());
  cursor.setPosition(table->cellAt(p_row, p_column).firstCursorPosition().position() + p_offset);
  cursor.setPosition(cursor.position() + 1, QTextCursor::KeepAnchor);
  return cursor.charFormat();
}

const char *c_styledTable = "| h1 | h2 | h3 |\n| --- | --- | --- |\n| **a** | b | c |\n";
} // namespace

void TestInteractivePreview::testCellsCarrySyntaxHighlighting() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  setTextAndSettle(editor, QLatin1String(c_styledTable));

  auto sheet = sheetView(singlePreviewWidget(editor));
  QVERIFY(sheet);

  // The cell keeps its literal raw Markdown...
  QCOMPARE(sheetCell(sheet, 1, 0), QStringLiteral("**a**"));

  // ... and is painted with the editor's own style for it, which a plain cell
  // of the very same row does not carry.
  const QTextCharFormat styled = sheetFormatAt(sheet, 1, 0, 0);
  const QTextCharFormat plain = sheetFormatAt(sheet, 1, 1, 0);
  QVERIFY2(styled != plain, "the emphasized cell is painted like a plain one");
}

void TestInteractivePreview::testHighlightingSurvivesACommit() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.resize(600, 400);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  setTextAndSettle(editor, QLatin1String(c_styledTable));

  auto sheet = sheetView(singlePreviewWidget(editor));
  QVERIFY(sheet);

  // The reference: a cell which stays plain for the whole test.
  const QTextCharFormat plainBefore = sheetFormatAt(sheet, 1, 2, 0);

  editCell(sheet, 1, 1, QStringLiteral("`x`"));
  // Still inside the debounce: nothing has been written back yet.
  QVERIFY(!editor.document()->toPlainText().contains(QStringLiteral("`x`")));

  flushSheet(sheet);
  QTest::qWait(c_commitDebounceMs + 200);
  settle(editor);

  // The round trip is unaffected.
  QVERIFY2(editor.document()->toPlainText().contains(QStringLiteral("`x`")),
           qPrintable(editor.document()->toPlainText()));

  auto rebound = sheetView(singlePreviewWidget(editor));
  QVERIFY(rebound);
  QCOMPARE(sheetCell(rebound, 1, 1), QStringLiteral("`x`"));

  // The full-parse snapshot which follows the run-less rebase brings the runs
  // back: the committed code span is painted differently from a plain cell,
  // and the untouched emphasized cell kept its own style.
  const QTextCharFormat code = sheetFormatAt(rebound, 1, 1, 0);
  const QTextCharFormat plainAfter = sheetFormatAt(rebound, 1, 2, 0);
  const QTextCharFormat emphasized = sheetFormatAt(rebound, 1, 0, 0);
  QCOMPARE(plainAfter, plainBefore);
  QVERIFY2(code != plainAfter, "the committed code cell was not highlighted");
  QVERIFY2(emphasized != plainAfter, "the untouched cell lost its highlighting");
}

void TestInteractivePreview::testHighlightingStopsAtTheRunEnd() {
  // A styled span in the middle of a cell must not bleed into the text which
  // follows it.
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  setTextAndSettle(editor, QStringLiteral("| h1 | h2 |\n| --- | --- |\n"
                                          "| `c` | lead **bold** tail |\n"));

  auto sheet = sheetView(singlePreviewWidget(editor));
  QVERIFY(sheet);

  const QString text = QStringLiteral("lead **bold** tail");
  QCOMPARE(sheetCell(sheet, 1, 1), text);

  const QTextCharFormat lead = sheetFormatAt(sheet, 1, 1, 0);
  const QTextCharFormat strong = sheetFormatAt(sheet, 1, 1, 5);
  const QTextCharFormat tail = sheetFormatAt(sheet, 1, 1, text.size() - 1);

  QVERIFY2(strong != lead, "the emphasized span is not highlighted");
  QVERIFY2(tail == lead, "the text after the emphasized span inherited its format");
}

void TestInteractivePreview::testNoWidgetForImageCodeMathByDefault() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  setTextAndSettle(editor, QStringLiteral("![a](b.png)\n\n```cpp\nint a;\n```\n\n$$\nx\n$$\n"));

  // Without a custom factory those elements keep the painted static path.
  QVERIFY(previewWidgets(editor).isEmpty());
}

void TestInteractivePreview::testCustomFactoryOverridesBuiltin() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  setTextAndSettle(editor, QLatin1String(c_table));
  QVERIFY(singlePreviewWidget(editor));

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 1));
  QTest::qWait(50);
  QCoreApplication::processEvents();

  auto widget = qobject_cast<RecordingPreviewWidget *>(singlePreviewWidget(editor));
  QVERIFY(widget);
  QCOMPARE(widget->m_preview->type(), PreviewElementType::Table);
}

void TestInteractivePreview::testMultiTypeFactory() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Image, PreviewElementType::Code,
                                              PreviewElementType::Math, PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 1));

  setTextAndSettle(editor, QStringLiteral("![a](b.png)\n\n```cpp\nint a;\n```\n\n$$\nx\n$$\n\n") +
                               QLatin1String(c_table));

  QSet<int> types;
  const auto widgets = previewWidgets(editor);
  for (auto widget : widgets) {
    auto recording = qobject_cast<RecordingPreviewWidget *>(widget);
    QVERIFY(recording);
    types.insert(static_cast<int>(recording->m_preview->type()));
  }

  QCOMPARE(widgets.size(), 4);
  QCOMPARE(types.size(), 4);
}

void TestInteractivePreview::testFactoryPriorityAndOrder() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto low = new RecordingPreviewFactory({PreviewElementType::Table});
  auto high = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(low, 1));
  QVERIFY(editor.registerPreviewWidgetFactory(high, 5));

  setTextAndSettle(editor, QLatin1String(c_table));

  QCOMPARE(high->m_widgets.size(), 1);
  QCOMPARE(low->m_createCount, 0);

  // Same priority: registration order decides.
  auto firstOfTwo = new RecordingPreviewFactory({PreviewElementType::Table});
  auto secondOfTwo = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(firstOfTwo, 9));
  QVERIFY(editor.registerPreviewWidgetFactory(secondOfTwo, 9));
  QTest::qWait(50);
  QCoreApplication::processEvents();

  QCOMPARE(firstOfTwo->m_widgets.size(), 1);
  QCOMPARE(secondOfTwo->m_createCount, 0);
}

void TestInteractivePreview::testDecliningFactoryFallsThrough() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto declining = new RecordingPreviewFactory({PreviewElementType::Table});
  declining->m_decline = true;
  QVERIFY(editor.registerPreviewWidgetFactory(declining, 5));

  setTextAndSettle(editor, QLatin1String(c_table));

  QVERIFY(declining->m_createCount > 0);
  QVERIFY(declining->m_widgets.isEmpty());
  // The host asks what a factory claims before it ever builds a widget, so a
  // declining factory is still consulted.
  QVERIFY(declining->m_supportedTypesCount > 0);
  // The built-in renderer took over.
  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  QVERIFY(!qobject_cast<RecordingPreviewWidget *>(widget));
}

void TestInteractivePreview::testRefusingWidgetFallsThrough() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto refusing = new RecordingPreviewFactory({PreviewElementType::Table});
  refusing->m_refuseSetPreview = true;
  QVERIFY(editor.registerPreviewWidgetFactory(refusing, 5));

  setTextAndSettle(editor, QLatin1String(c_table));

  QVERIFY(refusing->m_createCount > 0);
  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  QVERIFY(!qobject_cast<RecordingPreviewWidget *>(widget));
}

void TestInteractivePreview::testRegistrationValidation() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  QVERIFY(!editor.registerPreviewWidgetFactory(nullptr, 0));

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 1));
  // Duplicate registration is rejected and does not take ownership again.
  QVERIFY(!editor.registerPreviewWidgetFactory(factory, 2));

  QVERIFY(!editor.unregisterPreviewWidgetFactory(nullptr));
}

void TestInteractivePreview::testUnregisterRestoresFallback() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QLatin1String(c_table));
  QVERIFY(qobject_cast<RecordingPreviewWidget *>(singlePreviewWidget(editor)));

  QVERIFY(editor.unregisterPreviewWidgetFactory(factory));
  QTest::qWait(50);
  QCoreApplication::processEvents();

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  QVERIFY(!qobject_cast<RecordingPreviewWidget *>(widget));
}

void TestInteractivePreview::testUnregisterDestroysFactory() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QPointer<RecordingPreviewFactory> guard(factory);
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  QCOMPARE(factory->parent(),
           editor.findChild<QObject *>(QStringLiteral("vte_interactive_preview_host")));

  QVERIFY(editor.unregisterPreviewWidgetFactory(factory));
  // Unregistering twice is refused.
  QTest::qWait(50);
  QCoreApplication::processEvents();
  QVERIFY(guard.isNull());
}

void TestInteractivePreview::testEditorDestructionDestroysFactory() {
  QPointer<RecordingPreviewFactory> guard;
  {
    VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
    auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
    guard = factory;
    QVERIFY(editor.registerPreviewWidgetFactory(factory, 1));
    setTextAndSettle(editor, QLatin1String(c_table));
    QVERIFY(!guard.isNull());
  }

  QCoreApplication::processEvents();
  QVERIFY(guard.isNull());
}

void TestInteractivePreview::testIdentityReuseOnUnrelatedEdit() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));

  setTextAndSettle(editor, QLatin1String(c_table) + QStringLiteral("\ntail\n"));
  QCOMPARE(factory->m_widgets.size(), 1);
  auto widget = factory->m_widgets.first();
  const int firstSetCount = widget->m_setPreviewCount;

  // Append unrelated text: the same widget must be updated in place.
  QTextCursor cursor(editor.document());
  cursor.movePosition(QTextCursor::End);
  cursor.insertText(QStringLiteral("more text\n"));
  settle(editor);

  QCOMPARE(factory->m_widgets.size(), 1);
  QVERIFY(widget->m_setPreviewCount > firstSetCount);
  QCOMPARE(singlePreviewWidget(editor), static_cast<PreviewWidget *>(widget));
}

// ---------------------------------------------------------------------------
// Replacement
// ---------------------------------------------------------------------------

void TestInteractivePreview::testReplacementAccepted() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QLatin1String(c_table));

  QCOMPARE(factory->m_widgets.size(), 1);
  auto widget = factory->m_widgets.first();
  widget->previewContext()->requestSourceReplacement(
      QStringLiteral("| h1 | h2 |\n| --- | --- |\n| z | b |"));

  QCOMPARE(widget->m_resultCount, 1);
  QCOMPARE(widget->m_lastResult.status(), PreviewReplacementResult::Accepted);
  QVERIFY(widget->m_lastResult.isAccepted());
  QVERIFY(editor.document()->toPlainText().contains(QStringLiteral("| z | b |")));
}

void TestInteractivePreview::testReplacementIsOneUndoStep() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QLatin1String(c_table));

  const QString before = editor.document()->toPlainText();
  auto widget = factory->m_widgets.first();
  widget->previewContext()->requestSourceReplacement(
      QStringLiteral("| h1 | h2 |\n| --- | --- |\n| z | b |"));
  QVERIFY(widget->m_lastResult.isAccepted());
  QVERIFY(editor.document()->toPlainText() != before);

  editor.document()->undo();
  QCOMPARE(editor.document()->toPlainText(), before);

  editor.document()->redo();
  QVERIFY(editor.document()->toPlainText().contains(QStringLiteral("| z | b |")));
}

void TestInteractivePreview::testReplacementRejectedWhenReadOnly() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QLatin1String(c_table));

  const QString before = editor.document()->toPlainText();
  editor.getTextEdit()->setReadOnly(true);

  auto widget = factory->m_widgets.first();
  widget->previewContext()->requestSourceReplacement(
      QStringLiteral("| h1 | h2 |\n| --- | --- |\n| z | b |"));

  QCOMPARE(widget->m_lastResult.status(), PreviewReplacementResult::ReadOnly);
  QCOMPARE(editor.document()->toPlainText(), before);
}

void TestInteractivePreview::testReplacementRejectedOnStaleSnapshot() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QLatin1String(c_table));

  auto widget = factory->m_widgets.first();
  auto context = widget->previewContext();
  auto stale = context->preview();

  // Produce a newer accepted generation.
  QTextCursor cursor(editor.document());
  cursor.movePosition(QTextCursor::End);
  cursor.insertText(QStringLiteral("\ntail\n"));
  settle(editor);

  QVERIFY(context->preview()->revision() != stale->revision());

  // Ask the host directly with the superseded revision.
  QSignalSpy spy(context, &PreviewWidgetContext::replacementFinished);
  emit context->sourceReplacementRequested(context->identity(), stale->revision(),
                                           stale->sourceMarkdown(),
                                           QStringLiteral("| h1 | h2 |\n| --- | --- |\n| z | b |"));
  QCOMPARE(spy.count(), 1);
  QCOMPARE(widget->m_lastResult.status(), PreviewReplacementResult::StaleSnapshot);
}

void TestInteractivePreview::testReplacementRejectedOnTypeMismatch() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QLatin1String(c_table));

  const QString before = editor.document()->toPlainText();
  auto widget = factory->m_widgets.first();
  widget->previewContext()->requestSourceReplacement(QStringLiteral("```cpp\nint a;\n```"));

  QCOMPARE(widget->m_lastResult.status(), PreviewReplacementResult::TypeMismatch);
  QCOMPARE(editor.document()->toPlainText(), before);
}

void TestInteractivePreview::testReplacementRejectedOnElementCountMismatch() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QLatin1String(c_table));

  const QString before = editor.document()->toPlainText();
  auto widget = factory->m_widgets.first();

  // Two tables.
  widget->previewContext()->requestSourceReplacement(QLatin1String(c_table) + QStringLiteral("\n") +
                                                     QLatin1String(c_table));
  QCOMPARE(widget->m_lastResult.status(), PreviewReplacementResult::ElementCountMismatch);

  // Unrelated content outside the table.
  widget->previewContext()->requestSourceReplacement(
      QStringLiteral("| h1 | h2 |\n| --- | --- |\n| a | b |\n\nunrelated"));
  QCOMPARE(widget->m_lastResult.status(), PreviewReplacementResult::ElementCountMismatch);

  // Not parseable as any element.
  widget->previewContext()->requestSourceReplacement(QStringLiteral("just text"));
  QCOMPARE(widget->m_lastResult.status(), PreviewReplacementResult::ElementCountMismatch);

  QCOMPARE(editor.document()->toPlainText(), before);
}

void TestInteractivePreview::testReplacementAcceptedAfterUnrelatedEdit() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QLatin1String(c_table) + QStringLiteral("\ntail\n"));

  auto widget = factory->m_widgets.first();

  // An edit elsewhere bumps the document revision but the anchored source is
  // still identical, so the request must be accepted.
  QTextCursor cursor(editor.document());
  cursor.movePosition(QTextCursor::End);
  cursor.insertText(QStringLiteral("appended\n"));
  settle(editor);

  widget->previewContext()->requestSourceReplacement(
      QStringLiteral("| h1 | h2 |\n| --- | --- |\n| z | b |"));
  QCOMPARE(widget->m_lastResult.status(), PreviewReplacementResult::Accepted);
  QVERIFY(editor.document()->toPlainText().contains(QStringLiteral("| z | b |")));
  QVERIFY(editor.document()->toPlainText().contains(QStringLiteral("appended")));
}

void TestInteractivePreview::testReplacementPreservesBlockquotePrefix() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QStringLiteral("> | a | b |\n> | --- | --- |\n> | c | d |\n"));

  QCOMPARE(factory->m_widgets.size(), 1);
  auto widget = factory->m_widgets.first();
  QCOMPARE(widget->m_preview->startPos(), 0);
  QVERIFY(widget->m_preview->sourceMarkdown().startsWith(QStringLiteral("> |")));

  // A replacement dropping the prefix no longer covers the whole text as a
  // single table descendant of the original wrapper chain, but a prefixed one
  // is accepted.
  widget->previewContext()->requestSourceReplacement(
      QStringLiteral("> | a | b |\n> | --- | --- |\n> | z | d |"));
  QCOMPARE(widget->m_lastResult.status(), PreviewReplacementResult::Accepted);
  QVERIFY(editor.document()->toPlainText().contains(QStringLiteral("> | z | d |")));
}

void TestInteractivePreview::testTableEditCommitsCanonicalMarkdown() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  setTextAndSettle(editor, QLatin1String(c_table));

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto sheet = sheetView(widget);
  QVERIFY(sheet);
  auto table = sheetTable(sheet);
  QVERIFY(table);
  QCOMPARE(table->rows(), 2);
  QCOMPARE(table->columns(), 2);
  QCOMPARE(sheetCell(sheet, 0, 0), QStringLiteral("h1"));
  QCOMPARE(sheetCell(sheet, 1, 1), QStringLiteral("b"));

  // A no-op edit changes nothing: the flush compares what the document would
  // serialize to, so retyping the same value never reaches the document.
  const QString before = editor.document()->toPlainText();
  editCell(sheet, 1, 1, QStringLiteral("b"));
  flushSheet(sheet);
  QCOMPARE(editor.document()->toPlainText(), before);

  // A real edit is written back in canonical form.
  editCell(sheet, 1, 1, QStringLiteral("changed"));
  flushSheet(sheet);
  const QString after = editor.document()->toPlainText();
  QVERIFY2(after.contains(QStringLiteral("| a | changed |")), qPrintable(after));
  QVERIFY2(after.contains(QStringLiteral("| --- | --- |")), qPrintable(after));
  QVERIFY2(after.contains(QStringLiteral("| h1 | h2 |")), qPrintable(after));
}

void TestInteractivePreview::testEnterInTheLastCellGrowsTheSource() {
  // The unit test's harness answers the replacement itself, so only this
  // target can prove that the *real* host accepts a table which grew: the
  // candidate carries a row the bound snapshot never had, which is exactly
  // what its prefix matching and its anchor retargeting have to tolerate.
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  setTextAndSettle(editor, QLatin1String(c_table));

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto sheet = sheetView(widget);
  QVERIFY(sheet);
  QTextTable *table = sheetTable(sheet);
  QVERIFY(table);
  QCOMPARE(table->rows(), 2);

  putCaretIn(sheet, table->rows() - 1, table->columns() - 1);
  QTest::keyClick(sheet, Qt::Key_Return);
  QCoreApplication::processEvents();

  // Leaving the finished cell flushes without waiting for the idle window, so
  // the source has already grown by exactly one body row.
  const QString after = editor.document()->toPlainText();
  QVERIFY2(after.contains(QStringLiteral("| h1 | h2 |\n"
                                         "| --- | --- |\n"
                                         "| a | b |\n"
                                         "|  |  |")),
           qPrintable(after));

  // The replacement was accepted, not rejected into a reset: the sheet keeps
  // its enlarged table and its caret in the new row.
  QCOMPARE(sheetTable(sheet)->rows(), 3);
  QTextTableCell caretCell = sheetTable(sheet)->cellAt(sheet->textCursor().position());
  QVERIFY(caretCell.isValid());
  QCOMPARE(caretCell.row(), 2);
  QCOMPARE(caretCell.column(), 0);

  // And the parse generation which follows re-binds the same widget onto the
  // enlarged snapshot rather than rebuilding the sheet away underneath the
  // caret.
  settle(editor);
  QCOMPARE(singlePreviewWidget(editor), widget);
  QCOMPARE(sheetView(widget), sheet);
  QCOMPARE(sheetTable(sheet)->rows(), 3);
  caretCell = sheetTable(sheet)->cellAt(sheet->textCursor().position());
  QVERIFY(caretCell.isValid());
  QCOMPARE(caretCell.row(), 2);
  QCOMPARE(caretCell.column(), 0);

  // Typing into the new row still commits, which is the proof the anchor was
  // retargeted onto the enlarged table.
  editCell(sheet, 2, 0, QStringLiteral("c"));
  flushSheet(sheet);
  QVERIFY2(editor.document()->toPlainText().contains(QStringLiteral("| c |  |")),
           qPrintable(editor.document()->toPlainText()));
}

void TestInteractivePreview::testAColumnInsertGrowsTheSource() {
  // The unit target's harness accepts a replacement itself; only the real host
  // re-validates the parse, the prefixes, the expected source and the live
  // anchor. A column insert is the widest of those changes - the header and
  // the delimiter row both grow - so it is what proves the retargeting copes
  // with more than an appended row.
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  setTextAndSettle(editor, QLatin1String(c_table));

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto sheet = sheetView(widget);
  QVERIFY(sheet);
  QCOMPARE(sheetTable(sheet)->columns(), 2);

  const QString sourceBefore = editor.document()->toPlainText();

  QVERIFY(triggerTableAction(sheet, 1, 1, "InsertColumnRight"));
  flushSheet(sheet);

  const QString after = editor.document()->toPlainText();
  QVERIFY2(after.contains(QStringLiteral("| h1 | h2 |  |\n"
                                         "| --- | --- | --- |\n"
                                         "| a | b |  |")),
           qPrintable(after));

  // Accepted rather than rejected into a reset: the sheet keeps the column and
  // the caret waits in it.
  QCOMPARE(sheetTable(sheet)->columns(), 3);
  const QTextTableCell caretCell = sheetTable(sheet)->cellAt(sheet->textCursor().position());
  QVERIFY(caretCell.isValid());
  QCOMPARE(caretCell.column(), 2);

  settle(editor);
  QCOMPARE(singlePreviewWidget(editor), widget);
  QCOMPARE(sheetView(widget), sheet);

  const QString sourceAfterInsert = editor.document()->toPlainText();

  // Typing into the new column still commits, which is the proof the anchor
  // was retargeted onto the widened table.
  editCell(sheet, 1, 2, QStringLiteral("c"));
  flushSheet(sheet);
  QVERIFY2(editor.document()->toPlainText().contains(QStringLiteral("| a | b | c |")),
           qPrintable(editor.document()->toPlainText()));

  // The inner document has no undo stack of its own, so every commit is one
  // whole-table replacement on the editor's: one undo per operation, and the
  // second one takes the column out again.
  editor.document()->undo();
  settle(editor);
  QCOMPARE(editor.document()->toPlainText(), sourceAfterInsert);

  editor.document()->undo();
  settle(editor);
  QCOMPARE(editor.document()->toPlainText(), sourceBefore);

  // And a structural change which really does move the sheet's geometry is
  // reported to the host, which is what makes it re-reserve the band. A row is
  // what proves the path: a column can leave the height exactly as it was, and
  // the sheet deliberately terminates on an unchanged measurement.
  widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  sheet = sheetView(widget);
  QVERIFY(sheet);
  const int heightBefore = widget->height();
  QSignalSpy geometrySpy(sheet, SIGNAL(preferredGeometryChanged()));
  QVERIFY(geometrySpy.isValid());

  QVERIFY(triggerTableAction(sheet, 1, 1, "InsertRowBelow"));
  flushSheet(sheet);
  settle(editor);

  QVERIFY2(geometrySpy.count() > 0, "the structural change was not re-measured");
  QVERIFY2(singlePreviewWidget(editor)->height() > heightBefore,
           "the band did not grow with the row");
}

void TestInteractivePreview::testAnAlignmentChangeReachesTheDelimiterRow() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  setTextAndSettle(editor, QLatin1String(c_table));

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto sheet = sheetView(widget);
  QVERIFY(sheet);

  const QString sourceBefore = editor.document()->toPlainText();

  // Nothing about the cells changes, so the delimiter row is the only thing
  // which can carry this commit at all.
  QVERIFY(triggerTableAction(sheet, 0, 1, "AlignmentRight"));
  flushSheet(sheet);

  const QString after = editor.document()->toPlainText();
  QVERIFY2(after.contains(QStringLiteral("| h1 | h2 |\n"
                                         "| --- | ---: |\n"
                                         "| a | b |")),
           qPrintable(after));

  settle(editor);
  QCOMPARE(singlePreviewWidget(editor), widget);
  QCOMPARE(sheetView(widget), sheet);

  // One undo, and the source is exactly what it was.
  editor.document()->undo();
  settle(editor);
  QCOMPARE(editor.document()->toPlainText(), sourceBefore);
}

void TestInteractivePreview::testPaddedSourceIsOnlyRewrittenOnARealEdit() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  // A padded source is not the canonical form any more, so it is the case
  // where a no-op flush could most easily rewrite the document for nothing.
  setTextAndSettle(editor, QStringLiteral("| h1  | h2      |\n"
                                          "| --- | ------- |\n"
                                          "| a   | b       |\n"));

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto sheet = sheetView(widget);
  QVERIFY(sheet);
  QVERIFY(sheetTable(sheet));

  // Retyping the same value leaves the padded source exactly as it was: the
  // baseline is the source's own canonical form, not its literal text.
  const QString before = editor.document()->toPlainText();
  editCell(sheet, 1, 1, QStringLiteral("b"));
  flushSheet(sheet);
  QCOMPARE(editor.document()->toPlainText(), before);

  // A real edit rewrites the whole table compactly, padding and all.
  editCell(sheet, 1, 1, QStringLiteral("changed"));
  flushSheet(sheet);
  const QString after = editor.document()->toPlainText();
  QVERIFY2(after.contains(QStringLiteral("| h1 | h2 |\n"
                                         "| --- | --- |\n"
                                         "| a | changed |")),
           qPrintable(after));

  // And the parse echo of that rewrite is itself a no-op: the sheet does not
  // rewrite the document a second time.
  settle(editor);
  const QString settled = editor.document()->toPlainText();
  flushSheet(sheet);
  QCOMPARE(editor.document()->toPlainText(), settled);
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

void TestInteractivePreview::testSourceBitDisablesTablePreview() {
  auto config = makeConfig();
  config->m_inplacePreviewSources = MarkdownEditorConfig::ImageLink |
                                    MarkdownEditorConfig::CodeBlock | MarkdownEditorConfig::Math;

  VMarkdownEditor editor(config, QSharedPointer<TextEditorParameters>::create());
  setTextAndSettle(editor, QLatin1String(c_table));
  QVERIFY(previewWidgets(editor).isEmpty());

  // Turning the bit on at runtime brings the sheet back.
  auto enabled = makeConfig();
  enabled->m_inplacePreviewSources = MarkdownEditorConfig::Table;
  editor.setConfig(enabled);
  settle(editor);
  QVERIFY(singlePreviewWidget(editor));
}

void TestInteractivePreview::testGlobalDisableRemovesWidgets() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  setTextAndSettle(editor, QLatin1String(c_table));
  QVERIFY(singlePreviewWidget(editor));

  editor.setInplacePreviewEnabled(false);
  QTest::qWait(50);
  QCoreApplication::processEvents();
  QVERIFY(previewWidgets(editor).isEmpty());

  editor.setInplacePreviewEnabled(true);
  QTest::qWait(50);
  QCoreApplication::processEvents();
  QVERIFY(singlePreviewWidget(editor));
}

void TestInteractivePreview::testDuplicateTablesGetDistinctIdentities() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QLatin1String(c_table) + QStringLiteral("\n") + QLatin1String(c_table));

  QCOMPARE(factory->m_widgets.size(), 2);
  const auto first = factory->m_widgets[0]->previewContext();
  const auto second = factory->m_widgets[1]->previewContext();
  QVERIFY(first->identity() != second->identity());
  QVERIFY(first->preview()->startPos() != second->preview()->startPos());
}

void TestInteractivePreview::testWidgetGeometryFollowsScrolling() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));

  QString text;
  for (int i = 0; i < 80; ++i) {
    text += QStringLiteral("filler line %1\n").arg(i);
  }
  text += QStringLiteral("\n");
  text += QLatin1String(c_table);

  editor.resize(600, 300);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  setTextAndSettle(editor, text);

  QVERIFY2(factory->m_widgets.size() == 1, qPrintable(QStringLiteral("created=%1 live=%2")
                                                          .arg(factory->m_widgets.size())
                                                          .arg(previewWidgets(editor).size())));
  auto widget = factory->m_widgets.first();
  auto vbar = editor.getTextEdit()->verticalScrollBar();
  QVERIFY(vbar->maximum() > 0);

  // Both sample points keep the preview ON SCREEN, deliberately.
  //
  // The placement pass only repositions the widgets inside the viewport plus
  // one viewport height of margin; one parked far outside that interval is
  // hidden and its stale viewport coordinates are not maintained, because
  // maintaining them is exactly the O(N)-per-scroll-tick cost that was
  // removed. What has to hold is that a VISIBLE widget tracks the scroll.
  vbar->setValue(vbar->maximum());
  QCoreApplication::processEvents();
  QVERIFY2(widget->isVisible(), "the preview is not on screen at the bottom of the document");
  const int bottomY = widget->y();

  vbar->setValue(qMax(vbar->minimum(), vbar->maximum() - 40));
  QCoreApplication::processEvents();
  QVERIFY2(widget->isVisible(), "the preview left the screen after a 40 px scroll");
  // Scrolling up moves the content down, so the widget's viewport y grows.
  QVERIFY2(widget->y() > bottomY, "the widget did not follow the scroll");

  // The document rect stays stable while only the viewport mapping moves.
  QVERIFY(!widget->previewContext()->assignedPreviewRect().isNull());
  QVERIFY(widget->isVisible());
}

// ---------------------------------------------------------------------------
// Regressions
// ---------------------------------------------------------------------------

void TestInteractivePreview::testReplacementRejectedOnChangedContainerChain() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QStringLiteral("> | a | b |\n> | --- | --- |\n> | c | d |\n"));

  QCOMPARE(factory->m_widgets.size(), 1);
  auto widget = factory->m_widgets.first();
  const QString before = editor.document()->toPlainText();

  // Dropping the block quote would lift the table out of its container.
  widget->previewContext()->requestSourceReplacement(
      QStringLiteral("| a | b |\n| --- | --- |\n| z | d |"));
  QVERIFY(!widget->m_lastResult.isAccepted());
  QCOMPARE(editor.document()->toPlainText(), before);

  // Deepening the block quote is a structural change too.
  widget->previewContext()->requestSourceReplacement(
      QStringLiteral(">> | a | b |\n>> | --- | --- |\n>> | z | d |"));
  QVERIFY(!widget->m_lastResult.isAccepted());
  QCOMPARE(editor.document()->toPlainText(), before);

  // Turning it into a list item is rejected as well.
  widget->previewContext()->requestSourceReplacement(
      QStringLiteral("- | a | b |\n  | --- | --- |\n  | z | d |"));
  QVERIFY(!widget->m_lastResult.isAccepted());
  QCOMPARE(editor.document()->toPlainText(), before);
}

void TestInteractivePreview::testReplacementRejectedWhenSplittingTrailingText() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Image});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QStringLiteral("lead ![a](b.png) trail\n"));

  QCOMPARE(factory->m_widgets.size(), 1);
  auto widget = factory->m_widgets.first();
  const QString before = editor.document()->toPlainText();

  // A multi-line replacement would push " trail" into a new block.
  widget->previewContext()->requestSourceReplacement(QStringLiteral("![a](b.png)\n![c](d.png)"));
  QVERIFY(!widget->m_lastResult.isAccepted());
  QCOMPARE(editor.document()->toPlainText(), before);

  // A single-line replacement is fine.
  widget->previewContext()->requestSourceReplacement(QStringLiteral("![c](d.png)"));
  QCOMPARE(widget->m_lastResult.status(), PreviewReplacementResult::Accepted);
  QVERIFY(editor.document()->toPlainText().contains(QStringLiteral("lead ![c](d.png) trail")));
}

void TestInteractivePreview::testSourceMismatchDoesNotRestoreStaleValues() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  setTextAndSettle(editor, QLatin1String(c_table));

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto sheet = sheetView(widget);
  QVERIFY(sheet);
  QVERIFY(!sheet->isReadOnly());

  // Change the source without letting a new snapshot be published.
  QTextCursor cursor(editor.document());
  cursor.setPosition(3);
  cursor.insertText(QStringLiteral("X"));

  editCell(sheet, 1, 1, QStringLiteral("edited"));
  flushSheet(sheet);

  // The live source no longer matches, so the widget must stop presenting the
  // superseded snapshot as the truth instead of restoring its old values.
  QVERIFY(sheet->isReadOnly());
  QCOMPARE(sheetCell(sheet, 1, 1), QStringLiteral("edited"));
  QVERIFY(editor.document()->toPlainText().contains(QStringLiteral("| hX1 | h2 |")));

  // The authoritative snapshot re-enables editing.
  settle(editor);
  auto refreshed = singlePreviewWidget(editor);
  QVERIFY(refreshed);
  QVERIFY(!sheetView(refreshed)->isReadOnly());
}

void TestInteractivePreview::testFactoryUnregisteringItselfFromCallback() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  int createCount = 0;
  auto suicidal = new RecordingPreviewFactory({PreviewElementType::Table});
  suicidal->m_createCountSink = &createCount;
  QPointer<RecordingPreviewFactory> guard(suicidal);
  QVERIFY(editor.registerPreviewWidgetFactory(suicidal, 5));
  suicidal->m_unregisterSelfIn = &editor;

  setTextAndSettle(editor, QLatin1String(c_table));

  // Unregistering from inside a callback destroys the factory without ever
  // reaching createWidget() on the now inactive instance.
  QVERIFY(guard.isNull());
  QVERIFY2(createCount == 0, qPrintable(QStringLiteral("create=%1").arg(createCount)));

  // The host fell through to the built-in renderer.
  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  QVERIFY(!qobject_cast<RecordingPreviewWidget *>(widget));
}

void TestInteractivePreview::testReentrantRegistrationRejected() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  factory->m_registerReentrantlyIn = &editor;

  setTextAndSettle(editor, QLatin1String(c_table));

  QVERIFY(factory->m_createCount > 0);
  QVERIFY(!factory->m_reentrantRegistrationAccepted);
  QVERIFY(qobject_cast<RecordingPreviewWidget *>(singlePreviewWidget(editor)));
}

void TestInteractivePreview::testEditorDestructionDestroysWidgetsAndContexts() {
  QPointer<PreviewWidget> widgetGuard;
  QPointer<PreviewWidgetContext> contextGuard;
  {
    VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
    setTextAndSettle(editor, QLatin1String(c_table));

    auto widget = singlePreviewWidget(editor);
    QVERIFY(widget);
    widgetGuard = widget;
    contextGuard = widget->previewContext();
    QVERIFY(!contextGuard.isNull());
  }

  // No event loop turn needed: nothing is left unowned behind the editor.
  QVERIFY(widgetGuard.isNull());
  QVERIFY(contextGuard.isNull());
}

void TestInteractivePreview::testWrappingWidgetGeometryIsStable() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Image});
  factory->m_wrapping = true;
  factory->m_hint = QSize(400, 40);
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));

  editor.resize(500, 400);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  // An inline image whose source span is much narrower than its size hint.
  setTextAndSettle(editor, QStringLiteral("lead ![a](b.png) trail\n"));

  QCOMPARE(factory->m_widgets.size(), 1);
  auto widget = factory->m_widgets.first();
  const QRectF assigned = widget->previewContext()->assignedPreviewRect();
  QVERIFY(!assigned.isNull());

  // The reserved height honors the width the widget is actually given.
  QCOMPARE(qRound(assigned.height()), widget->heightForWidth(qRound(assigned.width())));
  QCOMPARE(widget->height(), qRound(assigned.height()));

  // Applying the assigned geometry must not feed back into the measurement.
  QTest::qWait(200);
  QCoreApplication::processEvents();
  QCOMPARE(widget->previewContext()->assignedPreviewRect(), assigned);
}

void TestInteractivePreview::testEditorDestructionDestroysPendingRemovals() {
  QPointer<PreviewWidget> widgetGuard;
  QPointer<PreviewWidgetContext> contextGuard;
  {
    VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
    setTextAndSettle(editor, QLatin1String(c_table));

    auto widget = singlePreviewWidget(editor);
    QVERIFY(widget);
    widgetGuard = widget;
    contextGuard = widget->previewContext();

    // Remove the table so the item is queued for deferred deletion, and do not
    // let the event loop deliver it.
    QTextCursor cursor(editor.document());
    cursor.select(QTextCursor::Document);
    cursor.insertText(QStringLiteral("plain text\n"));
    editor.getHighlighter()->updateHighlight();
    QTRY_VERIFY(widgetGuard.isNull() || previewWidgets(editor).isEmpty());
  }

  // Nothing survives the editor, even when its deferred deletion never ran.
  QVERIFY(widgetGuard.isNull());
  QVERIFY(contextGuard.isNull());
}

void TestInteractivePreview::testWrappedInlineSourceMeasuredAtAssignedWidth() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Image});
  factory->m_wrapping = true;
  factory->m_hint = QSize(600, 60);
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));

  editor.resize(320, 400);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));

  // A long inline image whose own syntax wraps over several visual lines.
  const QString alt = QString(160, QLatin1Char('w'));
  setTextAndSettle(editor, QStringLiteral("lead ![%1](b.png) trail\n").arg(alt));

  QCOMPARE(factory->m_widgets.size(), 1);
  auto widget = factory->m_widgets.first();
  const QRectF assigned = widget->previewContext()->assignedPreviewRect();
  QVERIFY(!assigned.isNull());
  QVERIFY(assigned.width() > 0);

  // The reserved height matches the width the widget is actually given, even
  // though the source spans several visual lines.
  QCOMPARE(qRound(assigned.height()), widget->heightForWidth(qRound(assigned.width())));
  QCOMPARE(widget->width(), qRound(assigned.width()));
  QCOMPARE(widget->height(), qRound(assigned.height()));

  QTest::qWait(200);
  QCoreApplication::processEvents();
  QCOMPARE(widget->previewContext()->assignedPreviewRect(), assigned);
}

void TestInteractivePreview::testReplacementOfLaterInlineElement() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Image});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QStringLiteral("![first](a.png) ![second](b.png)\n"));

  QCOMPARE(factory->m_widgets.size(), 2);

  // Find the widget bound to the second image.
  RecordingPreviewWidget *second = nullptr;
  for (auto widget : factory->m_widgets) {
    if (widget->m_preview && widget->m_preview->startPos() > 0) {
      second = widget;
    }
  }
  QVERIFY(second);

  // Its probe carries the preceding image as line prefix; that must not make
  // the candidate ambiguous.
  second->previewContext()->requestSourceReplacement(QStringLiteral("![changed](c.png)"));
  QCOMPARE(second->m_lastResult.status(), PreviewReplacementResult::Accepted);
  QCOMPARE(editor.document()->toPlainText(), QStringLiteral("![first](a.png) ![changed](c.png)\n"));
}

void TestInteractivePreview::testWidgetDestroyingItselfOnUpdateFallsBack() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QLatin1String(c_table) + QStringLiteral("\ntail\n"));

  auto widget = qobject_cast<RecordingPreviewWidget *>(singlePreviewWidget(editor));
  QVERIFY(widget);
  const int createdBefore = factory->m_createCount;

  // Arm the widget so the next update destroys it from inside setPreview().
  widget->m_setPreviewCount = 1;
  widget->m_selfDestructOnUpdate = true;

  QTextCursor cursor(editor.document());
  cursor.movePosition(QTextCursor::End);
  cursor.insertText(QStringLiteral("appended\n"));
  settle(editor);

  // The host noticed and rebuilt through the factory chain instead of leaving
  // a dead entry behind.
  QVERIFY(factory->m_createCount > createdBefore);
  auto rebuilt = singlePreviewWidget(editor);
  QVERIFY(rebuilt);
  QVERIFY(rebuilt->previewContext());
  QCOMPARE(rebuilt->previewContext()->preview()->type(), PreviewElementType::Table);
}

void TestInteractivePreview::testCommitKeepsTheSameWidget() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QLatin1String(c_table) + QStringLiteral("\ntail\n"));

  auto widget = qobject_cast<RecordingPreviewWidget *>(singlePreviewWidget(editor));
  QVERIFY(widget);
  const quint64 identity = widget->previewContext()->identity();
  const int createdBefore = factory->m_createCount;

  widget->previewContext()->requestSourceReplacement(
      QStringLiteral("| h1 | h2 |\n| --- | --- |\n| z | b |"));
  QCOMPARE(widget->m_lastResult.status(), PreviewReplacementResult::Accepted);

  // The anchor was retargeted onto the replacement, so the reservation is not
  // dropped and the widget survives the commit.
  QCoreApplication::processEvents();
  QCOMPARE(singlePreviewWidget(editor), static_cast<PreviewWidget *>(widget));
  QVERIFY(!widget->previewContext()->assignedPreviewRect().isNull());

  settle(editor);
  QCOMPARE(singlePreviewWidget(editor), static_cast<PreviewWidget *>(widget));
  QCOMPARE(widget->previewContext()->identity(), identity);
  QCOMPARE(factory->m_createCount, createdBefore);
}

void TestInteractivePreview::testConfigChangeKeepsLiveAnchors() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QStringLiteral("head\n\n") + QLatin1String(c_table));

  auto widget = qobject_cast<RecordingPreviewWidget *>(singlePreviewWidget(editor));
  QVERIFY(widget);
  const int anchoredStart = widget->m_preview->startPos();

  // Edit before the table without letting a new parse generation land, then
  // force a rebuild through a source-bit change.
  QTextCursor cursor(editor.document());
  cursor.setPosition(0);
  cursor.insertText(QStringLiteral("prefix "));

  auto config = makeConfig();
  config->m_inplacePreviewSources = MarkdownEditorConfig::Table | MarkdownEditorConfig::ImageLink;
  editor.setConfig(config);
  QTest::qWait(50);
  QCoreApplication::processEvents();

  // The rebuilt preview must follow the edit, not fall back to the superseded
  // parse position.
  auto rebuilt = singlePreviewWidget(editor);
  QVERIFY(rebuilt);
  const QString anchored = editor.document()->toPlainText().mid(anchoredStart + 7, 4);
  QCOMPARE(anchored, QStringLiteral("| h1"));

  // A commit still resolves against the live source.
  auto recording = qobject_cast<RecordingPreviewWidget *>(rebuilt);
  QVERIFY(recording);
  recording->previewContext()->requestSourceReplacement(
      QStringLiteral("| h1 | h2 |\n| --- | --- |\n| z | b |"));
  QCOMPARE(recording->m_lastResult.status(), PreviewReplacementResult::Accepted);
  QVERIFY(editor.document()->toPlainText().startsWith(QStringLiteral("prefix head")));
}

void TestInteractivePreview::testReplacementRejectsExoticLineSeparators() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QLatin1String(c_table));

  auto widget = factory->m_widgets.first();
  const QString before = editor.document()->toPlainText();

  // QTextCursor::insertText() would start a new block on each of these while
  // cmark's line table only splits on '\n'.
  const QVector<QChar> separators{QChar(QLatin1Char('\r')), QChar(0x2028), QChar(0x2029)};
  for (const auto &separator : separators) {
    widget->previewContext()->requestSourceReplacement(QStringLiteral("| h1 | h2 |") + separator +
                                                       QStringLiteral("| --- | --- |") + separator +
                                                       QStringLiteral("| z | b |"));
    QCOMPARE(widget->m_lastResult.status(), PreviewReplacementResult::ParseFailure);
    QCOMPARE(editor.document()->toPlainText(), before);
  }
}

void TestInteractivePreview::testOversizedTableFallsBackToSource() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  // A QTextTable is not virtualized and relays the whole table out on every
  // keystroke, so the cell bound is a latency bound. Past it the raw Markdown
  // stays as the only rendering.
  QString text = QStringLiteral("| a | b | c | d | e |\n| --- | --- | --- | --- | --- |\n");
  for (int i = 0; i < 200; ++i) {
    text += QStringLiteral("| r | r | r | r | r |\n");
  }

  setTextAndSettle(editor, text);

  // No sheet is materialized; the raw Markdown stays as the only rendering.
  QVERIFY(previewWidgets(editor).isEmpty());
}

void TestInteractivePreview::testReadOnlyEditorDisablesCellEditing() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.getTextEdit()->setReadOnly(true);
  setTextAndSettle(editor, QLatin1String(c_table));

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto sheet = sheetView(widget);
  QVERIFY(sheet);
  // The sheet is presented as a viewer rather than swallowing edits the host
  // would reject at commit time. Read-only still allows the caret, a selection
  // and a copy.
  QVERIFY(sheet->isReadOnly());

  editor.getTextEdit()->setReadOnly(false);
  editor.getHighlighter()->updateHighlight();
  QTest::qWait(50);
  QCoreApplication::processEvents();
  QVERIFY(!sheet->isReadOnly());
}

void TestInteractivePreview::testNoSnapshotWorkWithoutAClaimableFactory() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  setTextAndSettle(editor, QStringLiteral("![a](b.png)\n\n```cpp\nint a;\n```\n\n$$\nx\n$$\n"));

  // Only Table has a built-in renderer, so no snapshot is produced for the
  // other types at all and the painted path is untouched.
  QSignalSpy spy(editor.getHighlighter(), &MarkdownHighlighter::previewElementsUpdated);
  editor.getHighlighter()->updateHighlight();
  QTRY_VERIFY(spy.count() > 0);

  const auto previews = spy.last().at(1).value<QVector<QSharedPointer<const Preview>>>();
  QVERIFY(previews.isEmpty());
  QVERIFY(previewWidgets(editor).isEmpty());

  // Registering a renderer makes the snapshots appear without a document edit.
  auto factory = new RecordingPreviewFactory({PreviewElementType::Image});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 1));
  QTest::qWait(50);
  QCoreApplication::processEvents();

  QCOMPARE(previewWidgets(editor).size(), 1);
  QCOMPARE(factory->m_widgets.first()->m_preview->type(), PreviewElementType::Image);
}

// ---------------------------------------------------------------------------
// Regressions
// ---------------------------------------------------------------------------

void TestInteractivePreview::testBackToBackReplacementsAccepted() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QLatin1String(c_table));

  auto widget = factory->m_widgets.first();
  auto context = widget->previewContext();

  context->requestSourceReplacement(QStringLiteral("| h1 | h2 |\n| --- | --- |\n| z | b |"));
  QCOMPARE(widget->m_lastResult.status(), PreviewReplacementResult::Accepted);

  // A second commit before the next parse generation lands. The bound snapshot
  // must describe what the first commit put in the document, otherwise this is
  // rejected as a SourceMismatch and the edit is silently dropped.
  context->requestSourceReplacement(QStringLiteral("| h1 | h2 |\n| --- | --- |\n| z | y |"));
  QCOMPARE(widget->m_lastResult.status(), PreviewReplacementResult::Accepted);
  QVERIFY(editor.document()->toPlainText().contains(QStringLiteral("| z | y |")));

  // And the snapshot the next parse generation delivers still matches.
  settle(editor);
  QCOMPARE(previewWidgets(editor).size(), 1);
}

void TestInteractivePreview::testBackToBackReplacementsAcceptedForNonTable() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  // Rebasing is not a table concern: any application renderer can commit twice
  // inside one parse generation.
  auto factory = new RecordingPreviewFactory({PreviewElementType::Code});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QStringLiteral("```cpp\nint a;\n```\n"));

  QCOMPARE(factory->m_widgets.size(), 1);
  auto context = factory->m_widgets.first()->previewContext();

  context->requestSourceReplacement(QStringLiteral("```cpp\nint b;\n```"));
  QCOMPARE(factory->m_widgets.first()->m_lastResult.status(), PreviewReplacementResult::Accepted);

  context->requestSourceReplacement(QStringLiteral("```cpp\nint c;\n```"));
  QCOMPARE(factory->m_widgets.first()->m_lastResult.status(), PreviewReplacementResult::Accepted);
  QVERIFY(editor.document()->toPlainText().contains(QStringLiteral("int c;")));
}

void TestInteractivePreview::testRebasedSourceSurvivesRebuild() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QLatin1String(c_table));

  const QString committed = QStringLiteral("| h1 | h2 |\n| --- | --- |\n| z | b |");
  factory->m_widgets.first()->previewContext()->requestSourceReplacement(committed);
  QCOMPARE(factory->m_widgets.first()->m_lastResult.status(), PreviewReplacementResult::Accepted);

  // A factory change rebuilds every item by replaying the parse generation,
  // which still holds the pre-commit source, and forces a re-emit of that same
  // generation on top. The binding has to keep describing what the live anchor
  // actually spans. Only the queued rebuild is awaited here: a fresh parse
  // generation would repair the state and mask the defect.
  auto other = new RecordingPreviewFactory({PreviewElementType::Image});
  QVERIFY(editor.registerPreviewWidgetFactory(other, 1));
  QCoreApplication::processEvents();
  QCoreApplication::processEvents();

  QVERIFY(!factory->m_widgets.isEmpty());
  auto rebuilt = factory->m_widgets.last();
  QVERIFY(rebuilt->m_preview);
  QCOMPARE(rebuilt->m_preview->sourceMarkdown(), committed);

  // And a further commit is still accepted against it.
  rebuilt->previewContext()->requestSourceReplacement(
      QStringLiteral("| h1 | h2 |\n| --- | --- |\n| z | y |"));
  QCOMPARE(rebuilt->m_lastResult.status(), PreviewReplacementResult::Accepted);
  QVERIFY(editor.document()->toPlainText().contains(QStringLiteral("| z | y |")));
}

void TestInteractivePreview::testReadOnlyToggleReachesLiveSheets() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  setTextAndSettle(editor, QLatin1String(c_table));

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto sheet = sheetView(widget);
  QVERIFY(sheet);
  QVERIFY(!sheet->isReadOnly());

  // QTextEdit::setReadOnly() emits no signal and changes no content, so nothing
  // would republish. The sheet still has to stop offering edits immediately.
  editor.getTextEdit()->setReadOnly(true);
  QVERIFY(sheet->isReadOnly());

  editor.getTextEdit()->setReadOnly(false);
  QVERIFY(!sheet->isReadOnly());
}

void TestInteractivePreview::testRaggedTableIsNotEditable() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  // The body row carries one cell more than the header declares. GFM ignores
  // the excess, so writing the sheet back must not promote it into the header.
  const QString ragged = QStringLiteral("| h1 | h2 |\n| --- | --- |\n| a | b | c |\n");
  setTextAndSettle(editor, ragged);

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto sheet = sheetView(widget);
  QVERIFY(sheet);
  auto table = sheetTable(sheet);
  QVERIFY(table);

  // The extra cell stays visible, so nothing is hidden from the user.
  QCOMPARE(table->columns(), 3);

  // But the sheet is a viewer: it cannot be written back without changing what
  // the table renders to.
  QVERIFY(sheet->isReadOnly());

  // Even an edit which bypasses the read-only flag leaves the document
  // untouched, because the flush refuses to serialize a non round-trippable
  // sheet at all.
  const QString before = editor.document()->toPlainText();
  editCell(sheet, 1, 0, QStringLiteral("zz"));
  flushSheet(sheet);
  QCOMPARE(editor.document()->toPlainText(), before);
}

void TestInteractivePreview::testCommitKeepsCellEditorAcrossNextParse() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  setTextAndSettle(editor, QLatin1String(c_table));

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto sheet = sheetView(widget);
  QVERIFY(sheet);

  editCell(sheet, 1, 0, QStringLiteral("zz"));
  flushSheet(sheet);
  QVERIFY(editor.document()->toPlainText().contains(QStringLiteral("zz")));

  // Put the caret in another cell, then let the parse generation which merely
  // echoes this sheet's own commit arrive. Rebuilding the document here would
  // destroy the caret the user is typing at.
  putCaretIn(sheet, 1, 1);
  const int caret = sheet->textCursor().position();
  QVERIFY(sheet->textCursor().currentTable());

  settle(editor);

  QCOMPARE(singlePreviewWidget(editor), widget);
  QCOMPARE(sheetView(widget), sheet);
  QCOMPARE(sheet->textCursor().position(), caret);
  QCOMPARE(sheetCell(sheet, 1, 0), QStringLiteral("zz"));
}

void TestInteractivePreview::testTableIsOptInByDefault() {
  // A consumer which never asks for it must not get a preview which can
  // rewrite the document.
  auto textConfig = QSharedPointer<TextEditorConfig>::create();
  auto config = QSharedPointer<MarkdownEditorConfig>::create(textConfig);
  QVERIFY(!(config->m_inplacePreviewSources & MarkdownEditorConfig::Table));

  VMarkdownEditor editor(config, QSharedPointer<TextEditorParameters>::create());
  setTextAndSettle(editor, QLatin1String(c_table));
  QVERIFY(previewWidgets(editor).isEmpty());
}

void TestInteractivePreview::testMultiLineImageIsStandalone() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Image});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));

  // The construct spans two source lines but nothing else shares them, which
  // the painted path classifies as a block image. The widget path has to place
  // it the same way, not as an inline element above the line.
  setTextAndSettle(editor, QStringLiteral("![alt\ntext](img.png)\n"));

  QCOMPARE(factory->m_widgets.size(), 1);
  auto preview = factory->m_widgets.first()->m_preview;
  QVERIFY(preview);
  QCOMPARE(preview->placement(), PreviewPlacement::BlockAfterSource);
}

void TestInteractivePreview::testTableSheetRefitsAfterFontChange() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.resize(600, 400);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  setTextAndSettle(editor, QStringLiteral("| header | header |\n| --- | --- |\n| haha | haha |\n"));

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto sheet = sheetView(widget);
  QVERIFY(sheet);
  QVERIFY(sheetTable(sheet));
  QCOMPARE(sheetTable(sheet)->columns(), 2);

  QTest::qWait(50);
  QCoreApplication::processEvents();

  auto fits = [&](const char *p_stage) {
    // Nothing is clipped: the table fits the band it was laid out in, and the
    // band the host reserved is tall enough for the whole document.
    QVERIFY2(tableWidth(sheet) <= sheet->viewport()->width() + 2,
             qPrintable(QStringLiteral("%1: the table is %2 wide inside a %3 viewport")
                            .arg(QLatin1String(p_stage))
                            .arg(tableWidth(sheet))
                            .arg(sheet->viewport()->width())));
    QVERIFY2(sheet->horizontalScrollBar()->maximum() == 0,
             qPrintable(QStringLiteral("%1: the sheet scrolls sideways, so a column is clipped")
                            .arg(QLatin1String(p_stage))));
    QVERIFY2(sheet->verticalScrollBar()->maximum() == 0,
             qPrintable(QStringLiteral("%1: the band is shorter than the sheet needs")
                            .arg(QLatin1String(p_stage))));
  };

  fits("initial");
  if (QTest::currentTestFailed()) {
    return;
  }

  // Growing a cell in the source, one keystroke at a time, is what an actual
  // edit session looks like. The reserved band has to keep up.
  QTextCursor cursor(editor.document());
  cursor.setPosition(editor.document()->toPlainText().indexOf(QStringLiteral("haha")) + 4);
  for (int i = 0; i < 12; ++i) {
    cursor.insertText(QStringLiteral("x"));
    settle(editor);

    fits("while typing");
    if (QTest::currentTestFailed()) {
      return;
    }
  }

  // The editor's font is what the sheet inherits, and an application may apply
  // its theme after the first measurement.
  QFont bigger = editor.getTextEdit()->font();
  bigger.setPointSize(bigger.pointSize() + 8);
  editor.getTextEdit()->setFont(bigger);

  QTest::qWait(100);
  QCoreApplication::processEvents();

  fits("after the font grew");
}

void TestInteractivePreview::testTableSheetFitsWithALargeThemeFont() {
  auto textConfig = QSharedPointer<TextEditorConfig>::create();
  textConfig->m_theme =
      Theme::createThemeFromFile(QStringLiteral(":/vtextedit/editor/data/themes/default.theme"));
  QVERIFY(textConfig->m_theme);

  auto config = QSharedPointer<MarkdownEditorConfig>::create(textConfig);
  config->m_inplacePreviewSources |= MarkdownEditorConfig::Table;
  // After construction: that is where overrideTextStyle() replaces the whole
  // generic text format with the Markdown one.
  config->overrideTextFontFamily(QStringLiteral("Courier New"));
  textConfig->m_theme->editorStyle(Theme::EditorStyle::Text).m_fontPointSize = 20;

  VMarkdownEditor editor(config, QSharedPointer<TextEditorParameters>::create());
  editor.resize(600, 400);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  setTextAndSettle(editor, QStringLiteral("| a | b |\n| --- | --- |\n| hello | hello |\n"));

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto sheet = sheetView(widget);
  QVERIFY(sheet);

  QTest::qWait(50);
  QCoreApplication::processEvents();

  // A theme font much larger than the application default is the case which
  // originally under-reserved: the sheet measured itself with the 9pt default
  // while being painted with the theme font, and the shortfall clipped the
  // contents.
  QCOMPARE(sheet->font().pointSize(), 20);

  QVERIFY2(tableWidth(sheet) <= sheet->viewport()->width() + 2,
           qPrintable(QStringLiteral("the table is %1 wide inside a %2 viewport")
                          .arg(tableWidth(sheet))
                          .arg(sheet->viewport()->width())));
  QCOMPARE(sheet->horizontalScrollBar()->maximum(), 0);
  QCOMPARE(sheet->verticalScrollBar()->maximum(), 0);
}

void TestInteractivePreview::testTableSheetUsesTheThemeGenericFont() {
  auto textConfig = QSharedPointer<TextEditorConfig>::create();
  // A private theme instance: the default one is shared process wide.
  textConfig->m_theme =
      Theme::createThemeFromFile(QStringLiteral(":/vtextedit/editor/data/themes/default.theme"));
  QVERIFY(textConfig->m_theme);

  auto config = QSharedPointer<MarkdownEditorConfig>::create(textConfig);
  config->m_inplacePreviewSources |= MarkdownEditorConfig::Table;
  // Applied after construction, which is where overrideTextStyle() replaces the
  // generic text style with the Markdown one. A comma is what discriminates the
  // two candidate rules here: overrideTextFontFamily() writes the application
  // string straight into the single already-resolved family field, so a host
  // which re-splits it on ',' would measure with "Courier New" while the style
  // sheet declares the whole string.
  config->overrideTextFontFamily(QStringLiteral("Courier New, A"));

  VMarkdownEditor editor(config, QSharedPointer<TextEditorParameters>::create());
  editor.resize(600, 400);
  setTextAndSettle(editor, QStringLiteral("| a | b |\n| --- | --- |\n| hello | hello |\n"));

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto sheet = sheetView(widget);
  QVERIFY(sheet);

  const auto &generic = editor.theme()->editorStyle(Theme::EditorStyle::Text);
  QCOMPARE(generic.m_fontFamily, QStringLiteral("Courier New, A"));

  // Before the editor is shown its style sheet has not been polished, so
  // VTextEdit::font() is still the application default. The sheet has to take
  // the font from the theme, otherwise the very first measurement - the one
  // the layout reserves space from - is made with the wrong metrics.
  QVERIFY2(sheet->font().family() == generic.m_fontFamily,
           qPrintable(QStringLiteral("sheet font %1 is not the theme's generic font %2")
                          .arg(sheet->font().family(), generic.m_fontFamily)));

  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  QTest::qWait(50);
  QCoreApplication::processEvents();

  QCOMPARE(sheet->font().family(), generic.m_fontFamily);
  QCOMPARE(sheet->font().pointSize(), editor.editorFontPointSize());

  // Zooming re-sizes the editor, and the sheet has to follow it.
  const int before = sheet->font().pointSize();
  editor.zoom(4);
  QTest::qWait(50);
  QCoreApplication::processEvents();

  QCOMPARE(sheet->font().pointSize(), editor.editorFontPointSize());
  QVERIFY2(sheet->font().pointSize() > before,
           qPrintable(QStringLiteral("point size %1 did not grow from %2")
                          .arg(sheet->font().pointSize())
                          .arg(before)));
}

// ---------------------------------------------------------------------------
// Sheet geometry
// ---------------------------------------------------------------------------

namespace {
// TablePreviewWidget::c_widthFraction, which lives in an internal header this
// target deliberately cannot reach. The sheet now spans the whole band: the
// column widths are owned by Qt's table layout, which distributes whatever
// width it is given, so there is no natural width to keep.
const qreal c_expectedWidthFraction = 1.0;

// The reserved band is a qreal rectangle turned into widget geometry with
// toAlignedRect(), which can round outwards by a pixel on either edge.
const int c_widthTolerance = 2;

// The whole width contract in one expression: whatever the contents, the sheet
// spans the text column.
int expectedSheetWidth(qreal p_available, int p_natural) {
  return qRound(qBound(p_available * c_expectedWidthFraction, qreal(p_natural), p_available));
}
} // namespace

void TestInteractivePreview::testTableSheetSpansContentWidth() {
  // Whatever the contents - a table which would be far narrower than the band,
  // and one whose single cell is far wider than it - the sheet is the band.
  const QVector<QString> tables{QStringLiteral("| a | b |\n| --- | --- |\n| c | d |\n"),
                                QStringLiteral("| h |\n| --- |\n| ") +
                                    QString(300, QLatin1Char('x')) + QStringLiteral(" |\n")};

  for (int i = 0; i < tables.size(); ++i) {
    VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
    editor.resize(600, 400);
    editor.show();
    QVERIFY(QTest::qWaitForWindowExposed(&editor));
    setTextAndSettle(editor, tables.at(i));

    auto widget = singlePreviewWidget(editor);
    QVERIFY(widget);
    QVERIFY(widget->previewContext());
    auto sheet = sheetView(widget);
    QVERIFY(sheet);

    const qreal available = widget->previewContext()->availableContentRect().width();
    QVERIFY(available > 0);

    // Width 0 on purpose: the sheet reports no natural width, so the host
    // resolves the band from preferredWidthFraction() alone.
    const int natural = widget->sizeHint().width();
    QCOMPARE(natural, 0);

    const int expected = expectedSheetWidth(available, natural);
    QVERIFY2(qAbs(widget->width() - expected) <= c_widthTolerance,
             qPrintable(QStringLiteral("table %1: sheet width %2 is not the expected %3 "
                                       "(available %4)")
                            .arg(i)
                            .arg(widget->width())
                            .arg(expected)
                            .arg(available)));

    // And the table really occupies that band rather than hugging the margin.
    QVERIFY2(tableWidth(sheet) > 0.9 * sheet->viewport()->width(),
             qPrintable(QStringLiteral("table %1: the table is %2 wide inside a %3 viewport")
                            .arg(i)
                            .arg(tableWidth(sheet))
                            .arg(sheet->viewport()->width())));

    // Even the very wide cell is wrapped into the band rather than clipped.
    QCOMPARE(sheet->horizontalScrollBar()->maximum(), 0);
    QCOMPARE(sheet->verticalScrollBar()->maximum(), 0);
  }
}

void TestInteractivePreview::testClickEditsACellInPlace() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.resize(1100, 500);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));

  setTextAndSettle(editor, QStringLiteral("| Left | Center | Right |\n"
                                          "|:-----|:------:|------:|\n"
                                          "| *italic* | **bold** | `code` |\n"));
  QTest::qWait(50);
  QCoreApplication::processEvents();

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto sheet = sheetView(widget);
  QVERIFY(sheet);
  QVERIFY(!sheet->isReadOnly());

  // One click inside the embedded sheet, near the start of the cell's text.
  const QRect cell = sheetCellRect(sheet, 1, 1);
  QVERIFY(cell.isValid());
  const QPoint pos(cell.left() + 2, cell.center().y());
  QTest::mouseClick(sheet->viewport(), Qt::LeftButton, Qt::NoModifier, pos);
  QTest::qWait(50);
  QCoreApplication::processEvents();

  // There is no edit mode and no cell editor: the click put the one caret at
  // the exact character under the pointer.
  const QTextCursor cursor = sheet->textCursor();
  QVERIFY2(cursor.currentTable(), "the click left the caret outside the table");
  const QTextTableCell hit = cursor.currentTable()->cellAt(cursor);
  QCOMPARE(hit.row(), 1);
  QCOMPARE(hit.column(), 1);
  QCOMPARE(sheetCell(sheet, 1, 1), QStringLiteral("**bold**"));

  // Nothing selected, so the value is not sitting there waiting to be wiped by
  // the next keystroke.
  QVERIFY(cursor.selectedText().isEmpty());
  QCOMPARE(cursor.position(), sheet->cursorForPosition(pos).position());
  QVERIFY2(cursor.position() < hit.lastCursorPosition().position(),
           qPrintable(QStringLiteral("the caret went to the end (%1) rather than the click")
                          .arg(cursor.position())));

  // Typing inserts at the caret, and the flush reaches the source.
  QTest::keyClicks(sheet, QStringLiteral("X"));
  QCOMPARE(sheetCell(sheet, 1, 1).size(), QStringLiteral("**bold**").size() + 1);
  flushSheet(sheet);

  QVERIFY2(editor.getTextEdit()->toPlainText().contains(QStringLiteral("bold")),
           "the committed cell lost its value");
  QVERIFY2(editor.getTextEdit()->toPlainText().contains(QLatin1Char('X')),
           "the typed character never reached the source");
}

void TestInteractivePreview::testTableSheetHeightMatchesItsRows() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.resize(420, 600);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));

  // Cells far longer than the column they can be given: the sheet compresses
  // the columns into the text column and wraps what no longer fits, so the
  // rows have to be measured against the widths they actually got.
  setTextAndSettle(
      editor,
      QStringLiteral("| Component | Description | Supported |\n"
                     "|-----------|-------------|:---------:|\n"
                     "| `VTextEdit` | Base edit widget with cursor and selection | Yes |\n"
                     "| `VTextEditor` | Adds syntax highlight, Vi mode and folding | Yes |\n"
                     "| `VMarkdownEditor` | Markdown parsing and in-place preview | Yes |\n"));
  QTest::qWait(50);
  QCoreApplication::processEvents();

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto sheet = sheetView(widget);
  QVERIFY(sheet);
  QVERIFY(sheetTable(sheet));
  QCOMPARE(sheetTable(sheet)->rows(), 4);

  qreal shortest = std::numeric_limits<qreal>::max();
  qreal tallest = 0;
  for (qreal height : rowHeights(sheet)) {
    shortest = qMin(shortest, height);
    tallest = qMax(tallest, height);
  }

  // The whole point of the narrow editor: a description cell no longer fits on
  // one line and is shown in full instead of being elided.
  QVERIFY2(
      tallest >= 2 * shortest,
      qPrintable(
          QStringLiteral("nothing wrapped: tallest %1, shortest %2").arg(tallest).arg(shortest)));

  // The band the host reserved is exactly the sheet, and the sheet is exactly
  // the document: no empty strip under the last row, and nothing clipped.
  QCOMPARE(widget->height(), sheet->height());
  QCOMPARE(sheet->verticalScrollBar()->maximum(), 0);
  QVERIFY2(totalRowHeight(sheet) <= sheet->viewport()->height(),
           qPrintable(QStringLiteral("the rows total %1 inside a %2 viewport")
                          .arg(totalRowHeight(sheet))
                          .arg(sheet->viewport()->height())));

  // Given room, the same table stops wrapping and the band shrinks with it.
  const int tallBand = widget->height();
  editor.resize(1100, 600);
  settle(editor);
  QTest::qWait(50);
  QCoreApplication::processEvents();

  qreal wideShortest = std::numeric_limits<qreal>::max();
  qreal wideTallest = 0;
  for (qreal height : rowHeights(sheet)) {
    wideShortest = qMin(wideShortest, height);
    wideTallest = qMax(wideTallest, height);
  }

  QVERIFY2(wideTallest < 2 * wideShortest,
           qPrintable(QStringLiteral("a row still wraps at full width: tallest %1, shortest %2")
                          .arg(wideTallest)
                          .arg(wideShortest)));
  QVERIFY2(
      widget->height() < tallBand,
      qPrintable(
          QStringLiteral("the band did not shrink: %1 vs %2").arg(widget->height()).arg(tallBand)));
  QCOMPARE(sheet->verticalScrollBar()->maximum(), 0);
}

void TestInteractivePreview::testTableSheetKeepsANaturalWidthInsideTheBand() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.resize(600, 400);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  setTextAndSettle(editor,
                   QStringLiteral("| header one | header two |\n| --- | --- |\n"
                                  "| a fairly long cell value | another long cell value |\n"));

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto sheet = sheetView(widget);
  QVERIFY(sheet);

  // The band is the width, and the contents follow it: the sheet reports no
  // natural width of its own, so a table whose contents happen to be narrower
  // or wider than the band gets exactly the same treatment.
  for (int width : {600, 900}) {
    editor.resize(width, 400);
    settle(editor);
    QTest::qWait(50);
    QCoreApplication::processEvents();

    const qreal available = widget->previewContext()->availableContentRect().width();
    QVERIFY(available > 0);
    QCOMPARE(widget->sizeHint().width(), 0);
    QVERIFY2(qAbs(widget->width() - qRound(available)) <= c_widthTolerance,
             qPrintable(QStringLiteral("sheet width %1 is not the band %2 at editor width %3")
                            .arg(widget->width())
                            .arg(available)
                            .arg(width)));

    // And the table was laid out for that band rather than clipped inside it.
    QVERIFY2(tableWidth(sheet) <= sheet->viewport()->width() + c_widthTolerance,
             qPrintable(QStringLiteral("the table is %1 wide inside a %2 viewport")
                            .arg(tableWidth(sheet))
                            .arg(sheet->viewport()->width())));
    QCOMPARE(sheet->horizontalScrollBar()->maximum(), 0);
  }
}

void TestInteractivePreview::testInlinePreviewIgnoresTheWidthFraction() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Image});
  factory->m_hint = QSize(40, 20);
  factory->m_widthFraction = c_expectedWidthFraction;
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));

  editor.resize(600, 400);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  setTextAndSettle(editor, QStringLiteral("lead ![a](b.png) trail\n"));

  QCOMPARE(factory->m_widgets.size(), 1);
  auto widget = factory->m_widgets.first();
  QVERIFY(widget->m_preview);
  QCOMPARE(widget->m_preview->placement(), PreviewPlacement::InlineAboveLine);

  // An inline preview is bound to the text span it replaces, whatever fraction
  // it declares. The host restricts the fill by placement rather than by "the
  // span width came back unset", because an inline span whose width could not
  // be resolved reaches the same branch and must not be widened either - that
  // sub-case is prevented by construction, not covered here.
  const qreal available = widget->previewContext()->availableContentRect().width();
  QVERIFY(available > 0);
  QVERIFY2(widget->width() < available * c_expectedWidthFraction,
           qPrintable(QStringLiteral("the inline preview was widened to %1 of an available %2")
                          .arg(widget->width())
                          .arg(available)));
}

void TestInteractivePreview::testTableColumnsShareTheExtraWidth() {
  // Equal contents must stay equal, and unequal contents must keep their
  // order: a layout which hands the whole band to one column breaks both.
  const QVector<QString> tables{
      QStringLiteral("| aaaa | aaaa |\n| --- | --- |\n| bbbb | bbbb |\n"),
      QStringLiteral("| aaaaaaaaaaaaaaaa | b |\n| --- | --- |\n| cccccccccccccccc | d |\n")};

  for (int i = 0; i < tables.size(); ++i) {
    VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
    editor.resize(600, 400);
    editor.show();
    QVERIFY(QTest::qWaitForWindowExposed(&editor));
    setTextAndSettle(editor, tables.at(i));

    auto widget = singlePreviewWidget(editor);
    QVERIFY(widget);
    auto sheet = sheetView(widget);
    QVERIFY(sheet);
    QVERIFY(sheetTable(sheet));
    QCOMPARE(sheetTable(sheet)->columns(), 2);

    QTest::qWait(50);
    QCoreApplication::processEvents();

    QVERIFY2(tableWidth(sheet) > 0.9 * sheet->viewport()->width(),
             qPrintable(QStringLiteral("table %1: the table is %2 wide inside a %3 viewport")
                            .arg(i)
                            .arg(tableWidth(sheet))
                            .arg(sheet->viewport()->width())));

    const QVector<qreal> widths = columnWidths(sheet);
    QCOMPARE(widths.size(), 2);
    if (i == 0) {
      QVERIFY2(qAbs(widths.at(0) - widths.at(1)) <= c_widthTolerance,
               qPrintable(QStringLiteral("equal columns were distributed unequally: %1 vs %2")
                              .arg(widths.at(0))
                              .arg(widths.at(1))));
    } else {
      QVERIFY2(widths.at(0) > widths.at(1),
               qPrintable(QStringLiteral("the wide column %1 did not stay wider than %2")
                              .arg(widths.at(0))
                              .arg(widths.at(1))));
    }
  }
}

void TestInteractivePreview::testSingleColumnTableFillsTheSheet() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.resize(600, 400);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  setTextAndSettle(editor, QStringLiteral("| only |\n| --- |\n| a |\n"));

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto sheet = sheetView(widget);
  QVERIFY(sheet);
  QVERIFY(sheetTable(sheet));
  QCOMPARE(sheetTable(sheet)->columns(), 1);

  QTest::qWait(50);
  QCoreApplication::processEvents();

  // The degenerate case: the band is still the whole text column, and the one
  // column still covers it.
  const qreal available = widget->previewContext()->availableContentRect().width();
  QVERIFY2(qAbs(widget->width() - qRound(available)) <= c_widthTolerance,
           qPrintable(QStringLiteral("the sheet is %1 wide inside a band of %2")
                          .arg(widget->width())
                          .arg(available)));
  QVERIFY2(tableWidth(sheet) > 0.9 * sheet->viewport()->width(),
           qPrintable(QStringLiteral("the only column %1 does not cover the viewport %2")
                          .arg(tableWidth(sheet))
                          .arg(sheet->viewport()->width())));
}

void TestInteractivePreview::testTableWidthFollowsEditorResize() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.resize(600, 400);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  setTextAndSettle(editor, QStringLiteral("| a | b |\n| --- | --- |\n| c | d |\n"));

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto sheet = sheetView(widget);
  QVERIFY(sheet);

  const qreal availableBefore = widget->previewContext()->availableContentRect().width();
  const int widthBefore = widget->width();

  editor.resize(900, 400);
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  settle(editor);
  QTest::qWait(50);
  QCoreApplication::processEvents();

  const qreal available = widget->previewContext()->availableContentRect().width();
  QVERIFY2(available > availableBefore,
           qPrintable(QStringLiteral("the text column did not grow: %1 -> %2")
                          .arg(availableBefore)
                          .arg(available)));

  // Both the re-measurement against the new width basis and the relayout of
  // the table inside it have to re-run.
  const int expected = expectedSheetWidth(available, widget->sizeHint().width());
  QVERIFY2(qAbs(widget->width() - expected) <= c_widthTolerance,
           qPrintable(QStringLiteral("sheet width %1 is not the expected %2 after the resize "
                                     "(was %3)")
                          .arg(widget->width())
                          .arg(expected)
                          .arg(widthBefore)));
  QVERIFY2(tableWidth(sheet) > 0.9 * sheet->viewport()->width(),
           qPrintable(QStringLiteral("the table is %1 wide inside a %2 viewport after the resize")
                          .arg(tableWidth(sheet))
                          .arg(sheet->viewport()->width())));
}

// ---------------------------------------------------------------------------
// Debounced write-back against the host's item lifecycle
// ---------------------------------------------------------------------------

void TestInteractivePreview::testRemovalDuringTheDebounceKeepsTheEdit() {
  // The sheet holds an edit for 400 ms before writing it back. If the host
  // drops the identity in that window, the flush the removal triggers arrives
  // after the identity is gone and is rejected as an UnknownIdentity, silently
  // losing the edit - unless the host flushes first, while the context and the
  // anchor are still authoritative.
  for (bool focused : {false, true}) {
    VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
    editor.resize(600, 400);
    editor.show();
    QVERIFY(QTest::qWaitForWindowExposed(&editor));
    setTextAndSettle(editor, QLatin1String(c_table));

    auto widget = singlePreviewWidget(editor);
    QVERIFY(widget);
    auto sheet = sheetView(widget);
    QVERIFY(sheet);

    if (focused) {
      sheet->setFocus();
      QCoreApplication::processEvents();
    }

    WarningRecorder recorder;

    editCell(sheet, 1, 0, QStringLiteral("kept"));
    // Still inside the idle window: nothing has been written back yet.
    QVERIFY2(!editor.document()->toPlainText().contains(QStringLiteral("kept")),
             "the edit was written back before the debounce elapsed");

    // Deleting the table's source removes the item.
    QTextCursor cursor(editor.document());
    cursor.movePosition(QTextCursor::Start);
    cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
    cursor.insertText(QStringLiteral("plain paragraph\n"));
    settle(editor);
    QTest::qWait(c_commitDebounceMs + 200);
    QCoreApplication::processEvents();

    QVERIFY(previewWidgets(editor).isEmpty());
    // The edit was flushed before the identity was dropped, so the removal is
    // the only thing which reached the document afterwards - and no late
    // request was rejected.
    QVERIFY2(!recorder.contains(QStringLiteral("UnknownIdentity")),
             qPrintable(recorder.m_messages.join(QLatin1Char('\n'))));
    QCOMPARE(editor.document()->toPlainText(), QStringLiteral("plain paragraph\n"));
  }
}

void TestInteractivePreview::testRebuildDuringTheDebounceKeepsTheEdit() {
  // Same window, but the item is rebuilt rather than removed: registering a
  // factory replays the whole generation through removeItem()/createItem().
  for (bool focused : {false, true}) {
    VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
    editor.resize(600, 400);
    editor.show();
    QVERIFY(QTest::qWaitForWindowExposed(&editor));
    setTextAndSettle(editor, QLatin1String(c_table));

    auto widget = singlePreviewWidget(editor);
    QVERIFY(widget);
    auto sheet = sheetView(widget);
    QVERIFY(sheet);

    if (focused) {
      sheet->setFocus();
      QCoreApplication::processEvents();
    }

    // The rebuild's own trace is the only place the carry is observable: the
    // end states converge, because the flush edits the document and the parse
    // generation that follows recreates whatever the replay dropped.
    QLoggingCategory::setFilterRules(QStringLiteral("vte.preview.host=true"));
    WarningRecorder recorder;

    editCell(sheet, 1, 0, QStringLiteral("kept"));
    QVERIFY(!editor.document()->toPlainText().contains(QStringLiteral("kept")));

    auto other = new RecordingPreviewFactory({PreviewElementType::Image});
    QVERIFY(editor.registerPreviewWidgetFactory(other, 1));
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    QTest::qWait(c_commitDebounceMs + 200);
    QCoreApplication::processEvents();

    const bool droppedToThePaintedPath =
        recorder.contains(QStringLiteral("carried across the rebuild is gone"));
    const bool lateRejection = recorder.contains(QStringLiteral("UnknownIdentity"));
    QLoggingCategory::setFilterRules(QString());

    // Neither data loss nor a late rejected request.
    QVERIFY2(editor.document()->toPlainText().contains(QStringLiteral("kept")),
             qPrintable(editor.document()->toPlainText()));
    QVERIFY2(!lateRejection, qPrintable(recorder.m_messages.join(QLatin1Char('\n'))));

    // And the replayed generation carried the item rather than dropping it. A
    // rebuild snapshots the live anchors before replaying, and an accepted
    // flush collapses the anchor it applies over - so a snapshot taken before
    // that flush leaves the element on the painted path until the next parse
    // generation resurrects it.
    QVERIFY2(!droppedToThePaintedPath,
             "the rebuild dropped the sheet and relied on the next parse to bring it back");

    // The rebuilt sheet shows the committed value.
    settle(editor);
    auto rebuilt = singlePreviewWidget(editor);
    QVERIFY(rebuilt);
    QCOMPARE(sheetCell(sheetView(rebuilt), 1, 0), QStringLiteral("kept"));
  }
}

void TestInteractivePreview::testEditorDestructionFlushesADirtySheet() {
  // The host is an ordinary QObject child, and QObject destroys its children
  // in creation order - which puts the text edit, its viewport and every
  // preview widget *before* the host. Unless the editor destroys the host
  // itself, its pre-removal flush runs after everything it needs is gone and
  // an edit still inside the debounce window is silently dropped.
  for (bool focused : {false, true}) {
    auto editor = new VMarkdownEditor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
    editor->resize(600, 400);
    editor->show();
    QVERIFY(QTest::qWaitForWindowExposed(editor));
    setTextAndSettle(*editor, QLatin1String(c_table));

    auto widget = singlePreviewWidget(*editor);
    QVERIFY(widget);
    auto sheet = sheetView(widget);
    QVERIFY(sheet);

    if (focused) {
      sheet->setFocus();
      QCoreApplication::processEvents();
    }

    // The document dies with the editor, so what it held has to be sampled
    // while it is still alive.
    QString lastText;
    QObject sink;
    auto doc = editor->document();
    QObject::connect(doc, &QTextDocument::contentsChanged, &sink,
                     [doc, &lastText]() { lastText = doc->toPlainText(); });

    editCell(sheet, 1, 0, QStringLiteral("kept"));
    QVERIFY(!editor->document()->toPlainText().contains(QStringLiteral("kept")));

    delete editor;
    QCoreApplication::processEvents();

    QVERIFY2(lastText.contains(QStringLiteral("kept")), qPrintable(lastText));
  }
}

void TestInteractivePreview::testUndoReachesTheEditorAfterTheFlush() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.resize(600, 400);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  setTextAndSettle(editor, QLatin1String(c_table) + QStringLiteral("\ntrailing\n"));

  // An unrelated earlier operation on the editor's own undo stack. Forwarding
  // Ctrl+Z straight through while a table edit is still inside the debounce
  // window would undo *this*, and the pending edit would then be committed on
  // top of whatever the undo restored.
  QTextCursor cursor(editor.document());
  cursor.movePosition(QTextCursor::End);
  cursor.insertText(QStringLiteral("unrelated\n"));
  settle(editor);
  QVERIFY(editor.document()->toPlainText().contains(QStringLiteral("unrelated")));

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto sheet = sheetView(widget);
  QVERIFY(sheet);

  editCell(sheet, 1, 0, QStringLiteral("typed"));
  QVERIFY(!editor.document()->toPlainText().contains(QStringLiteral("typed")));

  // Pressed inside the idle window: the flush goes first, so the undo the
  // editor performs is the table commit, not the unrelated insertion.
  QTest::keyClick(sheet, Qt::Key_Z, Qt::ControlModifier);
  QCoreApplication::processEvents();

  const QString after = editor.document()->toPlainText();
  QVERIFY2(after.contains(QStringLiteral("unrelated")), qPrintable(after));
  QVERIFY2(!after.contains(QStringLiteral("typed")), qPrintable(after));
  QVERIFY2(after.contains(QStringLiteral("| a | b |")) ||
               after.contains(QStringLiteral("| a   | b   |")),
           qPrintable(after));
}

void TestInteractivePreview::testArrowOutMovesTheEditorCaretToTheLiveAnchor() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.resize(600, 400);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  setTextAndSettle(editor, QStringLiteral("head\n\n") + QLatin1String(c_table) +
                               QStringLiteral("\ntail\n"));

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto sheet = sheetView(widget);
  QVERIFY(sheet);

  // An unrelated edit before the table moves the source. The sheet only knows
  // its snapshot coordinates, which are stale by now, so the destination has
  // to be resolved from the host's live anchor at delivery time.
  QTextCursor cursor(editor.document());
  cursor.setPosition(0);
  cursor.insertText(QStringLiteral("prefix "));
  QCoreApplication::processEvents();

  const QString text = editor.document()->toPlainText();
  const int sourceStart = text.indexOf(QStringLiteral("| h1"));
  QVERIFY(sourceStart > 0);
  const int sourceEnd =
      text.indexOf(QStringLiteral("| a | b |")) + QStringLiteral("| a | b |").size();
  QVERIFY(sourceEnd > sourceStart);

  // Up out of the first row. The source is rendered above the sheet, so "up"
  // is the end of the source.
  putCaretIn(sheet, 0, 1);
  sheet->setFocus();
  QTest::keyClick(sheet, Qt::Key_Up);
  QCoreApplication::processEvents();

  QVERIFY2(editor.getTextEdit()->hasFocus(), "the editor did not take the focus back");
  QCOMPARE(editor.getTextEdit()->textCursor().position(), sourceEnd);

  // Down out of the last row lands just past the source's own line.
  sheet->setFocus();
  putCaretIn(sheet, 1, 1);
  QTest::keyClick(sheet, Qt::Key_Down);
  QCoreApplication::processEvents();

  QVERIFY(editor.getTextEdit()->hasFocus());
  QCOMPARE(editor.getTextEdit()->textCursor().position(), sourceEnd + 1);
}

// ---------------------------------------------------------------------------
// Review fixes
// ---------------------------------------------------------------------------

void TestInteractivePreview::testRejectionAfterAcceptKeepsCommittedValues() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  setTextAndSettle(editor, QLatin1String(c_table));

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto context = widget->previewContext();
  QVERIFY(context);
  auto sheet = sheetView(widget);
  QVERIFY(sheet);

  // An accepted commit rebases the binding onto the text now in the document.
  editCell(sheet, 1, 1, QStringLiteral("committed"));
  flushSheet(sheet);
  QVERIFY(editor.document()->toPlainText().contains(QStringLiteral("committed")));
  const QString afterCommit = editor.document()->toPlainText();

  // A non-authoritative rejection makes the sheet restore itself from the
  // binding. The widget's own cached snapshot still describes the pre-commit
  // source, so restoring from it would revert the accepted commit.
  context->requestSourceReplacement(QStringLiteral("   "));
  QCOMPARE(sheetCell(sheet, 1, 1), QStringLiteral("committed"));
  QCOMPARE(editor.document()->toPlainText(), afterCommit);

  // And the next commit must not write the reverted matrix back: it would pass
  // the source check (both texts equal the rebased source) and be accepted.
  editCell(sheet, 1, 0, QStringLiteral("second"));
  flushSheet(sheet);
  const QString after = editor.document()->toPlainText();
  QVERIFY2(after.contains(QStringLiteral("committed")), qPrintable(after));
  QVERIFY2(after.contains(QStringLiteral("second")), qPrintable(after));
}

void TestInteractivePreview::testReconcileIsDeferredDuringWidgetCallback() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QLatin1String(c_table));

  // Drain every queued pass of the setup before taking a baseline.
  QTest::qWait(50);
  QCoreApplication::processEvents();
  QCOMPARE(factory->m_widgets.size(), 1);

  auto widget = factory->m_widgets.first();
  const int createdBefore = factory->m_createCount;

  int observedCreateCount = -1;
  PreviewWidget *observedWidget = nullptr;
  widget->m_duringSpin = [&]() {
    observedCreateCount = factory->m_createCount;
    observedWidget = singlePreviewWidget(editor);
  };
  widget->m_spinOnNextSetPreview = true;

  // From outside any callback, so the registration is not rejected as
  // reentrant. A rebuild is now owed and one delivery is queued.
  auto second = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(second, 1));

  // Registration edits no document, so this re-emits synchronously and the
  // host reaches setPreview() before the queued delivery gets an ordinary
  // event loop turn.
  editor.getHighlighter()->updateHighlight();
  QCOMPARE(widget->m_spinCount, 1);

  // The rebuild must not have run while the callback was on the stack.
  QCOMPARE(observedCreateCount, createdBefore);
  QCOMPARE(observedWidget, static_cast<PreviewWidget *>(widget));

  // Once the callback returns, the owed rebuild is delivered exactly once.
  QTest::qWait(50);
  QCoreApplication::processEvents();
  QCOMPARE(factory->m_createCount, createdBefore + 1);
  QCOMPARE(previewWidgets(editor).size(), 1);

  auto rebuilt = qobject_cast<RecordingPreviewWidget *>(singlePreviewWidget(editor));
  QVERIFY(rebuilt);
  QVERIFY(rebuilt->previewContext());
  QCOMPARE(rebuilt->previewContext()->preview()->type(), PreviewElementType::Table);
}

void TestInteractivePreview::testBlockedReconcileIsNotRearmedWhileBlocked() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QLatin1String(c_table));

  QTest::qWait(50);
  QCoreApplication::processEvents();
  QCOMPARE(factory->m_widgets.size(), 1);

  auto widget = factory->m_widgets.first();
  const int createdBefore = factory->m_createCount;
  widget->m_deliveryCounterSource = previewHost(editor);
  QVERIFY(widget->m_deliveryCounterSource);
  widget->m_spinOnNextSetPreview = true;

  auto second = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(second, 1));

  editor.getHighlighter()->updateHighlight();
  QCOMPARE(widget->m_spinCount, 1);

  // A delivery which finds the host blocked is consumed once and left to an
  // unblock hook. Re-arming from the blocked branch instead would spin: the
  // nested loop keeps delivering each newly armed zero timer while the flag it
  // waits on can only be cleared by the frame that loop is holding open.
  QVERIFY(widget->m_deliveriesBeforeSpin >= 0);
  QVERIFY(widget->m_deliveriesAfterSpin >= widget->m_deliveriesBeforeSpin);
  const int delta = widget->m_deliveriesAfterSpin - widget->m_deliveriesBeforeSpin;
  QVERIFY2(delta <= 1, qPrintable(QStringLiteral("%1 deliveries ran while blocked").arg(delta)));

  // The owed rebuild is not lost either.
  QTest::qWait(50);
  QCoreApplication::processEvents();
  QCOMPARE(factory->m_createCount, createdBefore + 1);
  QVERIFY(reconcileDeliveries(editor) > widget->m_deliveriesAfterSpin);
}

void TestInteractivePreview::testMeasurementIsNotRepeatedWhenNothingChanged() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));

  editor.resize(600, 400);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  setTextAndSettle(editor, QLatin1String(c_table));

  QCOMPARE(factory->m_widgets.size(), 1);
  auto widget = factory->m_widgets.first();

  // The widget is placed after its source block, so the width it is measured
  // at is the available content width, while the width it reports is its own.
  // Caching the derived width as the guard key never compares equal.
  QCOMPARE(widget->m_preview->placement(), PreviewPlacement::BlockAfterSource);

  auto host = previewHost(editor);
  QVERIFY(host);

  // Drain the queued layout requests of the setup before taking a baseline.
  QTest::qWait(100);
  QCoreApplication::processEvents();
  const int baseline = widget->m_sizeHintCount;
  QVERIFY(baseline > 0);

  for (int i = 0; i < 5; ++i) {
    QVERIFY(QMetaObject::invokeMethod(host, "schedulePublish", Qt::DirectConnection));
    QTest::qWait(20);
    QCoreApplication::processEvents();
  }

  QVERIFY2(widget->m_sizeHintCount == baseline,
           qPrintable(QStringLiteral("measured %1 more time(s) although nothing changed")
                          .arg(widget->m_sizeHintCount - baseline)));
}

void TestInteractivePreview::testWideCellSurvivesRoundTrip() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  setTextAndSettle(editor, QLatin1String(c_table));

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto sheet = sheetView(widget);
  QVERIFY(sheet);

  // A very long cell is emitted verbatim and never widens any other row: the
  // canonical form is compact.
  const QString wide = QString(200, QLatin1Char('w'));
  editCell(sheet, 1, 0, wide);
  flushSheet(sheet);
  QVERIFY(editor.document()->toPlainText().contains(wide));

  const auto before = widget->previewContext()->preview().staticCast<const TablePreview>();
  QVERIFY(before);

  settle(editor);

  auto reparsed = singlePreviewWidget(editor);
  QVERIFY(reparsed);
  auto after = reparsed->previewContext()->preview().staticCast<const TablePreview>();
  QVERIFY(after);

  QCOMPARE(after->cells(), before->cells());
  QCOMPARE(after->alignments(), before->alignments());
  QCOMPARE(after->cells().value(1).value(0), wide);
}

void TestInteractivePreview::testRebasedTableMatchesGenerationSnapshot() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QStringLiteral("> | h1 | h2 |\n> | :-- | --: |\n> | a | b |\n"));

  QCOMPARE(factory->m_widgets.size(), 1);
  auto widget = factory->m_widgets.first();

  widget->previewContext()->requestSourceReplacement(
      QStringLiteral("> | h1 | h2 |\n> | :-- | --: |\n> | z | b |"));
  QCOMPARE(widget->m_lastResult.status(), PreviewReplacementResult::Accepted);

  auto rebased = widget->previewContext()->preview().staticCast<const TablePreview>();
  QVERIFY(rebased);

  // The next parse generation describes the very same text. A second copy of
  // the element-to-snapshot conversion is how the two drift apart.
  settle(editor);

  auto generated = widget->previewContext()->preview().staticCast<const TablePreview>();
  QVERIFY(generated);
  QVERIFY(generated != rebased);

  QCOMPARE(generated->cells(), rebased->cells());
  QCOMPARE(generated->rowPrefixes(), rebased->rowPrefixes());
  QCOMPARE(generated->delimiterPrefix(), rebased->delimiterPrefix());
  QCOMPARE(generated->alignments(), rebased->alignments());
  QCOMPARE(generated->columnCount(), rebased->columnCount());
}

void TestInteractivePreview::testStaleGenerationReplayAfterShrink() {
  QString tail;
  for (int i = 0; i < 6; ++i) {
    tail += QStringLiteral("trailing paragraph %1 which stays behind\n").arg(i);
  }

  // (a) A snapshot which never became an item - its factory declined - is
  // still replayed from the parse generation, and nothing carried an anchor
  // for it, so its coordinates go through makeAnchor() unmediated.
  {
    WarningRecorder warnings;

    VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
    auto declining = new RecordingPreviewFactory({PreviewElementType::Image});
    declining->m_decline = true;
    QVERIFY(editor.registerPreviewWidgetFactory(declining, 5));

    setTextAndSettle(editor, QStringLiteral("![a](b.png)\n\n") + tail);
    QVERIFY(declining->m_createCount > 0);
    QVERIFY(previewWidgets(editor).isEmpty());

    // Truncate below the replayed coordinates, without letting a new parse
    // generation land, then force the replay through the queued rebuild.
    QTextCursor cursor(editor.document());
    cursor.select(QTextCursor::Document);
    cursor.insertText(QStringLiteral("ab\n"));

    auto other = new RecordingPreviewFactory({PreviewElementType::Table});
    QVERIFY(editor.registerPreviewWidgetFactory(other, 1));
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();

    QVERIFY(previewWidgets(editor).isEmpty());
    QVERIFY2(!warnings.contains(QStringLiteral("QTextCursor::setPosition")),
             qPrintable(warnings.m_messages.join(QLatin1Char('\n'))));
  }

  // (b) A live item whose source is deleted from the middle. Its carried
  // anchor collapses, which is evidence the tracked source is gone - not the
  // same as having no carried state. The replayed coordinates are still
  // numerically in bounds here, because the trailing text remains.
  {
    WarningRecorder warnings;

    VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
    setTextAndSettle(editor, QLatin1String(c_table) + QStringLiteral("\n") + tail);

    auto widget = singlePreviewWidget(editor);
    QVERIFY(widget);
    const int start = widget->previewContext()->preview()->startPos();
    const int end = widget->previewContext()->preview()->endPos();

    // Delete exactly the table, so its anchor collapses while the stale
    // [start, end) range still resolves against the trailing text.
    QTextCursor cursor(editor.document());
    cursor.setPosition(start);
    cursor.setPosition(end, QTextCursor::KeepAnchor);
    cursor.removeSelectedText();
    QVERIFY(editor.document()->characterCount() - 1 > end);

    // Force the replay without a new parse generation.
    auto other = new RecordingPreviewFactory({PreviewElementType::Image});
    QVERIFY(editor.registerPreviewWidgetFactory(other, 1));
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();

    // No item may be recreated: neither over a collapsed range, nor over the
    // unrelated text which now occupies the replayed coordinates.
    const auto live = previewWidgets(editor);
    QVERIFY2(live.isEmpty(),
             qPrintable(QStringLiteral("%1 preview(s) survived the deletion of their source")
                            .arg(live.size())));
    QVERIFY2(!warnings.contains(QStringLiteral("QTextCursor::setPosition")),
             qPrintable(warnings.m_messages.join(QLatin1Char('\n'))));
  }
}

// ---------------------------------------------------------------------------
// Local review fixes
// ---------------------------------------------------------------------------

void TestInteractivePreview::testUnregisteringTheBuiltinFactoryIsSafe() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  setTextAndSettle(editor, QLatin1String(c_table));
  QVERIFY(singlePreviewWidget(editor));

  auto host = previewHost(editor);
  QVERIFY(host);

  // The built-in table renderer is an ordinary registered factory, so the
  // public API can unregister it like any other. The host keeps a direct
  // reference to it for the per-publish read-only push, which must not outlive
  // the object.
  const auto factories =
      host->findChildren<PreviewWidgetFactory *>(QString(), Qt::FindDirectChildrenOnly);
  QCOMPARE(factories.size(), 1);
  QPointer<PreviewWidgetFactory> guard(factories.first());

  QVERIFY(editor.unregisterPreviewWidgetFactory(factories.first()));
  QTest::qWait(50);
  QCoreApplication::processEvents();
  QVERIFY(guard.isNull());
  QVERIFY(previewWidgets(editor).isEmpty());

  // Every path which used to touch the built-in factory directly.
  editor.getTextEdit()->setReadOnly(true);
  editor.getTextEdit()->setReadOnly(false);
  QVERIFY(QMetaObject::invokeMethod(host, "schedulePublish", Qt::DirectConnection));
  settle(editor);

  // Nothing renders the table anymore, and the editor is still usable.
  QVERIFY(previewWidgets(editor).isEmpty());
  QVERIFY(editor.document()->toPlainText().contains(QStringLiteral("| h1 | h2 |")));
}

void TestInteractivePreview::testReentrantRegistrationFromSupportedTypesRejected() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Image});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QLatin1String(c_table));

  // supportedTypes() is reached from the registry scan, not only from widget
  // construction. That scan must mark the span as a callback too, otherwise a
  // factory can mutate the registry from inside the very loop walking it.
  factory->m_registerReentrantlyInSupportedTypes = &editor;

  // Re-enabling recomputes the claimable type mask, which scans the registry.
  editor.setInplacePreviewEnabled(false);
  QCoreApplication::processEvents();
  editor.setInplacePreviewEnabled(true);
  QTest::qWait(50);
  QCoreApplication::processEvents();

  QVERIFY(!factory->m_registerReentrantlyInSupportedTypes);
  QVERIFY2(!factory->m_reentrantRegistrationAccepted,
           "a factory registered itself from inside the registry scan");

  // And the host is still coherent.
  QVERIFY(singlePreviewWidget(editor));
}

void TestInteractivePreview::testGenerationDeliveredDuringCallbackIsNotLost() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QLatin1String(c_table));

  QTest::qWait(50);
  QCoreApplication::processEvents();
  QCOMPARE(factory->m_widgets.size(), 1);

  auto widget = factory->m_widgets.first();
  auto host = previewHost(editor);
  QVERIFY(host);

  const quint64 nextRevision = widget->previewContext()->preview()->revision() + 1;
  bool delivered = false;

  // A widget callback can spin a nested event loop, which delivers whatever
  // the highlighter has queued. Standing in for that here: a newer generation
  // arrives while the reconciliation pass that called us is still running.
  widget->m_duringSpin = [&]() {
    delivered = QMetaObject::invokeMethod(
        host, "updatePreviews", Qt::DirectConnection, Q_ARG(quint64, nextRevision),
        Q_ARG(QVector<QSharedPointer<const Preview>>, QVector<QSharedPointer<const Preview>>()));
  };
  widget->m_spinOnNextSetPreview = true;

  editor.getHighlighter()->updateHighlight();
  QCOMPARE(widget->m_spinCount, 1);
  QVERIFY(delivered);

  QTest::qWait(50);
  QCoreApplication::processEvents();

  // The newer generation described no elements at all. Dropping it would leave
  // the sheet bound to superseded source with nothing to ever replay it.
  QVERIFY2(previewWidgets(editor).isEmpty(),
           "the generation delivered during the callback was discarded");
}

void TestInteractivePreview::testReplacementCannotSplitATableCell() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Image});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));

  // An inline element inside a table row. A single line is not a table to
  // cmark - that needs the delimiter row - so the validation probe cannot see
  // that '|' is structural here.
  setTextAndSettle(editor, QStringLiteral("| a | ![i](x.png) |\n| --- | --- |\n| b | c |\n"));

  QVERIFY2(factory->m_widgets.size() == 1,
           qPrintable(QStringLiteral("created=%1").arg(factory->m_widgets.size())));
  auto widget = factory->m_widgets.first();
  const QString before = editor.document()->toPlainText();

  // Would split the cell and silently give the table a third column.
  widget->previewContext()->requestSourceReplacement(QStringLiteral("![i|j](y.png)"));
  QVERIFY2(!widget->m_lastResult.isAccepted(), qPrintable(editor.document()->toPlainText()));
  QCOMPARE(editor.document()->toPlainText(), before);

  // An escaped pipe is not structural, and a plain replacement is fine.
  widget->previewContext()->requestSourceReplacement(QStringLiteral("![i](z.png)"));
  QCOMPARE(widget->m_lastResult.status(), PreviewReplacementResult::Accepted);
  QVERIFY(editor.document()->toPlainText().contains(QStringLiteral("| a | ![i](z.png) |")));
}

void TestInteractivePreview::testSourceTextRectFollowsTheSource() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));

  editor.resize(600, 500);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  setTextAndSettle(editor, QLatin1String(c_table) + QStringLiteral("\ntail\n"));

  QCOMPARE(factory->m_widgets.size(), 1);
  auto context = factory->m_widgets.first()->previewContext();
  QVERIFY(context);

  QTest::qWait(50);
  QCoreApplication::processEvents();

  const QRectF before = context->sourceTextRect();
  QVERIFY(!before.isNull());

  // The rect is cached, so an edit which pushes the source down has to
  // invalidate it. A stale rect would keep reporting the old position.
  QTextCursor cursor(editor.document());
  cursor.setPosition(0);
  cursor.insertText(QStringLiteral("head\n\nlines\n\n"));
  settle(editor);

  QTest::qWait(50);
  QCoreApplication::processEvents();

  const QRectF after = context->sourceTextRect();
  QVERIFY(!after.isNull());
  QVERIFY2(after.top() > before.top(),
           qPrintable(QStringLiteral("source rect stayed at %1 after the source moved down "
                                     "(was %2)")
                          .arg(after.top())
                          .arg(before.top())));
}

// ---------------------------------------------------------------------------
// Preview driven folding
// ---------------------------------------------------------------------------

namespace {
bool blockVisible(VMarkdownEditor &p_editor, int p_blockNumber) {
  const QTextBlock block = p_editor.document()->findBlockByNumber(p_blockNumber);
  return block.isValid() && block.isVisible();
}

// The fold evaluation is coalesced through a zero timer, and folding a range
// schedules another pass, so give the chain a few turns to come to rest.
void settleFolding() {
  QTest::qWait(30);
  QCoreApplication::processEvents();
  QCoreApplication::processEvents();
}

QSharedPointer<MarkdownEditorConfig> makeAutoFoldConfig(bool p_enabled) {
  auto config = makeConfig();
  config->m_autoFoldPreviewedBlocksEnabled = p_enabled;
  return config;
}

void putEditorCaretInBlock(VMarkdownEditor &p_editor, int p_blockNumber) {
  QTextCursor cursor = p_editor.getTextEdit()->textCursor();
  cursor.setPosition(p_editor.document()->findBlockByNumber(p_blockNumber).position());
  p_editor.getTextEdit()->setTextCursor(cursor);
}

// The widget whose source starts earliest, i.e. the first table in the
// document. The host hands them out in hash order.
RecordingPreviewWidget *firstWidgetBySource(RecordingPreviewFactory *p_factory) {
  RecordingPreviewWidget *first = nullptr;
  for (auto widget : p_factory->m_widgets) {
    auto context = widget ? widget->previewContext() : nullptr;
    if (!context || !context->preview()) {
      continue;
    }

    if (!first || context->preview()->startPos() < first->previewContext()->preview()->startPos()) {
      first = widget;
    }
  }

  return first;
}

int foldRefreshes(VMarkdownEditor &p_editor) {
  auto host = previewHost(p_editor);
  return host ? host->property("vte_preview_fold_refreshes").toInt() : -1;
}

// The gutter. It is an internal widget, so it is located by its class name and
// driven with plain Qt mouse events.
QWidget *indicatorsBorder(VMarkdownEditor &p_editor) {
  const auto children = p_editor.findChildren<QWidget *>();
  for (auto child : children) {
    if (QLatin1String(child->metaObject()->className()) == QLatin1String("vte::IndicatorsBorder")) {
      return child;
    }
  }

  return nullptr;
}

// Fold or unfold the range starting on @p_blockNumber the way a user does: by
// clicking the folding marker in the gutter. This is the only way a test can
// change the live fold state without the queued refresh having written it onto
// the preview item first, which is exactly the race the rewrite path guards
// against.
//
// The marker column sits at the right edge of the border, before a two pixel
// separator; a few offsets are tried so the exact metrics do not have to be
// reproduced here. @p_probeBlock is a block the toggle changes the visibility
// of. No event is processed after the click, so the caller still sees the item
// state the last delivered refresh left behind.
bool toggleFoldFromGutter(VMarkdownEditor &p_editor, int p_blockNumber, int p_probeBlock) {
  QWidget *border = indicatorsBorder(p_editor);
  if (!border || border->width() <= 0) {
    return false;
  }

  QTextCursor cursor(p_editor.document());
  cursor.setPosition(p_editor.document()->findBlockByNumber(p_blockNumber).position());
  const int y = p_editor.getTextEdit()->cursorRect(cursor).center().y();
  const bool before = blockVisible(p_editor, p_probeBlock);

  const QVector<int> offsets{3, 2, 5, 8, 11};
  for (int offset : offsets) {
    const int x = border->width() - offset;
    if (x < 0) {
      continue;
    }

    const QPointF pos(x, y);
    const QPointF global = border->mapToGlobal(pos.toPoint());

    QMouseEvent move(QEvent::MouseMove, pos, global, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(border, &move);
    // The gutter resolves the range under the pointer on a 300 ms timer.
    QTest::qWait(350);

    QMouseEvent press(QEvent::MouseButtonPress, pos, global, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QCoreApplication::sendEvent(border, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, pos, global, Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QCoreApplication::sendEvent(border, &release);

    if (blockVisible(p_editor, p_probeBlock) != before) {
      return true;
    }
  }

  return false;
}

// Records, at every viewport paint, whether the edited element's interior block
// is visible. A fold which is dropped and restored inside one event-loop turn is
// invisible to the user; one which a paint straddles is the flash the user
// reports. Nothing else in this file states that property: the nearest test
// samples visibility only after the write-back call returns, which cannot see a
// paint forced from inside it.
class OpenSourcePaintProbe : public QObject {
public:
  OpenSourcePaintProbe(VMarkdownEditor *p_editor, int p_interiorBlock)
      : m_editor(p_editor), m_interiorBlock(p_interiorBlock) {}

  int m_paints = 0;
  int m_openPaints = 0;

protected:
  bool eventFilter(QObject *p_watched, QEvent *p_event) Q_DECL_OVERRIDE {
    if (p_event->type() == QEvent::Paint) {
      ++m_paints;
      if (blockVisible(*m_editor, m_interiorBlock)) {
        ++m_openPaints;
      }
    }

    return QObject::eventFilter(p_watched, p_event);
  }

private:
  VMarkdownEditor *m_editor = nullptr;
  int m_interiorBlock = 0;
};
} // namespace

// A previewed table comes up folded: the header row and the last row stay
// visible, and only the interior is hidden.
void TestInteractivePreview::testPreviewedTableIsFoldedOnce() {
  VMarkdownEditor editor(makeAutoFoldConfig(true), QSharedPointer<TextEditorParameters>::create());
  setTextAndSettle(editor, QLatin1String(c_table) + QStringLiteral("\ntail\n"));
  QVERIFY(singlePreviewWidget(editor));

  settleFolding();
  QVERIFY(blockVisible(editor, 0));
  QVERIFY2(!blockVisible(editor, 1), "the previewed table did not fold");
  QVERIFY(blockVisible(editor, 2));

  // The decision is taken once: a later parse must not re-run it, and the
  // widget is still there to render what the fold hides.
  settle(editor);
  settleFolding();
  QVERIFY(!blockVisible(editor, 1));
  QVERIFY(singlePreviewWidget(editor));
}

void TestInteractivePreview::testAutoFoldIsOptional() {
  VMarkdownEditor editor(makeAutoFoldConfig(false), QSharedPointer<TextEditorParameters>::create());
  setTextAndSettle(editor, QLatin1String(c_table) + QStringLiteral("\ntail\n"));
  QVERIFY(singlePreviewWidget(editor));

  settleFolding();
  QVERIFY(blockVisible(editor, 1));

  settle(editor);
  settleFolding();
  QVERIFY(blockVisible(editor, 1));
}

// Folding keeps the first and last block visible, so only a caret in the
// interior would be hidden - and such a region is left open for good.
void TestInteractivePreview::testCaretInsideKeepsTheSourceOpen() {
  VMarkdownEditor editor(makeAutoFoldConfig(true), QSharedPointer<TextEditorParameters>::create());

  // Shown before the text, so the first parse generation already has a
  // viewport to realize the preview into. See setTextAndSettle(): this test
  // cannot use that helper because the caret has to be placed BEFORE the
  // generation lands, so the setText/settle pair is spelled out here.
  editor.resize(600, 400);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));

  // The caret is placed before the parse generation lands, so the very first
  // decision sees it inside the table.
  editor.setText(QLatin1String(c_table) + QStringLiteral("\ntail\n"));
  putEditorCaretInBlock(editor, 1);
  settle(editor);
  settleFolding();

  QVERIFY(singlePreviewWidget(editor));
  QVERIFY2(blockVisible(editor, 1), "the caret's own line was folded away");

  // Moving the caret away does not re-decide it.
  putEditorCaretInBlock(editor, editor.document()->blockCount() - 1);
  settle(editor);
  settleFolding();
  QVERIFY(blockVisible(editor, 1));
}

// Editing a cell through the sheet rewrites the whole table source, which
// destroys its folding range. The source must not expand, not even for one
// event-loop turn.
void TestInteractivePreview::testFoldSurvivesASheetCellEdit() {
  auto config = makeAutoFoldConfig(true);
  VMarkdownEditor editor(config, QSharedPointer<TextEditorParameters>::create());
  setTextAndSettle(editor, QLatin1String(c_table) + QStringLiteral("\ntail\n"));

  settleFolding();
  QVERIFY(!blockVisible(editor, 1));

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto sheet = sheetView(widget);
  QVERIFY(sheet);

  // The folded-line background band is an extra selection: a collapsed cursor
  // on the folded range's first block. ExtraSelectionMgr coalesces through a
  // 200ms timer, so let it land - that is the state a user typing into an
  // already folded table starts from.
  const auto &theme = config->m_textEditorConfig->m_theme;
  QVERIFY(theme);
  const QColor foldedBackground =
      theme->editorStyle(Theme::FoldedFoldingRangeLine).backgroundColor();
  QVERIFY(foldedBackground.isValid());

  auto foldedBandBlocks = [&editor, &foldedBackground]() {
    QVector<int> blocks;
    const auto selections = editor.getTextEdit()->extraSelections();
    for (const auto &selection : selections) {
      // Discriminated by colour: the cursor line is a full-width band too.
      if (selection.cursor.hasSelection() ||
          !selection.format.hasProperty(QTextFormat::FullWidthSelection) ||
          selection.format.background().color() != foldedBackground) {
        continue;
      }
      blocks.append(selection.cursor.blockNumber());
    }
    return blocks;
  };

  QTRY_COMPARE(foldedBandBlocks(), QVector<int>() << 0);

  editCell(sheet, 1, 0, QStringLiteral("zz"));
  // Not flushSheet(): the write-back is synchronous inside the focus-out
  // delivery, and no event loop may run before the assertions below, or the
  // 200ms coalescing timer could repair the band on its own and hide a missing
  // synchronous flush.
  {
    QFocusEvent out(QEvent::FocusOut);
    QCoreApplication::sendEvent(sheet, &out);
  }
  QVERIFY(editor.document()->toPlainText().contains(QStringLiteral("zz")));

  // Immediately after the write-back, before any parse could have run.
  QVERIFY2(!blockVisible(editor, 1), "the rewritten table expanded");

  // The rewrite replaces the table in place starting exactly at the band's
  // position, so Qt drags the applied cursor past the inserted text, onto the
  // table's last block. TextFolding does rebuild the list when the range is
  // dropped and restored, but ExtraSelectionMgr would only apply it 200ms later
  // - one visible blink per keystroke - unless restoreFoldAfterPreviewRewrite()
  // flushes it synchronously. Without the flush this is empty: the caret which
  // the rewrite moved forces an intermediate apply of a list whose folded entry
  // had already been dropped.
  QCOMPARE(foldedBandBlocks(), QVector<int>() << 0);

  QCoreApplication::processEvents();

  settle(editor);
  settleFolding();
  QVERIFY(!blockVisible(editor, 1));
  QVERIFY(singlePreviewWidget(editor));
}

// A replacement may carry trailing whitespace, which the anchor spans but the
// parsed element does not. Both the query and the restore have to describe the
// element, otherwise a second rewrite issued before the next parse misses the
// live range.
void TestInteractivePreview::testFoldSurvivesARewriteWithTrailingBlankLines() {
  VMarkdownEditor editor(makeAutoFoldConfig(true), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QLatin1String(c_table) + QStringLiteral("\ntail\n"));

  settleFolding();
  QVERIFY(!blockVisible(editor, 1));
  QCOMPARE(factory->m_widgets.size(), 1);

  auto widget = factory->m_widgets.first();
  widget->previewContext()->requestSourceReplacement(
      QStringLiteral("| h1 | h2 |\n| --- | --- |\n| z | b |\n\n"));
  QVERIFY2(widget->m_lastResult.isAccepted(), qPrintable(widget->m_lastResult.diagnostic()));
  QVERIFY(editor.document()->toPlainText().contains(QStringLiteral("| z | b |")));
  QVERIFY2(!blockVisible(editor, 1), "the restored range missed the trimmed extent");
  // Block 2 is the discriminator: the trimmed element ends there, the anchor
  // spans the two trailing blank lines as well. An untrimmed restore would fold
  // [0, 3] and hide the table's last row.
  QVERIFY2(blockVisible(editor, 2), "the restore used the anchor's untrimmed extent");

  // A second rewrite before the next parse still finds the restored range.
  widget->previewContext()->requestSourceReplacement(
      QStringLiteral("| h1 | h2 |\n| --- | --- |\n| y | b |\n\n"));
  QVERIFY2(widget->m_lastResult.isAccepted(), qPrintable(widget->m_lastResult.diagnostic()));
  QVERIFY(editor.document()->toPlainText().contains(QStringLiteral("| y | b |")));
  QVERIFY(!blockVisible(editor, 1));
  QVERIFY(blockVisible(editor, 2));

  // And the next parse agrees with the extent the restore used.
  settle(editor);
  settleFolding();
  QVERIFY(!blockVisible(editor, 1));
  QVERIFY(blockVisible(editor, 2));
}

// The property the user actually reports: not "is it folded afterwards", but
// "was it ever *painted* open".
//
// This one is a floor, not the discriminator. Measurement showed it already
// held before the fix - Qt coalesces the whole edit into a single repaint, so
// no paint ever observed the interval during which the source was expanded.
// testNoDocumentSizeIsPublishedForTheOpenSource() below is what actually
// caught the flash, and what actually fails without the fix. This one pins that
// no future change starts forcing a repaint or spinning an event loop across
// the rewrite, which would make that interval visible.
void TestInteractivePreview::testNoPaintObservesTheOpenSourceDuringACellEdit() {
  auto config = makeAutoFoldConfig(true);
  VMarkdownEditor editor(config, QSharedPointer<TextEditorParameters>::create());
  editor.resize(600, 400);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));

  setTextAndSettle(editor, QLatin1String(c_table) + QStringLiteral("\ntail\n"));
  settleFolding();
  QVERIFY(!blockVisible(editor, 1));

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto sheet = sheetView(widget);
  QVERIFY(sheet);

  // Drain everything the initial fold owes before arming, so only paints caused
  // by the cell edit are counted.
  settle(editor);
  settleFolding();

  OpenSourcePaintProbe probe(&editor, 1);
  editor.getTextEdit()->viewport()->installEventFilter(&probe);

  editCell(sheet, 1, 0, QStringLiteral("zz"));
  {
    QFocusEvent out(QEvent::FocusOut);
    QCoreApplication::sendEvent(sheet, &out);
  }
  QVERIFY(editor.document()->toPlainText().contains(QStringLiteral("zz")));
  QVERIFY2(!blockVisible(editor, 1), "the rewritten table expanded");
  QVERIFY2(probe.m_openPaints == 0,
           "a paint was delivered while the rewritten source was still expanded");

  // And across the parse which follows, where updateFoldingRegions() drops and
  // recreates the range.
  settle(editor);
  settleFolding();
  QVERIFY(!blockVisible(editor, 1));

  // A zero count only means something if paints were delivered at all.
  QVERIFY2(probe.m_paints > 0, "the viewport never repainted - the probe proves nothing");
  QCOMPARE(probe.m_openPaints, 0);

  editor.getTextEdit()->viewport()->removeEventFilter(&probe);
}

// The flash the user reports, measured where it actually is.
//
// A paint never observes the expanded source - Qt coalesces the whole edit into
// one repaint - but the DOCUMENT SIZE did. TextFolding maintains itself from
// contentsChange, which Qt emits BEFORE it hands the change to the layout
// (QTextDocumentPrivate::finishEdit()), so a restore made after
// endEditBlock() returns is one layout pass too late: documentChanged() has
// already run over a document whose folded-away rows were visible again, and
// TextDocumentLayout published documentSizeChanged() for that taller document.
// Every consumer of it - the scroll range, the preview geometry, anything
// tracking the document height - saw the table open and then closed again.
//
// The restore now runs inside the same contentsChange emission, so exactly one
// size is published and it describes the folded document.
void TestInteractivePreview::testNoDocumentSizeIsPublishedForTheOpenSource() {
  VMarkdownEditor editor(makeAutoFoldConfig(true), QSharedPointer<TextEditorParameters>::create());
  editor.resize(600, 400);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  setTextAndSettle(editor, QLatin1String(c_table) + QStringLiteral("\ntail\n"));
  settleFolding();
  QVERIFY(!blockVisible(editor, 1));

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto sheet = sheetView(widget);
  QVERIFY(sheet);

  // Drain what the initial fold owes, so only the cell edit is measured.
  settle(editor);
  settleFolding();

  int publications = 0;
  int openPublications = 0;
  QObject::connect(editor.document()->documentLayout(),
                   &QAbstractTextDocumentLayout::documentSizeChanged, &editor,
                   [&publications, &openPublications, &editor](const QSizeF &) {
                     ++publications;
                     if (blockVisible(editor, 1)) {
                       ++openPublications;
                     }
                   });

  editCell(sheet, 1, 0, QStringLiteral("zz"));
  {
    QFocusEvent out(QEvent::FocusOut);
    QCoreApplication::sendEvent(sheet, &out);
  }
  QVERIFY(editor.document()->toPlainText().contains(QStringLiteral("| zz | b |")));
  QVERIFY(!blockVisible(editor, 1));

  QVERIFY2(publications > 0, "the layout never republished - the probe proves nothing");
  QVERIFY2(openPublications == 0,
           "a document size was published while the rewritten source was expanded");

  settle(editor);
  settleFolding();
  QVERIFY(!blockVisible(editor, 1));
  QCOMPARE(openPublications, 0);
}

// The same continuity, for an HTML-backed table. Its folding region comes from
// the scanner's own span rather than from the HTML block node, and its commit
// re-serializes tags instead of pipes, so neither the query nor the restore
// extent is shared with the Markdown path.
void TestInteractivePreview::testFoldSurvivesASheetCellEditInAnHtmlTable() {
  VMarkdownEditor editor(makeAutoFoldConfig(true), QSharedPointer<TextEditorParameters>::create());

  // Blocks: 0..4 the table, 5 "", 6 "tail", 7 "".
  setTextAndSettle(editor, QStringLiteral("<table>\n"
                                          "<tr><th>a</th></tr>\n"
                                          "<tr><td>b</td></tr>\n"
                                          "<tr><td>c</td></tr>\n"
                                          "</table>\n\n"
                                          "tail\n"));
  settleFolding();

  QCOMPARE(editor.document()->findBlockByNumber(0).text(), QStringLiteral("<table>"));
  QCOMPARE(editor.document()->findBlockByNumber(4).text(), QStringLiteral("</table>"));
  QVERIFY2(!blockVisible(editor, 2), "the HTML table's source did not fold");

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto sheet = sheetView(widget);
  QVERIFY(sheet);

  // Row 1 is the first body row; its only cell holds "b".
  editCell(sheet, 1, 0, QStringLiteral("zz"));
  {
    QFocusEvent out(QEvent::FocusOut);
    QCoreApplication::sendEvent(sheet, &out);
  }
  QVERIFY2(editor.document()->toPlainText().contains(QStringLiteral("zz")),
           qPrintable(editor.document()->toPlainText()));

  // Immediately after the synchronous write-back, before any parse.
  QVERIFY2(!blockVisible(editor, 2), "the rewritten HTML table expanded");

  settle(editor);
  settleFolding();
  QVERIFY2(!blockVisible(editor, 2), "the parse lost the HTML table's fold");
  QVERIFY(singlePreviewWidget(editor));
}

// A merge converts a pipe table into HTML, so the replacement is longer, has a
// different shape and a different backing than the source it replaces. It is
// driven through the real context-menu action rather than a synthetic
// requestSourceReplacement(), so the sheet mutation, the arming and the
// debounced write-back are all the ones a user gets.
void TestInteractivePreview::testFoldSurvivesAMergeAction() {
  VMarkdownEditor editor(makeAutoFoldConfig(true), QSharedPointer<TextEditorParameters>::create());
  editor.resize(700, 500);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));

  setTextAndSettle(editor, QLatin1String(c_table) + QStringLiteral("\ntail\n"));
  settleFolding();
  QVERIFY2(!blockVisible(editor, 1), "the table did not fold");

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto sheet = sheetView(widget);
  QVERIFY(sheet);
  QTextTable *table = sheetTable(sheet);
  QVERIFY(table);

  // A rectangle across the two cells of the body row. It survives the sheet's
  // selection clamp precisely so Merge can read it.
  QTextCursor selection = table->cellAt(1, 0).firstCursorPosition();
  selection.setPosition(table->cellAt(1, 1).lastCursorPosition().position(),
                        QTextCursor::KeepAnchor);
  sheet->setTextCursor(selection);
  QCoreApplication::processEvents();
  QVERIFY2(sheet->textCursor().hasComplexSelection(), "the rectangle did not survive the clamp");

  OpenSourcePaintProbe probe(&editor, 1);
  editor.getTextEdit()->viewport()->installEventFilter(&probe);

  // contextMenuEvent() runs QMenu::exec(), so the action has to be triggered
  // from inside that nested loop, and the loop has to be left on EVERY path or
  // sendEvent() below never returns. Strictly INSIDE the rectangle: a click
  // outside it retargets the caret and collapses it before Merge is offered.
  const QPoint inside = sheet->cursorRect(table->cellAt(1, 1).firstCursorPosition()).center();
  bool merged = false;
  auto driveMenu = [&merged](int p_attemptsLeft, auto &&p_self) -> void {
    auto popup = qobject_cast<QMenu *>(QApplication::activePopupWidget());
    if (!popup) {
      // The menu is shown before exec() spins its loop, so this should not
      // happen; retry a bounded number of times rather than hang if it does.
      if (p_attemptsLeft > 0) {
        QTimer::singleShot(10, [p_attemptsLeft, &p_self]() { p_self(p_attemptsLeft - 1, p_self); });
      } else {
        QApplication::closeAllWindows();
      }
      return;
    }

    QAction *merge =
        popup->findChild<QAction *>(QStringLiteral("MergeCells"), Qt::FindChildrenRecursively);
    if (merge && merge->isEnabled()) {
      merge->trigger();
      merged = true;
    }

    popup->close();
  };
  QTimer::singleShot(0, sheet, [&driveMenu]() { driveMenu(20, driveMenu); });

  QContextMenuEvent menuEvent(QContextMenuEvent::Mouse, inside,
                              sheet->viewport()->mapToGlobal(inside));
  QCoreApplication::sendEvent(sheet->viewport(), &menuEvent);
  QVERIFY2(merged, "the Merge action was never triggered");

  // The merge is written back through the sheet's own commit, which the merge
  // arms rather than performs, so let it land.
  QTRY_VERIFY_WITH_TIMEOUT(editor.document()->toPlainText().contains(QStringLiteral("colspan")),
                           3000);

  settle(editor);
  settleFolding();
  QVERIFY2(!blockVisible(editor, 1), "the merged table lost its fold");
  QVERIFY2(probe.m_paints > 0, "the viewport never repainted - the probe proves nothing");
  QVERIFY2(probe.m_openPaints == 0, "a paint observed the merged table's source expanded");

  editor.getTextEdit()->viewport()->removeEventFilter(&probe);
}

// A table under a blockquote carries a container prefix on every line, and the
// quote itself is a folding region coextensive with the table. The de-duplication
// which resolves that has never been exercised across a rewrite.
void TestInteractivePreview::testFoldSurvivesASheetCellEditInABlockquotedTable() {
  VMarkdownEditor editor(makeAutoFoldConfig(true), QSharedPointer<TextEditorParameters>::create());

  // Blocks: 0..2 the quoted table, 3 "", 4 "tail", 5 "".
  setTextAndSettle(editor, QStringLiteral("> | h1 | h2 |\n> | --- | --- |\n> | a | b |\n"
                                          "\ntail\n"));
  settleFolding();
  QVERIFY2(!blockVisible(editor, 1), "the quoted table's source did not fold");

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto sheet = sheetView(widget);
  QVERIFY(sheet);

  editCell(sheet, 1, 0, QStringLiteral("zz"));
  {
    QFocusEvent out(QEvent::FocusOut);
    QCoreApplication::sendEvent(sheet, &out);
  }
  // The replacement keeps the "> " prefix, or it would be refused outright.
  QVERIFY2(editor.document()->toPlainText().contains(QStringLiteral("> | zz | b |")),
           qPrintable(editor.document()->toPlainText()));

  QVERIFY2(!blockVisible(editor, 1), "the rewritten quoted table expanded");

  settle(editor);
  settleFolding();
  QVERIFY2(!blockVisible(editor, 1), "the parse lost the quoted table's fold");
}

// What a user typing actually hits: the commit is fired by the 400 ms idle
// timer, from the event loop, with the sheet still focused - not from a
// focus-out inside a call the test controls. Every other fold test here drives
// the focus-out path.
void TestInteractivePreview::testFoldSurvivesADebouncedCommit() {
  VMarkdownEditor editor(makeAutoFoldConfig(true), QSharedPointer<TextEditorParameters>::create());
  editor.resize(600, 400);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));

  setTextAndSettle(editor, QLatin1String(c_table) + QStringLiteral("\ntail\n"));
  settleFolding();
  QVERIFY2(!blockVisible(editor, 1), "the table did not fold");

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto sheet = sheetView(widget);
  QVERIFY(sheet);
  sheet->setFocus();
  QVERIFY2(sheet->hasFocus(), "the sheet did not take the focus");

  settle(editor);
  settleFolding();

  OpenSourcePaintProbe probe(&editor, 1);
  editor.getTextEdit()->viewport()->installEventFilter(&probe);

  editCell(sheet, 1, 0, QStringLiteral("zz"));

  // No flushSheet(): the debounce is the point.
  QTRY_VERIFY_WITH_TIMEOUT(editor.document()->toPlainText().contains(QStringLiteral("| zz | b |")),
                           3000);
  QVERIFY2(!blockVisible(editor, 1), "the debounced commit expanded the source");
  QVERIFY2(probe.m_openPaints == 0,
           "a paint observed the source expanded during the debounced commit");

  settle(editor);
  settleFolding();
  QVERIFY2(!blockVisible(editor, 1), "the parse after a debounced commit lost the fold");
  QVERIFY2(probe.m_paints > 0, "the viewport never repainted - the probe proves nothing");
  QCOMPARE(probe.m_openPaints, 0);

  editor.getTextEdit()->viewport()->removeEventFilter(&probe);
}

// A rewrite which changes the row count shifts every range below it. Those
// ranges must keep their fold state, which is what the live-position
// reconciliation buys.
void TestInteractivePreview::testRewriteKeepsAFoldedTableBelowFolded() {
  VMarkdownEditor editor(makeAutoFoldConfig(true), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor,
                   QLatin1String(c_table) + QStringLiteral("\ntail\n\n") + QLatin1String(c_table));

  settleFolding();
  QCOMPARE(factory->m_widgets.size(), 2);
  // The second table starts at block 5: 0..2 table, 3 blank, 4 tail, 5..7.
  QVERIFY2(!blockVisible(editor, 1), "the first table did not fold");
  QVERIFY2(!blockVisible(editor, 7), "the second table did not fold");

  auto first = firstWidgetBySource(factory);
  QVERIFY(first);
  first->previewContext()->requestSourceReplacement(
      QStringLiteral("| h1 | h2 |\n| --- | --- |\n| a | b |\n| c | d |"));
  QVERIFY2(first->m_lastResult.isAccepted(), qPrintable(first->m_lastResult.diagnostic()));

  // Everything below moved down by one block.
  QVERIFY(!blockVisible(editor, 1));
  QVERIFY2(!blockVisible(editor, 8), "the table below the rewrite lost its fold");

  settle(editor);
  settleFolding();
  QVERIFY(!blockVisible(editor, 1));
  QVERIFY(!blockVisible(editor, 8));
}

// A region left open for the caret stays open across its own rewrite: the
// rewrite restores what was there, not what the option would have chosen.
void TestInteractivePreview::testCaretSkippedTableStaysOpenAcrossARewrite() {
  VMarkdownEditor editor(makeAutoFoldConfig(true), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));

  editor.setText(QLatin1String(c_table) + QStringLiteral("\ntail\n"));
  putEditorCaretInBlock(editor, 1);
  settle(editor);
  settleFolding();

  QCOMPARE(factory->m_widgets.size(), 1);
  QVERIFY(blockVisible(editor, 1));

  auto widget = factory->m_widgets.first();
  widget->previewContext()->requestSourceReplacement(
      QStringLiteral("| h1 | h2 |\n| --- | --- |\n| z | b |"));
  QVERIFY2(widget->m_lastResult.isAccepted(), qPrintable(widget->m_lastResult.diagnostic()));
  QVERIFY(blockVisible(editor, 1));

  putEditorCaretInBlock(editor, editor.document()->blockCount() - 1);
  settle(editor);
  settleFolding();
  QVERIFY2(blockVisible(editor, 1), "the rewrite folded a region which was open");
}

// With text folding switched off there is no gutter to unfold with, so nothing
// may hide source - and what the preview remembers has to survive untouched.
void TestInteractivePreview::testRewriteWhileFoldingIsDisabledKeepsTheState() {
  auto config = makeAutoFoldConfig(true);
  VMarkdownEditor editor(config, QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QLatin1String(c_table) + QStringLiteral("\ntail\n"));

  settleFolding();
  QVERIFY(!blockVisible(editor, 1));

  config->m_textEditorConfig->m_textFoldingEnabled = false;
  editor.setConfig(config);
  settle(editor);
  settleFolding();
  QVERIFY2(blockVisible(editor, 1), "disabling text folding left source hidden");

  auto widget = factory->m_widgets.last();
  QVERIFY(widget);
  widget->previewContext()->requestSourceReplacement(
      QStringLiteral("| h1 | h2 |\n| --- | --- |\n| z | b |"));
  QVERIFY2(widget->m_lastResult.isAccepted(), qPrintable(widget->m_lastResult.diagnostic()));
  QVERIFY(blockVisible(editor, 1));

  // Re-enabling brings back what the preview remembered.
  config->m_textEditorConfig->m_textFoldingEnabled = true;
  editor.setConfig(config);
  settle(editor);
  settleFolding();
  QVERIFY2(!blockVisible(editor, 1), "the remembered fold state was lost");
}

// Rebuilding the *renderer* of an existing element - here a widget which
// refuses the next snapshot - is not a new initial decision. The option is off
// by then, so only the state carried across the rebuild can fold it again.
void TestInteractivePreview::testFoldStateSurvivesAWidgetRebuild() {
  auto config = makeAutoFoldConfig(true);
  VMarkdownEditor editor(config, QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QLatin1String(c_table) + QStringLiteral("\ntail\n"));

  settleFolding();
  QVERIFY(!blockVisible(editor, 1));
  QCOMPARE(factory->m_widgets.size(), 1);

  // From now on a fresh decision would leave it open.
  config->m_autoFoldPreviewedBlocksEnabled = false;
  editor.setConfig(config);

  // Destroy every range and every entry *before* forcing the rebuild. With
  // text folding off the fold pass early-returns, so it can neither fold
  // anything nor write a state back onto the new item: the only way the
  // remembered Folded can reach the rebuilt item is createItem()'s carry.
  config->m_textEditorConfig->m_textFoldingEnabled = false;
  editor.setConfig(config);
  settle(editor);
  settleFolding();
  QVERIFY(blockVisible(editor, 1));

  // Force the remove + create rebuild: the live widget refuses the snapshot.
  factory->m_widgets.first()->m_refuseNextSetPreview = true;
  settle(editor);
  settleFolding();
  QVERIFY(factory->m_widgets.size() > 1);
  QVERIFY(blockVisible(editor, 1));

  // With the option off, only the state the rebuilt item carries can fold the
  // table when the ranges come back.
  config->m_textEditorConfig->m_textFoldingEnabled = true;
  editor.setConfig(config);
  settle(editor);
  settleFolding();
  QVERIFY2(!blockVisible(editor, 1), "the rebuilt widget lost its fold state");
}

// The fold evaluation is queued, so a fold or an unfold made from the gutter is
// not on the item yet when a rewrite arrives. The rewrite samples the live
// state instead of trusting the item, in both directions.
void TestInteractivePreview::testGutterUnfoldBeforeARewriteIsHonoured() {
  VMarkdownEditor editor(makeAutoFoldConfig(true), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));

  editor.resize(700, 500);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  setTextAndSettle(editor, QLatin1String(c_table) + QStringLiteral("\ntail\n"));

  settleFolding();
  QVERIFY(!blockVisible(editor, 1));

  // The item still remembers Folded from here on: nothing is processed between
  // the click and the rewrite.
  QVERIFY2(toggleFoldFromGutter(editor, 0, 1), "could not reach the gutter's folding marker");
  QVERIFY(blockVisible(editor, 1));

  auto widget = factory->m_widgets.last();
  QVERIFY(widget);
  widget->previewContext()->requestSourceReplacement(
      QStringLiteral("| h1 | h2 |\n| --- | --- |\n| z | b |"));
  QVERIFY2(widget->m_lastResult.isAccepted(), qPrintable(widget->m_lastResult.diagnostic()));

  QVERIFY2(blockVisible(editor, 1), "the rewrite re-folded a table the user had just opened");

  settle(editor);
  settleFolding();
  QVERIFY(blockVisible(editor, 1));
}

void TestInteractivePreview::testGutterFoldBeforeARewriteIsHonoured() {
  VMarkdownEditor editor(makeAutoFoldConfig(true), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));

  editor.resize(700, 500);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));

  // Settled unfolded by the caret rule, so the item remembers Unfolded.
  editor.setText(QLatin1String(c_table) + QStringLiteral("\ntail\n"));
  putEditorCaretInBlock(editor, 1);
  settle(editor);
  settleFolding();
  QVERIFY(blockVisible(editor, 1));

  QVERIFY2(toggleFoldFromGutter(editor, 0, 1), "could not reach the gutter's folding marker");
  QVERIFY(!blockVisible(editor, 1));

  auto widget = factory->m_widgets.last();
  QVERIFY(widget);
  widget->previewContext()->requestSourceReplacement(
      QStringLiteral("| h1 | h2 |\n| --- | --- |\n| z | b |"));
  QVERIFY2(widget->m_lastResult.isAccepted(), qPrintable(widget->m_lastResult.diagnostic()));

  QVERIFY2(!blockVisible(editor, 1), "the rewrite lost a fold the user had just made");

  settle(editor);
  settleFolding();
  QVERIFY(!blockVisible(editor, 1));
}

// Undo of a rewrite performs the inverse destructive replacement, which
// collapses the item's anchor. There is no post-edit retarget hook for it, so
// the next generation is a fresh element and the option decides again. This is
// documented behaviour, and the assertion is here to pin it down.
void TestInteractivePreview::testUndoOfARewriteReDecides() {
  VMarkdownEditor editor(makeAutoFoldConfig(true), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));

  // Deliberately left open by the caret rule.
  editor.setText(QLatin1String(c_table) + QStringLiteral("\ntail\n"));
  putEditorCaretInBlock(editor, 1);
  settle(editor);
  settleFolding();
  QVERIFY(blockVisible(editor, 1));

  auto widget = factory->m_widgets.first();
  widget->previewContext()->requestSourceReplacement(
      QStringLiteral("| h1 | h2 |\n| --- | --- |\n| z | b |"));
  QVERIFY(widget->m_lastResult.isAccepted());
  QVERIFY(blockVisible(editor, 1));

  editor.document()->undo();
  putEditorCaretInBlock(editor, editor.document()->blockCount() - 1);
  settle(editor);
  settleFolding();

  QVERIFY2(!blockVisible(editor, 1), "the documented re-decide after an undo no longer happens");

  // A redo is the same kind of destructive replacement, and re-decides the
  // same way. With the caret away from the interior, the option folds the
  // fresh element again.
  editor.document()->redo();
  putEditorCaretInBlock(editor, editor.document()->blockCount() - 1);
  settle(editor);
  settleFolding();

  QVERIFY(editor.document()->toPlainText().contains(QStringLiteral("| z | b |")));
  QVERIFY2(!blockVisible(editor, 1), "the documented re-decide after a redo no longer happens");
}

// A full replacement is a new document: every identity and every range is gone,
// so the option decides from scratch.
void TestInteractivePreview::testFullReplacementReDecides() {
  VMarkdownEditor editor(makeAutoFoldConfig(true), QSharedPointer<TextEditorParameters>::create());

  editor.setText(QLatin1String(c_table) + QStringLiteral("\ntail\n"));
  putEditorCaretInBlock(editor, 1);
  settle(editor);
  settleFolding();
  QVERIFY(blockVisible(editor, 1));

  setTextAndSettle(editor, QLatin1String(c_table) + QStringLiteral("\ntail\n"));
  settleFolding();
  QVERIFY2(!blockVisible(editor, 1), "a fresh document did not re-decide");
}

// A deleted source leaves the item alive until the next parse removes it. Its
// anchor has collapsed, so it describes no range and nothing may be folded for
// it.
void TestInteractivePreview::testDeletedSourceFoldsNothing() {
  VMarkdownEditor editor(makeAutoFoldConfig(true), QSharedPointer<TextEditorParameters>::create());
  setTextAndSettle(editor, QLatin1String(c_table) + QStringLiteral("\ntail\n"));

  settleFolding();
  QVERIFY(!blockVisible(editor, 1));

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  const int start = widget->previewContext()->preview()->startPos();
  const int end = widget->previewContext()->preview()->endPos();

  QTextCursor cursor(editor.document());
  cursor.setPosition(start);
  cursor.setPosition(end, QTextCursor::KeepAnchor);
  cursor.removeSelectedText();

  settleFolding();
  settle(editor);
  settleFolding();

  QVERIFY(previewWidgets(editor).isEmpty());
  for (int i = 0; i < editor.document()->blockCount(); ++i) {
    QVERIFY2(
        blockVisible(editor, i),
        qPrintable(QStringLiteral("block %1 stayed hidden after its source was deleted").arg(i)));
  }
}

// A widget callback may spin a real nested event loop, which delivers the
// host's own queued timers. The fold evaluation must not run against a
// half-reconciled item set.
void TestInteractivePreview::testFoldRefreshIsDeferredDuringWidgetCallback() {
  VMarkdownEditor editor(makeAutoFoldConfig(true), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QLatin1String(c_table) + QStringLiteral("\ntail\n"));

  settleFolding();
  QCOMPARE(factory->m_widgets.size(), 1);
  QVERIFY(!blockVisible(editor, 1));

  auto widget = factory->m_widgets.first();
  int duringSpin = -1;
  widget->m_foldCounterSource = previewHost(editor);
  widget->m_duringSpin = [&]() { duringSpin = foldRefreshes(editor); };
  widget->m_spinOnNextSetPreview = true;

  settle(editor);
  QCOMPARE(widget->m_spinCount, 1);

  // The nested loop delivers the host's own queued timers, and the fold pass
  // owed by this very parse generation lands right in the middle of it. It must
  // decline while the item set is half reconciled.
  QVERIFY(widget->m_foldRefreshesBeforeSpin >= 0);
  QVERIFY(duringSpin >= 0);
  QCOMPARE(duringSpin, widget->m_foldRefreshesBeforeSpin);

  settleFolding();

  // And it must run afterwards, on the settled item set.
  QVERIFY2(foldRefreshes(editor) > duringSpin, "the deferred fold pass never ran");
  QCOMPARE(previewWidgets(editor).size(), 1);
  QVERIFY(!blockVisible(editor, 1));
}

// ---------------------------------------------------------------------------
// Scrolling while a preview widget has the focus
// ---------------------------------------------------------------------------

namespace {
// A table at the top, then enough filler that a caret parked at the end of the
// document is far off-screen while the sheet stays visible at the top.
QString tableAboveFiller() {
  QString text = QLatin1String(c_table);
  for (int i = 0; i < 200; ++i) {
    text += QStringLiteral("filler line %1\n").arg(i);
  }

  return text;
}

// Filler on both sides, so the sheet can be brought on screen at a scroll
// position which is neither the minimum nor the maximum.
QString tableBetweenFiller() {
  QString text;
  for (int i = 0; i < 100; ++i) {
    text += QStringLiteral("leading line %1\n").arg(i);
  }

  text += QLatin1Char('\n');
  text += QLatin1String(c_table);
  text += QLatin1Char('\n');

  for (int i = 0; i < 100; ++i) {
    text += QStringLiteral("trailing line %1\n").arg(i);
  }

  return text;
}

// Caret at the end of the document, viewport parked at the top. The minimum is
// a stable parking spot: an auto-folded source changes the scrollbar *range*,
// which would move any other value on its own.
void parkCaretOffScreenAtTop(VMarkdownEditor &p_editor) {
  auto textEdit = p_editor.getTextEdit();
  QTextCursor cursor(p_editor.document());
  cursor.movePosition(QTextCursor::End);
  textEdit->setTextCursor(cursor);

  auto vbar = textEdit->verticalScrollBar();
  vbar->setValue(vbar->minimum());
  QCoreApplication::processEvents();
}

// Scroll until the document's single preview has been realized, and return it.
//
// A preview whose reserved band is nowhere near the viewport is BOUND but not
// REALIZED: it owns a band and folds its source, but no widget has been built
// for it yet. That is the point of lazy realization, and it means a fixture
// which parks a table 100 lines down has no widget to inspect until something
// brings it on screen.
//
// The scan walks the bar rather than jumping to a computed offset because the
// band's height is an estimate until the widget exists, so the offset that
// would centre it is not knowable in advance.
PreviewWidget *scrollUntilPreviewRealized(VMarkdownEditor &p_editor) {
  auto vbar = p_editor.getTextEdit()->verticalScrollBar();
  const int span = vbar->maximum() - vbar->minimum();
  const int step = qMax(1, span / 40);

  for (int value = vbar->minimum(); value <= vbar->maximum(); value += step) {
    vbar->setValue(value);
    QCoreApplication::processEvents();
    QTest::qWait(20);
    QCoreApplication::processEvents();
    if (auto widget = singlePreviewWidget(p_editor)) {
      return widget;
    }
  }

  return nullptr;
}
} // namespace

// The reported bug: an in-place edit rewrites the source, the highlighter
// completes, and the editor scrolls to its own caret - which is nowhere near
// the sheet the user is typing in.
void TestInteractivePreview::testSheetEditDoesNotScrollToTheEditorCaret() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.resize(600, 300);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  setTextAndSettle(editor, tableAboveFiller());

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  QPointer<QTextEdit> sheet = sheetView(widget);
  QVERIFY(sheet);

  auto vbar = editor.getTextEdit()->verticalScrollBar();
  QVERIFY(vbar->maximum() > vbar->minimum());
  parkCaretOffScreenAtTop(editor);
  QCOMPARE(vbar->value(), vbar->minimum());

  sheet->setFocus();
  QVERIFY2(sheet->hasFocus(), "the sheet did not take the focus");

  editCell(sheet, 1, 1, QStringLiteral("changed"));

  // Let the sheet's own 400 ms debounce fire. flushSheet() would fake a
  // FocusOut without moving the real focus, and the real focus is the very
  // thing under test here.
  QTRY_VERIFY_WITH_TIMEOUT(
      editor.document()->toPlainText().contains(QStringLiteral("| a | changed |")), 3000);
  QVERIFY(sheet);
  QVERIFY2(sheet->hasFocus(), "the commit moved the focus out of the sheet");

  // Drive the parse generation the rewrite owes. Before the fix this is where
  // the viewport jumped to the editor's caret at the end of the document.
  settle(editor);

  QVERIFY(sheet);
  QVERIFY2(sheet->hasFocus(), "the parse moved the focus out of the sheet");
  QCOMPARE(vbar->value(), vbar->minimum());
}

// The centering path is reached from cursorPositionChanged, so it is exercised
// directly: whether an edit which merely displaces the caret re-emits that
// signal is a Qt detail this guard must not rest on.
void TestInteractivePreview::testCenterCursorIsSkippedWhileASheetHasTheFocus() {
  auto config = makeConfig();
  config->m_textEditorConfig->m_centerCursor = CenterCursor::AlwaysCenter;

  VMarkdownEditor editor(config, QSharedPointer<TextEditorParameters>::create());
  editor.resize(600, 300);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  setTextAndSettle(editor, tableAboveFiller());

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto sheet = sheetView(widget);
  QVERIFY(sheet);

  auto textEdit = editor.getTextEdit();
  auto vbar = textEdit->verticalScrollBar();
  QVERIFY(vbar->maximum() > vbar->minimum());
  parkCaretOffScreenAtTop(editor);
  QCOMPARE(vbar->value(), vbar->minimum());

  sheet->setFocus();
  QVERIFY2(sheet->hasFocus(), "the sheet did not take the focus");
  textEdit->checkCenterCursor();
  QCOMPARE(vbar->value(), vbar->minimum());

  // And it centers again as soon as the editor owns the focus.
  textEdit->setFocus();
  QVERIFY2(textEdit->hasFocus(), "the editor did not take the focus back");
  vbar->setValue(vbar->minimum());
  textEdit->checkCenterCursor();
  QVERIFY2(vbar->value() > vbar->minimum(),
           "the editor did not center on its own caret once it had the focus");
}

// The guard must not suppress the ordinary case: with the focus in the editor,
// a completed parse still scrolls to the editor's caret.
void TestInteractivePreview::testDocumentEditStillScrollsWhenTheEditorHasFocus() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.resize(600, 300);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  setTextAndSettle(editor, tableAboveFiller());
  QVERIFY(singlePreviewWidget(editor));

  auto textEdit = editor.getTextEdit();
  textEdit->setFocus();
  QVERIFY2(textEdit->hasFocus(), "the editor did not take the focus");

  auto vbar = textEdit->verticalScrollBar();
  QVERIFY(vbar->maximum() > vbar->minimum());
  parkCaretOffScreenAtTop(editor);
  QCOMPARE(vbar->value(), vbar->minimum());

  // A detached cursor, not a synthetic key press: QTextEdit makes the caret
  // visible itself while handling a key, so a key press would pass even if the
  // guard suppressed every case.
  QTextCursor cursor(editor.document());
  cursor.setPosition(editor.document()->findBlockByNumber(5).position());
  cursor.insertText(QStringLiteral("prefix "));
  QCoreApplication::processEvents();

  settle(editor);

  QVERIFY2(vbar->value() > vbar->minimum(),
           "the editor no longer scrolls to its caret when it has the focus");
}

// An arrow key at a table edge hands the focus back to the editor and puts the
// caret on the source. Escape no longer does (decision D3): it belongs to the
// sheet's input mode, which is how Vi's insert and visual modes are left.
void TestInteractivePreview::testAnEdgeArrowFromASheetHandsTheFocusBack() {
  auto config = makeConfig();
  config->m_textEditorConfig->m_centerCursor = CenterCursor::AlwaysCenter;

  VMarkdownEditor editor(config, QSharedPointer<TextEditorParameters>::create());
  editor.resize(600, 300);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  setTextAndSettle(editor, tableAboveFiller());

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto sheet = sheetView(widget);
  QVERIFY(sheet);

  auto textEdit = editor.getTextEdit();
  auto vbar = textEdit->verticalScrollBar();
  QVERIFY(vbar->maximum() > vbar->minimum());
  parkCaretOffScreenAtTop(editor);
  QCOMPARE(vbar->value(), vbar->minimum());

  putCaretIn(sheet, 0, 1);
  sheet->setFocus();
  QVERIFY2(sheet->hasFocus(), "the sheet did not take the focus");

  // Row 0 is the first row, so Up is at the top edge.
  QTest::keyClick(sheet, Qt::Key_Up);
  QCoreApplication::processEvents();

  QVERIFY2(textEdit->hasFocus(), "the editor did not take the focus back");

  // Escape is the mode's now, and with no Vi mode installed nothing claims it:
  // the sheet keeps the focus.
  sheet->setFocus();
  QVERIFY(sheet->hasFocus());
  QTest::keyClick(sheet, Qt::Key_Escape);
  QCoreApplication::processEvents();
  QVERIFY2(sheet->hasFocus(), "Escape must no longer hand the focus back");
}

// The sheet runs the editor's configured input mode (decision D6), and tracks
// a change to it - which is what makes typing inside a previewed table behave
// like typing outside one.
void TestInteractivePreview::testASheetFollowsTheEditorsInputMode() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.resize(600, 300);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  editor.setInputMode(InputMode::ViMode);
  setTextAndSettle(editor, tableAboveFiller());

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto sheet = qobject_cast<VTextEdit *>(sheetView(widget));
  QVERIFY2(sheet, "the sheet is not a VTextEdit, so no input mode can run in it");

  // Decision D5: recorded, not built. A note can hold dozens of previewed
  // tables and a Vi mode is a whole KateVi::InputModeManager plus a command
  // bar; only a sheet the user moves into pays for one.
  QVERIFY2(!sheet->getInputMode(), "the mode was built before the sheet was focused");

  sheet->setFocus();
  QVERIFY(sheet->hasFocus());
  QCoreApplication::processEvents();

  QVERIFY(sheet->getInputMode());
  QCOMPARE(sheet->getInputMode()->mode(), InputMode::ViMode);

  // And it tracks a change. Sourced from the editor's existing modeChanged()
  // signal plus a query of the mode (decision D8), so no new signal was added.
  editor.setInputMode(InputMode::VscodeMode);
  QCOMPARE(sheet->getInputMode()->mode(), InputMode::VscodeMode);

  editor.setInputMode(InputMode::NormalMode);
  QCOMPARE(sheet->getInputMode()->mode(), InputMode::NormalMode);
}

// Decision D4: while a sheet holds the focus its mode's status widget takes the
// editor's single slot, and the editor takes it back on the way out.
void TestInteractivePreview::testAFocusedSheetTakesTheEditorsStatusSlot() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.resize(600, 300);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  editor.setInputMode(InputMode::ViMode);
  setTextAndSettle(editor, tableAboveFiller());

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto sheet = sheetView(widget);
  QVERIFY(sheet);

  QSignalSpy published(&editor, &VTextEditor::inputModeStatusWidgetChanged);

  const QSharedPointer<QWidget> editorWidget = editor.inputModeStatusWidget();
  QVERIFY2(editorWidget, "Vi mode has a status widget");

  sheet->setFocus();
  QVERIFY(sheet->hasFocus());
  QCoreApplication::processEvents();

  const QSharedPointer<QWidget> sheetWidget = editor.inputModeStatusWidget();
  QVERIFY2(sheetWidget, "the sheet's mode has a status widget of its own");
  QVERIFY2(sheetWidget != editorWidget, "the sheet did not take the editor's status slot");
  QVERIFY(published.count() > 0);

  auto textEdit = editor.getTextEdit();
  textEdit->setFocus();
  QVERIFY(textEdit->hasFocus());
  QCoreApplication::processEvents();

  QCOMPARE(editor.inputModeStatusWidget(), editorWidget);

  // Destroying the editor here is also the teardown test: the sheet's mode has
  // to die with its status bar already unparented, which ViInputMode asserts.
}

// Switching a FOCUSED sheet from a mode with no status widget to one that has
// one has to mount it. The Normal and vscode modes have none, so the "who owns
// the slot" bookkeeping reads 0 at exactly the moment the swap happens.
void TestInteractivePreview::testAFocusedModeSwitchMountsTheNewStatusWidget() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.resize(600, 300);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  editor.setInputMode(InputMode::NormalMode);
  setTextAndSettle(editor, tableAboveFiller());

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto sheet = sheetView(widget);
  QVERIFY(sheet);

  sheet->setFocus();
  QVERIFY(sheet->hasFocus());
  QCoreApplication::processEvents();

  // Normal mode: nobody has a status widget, so the slot is empty.
  QVERIFY(!editor.inputModeStatusWidget());

  editor.setInputMode(InputMode::ViMode);
  QCoreApplication::processEvents();

  const QSharedPointer<QWidget> mounted = editor.inputModeStatusWidget();
  QVERIFY2(mounted, "no status widget was mounted after the switch to Vi");

  // It is the SHEET's, not the editor's: the sheet still has the focus.
  auto sheetEdit = qobject_cast<VTextEdit *>(sheet);
  QVERIFY(sheetEdit);
  QVERIFY(sheetEdit->getInputMode());
  QVERIFY(sheetEdit->getInputMode()->statusWidget());
  QCOMPARE(mounted, sheetEdit->getInputMode()->statusWidget()->widget());

  // Back to a mode without one, still focused: the slot empties again rather
  // than keeping a widget whose mode is gone.
  editor.setInputMode(InputMode::VscodeMode);
  QCoreApplication::processEvents();
  QVERIFY(!editor.inputModeStatusWidget());
}

// Removing the FOCUSED preview has to hand the focus, and with it the input
// mode's notion of who is being typed into, back to the editor.
void TestInteractivePreview::testRemovingAFocusedSheetReturnsTheEditorsMode() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.resize(600, 300);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  editor.setInputMode(InputMode::ViMode);
  setTextAndSettle(editor, tableAboveFiller());

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto sheet = sheetView(widget);
  QVERIFY(sheet);

  auto textEdit = editor.getTextEdit();

  sheet->setFocus();
  QVERIFY(sheet->hasFocus());
  QCoreApplication::processEvents();

  // The sheet's mode holds the status slot while it has the focus.
  const QSharedPointer<QWidget> sheetWidget = editor.inputModeStatusWidget();
  QVERIFY(sheetWidget);

  // Rewriting the source away destroys the item under the focus. The status
  // slot has to come back BEFORE the mode is destroyed - ViInputMode asserts
  // its status bar is unparented - and the focus has to land on the editor.
  setTextAndSettle(editor, QStringLiteral("just text, no table at all\n"));

  QVERIFY2(!singlePreviewWidget(editor), "the table preview should be gone");
  QVERIFY2(textEdit->hasFocus(), "the editor did not take the focus back");

  // The editor's own mode owns the slot again.
  auto editorMode = editor.getInputMode();
  QVERIFY(editorMode);
  QVERIFY(editorMode->statusWidget());
  QCOMPARE(editor.inputModeStatusWidget(), editorMode->statusWidget()->widget());

  // And it is usable: the editor is in Vi normal mode and answers a motion.
  QCOMPARE(editor.getEditorMode(), EditorMode::ViModeNormal);
}

// The same removal, but with the sheet's Vi command bar holding the focus.
//
// The bar is mounted in the EDITOR's status slot, so it is not a descendant of
// the preview: a removal which only looked inside the preview's widget subtree
// would leave the focus on a bar whose mode is about to be destroyed. Handing
// the slot back also hides and unparents the bar synchronously, which can emit
// a focus change re-entrantly - so the ownership snapshot has to be taken
// before that.
void TestInteractivePreview::testRemovingASheetWhoseCommandBarHasFocus() {
  // A real host window: the editor above, its status widget below. The command
  // bar is only focusable once it is actually mounted in that status widget,
  // which is the whole point - it is outside the preview.
  QWidget host;
  auto layout = new QVBoxLayout(&host);
  layout->setContentsMargins(0, 0, 0, 0);

  auto editor = new VMarkdownEditor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  layout->addWidget(editor);

  auto status = editor->statusWidget();
  QVERIFY(status);
  layout->addWidget(status.data());

  host.resize(600, 360);
  host.show();
  QVERIFY(QTest::qWaitForWindowExposed(&host));
  host.activateWindow();
  QTest::qWaitForWindowActive(&host);

  editor->setInputMode(InputMode::ViMode);
  setTextAndSettle(*editor, tableAboveFiller());

  auto widget = singlePreviewWidget(*editor);
  QVERIFY(widget);
  auto sheet = sheetView(widget);
  QVERIFY(sheet);

  auto textEdit = editor->getTextEdit();

  sheet->setFocus();
  if (!sheet->hasFocus()) {
    QSKIP("the platform did not grant the keyboard focus");
  }
  QCoreApplication::processEvents();

  // ':' opens the emulated command bar, which then takes the focus.
  QTest::keyClick(sheet, Qt::Key_Colon);
  QCoreApplication::processEvents();

  QWidget *focus = QApplication::focusWidget();
  if (!focus || focus == sheet || sheet->isAncestorOf(focus)) {
    QSKIP("the command bar did not take the focus in this environment");
  }
  QVERIFY2(!widget->isAncestorOf(focus),
           "the command bar is expected to live outside the preview widget");

  // Rewriting the source away destroys the item, and with it the mode which
  // owns the bar that currently has the focus.
  setTextAndSettle(*editor, QStringLiteral("just text, no table at all\n"));

  QVERIFY2(!singlePreviewWidget(*editor), "the table preview should be gone");
  QVERIFY2(textEdit->hasFocus(), "the editor did not take the focus back from the command bar");

  auto editorMode = editor->getInputMode();
  QVERIFY(editorMode);
  QVERIFY(editorMode->statusWidget());
  QCOMPARE(editor->inputModeStatusWidget(), editorMode->statusWidget()->widget());
}

// ---------------------------------------------------------------------------
// The cursor line follows a preview widget which takes the focus
// ---------------------------------------------------------------------------

namespace {
// Settle the host's owed-work drain, which is where the cursor move runs.
void settleCursorLineSync() {
  QTest::qWait(30);
  QCoreApplication::processEvents();
  QCoreApplication::processEvents();
}
} // namespace

// The point of the feature: the caret, and with it the cursor-line highlight
// and the gutter's current line, land on the first block of the focused
// preview's source.
void TestInteractivePreview::testFocusingASheetMovesTheCursorLine() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.resize(600, 300);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  setTextAndSettle(editor, tableAboveFiller());

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto sheet = sheetView(widget);
  QVERIFY(sheet);

  auto textEdit = editor.getTextEdit();
  putEditorCaretInBlock(editor, 20);
  QCOMPARE(textEdit->textCursor().blockNumber(), 20);

  sheet->setFocus();
  QVERIFY2(sheet->hasFocus(), "the sheet did not take the focus");

  QTRY_COMPARE(textEdit->textCursor().blockNumber(), 0);
  QVERIFY2(!textEdit->textCursor().hasSelection(), "the sync selected text");
  QVERIFY2(sheet->hasFocus(), "the cursor move stole the focus from the sheet");
}

// The caret move saves the scroll position and puts it back with both bars'
// signals blocked. setTextCursor() relayouts on the way, so a viewport which
// just grew shrinks the maximum and that first restore clamps to a smaller
// value. valueChanged is the only thing that tells QAbstractScrollArea its new
// offset and re-places the preview widgets, so a clamped restore used to leave
// the widgets mapped for the saved value while the text was painted at the
// settled one - the previews ended up drawn over the source until an unrelated
// scroll. The blockers also silence ScrollBar's own range extension, so the
// clamp is owed a retry once that extension is replayed.
//
// The range is shrunk from a cursorPositionChanged handler, which runs
// synchronously inside setTextCursor() while the blockers are up. That is the
// same seam a resize hits, without depending on when the layout settles.
void TestInteractivePreview::testFocusingASheetResyncsAClampedScrollRestore() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.resize(600, 300);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  setTextAndSettle(editor, tableBetweenFiller());

  // The table is 100 lines down, so it is bound but not realized at scroll
  // zero. Bring it on screen first; everything below is about what happens to
  // the scroll position once a live sheet takes the focus.
  auto widget = scrollUntilPreviewRealized(editor);
  QVERIFY(widget);
  auto sheet = sheetView(widget);
  QVERIFY(sheet);

  auto textEdit = editor.getTextEdit();
  auto vbar = textEdit->verticalScrollBar();
  QVERIFY(vbar->maximum() > vbar->minimum());

  // Bring the sheet on screen at a scroll position well above the minimum, so
  // the maximum can be pulled below it later.
  QTRY_VERIFY(widget->y() != 0);
  vbar->setValue(vbar->value() + widget->y() - 40);
  QCoreApplication::processEvents();
  QTRY_VERIFY2(sheet->isVisible(), "the sheet is not on screen at this scroll position");

  putEditorCaretInBlock(editor, editor.document()->blockCount() - 1);

  const int saved = vbar->value();
  QVERIFY(saved > vbar->minimum() + 10);
  // Where the band sits in the document, which no scrolling may change.
  const int documentTop = widget->y() + saved;
  const QString before = editor.document()->toPlainText();

  // Armed only for the sync's own caret move, and only once.
  auto shrink = QSharedPointer<QMetaObject::Connection>::create();
  *shrink = connect(textEdit, &QTextEdit::cursorPositionChanged, textEdit, [&, shrink]() {
    disconnect(*shrink);
    vbar->setMaximum(saved - 10);
  });

  sheet->setFocus();
  QVERIFY2(sheet->hasFocus(), "the sheet did not take the focus");

  QTRY_VERIFY2(textEdit->textCursor().block().text().startsWith(QStringLiteral("| h1")),
               "the caret did not land on the first line of the table source");
  settleCursorLineSync();

  // VTextEdit's ScrollBar extends the maximum from rangeChanged so the bottom
  // of the content can still be scrolled up. The range moved while the bar was
  // blocked, so that extension is owed too - and nothing would revisit it
  // until the range happened to change again.
  QVERIFY2(vbar->maximum() > saved - 10,
           "the blocked range change lost ScrollBar's maximum extension");

  // The blocked restore necessarily clamped - the maximum was pulled below the
  // saved value while the bar could not answer with its extension. Once the
  // range replay puts that extension back, the saved value is representable
  // again and must be recovered: leaving the clamped position there is the
  // viewport visibly scrolling away under a click on the sheet.
  QVERIFY2(saved <= vbar->maximum(),
           "the replayed range cannot hold the saved value, so this is not the case under test");
  QCOMPARE(vbar->value(), saved);

  // Whatever the scroll settled on, the widget is placed for that same value.
  QCOMPARE(widget->y() + vbar->value(), documentTop);

  QVERIFY2(sheet->hasFocus(), "the sync dropped the focus out of the sheet");
  QCOMPARE(editor.document()->toPlainText(), before);
}

// The caret move must not scroll: the viewport shifting would hide the sheet,
// which drops its focus and triggers a write-back of whatever it holds.
void TestInteractivePreview::testFocusingASheetDoesNotScrollTheViewport() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.resize(600, 300);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  setTextAndSettle(editor, tableAboveFiller());

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto sheet = sheetView(widget);
  QVERIFY(sheet);

  auto textEdit = editor.getTextEdit();
  auto vbar = textEdit->verticalScrollBar();
  auto hbar = textEdit->horizontalScrollBar();
  QVERIFY(vbar->maximum() > vbar->minimum());

  // Scroll until the sheet sits at the top of the viewport, so the source
  // rendered above it is off screen while the sheet itself is still there.
  QVERIFY2(widget->y() > 0, "the sheet is already at the top of the viewport");
  vbar->setValue(qMin(vbar->maximum(), vbar->value() + widget->y()));
  QCoreApplication::processEvents();
  QTRY_VERIFY2(sheet->isVisible(), "the sheet is not on screen at this scroll position");

  putEditorCaretInBlock(editor, 20);
  const int vvalue = vbar->value();
  const int hvalue = hbar->value();
  const QString before = editor.document()->toPlainText();

  QSignalSpy vspy(vbar, &QScrollBar::valueChanged);
  QSignalSpy hspy(hbar, &QScrollBar::valueChanged);

  sheet->setFocus();
  QVERIFY2(sheet->hasFocus(), "the sheet did not take the focus");

  QTRY_COMPARE(textEdit->textCursor().blockNumber(), 0);
  settleCursorLineSync();

  QCOMPARE(vspy.count(), 0);
  QCOMPARE(hspy.count(), 0);
  QCOMPARE(vbar->value(), vvalue);
  QCOMPARE(hbar->value(), hvalue);
  QVERIFY2(sheet->hasFocus(), "the sync dropped the focus out of the sheet");
  QCOMPARE(editor.document()->toPlainText(), before);
}

// A caret the user deliberately parked inside the source is left alone, which
// is what keeps FocusEscapeDirection::Keep and a focus return stable.
void TestInteractivePreview::testFocusingASheetKeepsACaretAlreadyInTheSource() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.resize(600, 300);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  setTextAndSettle(editor, tableAboveFiller());

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto sheet = sheetView(widget);
  QVERIFY(sheet);

  auto textEdit = editor.getTextEdit();
  // The last row of the table's source, i.e. inside the range but not its
  // first block. An interior block may be folded away, and a folded block is
  // not a place a caret can be parked.
  putEditorCaretInBlock(editor, 2);
  QCOMPARE(textEdit->textCursor().blockNumber(), 2);
  const int position = textEdit->textCursor().position();

  sheet->setFocus();
  QVERIFY2(sheet->hasFocus(), "the sheet did not take the focus");
  settleCursorLineSync();

  QCOMPARE(textEdit->textCursor().blockNumber(), 2);
  QCOMPARE(textEdit->textCursor().position(), position);
}

// The sync goes through the host's blocked-aware scheduler, so a widget which
// grabs the focus from inside its own callback - while a nested event loop is
// delivering the host's timers - is served only after the block unwinds.
void TestInteractivePreview::testCursorLineSyncIsDeferredDuringWidgetCallback() {
  VMarkdownEditor editor(makeAutoFoldConfig(false), QSharedPointer<TextEditorParameters>::create());
  editor.resize(600, 300);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, tableAboveFiller());
  QCOMPARE(factory->m_widgets.size(), 1);

  auto widget = factory->m_widgets.first();
  auto textEdit = editor.getTextEdit();
  // The caret is outside the source, but the widget must stay on screen: a
  // hidden widget cannot take the focus at all.
  putEditorCaretInBlock(editor, 20);
  textEdit->verticalScrollBar()->setValue(textEdit->verticalScrollBar()->minimum());
  QCoreApplication::processEvents();
  QTRY_VERIFY2(widget->isVisible(), "the preview widget is not on screen");

  // The widget itself is the focusable thing here; a real renderer focuses a
  // descendant, which the host resolves the same way.
  widget->setFocusPolicy(Qt::StrongFocus);

  int duringSpin = -1;
  widget->m_duringSpin = [&]() {
    widget->setFocus();
    duringSpin = textEdit->textCursor().blockNumber();
  };
  widget->m_spinOnNextSetPreview = true;

  settle(editor);
  QCOMPARE(widget->m_spinCount, 1);
  QVERIFY2(duringSpin == 20, "the cursor moved while a widget callback was on the stack");

  settleCursorLineSync();
  QVERIFY2(widget->hasFocus(), "the widget lost the focus before the deferred sync ran");
  QCOMPARE(textEdit->textCursor().blockNumber(), 0);
}

// A caret moved into a hidden block is relocated by
// VTextEdit::handleCursorPositionChange(), which would land it somewhere the
// user never asked for. The sync declines instead.
void TestInteractivePreview::testFocusingASheetWithAHiddenFirstBlockKeepsTheCaret() {
  VMarkdownEditor editor(makeAutoFoldConfig(false), QSharedPointer<TextEditorParameters>::create());
  editor.resize(600, 300);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  setTextAndSettle(editor, tableAboveFiller());

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto sheet = sheetView(widget);
  QVERIFY(sheet);

  auto textEdit = editor.getTextEdit();
  putEditorCaretInBlock(editor, 20);
  QCOMPARE(textEdit->textCursor().blockNumber(), 20);

  QTextBlock firstSourceBlock = editor.document()->findBlockByNumber(0);
  firstSourceBlock.setVisible(false);
  QVERIFY(!blockVisible(editor, 0));

  sheet->setFocus();
  QVERIFY2(sheet->hasFocus(), "the sheet did not take the focus");
  settleCursorLineSync();

  QCOMPARE(textEdit->textCursor().blockNumber(), 20);
}

// The focus is watched application wide, so a host must serve only the widgets
// it owns: identities are per host and collide across editors.
void TestInteractivePreview::testFocusingASheetOnlyMovesItsOwnEditorsCursor() {
  VMarkdownEditor first(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  first.resize(600, 300);
  first.show();
  QVERIFY(QTest::qWaitForWindowExposed(&first));
  setTextAndSettle(first, tableAboveFiller());

  VMarkdownEditor second(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  second.resize(600, 300);
  second.show();
  QVERIFY(QTest::qWaitForWindowExposed(&second));
  setTextAndSettle(second, tableAboveFiller());

  auto secondSheet = sheetView(singlePreviewWidget(second));
  QVERIFY(secondSheet);

  putEditorCaretInBlock(first, 20);
  putEditorCaretInBlock(second, 20);

  secondSheet->setFocus();
  QVERIFY2(secondSheet->hasFocus(), "the sheet did not take the focus");

  QTRY_COMPARE(second.getTextEdit()->textCursor().blockNumber(), 0);
  settleCursorLineSync();
  QCOMPARE(first.getTextEdit()->textCursor().blockNumber(), 20);
}

// ---------------------------------------------------------------------------
// A preview selection is dropped when the focus goes back to the text editor
// ---------------------------------------------------------------------------

namespace {
// Select the whole content of one cell, which is what a drag or a Ctrl+A
// inside a cell leaves behind.
void selectCell(QTextEdit *p_sheet, int p_row, int p_column) {
  QTextTable *table = sheetTable(p_sheet);
  QVERIFY(table);
  const QTextTableCell cell = table->cellAt(p_row, p_column);
  QVERIFY(cell.isValid());

  QTextCursor cursor = p_sheet->textCursor();
  cursor.setPosition(cell.firstPosition());
  cursor.setPosition(cell.lastPosition(), QTextCursor::KeepAnchor);
  p_sheet->setTextCursor(cursor);
  QVERIFY(p_sheet->textCursor().hasSelection());
}

QString twoTables() {
  return QLatin1String(c_table) + QStringLiteral("\nmiddle\n\n") + QLatin1String(c_table);
}

// The widgets of a document holding two tables, ordered by their source
// position. The host hands them out in hash order.
QList<PreviewWidget *> widgetsBySourceStart(VMarkdownEditor &p_editor) {
  QList<PreviewWidget *> widgets = previewWidgets(p_editor);
  std::sort(widgets.begin(), widgets.end(), [](PreviewWidget *p_a, PreviewWidget *p_b) {
    return p_a->previewContext()->preview()->startPos() <
           p_b->previewContext()->preview()->startPos();
  });
  return widgets;
}
} // namespace

// The point of the feature: two competing highlights are never shown at once.
void TestInteractivePreview::testFocusReturningToTheEditorClearsTheSheetSelection() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.resize(600, 300);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  setTextAndSettle(editor, QLatin1String(c_table));

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto sheet = sheetView(widget);
  QVERIFY(sheet);

  sheet->setFocus();
  QVERIFY2(sheet->hasFocus(), "the sheet did not take the focus");
  selectCell(sheet, 1, 1);
  const int caret = sheet->textCursor().position();

  editor.getTextEdit()->setFocus();
  QVERIFY2(editor.getTextEdit()->hasFocus(), "the editor did not take the focus back");
  QCoreApplication::processEvents();

  QVERIFY2(!sheet->textCursor().hasSelection(), "the sheet kept its selection");
  QCOMPARE(sheet->textCursor().position(), caret);
}

// handleFocusEscape() sets the editor caret and then takes the focus, so it
// runs through the very same branch. The caret it placed must survive.
void TestInteractivePreview::testFocusEscapeClearsTheSelectionAndKeepsTheCaret() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.resize(600, 400);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  setTextAndSettle(editor, QStringLiteral("head\n\n") + QLatin1String(c_table) +
                               QStringLiteral("\ntail\n"));

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto sheet = sheetView(widget);
  QVERIFY(sheet);

  const QString text = editor.document()->toPlainText();
  const QString lastRow = QStringLiteral("| a | b |");
  const int sourceEnd = text.indexOf(lastRow) + lastRow.size();
  QVERIFY(sourceEnd > 0);

  sheet->setFocus();
  selectCell(sheet, 0, 1);
  QTest::keyClick(sheet, Qt::Key_Up);
  QCoreApplication::processEvents();

  QVERIFY2(editor.getTextEdit()->hasFocus(), "the editor did not take the focus back");
  QCOMPARE(editor.getTextEdit()->textCursor().position(), sourceEnd);
  QVERIFY2(!sheet->textCursor().hasSelection(), "the sheet kept its selection");
}

// Preview A -> preview B resets the focused identity, so clearing only the
// last focused one would let A's selection survive the return to the editor.
void TestInteractivePreview::testASecondPreviewKeepsTheSelectionUntilTheEditor() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.resize(600, 400);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  setTextAndSettle(editor, twoTables());

  const auto widgets = widgetsBySourceStart(editor);
  QCOMPARE(widgets.size(), 2);
  auto firstSheet = sheetView(widgets.at(0));
  auto secondSheet = sheetView(widgets.at(1));
  QVERIFY(firstSheet);
  QVERIFY(secondSheet);

  firstSheet->setFocus();
  QVERIFY(firstSheet->hasFocus());
  selectCell(firstSheet, 1, 1);

  secondSheet->setFocus();
  QVERIFY2(secondSheet->hasFocus(), "the second sheet did not take the focus");
  selectCell(secondSheet, 1, 0);
  QCoreApplication::processEvents();

  QVERIFY2(firstSheet->textCursor().hasSelection(),
           "a preview to preview move dropped the first selection");

  editor.getTextEdit()->setFocus();
  QVERIFY(editor.getTextEdit()->hasFocus());
  QCoreApplication::processEvents();

  QVERIFY2(!firstSheet->textCursor().hasSelection(), "the first sheet kept its selection");
  QVERIFY2(!secondSheet->textCursor().hasSelection(), "the second sheet kept its selection");
}

// Focus leaving the application entirely is not the editor taking it back, so
// the selection stays where the user left it.
void TestInteractivePreview::testFocusLeavingTheApplicationKeepsTheSelection() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.resize(600, 300);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  setTextAndSettle(editor, QLatin1String(c_table));

  auto sheet = sheetView(singlePreviewWidget(editor));
  QVERIFY(sheet);

  sheet->setFocus();
  QVERIFY(sheet->hasFocus());
  selectCell(sheet, 1, 1);

  // A separate top-level widget stands in for another window: the focus leaves
  // this editor without reaching its text edit.
  QWidget other;
  auto elsewhere = new QTextEdit(&other);
  auto layout = new QVBoxLayout(&other);
  layout->addWidget(elsewhere);
  other.resize(200, 100);
  other.show();
  QVERIFY(QTest::qWaitForWindowExposed(&other));
  elsewhere->setFocus();
  QCoreApplication::processEvents();

  QVERIFY2(!editor.getTextEdit()->hasFocus(),
           "the editor took the focus, which is a different case");
  QVERIFY2(sheet->textCursor().hasSelection(), "an unrelated focus move dropped the selection");
}

// The sheet stays selectable in viewer mode, so the clear has to reach it
// there too - and collapsing a selection never edits the document.
void TestInteractivePreview::testAReadOnlyEditorClearsTheSelectionToo() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.resize(600, 300);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  setTextAndSettle(editor, QLatin1String(c_table));
  editor.setReadOnly(true);
  QCoreApplication::processEvents();

  auto sheet = sheetView(singlePreviewWidget(editor));
  QVERIFY(sheet);

  sheet->setFocus();
  QVERIFY(sheet->hasFocus());
  selectCell(sheet, 1, 1);
  const QString before = editor.document()->toPlainText();

  editor.getTextEdit()->setFocus();
  QVERIFY(editor.getTextEdit()->hasFocus());
  QCoreApplication::processEvents();

  QVERIFY2(!sheet->textCursor().hasSelection(), "the viewer sheet kept its selection");
  QCOMPARE(editor.document()->toPlainText(), before);
}

// The hook is public, so a third-party renderer gets it too - and only on the
// transition that means it. Whatever it does from there is application code
// reached from inside the host, so a document mutation requested there has to
// be postponed rather than applied inline.
void TestInteractivePreview::testTheClearSelectionHookIsDispatchedGenerically() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.resize(600, 300);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QLatin1String(c_table));
  QCOMPARE(factory->m_widgets.size(), 1);

  auto widget = factory->m_widgets.first();
  QVERIFY(widget);
  widget->setFocusPolicy(Qt::StrongFocus);
  widget->setFocus();
  QVERIFY2(widget->hasFocus(), "the widget did not take the focus");
  QCoreApplication::processEvents();
  QCOMPARE(widget->m_clearSelectionCount, 0);

  const QString before = editor.document()->toPlainText();
  const QString replacement = QStringLiteral("| h1 | h2 |\n| --- | --- |\n| x | y |");
  widget->m_documentSource = [&editor]() { return editor.document()->toPlainText(); };
  widget->m_requestOnClearSelection = replacement;

  editor.getTextEdit()->setFocus();
  QVERIFY(editor.getTextEdit()->hasFocus());
  QCoreApplication::processEvents();

  QCOMPARE(widget->m_clearSelectionCount, 1);
  QVERIFY2(widget->m_documentDuringClearSelection == before,
           "the replacement was applied while the callback was still on the stack");

  // The host does not replay a third-party request verbatim: it reports
  // Deferred and leaves the retry to the requester.
  QTRY_VERIFY(widget->m_resultCount > 0);
  QCOMPARE(widget->m_lastResult.status(), PreviewReplacementResult::Deferred);
  QCOMPARE(editor.document()->toPlainText(), before);

  // A focus move which is not the editor taking it back dispatches nothing.
  const int dispatched = widget->m_clearSelectionCount;
  widget->setFocus();
  QVERIFY(widget->hasFocus());
  QCoreApplication::processEvents();
  QCOMPARE(widget->m_clearSelectionCount, dispatched);
}

// ---------------------------------------------------------------------------
// A document mutation requested from inside a layout pass or a geometry
// application
// ---------------------------------------------------------------------------

namespace {
// Every outcome one context reported, together with the document as it stood
// at that very moment. A postponed request must leave the document untouched,
// which is only observable from inside the completion.
class ReplacementRecorder : public QObject {
public:
  ReplacementRecorder(PreviewWidgetContext *p_context, VMarkdownEditor &p_editor,
                      QObject *p_parent = nullptr)
      : QObject(p_parent), m_editor(&p_editor) {
    connect(p_context, &PreviewWidgetContext::replacementFinished, this,
            [this](const PreviewReplacementResult &p_result) {
              m_statuses.append(p_result.status());
              m_documents.append(m_editor->document()->toPlainText());
              m_foldRefreshes.append(foldRefreshes(*m_editor));
            });
  }

  int count(PreviewReplacementResult::Status p_status) const {
    int n = 0;
    for (auto status : m_statuses) {
      if (status == p_status) {
        ++n;
      }
    }

    return n;
  }

  int indexOf(PreviewReplacementResult::Status p_status) const {
    return static_cast<int>(m_statuses.indexOf(p_status));
  }

  QVector<PreviewReplacementResult::Status> m_statuses;

  QStringList m_documents;

  // The fold refresh counter as it stood when each outcome was delivered, so a
  // test can assert that no lower-priority owed work ran in between.
  QVector<int> m_foldRefreshes;

private:
  VMarkdownEditor *m_editor = nullptr;
};
} // namespace

// The reported crash: the host reaches widget code from inside its own
// geometry application, that widget writes its pending edit back, and the
// editor's document is mutated from a stack Qt does not support mutating it
// from. The request must be answered Deferred, leave everything untouched, and
// land exactly once afterwards.
void TestInteractivePreview::testDeferredCommitDuringLayoutDrivenHide() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.resize(600, 300);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  setTextAndSettle(editor, tableAboveFiller());

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  QVERIFY(widget->previewContext());
  QPointer<QTextEdit> sheet = sheetView(widget);
  QVERIFY(sheet);

  ReplacementRecorder recorder(widget->previewContext(), editor);

  parkCaretOffScreenAtTop(editor);
  sheet->setFocus();
  QVERIFY2(sheet->hasFocus(), "the sheet did not take the focus");

  const QString before = editor.document()->toPlainText();
  editCell(sheet, 1, 1, QStringLiteral("changed"));
  QVERIFY2(!editor.document()->toPlainText().contains(QStringLiteral("changed")),
           "the edit was written back before the debounce elapsed");

  WarningRecorder warnings;

  // geometryContextChanged is emitted synchronously from inside the host's
  // geometry application, which is the very stack the hide of a focused sheet
  // runs on. Losing the focus there is what made the sheet write back mid-pass
  // in the crash trace.
  bool flushed = false;
  QObject sink;
  QObject::connect(widget->previewContext(), &PreviewWidgetContext::geometryContextChanged, &sink,
                   [&]() {
                     if (flushed || !sheet) {
                       return;
                     }

                     flushed = true;
                     QFocusEvent out(QEvent::FocusOut);
                     QCoreApplication::sendEvent(sheet, &out);
                   });

  // Any resize republishes and therefore hands every widget a new geometry
  // context.
  editor.resize(520, 300);
  QTRY_VERIFY2(flushed, "the geometry application never reached the widget");

  QVERIFY2(!recorder.m_statuses.isEmpty(), "the focus loss did not make the sheet write back");
  QCOMPARE(recorder.m_statuses.first(), PreviewReplacementResult::Deferred);
  // Nothing was applied: the document is byte-identical to what it was.
  QCOMPARE(recorder.m_documents.first(), before);
  QCOMPARE(editor.document()->toPlainText(), before);

  // And the write-back lands once the geometry application has unwound.
  QTRY_VERIFY_WITH_TIMEOUT(editor.document()->toPlainText().contains(QStringLiteral("changed")),
                           3000);
  QCOMPARE(recorder.count(PreviewReplacementResult::Accepted), 1);

  // The retry ranks above every other owed item, so nothing lower-priority ran
  // between the two outcomes. The fold refresh is the last step of a drain and
  // therefore the cheapest observable one.
  const int deferredAt = recorder.indexOf(PreviewReplacementResult::Deferred);
  const int acceptedAt = recorder.indexOf(PreviewReplacementResult::Accepted);
  QVERIFY(deferredAt >= 0 && acceptedAt > deferredAt);
  QCOMPARE(recorder.m_foldRefreshes.at(acceptedAt), recorder.m_foldRefreshes.at(deferredAt));

  // The sheet kept its authority: a rejected commit would have made it
  // read-only until the next snapshot, and would have restored the old value.
  QCOMPARE(recorder.count(PreviewReplacementResult::UnknownIdentity), 0);
  QCOMPARE(recorder.count(PreviewReplacementResult::InvalidRange), 0);
  QVERIFY2(!warnings.contains(QStringLiteral("reFormatBlock")),
           qPrintable(warnings.m_messages.join(QLatin1Char('\n'))));
  QVERIFY2(!warnings.contains(QStringLiteral("hasOffset")),
           qPrintable(warnings.m_messages.join(QLatin1Char('\n'))));
}

// The same rule for application code: a custom widget asking for a replacement
// from geometryContextChanged is inside the host's geometry application, so it
// is postponed - and the widget owns the retry.
void TestInteractivePreview::testCustomWidgetReplacementDuringGeometryContextIsDeferred() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.resize(600, 300);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QLatin1String(c_table));

  QCOMPARE(factory->m_widgets.size(), 1);
  auto widget = factory->m_widgets.first();
  QVERIFY(widget->previewContext());
  QVERIFY(widget->m_geometryContextCount > 0);

  const QString replacement = QStringLiteral("| h1 | h2 |\n| --- | --- |\n| z | b |");
  const QString before = editor.document()->toPlainText();
  const int resultsBefore = widget->m_resultCount;

  // Force a fresh geometry context and request the rewrite from inside it.
  widget->m_requestOnGeometryContext = replacement;
  editor.resize(500, 300);
  QTRY_VERIFY(widget->m_resultCount > resultsBefore);

  QCOMPARE(widget->m_results.last(), PreviewReplacementResult::Deferred);
  QVERIFY(!widget->m_lastResult.isAccepted());
  // Nothing moved: not the document, and not the host's binding.
  QCOMPARE(editor.document()->toPlainText(), before);
  QCOMPARE(widget->previewContext()->preview()->sourceMarkdown(),
           widget->m_preview->sourceMarkdown());

  // The host does not replay a third-party request; the widget's own retry
  // does, and it is accepted.
  QCoreApplication::processEvents();
  widget->previewContext()->requestSourceReplacement(replacement);
  QCOMPARE(widget->m_results.last(), PreviewReplacementResult::Accepted);
  QVERIFY(editor.document()->toPlainText().contains(QStringLiteral("| z | b |")));
}

// A reconcile and a rebuild requested while the host is blocked must be
// postponed rather than executed, and the edit which is still owed must
// survive both.
void TestInteractivePreview::testRemovalAndRebuildDuringADeferredFlushKeepTheEdit() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.resize(600, 300);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  setTextAndSettle(editor, tableAboveFiller());

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  QVERIFY(widget->previewContext());
  QPointer<QTextEdit> sheet = sheetView(widget);
  QVERIFY(sheet);

  parkCaretOffScreenAtTop(editor);
  sheet->setFocus();
  QVERIFY(sheet->hasFocus());
  editCell(sheet, 1, 1, QStringLiteral("changed"));

  int widgetsDuringBlock = -1;
  int widgetsAfterGeneration = -1;
  PreviewWidget *sameWidget = nullptr;
  bool droveThePasses = false;

  QObject sink;
  // Make the sheet write back from inside the host's geometry application, the
  // same stack the hide of a focused sheet runs on.
  bool flushed = false;
  QObject::connect(widget->previewContext(), &PreviewWidgetContext::geometryContextChanged, &sink,
                   [&]() {
                     if (flushed || !sheet) {
                       return;
                     }

                     flushed = true;
                     QFocusEvent out(QEvent::FocusOut);
                     QCoreApplication::sendEvent(sheet, &out);
                   });

  QObject::connect(widget->previewContext(), &PreviewWidgetContext::replacementFinished, &sink,
                   [&](const PreviewReplacementResult &p_result) {
                     if (droveThePasses ||
                         p_result.status() != PreviewReplacementResult::Deferred) {
                       return;
                     }

                     droveThePasses = true;
                     // The host is blocked right here. A rebuild requested now
                     // must not start: it would flush this very sheet from a
                     // stack which cannot apply the edit.
                     auto other = new RecordingPreviewFactory({PreviewElementType::Image});
                     QVERIFY(editor.registerPreviewWidgetFactory(other, 1));
                     widgetsDuringBlock = previewWidgets(editor).size();
                     sameWidget = singlePreviewWidget(editor);

                     // And a parse generation delivered here must be stashed,
                     // not applied.
                     editor.getHighlighter()->updateHighlight();
                     widgetsAfterGeneration = previewWidgets(editor).size();
                   });

  editor.resize(520, 300);
  QTRY_VERIFY2(droveThePasses, "the geometry application never produced a postponed write-back");
  // Neither pass ran while blocked, and no duplicate widget was created for
  // the same source.
  QCOMPARE(widgetsDuringBlock, 1);
  QCOMPARE(widgetsAfterGeneration, 1);
  QCOMPARE(sameWidget, widget);

  // The edit survives both passes.
  QTRY_VERIFY_WITH_TIMEOUT(editor.document()->toPlainText().contains(QStringLiteral("changed")),
                           3000);
  settle(editor);
  QCOMPARE(previewWidgets(editor).size(), 1);
}

// The sheet's other commit triggers must not issue a second request while one
// is on the wire: it would target an anchor the outer edit has collapsed. And
// a reconcile delivered from a nested event loop opened in that window must be
// postponed, not run against a sheet whose commit is still in flight.
void TestInteractivePreview::testConcurrentFlushTriggersSendOneRequest() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.resize(600, 400);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  setTextAndSettle(editor, QLatin1String(c_table));

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  QVERIFY(widget->previewContext());
  QPointer<QTextEdit> sheet = sheetView(widget);
  QVERIFY(sheet);

  sheet->setFocus();
  QVERIFY(sheet->hasFocus());

  ReplacementRecorder recorder(widget->previewContext(), editor);
  WarningRecorder warnings;

  editCell(sheet, 1, 1, QStringLiteral("changed"));

  bool reentered = false;
  QObject sink;
  QObject::connect(editor.document(), &QTextDocument::contentsChange, &sink, [&](int, int, int) {
    if (reentered || !sheet) {
      return;
    }

    reentered = true;

    // A commit is on the wire right now. Both a focus-out and a cell-left
    // land here in the real crash trace, and neither may issue a second
    // request against an anchor this very edit has collapsed.
    QFocusEvent out(QEvent::FocusOut);
    QCoreApplication::sendEvent(sheet, &out);
    QCoreApplication::sendEvent(sheet, &out);

    // A newer edit made in the same window is owed a write-back of its own.
    editCell(sheet, 1, 0, QStringLiteral("later"));
  });

  // Let the debounce fire.
  QTRY_VERIFY_WITH_TIMEOUT(editor.document()->toPlainText().contains(QStringLiteral("changed")),
                           3000);
  QVERIFY2(reentered, "the commit never reached the document");

  // Exactly one request was issued for the in-flight generation, and it was
  // accepted.
  QCOMPARE(recorder.m_statuses.size(), 1);
  QCOMPARE(recorder.m_statuses.first(), PreviewReplacementResult::Accepted);

  // The newer generation was re-armed and lands too, without ever being
  // rejected as a stale or unknown request.
  QTRY_VERIFY_WITH_TIMEOUT(editor.document()->toPlainText().contains(QStringLiteral("later")),
                           3000);
  QCOMPARE(recorder.count(PreviewReplacementResult::UnknownIdentity), 0);
  QCOMPARE(recorder.count(PreviewReplacementResult::StaleSnapshot), 0);
  QVERIFY2(!warnings.contains(QStringLiteral("UnknownIdentity")),
           qPrintable(warnings.m_messages.join(QLatin1Char('\n'))));
}

// A widget which opens a modal dialog from its own completion runs a nested
// event loop while the replacement transaction is still on the stack - and
// while the built-in sheet's commit would still be "in flight". A rebuild
// delivered there would reach a mandatory flush which can only decline, so it
// has to be postponed.
void TestInteractivePreview::testReconcileDuringAReplacementCompletionIsPostponed() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QLatin1String(c_table));

  QCOMPARE(factory->m_widgets.size(), 1);
  auto widget = factory->m_widgets.first();
  QVERIFY(widget->previewContext());

  const int createdBefore = factory->m_createCount;
  int createsDuringSpin = -1;
  PreviewWidget *widgetDuringSpin = nullptr;

  widget->m_spinOnNextReplacementFinished = true;
  widget->m_duringReplacementSpin = [&]() {
    // The transaction is still on the stack here.
    auto other = new RecordingPreviewFactory({PreviewElementType::Image});
    QVERIFY(editor.registerPreviewWidgetFactory(other, 1));

    QEventLoop loop;
    QTimer::singleShot(30, &loop, [&loop]() { loop.quit(); });
    loop.exec();

    createsDuringSpin = factory->m_createCount;
    widgetDuringSpin = singlePreviewWidget(editor);
  };

  WarningRecorder warnings;
  widget->previewContext()->requestSourceReplacement(
      QStringLiteral("| h1 | h2 |\n| --- | --- |\n| z | b |"));

  QCOMPARE(widget->m_results.last(), PreviewReplacementResult::Accepted);
  QVERIFY(editor.document()->toPlainText().contains(QStringLiteral("| z | b |")));

  // The rebuild did not run inside the transaction.
  QCOMPARE(createsDuringSpin, createdBefore);
  QCOMPARE(widgetDuringSpin, static_cast<PreviewWidget *>(widget));

  // And it is not lost either.
  QTest::qWait(50);
  QCoreApplication::processEvents();
  QCOMPARE(factory->m_createCount, createdBefore + 1);
  QCOMPARE(previewWidgets(editor).size(), 1);
  QVERIFY2(!warnings.contains(QStringLiteral("UnknownIdentity")),
           qPrintable(warnings.m_messages.join(QLatin1Char('\n'))));
  QVERIFY(editor.document()->toPlainText().contains(QStringLiteral("| z | b |")));
}

// The owed work is delivered by one drain, after the outermost block exits -
// not by one zero timer per owed item, and never while the block is held.
void TestInteractivePreview::testOwedWorkDrainsOnceUnderANestedEventLoop() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QLatin1String(c_table));
  QCOMPARE(factory->m_widgets.size(), 1);

  auto host = previewHost(editor);
  QVERIFY(host);
  auto drains = [host]() { return host->property("vte_preview_owed_work_drains").toInt(); };

  auto widget = factory->m_widgets.first();
  const int createdBefore = factory->m_createCount;
  int drainsBeforeSpin = -1;
  int drainsDuringSpin = -1;
  widget->m_spinOnNextSetPreview = true;
  widget->m_duringSpin = [&]() { drainsDuringSpin = drains(); };

  // Owed while the callback holds the host blocked: a rebuild, plus whatever
  // the nested loop's own timers ask for.
  auto second = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(second, 1));

  drainsBeforeSpin = drains();
  editor.getHighlighter()->updateHighlight();
  QCOMPARE(widget->m_spinCount, 1);

  // The nested loop ran for tens of milliseconds. A bare zero timer would have
  // been delivered - and re-armed - dozens of times in it.
  QVERIFY(drainsDuringSpin >= 0);
  QCOMPARE(drainsDuringSpin, drainsBeforeSpin);

  // Once the callback returns, everything owed is delivered by a bounded
  // number of drains, and the rebuild runs exactly once.
  QTest::qWait(50);
  QCoreApplication::processEvents();
  QCOMPARE(factory->m_createCount, createdBefore + 1);
  const int delta = drains() - drainsBeforeSpin;
  QVERIFY2(delta >= 1, "the owed work was never delivered");
  QVERIFY2(delta <= 4, qPrintable(QStringLiteral("%1 drains ran for one unblock").arg(delta)));
}

// ---------------------------------------------------------------------------
// The (original syntax -> candidate syntax) transition matrix
// ---------------------------------------------------------------------------

void TestInteractivePreview::testTableSyntaxTransitionMatrix() {
  const QString c_htmlTable = QStringLiteral("<table>\n<tr><td>a</td><td>b</td></tr>\n</table>\n");

  // MARKDOWN -> MARKDOWN: the historical delimiter/prefix logic, unchanged.
  {
    VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
    auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
    QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
    setTextAndSettle(editor, QLatin1String(c_table));
    auto widget = factory->m_widgets.first();

    widget->previewContext()->requestSourceReplacement(
        QStringLiteral("| h1 | h2 |\n| --- | --- |\n| z | b |"));
    QCOMPARE(widget->m_lastResult.status(), PreviewReplacementResult::Accepted);
  }

  // MARKDOWN -> HTML: the first-merge conversion of decision D-h. Accepted
  // because every prefix of the original is empty, which is exactly what D-l
  // guarantees is reachable. Without this case the conversion phases 2 and 3
  // depend on could never be accepted at all.
  {
    VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
    auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
    QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
    setTextAndSettle(editor, QLatin1String(c_table));
    auto widget = factory->m_widgets.first();

    widget->previewContext()->requestSourceReplacement(
        QStringLiteral("<table>\n<tr><td colspan=\"2\">a b</td></tr>\n</table>"));
    QCOMPARE(widget->m_lastResult.status(), PreviewReplacementResult::Accepted);
    QVERIFY(editor.document()->toPlainText().contains(QStringLiteral("colspan=\"2\"")));
  }

  // MARKDOWN -> HTML under a container prefix: refused. Decision D-a means the
  // converted table would not preview at all.
  {
    VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
    auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
    QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
    setTextAndSettle(editor, QStringLiteral("> | a | b |\n> | --- | --- |\n> | c | d |\n"));
    auto widget = factory->m_widgets.first();

    const QString before = editor.document()->toPlainText();
    widget->previewContext()->requestSourceReplacement(
        QStringLiteral("<table>\n<tr><td>a</td></tr>\n</table>"));
    QVERIFY(!widget->m_lastResult.isAccepted());
    QCOMPARE(editor.document()->toPlainText(), before);
  }

  // HTML -> HTML, including the ONE-ROW table the Markdown check rejects
  // outright, and HTML -> MARKDOWN, refused defensively under sticky-HTML D-e.
  {
    VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
    auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
    QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
    setTextAndSettle(editor, c_htmlTable);

    QCOMPARE(factory->m_widgets.size(), 1);
    auto widget = factory->m_widgets.first();
    auto table = widget->m_preview.staticCast<const TablePreview>();
    QCOMPARE(table->syntax(), PreviewTableSyntax::Html);
    QCOMPARE(table->gridRowCount(), 1);
    QCOMPARE(table->gridColumnCount(), 2);
    QVERIFY(!table->hasHeaderRow());

    widget->previewContext()->requestSourceReplacement(
        QStringLiteral("<table>\n<tr><td>z</td><td>b</td></tr>\n</table>"));
    QCOMPARE(widget->m_lastResult.status(), PreviewReplacementResult::Accepted);
    QVERIFY(editor.document()->toPlainText().contains(QStringLiteral("<td>z</td>")));

    const QString before = editor.document()->toPlainText();
    widget->previewContext()->requestSourceReplacement(
        QStringLiteral("| a | b |\n| --- | --- |\n| c | d |"));
    QVERIFY(!widget->m_lastResult.isAccepted());
    QCOMPARE(editor.document()->toPlainText(), before);
  }
}

void TestInteractivePreview::testHtmlTableSourceIsFoldedToItsOwnExtent() {
  // Two symptoms, one cause. A `<table>` opens a CommonMark type-6 HTML block,
  // which nothing in the walker used to emit a folding region for:
  //
  // - the source was never auto-folded, because
  //   MarkdownFoldingProvider::applyPreviewAutoFold() iterates FOLDING REGIONS
  //   and matches a preview whose extent equals one exactly, so an element with
  //   no region of its own is never even visited;
  // - and the only fold covering the table was the enclosing heading section,
  //   which runs to the end of the document.
  //
  // The region now comes from the scanner's exact span, so it stops at
  // `</table>` and the sheet's source folds onto it.
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));

  // Blocks: 0 "## Table", 1 "", 2..6 the table, 7 "", 8 "after", 9 "".
  const QString source = QStringLiteral("## Table\n\n"
                                        "<table>\n"
                                        "<tr><th>a</th></tr>\n"
                                        "<tr><td>b</td></tr>\n"
                                        "<tr><td>c</td></tr>\n"
                                        "</table>\n\n"
                                        "after\n");
  setTextAndSettle(editor, source);

  QCOMPARE(factory->m_widgets.size(), 1);
  QCOMPARE(editor.document()->findBlockByNumber(2).text(), QStringLiteral("<table>"));
  QCOMPARE(editor.document()->findBlockByNumber(6).text(), QStringLiteral("</table>"));

  auto visible = [&editor](int p_block) {
    return editor.document()->findBlockByNumber(p_block).isVisible();
  };

  // TextFolding::setRangeFolded() keeps BOTH boundary lines visible - the first
  // carries the fold marker, and the last is what shows where the range ends,
  // exactly as a folded fenced code block keeps both fences. Everything between
  // them is hidden.
  QVERIFY2(visible(2), "the table's opening tag must stay visible");
  for (int block = 3; block <= 5; ++block) {
    QVERIFY2(!visible(block),
             qPrintable(QStringLiteral("block %1 must be folded away").arg(block)));
  }
  QVERIFY2(visible(6), "the table's closing tag must stay visible");

  // And the fold stops at `</table>`: everything after it is untouched. This is
  // the whole point - a fold derived from the HTML block node would reach the
  // end of the document.
  QVERIFY2(visible(7), "the blank line after the table must stay visible");
  QVERIFY2(visible(8), "text after the table must stay visible");
  QVERIFY2(visible(0), "the heading must stay visible");
}
QTEST_MAIN(tests::TestInteractivePreview)
