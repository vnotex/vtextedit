#include "textfolding.h"

#include <QDebug>
#include <QTextDocument>

#include "extraselectionmgr.h"

using namespace vte;

#define TFDebug 0

TextFolding::FoldingRange::FoldingRange(const TextBlockRange &p_range, FoldingRangeFlags p_flags)
    : m_range(p_range), m_flags(p_flags) {
  Q_ASSERT(p_range.isValid());
}

TextFolding::FoldingRange::~FoldingRange() { qDeleteAll(m_nestedRanges); }

int TextFolding::FoldingRange::first() const { return m_range.first().blockNumber(); }

int TextFolding::FoldingRange::last() const { return m_range.last().blockNumber(); }

bool TextFolding::FoldingRange::contains(const FoldingRange *p_range) const {
  return first() <= p_range->first() && last() >= p_range->last();
}

bool TextFolding::FoldingRange::contains(int p_blockNumber) const {
  return m_range.contains(p_blockNumber);
}

bool TextFolding::FoldingRange::before(const FoldingRange *p_range) const {
  return last() <= p_range->first();
}

bool TextFolding::FoldingRange::isFolded() const {
  return m_flags & TextFolding::FoldingRangeFlag::Folded;
}

bool TextFolding::FoldingRange::isValid() const { return m_range.isValid(); }

QString TextFolding::FoldingRange::toString() const {
  return QStringLiteral("range [%1, %2]").arg(first()).arg(last());
}

TextFolding::TextFolding(QTextDocument *p_document) : QObject(p_document), m_document(p_document) {
  connect(m_document, &QTextDocument::contentsChange, this,
          [this](int p_position, int p_charsRemoved, int p_charsAdded) {
            Q_UNUSED(p_position);
            if (p_charsRemoved > 0 || p_charsAdded > 0) {
              checkAndUpdateFoldings();
            }
          });
}

TextFolding::~TextFolding() { qDeleteAll(m_foldingRanges); }

void TextFolding::clear() {
  m_nextId = 0;

  if (m_foldingRanges.isEmpty()) {
    Q_ASSERT(m_idToFoldingRange.isEmpty());
    Q_ASSERT(m_foldedFoldingRanges.isEmpty());
    return;
  }

  // Unfold any range.
  while (!m_foldedFoldingRanges.isEmpty()) {
    unfoldRange(m_foldedFoldingRanges.first(), false);
  }

  m_idToFoldingRange.clear();
  qDeleteAll(m_foldingRanges);
  m_foldingRanges.clear();

  markDocumentContentsDirty();
  emit foldingRangesChanged();
}

bool TextFolding::hasFoldedFolding() const { return !m_foldedFoldingRanges.isEmpty(); }

qint64 TextFolding::newFoldingRange(const TextBlockRange &p_range, FoldingRangeFlags p_flags) {
#if TFDebug
  qDebug() << "newFoldingRange" << p_range.toString() << p_flags;
#endif

  if (p_range.size() < 2) {
    qWarning() << "invalid block range to add a folding" << p_range.toString() << p_flags;
    return InvalidRangeId;
  }

  auto newRange = new FoldingRange(p_range, p_flags);
  if (!insertNewFoldingRange(nullptr, m_foldingRanges, newRange)) {
    delete newRange;
    return InvalidRangeId;
  }

  newRange->m_id = m_nextId++;
  // In case of wrapping.
  if (newRange->m_id < 0) {
    newRange->m_id = 0;
    m_nextId = 1;
  }

  m_idToFoldingRange.insert(newRange->m_id, newRange);

  if (newRange->isFolded()) {
    updateFoldedRangesForNewRange(newRange);
    markDocumentContentsDirty(newRange->m_range);
  }

  emit foldingRangesChanged();

  return newRange->m_id;
}

