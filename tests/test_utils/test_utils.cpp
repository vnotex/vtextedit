#include "test_utils.h"

#include <vtextedit/lrucache.h>

using namespace tests;

void TestUtils::testLruCache()
{
    vte::LruCache<int, QString> cache(5, QString());

    QCOMPARE(0, cache.size());

    cache.set(0, "a");
    QCOMPARE(cache.size(), 1);

    cache.set(1, "b");
    QCOMPARE(cache.size(), 2);

    cache.set(2, "c");
    QCOMPARE(cache.size(), 3);

    cache.set(3, "d");
    QCOMPARE(cache.size(), 4);

    cache.set(4, "e");
    QCOMPARE(cache.size(), 5);

    QCOMPARE(cache.get(0), "a");
    QCOMPARE(cache.get(1), "b");
    QCOMPARE(cache.get(2), "c");
    QCOMPARE(cache.get(3), "d");
    QCOMPARE(cache.get(4), "e");
    QVERIFY(cache.get(6).isNull());

    cache.set(5, "f");
    QCOMPARE(cache.size(), 5);
    QVERIFY(cache.get(0).isNull());
    QCOMPARE(cache.get(5), "f");

    QCOMPARE(cache.get(1), "b");

    cache.set(6, "g");
    QCOMPARE(cache.size(), 5);
    QVERIFY(cache.get(2).isNull());
    QCOMPARE(cache.get(6), "g");

    cache.set(6, "h");
    QCOMPARE(cache.get(6), "h");
}

QTEST_MAIN(tests::TestUtils)
