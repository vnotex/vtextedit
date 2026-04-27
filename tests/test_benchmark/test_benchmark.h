#ifndef TESTS_TEST_BENCHMARK_H
#define TESTS_TEST_BENCHMARK_H

#include <QtTest>

namespace tests
{
    class TestBenchmark : public QObject
    {
        Q_OBJECT
    private slots:
        void initTestCase();
        void benchmarkParse();
    };
} // ns tests

#endif