bool TextFolding::insertNewFoldingRange(FoldingRange *p_parent, FoldingRange::Vector &p_ranges,
                                        FoldingRange *p_newRange) {
  // @p_ranges are sorted and non-overlapping.
  auto lowerBound =
      std::lower_bound(p_ranges.begin(), p_ranges.end(), p_newRange, compareRangeByStart);
  auto upperBound =
      std::upper_bound(p_ranges.begin(), p_ranges.end(), p_newRange, compareRangeByEnd);

#if TFDebug
  qDebug() << "insertNewFoldingRange1" << p_newRange->toString()
           << (lowerBound == p_ranges.end() ? "END" : (*lowerBound)->toString())
           << (upperBound == p_ranges.end() ? "END" : (*upperBound)->toString());
#endif

  // One step left since we may overlap with the one before the lower bound.
  // If lowerbound is the end(), that also works.
  if ((lowerBound != p_ranges.begin()) && ((*(lowerBound - 1))->last() >= p_newRange->first())) {
    --lowerBound;
  }

#if TFDebug
  qDebug() << "insertNewFoldingRange2" << p_newRange->toString()
           << (lowerBound == p_ranges.end() ? "END" : (*lowerBound)->toString())
           << (upperBound == p_ranges.end() ? "END" : (*upperBound)->toString());
#endif

  if (lowerBound == upperBound) {
    // Overlap with nothing.
    if ((lowerBound == p_ranges.end()) || (p_newRange->last() < (*lowerBound)->first())) {
      p_ranges.insert(lowerBound, p_newRange);
      p_newRange->m_parent = p_parent;
      return true;
    }

    Q_ASSERT(p_newRange->first() < (*lowerBound)->last());

    // Contained within this one range.
    // Exclude new range starting from the same line.
    if ((*lowerBound)->contains(p_newRange) && (*lowerBound)->first() != p_newRange->first()) {
      return insertNewFoldingRange(*lowerBound, (*lowerBound)->m_nestedRanges, p_newRange);
    }

    // Not well nested.
    return false;
  }

  // Check if new range is contained in the lowerbound.
  if ((*lowerBound)->contains(p_newRange)) {
    if ((*lowerBound)->first() < p_newRange->first()) {
      return insertNewFoldingRange(*lowerBound, (*lowerBound)->m_nestedRanges, p_newRange);
    } else {
      return false;
    }
  }

  auto it = lowerBound;
  FoldingRange::Vector nestedRanges;
  // How could an upper bound be nested in the new range?
  while (it != p_ranges.end()) {
    if (it == upperBound) {
      if (p_newRange->last() < (*upperBound)->first()) {
        break;
      } else {
        return false;
      }
    }

    if (!p_newRange->contains(*it) || p_newRange->first() == (*it)->first()) {
      return false;
    }

    nestedRanges.push_back(*it);
    ++it;
  }

  it = p_ranges.erase(lowerBound, upperBound);
  p_ranges.insert(it, p_newRange);
  p_newRange->m_nestedRanges = nestedRanges;

  p_newRange->m_parent = p_parent;
  for (auto range : qAsConst(p_newRange->m_nestedRanges)) {
    range->m_parent = p_newRange;
  }

  return true;
}

bool TextFolding::compareRangeByStart(const FoldingRange *p_a, const FoldingRange *p_b) {
  return p_a->first() < p_b->first();
}

bool TextFolding::compareRangeByEnd(const FoldingRange *p_a, const FoldingRange *p_b) {
  return p_a->last() < p_b->last();
}

bool TextFolding::compareRangeByStartBeforeBlock(const FoldingRange *p_range, int p_blockNumber) {
  return p_range->first() < p_blockNumber;
}

bool TextFolding::compareRangeByStartAfterBlock(int p_blockNumber, const FoldingRange *p_range) {
  return p_blockNumber < p_range->first();
}

