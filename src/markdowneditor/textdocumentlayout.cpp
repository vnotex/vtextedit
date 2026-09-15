#include "textdocumentlayout.h"

#include <QDebug>
#include <QFont>
#include <QFontMetrics>
#include <QLoggingCategory>
#include <QPainter>
#include <QPointF>
#include <QTextBlock>
#include <QTextBoundaryFinder>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextLayout>
#include <QTimer>

#include <private/qfontengine_p.h>
#include <private/qtextengine_p.h>

#include <algorithm>
#include <cmath>

#include <vtextedit/previewdata.h>
#include <vtextedit/textblockdata.h>

#include "documentresourcemgr.h"
#include "markdownhighlightblockdata.h"

using namespace vte;

namespace {
// Layout self-healing, reported once per pass. Declared here rather than in
// previewlogging.h because this translation unit is compiled on its own by
// tests/test_markdownfolding, which does not build the preview logging unit.
//
// QtWarningMsg for the same reason the preview categories use it: an embedding
// application must not get a trace it never asked for.
Q_LOGGING_CATEGORY(layoutRepairLog, "vte.layout.repair", QtWarningMsg)

// Where the interactive preview bands end up: the reservation inside the block,
// the block offset it is anchored to, and the document rects published from the
// two together. Same translation-unit-local reasoning as above.
//
//   QT_LOGGING_RULES="vte.layout.geometry=true"
Q_LOGGING_CATEGORY(layoutGeometryLog, "vte.layout.geometry", QtWarningMsg)

// This property belongs only to this adapter, never to syntax highlighting.
constexpr int c_concealFormatProperty = QTextFormat::UserProperty + 0x5654;

bool validateConcealedRange(const QTextBlock &p_block, const QTextDocument *p_document, int p_start,
                            int p_end, ConcealedRange &p_range) {
  if (!p_block.isValid() || p_block.document() != p_document || p_start < 0 || p_end <= p_start ||
      p_end > p_block.length() - 1) {
    return false;
  }
  const QString text = p_block.text();
  QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
  finder.setPosition(p_end);
  if (!finder.isAtBoundary()) {
    return false;
  }
  finder.setPosition(p_start);
  if (!finder.isAtBoundary()) {
    return false;
  }
  // Ten clusters suffice to prove that the nine-cluster display is shorter.
  int prefixEnd = p_start;
  for (int i = 0; i < 10; ++i) {
    const int next = finder.toNextBoundary();
    if (next < 0 || next > p_end) {
      return false;
    }
    if (i == 2) {
      prefixEnd = next;
    }
  }
  finder.setPosition(p_end);
  for (int i = 0; i < 3; ++i) {
    finder.toPreviousBoundary();
  }
  p_range = {p_start, p_end, prefixEnd, static_cast<int>(finder.position())};
  return true;
}

bool concealedAtCursor(const QTextBlock &p_block, const ConcealedRange &p_range, int p_cursor) {
  const int column = p_cursor < 0 ? -1 : p_cursor - p_block.position();
  return column < p_range.m_start || column >= p_range.m_end;
}

QVector<QTextLayout::FormatRange> sourceFormats(QTextLayout *p_layout) {
  auto formats = p_layout->formats();
  formats.erase(std::remove_if(formats.begin(), formats.end(),
                               [](const QTextLayout::FormatRange &p_range) {
                                 return p_range.format.hasProperty(c_concealFormatProperty);
                               }),
                formats.end());
  return formats;
}

void setConcealCachePolicy(QTextLayout *p_layout, BlockLayoutData &p_data, bool p_enabled) {
  if (p_enabled) {
    if (!p_data.m_concealOwnsCache) {
      p_data.m_sourceCacheEnabled = p_layout->cacheEnabled();
      p_data.m_concealOwnsCache = true;
    }
    p_layout->setCacheEnabled(true);
  } else if (p_data.m_concealOwnsCache) {
    p_layout->setCacheEnabled(p_data.m_sourceCacheEnabled);
    p_data.m_concealOwnsCache = false;
  }
}

// Only cached glyphs and temporary line-breaking attributes change. The engine's
// document block, source string, bidi analysis and UTF-16 coordinates stay intact.
class ConcealGlyphAdapter {
public:
  ConcealGlyphAdapter(QTextLayout *p_layout, QVector<ConcealedRange> p_ranges,
                      const QTextCharFormat &p_format, QPaintDevice *p_device,
                      BlockLayoutData &p_data)
      : m_engine(p_layout->engine()) {
    const auto formats = sourceFormats(p_layout);
    // A failed preflight removes just that range and rebuilds source itemization
    // without its overlays. No glyph storage is changed until all survivors are
    // ready; the retry strictly decreases the candidate set.
    for (;;) {
      auto combined = formats;
      for (const auto &range : p_ranges) {
        QTextLayout::FormatRange overlay;
        overlay.start = range.m_start;
        overlay.length = range.m_end - range.m_start;
        overlay.format = p_format;
        overlay.format.setProperty(c_concealFormatProperty, 1);
        combined.append(overlay);
        overlay.start = range.m_hiddenStart;
        overlay.length = range.m_hiddenEnd - range.m_hiddenStart;
        overlay.format.setProperty(c_concealFormatProperty, 2);
        combined.append(overlay);
      }
      // QTextLayout::setFormats() would notify QTextDocument recursively here.
      if (combined != p_layout->formats()) {
        m_engine->setFormats(combined);
      }
      p_layout->beginLayout();
      if (p_ranges.isEmpty()) {
        setConcealCachePolicy(p_layout, p_data, false);
        return;
      }
      const bool haveAttributes = m_engine->attributes() != nullptr;
      QVector<ConcealedRange> accepted;
      int glyphCount = 0;
      for (const auto &range : p_ranges) {
        Patch patch;
        if (!haveAttributes || !prepare(range, p_device, patch)) {
          continue;
        }
        glyphCount += patch.m_blankGlyphs.size() + 1;
        accepted.append(range);
        m_patches.append(std::move(patch));
      }
      if (accepted.size() != p_ranges.size() || !m_engine->ensureSpace(glyphCount)) {
        if (accepted.size() == p_ranges.size()) {
          accepted.clear(); // Allocation failed: keep the source revealed.
        }
        m_patches.clear();
        p_layout->endLayout();
        p_ranges = std::move(accepted);
        continue;
      }
      setConcealCachePolicy(p_layout, p_data, true);
      for (auto &patch : m_patches) {
        const auto &range = patch.m_range;
        const auto *attributes = m_engine->attributes();
        patch.m_sourceAttributes.reserve(range.m_hiddenEnd - range.m_hiddenStart);
        for (int pos = range.m_hiddenStart; pos < range.m_hiddenEnd; ++pos) {
          patch.m_sourceAttributes.append(attributes[pos]);
        }
        for (int item = patch.m_firstItem; item <= patch.m_lastItem; ++item) {
          auto &si = m_engine->layoutData->items[item];
          const bool first = item == patch.m_firstItem;
          si.glyph_data_offset = m_engine->layoutData->used;
          si.num_glyphs = first ? 2 : 1;
          m_engine->layoutData->used += si.num_glyphs;
          si.analysis.flags = QScriptAnalysis::None;
          const auto markerLine = patch.m_paint.m_layout->lineAt(0);
          si.width = first ? QFixed::fromReal(markerLine.horizontalAdvance()) : QFixed();
          si.ascent = first ? QFixed::fromReal(markerLine.ascent()) : QFixed();
          si.descent = first ? QFixed::fromReal(markerLine.descent()) : QFixed();
          si.leading = first ? QFixed::fromReal(qMax<qreal>(0, markerLine.leading())) : QFixed();
          auto glyphs = m_engine->shapedGlyphs(&si);
          glyphs.clear();
          glyphs.glyphs[0] = patch.m_blankGlyphs.at(item - patch.m_firstItem);
          glyphs.attributes[0].clusterStart = true;
          if (first) {
            // One cluster, two blanks. Qt's line breaker tests the literal
            // source SoftHyphen even when its break attribute is suppressed;
            // it reads advances[logClusters[position]] as the discretionary
            // hyphen width. Keep that leading advance zero and reserve the
            // marker width on a continuation glyph of the very same cluster.
            glyphs.glyphs[1] = glyphs.glyphs[0];
            glyphs.advances[1] = si.width;
          }
          // dontPrint must stay false: it would also suppress the advance.
          auto *clusters = m_engine->logClusters(&si);
          std::fill(clusters, clusters + m_engine->length(item), 0);
        }
      }
      suppressBreaks();
      return;
    }
  }

  ~ConcealGlyphAdapter() { restoreAttributes(); }

  void restoreAttributes() {
    if (m_restored || m_patches.isEmpty()) {
      return;
    }
    // Shaping ordinary items can have reallocated the engine buffer.
    auto *attributes = const_cast<QCharAttributes *>(m_engine->attributes());
    for (const auto &patch : m_patches) {
      std::copy(patch.m_sourceAttributes.cbegin(), patch.m_sourceAttributes.cend(),
                attributes + patch.m_range.m_hiddenStart);
    }
    m_restored = true;
  }

  QVector<ConcealPaintData> paintData(QTextLayout *p_layout) {
    QVector<ConcealPaintData> result;
    result.reserve(m_patches.size());
    for (auto &patch : m_patches) {
      const auto line = p_layout->lineForTextPosition(patch.m_range.m_hiddenStart);
      if (!line.isValid()) {
        continue;
      }
      // Reproduce the engine's visual item walk, not cursorToX() at a bidi
      // boundary (which deliberately prefers the paragraph-direction neighbor).
      const auto &sl = m_engine->lines.at(line.lineNumber());
      m_engine->shapeLine(sl);
      const int first = m_engine->findItem(sl.from);
      const int end = sl.from + sl.length + sl.trailingSpaces;
      const int last = m_engine->findItem(end - 1, first);
      QVector<int> order(last - first + 1);
      QVector<quint8> levels(order.size());
      for (int i = 0; i < order.size(); ++i) {
        levels[i] = m_engine->layoutData->items.at(first + i).analysis.bidiLevel;
      }
      QTextEngine::bidiReorder(order.size(), levels.constData(), order.data());
      QFixed x = sl.x + m_engine->alignLine(sl);
      for (int visual : order) {
        const int item = first + visual;
        const auto &si = m_engine->layoutData->items.at(item);
        if (item == patch.m_firstItem) {
          patch.m_paint.m_rect = QRectF(x.toReal(), line.y(), si.width.toReal(), line.height());
          // Give marker selections the whole reserved line-height rectangle.
          // The dot glyphs retain their own font and share the source baseline.
          auto *markerEngine = patch.m_paint.m_layout->engine();
          markerEngine->lines[0].ascent = sl.ascent;
          markerEngine->lines[0].descent = sl.descent;
          markerEngine->lines[0].leading = sl.leading;
          result.append(std::move(patch.m_paint));
          break;
        }
        if (si.analysis.flags >= QScriptAnalysis::TabOrObject) {
          x += si.width;
          continue;
        }
        const int length = m_engine->length(item);
        const int from = qMax(sl.from, si.position) - si.position;
        const int to = qMin(end - si.position, length);
        const auto *clusters = m_engine->logClusters(&si);
        const auto glyphs = m_engine->shapedGlyphs(&si);
        const int glyphEnd = to == length ? si.num_glyphs : clusters[to];
        for (int glyph = clusters[from]; glyph < glyphEnd; ++glyph) {
          x += glyphs.effectiveAdvance(glyph);
        }
      }
    }
    return result;
  }

private:
  struct Patch {
    ConcealedRange m_range;
    ConcealPaintData m_paint;
    int m_firstItem = -1;
    int m_lastItem = -1;
    QVector<glyph_t> m_blankGlyphs;
    QVector<QCharAttributes> m_sourceAttributes;
  };

