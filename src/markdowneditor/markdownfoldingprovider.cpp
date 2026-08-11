#include "markdownfoldingprovider.h"

#include <algorithm>

#include <QDebug>
#include <QTextBlock>
#include <QTextDocument>

#include <vtextedit/previewdata.h>
#include <vtextedit/textblockdata.h>
#include <vtextedit/textrange.h>

#include <texteditor/textfolding.h>

#include "textdocumentlayout.h"

namespace vte {

namespace {
// Which element type a foldable region may carry a preview for. Region type is
// what stops a heading section that merely *contains* a previewed code block
// from folding.
bool previewTypeForRegion(md::FoldingRegionType p_region, PreviewElementType *p_type) {
  switch (p_region) {
  case md::FencedCode:
    *p_type = PreviewElementType::Code;
    return true;
  case md::Math:
    *p_type = PreviewElementType::Math;
    return true;
  case md::Table:
    *p_type = PreviewElementType::Table;
    return true;
  default:
    // Heading, Blockquote and FrontMatter are wrappers: they never carry a
    // preview of their own.
    return false;
  }
}

// The inverse of previewTypeForRegion(). Image is absent on purpose: an image
// never produces a folding region.
bool regionTypeForPreview(PreviewElementType p_type, md::FoldingRegionType *p_region) {
  switch (p_type) {
  case PreviewElementType::Code:
    *p_region = md::FencedCode;
    return true;
  case PreviewElementType::Math:
    *p_region = md::Math;
    return true;
  case PreviewElementType::Table:
    *p_region = md::Table;
    return true;
  default:
    return false;
  }
}

// The painted preview which stands in for @p_type, if any. The table itself
// lives in textdocumentlayout.h, next to the claim filter which uses the
// forward direction, so the two cannot disagree.
bool paintedSourceForPreview(PreviewElementType p_type, PreviewData::Source *p_source) {
  return previewElementTypeToSource(p_type, p_source);
}

// Whether @p_block carries a painted, non-inline preview of @p_source which has
// actually been rendered.
//
// Probing goes through QTextBlock::userData() rather than TextBlockData::get()
// or BlockPreviewData::get(): both of those create and install the data they
// are asked for, which would make a read-only pass mutate every block it looks
// at.
bool blockHasPaintedPreview(const QTextBlock &p_block, PreviewData::Source p_source) {
  if (!p_block.isValid()) {
    return false;
  }

  auto blockData = static_cast<TextBlockData *>(p_block.userData());
  if (!blockData) {
    return false;
  }

  const auto &previewData = blockData->getBlockPreviewData();
  if (!previewData) {
    return false;
  }

  const auto &previews = previewData->getPreviewData();
  for (auto preview : previews) {
    if (!preview || preview->source() != p_source) {
      continue;
    }

    auto image = preview->getImageData();
    if (!image || image->m_inline || image->m_imageSize.isEmpty()) {
      continue;
    }

    return true;
  }

  return false;
}

// Rank used to resolve two regions which cover exactly the same blocks.
// Preview-bearing types win over wrappers, ties go to the lower enum value.
int regionRank(const md::FoldingRegion &p_region) {
  PreviewElementType type = PreviewElementType::Image;
  const int group = previewTypeForRegion(p_region.m_type, &type) ? 0 : 1;
  return group * 100 + static_cast<int>(p_region.m_type);
}
} // namespace

MarkdownFoldingProvider::MarkdownFoldingProvider(TextFolding *p_textFolding,
                                                 QTextDocument *p_document)
    : m_textFolding(p_textFolding), m_document(p_document)
{
}

void MarkdownFoldingProvider::updateFoldingRegions(const QVector<md::FoldingRegion> &p_regions)
{
  // 0. Drop the ranges the document has invalidated behind TextFolding's back.
  //
  // Replacing the whole source of an element in one edit - what the preview
  // write-back path does when a table cell is edited, and what a paste or an
  // undo over the element does - destroys the blocks a range spans. The range
  // survives that edit holding stale endpoints and stops matching the block it
  // is supposed to start on, so the gutter loses its folding marker. A parse
  // result always describes a settled document, which makes this the right
  // moment to re-validate.
  m_textFolding->checkAndUpdateFoldings();

  // 1. Filter out regions that span fewer than 2 blocks.
  QVector<md::FoldingRegion> valid;
  valid.reserve(p_regions.size());
  for (const auto &r : p_regions) {
    if (r.m_endBlock - r.m_startBlock >= 1) {
      valid.append(r);
    }
  }

  // 1b. De-duplicate exact extents.
  //
  // A blockquote wrapping nothing but a table emits two regions covering
  // exactly the same blocks, and TextFolding can only hold one of them: it
  // rejects a second range starting on the same block. std::sort is not
  // stable, so without a deterministic rule which of the two survives would be
  // unspecified, and the fold state would follow whichever one won this parse.
  //
  // De-duplicating here rather than merely ordering the loser later is also
  // what lets a live wrapper entry be replaced when a table grows into exactly
  // its extent: the wrapper region is gone from the parsed set, so nothing
  // matches the live wrapper entry, it is removed below and the table is
  // created in its place.
  {
    QHash<QPair<int, int>, int> extentIndex;
    QVector<md::FoldingRegion> unique;
    unique.reserve(valid.size());
    for (const auto &r : valid) {
      const auto key = qMakePair(r.m_startBlock, r.m_endBlock);
      auto it = extentIndex.find(key);
      if (it == extentIndex.end()) {
        extentIndex.insert(key, unique.size());
        unique.append(r);
        continue;
      }

      if (regionRank(r) < regionRank(unique[it.value()])) {
        unique[it.value()] = r;
      }
    }

    valid = unique;
  }

  // 2. Sort outermost-first: ascending startBlock, then descending size.
  std::sort(valid.begin(), valid.end(), [](const md::FoldingRegion &a, const md::FoldingRegion &b) {
    if (a.m_startBlock != b.m_startBlock) {
      return a.m_startBlock < b.m_startBlock;
    }
    return (a.m_endBlock - a.m_startBlock) > (b.m_endBlock - b.m_startBlock);
  });

  // 3. Snapshot the live entries at their *current* positions.
  //
  // The table's keys describe where each region was at the last
  // reconciliation. Any edit above a range shifts its block numbers without
  // telling anyone, so matching the parsed regions against those keys would
  // drop and recreate every range below the edit - and lose its fold state
  // with it. TextFolding knows where each range lives now, so ask it.
  struct LiveEntry {
    Entry m_entry;

    int m_first = 0;

    int m_last = 0;

    bool m_matched = false;
  };

  QVector<LiveEntry> live;
  live.reserve(m_entries.size());
  QHash<QPair<int, int>, int> liveIndex;
  for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
    int first = 0;
    int last = 0;
    if (!m_textFolding->foldingRangeBlocks(it.value().m_id, &first, &last)) {
      // The range is gone: the entry has nothing left to describe.
      continue;
    }

    const auto key = qMakePair(first, last);
    if (liveIndex.contains(key)) {
      // Two live ranges can never share an extent - identical extents imply
      // identical start blocks, which TextFolding rejects - so this can only
      // be a stale duplicate entry. Keep the first one.
      continue;
    }

    LiveEntry entry;
    entry.m_entry = it.value();
    entry.m_first = first;
    entry.m_last = last;
    liveIndex.insert(key, live.size());
    live.append(entry);
  }

