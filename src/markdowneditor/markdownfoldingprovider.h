#ifndef MARKDOWNFOLDINGPROVIDER_H
#define MARKDOWNFOLDINGPROVIDER_H

#include <QHash>
#include <QPair>
#include <QVector>

#include <vtextedit/markdownhighlighterdata.h>
#include <vtextedit/preview.h>

namespace vte {
class TextFolding;
} // namespace vte

class QTextDocument;

namespace vte {

// Initial fold state a previewed element has been settled into. Undecided
// means no pass has ever seen this element together with a valid preview, so
// the option still gets to decide.
enum class PreviewFoldState { Undecided, Folded, Unfolded };

// One element which currently has a live interactive preview widget.
//
// The fold state of a previewed element belongs to the preview and not to the
// folding range: TextFolding destroys a range the moment the blocks it spans
// are replaced, which is exactly what an in-place source rewrite does, while
// the host's item and its anchor survive that edit.
struct PreviewedRange {
  quint64 m_identity = 0;

  int m_startBlock = 0;

  int m_endBlock = 0;

  PreviewElementType m_type = PreviewElementType::Image;

  PreviewFoldState m_foldState = PreviewFoldState::Undecided;
};

class MarkdownFoldingProvider {
public:
  MarkdownFoldingProvider(TextFolding *p_textFolding, QTextDocument *p_document);

  void updateFoldingRegions(const QVector<md::FoldingRegion> &p_regions);

  void clear();

  void resetState();

  void setAutoFoldPreviewsEnabled(bool p_enabled);

  // Decide, enforce and report the fold state of every previewed region.
  // @p_caretBlock is the block holding the caret, or -1. Returns the identities
  // whose PreviewFoldState the caller must write back onto its items.
  QVector<QPair<quint64, PreviewFoldState>>
  applyPreviewAutoFold(const QVector<PreviewedRange> &p_widgetRanges, int p_caretBlock);

  // Whether the range covering exactly [p_startBlock, p_endBlock] with the
  // folding type @p_type maps to is folded right now. False means "no such live
  // range" - which is not the same as unfolded, and is what folding being
  // disabled looks like - so the result is reported out separately.
  // Non-mutating and synchronous: used at the destructive-edit boundary, where
  // a queued report has not landed yet.
  bool tryRegionFolded(PreviewElementType p_type, int p_startBlock, int p_endBlock,
                       bool *p_folded) const;

  // Re-create, already folded and already decided, the range a preview-driven
  // rewrite destroyed. Called right after the edit. @p_type is mapped onto the
  // folding region type with the same table applyPreviewAutoFold() uses, so the
  // entry survives the next type-aware reconciliation instead of being dropped
  // and recreated.
  // Returns whether a range was actually created, so the caller can tell a real
  // restore from a no-op and skip the work it would otherwise owe.
  bool restoreFoldedRange(PreviewElementType p_type, int p_startBlock, int p_endBlock);

private:
  struct Entry {
    // TextFolding::InvalidRangeId. Spelled as a literal so the header can keep
    // forward-declaring TextFolding; a nested name needs the complete type.
    qint64 m_id = -1;

    md::FoldingRegionType m_type = md::Heading;

    // Whether the initial state of *this range* has already been settled.
    bool m_autoFoldDecided = false;
  };

  // Re-key every entry by the live extent of its range, dropping the entries
  // whose range is gone.
  void rekeyEntriesByLiveExtent();

  TextFolding *m_textFolding = nullptr;

  QTextDocument *m_document = nullptr;

  // Keyed by the region's (startBlock, endBlock) as of the last
  // reconciliation. The keys are only a starting point for matching: every
  // lookup which has to be exact resolves the range's *live* extent through
  // TextFolding instead.
  QHash<QPair<int, int>, Entry> m_entries;

  bool m_autoFoldEnabled = true;
};

} // namespace vte

#endif // MARKDOWNFOLDINGPROVIDER_H
