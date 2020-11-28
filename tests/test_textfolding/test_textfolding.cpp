#include "test_textfolding.h"

#include <QDebug>
#include <QTextDocument>

#include <utils/utils.h>

using namespace tests;

using namespace vte;

void TestTextFolding::initTestCase()
{
    Q_ASSERT(!m_doc);
    m_doc = new QTextDocument(utils::getCppText());
    m_textFolding = new TextFolding(m_doc);
}

void TestTextFolding::cleanupTestCase()
{
    delete m_doc;
    m_doc = nullptr;
}

static bool checkTextBlocksVisible(const QTextDocument *p_doc, int p_first, int p_last)
{
    auto block = p_doc->findBlockByNumber(p_first);
    while (block.isValid() && block.blockNumber() <= p_last) {
        if (!block.isVisible()) {
            return false;
        }

        block = block.next();
    }

    return true;
}

static bool checkTextBlocksInvisible(const QTextDocument *p_doc, int p_first, int p_last)
{
    auto block = p_doc->findBlockByNumber(p_first);
    while (block.isValid() && block.blockNumber() <= p_last) {
        if (block.isVisible()) {
            return false;
        }

        block = block.next();
    }

    return true;
}

void TestTextFolding::cleanup()
{
    m_textFolding->clear();
    QVERIFY(m_textFolding->m_foldingRanges.isEmpty());
    QVERIFY(m_textFolding->m_foldedFoldingRanges.isEmpty());
    QVERIFY(m_textFolding->m_idToFoldingRange.isEmpty());
    QVERIFY(checkTextBlocksVisible(m_doc, 0, m_doc->blockCount()));
}

qint64 TestTextFolding::insertNewFoldingRange(int p_first,
                                              int p_last,
                                              vte::TextFolding::FoldingRangeFlags p_flags)
{
    TextBlockRange range(m_doc->findBlockByNumber(p_first),
                         m_doc->findBlockByNumber(p_last));
    auto id = m_textFolding->newFoldingRange(range, p_flags);
    return id;
}

