#include "test_textfolding.h"

#include <QDebug>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>

#include <extraselectionmgr.h>
#include <utils/utils.h>

using namespace tests;

using namespace vte;

namespace {
// Terminates ExtraSelectionMgr's delivery before EditorExtraSelection, so no
// QTextEdit and no document layout are needed.
class RecordingExtraSelectionInterface : public vte::ExtraSelectionInterface {
public:
  QTextCursor textCursor() const Q_DECL_OVERRIDE { return QTextCursor(); }

  QString selectedText() const Q_DECL_OVERRIDE { return QString(); }

  void setExtraSelections(const QList<QTextEdit::ExtraSelection> &p_selections) Q_DECL_OVERRIDE {
    m_selections = p_selections;
  }

  QList<QTextCursor> findAllText(const QString &p_text, bool p_isRegularExpression,
                                 bool p_caseSensitive) Q_DECL_OVERRIDE {
    Q_UNUSED(p_text);
    Q_UNUSED(p_isRegularExpression);
    Q_UNUSED(p_caseSensitive);
    return QList<QTextCursor>();
  }

  QList<QTextEdit::ExtraSelection> m_selections;
};
} // namespace

void TestTextFolding::initTestCase() {
  Q_ASSERT(!m_doc);
  m_doc = new QTextDocument(utils::getCppText());
  m_textFolding = new TextFolding(m_doc);
}

void TestTextFolding::cleanupTestCase() {
  delete m_doc;
  m_doc = nullptr;
}

static bool checkTextBlocksVisible(const QTextDocument *p_doc, int p_first, int p_last) {
  auto block = p_doc->findBlockByNumber(p_first);
  while (block.isValid() && block.blockNumber() <= p_last) {
    if (!block.isVisible()) {
      return false;
    }

    block = block.next();
  }

  return true;
}

static bool checkTextBlocksInvisible(const QTextDocument *p_doc, int p_first, int p_last) {
  auto block = p_doc->findBlockByNumber(p_first);
  while (block.isValid() && block.blockNumber() <= p_last) {
    if (block.isVisible()) {
      return false;
    }

    block = block.next();
  }

  return true;
}

void TestTextFolding::cleanup() {
  m_textFolding->clear();
  QVERIFY(m_textFolding->m_foldingRanges.isEmpty());
  QVERIFY(m_textFolding->m_foldedFoldingRanges.isEmpty());
  QVERIFY(m_textFolding->m_idToFoldingRange.isEmpty());
  QVERIFY(checkTextBlocksVisible(m_doc, 0, m_doc->blockCount()));
}

qint64 TestTextFolding::insertNewFoldingRange(int p_first, int p_last,
                                              vte::TextFolding::FoldingRangeFlags p_flags) {
  TextBlockRange range(m_doc->findBlockByNumber(p_first), m_doc->findBlockByNumber(p_last));
  auto id = m_textFolding->newFoldingRange(range, p_flags);
  return id;
}

