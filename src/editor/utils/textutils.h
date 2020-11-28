#ifndef TEXTUTILS_H
#define TEXTUTILS_H

#include <QString>

namespace vte
{
    class TextUtils
    {
    public:
        TextUtils() = delete;

        static int firstNonSpace(const QString &p_text);

        static int lastNonSpace(const QString &p_text);

        static int trailingWhitespaces(const QString &p_text);

        static int fetchIndentation(const QString &p_text);

        static QString fetchIndentationSpaces(const QString &p_text);

        static QString unindentText(const QString &p_text, int p_spaces);

        // Check is all characters of p_text[p_start, p_end) are spaces.
        static bool isSpace(const QString &p_text, int p_start, int p_end);

        // Remove query in the url (?xxx).
        static QString purifyUrl(const QString &p_url);

        // Decode URL by simply replacing meta-characters.
        static void decodeUrl(QString &p_url);
    };
}

#endif // TEXTUTILS_H
