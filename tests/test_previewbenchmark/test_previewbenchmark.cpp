#include "test_previewbenchmark.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextStream>

#include <vtextedit/markdowneditorconfig.h>
#include <vtextedit/markdownhighlighter.h>
#include <vtextedit/previewwidget.h>
#include <vtextedit/texteditorconfig.h>
#include <vtextedit/vmarkdowneditor.h>
#include <vtextedit/vtextedit.h>

using namespace tests;
using namespace vte;

namespace {
// The fixture size every scenario runs at.
//
// 50 tables of 16 cells is large enough that an O(N) pass is plainly separated
// from an O(visible) one, and small enough that the whole target finishes well
// inside the ctest timeout in a Debug build on a loaded runner. Raise it with
// VTE_PREVIEW_BENCHMARK_TABLES when the shape of a curve is what is wanted
// rather than a single point; the recorded baseline for this plan was taken at
// 100.
int tableCount() {
  bool ok = false;
  const int n = qEnvironmentVariableIntValue("VTE_PREVIEW_BENCHMARK_TABLES", &ok);
  return ok && n > 0 ? n : 50;
}

const int c_tableRows = 4;

const int c_tableCols = 4;

// Single character inserts in benchmarkTyping().
const int c_typingSteps = 50;

// Unchanged republish/parse repetitions.
const int c_rounds = 5;

// Scrollbar steps in benchmarkScroll().
const int c_scrollSteps = 100;

QSharedPointer<MarkdownEditorConfig> makeConfig() {
  auto textConfig = QSharedPointer<TextEditorConfig>::create();
  auto config = QSharedPointer<MarkdownEditorConfig>::create(textConfig);
  // The interactive table sheet can rewrite the document, so it is opt-in.
  config->m_inplacePreviewSources |= MarkdownEditorConfig::Table;
  return config;
}

// Drive one parse generation to completion and let the host's zero timers
// drain. Same shape as test_interactivepreview's helper of the same name; the
// wait is what makes a scenario's measurement window closed.
void settle(VMarkdownEditor &p_editor) {
  auto highlighter = p_editor.getHighlighter();
  QSignalSpy completed(highlighter, &MarkdownHighlighter::highlightCompleted);
  highlighter->updateHighlight();
  QTRY_VERIFY_WITH_TIMEOUT(completed.count() > 0, 60000);
  QTest::qWait(50);
  QCoreApplication::processEvents();
}

// Wait for the parse an applied document edit owes. The parse debounce is
// 150 ms, so this is not the same wait as settle()'s.
void settleAfterEdit(VMarkdownEditor &p_editor) {
  auto highlighter = p_editor.getHighlighter();
  QSignalSpy completed(highlighter, &MarkdownHighlighter::highlightCompleted);
  QTRY_VERIFY_WITH_TIMEOUT(completed.count() > 0, 60000);
  QTest::qWait(50);
  QCoreApplication::processEvents();
}

// The host is an internal QObject child, reachable only by its object name.
QObject *previewHost(VMarkdownEditor &p_editor) {
  return p_editor.findChild<QObject *>(QStringLiteral("vte_interactive_preview_host"));
}

// Zero every performance counter. Callers settle first: anything already armed
// and not yet delivered would otherwise be charged to the next window.
void resetCounters(QObject *p_host) {
  if (p_host) {
    p_host->setProperty("vte_preview_counters_reset", true);
  }
}

qint64 counter(QObject *p_host, const char *p_name) {
  return p_host ? p_host->property(p_name).toLongLong() : -1;
}

int previewWidgetCount(VMarkdownEditor &p_editor) {
  return p_editor.getTextEdit()
      ->viewport()
      ->findChildren<PreviewWidget *>(QString(), Qt::FindDirectChildrenOnly)
      .size();
}
} // namespace