  // 4. Match every parsed region against the snapshot, on the extent *and* the
  // type. A hit carries the entry over unchanged, which is what preserves the
  // fold state and the settled decision across a re-parse and across any edit
  // that only shifted block numbers. The type is compared rather than being
  // part of the key so a fenced code block edited into a table at the same
  // extent is treated as the new element it is.
  QHash<QPair<int, int>, Entry> newEntries;
  newEntries.reserve(valid.size());
  QVector<md::FoldingRegion> missing;
  for (const auto &r : valid) {
    const auto key = qMakePair(r.m_startBlock, r.m_endBlock);
    const int idx = liveIndex.value(key, -1);
    if (idx >= 0 && !live[idx].m_matched && live[idx].m_entry.m_type == r.m_type) {
      live[idx].m_matched = true;
      newEntries.insert(key, live[idx].m_entry);
      continue;
    }

    missing.append(r);
  }

  // 5. Remove every unmatched live range.
  //
  // Removal must precede creation: TextFolding refuses a new range starting on
  // the same block as an existing one, so a region which only changed one
  // endpoint would otherwise be refused and left with no range at all until
  // some later parse which may never come.
  for (const auto &entry : live) {
    if (!entry.m_matched) {
      m_textFolding->removeFoldingRange(entry.m_entry.m_id);
    }
  }

