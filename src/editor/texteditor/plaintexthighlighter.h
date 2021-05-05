#ifndef PLAINTEXTHIGHLIGHTER_H
#define PLAINTEXTHIGHLIGHTER_H

#include "vsyntaxhighlighter.h"

namespace vte
{
    // Highlighter for plain text.
    // Only for spell check highlighting for now.
    class PlainTextHighlighter : public VSyntaxHighlighter
    {
        Q_OBJECT
    public:
        explicit PlainTextHighlighter(QTextDocument *p_doc);

        bool isSyntaxFoldingEnabled() const Q_DECL_OVERRIDE;

    protected:
        void highlightBlock(const QString &p_text) Q_DECL_OVERRIDE;
    };
}

#endif // PLAINTEXTHIGHLIGHTER_H