void TestTextFolding::testNewFoldingRange() {
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
    QVERIFY(checkTextBlocksInvisible(m_doc, 31, 39));
    QVERIFY(checkTextBlocksVisible(m_doc, 40, 40));
  }

  // Nested range [15, 16] is allowed.
  {
    auto id = insertNewFoldingRange(15, 16, TextFolding::Persistent | TextFolding::Folded);
    Q_UNUSED(id);
    QCOMPARE(m_textFolding->debugDump(),
             QStringLiteral("tree [10 p [15 pf 16] 20] [30 pf 40] - folded [15 pf 16] [30 pf 40]"));
    QVERIFY(checkTextBlocksVisible(m_doc, 15, 16));
    QVERIFY(checkTextBlocksVisible(m_doc, 30, 30));
    QVERIFY(checkTextBlocksInvisible(m_doc, 31, 39));
    QVERIFY(checkTextBlocksVisible(m_doc, 40, 40));
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
             QStringLiteral(
                 "tree [10 p [15 pf 16] 20] [30 pf [35  40] 40] - folded [15 pf 16] [30 pf 40]"));
    QVERIFY(checkTextBlocksVisible(m_doc, 15, 16));
    QVERIFY(checkTextBlocksVisible(m_doc, 30, 30));
    QVERIFY(checkTextBlocksInvisible(m_doc, 31, 39));
    QVERIFY(checkTextBlocksVisible(m_doc, 40, 40));
  }

  // New range [28, 40] is allowed.
  {
    auto id = insertNewFoldingRange(28, 40);
    Q_UNUSED(id);
    QCOMPARE(m_textFolding->debugDump(),
             QStringLiteral("tree [10 p [15 pf 16] 20] [28  [30 pf [35  40] 40] 40] - folded [15 "
                            "pf 16] [30 pf 40]"));
    QVERIFY(checkTextBlocksVisible(m_doc, 15, 16));
    QVERIFY(checkTextBlocksVisible(m_doc, 30, 30));
    QVERIFY(checkTextBlocksInvisible(m_doc, 31, 39));
    QVERIFY(checkTextBlocksVisible(m_doc, 40, 40));
  }

  // New range [8, 10] is not allowed.
  {
    auto id = insertNewFoldingRange(8, 10);
    QCOMPARE(id, TextFolding::InvalidRangeId);
    QCOMPARE(m_textFolding->debugDump(),
             QStringLiteral("tree [10 p [15 pf 16] 20] [28  [30 pf [35  40] 40] 40] - folded [15 "
                            "pf 16] [30 pf 40]"));
  }

  // New range [8, 11] is not allowed.
  {
    auto id = insertNewFoldingRange(8, 11);
    QCOMPARE(id, TextFolding::InvalidRangeId);
    QCOMPARE(m_textFolding->debugDump(),
             QStringLiteral("tree [10 p [15 pf 16] 20] [28  [30 pf [35  40] 40] 40] - folded [15 "
                            "pf 16] [30 pf 40]"));
  }

  // New range [19, 22] is not allowed.
  {
    auto id = insertNewFoldingRange(19, 22);
    QCOMPARE(id, TextFolding::InvalidRangeId);
    QCOMPARE(m_textFolding->debugDump(),
             QStringLiteral("tree [10 p [15 pf 16] 20] [28  [30 pf [35  40] 40] 40] - folded [15 "
                            "pf 16] [30 pf 40]"));
  }

  // New range [19, 29] is not allowed.
  {
    auto id = insertNewFoldingRange(19, 29);
    QCOMPARE(id, TextFolding::InvalidRangeId);
    QCOMPARE(m_textFolding->debugDump(),
             QStringLiteral("tree [10 p [15 pf 16] 20] [28  [30 pf [35  40] 40] 40] - folded [15 "
                            "pf 16] [30 pf 40]"));
  }
}