  // 6. Only then create the missing ranges, outermost-first.
  //
  // Creation can simply fail, e.g. when the range is not well nested with an
  // existing one; retrying it on the next parse costs nothing.
  for (const auto &r : missing) {
    TextBlockRange range(m_document->findBlockByNumber(r.m_startBlock),
                         m_document->findBlockByNumber(r.m_endBlock));
    qint64 id = m_textFolding->newFoldingRange(range, TextFolding::Persistent);
    if (id != TextFolding::InvalidRangeId) {
      Entry entry;
      entry.m_id = id;
      entry.m_type = r.m_type;
      newEntries.insert(qMakePair(r.m_startBlock, r.m_endBlock), entry);
    }
  }

  m_entries = newEntries;
}

void MarkdownFoldingProvider::clear()
{
  QVector<qint64> ids;
  ids.reserve(m_entries.size());
  for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
    ids.append(it.value().m_id);
  }
  m_entries.clear();
  for (qint64 id : ids) {
    m_textFolding->removeFoldingRange(id);
  }
}

void MarkdownFoldingProvider::resetState() { m_entries.clear(); }

void MarkdownFoldingProvider::setAutoFoldPreviewsEnabled(bool p_enabled)
{
  // Deliberately does not re-evaluate settled regions: the option only ever
  // decides the initial state of a region which has not been settled yet.
  m_autoFoldEnabled = p_enabled;
}