QString TestPreviewBenchmark::manyTables(int p_count, int p_rows, int p_cols) {
  QString text;
  for (int t = 0; t < p_count; ++t) {
    // Prose between the tables so each one is a separate element rather than
    // a single table with a continuation row.
    text += QStringLiteral("Paragraph before table %1.\n\n").arg(t);

    QString header;
    QString delimiter;
    for (int c = 0; c < p_cols; ++c) {
      header += QStringLiteral("| h%1 ").arg(c);
      delimiter += QStringLiteral("| --- ");
    }
    text += header + QStringLiteral("|\n") + delimiter + QStringLiteral("|\n");

    for (int r = 1; r < p_rows; ++r) {
      QString row;
      for (int c = 0; c < p_cols; ++c) {
        row += QStringLiteral("| t%1r%2c%3 ").arg(t).arg(r).arg(c);
      }
      text += row + QStringLiteral("|\n");
    }

    text += QStringLiteral("\n");
  }

  return text;
}

QString TestPreviewBenchmark::manyHtmlTables(int p_count, int p_rows, int p_cols) {
  QString text;
  for (int t = 0; t < p_count; ++t) {
    text += QStringLiteral("Paragraph before html table %1.\n\n").arg(t);
    text += QStringLiteral("<table>\n");
    for (int r = 0; r < p_rows; ++r) {
      // Row 0 is entirely <th>, every later row entirely <td>: the only two
      // header shapes the canonical subset accepts.
      const QString tag = r == 0 ? QStringLiteral("th") : QStringLiteral("td");
      text += QStringLiteral("<tr>\n");
      for (int c = 0; c < p_cols; ++c) {
        // The `<!--vte-md:...-->` payload is what makes the table MARKDOWN
        // BACKED, and Markdown backing is the only thing that makes the walker
        // parse each cell as its own cmark document. Without a payload the
        // table is HTML-only, its cells are literal text, and this fixture
        // would exercise none of the per-cell snippet parsing it exists to
        // measure.
        text += QStringLiteral("<%1><!--vte-md:**t%2r%3c%4**--><p><strong>t%2r%3c%4</strong>"
                               "</p></%1>\n")
                    .arg(tag)
                    .arg(t)
                    .arg(r)
                    .arg(c);
      }
      text += QStringLiteral("</tr>\n");
    }
    text += QStringLiteral("</table>\n\n");
  }

  return text;
}

QString TestPreviewBenchmark::manyGraphBlocks(int p_count) {
  static const char *langs[] = {"puml", "dot", "mermaid", "flow"};
  QString text;
  for (int i = 0; i < p_count; ++i) {
    text += QStringLiteral("Paragraph before graph %1.\n\n").arg(i);
    text += QStringLiteral("```%1\nnode%2 -> node%3\n```\n\n")
                .arg(QLatin1String(langs[i % 4]))
                .arg(i)
                .arg(i + 1);
  }

  return text;
}

void TestPreviewBenchmark::record(const QString &p_label, const QString &p_value) {
  m_report.append(QStringLiteral("%1: %2").arg(p_label, p_value));
}

void TestPreviewBenchmark::record(const QString &p_label, qint64 p_value) {
  record(p_label, QString::number(p_value));
}

void TestPreviewBenchmark::initTestCase() {
  m_report.append(QStringLiteral("In-place preview benchmark"));
  m_report.append(
      QStringLiteral("Date: %1").arg(QDateTime::currentDateTime().toString(Qt::ISODate)));
  m_report.append(QStringLiteral("Qt: %1").arg(QLatin1String(qVersion())));
  m_report.append(QStringLiteral("Platform plugin: %1").arg(QGuiApplication::platformName()));
  m_report.append(QStringLiteral("Fixture: %1 tables of %2x%3 (%4 cells)")
                      .arg(tableCount())
                      .arg(c_tableRows)
                      .arg(c_tableCols)
                      .arg(tableCount() * c_tableRows * c_tableCols));
  m_report.append(QString());
}