  bool prepare(const ConcealedRange &p_range, QPaintDevice *p_device, Patch &p_patch) {
    p_patch.m_range = p_range;
    p_patch.m_firstItem = m_engine->findItem(p_range.m_hiddenStart);
    p_patch.m_lastItem = m_engine->findItem(p_range.m_hiddenEnd - 1, p_patch.m_firstItem);
    if (p_patch.m_firstItem < 0 || p_patch.m_lastItem < p_patch.m_firstItem ||
        m_engine->layoutData->items.at(p_patch.m_firstItem).position != p_range.m_hiddenStart ||
        m_engine->layoutData->items.at(p_patch.m_lastItem).position +
                m_engine->length(p_patch.m_lastItem) !=
            p_range.m_hiddenEnd) {
      return false;
    }
    const auto &first = m_engine->layoutData->items.at(p_patch.m_firstItem);
    const QFont font = m_engine->font(first);
    QTextCharFormat format = m_engine->format(&first);
    format.clearProperty(c_concealFormatProperty);
    format.setFont(font);
    format.setVerticalAlignment(QTextCharFormat::AlignNormal);
    auto marker = QSharedPointer<QTextLayout>::create(QString(3, QChar(0x00b7)), font, p_device);
    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    option.setTextDirection(first.analysis.bidiLevel % 2 ? Qt::RightToLeft : Qt::LeftToRight);
    marker->setTextOption(option);
    marker->setFormats({{0, 3, format}});
    marker->setCacheEnabled(true);
    marker->beginLayout();
    // Neutral dots alone are itemized as Common; in the source they inherit
    // the surrounding script, which also chooses the script-specific font fallback.
    // Qt also has synthetic scripts (such as Emoji) beyond QChar::ScriptCount;
    // those classifications belong to the hidden source, not to neutral dots.
    if (first.analysis.script < QChar::ScriptCount) {
      for (auto &item : marker->engine()->layoutData->items) {
        item.analysis.script = first.analysis.script;
      }
    }
    auto line = marker->createLine();
    if (line.isValid()) {
      line.setNumColumns(3);
    }
    marker->endLayout();
    if (!line.isValid() || !qIsFinite(line.horizontalAdvance()) || line.horizontalAdvance() <= 0 ||
        line.horizontalAdvance() >= INT_MAX / 128 || !qIsFinite(line.height()) ||
        line.height() <= 0 || line.height() >= INT_MAX / 128) {
      return false;
    }
    auto *markerEngine = marker->engine();
    for (const auto &item : markerEngine->layoutData->items) {
      if (!markerEngine->fontEngine(item)->glyphIndex(0x00b7)) {
        return false;
      }
    }
    for (int item = p_patch.m_firstItem; item <= p_patch.m_lastItem; ++item) {
      auto &si = m_engine->layoutData->items[item];
      // Use the engine which will paint the synthetic item, not a SmallCaps
      // scaled engine or the source tab/object branch. Keep bidi levels intact.
      const auto flags = si.analysis.flags;
      si.analysis.flags = QScriptAnalysis::None;
      const auto glyph = m_engine->fontEngine(si)->glyphIndex(0x20);
      si.analysis.flags = flags;
      if (!glyph) {
        return false;
      }
      p_patch.m_blankGlyphs.append(glyph);
    }
    p_patch.m_paint.m_hiddenStart = p_range.m_hiddenStart;
    p_patch.m_paint.m_hiddenEnd = p_range.m_hiddenEnd;
    p_patch.m_paint.m_layout = std::move(marker);
    return true;
  }

  void suppressBreaks() {
    auto *attributes = const_cast<QCharAttributes *>(m_engine->attributes());
    for (const auto &patch : m_patches) {
      const auto &range = patch.m_range;
      for (int pos = range.m_hiddenStart; pos < range.m_hiddenEnd; ++pos) {
        // A space at the first hidden position also belongs to the marker,
        // not to Qt's separately accumulated/trimmable whitespace run.
        attributes[pos].whiteSpace = false;
        if (pos > range.m_hiddenStart) {
          attributes[pos].graphemeBoundary = false;
          attributes[pos].lineBreak = false;
          attributes[pos].mandatoryBreak = false;
        }
      }
    }
  }

  QTextEngine *m_engine = nullptr;
  QVector<Patch> m_patches;
  bool m_restored = false;
};
} // namespace

const int TextDocumentLayout::c_markerThickness = 2;

const int TextDocumentLayout::c_maxInlineImageHeight = 400;

const int TextDocumentLayout::c_imagePadding = 2;

const int TextDocumentLayout::c_cursorGeometryWidth = 4;

const int TextDocumentLayout::c_widgetPreviewPadding = 2;

static bool realEqual(qreal p_a, qreal p_b) { return qAbs(p_a - p_b) < 1e-8; }

TextDocumentLayout::PassGuard::~PassGuard() {
  Q_ASSERT(m_layout->m_passDepth > 0);
  if (--m_layout->m_passDepth > 0) {
    // An outer pass is still running, so the document is still off limits.
    return;
  }

  if (m_layout->m_idleNotificationOwed) {
    m_layout->m_idleNotificationOwed = false;
    // Only arms timers on the other side; see becameIdle()'s contract.
    emit m_layout->becameIdle();
  }
  if (m_layout->m_concealGeometryChanged && m_layout->m_pendingConcealBlocks.isEmpty()) {
    m_layout->m_concealGeometryChanged = false;
    emit m_layout->concealmentChanged();
  }
}

TextDocumentLayout::TextDocumentLayout(QTextDocument *p_doc, DocumentResourceMgr *p_resourceMgr)
    : QAbstractTextDocumentLayout(p_doc), m_margin(p_doc->documentMargin()),
      m_resourceMgr(p_resourceMgr) {
  connect(this, &TextDocumentLayout::becameIdle, this, [this]() {
    if (m_pendingConcealBlocks.isEmpty() || m_concealTimerPending) {
      return;
    }
    m_concealTimerPending = true;
    QTimer::singleShot(0, this, [this]() {
      m_concealTimerPending = false;
      flushConcealRelayout();
    });
  });
}

bool TextDocumentLayout::conceal(const QTextBlock &p_block, int p_start, int p_end) {
  ConcealedRange range;
  if (!validateConcealedRange(p_block, document(), p_start, p_end, range)) {
    return false;
  }
  auto data = BlockLayoutData::get(p_block);
  // Do not retain any previous coordinates after an edit to their block.
  const bool current = data->m_concealRevision == p_block.revision();
  if (current) {
    const auto next = std::lower_bound(
        data->m_concealedRanges.cbegin(), data->m_concealedRanges.cend(), p_start,
        [](const ConcealedRange &p_range, int p_position) { return p_range.m_start < p_position; });
    if (next != data->m_concealedRanges.cend() && *next == range) {
      return true;
    }
    if ((next != data->m_concealedRanges.cend() && next->m_start < p_end) ||
        (next != data->m_concealedRanges.cbegin() && (next - 1)->m_end > p_start)) {
      return false;
    }
    const int index = static_cast<int>(next - data->m_concealedRanges.cbegin());
    data->m_concealedRanges.insert(index, range);
  } else {
    data->m_concealedRanges = {range};
  }
  data->m_concealRevision = p_block.revision();
  if (!m_concealBlocks.contains(p_block)) {
    m_concealBlocks.append(p_block);
  }
  queueConcealRelayout(p_block);
  flushConcealRelayout();
  return true;
}

bool TextDocumentLayout::setConcealedRanges(const QVector<ConcealSpec> &p_ranges) {
  auto specs = p_ranges;
  // Validate every block before using its position in the ordering.
  for (const auto &spec : specs) {
    if (!spec.m_block.isValid() || spec.m_block.document() != document()) {
      return false;
    }
  }
  std::sort(specs.begin(), specs.end(), [](const ConcealSpec &p_a, const ConcealSpec &p_b) {
    if (p_a.m_block.position() != p_b.m_block.position()) {
      return p_a.m_block.position() < p_b.m_block.position();
    }
    return p_a.m_start != p_b.m_start ? p_a.m_start < p_b.m_start : p_a.m_end < p_b.m_end;
  });
  struct Submission {
    QTextBlock m_block;
    QVector<ConcealedRange> m_ranges;
  };
  QVector<Submission> submissions;
  for (const auto &spec : specs) {
    if (!submissions.isEmpty() && submissions.constLast().m_block == spec.m_block &&
        !submissions.constLast().m_ranges.isEmpty()) {
      const auto &last = submissions.constLast().m_ranges.constLast();
      if (last.m_start == spec.m_start && last.m_end == spec.m_end) {
        continue;
      }
      if (spec.m_start < last.m_end) {
        return false;
      }
    }
    ConcealedRange range;
    if (!validateConcealedRange(spec.m_block, document(), spec.m_start, spec.m_end, range)) {
      return false;
    }
    if (submissions.isEmpty() || submissions.constLast().m_block != spec.m_block) {
      submissions.append({spec.m_block, {}});
    }
    submissions.last().m_ranges.append(range);
  }

  // Validation is complete. Merge the old/new participating blocks, not the
  // whole document. Live handles retain their order when preceding blocks move.
  m_concealBlocks.erase(
      std::remove_if(m_concealBlocks.begin(), m_concealBlocks.end(),
                     [](const QTextBlock &p_block) { return !p_block.isValid(); }),
      m_concealBlocks.end());
  std::sort(
      m_concealBlocks.begin(), m_concealBlocks.end(),
      [](const QTextBlock &p_a, const QTextBlock &p_b) { return p_a.position() < p_b.position(); });
  int oldIndex = 0;
  QVector<QTextBlock> blocks;
  blocks.reserve(submissions.size());
  for (auto &submission : submissions) {
    while (oldIndex < m_concealBlocks.size() &&
           m_concealBlocks.at(oldIndex).position() < submission.m_block.position()) {
      const auto block = m_concealBlocks.at(oldIndex++);
      auto data = BlockLayoutData::get(block);
      data->m_concealedRanges.clear();
      data->m_concealRevision = -1;
      queueConcealRelayout(block);
    }
    if (oldIndex < m_concealBlocks.size() && m_concealBlocks.at(oldIndex) == submission.m_block) {
      ++oldIndex;
    }
    auto data = BlockLayoutData::get(submission.m_block);
    if (data->m_concealRevision != submission.m_block.revision() ||
        data->m_concealedRanges != submission.m_ranges) {
      data->m_concealedRanges = std::move(submission.m_ranges);
      data->m_concealRevision = submission.m_block.revision();
      queueConcealRelayout(submission.m_block);
    }
    blocks.append(submission.m_block);
  }
  while (oldIndex < m_concealBlocks.size()) {
    const auto block = m_concealBlocks.at(oldIndex++);
    auto data = BlockLayoutData::get(block);
    data->m_concealedRanges.clear();
    data->m_concealRevision = -1;
    queueConcealRelayout(block);
  }
  m_concealBlocks = std::move(blocks);
  flushConcealRelayout();
  return true;
}

void TextDocumentLayout::setConcealFormat(const QTextCharFormat &p_format) {
  if (m_concealFormat == p_format) {
    return;
  }
  m_concealFormat = p_format;
  for (const auto &block : m_concealBlocks) {
    if (!block.isValid() || !block.isVisible() || !block.layout()->preeditAreaText().isEmpty()) {
      continue;
    }
    const auto data = BlockLayoutData::get(block);
    if (data->m_concealRevision != block.revision()) {
      continue;
    }
    for (const auto &range : data->m_concealedRanges) {
      if (concealedAtCursor(block, range, m_concealCursor)) {
        queueConcealRelayout(block);
        break;
      }
    }
  }
  flushConcealRelayout();
}

void TextDocumentLayout::setConcealCursorPosition(int p_position) {
  if (m_concealCursor == p_position) {
    return;
  }
  const int oldPosition = m_concealCursor;
  m_concealCursor = p_position;
  const auto oldBlock = oldPosition < 0 ? QTextBlock() : document()->findBlock(oldPosition);
  const auto newBlock = p_position < 0 ? QTextBlock() : document()->findBlock(p_position);
  for (const auto &block : {oldBlock, newBlock}) {
    if (!block.isValid() || !block.isVisible() || !block.layout()->preeditAreaText().isEmpty()) {
      continue;
    }
    const auto data = BlockLayoutData::get(block);
    if (data->m_concealRevision != block.revision()) {
      continue;
    }
    for (const auto &range : data->m_concealedRanges) {
      if (concealedAtCursor(block, range, oldPosition) !=
          concealedAtCursor(block, range, p_position)) {
        queueConcealRelayout(block);
        break;
      }
    }
    if (oldBlock == newBlock) {
      break;
    }
  }
  flushConcealRelayout();
}

void TextDocumentLayout::queueConcealRelayout(const QTextBlock &p_block) {
  if (!m_pendingConcealBlocks.contains(p_block)) {
    m_pendingConcealBlocks.append(p_block);
  }
  if (isBusy()) {
    requestIdleNotification();
  }
}

void TextDocumentLayout::flushConcealRelayout() {
  if (m_pendingConcealBlocks.isEmpty()) {
    return;
  }
  if (isBusy()) {
    requestIdleNotification();
    return;
  }
  PassGuard pass(this);
  OrderedIntSet blocks;
  for (const auto &block : m_pendingConcealBlocks) {
    if (block.isValid()) {
      blocks.insert(block.blockNumber(), QMapDummyValue());
    }
  }
  m_pendingConcealBlocks.clear();
  if (!blocks.isEmpty()) {
    relayout(blocks);
  }
}

void TextDocumentLayout::invalidateConcealRanges(const QTextBlock &p_block) {
  auto data = BlockLayoutData::get(p_block);
  if (!data->m_concealedRanges.isEmpty() && data->m_concealRevision != p_block.revision()) {
    data->m_concealedRanges.clear();
    data->m_concealRevision = -1;
    m_concealBlocks.removeAll(p_block);
  }
}

