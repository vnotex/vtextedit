#ifndef TESTS_TEST_MARKDOWNPARSER_H
#define TESTS_TEST_MARKDOWNPARSER_H

#include <QtTest>

namespace tests {
class TestMarkdownParser : public QObject {
  Q_OBJECT
private slots:
  void initTestCase();
  void cleanupTestCase();

  // T5: Block element tests
  void testHeadings();
  void testBlockquotes();
  void testBlockquoteNestingDepth();
  void testHorizontalRules();
  void testFencedCodeBlocks();
  void testFencedCodeBlockIndentationFormat();
  void testIndentedCodeBlocks();
  void testHTMLBlocks();
  void testLists();
  void testFrontmatter();
  void testDisplayFormula();
  void testTables();

  // T6: Inline element tests
  void testEmphasis();
  void testStrong();
  void testInlineCode();
  void testLinks();
  void testAutoLinks();
  void testImages();
  void testHTMLInline();
  void testHTMLEntities();
  void testComments();
  void testReferences();
  void testStrikethrough();
  void testMark();
  void testFootnotes();
  void testInlineEquation();

  // T7: Edge case tests
  void testSurrogatePairs();
  void testEmptyElements();
  void testUnclosedDelimiters();
  void testDegenerate();
  void testNestedOverlap();
  void testAllExtensions();

  // T13: Performance benchmark
  void testPerformance();

  // Typed preview element extraction.
  void testTableElementBasic();
  void testTableElementAlignments();
  void testTableElementRawCells();
  void testTableElementEscapedPipes();
  void testTableElementEmptyAndRaggedRows();
  void testTableElementSurrogatePositions();
  void testTableElementNestedPrefixes();
  void testTableElementInvalid();
  void testImageCodeMathElements();
  void testCmarkNodeSpans();

  // Extra selection invalidation
  void testCursorLineInvalidationExpanded_data();
  void testCursorLineInvalidationExpanded();
};
} // namespace tests

#endif