void TestPreviewBenchmark::cleanupTestCase() {
  QDir evidenceDir(QStringLiteral(EVIDENCE_DIR));
  if (!evidenceDir.exists()) {
    evidenceDir.mkpath(QStringLiteral("."));
  }

  QFile out(evidenceDir.filePath(QStringLiteral("preview-benchmark.txt")));
  // A benchmark that cannot write its report is still a benchmark that ran, so
  // this is a warning rather than a failure: the numbers are also in the log.
  if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
    qWarning() << "cannot write the evidence file" << out.fileName();
    return;
  }

  QTextStream ts(&out);
  for (const auto &line : m_report) {
    ts << line << "\n";
  }
  out.close();

  qInfo() << "evidence written to" << out.fileName();
}

void TestPreviewBenchmark::benchmarkOpen() {
  const QString text = manyTables(tableCount(), c_tableRows, c_tableCols);

  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.resize(900, 600);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));

  auto host = previewHost(editor);
  QVERIFY(host);
  resetCounters(host);

  QElapsedTimer timer;
  timer.start();
  editor.setText(text);
  settle(editor);
  const qint64 elapsed = timer.elapsed();

  // Every table is claimed by the built-in factory, so the eager pipeline
  // realizes exactly one widget per table. This is the assertion a lazy
  // realization change is expected to move.
  // Every table is BOUND - that is what reserves its band and what makes it
  // fold - but only the handful within a viewport height of the top is
  // REALIZED. This is the assertion that states the whole point of the lazy
  // path; before it, this was one widget per table.
  const qint64 realized = counter(host, "vte_preview_widgets_realized");
  QVERIFY2(realized >= 1, "nothing was realized at all - the viewport band is broken");
  QVERIFY2(
      realized < qint64(tableCount()),
      qPrintable(
          QStringLiteral("every one of the %1 tables was realized eagerly").arg(tableCount())));
  QCOMPARE(previewWidgetCount(editor), int(realized));

  // Not an equality: setText() and the settle() that follows it are two parse
  // generations, so every item is created once and then updated in place at
  // least once. What matters is that no item is missing.
  QVERIFY2(
      counter(host, "vte_preview_previews_bound") >= qint64(tableCount()),
      qPrintable(QStringLiteral("only %1 bound").arg(counter(host, "vte_preview_previews_bound"))));

  // Cells are built by realization, so this now tracks the realized subset
  // rather than the whole document.
  const qint64 cells = counter(host, "vte_preview_table_cells_built");
  QVERIFY2(
      cells <= realized * c_tableRows * c_tableCols,
      qPrintable(QStringLiteral("%1 cells built for %2 realized tables").arg(cells).arg(realized)));

  qInfo() << "open:" << elapsed << "ms," << tableCount() << "tables bound," << realized
          << "realized," << cells << "cells," << counter(host, "vte_preview_publishes")
          << "publishes";

  record(QStringLiteral("open.ms"), elapsed);
  record(QStringLiteral("open.widgetsRealized"), realized);
  record(QStringLiteral("open.previewsBound"), counter(host, "vte_preview_previews_bound"));
  record(QStringLiteral("open.tableCellsBuilt"), cells);
  record(QStringLiteral("open.tablesBound"), tableCount());
  record(QStringLiteral("open.publishes"), counter(host, "vte_preview_publishes"));
  record(QStringLiteral("open.geometrySetCalls"), counter(host, "vte_preview_geometry_set_calls"));
  record(QStringLiteral("open.identityFallbackHits"),
         counter(host, "vte_preview_identity_fallback_hits"));
}

