#ifndef VTEXTEDIT_MARKDOWNEDITORCONFIG_H
#define VTEXTEDIT_MARKDOWNEDITORCONFIG_H

#include "global.h"
#include "texteditorconfig.h"

namespace vte
{
    class VTEXTEDIT_EXPORT MarkdownEditorConfig
    {
    public:
        MarkdownEditorConfig(const QSharedPointer<TextEditorConfig> &p_textEditorConfig);

        void fillDefaultTheme();

        QSharedPointer<TextEditorConfig> m_textEditorConfig;

        // Whether constrain the width of in-place preview.
        bool m_constrainInPlacePreviewWidthEnabled = false;

    private:
        void overrideTextStyle();
    };
}

#endif // MARKDOWNEDITORCONFIG_H
