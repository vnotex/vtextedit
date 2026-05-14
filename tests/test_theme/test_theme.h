#ifndef TESTS_TEST_THEME_H
#define TESTS_TEST_THEME_H

#include <QtTest>

namespace tests
{
    class TestTheme : public QObject
    {
        Q_OBJECT
    private slots:
        void initTestCase();

        void testCreateThemeFromContent_validJson();

        void testCreateThemeFromContent_invalidJson();

        void testCreateThemeFromContent_emptyString();

        void cleanupTestCase();
    };
} // ns tests

#endif
