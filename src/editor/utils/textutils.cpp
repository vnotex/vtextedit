#include <vtextedit/textutils.h>

#include <QHash>

using namespace vte;

int TextUtils::firstNonSpace(const QString &p_text)
{
    for (int i = 0; i < p_text.size(); ++i) {
        if (!p_text.at(i).isSpace()) {
            return i;
        }
    }

    return -1;
}

int TextUtils::lastNonSpace(const QString &p_text)
{
    for (int i = p_text.size() - 1; i >= 0; --i) {
        if (!p_text.at(i).isSpace()) {
            return i;
        }
    }

    return -1;
}

int TextUtils::trailingWhitespaces(const QString &p_text)
{
    int idx = p_text.size() - 1;
    while (idx >= 0) {
        if (!p_text.at(idx).isSpace()) {
            break;
        }
        --idx;
    }
    return p_text.size() - 1 - idx;
}

int TextUtils::fetchIndentation(const QString &p_text)
{
    int idx = firstNonSpace(p_text);
    return idx == -1 ? p_text.size() : idx;
}

QString TextUtils::fetchIndentationSpaces(const QString &p_text)
{
    int indentation = fetchIndentation(p_text);
    return p_text.left(indentation);
}

QString TextUtils::fetchIndentationSpacesInMultiLines(const QString &p_text, int p_pos)
{
    int start = 0;
    if (p_pos != 0) {
        start = p_text.lastIndexOf(QLatin1Char('\n'), p_pos - 1);
        if (start == -1) {
            start = 0;
        } else {
            ++start;
        }
    }

    for (int i = start; i < p_pos; ++i) {
        if (!p_text.at(i).isSpace()) {
            return p_text.mid(start, i - start);
        }
    }

    return QString();
}

QString TextUtils::unindentText(const QString &p_text, int p_spaces)
{
    if (p_spaces == 0) {
        return p_text;
    }

    int idx = 0;
    while (idx < p_spaces && idx < p_text.size() && p_text[idx].isSpace()) {
        ++idx;
    }
    return p_text.right(p_text.size() - idx);
}

bool TextUtils::isSpace(const QString &p_text, int p_start, int p_end)
{
    int len = qMin(p_text.size(), p_end);
    for (int i = p_start; i < len; ++i) {
        if (!p_text[i].isSpace()) {
            return false;
        }
    }

    return true;
}

QString TextUtils::purifyUrl(const QString &p_url)
{
    int idx = p_url.indexOf('?');
    if (idx > -1) {
        return p_url.left(idx);
    }

    return p_url;
}

void TextUtils::decodeUrl(QString &p_url)
{
    static QHash<QString, QString> maps;
    if (maps.isEmpty()) {
        maps.insert("%20", " ");
    }

    for (auto it = maps.begin(); it != maps.end(); ++it) {
        p_url.replace(it.key(), it.value());
    }
}

QString TextUtils::removeCodeBlockFence(const QString &p_text)
{
    auto text = unindentTextMultiLines(p_text);
    Q_ASSERT(text.startsWith(QStringLiteral("```")) || text.startsWith(QStringLiteral("~~~")));
    int idx = text.indexOf(QLatin1Char('\n')) + 1;
    int lidx = text.size() - 1;
    // Trim spaces at the end.
    while (lidx >= 0 && text[lidx].isSpace()) {
        --lidx;
    }

    Q_ASSERT(text[lidx] == QLatin1Char('`') || text[lidx] == QLatin1Char('~'));
    return text.mid(idx, lidx + 1 - idx - 3);
}

QString TextUtils::unindentTextMultiLines(const QString &p_text)
{
    if (p_text.isEmpty()) {
        return p_text;
    }

    auto lines = p_text.split(QLatin1Char('\n'));
    Q_ASSERT(lines.size() > 0);

    const int indentation = fetchIndentation(lines[0]);
    if (indentation == 0) {
        return p_text;
    }

    QString res = lines[0].right(lines[0].size() - indentation);
    for (int i = 1; i < lines.size(); ++i) {
        const auto &line = lines[i];
        int idx = 0;
        while (idx < indentation && idx < line.size() && line[idx].isSpace()) {
            ++idx;
        }
        res = res + QLatin1Char('\n') + line.right(line.size() - idx);
    }

    return res;
}

bool TextUtils::isClosingBracket(const QChar &p_char)
{
    return p_char == QLatin1Char(')')
           || p_char == QLatin1Char(']')
           || p_char == QLatin1Char('}');
}

bool TextUtils::matchBracket(const QChar &p_open, const QChar &p_close)
{
    return (p_open == QLatin1Char('(') && p_close == QLatin1Char(')'))
           || (p_open == QLatin1Char('[') && p_close == QLatin1Char(']'))
           || (p_open == QLatin1Char('{') && p_close == QLatin1Char('}'));
}
