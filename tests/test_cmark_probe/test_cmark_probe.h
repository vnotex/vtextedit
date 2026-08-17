#ifndef TESTS_TEST_CMARK_PROBE_H
#define TESTS_TEST_CMARK_PROBE_H

#include <QtTest>

namespace tests {
class TestCmarkProbe : public QObject {
  Q_OBJECT
private slots:
  void initTestCase();
  void cleanupTestCase();

  void testPositionModel();
  void testDelimiterBoundary();
  void testExtensionSourcePositions();
  void testMultiLinePositions();
  void testUtf8Columns();
  void testHeadingLevel();
  void testFenceInfo();
  void testListType();
  void testTableStructure();
  void testFirstTableRowIsHeader();
  void testLinkUrl();

  // Fork patch survival. The vendored cmark carries local patches against
  // upstream; an upstream merge that dropped them would show up only as a
  // subtle span regression somewhere else.
  void testForkPatchImageSize();
  void testForkPatchMultiLineSpans();
  void testForkPatchUrlPosition();

  // LineOffsetTable tests
  void testLineOffsetTableAscii();
  void testLineOffsetTableCJK();
  void testLineOffsetTableEmoji();
  void testLineOffsetTableMultiLine();

  // Walker tests
  void testWalkerSimple();
  void testWalkerTable();
  void testWalkerListItemInlines();
  void testWalkerLazyContinuation();
  void testWalkerBlockquoteInlines();
  void testWalkerCJKNestedLists();

  void testParseCmark();
};
} // namespace tests

#endif