void TextDocumentLayout::setListItemRanges(TimeStamp p_timeStamp,
                                           const QVector<md::ListItemRange> &p_ranges) {
  if (p_timeStamp < m_listItemTimeStamp ||
      (p_timeStamp == m_listItemTimeStamp && m_listItemRanges.constData() == p_ranges.constData() &&
       m_listItemRanges.size() == p_ranges.size())) {
    return;
  }
  const bool wasVisible =
      (m_listGuideForeground.isValid() && !m_listItemRanges.isEmpty()) || m_activeListItem >= 0;
  m_listItemTimeStamp = p_timeStamp;
  m_listItemRanges = p_ranges;
  m_activeListItem = m_activeListBackground.isValid() ? listItemAtPosition(m_listItemCursor) : -1;
  if (wasVisible || (m_listGuideForeground.isValid() && !m_listItemRanges.isEmpty()) ||
      m_activeListItem >= 0) {
    emit update();
  }
}

void TextDocumentLayout::setListItemDecorationColors(const QColor &p_guideForeground,
                                                     const QColor &p_activeBackground) {
  if (p_guideForeground == m_listGuideForeground && p_activeBackground == m_activeListBackground) {
    return;
  }
  const bool guidesChanged =
      p_guideForeground != m_listGuideForeground && !m_listItemRanges.isEmpty();
  const int oldActive = m_activeListItem;
  const bool backgroundChanged = p_activeBackground != m_activeListBackground;
  m_listGuideForeground = p_guideForeground;
  m_activeListBackground = p_activeBackground;
  if (backgroundChanged) {
    m_activeListItem = p_activeBackground.isValid() ? listItemAtPosition(m_listItemCursor) : -1;
  }
  if (guidesChanged || (backgroundChanged && (oldActive >= 0 || m_activeListItem >= 0))) {
    emit update();
  }
}

void TextDocumentLayout::setListItemCursorPosition(int p_position) {
  if (m_listItemCursor == p_position) {
    return;
  }
  m_listItemCursor = p_position;
  if (!m_activeListBackground.isValid()) {
    return;
  }
  const int active = listItemAtPosition(p_position);
  if (active != m_activeListItem) {
    m_activeListItem = active;
    emit update();
  }
}

int TextDocumentLayout::listItemAtPosition(int p_position) const {
  if (p_position < 0 || m_listItemRanges.isEmpty()) {
    return -1;
  }
  const auto block = document()->findBlock(p_position);
  if (!block.isValid()) {
    return -1;
  }
  const int blockNumber = block.blockNumber();
  const int column = p_position - block.position();
  const auto next = std::upper_bound(m_listItemRanges.cbegin(), m_listItemRanges.cend(), column,
                                     [blockNumber](int p_column, const md::ListItemRange &p_range) {
                                       return blockNumber < p_range.m_startBlock ||
                                              (blockNumber == p_range.m_startBlock &&
                                               p_column < p_range.m_markerStart);
                                     });
  int item = static_cast<int>(next - m_listItemRanges.cbegin()) - 1;
  while (item >= 0) {
    const auto &range = m_listItemRanges.at(item);
    if (range.m_startBlock <= blockNumber && blockNumber <= range.m_endBlock &&
        (blockNumber > range.m_startBlock || column >= range.m_markerStart)) {
      return item;
    }
    item = range.m_parent;
  }
  return -1;
}

static void fillBackground(QPainter *p_painter, const QRectF &p_rect, QBrush p_brush,
                           QRectF p_gradientRect = QRectF()) {
  p_painter->save();
  if (p_brush.style() >= Qt::LinearGradientPattern &&
      p_brush.style() <= Qt::ConicalGradientPattern) {
    if (!p_gradientRect.isNull()) {
      QTransform m = QTransform::fromTranslate(p_gradientRect.left(), p_gradientRect.top());
      m.scale(p_gradientRect.width(), p_gradientRect.height());
      p_brush.setTransform(m);
      const_cast<QGradient *>(p_brush.gradient())->setCoordinateMode(QGradient::LogicalMode);
    }
  } else {
    p_painter->setBrushOrigin(p_rect.topLeft());
  }

  p_painter->fillRect(p_rect, p_brush);
  p_painter->restore();
}

void TextDocumentLayout::blockRangeFromRect(const QRectF &p_rect, int &p_first, int &p_last) const {
  if (p_rect.isNull()) {
    p_first = 0;
    p_last = document()->blockCount() - 1;
    return;
  }

  p_first = -1;
  p_last = document()->blockCount() - 1;
  qreal y = p_rect.y();
  QTextBlock block = document()->firstBlock();
  while (block.isValid()) {
    auto info = BlockLayoutData::get(block);
    Q_ASSERT(info->hasOffset());

    if (info->top() == y || (info->top() < y && info->bottom() >= y)) {
      p_first = block.blockNumber();
      break;
    }

    block = block.next();
  }

  if (p_first == -1) {
    p_last = -1;
    return;
  }

  y += p_rect.height();
  while (block.isValid()) {
    auto info = BlockLayoutData::get(block);
    Q_ASSERT(info->hasOffset());

    if (info->bottom() > y) {
      p_last = block.blockNumber();
      break;
    }

    block = block.next();
  }
}

void TextDocumentLayout::blockRangeFromRectBS(const QRectF &p_rect, int &p_first,
                                              int &p_last) const {
  if (p_rect.isNull()) {
    p_first = 0;
    p_last = document()->blockCount() - 1;
    return;
  }

  p_first = findBlockByPosition(p_rect.topLeft());
  if (p_first == -1) {
    p_last = -1;
    return;
  }

  qreal y = p_rect.bottom();
  QTextBlock block = document()->findBlockByNumber(p_first);
  auto info = BlockLayoutData::get(block);
  if (realEqual(info->top(), p_rect.top()) && p_first > 0) {
    --p_first;
  }

  p_last = document()->blockCount() - 1;
  while (block.isValid()) {
    auto tinfo = BlockLayoutData::get(block);
    if (!tinfo->hasOffset()) {
      qWarning() << "block without offset" << block.blockNumber() << tinfo->m_offset
                 << tinfo->m_rect << tinfo->m_rect.isNull();
    }

    Q_ASSERT(tinfo->hasOffset());

    if (tinfo->bottom() > y) {
      p_last = block.blockNumber();
      break;
    }

    block = block.next();
  }
}

int TextDocumentLayout::findBlockByPosition(const QPointF &p_point) const {
  QTextDocument *doc = document();
  int first = 0, last = doc->blockCount() - 1;
  qreal y = p_point.y();
  while (first <= last) {
    int mid = (first + last) / 2;
    QTextBlock blk = doc->findBlockByNumber(mid);
    auto info = BlockLayoutData::get(blk);
    if (!info) {
      return -1;
    }

    Q_ASSERT(info->hasOffset());
    if (info->top() <= y && info->bottom() > y) {
      // Found it.
      return mid;
    } else if (info->top() > y) {
      last = mid - 1;
    } else {
      first = mid + 1;
    }
  }

  QTextBlock blk = doc->lastBlock();
  auto info = BlockLayoutData::get(blk);
  if (y >= info->bottom()) {
    return blk.blockNumber();
  }

  return 0;
}

TextDocumentLayout::ListGuideAnchor TextDocumentLayout::listGuideAnchor(int p_item) const {
  const auto &range = m_listItemRanges.at(p_item);
  const auto block = document()->findBlockByNumber(range.m_startBlock);
  if (!block.isValid() || !block.isVisible()) {
    return {};
  }
  const auto blockData = dynamic_cast<TextBlockData *>(block.userData());
  if (!blockData) {
    return {};
  }
  const auto &data = blockData->getBlockLayoutData();
  const auto layout = block.layout();
  if (!data || !data->hasOffset() || !layout || !layout->preeditAreaText().isEmpty()) {
    return {};
  }
  const int start = range.m_markerStart;
  const int end = range.m_markerEnd;
  if (start < 0 || end <= start || end >= block.length()) {
    return {};
  }
  const auto line = layout->lineForTextPosition(start);
  if (!line.isValid() || end - 1 >= line.textStart() + line.textLength()) {
    return {};
  }
  return {(line.cursorToX(start) + line.cursorToX(end)) / 2,
          data->m_offset + line.y() + line.height(), true};
}

qreal TextDocumentLayout::listDecorationBottom(const QTextBlock &p_block,
                                               const BlockLayoutData &p_data) const {
  qreal bottom = 0;
  const auto layout = p_block.layout();
  for (int i = 0; i < layout->lineCount(); ++i) {
    const auto line = layout->lineAt(i);
    bottom = qMax(bottom, line.y() + line.height());
  }
  for (const auto &image : p_data.m_images) {
    bottom = qMax(bottom, image.m_rect.bottom());
  }
  for (const auto &widget : p_data.m_widgets) {
    bottom = qMax(bottom, widget.m_rect.bottom());
  }
  return qMin(p_data.bottom(), p_data.m_offset + bottom);
}

void TextDocumentLayout::drawActiveListItemBackground(QPainter *p_painter,
                                                      const QTextBlock &p_block,
                                                      const BlockLayoutData &p_data,
                                                      const QRectF &p_clip) const {
  const auto &range = m_listItemRanges.at(m_activeListItem);
  const int number = p_block.blockNumber();
  if (number < range.m_startBlock || number > range.m_endBlock ||
      p_block.layout()->preeditAreaText().size() > 0) {
    return;
  }
  const qreal width =
      document()->pageSize().width() <= 0 ? m_width - 2 * m_margin : availableContentWidth();
  if (width <= 0) {
    return;
  }
  const qreal bottom =
      number == range.m_endBlock ? listDecorationBottom(p_block, p_data) : p_data.bottom();
  QRectF band(m_margin, p_data.top(), width, bottom - p_data.top());
  if (!p_clip.isNull()) {
    band = band.intersected(p_clip);
  }
  if (!band.isEmpty()) {
    p_painter->fillRect(band, m_activeListBackground);
  }
}