void TestPreviewBenchmark::benchmarkUnchangedRepublish() {
  const QString text = manyTables(tableCount(), c_tableRows, c_tableCols);

  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.resize(900, 600);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  editor.setText(text);
  settle(editor);

  auto host = previewHost(editor);
  QVERIFY(host);
  resetCounters(host);

  // updateHighlight() replays the cached parse result rather than reparsing,
  // so the whole measured cost is snapshot rebuild, rebinding and publication.
  const int rounds = c_rounds;
  QElapsedTimer timer;
  timer.start();
  for (int i = 0; i < rounds; ++i) {
    settle(editor);
  }
  const qint64 elapsed = timer.elapsed();

  // Nothing changed, so no widget may be built or torn down: every item is
  // matched by its exact anchor and updated in place.
  QCOMPARE(counter(host, "vte_preview_widgets_realized"), qint64(0));
  QCOMPARE(counter(host, "vte_preview_widgets_destroyed"), qint64(0));
  QCOMPARE(counter(host, "vte_preview_table_cells_built"), qint64(0));
  QCOMPARE(counter(host, "vte_preview_identity_fallback_hits"), qint64(0));
  QCOMPARE(counter(host, "vte_preview_previews_bound"), qint64(tableCount()) * rounds);

  // And the document is not re-measured. This is the assertion that pins the
  // measurement cache: a realized sheet measures by laying its QTextDocument
  // out, so re-measuring elements whose source did not change is the single
  // most expensive thing an unchanged publication could do.
  //
  // The gate is "does not SCALE", not "is exactly zero". An uncached
  // implementation measures every item on every round - tableCount() * rounds -
  // whereas a small constant residue is legitimate: a late realization or a
  // width basis that settles once both re-measure a handful of items, and
  // neither grows with the number of rounds.
  const qint64 republishMeasurements = counter(host, "vte_preview_measurements");
  QVERIFY2(republishMeasurements < qint64(tableCount()),
           qPrintable(QStringLiteral("%1 measurements over %2 unchanged rounds of %3 items")
                          .arg(republishMeasurements)
                          .arg(rounds)
                          .arg(tableCount())));

  const double perRound = static_cast<double>(elapsed) / rounds;
  qInfo() << "unchanged republish:" << perRound << "ms/round over" << rounds << "rounds,"
          << counter(host, "vte_preview_geometry_set_calls") << "setGeometry calls";

  record(QStringLiteral("republish.rounds"), rounds);
  record(QStringLiteral("republish.msPerRound"), QString::number(perRound, 'f', 2));
  record(QStringLiteral("republish.measurements"), counter(host, "vte_preview_measurements"));
  record(QStringLiteral("republish.previewsBound"), counter(host, "vte_preview_previews_bound"));
  record(QStringLiteral("republish.publishes"), counter(host, "vte_preview_publishes"));
  record(QStringLiteral("republish.geometrySetCalls"),
         counter(host, "vte_preview_geometry_set_calls"));
}