QVector<QPair<quint64, PreviewFoldState>>
MarkdownFoldingProvider::applyPreviewAutoFold(const QVector<PreviewedRange> &p_widgetRanges,
                                              int p_caretBlock)
{
  QVector<QPair<quint64, PreviewFoldState>> reports;

  // Folding availability is tracked separately from the option: with text
  // folding switched off there is no gutter to unfold with, so nothing may
  // hide source. A widget preview's remembered state is left untouched, so
  // re-enabling folding restores it once the next parse recreates the ranges.
  if (!m_textFolding->isEnabled()) {
    return reports;
  }

  // Index the widget previews by extent. The pass runs on every publish, so a
  // linear scan per entry would make it quadratic in the number of previewed
  // elements. Several elements can share an extent only across types, so the
  // bucket is walked for the type.
  QHash<QPair<int, int>, QVector<const PreviewedRange *>> widgetIndex;
  widgetIndex.reserve(p_widgetRanges.size());
  for (const auto &range : p_widgetRanges) {
    widgetIndex[qMakePair(range.m_startBlock, range.m_endBlock)].append(&range);
  }

  // Deciding and enforcing are two separate phases, because foldRange() emits
  // foldingRangesChanged() synchronously and that reaches application code:
  // the layout's widgetPreviewGeometryChanged is delivered directly to the
  // preview host, which hands a geometry context to every widget. Anything an
  // application does from there - disabling text folding, for instance - can
  // end up in resetState(), which clears m_entries. So nothing may hold an
  // iterator into it across a fold.
  struct PendingDecision {
    QPair<int, int> m_key;

    qint64 m_id = -1;

    bool m_fold = false;

    bool m_settle = false;

    bool m_hasWidget = false;

    quint64 m_identity = 0;

    // What the widget currently remembers, i.e. what a report is diffed
    // against.
    PreviewFoldState m_remembered = PreviewFoldState::Undecided;
  };

  QVector<PendingDecision> pending;

  for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
    const Entry &entry = it.value();

    PreviewElementType type = PreviewElementType::Image;
    if (!previewTypeForRegion(entry.m_type, &type)) {
      continue;
    }

    int first = 0;
    int last = 0;
    if (!m_textFolding->foldingRangeBlocks(entry.m_id, &first, &last)) {
      continue;
    }

    // Exact extent equality is correct rather than approximate: the folding
    // region and the preview element are computed from the same cmark node.
    const PreviewedRange *widget = nullptr;
    const auto bucket = widgetIndex.constFind(qMakePair(first, last));
    if (bucket != widgetIndex.constEnd()) {
      for (const auto *range : bucket.value()) {
        if (range->m_type == type) {
          widget = range;
          break;
        }
      }
    }

    // A settled region without a widget can neither be folded again nor
    // reported, so the painted probe below - two block lookups per entry - is
    // pure waste for it. This is the steady state on every keystroke.
    if (entry.m_autoFoldDecided && !widget) {
      continue;
    }

    bool hasPreview = widget != nullptr;
    if (!hasPreview) {
      // A painted preview carries no source range of its own, only the block
      // it is drawn on. Only the region's first and last block are inspected,
      // because those are the only two setRangeFolded() keeps visible - which
      // is what guarantees folding can never hide the very preview it folded
      // for. A host which attaches its preview to an interior block simply
      // never auto-folds, which is a safe failure.
      PreviewData::Source source = PreviewData::ImageLink;
      if (paintedSourceForPreview(type, &source)) {
        hasPreview = blockHasPaintedPreview(m_document->findBlockByNumber(first), source) ||
                     blockHasPaintedPreview(m_document->findBlockByNumber(last), source);
      }
    }

    if (!hasPreview) {
      continue;
    }

    PendingDecision decision;
    decision.m_key = it.key();
    decision.m_id = entry.m_id;
    decision.m_hasWidget = widget != nullptr;
    if (widget) {
      decision.m_identity = widget->m_identity;
      decision.m_remembered = widget->m_foldState;
    }

    if (!entry.m_autoFoldDecided) {
      if (widget && widget->m_foldState != PreviewFoldState::Undecided) {
        // The preview already knows its state: this range was just (re)created
        // by a rewrite, an undo, a re-parse or a folding enable cycle. This
        // branch runs even when the option is off, so restoring a fold across
        // a rewrite does not depend on auto-folding being enabled.
        decision.m_fold = widget->m_foldState == PreviewFoldState::Folded &&
                          !m_textFolding->isRangeFolded(entry.m_id);
      } else if (m_autoFoldEnabled && !(p_caretBlock > first && p_caretBlock < last)) {
        // The caret rule: folding keeps the first and last block visible, so
        // only a caret in the interior would be hidden by the fold.
        decision.m_fold = true;
      }

      // Settled either way, and settled the first time a preview is seen: the
      // option is never re-read for this range.
      decision.m_settle = true;
    }

    pending.append(decision);
  }

  for (const auto &decision : pending) {
    if (decision.m_fold) {
      m_textFolding->foldRange(decision.m_id);
    }

    if (decision.m_settle) {
      // Re-resolved rather than remembered: the fold above may have taken a
      // reentrant path which rebuilt or cleared the table.
      auto it = m_entries.find(decision.m_key);
      if (it != m_entries.end() && it.value().m_id == decision.m_id) {
        it.value().m_autoFoldDecided = true;
      }
    }

    if (!decision.m_hasWidget) {
      continue;
    }

    // Only report while the range is still live and folding is still on: a
    // reentrant disable destroys every range, and reporting "unfolded" for it
    // would throw away exactly the state re-enabling folding restores from.
    if (!m_textFolding->isEnabled() ||
        !m_textFolding->foldingRangeBlocks(decision.m_id, nullptr, nullptr)) {
      continue;
    }

    const auto liveState = m_textFolding->isRangeFolded(decision.m_id)
                               ? PreviewFoldState::Folded
                               : PreviewFoldState::Unfolded;
    if (liveState != decision.m_remembered) {
      reports.append(qMakePair(decision.m_identity, liveState));
    }
  }

  return reports;
}