void TestTextFolding::textFoldRange() {
  // New range [10, 20] (persistent, non-folded).
  {
    auto id = insertNewFoldingRange(10, 20, TextFolding::Persistent);
    QCOMPARE(m_textFolding->debugDump(), QStringLiteral("tree [10 p 20] - folded "));
    QVERIFY(checkTextBlocksVisible(m_doc, 0, m_doc->blockCount()));

    m_textFolding->toggleRange(id);
    QCOMPARE(m_textFolding->debugDump(), QStringLiteral("tree [10 pf 20] - folded [10 pf 20]"));
    QVERIFY(checkTextBlocksVisible(m_doc, 10, 10));
    QVERIFY(checkTextBlocksInvisible(m_doc, 11, 19));
    QVERIFY(checkTextBlocksVisible(m_doc, 20, 20));

    QCOMPARE(m_textFolding->lineToVisibleLine(10), 10);
    QCOMPARE(m_textFolding->lineToVisibleLine(11), 10);
    QCOMPARE(m_textFolding->lineToVisibleLine(19), 10);
    QCOMPARE(m_textFolding->lineToVisibleLine(20), 11);
    QCOMPARE(m_textFolding->lineToVisibleLine(21), 12);
    QCOMPARE(m_textFolding->visibleLineToLine(10), 10);
    QCOMPARE(m_textFolding->visibleLineToLine(11), 20);
    QCOMPARE(m_textFolding->visibleLineToLine(12), 21);

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
    QVERIFY(checkTextBlocksInvisible(m_doc, 31, 39));
    QVERIFY(checkTextBlocksVisible(m_doc, 40, 40));

    auto rangesAt10 = m_textFolding->foldingRangesStartingOnBlock(10);
    QCOMPARE(rangesAt10.size(), 1);
    m_textFolding->toggleRange(rangesAt10[0].first);

    QCOMPARE(m_textFolding->lineToVisibleLine(20), 11);
    QCOMPARE(m_textFolding->lineToVisibleLine(21), 12);
    QCOMPARE(m_textFolding->lineToVisibleLine(30), 21);
    QCOMPARE(m_textFolding->lineToVisibleLine(31), 21);
    QCOMPARE(m_textFolding->lineToVisibleLine(39), 21);
    QCOMPARE(m_textFolding->lineToVisibleLine(40), 22);
    QCOMPARE(m_textFolding->lineToVisibleLine(41), 23);
    QCOMPARE(m_textFolding->visibleLineToLine(11), 20);
    QCOMPARE(m_textFolding->visibleLineToLine(12), 21);
    QCOMPARE(m_textFolding->visibleLineToLine(21), 30);
    QCOMPARE(m_textFolding->visibleLineToLine(22), 40);
    QCOMPARE(m_textFolding->visibleLineToLine(23), 41);

    m_textFolding->toggleRange(rangesAt10[0].first);

    auto childId = insertNewFoldingRange(35, 40, TextFolding::Folded);
    QVERIFY(childId != TextFolding::InvalidRangeId);

    m_textFolding->toggleRange(id);
    QCOMPARE(m_textFolding->debugDump(),
             QStringLiteral("tree [10 p 20] [30 p [35 f 40] 40] - folded [35 f 40]"));
    QVERIFY(checkTextBlocksVisible(m_doc, 30, 35));
    QVERIFY(checkTextBlocksInvisible(m_doc, 36, 39));
    QVERIFY(checkTextBlocksVisible(m_doc, 40, 40));

    m_textFolding->toggleRange(childId);
    QCOMPARE(m_textFolding->debugDump(), QStringLiteral("tree [10 p 20] [30 p 40] - folded "));
    QVERIFY(checkTextBlocksVisible(m_doc, 0, m_doc->blockCount()));
  }

  // A two-line folded range retains folded state without hiding either endpoint.
  {
    auto id = insertNewFoldingRange(44, 45, TextFolding::Folded);
    QVERIFY(id != TextFolding::InvalidRangeId);
    auto rangesAt44 = m_textFolding->foldingRangesStartingOnBlock(44);
    QCOMPARE(rangesAt44.size(), 1);
    QVERIFY(rangesAt44[0].second.testFlag(TextFolding::Folded));
    QVERIFY(checkTextBlocksVisible(m_doc, 44, 45));
    QCOMPARE(m_textFolding->lineToVisibleLine(44), 44);
    QCOMPARE(m_textFolding->lineToVisibleLine(45), 45);
    QCOMPARE(m_textFolding->lineToVisibleLine(46), 46);
    QCOMPARE(m_textFolding->visibleLineToLine(44), 44);
    QCOMPARE(m_textFolding->visibleLineToLine(45), 45);
    QCOMPARE(m_textFolding->visibleLineToLine(46), 46);

    m_textFolding->toggleRange(id);
  }

  // A folded range may collapse to one block after an edit and must remain a zero-delta fold.
  {
    QTextDocument doc(QStringLiteral("0\n1\n2\n3\n4\n5\n6"));
    TextFolding folding(&doc);
    auto outerId =
        folding.newFoldingRange(TextBlockRange(doc.findBlockByNumber(0), doc.findBlockByNumber(6)),
                                TextFolding::Persistent | TextFolding::Folded);
    auto collapsedId =
        folding.newFoldingRange(TextBlockRange(doc.findBlockByNumber(1), doc.findBlockByNumber(2)),
                                TextFolding::Persistent | TextFolding::Folded);
    auto followingId =
        folding.newFoldingRange(TextBlockRange(doc.findBlockByNumber(3), doc.findBlockByNumber(5)),
                                TextFolding::Persistent | TextFolding::Folded);
    QVERIFY(outerId != TextFolding::InvalidRangeId);
    QVERIFY(collapsedId != TextFolding::InvalidRangeId);
    QVERIFY(followingId != TextFolding::InvalidRangeId);

    QTextCursor cursor(&doc);
    cursor.setPosition(doc.findBlockByNumber(1).position());
    cursor.setPosition(doc.findBlockByNumber(2).position(), QTextCursor::KeepAnchor);
    cursor.removeSelectedText();

    QCOMPARE(folding.m_idToFoldingRange.value(collapsedId)->first(), 1);
    QCOMPARE(folding.m_idToFoldingRange.value(collapsedId)->last(), 1);
    QCOMPARE(folding.m_idToFoldingRange.value(followingId)->first(), 2);
    QCOMPARE(folding.m_idToFoldingRange.value(followingId)->last(), 4);
    QCOMPARE(folding.debugDump(),
             QStringLiteral("tree [0 pf [1 pf 1] [2 pf 4] 5] - folded [0 pf 5]"));
    folding.toggleRange(outerId);
    QCOMPARE(folding.debugDump(),
             QStringLiteral("tree [0 p [1 pf 1] [2 pf 4] 5] - folded [1 pf 1] [2 pf 4]"));

    QVERIFY(checkTextBlocksVisible(&doc, 0, 2));
    QVERIFY(checkTextBlocksInvisible(&doc, 3, 3));
    QVERIFY(checkTextBlocksVisible(&doc, 4, 5));
    QCOMPARE(folding.lineToVisibleLine(1), 1);
    QCOMPARE(folding.lineToVisibleLine(2), 2);
    QCOMPARE(folding.lineToVisibleLine(3), 2);
    QCOMPARE(folding.lineToVisibleLine(4), 3);
    QCOMPARE(folding.lineToVisibleLine(5), 4);
    QCOMPARE(folding.visibleLineToLine(1), 1);
    QCOMPARE(folding.visibleLineToLine(2), 2);
    QCOMPARE(folding.visibleLineToLine(3), 4);
    QCOMPARE(folding.visibleLineToLine(4), 5);
  }

  // Nested folded range [32, 38] (folded).
  {
    auto id = insertNewFoldingRange(32, 38, TextFolding::Folded);
    QCOMPARE(m_textFolding->debugDump(),
             QStringLiteral("tree [10 p 20] [30 p [32 f 38] 40] - folded [32 f 38]"));
    QVERIFY(checkTextBlocksVisible(m_doc, 32, 32));
    QVERIFY(checkTextBlocksInvisible(m_doc, 33, 37));
    QVERIFY(checkTextBlocksVisible(m_doc, 38, 38));

    m_textFolding->toggleRange(id);
    QCOMPARE(m_textFolding->debugDump(), QStringLiteral("tree [10 p 20] [30 p 40] - folded "));
    QVERIFY(checkTextBlocksVisible(m_doc, 0, m_doc->blockCount()));
  }

  // A large folded range [8, 42] (persistent, folded).
  {
    auto id = insertNewFoldingRange(8, 42, TextFolding::Persistent | TextFolding::Folded);
    QCOMPARE(m_textFolding->debugDump(),
             QStringLiteral("tree [8 pf [10 p 20] [30 p 40] 42] - folded [8 pf 42]"));
    QVERIFY(checkTextBlocksVisible(m_doc, 8, 8));
    QVERIFY(checkTextBlocksInvisible(m_doc, 9, 41));
    QVERIFY(checkTextBlocksVisible(m_doc, 42, 42));

    auto subId = insertNewFoldingRange(22, 26, TextFolding::Persistent | TextFolding::Folded);
    QCOMPARE(m_textFolding->debugDump(),
             QStringLiteral("tree [8 pf [10 p 20] [22 pf 26] [30 p 40] 42] - folded [8 pf 42]"));
    QVERIFY(checkTextBlocksVisible(m_doc, 8, 8));
    QVERIFY(checkTextBlocksInvisible(m_doc, 9, 41));
    QVERIFY(checkTextBlocksVisible(m_doc, 42, 42));

    m_textFolding->toggleRange(id);
    QCOMPARE(m_textFolding->debugDump(),
             QStringLiteral("tree [8 p [10 p 20] [22 pf 26] [30 p 40] 42] - folded [22 pf 26]"));
    QVERIFY(checkTextBlocksVisible(m_doc, 8, 22));
    QVERIFY(checkTextBlocksInvisible(m_doc, 23, 25));
    QVERIFY(checkTextBlocksVisible(m_doc, 26, 42));

    m_textFolding->toggleRange(subId);
    QCOMPARE(m_textFolding->debugDump(),
             QStringLiteral("tree [8 p [10 p 20] [22 p 26] [30 p 40] 42] - folded "));
    QVERIFY(checkTextBlocksVisible(m_doc, 8, 42));
  }
}

