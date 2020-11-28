#ifndef TESTS_TEST_TEXTFOLDING_H
#define TESTS_TEST_TEXTFOLDING_H

#include <QtTest>

namespace tests
{
    class TestUtils : public QObject
    {
        Q_OBJECT
    private slots:
        // LruCache Tests.
        void testLruCache();

    };
} // ns tests

#endif