void TextDocumentLayout::drawListItemGuides(QPainter *p_painter, const QTextBlock &p_block,
                                            const BlockLayoutData &p_data,
                                            const QVector<ActiveListGuide> &p_active,
                                            const QRectF &p_clip) const {
  const auto layout = p_block.layout();
  if (p_active.isEmpty() || !m_listGuideForeground.isValid() ||
      !layout->preeditAreaText().isEmpty()) {
    return;
  }
  const auto transform = p_painter->deviceTransform();
  bool invertible = false;
  const auto inverse = transform.inverted(&invertible);
  if (!invertible) {
    return;
  }
  // A cosmetic guide occupies one device pixel, not two logical pixels.
  const qreal halfPixel = (qAbs(inverse.m11()) + qAbs(inverse.m21())) / 2;

  // Runs are cached per visual line for all the guides crossing this block.
  // Document-backed QTextLayout::text() may be empty; query only the
  // characters needed for occlusion, without copying the block or document.
  struct WhitespaceRun {
    int m_start;
    int m_end;
    qreal m_left;
    qreal m_right;
    bool m_contiguous;
  };
  const auto doc = document();
  const int blockStart = p_block.position();
  const int blockLength = p_block.length() - 1;
  QVector<QVector<WhitespaceRun>> whitespace(layout->lineCount());
  QVector<QPair<qreal, qreal>> excluded;
  excluded.reserve(layout->lineCount() + p_data.m_images.size() + p_data.m_widgets.size());

  p_painter->save();
  p_painter->setRenderHint(QPainter::Antialiasing, false);
  QPen pen(m_listGuideForeground);
  pen.setWidth(0);
  pen.setCosmetic(true);
  pen.setStyle(Qt::SolidLine);
  pen.setCapStyle(Qt::FlatCap);
  p_painter->setPen(pen);

  for (const auto &guide : p_active) {
    if (!guide.m_anchor.m_valid) {
      continue;
    }
    const auto &range = m_listItemRanges.at(guide.m_item);
    qreal top = qMax(p_data.top(), guide.m_anchor.m_startY);
    qreal bottom = p_block.blockNumber() == range.m_endBlock ? listDecorationBottom(p_block, p_data)
                                                             : p_data.bottom();
    if (!p_clip.isNull()) {
      top = qMax(top, p_clip.top());
      bottom = qMin(bottom, p_clip.bottom());
    }
    if (bottom <= top) {
      continue;
    }
    auto devicePoint = transform.map(QPointF(guide.m_anchor.m_x, top));
    devicePoint.setX(std::floor(devicePoint.x()) + 0.5);
    const qreal x = inverse.map(devicePoint).x();
    if (!p_clip.isNull() && (x < p_clip.left() || x > p_clip.right())) {
      continue;
    }
    excluded.clear();
    for (int i = 0; i < layout->lineCount(); ++i) {
      const auto line = layout->lineAt(i);
      const qreal lineTop = p_data.m_offset + line.y();
      const qreal lineBottom = lineTop + line.height();
      if (lineBottom <= top || lineTop >= bottom) {
        continue;
      }
      const auto occupied = line.naturalTextRect();
      if (x + halfPixel <= occupied.left() || x - halfPixel >= occupied.right()) {
        continue;
      }
      bool clear = false;
      const int character = line.xToCursor(x, QTextLine::CursorOnCharacter);
      const int start = line.textStart();
      const int end = qMin(start + line.textLength(), blockLength);
      if (character >= start && character < end &&
          doc->characterAt(blockStart + character).isSpace()) {
        auto &runs = whitespace[i];
        const WhitespaceRun *run = nullptr;
        for (const auto &cached : runs) {
          if (character >= cached.m_start && character < cached.m_end) {
            run = &cached;
            break;
          }
        }
        if (!run) {
          int begin = character;
          int finish = character + 1;
          while (begin > start && doc->characterAt(blockStart + begin - 1).isSpace()) {
            --begin;
          }
          while (finish < end && doc->characterAt(blockStart + finish).isSpace()) {
            ++finish;
          }
          qreal previous = line.cursorToX(begin);
          qreal left = previous, right = previous;
          int direction = 0;
          bool contiguous = true;
          for (int c = begin + 1; c <= finish; ++c) {
            const qreal edge = line.cursorToX(c);
            const int step = edge > previous ? 1 : (edge < previous ? -1 : 0);
            if (direction && step && direction != step) {
              contiguous = false;
            }
            if (step) {
              direction = step;
            }
            left = qMin(left, edge);
            right = qMax(right, edge);
            previous = edge;
          }
          runs.append({begin, finish, left, right, contiguous});
          run = &runs.constLast();
        }
        clear = run->m_contiguous && x - halfPixel >= run->m_left && x + halfPixel <= run->m_right;
      }
      if (!clear) {
        excluded.append({lineTop, lineBottom});
      }
    }
    const auto excludePreview = [&](const QRectF &p_rect) {
      if (x >= p_rect.left() - halfPixel && x <= p_rect.right() + halfPixel) {
        excluded.append({p_data.m_offset + p_rect.top(), p_data.m_offset + p_rect.bottom()});
      }
    };
    for (const auto &image : p_data.m_images) {
      excludePreview(image.m_rect);
    }
    for (const auto &widget : p_data.m_widgets) {
      excludePreview(widget.m_rect);
    }
    std::sort(excluded.begin(), excluded.end());
    // Union the exclusions while drawing their complement, so adjacent free
    // bands become one segment rather than acquiring a cap at every line.
    for (const auto &interval : excluded) {
      if (interval.second <= top) {
        continue;
      }
      if (interval.first > top) {
        p_painter->drawLine(QPointF(x, top), QPointF(x, qMin(bottom, interval.first)));
      }
      top = qMax(top, interval.second);
      if (top >= bottom) {
        break;
      }
    }
    if (top < bottom) {
      p_painter->drawLine(QPointF(x, top), QPointF(x, bottom));
    }
  }
  p_painter->restore();
}

void TextDocumentLayout::draw(QPainter *p_painter, const PaintContext &p_context) {
  // Conservative: draw() emits nothing today, but it must never become a
  // window in which a widget can mutate the document.
  PassGuard pass(this);

  // Find out the blocks.
  int first, last;
  blockRangeFromRectBS(p_context.clip, first, last);
  if (first == -1) {
    return;
  }

  p_painter->setRenderHints(QPainter::SmoothPixmapTransform | QPainter::Antialiasing);

  QTextDocument *doc = document();
  QTextBlock block = doc->findBlockByNumber(first);
  // The left margin is already carried by the position of each QTextLine (see
  // layoutLines()) and by the image/marker rects, so the drawing offset must NOT
  // add it again. Adding it here would paint everything m_margin px to the right
  // of where blockBoundingRect() reports it, which makes the cursor rects Qt
  // computes (QWidgetTextControl::cursorRect()) miss the painted cursor column.
  // The drag&drop feedback cursor repaints exactly that unexpanded rect, so the
  // mismatch made the drop position cursor invisible (#2249).
  QPointF offset(0, 0);
  QTextBlock lastBlock = doc->findBlockByNumber(last);

  const bool guidesEnabled = m_listGuideForeground.isValid() && !m_listItemRanges.isEmpty();
  QVector<ActiveListGuide> activeGuides;
  int nextItem = 0;
  if (guidesEnabled) {
    const auto next = std::lower_bound(m_listItemRanges.cbegin(), m_listItemRanges.cend(), first,
                                       [](const md::ListItemRange &p_range, int p_block) {
                                         return p_range.m_startBlock < p_block;
                                       });
    nextItem = static_cast<int>(next - m_listItemRanges.cbegin());
    for (int item = nextItem - 1; item >= 0; item = m_listItemRanges.at(item).m_parent) {
      if (m_listItemRanges.at(item).m_endBlock >= first) {
        activeGuides.append({item, listGuideAnchor(item)});
      }
    }
    std::reverse(activeGuides.begin(), activeGuides.end());
  }

  QPen oldPen = p_painter->pen();
  p_painter->setPen(p_context.palette.color(QPalette::Text));

  while (block.isValid()) {
    if (guidesEnabled) {
      const int blockNumber = block.blockNumber();
      while (!activeGuides.isEmpty() &&
             m_listItemRanges.at(activeGuides.constLast().m_item).m_endBlock < blockNumber) {
        activeGuides.removeLast();
      }
      while (nextItem < m_listItemRanges.size() &&
             m_listItemRanges.at(nextItem).m_startBlock <= blockNumber) {
        const auto &range = m_listItemRanges.at(nextItem);
        while (!activeGuides.isEmpty() && activeGuides.constLast().m_item != range.m_parent) {
          activeGuides.removeLast();
        }
        activeGuides.append({nextItem, listGuideAnchor(nextItem)});
        ++nextItem;
      }
    }
    auto info = BlockLayoutData::get(block);
    Q_ASSERT(info->hasOffset());

    // Position each block at the offset the rest of the layout publishes for
    // it - the same value blockBoundingRect() returns, and therefore the one
    // the line number border, the hit testing and the preview widget bands are
    // all placed from. Accumulating block heights here instead would make the
    // painted text drift away from all of them as soon as a single height
    // disagreed with the offset chain, drawing the source over the previews
    // until a scroll re-anchored the walk on another block.
    if (layoutGeometryLog().isDebugEnabled() && !realEqual(offset.y(), info->top()) &&
        block.blockNumber() != first) {
      qCDebug(layoutGeometryLog) << "block" << block.blockNumber() << "offset" << info->top()
                                 << "disagrees with the accumulated height chain" << offset.y()
                                 << "by" << (info->top() - offset.y());
    }

    offset.setY(info->top());

    const QRectF &rect = info->m_rect;
    QTextLayout *layout = block.layout();
    if (!block.isVisible()) {
      offset.ry() += rect.height();
      if (block == lastBlock) {
        break;
      }

      block = block.next();
      continue;
    }

    QTextBlockFormat blockFormat = block.blockFormat();
    QBrush bg = blockFormat.background();
    if (bg != Qt::NoBrush) {
      int x = offset.x();
      int y = offset.y();
      fillBackground(p_painter, rect.adjusted(x, y, x, y), bg);
    }

    if (m_activeListBackground.isValid() && m_activeListItem >= 0) {
      drawActiveListItemBackground(p_painter, block, *info, p_context.clip);
    }
    if (guidesEnabled) {
      drawListItemGuides(p_painter, block, *info, activeGuides, p_context.clip);
    }

    auto selections = formatRangeFromSelection(block, p_context.selections);

    layout->draw(p_painter, offset, selections,
                 p_context.clip.isValid() ? p_context.clip : QRectF());

    for (const auto &marker : info->m_concealMarkers) {
      QVector<QTextLayout::FormatRange> markerSelections;
      for (const auto &selection : selections) {
        // Any nonempty intersection selects the complete visual unit. Let
        // QTextLayout apply its ordinary background/foreground precedence in
        // the original selection order, including search/extra selections.
        if (selection.length > 0 && selection.start < marker.m_hiddenEnd &&
            qint64(selection.start) + selection.length > marker.m_hiddenStart) {
          markerSelections.append({0, 3, selection.format});
        }
      }
      marker.m_layout->draw(p_painter, offset + marker.m_rect.topLeft(), markerSelections,
                            p_context.clip.isValid() ? p_context.clip : QRectF());
    }

    drawPreview(p_painter, block, offset);

    drawPreviewMarker(p_painter, block, offset);

    // Draw the cursor.
    {
      int blpos = block.position();
      int bllen = block.length();
      bool drawCursor =
          p_context.cursorPosition >= blpos && p_context.cursorPosition < blpos + bllen;
      if (drawCursor || (p_context.cursorPosition < -1 && !layout->preeditAreaText().isEmpty())) {
        int cursorPosition = p_context.cursorPosition - blpos;
        if (p_context.cursorPosition < -1) {
          cursorPosition = layout->preeditAreaPosition() - (p_context.cursorPosition + 2);
        }

        layout->drawCursor(p_painter, offset, cursorPosition, m_cursorWidth);
      }
    }

    offset.ry() += rect.height();
    if (block == lastBlock) {
      break;
    }

    block = block.next();
  }

  p_painter->setPen(oldPen);
}

QVector<QTextLayout::FormatRange>
TextDocumentLayout::formatRangeFromSelection(const QTextBlock &p_block,
                                             const QVector<Selection> &p_selections) const {
  QVector<QTextLayout::FormatRange> ret;

  int blpos = p_block.position();
  int bllen = p_block.length();
  for (int i = 0; i < p_selections.size(); ++i) {
    const QAbstractTextDocumentLayout::Selection &range = p_selections.at(i);
    const int selStart = range.cursor.selectionStart() - blpos;
    const int selEnd = range.cursor.selectionEnd() - blpos;
    if (selStart < bllen && selEnd > 0 && selEnd > selStart) {
      QTextLayout::FormatRange o;
      o.start = selStart;
      o.length = selEnd - selStart;
      o.format = range.format;
      ret.append(o);
    } else if (!range.cursor.hasSelection() &&
               range.format.hasProperty(QTextFormat::FullWidthSelection) &&
               p_block.contains(range.cursor.position())) {
      // For full width selections we don't require an actual selection, just
      // a position to specify the line. that's more convenience in usage.
      QTextLayout::FormatRange o;
      QTextLine l = p_block.layout()->lineForTextPosition(range.cursor.position() - blpos);
      if (!l.isValid()) {
        qWarning() << "invalid layout lineForTextPosition" << p_block.blockNumber()
                   << range.cursor.position() << blpos;
        Q_ASSERT(false);
        continue;
      }
      o.start = l.textStart();
      o.length = l.textLength();
      if (o.start + o.length == bllen - 1) {
        ++o.length; // include newline
      }

      o.format = range.format;
      ret.append(o);
    }
  }

  return ret;
}

int TextDocumentLayout::hitTest(const QPointF &p_point, Qt::HitTestAccuracy p_accuracy) const {
  int bn = findBlockByPosition(p_point);
  if (bn == -1) {
    return -1;
  }

  QTextBlock block = document()->findBlockByNumber(bn);
  Q_ASSERT(block.isValid());
  QTextLayout *layout = block.layout();
  int off = 0;
  const auto data = BlockLayoutData::get(block);
  QPointF pos = p_point - QPointF(0, data->top());
  const auto markerHit = [&](int p_line) {
    for (const auto &marker : data->m_concealMarkers) {
      if (marker.m_rect.contains(pos) &&
          layout->lineForTextPosition(marker.m_hiddenStart).lineNumber() == p_line) {
        return block.position() + marker.m_hiddenStart;
      }
    }
    return -1;
  };
  if (p_accuracy == Qt::ExactHit) {
    for (int i = 0; i < layout->lineCount(); ++i) {
      QTextLine line = layout->lineAt(i);
      const QRectF lr = line.naturalTextRect();
      if (pos.x() > lr.left() && pos.x() < lr.right() && pos.y() > lr.top() &&
          pos.y() < lr.bottom()) {
        const int concealed = markerHit(i);
        return concealed >= 0
                   ? concealed
                   : block.position() + line.xToCursor(pos.x(), QTextLine::CursorOnCharacter);
      }
    }

    return -1;
  }

  // Resolve the line vertically. A block reserves m_leadingSpaceOfLine above
  // every line, so a point may land outside all natural text rects while still
  // being inside the block. In such a gap the x coordinate must still be
  // honored, otherwise the cursor collapses to the line boundary.
  const int lineCount = layout->lineCount();
  if (pos.y() < 0) {
    // Above the whole document. findBlockByPosition() falls back to the first
    // block here, so keep the block start instead of resolving horizontally.
    return block.position();
  }

  int targetLine = -1;
  for (int i = 0; i < lineCount; ++i) {
    QTextLine line = layout->lineAt(i);
    const QRectF lr = line.naturalTextRect();
    if (pos.y() >= lr.bottom()) {
      // Below this line. Keep looking at the following ones.
      continue;
    }

    targetLine = i;
    if (pos.y() < lr.top() && i > 0) {
      // Within the gap above this line. layoutLines() reserves
      // m_leadingSpaceOfLine there plus, when this line owns an inline
      // preview, the image space. Only the plain leading space is shared with
      // the previous line; image space stays with the line owning the image.
      const QTextLine prevLine = layout->lineAt(i - 1);
      const qreal reserved = line.y() - (prevLine.y() + prevLine.height());
      const QRectF prevRect = prevLine.naturalTextRect();
      if (reserved <= m_leadingSpaceOfLine + 1e-6 &&
          pos.y() - prevRect.bottom() < lr.top() - pos.y()) {
        targetLine = i - 1;
      }
    }

    break;
  }

  if (targetLine != -1) {
    const int concealed = markerHit(targetLine);
    if (concealed >= 0) {
      return concealed;
    }
    off = layout->lineAt(targetLine).xToCursor(pos.x(), QTextLine::CursorBetweenCharacters);
  } else if (lineCount > 0) {
    // Below every line of the block, such as the preview image area. Keep the
    // cursor at the end of the block.
    const QTextLine line = layout->lineAt(lineCount - 1);
    off = line.textStart() + line.textLength();
  }

  return block.position() + off;
}