void TestTextFolding::testRemoveFoldingRange() {
  // 1. Create persistent range, remove by ID, verify gone.
  {
    auto id = insertNewFoldingRange(10, 20, TextFolding::Persistent);
    QVERIFY(id != TextFolding::InvalidRangeId);
    QVERIFY(m_textFolding->m_idToFoldingRange.contains(id));

    QVERIFY(m_textFolding->removeFoldingRange(id));
    QVERIFY(!m_textFolding->m_idToFoldingRange.contains(id));
    QVERIFY(m_textFolding->m_foldingRanges.isEmpty());
  }

  // 2. Create persistent folded range, remove, verify blocks visible again.
  {
    auto id = insertNewFoldingRange(30, 40, TextFolding::Persistent | TextFolding::Folded);
    QVERIFY(id != TextFolding::InvalidRangeId);
    QVERIFY(checkTextBlocksInvisible(m_doc, 31, 39));
    QVERIFY(checkTextBlocksVisible(m_doc, 40, 40));

    QVERIFY(m_textFolding->removeFoldingRange(id));
    QVERIFY(checkTextBlocksVisible(m_doc, 30, 40));
    QVERIFY(!m_textFolding->m_idToFoldingRange.contains(id));
  }

  // 3. Remove non-existent ID returns false.
  {
    QVERIFY(!m_textFolding->removeFoldingRange(9999));
  }

  // 4. Nested ranges: remove parent, children reparented to grandparent.
  {
    auto parentId = insertNewFoldingRange(10, 40, TextFolding::Persistent);
    auto childId = insertNewFoldingRange(15, 20, TextFolding::Persistent);
    QVERIFY(parentId != TextFolding::InvalidRangeId);
    QVERIFY(childId != TextFolding::InvalidRangeId);

    // Child is nested under parent.
    auto childRange = m_textFolding->m_idToFoldingRange.value(childId);
    QVERIFY(childRange);
    QVERIFY(childRange->m_parent != nullptr);

    QVERIFY(m_textFolding->removeFoldingRange(parentId));
    QVERIFY(!m_textFolding->m_idToFoldingRange.contains(parentId));
    // Child still exists and reparented to root (null parent).
    QVERIFY(m_textFolding->m_idToFoldingRange.contains(childId));
    childRange = m_textFolding->m_idToFoldingRange.value(childId);
    QVERIFY(childRange->m_parent == nullptr);
  }

  // 5. Fold then remove: verify unfolded and removed.
  {
    auto id = insertNewFoldingRange(10, 20, TextFolding::Persistent);
    QVERIFY(id != TextFolding::InvalidRangeId);
    m_textFolding->toggleRange(id);
    QVERIFY(checkTextBlocksInvisible(m_doc, 11, 19));
    QVERIFY(checkTextBlocksVisible(m_doc, 20, 20));

    QVERIFY(m_textFolding->removeFoldingRange(id));
    QVERIFY(checkTextBlocksVisible(m_doc, 10, 20));
    QVERIFY(!m_textFolding->m_idToFoldingRange.contains(id));
  }
}

