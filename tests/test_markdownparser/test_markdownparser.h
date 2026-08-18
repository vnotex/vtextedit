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

  // Shared cmark source-position mapping, and the `=WxH` size extension.
  void testCmarkNodeSpans();
  void testImageSizeElements();

  // The unified snapshot API.
  void testFetchImageLinksSpans();
  void testFetchImageLinksWithoutUrlSpan();
  void testFetchImageLinksClassification();
  void testFetchImageLinksSortContract();
  void testWalkerAndSnapshotAgreeOnRegions();
  void testImageLinkInvariants();
  void testNestedImages();
  void testFileUrlClassification();

  // The HTML `<img>` scanner, and HTML images through the snapshot API and the
  // live walker.
  void testHtmlImgScannerQuoting();
  void testHtmlImgScannerSuppression();
  void testHtmlImgScannerAttrSpans();
  void testFetchImageLinksHtml();
  void testFetchImageLinksHtmlContainers();
  void testFetchImageLinksHtmlRawText();
  void testFetchImageLinksHtmlAfterMultilineConstruct();
  void testFetchImageLinksMixedOrdering();
  void testWalkerHtmlImages();
  void testGenerateImageTag();

  // Extra selection invalidation
  void testCursorLineInvalidationExpanded_data();
  void testCursorLineInvalidationExpanded();
};
} // namespace tests

#endif
