#include "ksyntaxcodeblockhighlighter.h"

#include <Repository>
#include <FoldingRegion>
#include <Format>
#include <State>

#include <utils/textutils.h>
#include <vtextedit/markdownutils.h>
#include <texteditor/ksyntaxhighlighterwrapper.h>

using namespace vte;

KSyntaxCodeBlockHighlighter::KSyntaxCodeBlockHighlighter(const QString &p_theme, QObject *p_parent)
    : CodeBlockHighlighter(p_parent)
{
    auto formatFunctor = [this](int p_offset,
                                int p_length,
                                const KSyntaxHighlighting::Format &p_format) {
        applyFormat(p_offset, p_length, p_format);
    };

    auto foldingFunctor = [](int p_offset,
                             int p_length,
                             KSyntaxHighlighting::FoldingRegion p_region) {
        Q_UNUSED(p_offset);
        Q_UNUSED(p_length);
        Q_UNUSED(p_region);
    };

    m_syntaxHighlighter = new KSyntaxHighlighterWrapper(formatFunctor,
                                                        foldingFunctor,
                                                        this);

    KSyntaxHighlighting::Theme th;
    if (!p_theme.isEmpty()) {
        th = KSyntaxHighlighterWrapper::repository()->theme(p_theme);
    }
    if (!th.isValid()) {
        th = KSyntaxHighlighterWrapper::repository()->defaultTheme();
    }
    m_syntaxHighlighter->setTheme(th);
}

void KSyntaxCodeBlockHighlighter::highlightInternal(int p_idx)
{
    const auto &block = m_codeBlocks[p_idx];
    if (block.m_lang.isEmpty()) {
        finishHighlightOne(HighlightResult(m_timeStamp, p_idx));
        return;
    }

    // Detect whether the syntax is supported.
    auto def = KSyntaxHighlighterWrapper::definitionForSyntax(block.m_lang);
    if (!def.isValid()) {
        // Do not highlight this.
        finishHighlightOne(HighlightResult(m_timeStamp, p_idx));
        return;
    }

    auto lines = block.m_text.split(QLatin1Char('\n'));
    if (lines.size() < 3) {
        // Empty code block.
        finishHighlightOne(HighlightResult(m_timeStamp, p_idx));
        return;
    }

    m_currentInfo.startNewHighlight(p_idx, lines.size());

    // Get the indentation of the code block.
    Q_ASSERT(MarkdownUtils::isFencedCodeBlockStartMark(lines[0]));
    int blockIndentation = TextUtils::fetchIndentation(lines[0]);

    m_syntaxHighlighter->setDefinition(def);
    KSyntaxHighlighting::State state;
    for (int i = 1; i < lines.size() - 1; ++i) {
        m_currentInfo.m_lineIndex = i;
        auto text = TextUtils::unindentText(lines[i], blockIndentation);
        m_currentInfo.m_indentation = lines[i].size() - text.size();
        state = m_syntaxHighlighter->highlightLine(text, state);
    }

    HighlightResult result(m_timeStamp, p_idx);
    result.m_highlights = m_currentInfo.m_highlights;
    finishHighlightOne(result);
}

void KSyntaxCodeBlockHighlighter::applyFormat(int p_offset,
                                              int p_length,
                                              const KSyntaxHighlighting::Format &p_format)
{
    if (p_length == 0) {
        return;
    }

    peg::HLUnitStyle unit;
    unit.start = p_offset + m_currentInfo.m_indentation;
    unit.length = p_length;
    unit.format = KSyntaxHighlighterWrapper::toTextCharFormat(m_syntaxHighlighter->theme(),
                                                              p_format);
    m_currentInfo.addHighlightUnit(unit);
}
