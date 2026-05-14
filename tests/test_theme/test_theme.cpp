#include "test_theme.h"

#include <vtextedit/theme.h>

using namespace vte;

namespace tests
{
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

    void TestTheme::cleanupTestCase() {
        // Cleanup after all tests
    }
} // ns tests

QTEST_MAIN(tests::TestTheme)
