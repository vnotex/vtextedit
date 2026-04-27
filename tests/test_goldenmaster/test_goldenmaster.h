#ifndef TESTS_TEST_GOLDENMASTER_H
#define TESTS_TEST_GOLDENMASTER_H

#include <QtTest>

namespace tests
{
    class TestGoldenMaster : public QObject
    {
        Q_OBJECT
    private slots:
        void generateGolden();
        void verifyGolden();
    };
} // ns tests

#endif