bool MarkdownFoldingProvider::tryRegionFolded(PreviewElementType p_type, int p_startBlock,
                                              int p_endBlock, bool *p_folded) const
{
  // With text folding switched off there is no range a caller may act on, even
  // though a parse still creates them: newFoldingRange() is not gated on the
  // switch, only TextFolding's own maintenance is. Reporting "unfolded" here
  // would let the rewrite path overwrite what a preview remembers, and
  // re-enabling folding could then no longer restore it.
  if (!m_textFolding->isEnabled()) {
    return false;
  }

  md::FoldingRegionType regionType = md::Heading;
  if (!regionTypeForPreview(p_type, &regionType)) {
    return false;
  }

  // Walk the entries and resolve each id, so the answer does not depend on the
  // keys still being current.
  for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
    if (it.value().m_type != regionType) {
      continue;
    }

    int first = 0;
    int last = 0;
    if (!m_textFolding->foldingRangeBlocks(it.value().m_id, &first, &last)) {
      continue;
    }

    if (first != p_startBlock || last != p_endBlock) {
      continue;
    }

    if (p_folded) {
      *p_folded = m_textFolding->isRangeFolded(it.value().m_id);
    }
    return true;
  }

  return false;
}

void MarkdownFoldingProvider::rekeyEntriesByLiveExtent()
{
  QHash<QPair<int, int>, Entry> rekeyed;
  rekeyed.reserve(m_entries.size());
  for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
    int first = 0;
    int last = 0;
    if (!m_textFolding->foldingRangeBlocks(it.value().m_id, &first, &last)) {
      // Dead range: the entry describes nothing, and TextFolding has already
      // dropped it, so forgetting it leaks nothing.
      continue;
    }

    // Two live ranges can never share an extent, so this cannot evict a live
    // entry.
    rekeyed.insert(qMakePair(first, last), it.value());
  }

  m_entries = rekeyed;
}

bool MarkdownFoldingProvider::restoreFoldedRange(PreviewElementType p_type, int p_startBlock,
                                                 int p_endBlock)
{
  if (!m_textFolding->isEnabled()) {
    return false;
  }

  md::FoldingRegionType regionType = md::Heading;
  if (!regionTypeForPreview(p_type, &regionType)) {
    return false;
  }

  if (p_endBlock - p_startBlock < 1) {
    return false;
  }

  TextBlockRange range(m_document->findBlockByNumber(p_startBlock),
                       m_document->findBlockByNumber(p_endBlock));
  if (!range.isValid()) {
    return false;
  }

  qint64 id = m_textFolding->newFoldingRange(range,
                                             TextFolding::Persistent | TextFolding::Folded);
  if (id == TextFolding::InvalidRangeId) {
    // The next parse recreates the range and the restore branch of
    // applyPreviewAutoFold() folds it, with a visible flash in between.
    qWarning() << "failed to restore the folded range of a rewritten preview at ["
               << p_startBlock << "," << p_endBlock << "]";
    return false;
  }

  Entry entry;
  entry.m_id = id;
  entry.m_type = regionType;
  // Already settled: this is not a new initial decision, it is the state the
  // element carried into the rewrite.
  entry.m_autoFoldDecided = true;

  // The keys still describe where each region sat at the last reconciliation,
  // and the rewrite has just moved everything below it. Canonicalising them
  // first is what makes the insert below unable to evict a live entry: after
  // it, a key collision would mean another live range with exactly this
  // extent, which newFoldingRange() would have refused.
  rekeyEntriesByLiveExtent();
  m_entries.insert(qMakePair(p_startBlock, p_endBlock), entry);
  return true;
}

} // namespace vte
