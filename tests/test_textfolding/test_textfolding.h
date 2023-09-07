#ifndef TESTS_TEST_TEXTFOLDING_H
#define TESTS_TEST_TEXTFOLDING_H

#include <QtTest>

#include <textfolding.h>

class QTextDocument;

namespace tests
{
    class TestTextFolding : public QObject
    {
        Q_OBJECT
    private slots:
        void initTestCase();

        // Define test cases here per slot.
        void testNewFoldingRange();

        void textFoldRange();

        void cleanupTestCase();

        // Will be executed before any test function.
        void cleanup();

    private:
        qint64 insertNewFoldingRange(int p_first,
                                     int p_last,
                                     vte::TextFolding::FoldingRangeFlags p_flags = vte::TextFolding::Default);

        QTextDocument *m_doc = nullptr;

        vte::TextFolding *m_textFolding = nullptr;
    };
} // ns tests

#endif
