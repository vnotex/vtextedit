#include "typsthighlighter.h"

#include <QTextBlock>
#include <QTextDocument>
#include <Repository>
#include <Theme>

#include <spellcheck/spellcheckhighlighthelper.h>
#include <texteditor/blockspellcheckdata.h>
#include <texteditor/ksyntaxhighlighterwrapper.h>
#include <utils/utils.h>
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
  if (m_generation != p_generation || m_revision != document()->revision()) {
    invalidate();
  }
  m_generation = p_generation;
  m_revision = document()->revision();
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
  if (!m_acceptHighlights || p_generation != m_generation || m_revision != document()->revision()) {
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
        !boundary(source.m_start) || !boundary(source.m_end) || !m_formats.contains(kind)) {
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

void TypstHighlighter::refreshTheme(const TextEditorConfig &p_config) {
  using SyntaxTheme = KSyntaxHighlighting::Theme;
  KSyntaxHighlighterWrapper::Initialize({});
  auto repository = KSyntaxHighlighterWrapper::repository();
  SyntaxTheme syntaxTheme;
  if (!p_config.m_syntaxTheme.isEmpty()) {
    syntaxTheme = Utils::isFilePath(p_config.m_syntaxTheme)
                      ? repository->themeFromFile(p_config.m_syntaxTheme)
                      : repository->theme(p_config.m_syntaxTheme);
  }
  if (!syntaxTheme.isValid()) {
    syntaxTheme = repository->defaultTheme();
  }
  const auto theme = p_config.m_theme ? p_config.m_theme : TextEditorConfig::defaultTheme();
  QTextCharFormat base = theme->editorStyle(Theme::Text).toTextCharFormat();
  // Inherit editor geometry: absolute syntax font sizes would defeat zoom.
  base.clearProperty(QTextFormat::FontPointSize);
  m_formats.clear();
  const auto add = [this, &syntaxTheme, &base](const char *p_kind, SyntaxTheme::TextStyle p_style) {
    QTextCharFormat format = base;
    if (syntaxTheme.textColor(p_style)) {
      format.setForeground(QColor(syntaxTheme.textColor(p_style)));
    }
    if (syntaxTheme.backgroundColor(p_style)) {
      format.setBackground(QColor(syntaxTheme.backgroundColor(p_style)));
    }
    format.setFontWeight(syntaxTheme.isBold(p_style) ? QFont::Bold : QFont::Normal);
    format.setFontItalic(syntaxTheme.isItalic(p_style));
    format.setFontUnderline(syntaxTheme.isUnderline(p_style));
    format.setFontStrikeOut(syntaxTheme.isStrikeThrough(p_style));
    m_formats.insert(QString::fromLatin1(p_kind), format);
  };
  add("comment", SyntaxTheme::Comment);
  add("punctuation", SyntaxTheme::Operator);
  add("escape", SyntaxTheme::SpecialChar);
  add("strong", SyntaxTheme::Normal);
  add("emph", SyntaxTheme::Normal);
  add("link", SyntaxTheme::Annotation);
  add("raw", SyntaxTheme::VerbatimString);
  add("label", SyntaxTheme::Attribute);
  add("ref", SyntaxTheme::Annotation);
  add("heading", SyntaxTheme::Keyword);
  add("listmarker", SyntaxTheme::Operator);
  add("listterm", SyntaxTheme::Attribute);
  add("mathdelimiter", SyntaxTheme::SpecialString);
  add("mathoperator", SyntaxTheme::Operator);
  add("mathgroupingparens", SyntaxTheme::Operator);
  add("keyword", SyntaxTheme::Keyword);
  add("operator", SyntaxTheme::Operator);
  add("number", SyntaxTheme::Float);
  add("string", SyntaxTheme::String);
  add("function", SyntaxTheme::Function);
  add("interpolated", SyntaxTheme::Variable);
  add("error", SyntaxTheme::Error);
  m_formats[QStringLiteral("strong")].setFontWeight(QFont::Bold);
  m_formats[QStringLiteral("emph")].setFontItalic(true);
  m_formats[QStringLiteral("heading")].setFontWeight(QFont::Bold);
  m_formats[QStringLiteral("link")].setFontUnderline(true);
  m_refreshTimer.start(0);
}

void TypstHighlighter::highlightBlock(const QString &p_text) {
  const auto block = currentBlock();
  auto data = TextBlockData::get(block);
  if (m_acceptHighlights && m_revision == document()->revision()) {
    const auto ranges = m_blocks.constFind(block.blockNumber());
    if (ranges != m_blocks.constEnd()) {
      for (const auto &range : ranges.value()) {
        setFormat(range.m_start, range.m_length, m_formats.value(range.m_kind));
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
