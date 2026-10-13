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

void TestTheme::testTypstSyntaxStyles() {
  const auto theme = Theme::createThemeFromContent(QStringLiteral(R"({
    "metadata": {"type": "vtextedit"},
    "typst-syntax-styles": {
      "heading": "@markdown-syntax-styles:H2",
      "strong": "@markdown-syntax-styles:STRONG",
      "string": {"text-color": "#abcdef", "bold": true, "italic": true,
                 "underline": true, "strike-through": true},
      "raw": {}
    },
    "markdown-syntax-styles": {
      "H2": {"text-color": "#123456", "background-color": "#654321", "font-size": 19,
             "font-family": "Consolas"},
      "STRONG": {"bold": false, "italic": false, "underline": false, "strike-through": false}
    }
  })"));
  QVERIFY(theme);
  const auto &styles = theme->typstSyntaxStyles();
  const auto heading = styles.value(QStringLiteral("heading")).toTextCharFormat();
  QCOMPARE(heading.foreground().color(), QColor("#123456"));
  QCOMPARE(heading.background().color(), QColor("#654321"));
  QCOMPARE(heading.fontPointSize(), 19.0);
  QCOMPARE(heading.fontFamily(), QStringLiteral("Consolas"));

  auto decorated = styles.value(QStringLiteral("string")).toTextCharFormat();
  QCOMPARE(decorated.foreground().color(), QColor("#abcdef"));
  QCOMPARE(decorated.fontWeight(), int(QFont::Bold));
  QVERIFY(decorated.fontItalic());
  QVERIFY(decorated.fontUnderline());
  QVERIFY(decorated.fontStrikeOut());
  // Explicit false flags must override existing formatting, not just be absent.
  decorated.merge(styles.value(QStringLiteral("strong")).toTextCharFormat());
  QCOMPARE(decorated.fontWeight(), int(QFont::Normal));
  QVERIFY(!decorated.fontItalic());
  QVERIFY(!decorated.fontUnderline());
  QVERIFY(!decorated.fontStrikeOut());
  QCOMPARE(decorated.foreground().color(), QColor("#abcdef"));
  QVERIFY(styles.contains(QStringLiteral("raw")));
  QVERIFY(styles.value(QStringLiteral("raw")).toTextCharFormat().isEmpty());
  QVERIFY(!styles.contains(QStringLiteral("comment")));
}

void TestTheme::testTypstInvalidReferences() {
  const auto theme = Theme::createThemeFromContent(QStringLiteral(R"({
    "metadata": {"type": "vtextedit"},
    "markdown-syntax-styles": {"H1": {}, "EMPH": false, "NOT_A_STYLE": {}},
    "typst-syntax-styles": {
      "heading": "@markdown-syntax-styles:H1",
      "strong": "@markdown-syntax-styles:STRONG",
      "emph": "@markdown-syntax-styles:EMPH",
      "raw": "@markdown-syntax-styles:NOT_A_STYLE",
      "comment": "@markdown-syntax-styles:MaxMarkdownSyntaxStyle",
      "link": "@markdown-syntax-styles:h1",
      "label": "@markdown-syntax-styles:",
      "ref": "@typst-syntax-styles:heading",
      "keyword": "H1",
      "number": null,
      "string": [],
      "operator": 7,
      "function": false
    }
  })"));
  QVERIFY(theme);
  QCOMPARE(theme->typstSyntaxStyles().keys(), QStringList{QStringLiteral("heading")});
  const auto omitted = Theme::createThemeFromContent(
      QStringLiteral(R"({"metadata": {"type": "vtextedit"}})"));
  QVERIFY(omitted);
  QVERIFY(omitted->typstSyntaxStyles().isEmpty());
  const auto noMarkdown = Theme::createThemeFromContent(QStringLiteral(R"({
    "metadata": {"type": "vtextedit"},
    "typst-syntax-styles": {"strong": "@markdown-syntax-styles:STRONG", "raw": {}}
  })"));
  QVERIFY(noMarkdown);
  QCOMPARE(noMarkdown->typstSyntaxStyles().keys(), QStringList{QStringLiteral("raw")});
}

void TestTheme::cleanupTestCase() {
  // Cleanup after all tests
}
} // namespace tests

QTEST_MAIN(tests::TestTheme)