int TextDocumentLayout::pageCount() const { return 1; }

QSizeF TextDocumentLayout::documentSize() const { return QSizeF(m_width, m_height); }

QRectF TextDocumentLayout::frameBoundingRect(QTextFrame *p_frame) const {
  Q_UNUSED(p_frame);
  return QRectF(0, 0, qMax(document()->pageSize().width(), m_width), qreal(INT_MAX));
}

// Sometimes blockBoundingRect() may be called before documentChanged().
QRectF TextDocumentLayout::blockBoundingRect(const QTextBlock &p_block) const {
  if (!p_block.isValid()) {
    return QRectF();
  }
  PassGuard pass(const_cast<TextDocumentLayout *>(this));

  auto info = BlockLayoutData::get(p_block);
  if (!info->hasOffset()) {
    auto self = const_cast<TextDocumentLayout *>(this);
    const bool wasNull = info->isNull();
    if (wasNull) {
      self->layoutBlockAndUpdateOffset(p_block);
    } else {
      self->updateOffset(p_block);
    }

    qCDebug(layoutGeometryLog) << "lazy repair of block" << p_block.blockNumber()
                               << (wasNull ? "(relayout)" : "(offset only)") << "-> offset"
                               << info->m_offset << "height" << info->m_rect.height();

    // Both repairs move the offsets of the following blocks, and this entry
    // point is reached from painting and hit testing, which run no document
    // size pass afterwards. Without republishing here, the reserved bands move
    // with the text while the widgets stay behind and end up drawn on top of
    // the source. The emitter is a no-op when nothing actually moved.
    self->updateWidgetPreviewGeometry();
  }

  QRectF geo = info->m_rect.adjusted(0, info->m_offset, 0, info->m_offset);
  return geo;
}

void TextDocumentLayout::documentChanged(int p_from, int p_charsRemoved, int p_charsAdded) {
  PassGuard pass(this);

  QTextDocument *doc = document();
  int newBlockCount = doc->blockCount();
  m_concealBlocks.erase(
      std::remove_if(m_concealBlocks.begin(), m_concealBlocks.end(),
                     [](const QTextBlock &p_block) { return !p_block.isValid(); }),
      m_concealBlocks.end());

  // Update the margin.
  m_margin = doc->documentMargin();

  int charsChanged = p_charsRemoved + p_charsAdded;

  QTextBlock changeStartBlock = doc->findBlock(p_from);
  // May be an invalid block.
  QTextBlock changeEndBlock;
  if (p_charsRemoved == p_charsAdded && newBlockCount == m_blockCount &&
      changeStartBlock.position() == p_from && changeStartBlock.length() == p_charsAdded) {
    // TODO: we may need one more next block.
    changeEndBlock = changeStartBlock;
  } else {
    changeEndBlock = doc->findBlock(p_from + charsChanged);
  }

  /*
  qDebug() << "documentChanged" << p_from << p_charsRemoved << p_charsAdded
           << m_blockCount << newBlockCount
           << changeStartBlock.blockNumber() << changeEndBlock.blockNumber();
  */

  bool needRelayout = true;
  if (changeStartBlock == changeEndBlock && newBlockCount == m_blockCount) {
    // Change single block internal only.
    QTextBlock block = changeStartBlock;
    if (block.isValid() && block.length()) {
      needRelayout = false;
      QRectF oldBr = blockBoundingRect(block);
      clearBlockLayout(block);
      layoutBlockAndUpdateOffset(block);
      QRectF newBr = blockBoundingRect(block);
      // Only one block is affected.
      if (newBr.height() == oldBr.height()) {
        // Update document size.
        updateDocumentSizeWithOneBlockChanged(block);

        emit updateBlock(block);
        return;
      }
    }
  }

  if (needRelayout) {
    QTextBlock block = changeStartBlock;
    do {
      clearBlockLayout(block);
      layoutBlock(block);
      if (block == changeEndBlock) {
        break;
      }

      block = block.next();
    } while (block.isValid());

    updateOffset(changeStartBlock);
  }

  m_blockCount = newBlockCount;

  updateDocumentSize();

  // TODO: Update the view of all the blocks after changeStartBlock.
  qreal offset = BlockLayoutData::get(changeStartBlock)->m_offset;
  emit update(QRectF(0., offset, 1000000000., 1000000000.));
}

// MUST layout out the block after clearBlockLayout().
// TODO: Do we need to clear all the offset after @p_block?
void TextDocumentLayout::clearBlockLayout(QTextBlock &p_block) {
  p_block.clearLayout();
  auto info = BlockLayoutData::get(p_block);
  m_concealGeometryChanged |= !info->m_concealMarkers.isEmpty();
  info->reset();
}

// From Qt's qguiapplication_p.h.
static Qt::Alignment visualAlignment(Qt::LayoutDirection p_direction, Qt::Alignment p_alignment) {
  if (!(p_alignment & Qt::AlignHorizontal_Mask)) {
    p_alignment |= Qt::AlignLeft;
  }

  if (!(p_alignment & Qt::AlignAbsolute) && (p_alignment & (Qt::AlignLeft | Qt::AlignRight))) {
    if (p_direction == Qt::RightToLeft) {
      p_alignment ^= (Qt::AlignLeft | Qt::AlignRight);
    }

    p_alignment |= Qt::AlignAbsolute;
  }

  return p_alignment;
}

void TextDocumentLayout::layoutBlock(const QTextBlock &p_block) {
  QTextDocument *doc = document();
  Q_ASSERT(m_margin == doc->documentMargin());
  invalidateConcealRanges(p_block);

  if (!p_block.isVisible()) {
    // Invisible (folded) blocks get zero height but non-null rect to preserve
    // BlockLayoutData sentinel semantics (isNull() checks width AND height).
    QTextLayout *tl = p_block.layout();
    const auto formats = sourceFormats(tl);
    if (formats != tl->formats()) {
      tl->engine()->setFormats(formats);
    }
    setConcealCachePolicy(tl, *BlockLayoutData::get(p_block), false);
    tl->beginLayout();
    tl->endLayout();
    const_cast<QTextBlock &>(p_block).setLineCount(0);

    auto info = BlockLayoutData::get(p_block);
    info->reset();
    info->m_rect = QRectF(0, 0, m_margin * 2 + c_cursorGeometryWidth, 0);
    return;
  }

  QTextLayout *tl = p_block.layout();
  QTextOption option = doc->defaultTextOption();

  {
    auto direction = p_block.textDirection();
    option.setTextDirection(direction);

    auto alignment = option.alignment();
    QTextBlockFormat blockFormat = p_block.blockFormat();
    if (blockFormat.hasProperty(QTextFormat::BlockAlignment)) {
      alignment = blockFormat.alignment();
    }

    // For paragraph that are RTL, alignment is auto-reversed.
    option.setAlignment(visualAlignment(direction, alignment));
  }

  tl->setTextOption(option);

  int extraMargin = 0;
  if (option.flags() & QTextOption::AddSpaceForLineAndParagraphSeparators) {
    QFontMetrics fm(p_block.charFormat().font());
    extraMargin += fm.horizontalAdvance(QChar(0x21B5));
  }

  qreal availableWidth = doc->pageSize().width();
  if (availableWidth <= 0) {
    availableWidth = qreal(INT_MAX);
  }

  availableWidth -= (2 * m_margin + extraMargin + m_cursorMargin + c_cursorGeometryWidth);

  QVector<Marker> markers;
  QVector<ImagePaintData> images;
  QVector<WidgetPaintData> widgets;
  QVector<Marker> widgetMarkers;

  layoutLines(p_block, tl, markers, images, widgets, widgetMarkers, availableWidth, 0);

  // Set this block's line count to its layout's line count.
  // That is one block may occupy multiple visual lines.
  const_cast<QTextBlock &>(p_block).setLineCount(p_block.isVisible() ? tl->lineCount() : 0);

  // Update the info about this block.
  finishBlockLayout(p_block, markers, images, widgets, widgetMarkers);
}

void TextDocumentLayout::updateOffsetBefore(const QTextBlock &p_block) {
  auto info = BlockLayoutData::get(p_block);
  Q_ASSERT(!info->isNull());

  const int blockNum = p_block.blockNumber();
  if (blockNum == 0) {
    info->m_offset = 0;
  } else {
    QTextBlock blk = p_block.previous();
    while (blk.isValid()) {
      auto pinfo = BlockLayoutData::get(blk);
      if (!pinfo->hasOffset()) {
        int num = blk.blockNumber();
        if (pinfo->isNull()) {
          layoutBlock(blk);
        }

        if (num == 0) {
          pinfo->m_offset = 0;
        } else {
          blk = blk.previous();
          continue;
        }
      }

      // Now we reach a block with offset.
      qreal offset = pinfo->bottom();
      blk = blk.next();
      while (blk.isValid() && blk.blockNumber() <= blockNum) {
        auto ninfo = BlockLayoutData::get(blk);
        Q_ASSERT(!ninfo->isNull());
        ninfo->m_offset = offset;
        offset = ninfo->bottom();
        blk = blk.next();
      }

      break;
    }

    Q_ASSERT(info->hasOffset());
  }
}

// NOTICE: It will skip non-layouted or offset-non-changed blocks.
// So if you relayout separated blocks, you need to updateOffsetAfter() for each
// of them.
void TextDocumentLayout::updateOffsetAfter(const QTextBlock &p_block) {
  auto info = BlockLayoutData::get(p_block);
  Q_ASSERT(info->hasOffset());
  qreal offset = info->bottom();
  QTextBlock blk = p_block.next();
  while (blk.isValid()) {
    auto ninfo = BlockLayoutData::get(blk);
    if (ninfo->isNull()) {
      break;
    }

    if (realEqual(ninfo->m_offset, offset)) {
      break;
    }

    ninfo->m_offset = offset;
    offset = ninfo->bottom();
    blk = blk.next();
  }
}

