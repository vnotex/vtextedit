#ifndef TESTS_TEST_RICHTEXTEDITOR_H
#define TESTS_TEST_RICHTEXTEDITOR_H

#include <QtTest>

namespace tests {
class TestRichTextEditor : public QObject {
  Q_OBJECT
private slots:
  // Base interface defaults through the public widget.
  void testFoldKeysFallThroughWithoutFolding();
  void testCompletionKeysDoNothing();

  // Mode switching.
  void testSetInputModeSwitchesAndIsIdempotent();
  void testLineMappingIsIdentity();
  void testStatusWidgetEmissionsFollowTheTransitions();

  // Lifetime.
  void testDestructionWithAMountedViStatusWidget();

  // Initial state.
  void testConstructedInViMode();

  // Focus.
  void testFocusTransitionsFireOncePerConsumer();
  void testFocusThroughTheViCommandBar();

  // Vi edits over rich text.
  void testViMotionsKeepFormatting();
  void testViEditCommandsKeepFormatting();

  // Mime data.
  void testMimeDataOfOverriddenSelectionHasHtmlAndPlain();
  void testMimeDataOfAListSelection();

  // Characterisation of the accepted format-lossy paths.
  void testJoinLinesIsFormatLossy();
  void testVscodeMoveLineIsFormatLossy();
  void testVscodeDuplicateLineIsFormatLossy();
  void testViMotionsTraverseATable();
};
} // namespace tests

#endif // TESTS_TEST_RICHTEXTEDITOR_H
