#include <vtextedit/markdowneditorconfig.h>

using namespace vte;

MarkdownEditorConfig::MarkdownEditorConfig(const QSharedPointer<TextEditorConfig> &p_textEditorConfig)
    : m_textEditorConfig(p_textEditorConfig)
{
    Q_ASSERT(p_textEditorConfig);
    fillDefaultTheme();
    overrideTextStyle();
}

void MarkdownEditorConfig::fillDefaultTheme()
{
    static QSharedPointer<Theme> theme = nullptr;
    if (!m_textEditorConfig->m_theme) {
        if (!theme) {
            theme = TextEditorConfig::defaultTheme();
        }

        m_textEditorConfig->m_theme = theme;
    }
}

void MarkdownEditorConfig::overrideTextStyle()
{
    auto &theme = m_textEditorConfig->m_theme;
    Q_ASSERT(theme);
    const auto &markdownEditorStyles = theme->markdownEditorStyles();
    for (auto it = markdownEditorStyles.begin(); it != markdownEditorStyles.end(); ++it) {
        theme->editorStyle(it.key()) = it.value();
    }
}