void TestTextFolding::testDocumentReplacement() {
  // Add folding ranges.
  auto id1 = insertNewFoldingRange(10, 20, TextFolding::Persistent);
  QVERIFY(id1 != TextFolding::InvalidRangeId);
  auto id2 = insertNewFoldingRange(30, 40, TextFolding::Persistent | TextFolding::Folded);
  QVERIFY(id2 != TextFolding::InvalidRangeId);
  QVERIFY(!m_textFolding->isEmpty());

  // Replace document content (simulates setPlainText).
  // This should trigger hardClear via contentsChange detection.
  m_doc->setPlainText(utils::getCppText());

  // All folding ranges should be cleared without crash.
  QVERIFY(m_textFolding->isEmpty());
  QVERIFY(m_textFolding->m_foldingRanges.isEmpty());
  QVERIFY(m_textFolding->m_foldedFoldingRanges.isEmpty());
  QVERIFY(m_textFolding->m_idToFoldingRange.isEmpty());

  // All blocks should be visible.
  QVERIFY(checkTextBlocksVisible(m_doc, 0, m_doc->blockCount() - 1));

  // New folding ranges can be created on the new document.
  auto id3 = insertNewFoldingRange(5, 15, TextFolding::Persistent);
  QVERIFY(id3 != TextFolding::InvalidRangeId);
  QVERIFY(!m_textFolding->isEmpty());
}

void TestTextFolding::testDocumentClear() {
  auto id1 = insertNewFoldingRange(10, 20, TextFolding::Persistent | TextFolding::Folded);
  QVERIFY(id1 != TextFolding::InvalidRangeId);
  QVERIFY(checkTextBlocksInvisible(m_doc, 11, 19));
  QVERIFY(checkTextBlocksVisible(m_doc, 20, 20));

  // Clear the document entirely.
  m_doc->clear();

  // Folding ranges should be hard-cleared without crash.
  QVERIFY(m_textFolding->isEmpty());
  QVERIFY(m_textFolding->m_foldingRanges.isEmpty());
  QVERIFY(m_textFolding->m_foldedFoldingRanges.isEmpty());
  QVERIFY(m_textFolding->m_idToFoldingRange.isEmpty());
}