void TextFolding::updateFoldedRangesForNewRange(TextFolding::FoldingRange *p_newRange) {
  Q_ASSERT(p_newRange->isFolded());

  // If any of its parents is folded, skip it.
  auto parent = p_newRange->m_parent;
  while (parent) {
    if (parent->isFolded()) {
      return;
    }

    parent = parent->m_parent;
  }

  FoldingRange::Vector foldedFoldingRanges;
  foldedFoldingRanges.reserve(m_foldedFoldingRanges.size() + 1);
  bool inserted = false;
  for (auto range : qAsConst(m_foldedFoldingRanges)) {
    if (!inserted) {
      if (p_newRange->contains(range)) {
        // Range is contained in p_newRange. Remove it.
        continue;
      }

      if (p_newRange->before(range)) {
        foldedFoldingRanges.push_back(p_newRange);
        inserted = true;
      }
    } else {
      Q_ASSERT(!p_newRange->contains(range));
    }

    foldedFoldingRanges.push_back(range);
  }

  if (!inserted) {
    foldedFoldingRanges.push_back(p_newRange);
  }

  m_foldedFoldingRanges = foldedFoldingRanges;

  setRangeFolded(p_newRange->m_range, true);
}

void TextFolding::updateFoldedRangesForRemovedRange(TextFolding::FoldingRange *p_oldRange) {
  Q_ASSERT(!p_oldRange->isFolded());

  // If any of its parents is folded, skip it.
  auto parent = p_oldRange->m_parent;
  while (parent) {
    if (parent->isFolded()) {
      return;
    }

    parent = parent->m_parent;
  }

  FoldingRange::Vector foldedFoldingRanges;
  foldedFoldingRanges.reserve(m_foldedFoldingRanges.size());
  for (auto range : qAsConst(m_foldedFoldingRanges)) {
    if (range == p_oldRange) {
      // Get folded child ranges.
      auto foldedRanges = retrieveFoldedRanges(p_oldRange->m_nestedRanges);
      foldedFoldingRanges.append(foldedRanges);
      // Unfold the old range while keep its folded child ranges folded.
      unfoldRangeWithNestedFoldedRanges(p_oldRange->m_range, foldedRanges);
    } else {
      foldedFoldingRanges.push_back(range);
    }
  }

  m_foldedFoldingRanges = foldedFoldingRanges;
}

QVector<QPair<qint64, TextFolding::FoldingRangeFlags>>
TextFolding::foldingRangesStartingOnBlock(int p_blockNumber) const {
  QVector<QPair<qint64, FoldingRangeFlags>> results;

  foldingRangesStartingOnBlock(m_foldingRanges, p_blockNumber, results);

  return results;
}

void TextFolding::foldingRangesStartingOnBlock(
    const TextFolding::FoldingRange::Vector &p_ranges, int p_blockNumber,
    QVector<QPair<qint64, FoldingRangeFlags>> &p_results) const {
  if (p_ranges.isEmpty()) {
    return;
  }

  auto lowerBound = std::lower_bound(p_ranges.begin(), p_ranges.end(), p_blockNumber,
                                     compareRangeByStartBeforeBlock);

  auto upperBound = std::upper_bound(p_ranges.begin(), p_ranges.end(), p_blockNumber,
                                     compareRangeByStartAfterBlock);

  if (lowerBound != p_ranges.begin() && (*(lowerBound - 1))->last() >= p_blockNumber) {
    --lowerBound;
  }

  for (auto it = lowerBound; it != upperBound; ++it) {
    if ((*it)->first() == p_blockNumber) {
      p_results.push_back(qMakePair((*it)->m_id, (*it)->m_flags));
    }

    foldingRangesStartingOnBlock((*it)->m_nestedRanges, p_blockNumber, p_results);
  }
}

void TextFolding::setRangeFolded(const TextBlockRange &p_range, bool p_folded) const {
  if (!p_range.isValid()) {
    return;
  }

  auto blockIt = p_range.first();
  blockIt.setVisible(true);

  int lastBlockNumber = p_range.last().blockNumber();

  blockIt = blockIt.next();
  while (blockIt.isValid()) {
    blockIt.setVisible(!p_folded);

    if (blockIt.blockNumber() == lastBlockNumber) {
      break;
    }

    blockIt = blockIt.next();
  }
}

