#ifndef TESTS_TEST_ASTWALKER_H
#define TESTS_TEST_ASTWALKER_H

#include <QtTest>

namespace tests {

class TestASTWalker : public QObject {
  Q_OBJECT
private slots:
  void verifyBlocksHighlights();
  void verifyRegions();
  void testFoldingRegionsHeadings();
  void testFoldingRegionsCodeBlock();
  void testFoldingRegionsBlockquote();
  void testFoldingRegionsTable();
  void testFoldingRegionsMathBlock();
  void testFoldingRegionsFrontMatter();
  void testFoldingRegionsMixed();
};

} // namespace tests

#endif