void TestTextFolding::testRangeAccessors() {
  // testDocumentClear() leaves the document empty, and slots run in
  // declaration order, so refill it before asking for any range.
  m_doc->setPlainText(utils::getCppText());

  // 1. foldingRangeBlocks() reports the live extent of a live range.
  {
    auto id = insertNewFoldingRange(10, 20, TextFolding::Persistent);
    QVERIFY(id != TextFolding::InvalidRangeId);

    int first = -1;
    int last = -1;
    QVERIFY(m_textFolding->foldingRangeBlocks(id, &first, &last));
    QCOMPARE(first, 10);
    QCOMPARE(last, 20);

    // An unknown id answers false and leaves the outputs alone.
    int untouched = -7;
    QVERIFY(!m_textFolding->foldingRangeBlocks(9999, &untouched, &untouched));
    QCOMPARE(untouched, -7);

    // Null outputs are accepted: the answer alone is often enough.
    QVERIFY(m_textFolding->foldingRangeBlocks(id, nullptr, nullptr));

    // 2. isRangeFolded() before and after folding.
    QVERIFY(!m_textFolding->isRangeFolded(id));
    QVERIFY(!m_textFolding->isRangeFolded(9999));

    // 3. foldRange() folds once and is a no-op the second time. Unlike
    // toggleRange(), a second call must not unfold it.
    QVERIFY(m_textFolding->foldRange(id));
    QVERIFY(m_textFolding->isRangeFolded(id));
    QVERIFY(checkTextBlocksInvisible(m_doc, 11, 19));
    QVERIFY(checkTextBlocksVisible(m_doc, 10, 10));
    QVERIFY(checkTextBlocksVisible(m_doc, 20, 20));

    QVERIFY(m_textFolding->foldRange(id));
    QVERIFY(m_textFolding->isRangeFolded(id));
    QVERIFY(checkTextBlocksInvisible(m_doc, 11, 19));

    QVERIFY(!m_textFolding->foldRange(9999));

    // 4. The extent still resolves while folded.
    QVERIFY(m_textFolding->foldingRangeBlocks(id, &first, &last));
    QCOMPARE(first, 10);
    QCOMPARE(last, 20);

    // 5. A stale id: the range is gone once it is removed.
    QVERIFY(m_textFolding->removeFoldingRange(id));
    QVERIFY(!m_textFolding->foldingRangeBlocks(id, &first, &last));
    QVERIFY(!m_textFolding->isRangeFolded(id));
    QVERIFY(!m_textFolding->foldRange(id));
  }

  // 6. isEnabled() mirrors setEnabled(), which clears every range.
  {
    QVERIFY(m_textFolding->isEnabled());

    auto id = insertNewFoldingRange(10, 20, TextFolding::Persistent);
    QVERIFY(id != TextFolding::InvalidRangeId);

    m_textFolding->setEnabled(false);
    QVERIFY(!m_textFolding->isEnabled());
    QVERIFY(!m_textFolding->foldingRangeBlocks(id, nullptr, nullptr));

    m_textFolding->setEnabled(true);
    QVERIFY(m_textFolding->isEnabled());
  }
}

// hardClear() drops every range without unfolding it, because after a real
// document replacement the blocks a range spanned are gone and must not be
// touched. The replacement heuristic in the contentsChange handler also fires
// on a full-document *format* change, though - the editor produces one from
// QSyntaxHighlighter::rehighlight(), which VTextEditor::setConfig() runs on
// every configuration or theme change - and there the blocks are the very same
// objects. Leaving them hidden would hide the folded source for good, with no
// range left for the gutter to unfold it. The end-to-end case is covered by
// test_interactivepreview's testFoldStateSurvivesAWidgetRebuild.
void TestTextFolding::testHardClearRestoresVisibility() {
  m_doc->setPlainText(utils::getCppText());

  auto id = insertNewFoldingRange(10, 20, TextFolding::Persistent | TextFolding::Folded);
  QVERIFY(id != TextFolding::InvalidRangeId);
  QVERIFY(checkTextBlocksInvisible(m_doc, 11, 19));

  const QString before = m_doc->toPlainText();

  m_textFolding->hardClear();

  // No text changed, and every range is gone.
  QCOMPARE(m_doc->toPlainText(), before);
  QVERIFY(m_textFolding->isEmpty());
  QVERIFY(m_textFolding->m_idToFoldingRange.isEmpty());
  QVERIFY(m_textFolding->m_foldedFoldingRanges.isEmpty());
  QVERIFY2(checkTextBlocksVisible(m_doc, 0, m_doc->blockCount() - 1),
           "the hard clear left folded blocks hidden with no range to unfold them");
}