QSharedPointer<QPair<qint64, TextBlockRange>>
TextFolding::leafFoldingRangeOnBlock(int p_blockNumber) const {
  return leafFoldingRangeOnBlock(m_foldingRanges, p_blockNumber);
}

QSharedPointer<QPair<qint64, TextBlockRange>>
TextFolding::leafFoldingRangeOnBlock(const TextFolding::FoldingRange::Vector &p_ranges,
                                     int p_blockNumber) const {
  if (p_ranges.isEmpty()) {
    return nullptr;
  }

  auto lowerBound = std::lower_bound(p_ranges.begin(), p_ranges.end(), p_blockNumber,
                                     compareRangeByStartBeforeBlock);

  auto upperBound = std::upper_bound(p_ranges.begin(), p_ranges.end(), p_blockNumber,
                                     compareRangeByStartAfterBlock);

  if (lowerBound != p_ranges.begin() && (*(lowerBound - 1))->last() >= p_blockNumber) {
    --lowerBound;
  }

  for (auto it = lowerBound; it != upperBound; ++it) {
    if ((*it)->contains(p_blockNumber)) {
      auto range = leafFoldingRangeOnBlock((*it)->m_nestedRanges, p_blockNumber);
      if (range) {
        return range;
      } else {
        auto pair = qMakePair((*it)->m_id, (*it)->m_range);
        return QSharedPointer<QPair<qint64, TextBlockRange>>::create(pair);
      }
    }
  }

  return nullptr;
}

bool TextFolding::toggleRange(qint64 p_id) {
  auto range = m_idToFoldingRange.value(p_id);
  if (!range) {
    return false;
  }

  if (range->isFolded()) {
    if (unfoldRange(range, false)) {
      // Remove the range from id mapping.
      m_idToFoldingRange.remove(p_id);
    }
  } else {
    foldRange(range);
  }

  return true;
}

void TextFolding::foldRange(FoldingRange *p_range) {
  Q_ASSERT(p_range);
  if (p_range->isFolded()) {
    return;
  }

  p_range->m_flags |= TextFolding::FoldingRangeFlag::Folded;
  updateFoldedRangesForNewRange(p_range);
  markDocumentContentsDirty(p_range->m_range);
  emit foldingRangesChanged();
}

bool TextFolding::unfoldRange(FoldingRange *p_range, bool p_remove) {
  Q_ASSERT(p_range);
  if (!p_remove && !p_range->isFolded()) {
    return false;
  }

  const bool needToRemove =
      p_remove || !(p_range->m_flags & TextFolding::FoldingRangeFlag::Persistent);

  if (needToRemove) {
    // Remove it from folding vector.
    auto &parentRanges = p_range->m_parent ? p_range->m_parent->m_nestedRanges : m_foldingRanges;
    FoldingRange::Vector newRanges;
    newRanges.reserve(parentRanges.size());
    for (auto range : qAsConst(parentRanges)) {
      if (range == p_range) {
        // Reparent our nested folding ranges.
        for (auto nestedRange : qAsConst(p_range->m_nestedRanges)) {
          nestedRange->m_parent = p_range->m_parent;
          newRanges.push_back(nestedRange);
        }
      } else {
        newRanges.push_back(range);
      }
    }

    parentRanges = newRanges;
  }

  // Unfold the range if needed.
  if (p_range->isFolded()) {
    p_range->m_flags &= ~TextFolding::FoldingRangeFlag::Folded;
    updateFoldedRangesForRemovedRange(p_range);
  }

  markDocumentContentsDirty(p_range->m_range);
  emit foldingRangesChanged();

  if (needToRemove) {
    // We have transferred them to parent.
    p_range->m_nestedRanges.clear();
    delete p_range;
  }

  return needToRemove;
}

void TextFolding::markDocumentContentsDirty(int p_position, int p_length) const {
  m_document->markContentsDirty(p_position, p_length);
}

