#ifndef TESTS_TEST_PREVIEWBENCHMARK_H
#define TESTS_TEST_PREVIEWBENCHMARK_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QtTest>

namespace tests {
// Measures the in-place preview pipeline of one VMarkdownEditor holding many
// interactive table previews.
//
// Two kinds of output, and the distinction is load bearing:
//
//   * TIMINGS are logged with qInfo() and written to the evidence file. They
//     are NEVER asserted. The suite runs on shared CI runners, headless on
//     Linux and macOS and on the native platform plugin on Windows, so a wall
//     clock ceiling here would be a flake generator, not a gate.
//
//   * COUNTERS are read from the host's dynamic properties and ARE asserted,
//     but only for facts that hold by construction: that every table is BOUND,
//     that only the ones near the viewport are REALIZED, and that an unchanged
//     pass builds and destroys nothing. The absolute values are recorded so a
//     later change can be shown to have moved them.
//
// "Bound" versus "realized" is the distinction the whole file turns on. Every
// previewable element is bound - it owns a reserved band and folds its source -
// but a widget is only built for it once it is near the viewport, so a widget
// per table is exactly what these scenarios assert is NO LONGER true.
//
// The host is an internal, non-exported class, which is why every counter
// arrives as a QObject dynamic property rather than through a getter: this
// target links the shared VTextEdit and can see no internal symbol on Windows.
class TestPreviewBenchmark : public QObject {
  Q_OBJECT
public:
  TestPreviewBenchmark() = default;

private slots:
  void initTestCase();

  void cleanupTestCase();

  // Construct an editor, set the whole document, settle.
  void benchmarkOpen();

  // updateHighlight() explicitly does NOT reparse when a current result
  // exists; it replays the cached one. So this measures snapshot rebuild,
  // rebinding and publication with the parser taken out of the picture.
  void benchmarkUnchangedRepublish();

  // A full parse whose preview set is identical, driven by an edit and its
  // exact undo far away from any table. Deliberately a separate scenario from
  // the one above: they exercise disjoint halves of the pipeline.
  void benchmarkUnchangedFullParse();

  // Single character inserts far from any table. Records how often identity
  // resolution falls through the exact-anchor index into the linear scan.
  void benchmarkTyping();

  // Scrollbar steps across a document full of previews.
  void benchmarkScroll();

  // Disable and re-enable the table type, i.e. two rebuildAll() passes.
  void benchmarkToggle();

private:
  // n tables of rows x cols GFM pipe cells, separated by prose so each is its
  // own element. Every table stays well under the 300-cell per-table cell
  // highlight guard.
  static QString manyTables(int p_count, int p_rows, int p_cols);

  // The same shape spelled as canonical HTML <table> blocks, which is the
  // form that reaches the per-cell cmark snippet parses.
  static QString manyHtmlTables(int p_count, int p_rows, int p_cols);

  // n fenced graph blocks, cycling through the languages VNote renders out of
  // process. Nothing in vtextedit rasterizes them; they are here so the parse
  // and typing scenarios see a realistic mix.
  static QString manyGraphBlocks(int p_count);

  // Append one "<label>: <value>" line to the evidence report.
  void record(const QString &p_label, const QString &p_value);

  void record(const QString &p_label, qint64 p_value);

  // Everything record() has collected, written out by cleanupTestCase().
  QStringList m_report;
};
} // namespace tests

#endif // TESTS_TEST_PREVIEWBENCHMARK_H