// The folded-line background is an extra selection: a collapsed QTextCursor at
// the folded range's first block. A destructive in-place replacement starting
// exactly at that position drags the applied cursor past the inserted text
// (QTextCursorPrivate::adjustPosition with MoveCursor), so what is on screen
// points at the wrong block until something re-applies the list. TextFolding
// does rebuild the list when the range is dropped and recreated, but only
// behind ExtraSelectionMgr's 200ms coalescing timer. A caller which restores
// the fold synchronously inside the edit's turn must therefore be able to flush
// the manager without waiting - that is
// ExtraSelectionMgr::applyExtraSelections(), reached in production through
// VTextEditor::applyPendingExtraSelections(). This asserts the flushed list is
// correct with no QTest::qWait().
void TestTextFolding::testFoldedLineSelectionSurvivesInPlaceReplacement() {
  QTextDocument doc(QStringLiteral("0\n1\n2\n3\n4\n5\n6"));
  TextFolding folding(&doc);

  RecordingExtraSelectionInterface interface;
  ExtraSelectionMgr mgr(&interface);
  // Registers the folded-line extra selection type and installs the rebuild
  // lambda on foldingRangesChanged. One-shot per TextFolding instance.
  folding.setExtraSelectionMgr(&mgr);

  auto id =
      folding.newFoldingRange(TextBlockRange(doc.findBlockByNumber(0), doc.findBlockByNumber(4)),
                              TextFolding::Persistent | TextFolding::Folded);
  QVERIFY(id != TextFolding::InvalidRangeId);

  mgr.applyExtraSelections();
  QCOMPARE(interface.m_selections.size(), 1);
  QCOMPARE(interface.m_selections.first().cursor.blockNumber(), 0);
  QVERIFY(!interface.m_selections.first().cursor.hasSelection());

  // Keep the applied cursor alive to observe what Qt does to it.
  QTextCursor appliedCursor = interface.m_selections.first().cursor;

  // The in-place replacement performed by InteractivePreviewHost::applyReplacement().
  {
    const auto lastBlock = doc.findBlockByNumber(4);
    QTextCursor cursor(&doc);
    cursor.setPosition(doc.findBlockByNumber(0).position());
    cursor.setPosition(lastBlock.position() + lastBlock.length() - 1, QTextCursor::KeepAnchor);
    cursor.insertText(QStringLiteral("A\nB\nC"));
  }

  // The motivating Qt behaviour: the already-applied cursor no longer points
  // at the folded range's first block.
  QVERIFY2(appliedCursor.blockNumber() != 0,
           "expected the applied folded-line cursor to be dragged past the replacement");

  // The range spanned blocks which the replacement destroyed, so
  // checkAndUpdateFoldings() dropped it. This is what restoreFoldedRange()
  // does afterwards.
  QVERIFY(!folding.foldingRangeBlocks(id, nullptr, nullptr));
  auto restoredId =
      folding.newFoldingRange(TextBlockRange(doc.findBlockByNumber(0), doc.findBlockByNumber(2)),
                              TextFolding::Persistent | TextFolding::Folded);
  QVERIFY(restoredId != TextFolding::InvalidRangeId);

  // The flush, with no wait for the coalescing timer.
  mgr.applyExtraSelections();

  QCOMPARE(interface.m_selections.size(), 1);
  const auto &selection = interface.m_selections.first();
  QCOMPARE(selection.cursor.document(), &doc);
  // Collapsed, not merely on the right block: the band in
  // TextDocumentLayout::formatRangeFromSelection() is only resolved from
  // cursor.position() when hasSelection() is false.
  QVERIFY2(!selection.cursor.hasSelection(),
           "the folded-line selection must stay collapsed to paint as a full-width band");
  QCOMPARE(selection.cursor.blockNumber(), 0);
}

