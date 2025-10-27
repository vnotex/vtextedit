#ifndef TEXTUTILS_H
#define TEXTUTILS_H

#include "vtextedit_export.h"

#include <QString>

#include "global.h"

namespace vte {
class VTEXTEDIT_EXPORT TextUtils {
public:
  TextUtils() = delete;

  static int firstNonSpace(const QString &p_text);

  static int lastNonSpace(const QString &p_text);

  static int trailingWhitespaces(const QString &p_text);

  static int fetchIndentation(const QString &p_text);

  static QString fetchIndentationSpaces(const QString &p_text);

  // @p_text may have multiline.
  // Fetch the indentation of the line located by @p_pos.
  static QString fetchIndentationSpacesInMultiLines(const QString &p_text, int p_pos);

  static QString unindentText(const QString &p_text, int p_spaces);

  // Check is all characters of p_text[p_start, p_end) are spaces.
  static bool isSpace(const QString &p_text, int p_start, int p_end);

  // Remove query in the url (?xxx).
  static QString purifyUrl(const QString &p_url);

  // Decode URL by simply replacing meta-characters.
  static QString decodeUrl(const QString &p_url);

  // We only handle space here.
  static QString encodeUrl(const QString &p_path);

  static QString removeCodeBlockFence(const QString &p_text);

  // Unindent multi-lines text according to the indentation of the first line.
  static QString unindentTextMultiLines(const QString &p_text);

  static bool isClosingBracket(const QChar &p_char);

  static bool matchBracket(const QChar &p_open, const QChar &p_close);

  static LineEnding detectLineEnding(const QString &p_text);

  static void transformLineEnding(QString &p_text, LineEnding p_before, LineEnding p_after);

  static QString lineEndingString(LineEnding p_lineEnding);

  // Whether the char at @p_offset is escpaed.
  static bool isEscaped(const QString &p_text, int p_offset,
                        const QChar &p_escapeChar = QLatin1Char('\\'));
};
} // namespace vte

#endif // TEXTUTILS_H
