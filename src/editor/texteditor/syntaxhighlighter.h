#ifndef SYNTAXHIGHLIGHTER_H
#define SYNTAXHIGHLIGHTER_H

#include <QSyntaxHighlighter>

#include <AbstractHighlighter>
#include <Definition>
#include <QHash>
#include <QSharedPointer>

class QTextDocument;

namespace vte
{
    struct BlockSpellCheckData;

    class SyntaxHighlighter : public QSyntaxHighlighter,
                              public KSyntaxHighlighting::AbstractHighlighter
    {
        Q_OBJECT
        Q_INTERFACES(KSyntaxHighlighting::AbstractHighlighter)
    public:
        // @p_theme: a theme file path or a theme name.
        SyntaxHighlighter(QTextDocument *p_doc,
                          const QString &p_theme,
                          const QString &p_syntax);

        void setSpellCheckEnabled(bool p_enabled);

        static bool isValidSyntax(const QString &p_syntax);

    protected:
        void highlightBlock(const QString &p_text) Q_DECL_OVERRIDE;

        void applyFormat(int p_offset,
                         int p_length,
                         const KSyntaxHighlighting::Format &p_format) Q_DECL_OVERRIDE;

        void applyFolding(int p_offset,
                          int p_length,
                          KSyntaxHighlighting::FoldingRegion p_region) Q_DECL_OVERRIDE;

    private:
        void highlightMisspell(const QSharedPointer<BlockSpellCheckData> &p_data);

        // Will be set and cleared within highlightBlock().
        QHash<int, int> m_pendingFoldingStart;

        bool m_spellCheckEnabled = false;
    };
}

#endif // SYNTAXHIGHLIGHTER_H
