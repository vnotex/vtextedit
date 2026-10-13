#include "typsthighlighter.h"

#include <QTextBlock>
#include <QTextDocument>
#include <algorithm>

#include <spellcheck/spellcheckhighlighthelper.h>
#include <texteditor/blockspellcheckdata.h>
#include <vtextedit/textblockdata.h>
#include <vtextedit/texteditorconfig.h>
#include <vtextedit/theme.h>

using namespace vte;

TypstHighlighter::TypstHighlighter(QTextDocument *p_document, const TextEditorConfig &p_config)
    : VSyntaxHighlighter(nullptr) {
  setParent(p_document);
  m_refreshTimer.setSingleShot(true);
  connect(&m_refreshTimer, &QTimer::timeout, this, [this]() {
    rehighlight();
    emit highlightingUpdated();
  });
  // Map before Qt rehighlights the changed blocks, not on a later full refresh.
  connect(p_document, &QTextDocument::contentsChange, this,
          [this](int p_position, int p_removed, int p_added) {
            if (p_removed || p_added) {
              m_acceptHighlights = false;
              mapRanges(p_position, p_removed, p_added);
            }
          });
  setDocument(p_document);
  refreshTheme(p_config);
}

void TypstHighlighter::setGeneration(quint64 p_generation) {
  // Freshness gates incoming results, not the last displayed formatting.
  m_generation = p_generation;
  m_acceptHighlights = p_generation != 0;
}

void TypstHighlighter::invalidate() {
  m_acceptHighlights = false;
  if (!m_ranges.isEmpty()) {
    m_ranges.clear();
    // Never re-enter the document's contentsChange/layout stack.
    m_refreshTimer.start(0);
  }
}

void TypstHighlighter::mapRanges(int p_position, int p_removed, int p_added) {
  const int removedEnd = p_position + p_removed;
  const int delta = p_added - p_removed;
  int count = 0;
  int maxEnd = 0;
  for (int i = 0; i < m_ranges.size(); ++i) {
    auto &range = m_ranges[i];
    if (p_removed && p_position <= range.m_start && removedEnd >= range.m_end)
      continue;
    if (range.m_end > p_position) {
      if (range.m_start >= removedEnd) {
        range.m_start += delta;
        range.m_end += delta;
      } else {
        range.m_start = qMin(range.m_start, p_position);
        range.m_end = qMax(p_position + p_added, range.m_end + delta);
      }
    }
    if (range.m_end <= range.m_start)
      continue;
    maxEnd = qMax(maxEnd, range.m_end);
    range.m_maxEnd = maxEnd;
    if (count != i)
      m_ranges[count] = std::move(range);
    ++count;
  }
  m_ranges.resize(count);
}

void TypstHighlighter::setHighlights(quint64 p_generation,
                                     const QVector<TypstHighlight> &p_highlights,
                                     const QString &p_activeFile) {
  if (!m_acceptHighlights || p_generation != m_generation) {
    return;
  }
  m_ranges.clear();
  m_ranges.reserve(p_highlights.size());
  auto doc = document();
  const int limit = doc->characterCount() - 1;
  const auto boundary = [doc, limit](int p_pos) {
    return p_pos >= 0 && p_pos <= limit &&
           !(p_pos > 0 && p_pos < limit && doc->characterAt(p_pos).isLowSurrogate() &&
             doc->characterAt(p_pos - 1).isHighSurrogate());
  };
  for (const auto &highlight : p_highlights) {
    const auto &source = highlight.m_source;
    const QString kind = highlight.m_kind.toLower();
    if (source.m_file != p_activeFile || source.m_end <= source.m_start ||
        !boundary(source.m_start) || !boundary(source.m_end)) {
      continue;
    }
    m_ranges.append({source.m_start, source.m_end, 0, kind});
  }
  std::stable_sort(m_ranges.begin(), m_ranges.end(),
                   [](const Range &p_a, const Range &p_b) { return p_a.m_start < p_b.m_start; });
  int maxEnd = 0;
  for (auto &range : m_ranges) {
    maxEnd = qMax(maxEnd, range.m_end);
    range.m_maxEnd = maxEnd;
  }
  m_refreshTimer.stop();
  rehighlight();
  emit highlightingUpdated();
}

void TypstHighlighter::refreshTheme(const TextEditorConfig &p_config, int p_zoomDelta) {
  const auto theme = p_config.m_theme ? p_config.m_theme : TextEditorConfig::defaultTheme();
  QTextCharFormat base = theme->editorStyle(Theme::Text).toTextCharFormat();
  // Unspecified sizes inherit the editor font; explicit sizes follow its zoom.
  base.clearProperty(QTextFormat::FontPointSize);
  m_formats.clear();
  const auto &styles = theme->typstSyntaxStyles();
  for (auto it = styles.constBegin(); it != styles.constEnd(); ++it) {
    QTextCharFormat format = base;
    format.merge(it.value().toTextCharFormat());
    if (it.value().m_fontPointSize > 0) {
      format.setFontPointSize(qMax(2, it.value().m_fontPointSize + p_zoomDelta));
    }
    m_formats.insert(it.key(), format);
  }
  m_refreshTimer.start(0);
}

void TypstHighlighter::highlightBlock(const QString &p_text) {
  const auto block = currentBlock();
  auto data = TextBlockData::get(block);
  const int blockStart = block.position();
  const int blockEnd = blockStart + p_text.size();
  auto range = std::upper_bound(m_ranges.constBegin(), m_ranges.constEnd(), blockStart,
                                [](int p_start, const Range &p_range) {
                                  return p_start < p_range.m_maxEnd;
                                });
  for (; range != m_ranges.constEnd() && range->m_start < blockEnd; ++range) {
    const int start = qMax(blockStart, range->m_start);
    const int end = qMin(blockEnd, range->m_end);
    if (start < end) {
      const auto format = m_formats.constFind(range->m_kind);
      if (format != m_formats.constEnd())
        setFormat(start - blockStart, end - start, format.value());
    }
  }
  if (!p_text.isEmpty() && m_spellCheckEnabled &&
      SpellCheckHighlightHelper::checkBlock(block, p_text, m_autoDetectLanguageEnabled)) {
    const auto spellData = data->getBlockSpellCheckData();
    if (spellData && spellData->isValid(block.revision()) && !spellData->isEmpty()) {
      highlightMisspell(spellData);
    }
  }
}
