#ifndef WEBCODEBLOCKHIGHLIGHTER_H
#define WEBCODEBLOCKHIGHLIGHTER_H

#include <vtextedit/codeblockhighlighter.h>

class QXmlStreamReader;

namespace vte
{
    class WebCodeBlockHighlighter : public CodeBlockHighlighter
    {
        Q_OBJECT
    public:
        typedef QHash<QString, QTextCharFormat> ExternalCodeBlockHighlightStyles;

        explicit WebCodeBlockHighlighter(QObject *p_parent);

        void handleExternalCodeBlockHighlightData(int p_idx, TimeStamp p_timeStamp, const QString &p_html);

        static void setExternalCodeBlockHighlihgtStyles(const ExternalCodeBlockHighlightStyles &p_styles);

    signals:
        void externalCodeBlockHighlightRequested(int p_idx, TimeStamp p_timeStamp, const QString &p_text);

    protected:
        // @p_idx Index in m_codeBlocks.
        void highlightInternal(int p_idx) Q_DECL_OVERRIDE;

    private:
        static QTextCharFormat styleOfClasses(const QStringList &p_classList);

        static void parseXmlAndMatch(const QString &p_html,
                                     const QStringList &p_lines,
                                     HighlightStyles &p_styles,
                                     int &p_idx,
                                     int &p_offset);

        // Return true on success.
        static bool parseSpanElement(QXmlStreamReader &p_reader,
                                     const QStringList &p_lines,
                                     HighlightStyles &p_styles,
                                     QStringList &p_classList,
                                     int &p_idx,
                                     int &p_offset);

        static ExternalCodeBlockHighlightStyles s_styles;
    };
}

#endif // WEBCODEBLOCKHIGHLIGHTER_H
