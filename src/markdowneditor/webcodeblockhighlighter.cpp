#include "webcodeblockhighlighter.h"

#include <QDebug>
#include <QXmlStreamReader>

#include <vtextedit/textutils.h>

using namespace vte;

WebCodeBlockHighlighter::ExternalCodeBlockHighlightStyles WebCodeBlockHighlighter::s_styles;

WebCodeBlockHighlighter::WebCodeBlockHighlighter(QObject *p_parent)
    : CodeBlockHighlighter(p_parent)
{
}

void WebCodeBlockHighlighter::highlightInternal(int p_idx)
{
    const auto &block = m_codeBlocks[p_idx];
    if (block.m_lang.isEmpty()) {
        finishHighlightOne(HighlightResult(m_timeStamp, p_idx));
        return;
    }

    const auto &unindentedText = TextUtils::unindentTextMultiLines(block.m_text);
    emit externalCodeBlockHighlightRequested(p_idx, m_timeStamp, unindentedText);
}

void WebCodeBlockHighlighter::handleExternalCodeBlockHighlightData(int p_idx, TimeStamp p_timeStamp, const QString &p_html)
{
    if (m_timeStamp != p_timeStamp) {
        return;
    }

    if (p_html.isEmpty()) {
        finishHighlightOne(HighlightResult(p_timeStamp, p_idx));
        return;
    }

    auto lines = m_codeBlocks[p_idx].m_text.split(QLatin1Char('\n'));
    Q_ASSERT(lines.size() > 2);

    HighlightResult hiRes(p_timeStamp, p_idx);
    hiRes.m_highlights.resize(lines.size());

    auto htmlLines = p_html.split(QLatin1Char('\n'));

    int lineIdx = 1;
    int lineOffset = 0;

    for (int htmlLineIdx = 0; htmlLineIdx < htmlLines.size(); ++htmlLineIdx) {
        parseXmlAndMatch(htmlLines[htmlLineIdx], lines, hiRes.m_highlights, lineIdx, lineOffset);
    }

    if (m_timeStamp != p_timeStamp) {
        return;
    }

    finishHighlightOne(hiRes);
}

void WebCodeBlockHighlighter::parseXmlAndMatch(const QString &p_html,
                                               const QStringList &p_lines,
                                               HighlightStyles &p_styles,
                                               int &p_idx,
                                               int &p_offset)
{
    if (p_html.isEmpty()) {
        return;
    }

    // Add a wrapper <span> here.
    QXmlStreamReader reader(QStringLiteral("<span>") + p_html + QStringLiteral("</span>"));

    bool failed = false;
    while (!failed) {
        auto type = reader.readNext();
        if (reader.atEnd()) {
            break;
        }

        switch (type) {
        case QXmlStreamReader::StartDocument:
            Q_FALLTHROUGH();
        case QXmlStreamReader::EndDocument:
            break;

        case QXmlStreamReader::StartElement:
        {
            if (reader.name() != QStringLiteral("span")) {
                qWarning() << "unknown start element" << reader.name();
                failed = true;
                break;
            }

            QStringList classList;
            failed = !parseSpanElement(reader, p_lines, p_styles, classList, p_idx, p_offset);
            break;
        }

        default:
            qWarning() << "unknown token" << reader.tokenString();
            failed = true;
            break;
        }
    }
}

bool WebCodeBlockHighlighter::parseSpanElement(QXmlStreamReader &p_reader,
                                               const QStringList &p_lines,
                                               HighlightStyles &p_styles,
                                               QStringList &p_classList,
                                               int &p_idx,
                                               int &p_offset)
{
    if (p_idx >= p_lines.size()) {
        return false;
    }

    const auto localClassList = p_reader.attributes().value(QStringLiteral("class")).toString().split(QLatin1Char(' '));
    int parentClassListCnt = p_classList.size();
    for (const auto cla : localClassList) {
        auto na = cla.trimmed();
        if (na.isEmpty()) {
            continue;
        }

        p_classList.append(na);
    }

    bool failed = false;
    bool completed = false;
    while (!failed && !completed) {
        auto type = p_reader.readNext();
        switch (type) {
        case QXmlStreamReader::Characters:
        {
            // Already unescaped.
            const auto tokenText = p_reader.text().toString();
            while (p_idx < p_lines.size()) {
                auto pos = p_lines[p_idx].indexOf(tokenText, p_offset);
                if (pos == -1) {
                    // Move to next line.
                    ++p_idx;
                    p_offset = 0;
                } else {
                    // Matched.
                    p_offset = pos + tokenText.size();
                    if (!p_classList.isEmpty()) {
                        // Translate class to styles.
                        p_styles[p_idx].push_back(peg::HLUnitStyle());
                        auto &unit = p_styles[p_idx].back();
                        unit.start = pos;
                        unit.length = tokenText.size();
                        unit.format = styleOfClasses(p_classList);
                    }
                    break;
                }
            }

            if (p_idx >= p_lines.size()) {
                // No matched.
                qWarning() << "mismatched token" << tokenText << p_lines;
                failed = true;
                break;
            }
            break;
        }

        case QXmlStreamReader::StartElement:
        {
            if (p_reader.name() != QStringLiteral("span")) {
                qWarning() << "unknown start element" << p_reader.name();
                failed = true;
                break;
            }

            // Embedded <span>.
            failed = !parseSpanElement(p_reader, p_lines, p_styles, p_classList, p_idx, p_offset);
            break;
        }

        case QXmlStreamReader::EndElement:
        {
            if (p_reader.name() != QStringLiteral("span")) {
                qWarning() << "mismatched end element" << p_reader.name();
                failed = true;
                break;
            }

            // Got a complete <span>.
            completed = true;
            break;
        }

        default:
            qWarning() << "unknown token" << p_reader.tokenString();
            failed = true;
            break;
        }
    }

    p_classList.erase(p_classList.begin() + parentClassListCnt, p_classList.end());
    return !failed;
}

QTextCharFormat WebCodeBlockHighlighter::styleOfClasses(const QStringList &p_classList)
{
    QTextCharFormat fmt;
    for (const auto &cla : p_classList) {
        if (cla == QStringLiteral("token")) {
            continue;
        }
        auto it = s_styles.find(cla);
        if (it != s_styles.end()) {
            fmt.merge(it.value());
        }
    }
    return fmt;
}

void WebCodeBlockHighlighter::setExternalCodeBlockHighlihgtStyles(const ExternalCodeBlockHighlightStyles &p_styles)
{
    s_styles = p_styles;
}
