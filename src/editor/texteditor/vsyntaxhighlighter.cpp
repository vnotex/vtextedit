#include "vsyntaxhighlighter.h"

#include "blockspellcheckdata.h"

using namespace vte;

VSyntaxHighlighter::VSyntaxHighlighter(QTextDocument *p_doc)
    : QSyntaxHighlighter(p_doc)
{
}

void VSyntaxHighlighter::highlightMisspell(const QSharedPointer<BlockSpellCheckData> &p_data)
{
    for (const auto &seg : p_data->m_misspellings) {
        auto format = QSyntaxHighlighter::format(seg.m_offset);
        format.setFontUnderline(true);
        format.setUnderlineStyle(QTextCharFormat::SpellCheckUnderline);
        format.setUnderlineColor(Qt::red);
        setFormat(seg.m_offset, seg.m_length, format);
    }
}

void VSyntaxHighlighter::setSpellCheckEnabled(bool p_enabled)
{
    if (m_spellCheckEnabled == p_enabled) {
        return;
    }
    m_spellCheckEnabled = p_enabled;
    rehighlight();
}

void VSyntaxHighlighter::setAutoDetectLanguageEnabled(bool p_enabled)
{
    if (m_autoDetectLanguageEnabled == p_enabled) {
        return;
    }
    m_autoDetectLanguageEnabled = p_enabled;
    rehighlight();
}
