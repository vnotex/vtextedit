#include "textutils.h"

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
