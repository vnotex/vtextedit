#ifndef VTEXTEDIT_MARKDOWNEDITORCONFIG_H
#define VTEXTEDIT_MARKDOWNEDITORCONFIG_H

#include "global.h"
#include "texteditorconfig.h"

namespace vte
{
    class VTEXTEDIT_EXPORT MarkdownEditorConfig
    {
    public:
        enum InplacePreviewSource
        {
            NoInplacePreview = 0,
            ImageLink = 0x1,
            CodeBlock = 0x2,
            Math = 0x4
        };
        Q_DECLARE_FLAGS(InplacePreviewSources, InplacePreviewSource);

        MarkdownEditorConfig(const QSharedPointer<TextEditorConfig> &p_textEditorConfig);

        void fillDefaultTheme();

        // Override the font family of Text style.
        void overrideTextFontFamily(const QString &p_fontFamily);

        QSharedPointer<TextEditorConfig> m_textEditorConfig;

        // Whether constrain the width of in-place preview.
        bool m_constrainInplacePreviewWidthEnabled = false;

        InplacePreviewSources m_inplacePreviewSources;

    private:
        void overrideTextStyle();
    };
}

Q_DECLARE_OPERATORS_FOR_FLAGS(vte::MarkdownEditorConfig::InplacePreviewSources)

#endif // MARKDOWNEDITORCONFIG_H
