#ifndef TESTS_TEST_MARKDOWNFOLDING_H
#define TESTS_TEST_MARKDOWNFOLDING_H

#include <QtTest>

#include <textfolding.h>

class QTextDocument;

namespace vte {
class MarkdownFoldingProvider;
}

namespace tests {
class TestMarkdownFolding : public QObject {
  Q_OBJECT
private slots:
  void initTestCase();

  void testApplyFoldingRegions();

  void testDiffPreservesFoldState();

  void testDiffRemovesStaleRanges();

  void testDiffAddsNewRanges();

  void testSkipsSmallRanges();

  void testNesting();

  void testEmptyRegions();

  void testClearOnDisable();

  // Heading section tests.
  void testHeadingSectionBasic();
  void testHeadingSectionMultiple();
  void testHeadingSectionNested();
  void testHeadingSectionTooSmall();
  void testHeadingSectionAtEnd();
  void testHeadingSectionInsideBlockquote();

  void testEndToEndFolding();

  void testFoldingBlockHeights();

  void cleanupTestCase();

  void cleanup();

private:
  QTextDocument *m_doc = nullptr;
  vte::TextFolding *m_textFolding = nullptr;
  vte::MarkdownFoldingProvider *m_provider = nullptr;
};
} // namespace tests

#endif
