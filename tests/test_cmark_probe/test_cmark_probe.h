#ifndef TESTS_TEST_CMARK_PROBE_H
#define TESTS_TEST_CMARK_PROBE_H

#include <QtTest>

namespace tests
{
    class TestCmarkProbe : public QObject
    {
        Q_OBJECT
    private slots:
        void initTestCase();
        void cleanupTestCase();

        void testPositionModel();
        void testDelimiterBoundary();
        void testExtensionSourcePositions();
        void testMultiLinePositions();
        void testUtf8Columns();
        void testHeadingLevel();
        void testFenceInfo();
        void testListType();
        void testTableStructure();
        void testFirstTableRowIsHeader();
        void testLinkUrl();

        // LineOffsetTable tests
        void testLineOffsetTableAscii();
        void testLineOffsetTableCJK();
        void testLineOffsetTableEmoji();
        void testLineOffsetTableMultiLine();

        // Walker tests
        void testWalkerSimple();
        void testWalkerTable();

        void testParseCmark();
    };
} // ns tests

#endif
