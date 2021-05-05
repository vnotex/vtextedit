#ifndef VSYNTAXHIGHLIGHTER_H
#define VSYNTAXHIGHLIGHTER_H

#include <QSyntaxHighlighter>
#include <QSharedPointer>

namespace vte
{
    struct BlockSpellCheckData;

    class VSyntaxHighlighter : public QSyntaxHighlighter
    {
        Q_OBJECT
    public:
        explicit VSyntaxHighlighter(QTextDocument *p_doc);

        virtual ~VSyntaxHighlighter() = default;

        void setSpellCheckEnabled(bool p_enabled);

        void setAutoDetectLanguageEnabled(bool p_enabled);

        virtual bool isSyntaxFoldingEnabled() const = 0;

    protected:
        void highlightMisspell(const QSharedPointer<BlockSpellCheckData> &p_data);

        bool m_spellCheckEnabled = false;

        bool m_autoDetectLanguageEnabled = false;
    };
}

#endif // VSYNTAXHIGHLIGHTER_H