void TextFolding::markDocumentContentsDirty(const TextBlockRange &p_range) {
  if (!p_range.isValid()) {
    markDocumentContentsDirty();
    return;
  }

  int pos = p_range.first().position();
  int len = p_range.last().position() + p_range.last().length() - pos + 1;
  markDocumentContentsDirty(pos, len);
}

TextFolding::FoldingRange::Vector
TextFolding::retrieveFoldedRanges(const TextFolding::FoldingRange::Vector &p_ranges) const {
  FoldingRange::Vector foldedRanges;
  for (auto range : p_ranges) {
    if (range->isFolded()) {
      foldedRanges.push_back(range);
    } else {
      foldedRanges.append(retrieveFoldedRanges(range->m_nestedRanges));
    }
  }

  return foldedRanges;
}

void TextFolding::unfoldRangeWithNestedFoldedRanges(
    const TextBlockRange &p_range,
    const TextFolding::FoldingRange::Vector &p_foldedChildren) const {
  if (!p_range.isValid()) {
    // We will update the visibility of all blocks later.
    return;
  }

  auto blockIt = p_range.first();
  auto lastBlockNumber = p_range.last().blockNumber();
  int idx = 0;
  while (blockIt.isValid()) {
    int blockNumber = blockIt.blockNumber();
    bool isHit = false;
    if (idx < p_foldedChildren.size()) {
      const auto &range = p_foldedChildren[idx];
      if (range->contains(blockNumber)) {
        isHit = true;
        if (range->first() == blockNumber) {
          blockIt.setVisible(true);
        } else if (range->last() == blockNumber) {
          blockIt.setVisible(false);
          ++idx;
        } else {
          blockIt.setVisible(false);
        }
      }
    }

    if (!isHit) {
      blockIt.setVisible(true);
    }

    if (blockNumber == lastBlockNumber) {
      break;
    }
    blockIt = blockIt.next();
  }
}

void TextFolding::checkAndUpdateFoldings() {
  if (!m_enabled) {
    return;
  }

  bool needUpdate = checkAndUpdateFoldings(m_foldingRanges);
  if (needUpdate) {
    markDocumentContentsDirty();
    emit foldingRangesChanged();
  }
}

bool TextFolding::checkAndUpdateFoldings(TextFolding::FoldingRange::Vector &p_ranges) {
  if (p_ranges.isEmpty()) {
    return false;
  }

  bool needUpdate = false;
  bool needUnfold = false;

  FoldingRange::Vector newRanges;
  newRanges.reserve(p_ranges.size());
  for (const auto &range : qAsConst(p_ranges)) {
    bool ret = checkAndUpdateFoldings(range->m_nestedRanges);
    needUpdate = needUpdate | ret;

    // All of its children have been updated now.

    if (range->isValid()) {
      range->m_range.update();
      newRanges.push_back(range);
    } else {
      // Remove this range and expose its children.
      needUpdate = true;

      // Remove the range from id mapping.
      m_idToFoldingRange.remove(range->m_id);

      // Reparent our nested folding ranges.
      for (auto nestedRange : qAsConst(range->m_nestedRanges)) {
        nestedRange->m_parent = range->m_parent;
        newRanges.push_back(nestedRange);
      }

      if (range->isFolded()) {
        range->m_flags &= ~TextFolding::FoldingRangeFlag::Folded;
        updateFoldedRangesForRemovedRange(range);

        needUnfold = true;
      }

      range->m_nestedRanges.clear();
      delete range;
    }
  }

  p_ranges = newRanges;

  // We need to update the visibility of all blocks.
  if (needUnfold) {
    TextBlockRange fullRange(m_document->firstBlock(), m_document->lastBlock());
    unfoldRangeWithNestedFoldedRanges(fullRange, m_foldedFoldingRanges);
  }

  return needUpdate;
}

QString TextFolding::debugDump() const {
  return QStringLiteral("tree %1 - folded %2")
      .arg(debugDump(m_foldingRanges, true), debugDump(m_foldedFoldingRanges, false));
}

