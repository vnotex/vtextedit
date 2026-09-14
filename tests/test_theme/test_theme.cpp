#include "test_theme.h"

#include <vtextedit/theme.h>

using namespace vte;

namespace tests {
void TestTheme::initTestCase() {
  // Setup for all tests
}

void TestTheme::testCreateThemeFromContent_validJson() {
  // Valid theme JSON with required metadata
  const QString validJson = R"({
            "metadata": {
                "type": "vtextedit",
                "name": "TestTheme",
                "version": "1.0"
            },
            "editor": {
                "background": "#ffffff",
                "foreground": "#000000"
            }
        })";

  auto theme = Theme::createThemeFromContent(validJson);
  qDebug() << "Valid JSON test: theme is null?" << theme.isNull();
  QVERIFY(!theme.isNull());
  qDebug() << "Valid JSON test: theme name =" << theme->name();
  QCOMPARE(theme->name(), QString("TestTheme"));
  qDebug() << "Valid JSON test: PASSED";
}

void TestTheme::testCreateThemeFromContent_invalidJson() {
  // Invalid JSON string
  const QString invalidJson = "{not valid json";

  auto theme = Theme::createThemeFromContent(invalidJson);
  QVERIFY(theme.isNull());
}

void TestTheme::testCreateThemeFromContent_emptyString() {
  // Empty string
  const QString emptyJson = "";

  auto theme = Theme::createThemeFromContent(emptyJson);
  QVERIFY(theme.isNull());
}

void TestTheme::testListItemDecorationStyles() {
  const auto theme = Theme::createThemeFromContent(QStringLiteral(R"({
    "metadata": {"type": "vtextedit"},
    "editor-styles": {
      "ListItemGuide": {"text-color": "#123456"},
      "ActiveListItem": {"background-color": "#abcdef"}
    },
    "markdown-editor-styles": {
      "ListItemGuide": {"text-color": "#654321"},
      "ActiveListItem": {"background-color": "#fedcba"}
    }
  })"));
  QVERIFY(theme);
  QCOMPARE(theme->editorStyle(Theme::ListItemGuide).textColor(), QColor("#123456"));
  QCOMPARE(theme->editorStyle(Theme::ActiveListItem).backgroundColor(), QColor("#abcdef"));
  const auto &overrides = theme->markdownEditorStyles();
  QCOMPARE(overrides.value(Theme::ListItemGuide).textColor(), QColor("#654321"));
  QCOMPARE(overrides.value(Theme::ActiveListItem).backgroundColor(), QColor("#fedcba"));
  QVERIFY(!theme->editorStyle(Theme::ListItemGuide).backgroundColor().isValid());
  QVERIFY(!theme->editorStyle(Theme::ActiveListItem).textColor().isValid());
  QVERIFY(!overrides.value(Theme::ListItemGuide).backgroundColor().isValid());
  QVERIFY(!overrides.value(Theme::ActiveListItem).textColor().isValid());

  const auto omitted =
      Theme::createThemeFromContent(QStringLiteral(R"({"metadata": {"type": "vtextedit"}})"));
  QVERIFY(omitted);
  QVERIFY(!omitted->editorStyle(Theme::ListItemGuide).textColor().isValid());
  QVERIFY(!omitted->editorStyle(Theme::ActiveListItem).backgroundColor().isValid());
}

void TestTheme::cleanupTestCase() {
  // Cleanup after all tests
}
} // namespace tests

QTEST_MAIN(tests::TestTheme)