qreal TextDocumentLayout::layoutLines(const QTextBlock &p_block, QTextLayout *p_tl,
                                      QVector<Marker> &p_markers, QVector<ImagePaintData> &p_images,
                                      QVector<WidgetPaintData> &p_widgets,
                                      QVector<Marker> &p_widgetMarkers, qreal p_availableWidth,
                                      qreal p_height) {
  Q_ASSERT(p_block.isValid());

  // Handle block inline image.
  QVector<PreviewData *> previewData;
  const QVector<PreviewData *> *pPreviewData = nullptr;
  if (m_previewEnabled) {
    previewData = unclaimedPreviewData(p_block);
    for (auto data : previewData) {
      auto imageData = data ? data->getImageData() : nullptr;
      if (imageData && imageData->m_inline) {
        pPreviewData = &previewData;
        break;
      }
    }
  }

  // Interactive preview widgets placed above their visual line.
  QVector<const WidgetPreviewSpec *> inlineWidgets;
  QVector<bool> inlineWidgetPlaced;
  if (m_previewEnabled) {
    inlineWidgets = widgetSpecsForBlock(p_block, PreviewPlacement::InlineAboveLine);
    inlineWidgetPlaced.fill(false, inlineWidgets.size());
  }

  const int blockPos = p_block.position();

  auto info = BlockLayoutData::get(p_block);
  QVector<ConcealedRange> concealed;
  if (p_tl->preeditAreaText().isEmpty()) {
    for (const auto &range : info->m_concealedRanges) {
      if (concealedAtCursor(p_block, range, m_concealCursor)) {
        concealed.append(range);
      }
    }
  }
  ConcealGlyphAdapter concealment(p_tl, std::move(concealed), m_concealFormat, paintDevice(),
                                  *info);

  int imgIdx = 0;
  while (true) {
    QTextLine line = p_tl->createLine();
    if (!line.isValid()) {
      break;
    }

    // Will introduce extra space on macOS.
    // line.setLeadingIncluded(true);
    line.setLineWidth(p_availableWidth);
    p_height += m_leadingSpaceOfLine;

    if (pPreviewData) {
      QVector<const PreviewImageData *> images;
      QVector<QPair<qreal, qreal>> imageRange;
      qreal imgHeight =
          fetchInlineImagesForOneLine(*pPreviewData, &line, m_margin, imgIdx, images, imageRange);

      for (int i = 0; i < images.size(); ++i) {
        layoutInlineImage(images[i], p_height, imgHeight, imageRange[i].first, imageRange[i].second,
                          p_markers, p_images);
      }

      if (!images.isEmpty()) {
        p_height += imgHeight + c_markerThickness * 2 + c_imagePadding * 2;
      }
    }

    if (!inlineWidgets.isEmpty()) {
      // Reserve one shared band above this visual line for every widget
      // anchored to it, using the same ownership rule as
      // inlinePlacementWidth() so the measured and assigned widths agree.
      QVector<int> onThisLine;
      QVector<QPair<qreal, qreal>> spans;
      qreal bandHeight = 0;
      for (int i = 0; i < inlineWidgets.size(); ++i) {
        if (inlineWidgetPlaced[i]) {
          continue;
        }

        const auto *spec = inlineWidgets[i];
        qreal startX = 0;
        qreal endX = 0;
        if (!lineClaimsRange(line, spec->m_startPos - blockPos, spec->m_endPos - blockPos, false,
                             &startX, &endX)) {
          continue;
        }

        onThisLine.append(i);
        spans.append(QPair<qreal, qreal>(startX, endX));
        bandHeight = qMax(bandHeight, spec->m_height);
        inlineWidgetPlaced[i] = true;
      }

      if (!onThisLine.isEmpty() && bandHeight > 0) {
        p_height += c_widgetPreviewPadding;
        for (int i = 0; i < onThisLine.size(); ++i) {
          const auto *spec = inlineWidgets[onThisLine[i]];
          WidgetPaintData wpd;
          wpd.m_id = spec->m_id;
          const qreal spanWidth = spans[i].second - spans[i].first;
          wpd.m_rect = QRectF(spans[i].first, p_height + bandHeight - spec->m_height,
                              qMax<qreal>(0, spanWidth), spec->m_height);
          p_widgets.append(wpd);
        }
        p_height += bandHeight + c_widgetPreviewPadding;

        // Dashed marker right below the band, spanning the claimed text range,
        // mirroring what layoutInlineImage() does for inline images. The rects
        // above are already final, so the extra height must be added only now.
        const qreal mky = p_height + c_markerThickness;
        for (int i = 0; i < onThisLine.size(); ++i) {
          Marker mk;
          mk.m_start = QPointF(spans[i].first, mky);
          mk.m_end = QPointF(spans[i].second, mky);
          p_widgetMarkers.append(mk);
        }

        p_height += c_markerThickness * 2;
      }
    }

    line.setPosition(QPointF(m_margin, p_height));
    p_height += line.height();
  }

  concealment.restoreAttributes();
  p_tl->endLayout();
  info->m_concealMarkers = concealment.paintData(p_tl);
  m_concealGeometryChanged |= !info->m_concealMarkers.isEmpty();

  return p_height;
}

void TextDocumentLayout::layoutInlineImage(const PreviewImageData *p_data, qreal p_heightInBlock,
                                           qreal p_imageSpaceHeight, qreal p_xStart, qreal p_xEnd,
                                           QVector<Marker> &p_markers,
                                           QVector<ImagePaintData> &p_images) {
  Marker mk;
  qreal mky = p_imageSpaceHeight + p_heightInBlock + c_imagePadding * 2 + c_markerThickness;
  mk.m_start = QPointF(p_xStart, mky);
  mk.m_end = QPointF(p_xEnd, mky);
  p_markers.append(mk);

  if (p_data) {
    QSize size = p_data->m_imageSize;
    scaleSize(size, p_xEnd - p_xStart, p_imageSpaceHeight);

    ImagePaintData ipd;
    ipd.m_name = p_data->m_imageName;
    ipd.m_rect = QRectF(
        QPointF(p_xStart, p_heightInBlock + c_imagePadding + p_imageSpaceHeight - size.height()),
        size);
    if (p_data->m_backgroundColor != 0) {
      ipd.m_backgroundColor = QColor(p_data->m_backgroundColor);
    }

    p_images.append(ipd);
  }
}

void TextDocumentLayout::finishBlockLayout(const QTextBlock &p_block,
                                           const QVector<Marker> &p_markers,
                                           const QVector<ImagePaintData> &p_images,
                                           const QVector<WidgetPaintData> &p_widgets,
                                           const QVector<Marker> &p_widgetMarkers) {
  Q_ASSERT(p_block.isValid());
  ImagePaintData ipd;
  QVector<WidgetPaintData> blockWidgets;
  auto info = BlockLayoutData::get(p_block);
  Q_ASSERT(info->isNull());
  auto concealMarkers = std::move(info->m_concealMarkers);
  info->reset();
  info->m_concealMarkers = std::move(concealMarkers);
  info->m_rect = blockRectFromTextLayout(p_block, &ipd, &blockWidgets);
  Q_ASSERT(!info->m_rect.isNull());

  bool hasPreview = false;
  if (ipd.isValid()) {
    Q_ASSERT(p_markers.isEmpty());
    Q_ASSERT(p_images.isEmpty());
    info->m_images.append(ipd);
    hasPreview = true;
  } else if (!p_markers.isEmpty()) {
    info->m_markers = p_markers;
    info->m_images = p_images;
    hasPreview = true;
  }

  // Inline widget markers are independent of the painted image branches above,
  // so they are merged after the assertions on p_markers have been evaluated.
  info->m_markers += p_widgetMarkers;

  // Inline bands are reserved during line layout; block bands are appended
  // after the text.
  info->m_widgets = p_widgets;
  info->m_widgets += blockWidgets;

  // Add vertical marker for painted previews as well as interactive preview
  // widgets. info->m_rect.height() already covers the reserved widget bands.
  if (hasPreview || !info->m_widgets.isEmpty()) {
    // Fill the marker.
    // Stored in block coordinates, just like the line positions and the image
    // rects, so that draw() needs no horizontal offset.
    const qreal markerX = m_margin - 1;
    Marker mk;
    mk.m_start = QPointF(markerX, 0);
    mk.m_end = QPointF(markerX, info->m_rect.height());

    info->m_markers.append(mk);
  }
}

void TextDocumentLayout::updateDocumentSize() {
  PassGuard pass(this);

  QTextDocument *doc = document();

  qreal oldHeight = m_height;
  qreal oldWidth = m_width;

  // One forward walk which repairs and measures at the same time.
  //
  // A block can lose its offset when a relayout walk missed it - most visibly
  // when a nested document edit merged into the pending change triple, so
  // documentChanged() was handed a range which no longer described the edit.
  // Repairing in the same forward order the widths are sampled in is safe:
  // every predecessor already has an offset by the time a block is reached, so
  // updateOffsetBefore() never lays an earlier block out again and no width
  // sampled before it can go stale.
  //
  // The repair short-circuits on hasOffset(), so the normal case does no
  // layout work at all. It also converges in one pass: updateOffsetBefore()
  // lays out every null predecessor and leaves the block with an offset, and
  // where updateOffsetAfter() stops early at another null block, this same walk
  // reaches and repairs that block itself.
  int repaired = 0;
  int firstRepaired = -1;

  m_width = 0;
  QTextBlock blk = doc->firstBlock();
  while (blk.isValid()) {
    auto ninfo = BlockLayoutData::get(blk);
    if (!ninfo->hasOffset()) {
      if (firstRepaired < 0) {
        firstRepaired = blk.blockNumber();
      }
      ++repaired;

      if (ninfo->isNull()) {
        // Guarded by isNull(), so finishBlockLayout()'s own assertion holds.
        layoutBlock(blk);
      }

      // Both directions: updateOffsetBefore() alone would leave every later
      // block describing the pre-repair geometry.
      updateOffset(blk);
    }

    Q_ASSERT(ninfo->hasOffset());
    if (m_width < ninfo->m_rect.width()) {
      m_width = ninfo->m_rect.width();
      m_maximumWidthBlockNumber = blk.blockNumber();
    }

    blk = blk.next();
  }

  if (repaired > 0) {
    // One line per pass, not one per block: the degraded case is exactly the
    // one where many blocks are broken at once.
    qCWarning(layoutRepairLog) << "repaired" << repaired
                               << "block(s) which lost their layout offset, from block"
                               << firstRepaired;
  }

  // Sampled after the repair, from the block the walk left with a valid offset.
  m_height = BlockLayoutData::get(doc->lastBlock())->bottom();

  if (!realEqual(oldHeight, m_height) || !realEqual(oldWidth, m_width)) {
    emit documentSizeChanged(documentSize());
  }

  updateWidgetPreviewGeometry();
}

QRectF TextDocumentLayout::blockRectFromTextLayout(const QTextBlock &p_block,
                                                   ImagePaintData *p_image,
                                                   QVector<WidgetPaintData> *p_widgets) {
  if (p_image) {
    *p_image = ImagePaintData();
  }

  QTextLayout *tl = p_block.layout();
  if (tl->lineCount() < 1) {
    return QRectF();
  }

  QRectF tlRect = tl->boundingRect();
  QRectF br(QPointF(0, 0), tlRect.bottomRight());

  // Do not know why. Copied from QPlainTextDocumentLayout.
  if (tl->lineCount() == 1) {
    br.setWidth(qMax(br.width(), tl->lineAt(0).naturalTextWidth()));
  }

  // Handle block non-inline image.
  if (m_previewEnabled) {
    const auto previewData = unclaimedPreviewData(p_block);
    if (previewData.size() == 1) {
      auto data = previewData.first();
      auto img = data ? data->getImageData() : nullptr;
      if (img && !img->m_inline) {
        int maximumWidth = tlRect.width();
        int padding;
        QSize size;
        adjustImagePaddingAndSize(img, maximumWidth, padding, size);

        if (p_image) {
          p_image->m_name = img->m_imageName;
          p_image->m_rect =
              QRectF(padding + m_margin, br.height() + m_leadingSpaceOfLine + c_imagePadding,
                     size.width(), size.height());
          if (img->m_backgroundColor != 0) {
            p_image->m_backgroundColor = QColor(img->m_backgroundColor);
          }
        }

        int dw = padding + size.width() + m_margin - br.width();
        int dh = size.height() + m_leadingSpaceOfLine + c_imagePadding * 2;
        br.adjust(0, 0, dw > 0 ? dw : 0, dh);
      }
    }
  }

  // Reserve the band of every interactive preview widget anchored after this
  // block. Widths are clamped to the available content width.
  if (m_previewEnabled && p_widgets) {
    const auto specs = widgetSpecsForBlock(p_block, PreviewPlacement::BlockAfterSource);
    if (!specs.isEmpty()) {
      const qreal maxWidth = availableContentWidth();
      qreal y = br.height();
      qreal maxRight = 0;
      for (const auto *spec : specs) {
        qreal width = spec->m_width;
        if (maxWidth > 0) {
          width = qMin(width, maxWidth);
        }
        width = qMax<qreal>(0, width);
        const qreal height = qMax<qreal>(0, spec->m_height);

        y += c_widgetPreviewPadding;

        WidgetPaintData wpd;
        wpd.m_id = spec->m_id;
        wpd.m_rect = QRectF(m_margin, y, width, height);
        p_widgets->append(wpd);

        y += height;
        maxRight = qMax(maxRight, m_margin + width);
      }

      y += c_widgetPreviewPadding;
      br.setHeight(y);
      br.setWidth(qMax(br.width(), maxRight));
    }
  }

  // Add the right margin. The left margin is already included in the bounding
  // rect, since every line is positioned at x == m_margin.
  br.adjust(0, 0, m_margin + c_cursorGeometryWidth, 0);

  // Add bottom margin.
  if (!p_block.next().isValid()) {
    br.adjust(0, 0, 0, m_margin);
  }

  return br;
}

void TextDocumentLayout::updateDocumentSizeWithOneBlockChanged(const QTextBlock &p_block) {
  PassGuard pass(this);

  auto info = BlockLayoutData::get(p_block);
  qreal width = info->m_rect.width();
  if (width > m_width) {
    m_width = width;
    m_maximumWidthBlockNumber = p_block.blockNumber();
    emit documentSizeChanged(documentSize());
  } else if (width < m_width && p_block.blockNumber() == m_maximumWidthBlockNumber) {
    // Shrink the longest block.
    updateDocumentSize();
    return;
  }

  updateWidgetPreviewGeometry();
}