QString TextFolding::debugDump(const TextFolding::FoldingRange::Vector &p_ranges,
                               bool p_recursive) const {
  QString dumpStr;
  for (const auto &range : p_ranges) {
    Q_ASSERT(range->isValid());

    if (!dumpStr.isEmpty()) {
      dumpStr += QLatin1Char(' ');
    }

    const QString persistentStr =
        (range->m_flags & FoldingRangeFlag::Persistent) ? QStringLiteral("p") : QString();
    const QString foldedStr = range->isFolded() ? QStringLiteral("f") : QString();
    dumpStr += QStringLiteral("[%1 %2%3 ").arg(range->first()).arg(persistentStr, foldedStr);

    if (p_recursive) {
      QString innerDump = debugDump(range->m_nestedRanges, p_recursive);
      if (!innerDump.isEmpty()) {
        dumpStr += innerDump + QLatin1Char(' ');
      }
    }

    dumpStr += QStringLiteral("%1]").arg(range->last());
  }

  return dumpStr;
}

void TextFolding::setExtraSelectionMgr(ExtraSelectionMgr *p_mgr) {
  Q_ASSERT(!m_extraSelectionMgr);
  m_extraSelectionMgr = p_mgr;
  Q_ASSERT(m_extraSelectionMgr);

  m_extraSelectionType = m_extraSelectionMgr->registerExtraSelection();
  m_extraSelectionMgr->setExtraSelectionEnabled(m_extraSelectionType, true);
  m_extraSelectionMgr->setExtraSelectionFormat(m_extraSelectionType, QColor(),
                                               m_foldedFoldingRangeLineBackground, true);
  connect(this, &TextFolding::foldingRangesChanged, this, [=]() {
    QList<QTextCursor> selections;
    for (const auto &range : m_foldedFoldingRanges) {
      if (!range->isValid()) {
        continue;
      }
      auto block = m_document->findBlockByNumber(range->first());
      if (block.isValid()) {
        selections.append(QTextCursor(block));
      }
    }
    m_extraSelectionMgr->setSelections(m_extraSelectionType, selections);
  });
}

int TextFolding::lineToVisibleLine(int p_line) const {
  if (m_foldedFoldingRanges.isEmpty()) {
    return p_line;
  }
  if (p_line < 0) {
    return 0;
  }

  // visible line - line.
  int delta = 0;
  for (const auto &range : m_foldedFoldingRanges) {
    Q_ASSERT(range->isValid());
    if (p_line <= range->first()) {
      return p_line + delta;
    } else if (p_line <= range->last()) {
      // Locate within the folded range.
      delta += (range->first() - p_line);
      return p_line + delta;
    } else {
      delta += (range->first() - range->last());
    }
  }

  // @p_line goes through all the folded folding ranges.
  return qMin(p_line, m_document->blockCount() - 1) + delta;
}

int TextFolding::visibleLineToLine(int p_line) const {
  if (m_foldedFoldingRanges.isEmpty()) {
    return p_line;
  }
  if (p_line < 0) {
    return 0;
  }

  // line - visual line.
  int delta = 0;
  for (const auto &range : m_foldedFoldingRanges) {
    Q_ASSERT(range->isValid());
    // Visual line for this range.
    int visualLine = range->first() - delta;
    if (visualLine >= p_line) {
      return p_line + delta;
    } else {
      delta += (range->last() - range->first());
    }
  }

  // @p_line goes through all the folded folding ranges.
  return qMin(p_line + delta, m_document->blockCount() - 1);
}

void TextFolding::setEnabled(bool p_enable) {
  m_enabled = p_enable;
  if (!p_enable) {
    clear();
  }
}

void TextFolding::setFoldedFoldingRangeLineBackgroundColor(const QColor &p_color) {
  m_foldedFoldingRangeLineBackground = p_color;
  m_extraSelectionMgr->setExtraSelectionFormat(m_extraSelectionType, QColor(),
                                               m_foldedFoldingRangeLineBackground, true);
}
