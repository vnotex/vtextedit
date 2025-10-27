#include <vtextedit/markdowneditorconfig.h>

using namespace vte;

MarkdownEditorConfig::MarkdownEditorConfig(
    const QSharedPointer<TextEditorConfig> &p_textEditorConfig)
    : m_textEditorConfig(p_textEditorConfig),
      m_inplacePreviewSources(InplacePreviewSource::ImageLink | InplacePreviewSource::CodeBlock |
                              InplacePreviewSource::Math) {
  Q_ASSERT(p_textEditorConfig);
  fillDefaultTheme();
  overrideTextStyle();
}

void MarkdownEditorConfig::fillDefaultTheme() {
  static QSharedPointer<Theme> theme = nullptr;
  if (!m_textEditorConfig->m_theme) {
    if (!theme) {
      theme = TextEditorConfig::defaultTheme();
    }

    m_textEditorConfig->m_theme = theme;
  }
}

void MarkdownEditorConfig::overrideTextStyle() {
  auto &theme = m_textEditorConfig->m_theme;
  Q_ASSERT(theme);
  const auto &markdownEditorStyles = theme->markdownEditorStyles();
  for (auto it = markdownEditorStyles.begin(); it != markdownEditorStyles.end(); ++it) {
    theme->editorStyle(it.key()) = it.value();
  }
}

void MarkdownEditorConfig::overrideTextFontFamily(const QString &p_fontFamily) {
  if (p_fontFamily.isEmpty()) {
    return;
  }

  auto &theme = m_textEditorConfig->m_theme;
  Q_ASSERT(theme);
  theme->editorStyle(Theme::EditorStyle::Text).m_fontFamily = p_fontFamily;
}