void TestPreviewBenchmark::benchmarkUnchangedFullParse() {
  // HTML tables, because their cells are the ones that reach the per-cell
  // cmark snippet parse this scenario is meant to isolate.
  const QString text = manyHtmlTables(tableCount(), c_tableRows, c_tableCols);

  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.resize(900, 600);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  editor.setText(text);
  settle(editor);

  auto host = previewHost(editor);
  QVERIFY(host);
  const QString before = editor.document()->toPlainText();
  resetCounters(host);

  // There is no public "reparse now" seam, and updateHighlight() deliberately
  // will not reparse. A real full parse is therefore driven by an edit and its
  // exact undo, appended past the last table so the preview set either side is
  // identical. Two parses are measured; the document ends as it started.
  const int rounds = c_rounds;
  QElapsedTimer timer;
  timer.start();
  for (int i = 0; i < rounds; ++i) {
    QTextCursor cursor(editor.document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(QStringLiteral("x"));
    settleAfterEdit(editor);

    cursor.deletePreviousChar();
    settleAfterEdit(editor);
  }
  const qint64 elapsed = timer.elapsed();

  QCOMPARE(editor.document()->toPlainText(), before);

  // The cost this scenario isolates. Cell syntax highlighting parses each
  // Markdown-backed cell as its own cmark document, so a full parse of a
  // document of HTML tables pays one parse per cell - and the walker's
  // document-wide budget is what stops that growing without bound.
  //
  // tableCellsBuilt is deliberately NOT the measure here: it counts widget
  // construction, which an unchanged parse does not perform at all.
  const qint64 snippetParses = counter(host, "vte_preview_snippet_parses");
  QVERIFY2(snippetParses > 0,
           "no per-cell snippet parses at all - the fixture is not Markdown backed");

  const double perParse = static_cast<double>(elapsed) / (rounds * 2);
  qInfo() << "unchanged full parse:" << perParse << "ms/parse over" << (rounds * 2) << "parses,"
          << snippetParses << "per-cell snippet parses";

  record(QStringLiteral("fullParse.parses"), rounds * 2);
  record(QStringLiteral("fullParse.msPerParse"), QString::number(perParse, 'f', 2));
  record(QStringLiteral("fullParse.previewsBound"), counter(host, "vte_preview_previews_bound"));
  record(QStringLiteral("fullParse.snippetParses"), snippetParses);
  record(QStringLiteral("fullParse.snippetParsesPerParse"),
         QString::number(static_cast<double>(snippetParses) / (rounds * 2), 'f', 1));
  record(QStringLiteral("fullParse.identityFallbackHits"),
         counter(host, "vte_preview_identity_fallback_hits"));
}

void TestPreviewBenchmark::benchmarkTyping() {
  // A prose paragraph at the very top, so every insert is far from every
  // table and shifts all of them by one character.
  const QString text =
      QStringLiteral("Typing here.\n\n") + manyTables(tableCount(), c_tableRows, c_tableCols);

  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.resize(900, 600);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  editor.setText(text);
  settle(editor);

  auto host = previewHost(editor);
  QVERIFY(host);
  resetCounters(host);

  QElapsedTimer timer;
  timer.start();
  for (int i = 0; i < c_typingSteps; ++i) {
    QTextCursor cursor(editor.document());
    cursor.setPosition(QStringLiteral("Typing here.").size());
    cursor.insertText(QStringLiteral("a"));
    settleAfterEdit(editor);
  }
  const qint64 elapsed = timer.elapsed();

  // The measurement §1b of the plan is gated on: every table moved by one
  // character on every keystroke, so if the exact-anchor index is doing its
  // job this is zero, and the linear overlap scan is cold.
  const qint64 fallback = counter(host, "vte_preview_identity_fallback_hits");
  const qint64 measurements = counter(host, "vte_preview_measurements");
  const double perKeystroke = static_cast<double>(elapsed) / c_typingSteps;

  // Every table is rebound on every keystroke, but none of them changed, so
  // typing in a paragraph must not lay the tables out. The gate is that the
  // count does not scale with the keystrokes: an uncached implementation
  // measures tableCount() items on every one of them.
  QVERIFY2(measurements < qint64(tableCount()),
           qPrintable(QStringLiteral("%1 measurements over %2 keystrokes against %3 items")
                          .arg(measurements)
                          .arg(c_typingSteps)
                          .arg(tableCount())));

  qInfo() << "typing:" << perKeystroke << "ms/keystroke over" << c_typingSteps << "keystrokes,"
          << fallback << "identity fallback hits," << measurements << "measurements,"
          << counter(host, "vte_preview_widgets_realized") << "widgets realized";

  record(QStringLiteral("typing.keystrokes"), c_typingSteps);
  record(QStringLiteral("typing.msPerKeystroke"), QString::number(perKeystroke, 'f', 2));
  record(QStringLiteral("typing.identityFallbackHits"), fallback);
  record(QStringLiteral("typing.measurements"), measurements);
  record(QStringLiteral("typing.widgetsRealized"), counter(host, "vte_preview_widgets_realized"));
  record(QStringLiteral("typing.widgetsDestroyed"), counter(host, "vte_preview_widgets_destroyed"));
  record(QStringLiteral("typing.tableCellsBuilt"), counter(host, "vte_preview_table_cells_built"));
  record(QStringLiteral("typing.publishes"), counter(host, "vte_preview_publishes"));
  record(QStringLiteral("typing.geometrySetCalls"),
         counter(host, "vte_preview_geometry_set_calls"));
}

void TestPreviewBenchmark::benchmarkScroll() {
  const QString text = manyTables(tableCount(), c_tableRows, c_tableCols);

  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.resize(900, 600);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  editor.setText(text);
  settle(editor);

  auto host = previewHost(editor);
  QVERIFY(host);

  auto vbar = editor.getTextEdit()->verticalScrollBar();
  QVERIFY2(vbar->maximum() > vbar->minimum(), "the fixture does not scroll");

  const int span = vbar->maximum() - vbar->minimum();
  const int step = qMax(1, span / c_scrollSteps);

  resetCounters(host);

  QElapsedTimer timer;
  timer.start();
  for (int i = 0; i < c_scrollSteps; ++i) {
    vbar->setValue(vbar->minimum() + step * i);
    QCoreApplication::processEvents();
  }
  const qint64 elapsed = timer.elapsed();

  // The placement pass deliberately does not publish the counters - nine
  // property writes per scroll tick would cost as much as the placement being
  // measured - so let the owed-work drain settle and publish them before they
  // are read. Outside the timed region: this is measurement bookkeeping, not
  // scroll cost.
  QTest::qWait(100);
  QCoreApplication::processEvents();

  // Scrolling realizes the items it brings into the band - that is the demand
  // side of lazy realization - but it must never destroy one, and it must
  // never rebind a snapshot: nothing about the document changed.
  QCOMPARE(counter(host, "vte_preview_widgets_destroyed"), qint64(0));
  QCOMPARE(counter(host, "vte_preview_previews_bound"), qint64(0));

  const qint64 realized = counter(host, "vte_preview_widgets_realized");
  const qint64 placements = counter(host, "vte_preview_geometry_set_calls");
  const double perTick = static_cast<double>(elapsed) / c_scrollSteps;
  const double placementsPerTick = static_cast<double>(placements) / c_scrollSteps;

  // A SECOND pass over the same range, with everything it touches already
  // realized. The first pass pays for both placement and one-time widget
  // construction, and conflating the two hides which of them the placement
  // index actually improved; this isolates the steady state, which is what a
  // user scrolling back and forth through a document experiences.
  resetCounters(host);

  QElapsedTimer steadyTimer;
  steadyTimer.start();
  for (int i = 0; i < c_scrollSteps; ++i) {
    vbar->setValue(vbar->minimum() + step * i);
    QCoreApplication::processEvents();
  }
  const qint64 steadyElapsed = steadyTimer.elapsed();

  QTest::qWait(100);
  QCoreApplication::processEvents();

  const qint64 steadyRealized = counter(host, "vte_preview_widgets_realized");
  const double steadyPerTick = static_cast<double>(steadyElapsed) / c_scrollSteps;
  const double steadyPlacementsPerTick =
      static_cast<double>(counter(host, "vte_preview_geometry_set_calls")) / c_scrollSteps;

  qInfo() << "scroll (first pass):" << perTick << "ms/tick," << placementsPerTick
          << "setGeometry per tick," << realized << "realized on the way";
  qInfo() << "scroll (steady state):" << steadyPerTick << "ms/tick," << steadyPlacementsPerTick
          << "setGeometry per tick," << steadyRealized << "realized, against" << tableCount()
          << "items";

  record(QStringLiteral("scroll.ticks"), c_scrollSteps);
  record(QStringLiteral("scroll.msPerTick"), QString::number(perTick, 'f', 2));
  record(QStringLiteral("scroll.geometrySetCalls"), placements);
  record(QStringLiteral("scroll.geometrySetCallsPerTick"),
         QString::number(placementsPerTick, 'f', 2));
  record(QStringLiteral("scroll.widgetsRealized"), realized);
  record(QStringLiteral("scroll.steady.msPerTick"), QString::number(steadyPerTick, 'f', 2));
  record(QStringLiteral("scroll.steady.geometrySetCallsPerTick"),
         QString::number(steadyPlacementsPerTick, 'f', 2));
  record(QStringLiteral("scroll.steady.widgetsRealized"), steadyRealized);
  record(QStringLiteral("scroll.items"), tableCount());
}

void TestPreviewBenchmark::benchmarkToggle() {
  const QString text = manyTables(tableCount(), c_tableRows, c_tableCols);

  // Held for the whole scenario: there is no getter returning the markdown
  // config back, so the toggle has to mutate and re-hand the very object the
  // editor was constructed with.
  auto config = makeConfig();
  VMarkdownEditor editor(config, QSharedPointer<TextEditorParameters>::create());
  editor.resize(900, 600);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  editor.setText(text);
  settle(editor);

  auto host = previewHost(editor);
  QVERIFY(host);
  // How many widgets exist going in. Only those can be torn down, and after
  // lazy realization that is the realized subset, not every table.
  const int realizedBefore = previewWidgetCount(editor);
  QVERIFY2(realizedBefore > 0, "nothing was realized, so the disable half measures nothing");
  resetCounters(host);

  // Disable: every widget is torn down. m_lastPreviews still holds the table
  // snapshots, so this half needs no reparse - which is exactly what T5 of the
  // plan is about, and these counters are how it is shown.
  QElapsedTimer disableTimer;
  disableTimer.start();
  config->m_inplacePreviewSources &= ~MarkdownEditorConfig::Table;
  editor.setConfig(config);
  // Deliberately NOT settle(): settle() calls updateHighlight() itself, which
  // is precisely the reparse the disable path is supposed to no longer need.
  // Driving one here would measure the cost this scenario exists to show is
  // gone.
  //
  // The gate is the observable outcome, not a fixed sleep: a wall-clock wait
  // long enough on this machine is a flake on a loaded CI runner, and it would
  // make every assertion below secretly time dependent.
  QTRY_COMPARE_WITH_TIMEOUT(previewWidgetCount(editor), 0, 60000);
  QCoreApplication::processEvents();
  const qint64 disableMs = disableTimer.elapsed();

  QCOMPARE(previewWidgetCount(editor), 0);
  const qint64 destroyed = counter(host, "vte_preview_widgets_destroyed");
  const qint64 disableRebuilds = counter(host, "vte_preview_rebuild_alls");
  QCOMPARE(destroyed, qint64(realizedBefore));

  resetCounters(host);

  // Enable: the items are bound again, and the ones near the viewport are
  // realized again. Everything else costs an estimate.
  QElapsedTimer enableTimer;
  enableTimer.start();
  config->m_inplacePreviewSources |= MarkdownEditorConfig::Table;
  editor.setConfig(config);
  settle(editor);
  const qint64 enableMs = enableTimer.elapsed();

  const qint64 realized = counter(host, "vte_preview_widgets_realized");
  const qint64 cells = counter(host, "vte_preview_table_cells_built");
  QCOMPARE(previewWidgetCount(editor), int(realized));
  QVERIFY2(realized >= 1, "re-enabling realized nothing");
  QVERIFY2(realized < qint64(tableCount()), "re-enabling realized every table eagerly");

  qInfo() << "toggle: disable" << disableMs << "ms (" << destroyed << "widgets destroyed,"
          << disableRebuilds << "rebuildAll), enable" << enableMs << "ms (" << realized
          << "widgets realized," << cells << "cells built)";

  record(QStringLiteral("toggle.disable.ms"), disableMs);
  record(QStringLiteral("toggle.disable.widgetsDestroyed"), destroyed);
  record(QStringLiteral("toggle.disable.rebuildAlls"), disableRebuilds);
  record(QStringLiteral("toggle.enable.ms"), enableMs);
  record(QStringLiteral("toggle.enable.widgetsRealized"), realized);
  record(QStringLiteral("toggle.enable.tableCellsBuilt"), cells);
  record(QStringLiteral("toggle.enable.rebuildAlls"), counter(host, "vte_preview_rebuild_alls"));
}

QTEST_MAIN(tests::TestPreviewBenchmark)