void TextDocumentLayout::adjustImagePaddingAndSize(const PreviewImageData *p_data,
                                                   int p_maximumWidth, int &p_padding,
                                                   QSize &p_size) const {
  const int minimumImageWidth = 400;

  p_padding = p_data->m_padding;
  p_size = p_data->m_imageSize;

  if (!m_constrainPreviewWidthEnabled) {
    return;
  }

  int availableWidth = p_maximumWidth - p_data->m_padding;
  if (availableWidth < p_data->m_imageSize.width()) {
    // Need to resize the width.
    if (availableWidth >= minimumImageWidth) {
      p_size.scale(availableWidth, p_size.height(), Qt::KeepAspectRatio);
    } else {
      // Omit the padding.
      p_padding = 0;
      p_size.scale(p_maximumWidth, p_size.height(), Qt::KeepAspectRatio);
    }
  }
}

void TextDocumentLayout::drawPreview(QPainter *p_painter, const QTextBlock &p_block,
                                     const QPointF &p_offset) {
  const QVector<ImagePaintData> &images = BlockLayoutData::get(p_block)->m_images;
  if (images.isEmpty()) {
    return;
  }

  for (auto const &img : images) {
    const QPixmap *image = m_resourceMgr->findImage(img.m_name);
    if (!image) {
      continue;
    }

    QRect targetRect =
        img.m_rect.adjusted(p_offset.x(), p_offset.y(), p_offset.x(), p_offset.y()).toRect();

    // Qt do not render the background of some SVGs.
    // We add a forced background mechanism to complement this.
    if (img.hasForcedBackground()) {
      p_painter->fillRect(targetRect, img.m_backgroundColor);
    }

    p_painter->drawPixmap(targetRect, *image);
  }
}

void TextDocumentLayout::drawPreviewMarker(QPainter *p_painter, const QTextBlock &p_block,
                                           const QPointF &p_offset) {
  const QVector<Marker> &markers = BlockLayoutData::get(p_block)->m_markers;
  if (markers.isEmpty()) {
    return;
  }

  QPen oldPen = p_painter->pen();
  QPen newPen(m_previewMarkerForeground, c_markerThickness, Qt::DashLine);
  p_painter->setPen(newPen);

  for (auto const &mk : markers) {
    p_painter->drawLine(mk.m_start + p_offset, mk.m_end + p_offset);
  }

  p_painter->setPen(oldPen);
}

void TextDocumentLayout::relayout() {
  PassGuard pass(this);

  QTextDocument *doc = document();

  // Update the margin.
  m_margin = doc->documentMargin();

  QTextBlock block = doc->firstBlock();
  while (block.isValid()) {
    clearBlockLayout(block);
    layoutBlock(block);

    block = block.next();
  }

  updateOffset(doc->firstBlock());

  updateDocumentSize();

  emit update(QRectF(0., 0., 1000000000., 1000000000.));
}

void TextDocumentLayout::relayout(const OrderedIntSet &p_blocks) {
  PassGuard pass(this);

  if (p_blocks.isEmpty()) {
    return;
  }

  QTextDocument *doc = document();

  // Need to relayout and update blocks in ascending order.
  QVector<QTextBlock> blocks;
  blocks.reserve(p_blocks.size());
  for (auto bn = p_blocks.keyBegin(); bn != p_blocks.keyEnd(); ++bn) {
    QTextBlock block = doc->findBlockByNumber(*bn);
    if (block.isValid()) {
      blocks.append(block);
      clearBlockLayout(block);
      layoutBlock(block);
    }
  }

  if (blocks.isEmpty()) {
    return;
  }

  // Need to update offset for each of these discontinuous blocks, because
  // the offset of the non-touched blocks may be the same but there are still
  // touched blocks after them.
  for (auto &blk : blocks) {
    updateOffset(blk);
  }

  updateDocumentSize();

  qreal offset = BlockLayoutData::get(blocks.first())->m_offset;
  emit update(QRectF(0., offset, 1000000000., 1000000000.));
}

qreal TextDocumentLayout::fetchInlineImagesForOneLine(const QVector<PreviewData *> &p_data,
                                                      const QTextLine *p_line, qreal p_margin,
                                                      int &p_index,
                                                      QVector<const PreviewImageData *> &p_images,
                                                      QVector<QPair<qreal, qreal>> &p_imageRange) {
  qreal maxHeight = 0;
  int start = p_line->textStart();
  int end = p_line->textLength() + start;

  for (int i = 0; i < p_data.size(); ++i) {
    auto data = p_data[i];
    auto img = data ? data->getImageData() : nullptr;
    if (!img || !img->m_inline) {
      continue;
    }

    if (img->m_startPos >= start && img->m_startPos < end) {
      // Start of a new image.
      qreal startX = p_line->cursorToX(img->m_startPos) + p_margin;
      qreal endX;
      if (img->m_endPos <= end) {
        // End an image.
        endX = p_line->cursorToX(img->m_endPos) + p_margin;
        p_images.append(img);
        p_imageRange.append(QPair<qreal, qreal>(startX, endX));

        QSize size = img->m_imageSize;
        scaleSize(size, endX - startX, c_maxInlineImageHeight);
        if (size.height() > maxHeight) {
          maxHeight = size.height();
        }

        // Image i has been drawn.
        p_index = i + 1;
      } else {
        // This image cross the line.
        endX = p_line->x() + p_line->width() + p_margin;
        if (end - img->m_startPos >= ((img->m_endPos - img->m_startPos) >> 1)) {
          // Put image at this side.
          p_images.append(img);
          p_imageRange.append(QPair<qreal, qreal>(startX, endX));

          QSize size = img->m_imageSize;
          scaleSize(size, endX - startX, c_maxInlineImageHeight);
          if (size.height() > maxHeight) {
            maxHeight = size.height();
          }

          // Image i has been drawn.
          p_index = i + 1;
        } else {
          // Just put a marker here.
          p_images.append(NULL);
          p_imageRange.append(QPair<qreal, qreal>(startX, endX));
        }

        break;
      }
    } else if (img->m_endPos > start && img->m_startPos < start) {
      qreal startX = p_line->x() + p_margin;
      qreal endX = img->m_endPos > end ? p_line->x() + p_line->width() + p_margin
                                       : p_line->cursorToX(img->m_endPos) + p_margin;
      if (p_index <= i) {
        // Image i has not been drawn. Draw it here.
        p_images.append(img);
        p_imageRange.append(QPair<qreal, qreal>(startX, endX));

        QSize size = img->m_imageSize;
        scaleSize(size, endX - startX, c_maxInlineImageHeight);
        if (size.height() > maxHeight) {
          maxHeight = size.height();
        }

        // Image i has been drawn.
        p_index = i + 1;
      } else {
        // Image i has been drawn. Just put a marker here.
        p_images.append(NULL);
        p_imageRange.append(QPair<qreal, qreal>(startX, endX));
      }

      if (img->m_endPos >= end) {
        break;
      }
    } else if (img->m_endPos <= start) {
      continue;
    } else {
      break;
    }
  }

  return maxHeight;
}

int TextDocumentLayout::getTextWidthWithinTextLine(const QTextLayout *p_layout, int p_pos,
                                                   int p_length) {
  QTextLine line = p_layout->lineForTextPosition(p_pos);
  Q_ASSERT(line.isValid());
  Q_ASSERT(p_pos + p_length <= line.textStart() + line.textLength());
  Q_ASSERT(p_pos + p_length >= 0);
  return qAbs(line.cursorToX(p_pos + p_length) - line.cursorToX(p_pos));
}

void TextDocumentLayout::updateBlockByNumber(int p_blockNumber) {
  if (p_blockNumber == -1) {
    return;
  }

  QTextBlock block = document()->findBlockByNumber(p_blockNumber);
  if (block.isValid()) {
    emit updateBlock(block);
  }
}

void TextDocumentLayout::scaleSize(QSize &p_size, int p_width, int p_height) {
  if (p_size.width() > p_width || p_size.height() > p_height) {
    p_size.scale(p_width, p_height, Qt::KeepAspectRatio);
  }
}

void TextDocumentLayout::setCursorWidth(int p_width) {
  if (m_cursorWidth == p_width) {
    return;
  }

  m_cursorWidth = p_width;
  emit update();
}

int TextDocumentLayout::cursorWidth() const { return m_cursorWidth; }

void TextDocumentLayout::layoutBlockAndUpdateOffset(const QTextBlock &p_block) {
  layoutBlock(p_block);
  updateOffset(p_block);
}

void TextDocumentLayout::updateOffset(const QTextBlock &p_block) {
  updateOffsetBefore(p_block);
  updateOffsetAfter(p_block);
}

void TextDocumentLayout::setPreviewMarkerForeground(const QColor &p_color) {
  m_previewMarkerForeground = p_color;
}

void TextDocumentLayout::setConstrainPreviewWidthEnabled(bool p_enabled) {
  if (m_constrainPreviewWidthEnabled == p_enabled) {
    return;
  }

  m_constrainPreviewWidthEnabled = p_enabled;
  relayout();
}

void TextDocumentLayout::setPreviewEnabled(bool p_enabled) {
  if (m_previewEnabled == p_enabled) {
    return;
  }

  m_previewEnabled = p_enabled;
  relayout();
}

qreal TextDocumentLayout::getLeadingSpaceOfLine() const { return m_leadingSpaceOfLine; }

void TextDocumentLayout::setLeadingSpaceOfLine(qreal p_leading) {
  if (p_leading >= 0) {
    m_leadingSpaceOfLine = p_leading;
  }
}

void TextDocumentLayout::ensureWidgetPreviewMap() {
  const int revision = document()->revision();
  if (!m_widgetPreviewMapDirty && revision == m_widgetPreviewMapRevision) {
    return;
  }

  m_widgetPreviewMapDirty = false;
  m_widgetPreviewMapRevision = revision;
  m_widgetPreviewsByBlock.clear();
  if (m_widgetPreviews.isEmpty()) {
    return;
  }

  for (int i = 0; i < m_widgetPreviews.size(); ++i) {
    const QTextBlock block = blockForSpec(m_widgetPreviews[i]);
    if (!block.isValid()) {
      continue;
    }

    m_widgetPreviewsByBlock[block.blockNumber()].append(i);
  }

  // Stack by source start, then type, then identity.
  const auto &specs = m_widgetPreviews;
  for (auto it = m_widgetPreviewsByBlock.begin(); it != m_widgetPreviewsByBlock.end(); ++it) {
    std::sort(it.value().begin(), it.value().end(), [&specs](int p_a, int p_b) {
      const auto &a = specs[p_a];
      const auto &b = specs[p_b];
      if (a.m_startPos != b.m_startPos) {
        return a.m_startPos < b.m_startPos;
      }
      if (a.m_typeOrder != b.m_typeOrder) {
        return a.m_typeOrder < b.m_typeOrder;
      }
      return a.m_id < b.m_id;
    });
  }
}

QVector<const TextDocumentLayout::WidgetPreviewSpec *>
TextDocumentLayout::widgetSpecsForBlock(const QTextBlock &p_block, PreviewPlacement p_placement) {
  QVector<const WidgetPreviewSpec *> result;
  if (m_widgetPreviews.isEmpty()) {
    return result;
  }

  ensureWidgetPreviewMap();

  auto it = m_widgetPreviewsByBlock.constFind(p_block.blockNumber());
  if (it == m_widgetPreviewsByBlock.constEnd()) {
    return result;
  }

  for (int idx : it.value()) {
    const auto &spec = m_widgetPreviews[idx];
    if (spec.m_placement == p_placement) {
      result.append(&spec);
    }
  }

  return result;
}

bool TextDocumentLayout::isPreviewClaimed(int p_start, int p_end, PreviewElementType p_type) const {
  for (const auto &claim : m_claimedPreviews) {
    if (claim.m_startPos >= p_end) {
      // Claims are sorted by start position.
      break;
    }

    if (claim.m_type == p_type && claim.m_endPos > p_start) {
      return true;
    }
  }

  return false;
}

// The painted preview sources map one to one onto the interactive element
// types; a Table claim has no painted counterpart and therefore suppresses
// nothing. The table itself lives in the header, so the folding provider's
// reverse lookup cannot drift away from this one.

QVector<PreviewData *> TextDocumentLayout::unclaimedPreviewData(const QTextBlock &p_block) const {
  const auto &data = BlockPreviewData::get(p_block)->getPreviewData();
  if (m_claimedPreviews.isEmpty()) {
    return data;
  }

  const int blockPos = p_block.position();
  QVector<PreviewData *> result;
  result.reserve(data.size());
  for (auto item : data) {
    auto image = item ? item->getImageData() : nullptr;
    PreviewElementType type = PreviewElementType::Image;
    if (image && previewSourceToElementType(item->source(), &type) &&
        isPreviewClaimed(blockPos + image->m_startPos, blockPos + image->m_endPos, type)) {
      continue;
    }

    result.append(item);
  }

  return result;
}