// The two queries backing the fold/unfold-at-cursor commands.
void TestTextFolding::testDeepestFoldableRangeOnBlock() {
  m_doc->setPlainText(utils::getCppText());

  // Nested: outer [10, 30] contains inner [15, 25].
  auto outerId = insertNewFoldingRange(10, 30, TextFolding::Persistent);
  QVERIFY(outerId != TextFolding::InvalidRangeId);
  auto innerId = insertNewFoldingRange(15, 25, TextFolding::Persistent);
  QVERIFY(innerId != TextFolding::InvalidRangeId);

  // Nothing folded: the deepest containing range wins.
  QCOMPARE(m_textFolding->deepestFoldableRangeOnBlock(20), innerId);
  // A block only inside the outer range.
  QCOMPARE(m_textFolding->deepestFoldableRangeOnBlock(12), outerId);
  // Outside of every range.
  QCOMPARE(m_textFolding->deepestFoldableRangeOnBlock(5), (qint64)TextFolding::InvalidRangeId);

  // Inner folded, outer not: the cursor sits on the inner range's still
  // visible first block, and the outer range is what can still be folded.
  QVERIFY(m_textFolding->foldRange(innerId));
  QCOMPARE(m_textFolding->deepestFoldableRangeOnBlock(15), outerId);

  // Outer folded too: nothing left to fold on that block.
  QVERIFY(m_textFolding->foldRange(outerId));
  QCOMPARE(m_textFolding->deepestFoldableRangeOnBlock(15), (qint64)TextFolding::InvalidRangeId);
}

void TestTextFolding::testOutermostFoldedRangeOnBlock() {
  m_doc->setPlainText(utils::getCppText());

  auto outerId = insertNewFoldingRange(10, 30, TextFolding::Persistent);
  QVERIFY(outerId != TextFolding::InvalidRangeId);
  auto innerId = insertNewFoldingRange(15, 25, TextFolding::Persistent);
  QVERIFY(innerId != TextFolding::InvalidRangeId);

  // Nothing folded.
  QCOMPARE(m_textFolding->outermostFoldedRangeOnBlock(20), (qint64)TextFolding::InvalidRangeId);
  QCOMPARE(m_textFolding->outermostFoldedRangeOnBlock(5), (qint64)TextFolding::InvalidRangeId);

  // Only the inner one is folded.
  QVERIFY(m_textFolding->foldRange(innerId));
  QCOMPARE(m_textFolding->outermostFoldedRangeOnBlock(15), innerId);
  QCOMPARE(m_textFolding->outermostFoldedRangeOnBlock(12), (qint64)TextFolding::InvalidRangeId);

  // Both folded: the outermost one is reported, which is the one a single
  // unfold command has to open.
  QVERIFY(m_textFolding->foldRange(outerId));
  QCOMPARE(m_textFolding->outermostFoldedRangeOnBlock(15), outerId);
}

// Disabling must be checked *before* creating the ranges: setEnabled(false)
// calls clear(), so disabling afterwards would pass even without the m_enabled
// guard. newFoldingRange() itself has no enabled guard.
void TestTextFolding::testFoldQueriesWhenDisabled() {
  m_doc->setPlainText(utils::getCppText());

  m_textFolding->setEnabled(false);

  // An unfolded range and a folded one, so that each query is answered by its
  // m_enabled guard rather than by the ranges' state: without the guard the
  // unfolded range would be reported by deepestFoldableRangeOnBlock() and the
  // folded one by outermostFoldedRangeOnBlock().
  auto unfoldedId = insertNewFoldingRange(10, 30, TextFolding::Persistent);
  QVERIFY(unfoldedId != TextFolding::InvalidRangeId);
  auto foldedId = insertNewFoldingRange(40, 60, TextFolding::Persistent | TextFolding::Folded);
  QVERIFY(foldedId != TextFolding::InvalidRangeId);

  QCOMPARE(m_textFolding->deepestFoldableRangeOnBlock(20), (qint64)TextFolding::InvalidRangeId);
  QCOMPARE(m_textFolding->outermostFoldedRangeOnBlock(20), (qint64)TextFolding::InvalidRangeId);
  QCOMPARE(m_textFolding->deepestFoldableRangeOnBlock(50), (qint64)TextFolding::InvalidRangeId);
  QCOMPARE(m_textFolding->outermostFoldedRangeOnBlock(50), (qint64)TextFolding::InvalidRangeId);

  m_textFolding->setEnabled(true);
}

QTEST_MAIN(tests::TestTextFolding)