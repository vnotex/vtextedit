#include "typsthighlighter.h"

#include <QTextBlock>
#include <QTextDocument>

#include <spellcheck/spellcheckhighlighthelper.h>
#include <texteditor/blockspellcheckdata.h>
#include <vtextedit/textblockdata.h>
#include <vtextedit/texteditorconfig.h>
#include <vtextedit/theme.h>

using namespace vte;

TypstHighlighter::TypstHighlighter(QTextDocument *p_document, const TextEditorConfig &p_config)
    : VSyntaxHighlighter(p_document) {
  m_refreshTimer.setSingleShot(true);
  connect(&m_refreshTimer, &QTimer::timeout, this, [this]() {
    rehighlight();
    emit highlightingUpdated();
  });
  connect(p_document, &QTextDocument::contentsChange, this,
          [this](int, int p_removed, int p_added) {
            if (p_removed || p_added) {
              invalidate();
            }
          });
  refreshTheme(p_config);
}

void TypstHighlighter::setGeneration(quint64 p_generation) {
  // Qt rehighlighting advances the document revision without changing source.
  // Only source edits and compiler generations invalidate parser ranges.
  if (m_generation != p_generation) {
    invalidate();
  }
  m_generation = p_generation;
  m_acceptHighlights = p_generation != 0;
}

void TypstHighlighter::invalidate() {
  m_acceptHighlights = false;
  if (!m_blocks.isEmpty()) {
    m_blocks.clear();
    // Never re-enter the document's contentsChange/layout stack.
    m_refreshTimer.start(0);
  }
}

void TypstHighlighter::setHighlights(quint64 p_generation,
                                     const QVector<TypstHighlight> &p_highlights,
                                     const QString &p_activeFile) {
  if (!m_acceptHighlights || p_generation != m_generation) {
    return;
  }
  m_blocks.clear();
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
    for (auto block = doc->findBlock(source.m_start);
         block.isValid() && block.position() < source.m_end; block = block.next()) {
      const int start = qMax(source.m_start, block.position());
      const int end = qMin(source.m_end, block.position() + block.length() - 1);
      if (end > start) {
        m_blocks[block.blockNumber()].append({start - block.position(), end - start, kind});
      }
    }
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
  if (m_acceptHighlights) {
    const auto ranges = m_blocks.constFind(block.blockNumber());
    if (ranges != m_blocks.constEnd()) {
      for (const auto &range : ranges.value()) {
        const auto format = m_formats.constFind(range.m_kind);
        if (format != m_formats.constEnd()) {
          setFormat(range.m_start, range.m_length, format.value());
        }
      }
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
