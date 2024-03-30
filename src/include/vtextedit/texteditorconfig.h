#ifndef VTEXTEDIT_TEXTEDITORCONFIG_H
#define VTEXTEDIT_TEXTEDITORCONFIG_H

#include <QStringList>
#include <QSharedPointer>

#include "vtexteditor.h"
#include "theme.h"
#include "global.h"

namespace vte
{
    class ViConfig;

    class VTEXTEDIT_EXPORT TextEditorConfig
    {
    public:
        TextEditorConfig() = default;

        static QSharedPointer<Theme> defaultTheme();

        VTextEditor::LineNumberType m_lineNumberType = VTextEditor::LineNumberType::Absolute;

        bool m_textFoldingEnabled = true;

        QSharedPointer<Theme> m_theme;

        // Theme for syntax highlight.
        // Could be a theme name or a file path to the theme file.
        QString m_syntaxTheme;

        InputMode m_inputMode = InputMode::NormalMode;

        // Screen scale factor.
        qreal m_scaleFactor = 1;

        // Whether center the cursor.
        CenterCursor m_centerCursor = CenterCursor::NeverCenter;

        // Word wrap mode.
        WrapMode m_wrapMode = WrapMode::WordWrapOrAnywhere;

        // Whether expand Tab into spaces.
        bool m_expandTab = true;

        // How many spaces with a Tab be translated into.
        int m_tabStopWidth = 4;

        QSharedPointer<ViConfig> m_viConfig;

        LineEndingPolicy m_lineEndingPolicy = LineEndingPolicy::LF;

        // Highlight trailing space and tab.
        // TODO: for current implementation, this feature has perf issue.
        bool m_highlightWhitespace = false;
    };


    // Set only on construction.
    struct VTEXTEDIT_EXPORT TextEditorParameters
    {
        bool m_spellCheckEnabled = false;

        bool m_autoDetectLanguageEnabled = false;

        QString m_defaultSpellCheckLanguage = QStringLiteral("en_US");
    };
}

#endif
