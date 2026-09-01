#ifndef TESTS_TEST_PREVIEWSCROLLSTABILITY_H
#define TESTS_TEST_PREVIEWSCROLLSTABILITY_H

#include <QObject>
#include <QtTest>

namespace tests {
// Regression coverage for the painted in-place preview scroll compensation in
// EditorPreviewMgr.
//
// A painted preview reserves no space before it exists: its height comes from an
// already-rasterized pixmap. So every preview that lands grows its block, moves
// every later block's offset, and - because QScrollBar keeps its value() across
// the range update - slides whatever the user is reading. On a document full of
// diagrams that repeats once per arrival for several seconds.
//
// EditorPreviewMgr::relayout() pins the first visible block across the relayout
// and puts the scrollbar back so that block keeps its viewport position, and
// EditorPreviewMgr::ensureCursorVisible() no longer scrolls to an off screen
// caret. These are the assertions for both.
//
// Previews are injected through PreviewMgr::updateCodeBlocks(), which is the
// exact slot VNote's PreviewHelper is connected to - nothing here rasterizes a
// diagram, and nothing needs a web engine.
class TestPreviewScrollStability : public QObject {
  Q_OBJECT
public:
  TestPreviewScrollStability() = default;

private slots:
  // Previews arriving above the viewport, one relayout each: the first visible
  // block and its viewport y must not move, while the document grows.
  void testPreviewAboveViewportDoesNotMoveContent();

  // The caret is far above the viewport when a preview lands. The viewport must
  // stay where it is instead of being yanked back to the caret.
  void testOffScreenCaretIsNotChasedTo();

  // The caret IS visible: the old ensureCursorVisible() behaviour must survive.
  void testVisibleCaretStillKeptVisible();

  // At the very top of the document nothing above the viewport can displace
  // anything, so the compensation must not move the bar off the minimum.
  void testNoCompensationAtTopOfDocument();

  // Auto-folding a previewed block HIDES its source, so the document gets
  // shorter - the mirror image of the growth above, and a SEPARATE, later
  // geometry change driven from the preview host's owed-work drain. The
  // viewport must survive that too.
  void testAutoFoldAboveViewportDoesNotMoveContent();

  // The first visible block is INSIDE the range that gets folded, so the anchor
  // itself disappears. The recovery must land on the fold's surviving first
  // block rather than letting the viewport jump.
  void testAutoFoldOfTheAnchorBlockItself();
};
} // namespace tests

#endif // TESTS_TEST_PREVIEWSCROLLSTABILITY_H
