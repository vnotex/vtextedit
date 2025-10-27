#ifndef TEXTFOLDING_H
#define TEXTFOLDING_H

#include <QFlags>
#include <QHash>
#include <QObject>
#include <QSharedPointer>
#include <QVector>

#include <vtextedit/textrange.h>

class QTextBlock;
class QTextDocument;

namespace tests {
class TestTextFolding;
}

namespace vte {
class ExtraSelectionMgr;

// From KDE's text folding implementation.
class TextFolding : public QObject {
  Q_OBJECT
public:
  friend class tests::TestTextFolding;

  enum FoldingRangeFlag {
    // Range is persistent.
    // Highlighting won't add folddings by default. It will create a
    // temporary folding once user click to fold it.
    Persistent = 0x1,

    // Range is folded.
    Folded = 0x2
  };
  Q_DECLARE_FLAGS(FoldingRangeFlags, FoldingRangeFlag)

  enum { InvalidRangeId = -1 };

  explicit TextFolding(QTextDocument *p_document);

  ~TextFolding();

  bool hasFoldedFolding() const;

  qint64 newFoldingRange(const TextBlockRange &p_range, FoldingRangeFlags p_flags);

  QVector<QPair<qint64, TextFolding::FoldingRangeFlags>>
  foldingRangesStartingOnBlock(int p_blockNumber) const;

  QSharedPointer<QPair<qint64, TextBlockRange>> leafFoldingRangeOnBlock(int p_blockNumber) const;

  bool toggleRange(qint64 p_id);

  QString debugDump() const;

  void setExtraSelectionMgr(ExtraSelectionMgr *p_mgr);

  int lineToVisibleLine(int p_line) const;

  int visibleLineToLine(int p_line) const;

  void setEnabled(bool p_enable);

  void setFoldedFoldingRangeLineBackgroundColor(const QColor &p_color);

public slots:
  void clear();

  void checkAndUpdateFoldings();

signals:
  void foldingRangesChanged();

private:
  class FoldingRange {
  public:
    FoldingRange(const TextBlockRange &p_range, FoldingRangeFlags p_flags);

    ~FoldingRange();

    FoldingRange(const FoldingRange &) = delete;
    FoldingRange &operator=(const FoldingRange &) = delete;

    int first() const;

    int last() const;

    bool contains(const FoldingRange *p_range) const;

    bool contains(int p_blockNumber) const;

    bool before(const FoldingRange *p_range) const;

    bool isFolded() const;

    bool isValid() const;

    QString toString() const;

    typedef QVector<FoldingRange *> Vector;

    TextBlockRange m_range;

    FoldingRange *m_parent = nullptr;

    // Direct nested children of current folding range.
    // Sorted and non-overlapping.
    FoldingRange::Vector m_nestedRanges;

    FoldingRangeFlags m_flags;

    // Id of this range.
    qint64 m_id = InvalidRangeId;
  };

  bool insertNewFoldingRange(FoldingRange *p_parent, FoldingRange::Vector &p_ranges,
                             FoldingRange *p_newRange);

  void updateFoldedRangesForNewRange(TextFolding::FoldingRange *p_newRange);

  void updateFoldedRangesForRemovedRange(TextFolding::FoldingRange *p_oldRange);

  void foldingRangesStartingOnBlock(const TextFolding::FoldingRange::Vector &p_ranges,
                                    int p_blockNumber,
                                    QVector<QPair<qint64, FoldingRangeFlags>> &p_results) const;

  void setRangeFolded(const TextBlockRange &p_range, bool p_folded) const;

  void foldRange(FoldingRange *p_range);

  // Return true if @p_range is removed.
  // Caller should remove the range from id mappings.
  bool unfoldRange(FoldingRange *p_range, bool p_remove = false);

  QSharedPointer<QPair<qint64, TextBlockRange>>
  leafFoldingRangeOnBlock(const TextFolding::FoldingRange::Vector &p_ranges,
                          int p_blockNumber) const;

  void markDocumentContentsDirty(int p_position = 0, int p_length = INT_MAX) const;

  void markDocumentContentsDirty(const TextBlockRange &p_range);

  FoldingRange::Vector
  retrieveFoldedRanges(const TextFolding::FoldingRange::Vector &p_ranges) const;

  void unfoldRangeWithNestedFoldedRanges(
      const TextBlockRange &p_range,
      const TextFolding::FoldingRange::Vector &p_foldedChildren) const;

  bool checkAndUpdateFoldings(TextFolding::FoldingRange::Vector &p_ranges);

  QString debugDump(const TextFolding::FoldingRange::Vector &p_ranges, bool p_recursive) const;

  static bool compareRangeByStart(const FoldingRange *p_a, const FoldingRange *p_b);

  static bool compareRangeByEnd(const FoldingRange *p_a, const FoldingRange *p_b);

  static bool compareRangeByStartBeforeBlock(const FoldingRange *p_range, int p_blockNumber);

  static bool compareRangeByStartAfterBlock(int p_blockNumber, const FoldingRange *p_range);

  QTextDocument *m_document = nullptr;

  bool m_enabled = true;

  // Used to highlight folded ranges via extra selection.
  ExtraSelectionMgr *m_extraSelectionMgr = nullptr;

  int m_extraSelectionType = 0;

  // Background of extra selection of folded folding range.
  QColor m_foldedFoldingRangeLineBackground = "#befbdd";

  // Sorted and non-overlapping.
  FoldingRange::Vector m_foldingRanges;

  // Folded folding ranges.
  // Sorted and non-overlapping.
  // Subset of m_foldingRanges.
  // Flat: if the parent is folded, then all its children will be removed from
  // this.
  FoldingRange::Vector m_foldedFoldingRanges;

  qint64 m_nextId = 0;

  QHash<qint64, FoldingRange *> m_idToFoldingRange;
};
} // namespace vte

Q_DECLARE_OPERATORS_FOR_FLAGS(vte::TextFolding::FoldingRangeFlags)

#endif // TEXTFOLDING_H