void TestTextFolding::testNewFoldingRange()
{
    // Invalid new ranges.
    {
        QCOMPARE(insertNewFoldingRange(-1, 5), TextFolding::InvalidRangeId);
        QCOMPARE(insertNewFoldingRange(1, 1), TextFolding::InvalidRangeId);
        QCOMPARE(insertNewFoldingRange(10, 1), TextFolding::InvalidRangeId);
        QCOMPARE(insertNewFoldingRange(10, 1000), TextFolding::InvalidRangeId);
        QVERIFY(m_textFolding->m_foldingRanges.isEmpty());
        QCOMPARE(m_textFolding->debugDump(), QStringLiteral("tree  - folded "));
    }

    // New range [10, 20] (persistent, non-folded).
    {
        auto id = insertNewFoldingRange(10, 20, TextFolding::Persistent);
        Q_UNUSED(id);
        QCOMPARE(m_textFolding->debugDump(), QStringLiteral("tree [10 p 20] - folded "));
        QVERIFY(checkTextBlocksVisible(m_doc, 0, m_doc->blockCount()));
    }

    // Fold range [30, 40] (persistent, folded);
    {
        auto id = insertNewFoldingRange(30, 40, TextFolding::Persistent | TextFolding::Folded);
        Q_UNUSED(id);
        QCOMPARE(m_textFolding->debugDump(),
                 QStringLiteral("tree [10 p 20] [30 pf 40] - folded [30 pf 40]"));
        QVERIFY(checkTextBlocksVisible(m_doc, 30, 30));
        QVERIFY(checkTextBlocksInvisible(m_doc, 31, 40));
    }

    // Nested range [15, 16] is allowed.
    {
        auto id = insertNewFoldingRange(15, 16, TextFolding::Persistent | TextFolding::Folded);
        Q_UNUSED(id);
        QCOMPARE(m_textFolding->debugDump(),
                 QStringLiteral("tree [10 p [15 pf 16] 20] [30 pf 40] - folded [15 pf 16] [30 pf 40]"));
        QVERIFY(checkTextBlocksVisible(m_doc, 15, 15));
        QVERIFY(checkTextBlocksInvisible(m_doc, 16, 16));
        QVERIFY(checkTextBlocksVisible(m_doc, 30, 30));
        QVERIFY(checkTextBlocksInvisible(m_doc, 31, 40));
    }

    // Nested range [10, 12] is not allowed.
    {
        auto id = insertNewFoldingRange(10, 12, TextFolding::Persistent);
        QCOMPARE(id, TextFolding::InvalidRangeId);
        QCOMPARE(m_textFolding->debugDump(),
                 QStringLiteral("tree [10 p [15 pf 16] 20] [30 pf 40] - folded [15 pf 16] [30 pf 40]"));
    }

    // Same new range [10, 20] is not allowed.
    {
        auto id = insertNewFoldingRange(10, 20, TextFolding::Persistent);
        QCOMPARE(id, TextFolding::InvalidRangeId);
        QCOMPARE(m_textFolding->debugDump(),
                 QStringLiteral("tree [10 p [15 pf 16] 20] [30 pf 40] - folded [15 pf 16] [30 pf 40]"));
    }

    // New range containing existing one and starting from the same line is not allowed.
    // [10, 22].
    {
        auto id = insertNewFoldingRange(10, 22, TextFolding::Persistent);
        QCOMPARE(id, TextFolding::InvalidRangeId);
        QCOMPARE(m_textFolding->debugDump(),
                 QStringLiteral("tree [10 p [15 pf 16] 20] [30 pf 40] - folded [15 pf 16] [30 pf 40]"));
    }

    // Nested range [35, 40] ending at the same line is allowed.
    {
        auto id = insertNewFoldingRange(35, 40);
        Q_UNUSED(id);
        QCOMPARE(m_textFolding->debugDump(),
                 QStringLiteral("tree [10 p [15 pf 16] 20] [30 pf [35  40] 40] - folded [15 pf 16] [30 pf 40]"));
        QVERIFY(checkTextBlocksVisible(m_doc, 15, 15));
        QVERIFY(checkTextBlocksInvisible(m_doc, 16, 16));
        QVERIFY(checkTextBlocksVisible(m_doc, 30, 30));
        QVERIFY(checkTextBlocksInvisible(m_doc, 31, 40));
    }

    // New range [28, 40] is allowed.
    {
        auto id = insertNewFoldingRange(28, 40);
        Q_UNUSED(id);
        QCOMPARE(m_textFolding->debugDump(),
                 QStringLiteral("tree [10 p [15 pf 16] 20] [28  [30 pf [35  40] 40] 40] - folded [15 pf 16] [30 pf 40]"));
        QVERIFY(checkTextBlocksVisible(m_doc, 15, 15));
        QVERIFY(checkTextBlocksInvisible(m_doc, 16, 16));
        QVERIFY(checkTextBlocksVisible(m_doc, 30, 30));
        QVERIFY(checkTextBlocksInvisible(m_doc, 31, 40));
    }

    // New range [8, 10] is not allowed.
    {
        auto id = insertNewFoldingRange(8, 10);
        QCOMPARE(id, TextFolding::InvalidRangeId);
        QCOMPARE(m_textFolding->debugDump(),
                 QStringLiteral("tree [10 p [15 pf 16] 20] [28  [30 pf [35  40] 40] 40] - folded [15 pf 16] [30 pf 40]"));
    }

    // New range [8, 11] is not allowed.
    {
        auto id = insertNewFoldingRange(8, 11);
        QCOMPARE(id, TextFolding::InvalidRangeId);
        QCOMPARE(m_textFolding->debugDump(),
                 QStringLiteral("tree [10 p [15 pf 16] 20] [28  [30 pf [35  40] 40] 40] - folded [15 pf 16] [30 pf 40]"));
    }

    // New range [19, 22] is not allowed.
    {
        auto id = insertNewFoldingRange(19, 22);
        QCOMPARE(id, TextFolding::InvalidRangeId);
        QCOMPARE(m_textFolding->debugDump(),
                 QStringLiteral("tree [10 p [15 pf 16] 20] [28  [30 pf [35  40] 40] 40] - folded [15 pf 16] [30 pf 40]"));
    }

    // New range [19, 29] is not allowed.
    {
        auto id = insertNewFoldingRange(19, 29);
        QCOMPARE(id, TextFolding::InvalidRangeId);
        QCOMPARE(m_textFolding->debugDump(),
                 QStringLiteral("tree [10 p [15 pf 16] 20] [28  [30 pf [35  40] 40] 40] - folded [15 pf 16] [30 pf 40]"));
    }
}

