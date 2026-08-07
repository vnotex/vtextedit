#ifndef TESTS_TEST_TABLEPREVIEW_H
#define TESTS_TEST_TABLEPREVIEW_H

#include <QtTest>

namespace tests {
class TestTablePreview : public QObject {
  Q_OBJECT
private slots:
  // Serializer.
  void testEscapeCellParity();
  void testEscapeCellIdempotent();
  void testSerializeCanonical();
  void testSerializeAlignments();
  void testSerializeRaggedRows();
  void testSerializePreservesPrefixes();
  void testSerializeRejectsUnsafePrefixes();
  void testSerializeRejectsLineSeparators();
  void testSerializePreservesInlineMarkdown();
  void testEscapedPipeDoesNotWidenColumn();
  void testSerializeCapsPadding();

  // Model.
  void testModelNormalization();
  void testModelNoOpCommit();
  void testModelCommitEmitsOnRealChange();
  void testModelAlignmentRole();
  void testModelRoundTrip();

  // Widget.
  void testWidgetRejectsNonTable();
  void testWidgetVisibleRows();
  void testLargeTableFitsVisibleRows();
  void testWidgetRejectsOversizedTable();
  void testWidgetRejectsTooManyRowsOrColumns();
  void testRaggedTableIsNotRoundTrippable();
  void testPreferredSizeCacheTracksContents();
};
} // namespace tests

#endif