void TextDocumentLayout::setWidgetPreviews(const QVector<WidgetPreviewSpec> &p_specs) {
  PassGuard pass(this);

  if (m_widgetPreviews == p_specs) {
    return;
  }

  // Only the specs which actually changed can move any geometry, so relayout
  // their blocks instead of every block holding a reservation.
  OrderedIntSet affectedBlocks;
  collectSpecDeltaBlocks(m_widgetPreviews, p_specs, affectedBlocks);

  m_widgetPreviews = p_specs;
  m_widgetPreviewMapDirty = true;

  if (affectedBlocks.isEmpty()) {
    ensureWidgetPreviewMap();
    updateWidgetPreviewGeometry();
    return;
  }

  relayout(affectedBlocks);
}

// Both vectors are sorted by identity, so one merge walk yields the specs
// present in only one of them or differing between them.
void TextDocumentLayout::collectSpecDeltaBlocks(const QVector<WidgetPreviewSpec> &p_old,
                                                const QVector<WidgetPreviewSpec> &p_new,
                                                OrderedIntSet &p_blocks) {
  int oldIdx = 0;
  int newIdx = 0;
  while (oldIdx < p_old.size() || newIdx < p_new.size()) {
    if (newIdx >= p_new.size()) {
      collectBlocksForSpec(p_old[oldIdx++], p_blocks);
    } else if (oldIdx >= p_old.size()) {
      collectBlocksForSpec(p_new[newIdx++], p_blocks);
    } else if (p_old[oldIdx].m_id < p_new[newIdx].m_id) {
      collectBlocksForSpec(p_old[oldIdx++], p_blocks);
    } else if (p_new[newIdx].m_id < p_old[oldIdx].m_id) {
      collectBlocksForSpec(p_new[newIdx++], p_blocks);
    } else {
      if (p_old[oldIdx] != p_new[newIdx]) {
        collectBlocksForSpec(p_old[oldIdx], p_blocks);
        collectBlocksForSpec(p_new[newIdx], p_blocks);
      }
      ++oldIdx;
      ++newIdx;
    }
  }
}

QTextBlock TextDocumentLayout::blockForSpec(const WidgetPreviewSpec &p_spec) const {
  QTextDocument *doc = document();
  if (!doc) {
    return QTextBlock();
  }

  // BlockAfterSource anchors after the block containing endPos - 1.
  const int anchorPos = p_spec.m_placement == PreviewPlacement::BlockAfterSource
                            ? qMax(p_spec.m_startPos, p_spec.m_endPos - 1)
                            : p_spec.m_startPos;
  if (anchorPos < 0 || anchorPos > doc->characterCount() - 1) {
    return QTextBlock();
  }

  return doc->findBlock(anchorPos);
}

void TextDocumentLayout::collectBlocksForSpec(const WidgetPreviewSpec &p_spec,
                                              OrderedIntSet &p_blocks) {
  const QTextBlock block = blockForSpec(p_spec);
  if (block.isValid()) {
    p_blocks.insert(block.blockNumber(), QMapDummyValue());
  }
}

const QVector<TextDocumentLayout::WidgetPreviewSpec> &TextDocumentLayout::widgetPreviews() const {
  return m_widgetPreviews;
}

void TextDocumentLayout::setPreviewClaims(const QVector<PreviewClaim> &p_claims) {
  PassGuard pass(this);

  if (m_claimedPreviews == p_claims) {
    return;
  }

  // Only the blocks covered by the claim delta can change their painted
  // preview, so removing a claim restores the static fallback immediately
  // without relayouting every claimed block in the document.
  OrderedIntSet affectedBlocks;
  collectClaimDeltaBlocks(m_claimedPreviews, p_claims, affectedBlocks);

  m_claimedPreviews = p_claims;

  if (affectedBlocks.isEmpty()) {
    return;
  }

  relayout(affectedBlocks);
}

// Both vectors are sorted, so one merge walk yields the symmetric difference.
void TextDocumentLayout::collectClaimDeltaBlocks(const QVector<PreviewClaim> &p_old,
                                                 const QVector<PreviewClaim> &p_new,
                                                 OrderedIntSet &p_blocks) {
  int oldIdx = 0;
  int newIdx = 0;
  while (oldIdx < p_old.size() || newIdx < p_new.size()) {
    if (newIdx >= p_new.size()) {
      collectBlocksForClaim(p_old[oldIdx++], p_blocks);
    } else if (oldIdx >= p_old.size()) {
      collectBlocksForClaim(p_new[newIdx++], p_blocks);
    } else if (p_old[oldIdx] < p_new[newIdx]) {
      collectBlocksForClaim(p_old[oldIdx++], p_blocks);
    } else if (p_new[newIdx] < p_old[oldIdx]) {
      collectBlocksForClaim(p_new[newIdx++], p_blocks);
    } else {
      ++oldIdx;
      ++newIdx;
    }
  }
}

void TextDocumentLayout::collectBlocksForClaim(const PreviewClaim &p_claim,
                                               OrderedIntSet &p_blocks) {
  QTextDocument *doc = document();
  const int maxPos = doc->characterCount() - 1;
  const int start = qBound(0, p_claim.m_startPos, maxPos);
  const int end = qBound(start, p_claim.m_endPos, maxPos);
  QTextBlock block = doc->findBlock(start);
  while (block.isValid()) {
    p_blocks.insert(block.blockNumber(), QMapDummyValue());
    if (block.position() + block.length() > end) {
      break;
    }
    block = block.next();
  }
}

QRectF TextDocumentLayout::widgetPreviewRect(quint64 p_id) const {
  return m_widgetPreviewGeometry.value(p_id);
}

qreal TextDocumentLayout::availableContentWidth() const {
  const qreal pageWidth = document()->pageSize().width();
  if (pageWidth <= 0) {
    return 0;
  }

  return qMax<qreal>(0, pageWidth - (2 * m_margin + m_cursorMargin + c_cursorGeometryWidth));
}

QRectF TextDocumentLayout::sourceTextRect(int p_startPos, int p_endPos) const {
  QTextDocument *doc = document();
  if (p_startPos < 0 || p_endPos <= p_startPos) {
    return QRectF();
  }

  const int maxPos = doc->characterCount() - 1;
  if (p_startPos > maxPos) {
    return QRectF();
  }

  QRectF result;
  QTextBlock block = doc->findBlock(p_startPos);
  while (block.isValid() && block.position() < p_endPos) {
    if (!block.isVisible()) {
      block = block.next();
      continue;
    }

    auto info = BlockLayoutData::get(block);
    if (!info->hasOffset()) {
      block = block.next();
      continue;
    }

    QTextLayout *layout = block.layout();
    const int blockPos = block.position();
    const int localStart = qMax(0, p_startPos - blockPos);
    const int localEnd = qMin(block.length() - 1, p_endPos - blockPos);
    for (int i = 0; i < layout->lineCount(); ++i) {
      QTextLine line = layout->lineAt(i);
      const int lineStart = line.textStart();
      const int lineEnd = lineStart + line.textLength();
      if (lineEnd <= localStart || lineStart >= localEnd) {
        continue;
      }

      const qreal x1 = line.cursorToX(qMax(lineStart, localStart));
      const qreal x2 = line.cursorToX(qMin(lineEnd, localEnd));
      QRectF lineRect(qMin(x1, x2), line.y() + info->m_offset, qAbs(x2 - x1), line.height());
      result = result.isNull() ? lineRect : result.united(lineRect);
    }

    block = block.next();
  }

  return result;
}

bool TextDocumentLayout::lineClaimsRange(const QTextLine &p_line, int p_start, int p_end,
                                         bool p_positioned, qreal *p_startX, qreal *p_endX) const {
  const int lineStart = p_line.textStart();
  const int lineEnd = lineStart + p_line.textLength();
  if (p_end <= lineStart || p_start >= lineEnd) {
    return false;
  }

  const bool startsHere = p_start >= lineStart;
  const bool endsHere = p_end <= lineEnd;
  if (startsHere && !endsHere && lineEnd - p_start < ((p_end - p_start) >> 1)) {
    // The range crosses the line boundary and this side owns less than half of
    // it, so the following line claims it instead.
    return false;
  }

  // While the block is still being laid out QTextLine::x() is 0, so the
  // document margin has to be added by hand; on a finished layout
  // cursorToX() already carries it.
  const qreal margin = p_positioned ? 0 : m_margin;
  const qreal startX = (startsHere ? p_line.cursorToX(p_start) : p_line.x()) + margin;
  const qreal endX = (endsHere ? p_line.cursorToX(p_end) : p_line.x() + p_line.width()) + margin;

  if (p_startX) {
    *p_startX = startX;
  }
  if (p_endX) {
    *p_endX = qMax(startX, endX);
  }

  return true;
}

qreal TextDocumentLayout::inlinePlacementWidth(int p_startPos, int p_endPos) const {
  QTextDocument *doc = document();
  if (p_startPos < 0 || p_endPos <= p_startPos || p_startPos > doc->characterCount() - 1) {
    return 0;
  }

  QTextBlock block = doc->findBlock(p_startPos);
  if (!block.isValid() || !block.isVisible()) {
    return 0;
  }

  QTextLayout *layout = block.layout();
  const int blockPos = block.position();
  const int start = p_startPos - blockPos;
  const int end = p_endPos - blockPos;

  for (int i = 0; i < layout->lineCount(); ++i) {
    qreal startX = 0;
    qreal endX = 0;
    if (lineClaimsRange(layout->lineAt(i), start, end, true, &startX, &endX)) {
      return qMax<qreal>(0, endX - startX);
    }
  }

  return 0;
}

void TextDocumentLayout::updateWidgetPreviewGeometry() {
  // The emitter itself is guarded, not just its callers: setWidgetPreviews()'s
  // empty-delta path calls it directly, outside every other pass scope. This
  // is what makes "every widgetPreviewGeometryChanged() observes isBusy()"
  // true regardless of who called.
  PassGuard pass(this);

  QHash<quint64, QRectF> geometry;
  if (!m_widgetPreviews.isEmpty()) {
    ensureWidgetPreviewMap();

    QTextDocument *doc = document();
    for (auto it = m_widgetPreviewsByBlock.constBegin(); it != m_widgetPreviewsByBlock.constEnd();
         ++it) {
      QTextBlock block = doc->findBlockByNumber(it.key());
      if (!block.isValid() || !block.isVisible()) {
        qCDebug(layoutGeometryLog)
            << "  block" << it.key() << "holds a reservation but is"
            << (block.isValid() ? "folded" : "gone") << "- its widgets are unpublished";
        continue;
      }

      auto info = BlockLayoutData::get(block);
      if (!info->hasOffset() || info->m_widgets.isEmpty()) {
        qCDebug(layoutGeometryLog) << "  block" << it.key() << "holds a reservation but has"
                                   << (info->hasOffset() ? "no reserved band" : "no offset")
                                   << "- its widgets are unpublished";
        continue;
      }

      for (const auto &widget : info->m_widgets) {
        // The rects are already in block coordinates (the left margin is baked
        // into them, like the line positions), so only the vertical block
        // offset is applied here.
        const QRectF docRect = widget.m_rect.translated(0, info->m_offset);
        geometry.insert(widget.m_id, docRect);

        qCDebug(layoutGeometryLog)
            << "  widget" << widget.m_id << "block" << it.key() << "offset" << info->m_offset
            << "blockHeight" << info->m_rect.height() << "band" << widget.m_rect << "->" << docRect;
      }
    }
  }

  if (geometry == m_widgetPreviewGeometry) {
    qCDebug(layoutGeometryLog) << "widget geometry unchanged," << geometry.size() << "entry(ies)";
    return;
  }

  if (layoutGeometryLog().isDebugEnabled()) {
    for (auto it = geometry.constBegin(); it != geometry.constEnd(); ++it) {
      const auto oldIt = m_widgetPreviewGeometry.constFind(it.key());
      if (oldIt == m_widgetPreviewGeometry.constEnd()) {
        qCDebug(layoutGeometryLog) << "widget" << it.key() << "appeared at" << it.value();
      } else if (oldIt.value() != it.value()) {
        qCDebug(layoutGeometryLog)
            << "widget" << it.key() << "moved" << oldIt.value() << "->" << it.value();
      }
    }

    for (auto it = m_widgetPreviewGeometry.constBegin(); it != m_widgetPreviewGeometry.constEnd();
         ++it) {
      if (!geometry.contains(it.key())) {
        qCDebug(layoutGeometryLog) << "widget" << it.key() << "disappeared from" << it.value();
      }
    }
  }

  m_widgetPreviewGeometry = geometry;
  emit widgetPreviewGeometryChanged();
}