void TestTextFolding::textFoldRange()
{
    // New range [10, 20] (persistent, non-folded).
    {
        auto id = insertNewFoldingRange(10, 20, TextFolding::Persistent);
        QCOMPARE(m_textFolding->debugDump(), QStringLiteral("tree [10 p 20] - folded "));
        QVERIFY(checkTextBlocksVisible(m_doc, 0, m_doc->blockCount()));

        m_textFolding->toggleRange(id);
        QCOMPARE(m_textFolding->debugDump(), QStringLiteral("tree [10 pf 20] - folded [10 pf 20]"));
        QVERIFY(checkTextBlocksVisible(m_doc, 10, 10));
        QVERIFY(checkTextBlocksInvisible(m_doc, 11, 20));

        m_textFolding->toggleRange(id);
        QCOMPARE(m_textFolding->debugDump(), QStringLiteral("tree [10 p 20] - folded "));
        QVERIFY(checkTextBlocksVisible(m_doc, 0, m_doc->blockCount()));
    }

    // Fold range [30, 40] (persistent, folded).
    {
        auto id = insertNewFoldingRange(30, 40, TextFolding::Persistent | TextFolding::Folded);
        QCOMPARE(m_textFolding->debugDump(),
                 QStringLiteral("tree [10 p 20] [30 pf 40] - folded [30 pf 40]"));
        QVERIFY(checkTextBlocksVisible(m_doc, 30, 30));
        QVERIFY(checkTextBlocksInvisible(m_doc, 31, 40));

        m_textFolding->toggleRange(id);
        QCOMPARE(m_textFolding->debugDump(),
                 QStringLiteral("tree [10 p 20] [30 p 40] - folded "));
        QVERIFY(checkTextBlocksVisible(m_doc, 0, m_doc->blockCount()));
    }

    // Nested folded range [32, 38] (folded).
    {
        auto id = insertNewFoldingRange(32, 38, TextFolding::Folded);
        QCOMPARE(m_textFolding->debugDump(),
                 QStringLiteral("tree [10 p 20] [30 p [32 f 38] 40] - folded [32 f 38]"));
        QVERIFY(checkTextBlocksVisible(m_doc, 32, 32));
        QVERIFY(checkTextBlocksInvisible(m_doc, 33, 38));

        m_textFolding->toggleRange(id);
        QCOMPARE(m_textFolding->debugDump(),
                 QStringLiteral("tree [10 p 20] [30 p 40] - folded "));
        QVERIFY(checkTextBlocksVisible(m_doc, 0, m_doc->blockCount()));
    }

    // A large folded range [8, 42] (persistent, folded).
    {
        auto id = insertNewFoldingRange(8, 42, TextFolding::Persistent | TextFolding::Folded);
        QCOMPARE(m_textFolding->debugDump(),
                 QStringLiteral("tree [8 pf [10 p 20] [30 p 40] 42] - folded [8 pf 42]"));
        QVERIFY(checkTextBlocksVisible(m_doc, 8, 8));
        QVERIFY(checkTextBlocksInvisible(m_doc, 9, 42));

        auto subId = insertNewFoldingRange(22, 26, TextFolding::Persistent | TextFolding::Folded);
        QCOMPARE(m_textFolding->debugDump(),
                 QStringLiteral("tree [8 pf [10 p 20] [22 pf 26] [30 p 40] 42] - folded [8 pf 42]"));
        QVERIFY(checkTextBlocksVisible(m_doc, 8, 8));
        QVERIFY(checkTextBlocksInvisible(m_doc, 9, 42));

        m_textFolding->toggleRange(id);
        QCOMPARE(m_textFolding->debugDump(),
                 QStringLiteral("tree [8 p [10 p 20] [22 pf 26] [30 p 40] 42] - folded [22 pf 26]"));
        QVERIFY(checkTextBlocksVisible(m_doc, 8, 22));
        QVERIFY(checkTextBlocksInvisible(m_doc, 23, 26));
        QVERIFY(checkTextBlocksVisible(m_doc, 27, 42));

        m_textFolding->toggleRange(subId);
        QCOMPARE(m_textFolding->debugDump(),
                 QStringLiteral("tree [8 p [10 p 20] [22 p 26] [30 p 40] 42] - folded "));
        QVERIFY(checkTextBlocksVisible(m_doc, 8, 42));
    }
}

QTEST_MAIN(tests::TestTextFolding)
