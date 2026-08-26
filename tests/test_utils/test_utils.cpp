#include "test_utils.h"

#include <vtextedit/lrucache.h>

#include "scrollbar.h"

using namespace tests;

void TestUtils::testLruCache() {
  vte::LruCache<int, QString> cache(5, QString());

  QCOMPARE(0, (int)cache.size());

  cache.set(0, "a");
  QCOMPARE((int)cache.size(), 1);

  cache.set(1, "b");
  QCOMPARE((int)cache.size(), 2);

  cache.set(2, "c");
  QCOMPARE((int)cache.size(), 3);

  cache.set(3, "d");
  QCOMPARE((int)cache.size(), 4);

  cache.set(4, "e");
  QCOMPARE((int)cache.size(), 5);

  QCOMPARE(cache.get(0), "a");
  QCOMPARE(cache.get(1), "b");
  QCOMPARE(cache.get(2), "c");
  QCOMPARE(cache.get(3), "d");
  QCOMPARE(cache.get(4), "e");
  QVERIFY(cache.get(6).isNull());

  cache.set(5, "f");
  QCOMPARE((int)cache.size(), 5);
  QVERIFY(cache.get(0).isNull());
  QCOMPARE(cache.get(5), "f");

  QCOMPARE(cache.get(1), "b");

  cache.set(6, "g");
  QCOMPARE((int)cache.size(), 5);
  QVERIFY(cache.get(2).isNull());
  QCOMPARE(cache.get(6), "g");

  cache.set(6, "h");
  QCOMPARE(cache.get(6), "h");
}

// The bar answers a range change by extending its maximum, so the bottom of
// the content can still be scrolled up.
void TestUtils::testScrollBarExtendsTheMaximum() {
  vte::ScrollBar bar(Qt::Vertical, nullptr);
  bar.setSingleStep(10);
  bar.setPageStep(100);

  bar.setRange(0, 500);
  QCOMPARE(bar.maximum(), 500 + 100 - 10 * 3);

  // A second range change is extended just the same, from the new raw range
  // rather than on top of the previous extension.
  bar.setRange(0, 300);
  QCOMPARE(bar.maximum(), 300 + 100 - 10 * 3);
}

// The extension can work out to the maximum the bar already has, in which case
// setMaximum() emits nothing. The recursion guard must not be left armed by
// that, or it swallows the next genuine range change instead.
void TestUtils::testScrollBarKeepsExtendingAfterANoOpExtension() {
  vte::ScrollBar bar(Qt::Vertical, nullptr);
  // pageStep == singleStep * 3, so the extension adds nothing at all.
  bar.setSingleStep(10);
  bar.setPageStep(30);

  bar.setRange(0, 500);
  QCOMPARE(bar.maximum(), 500);

  // The next range change is a real one and must still be extended.
  bar.setPageStep(100);
  bar.setRange(0, 400);
  QCOMPARE(bar.maximum(), 400 + 100 - 10 * 3);
}

QTEST_MAIN(tests::TestUtils)
