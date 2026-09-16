#ifndef TEXTUTILS_H
#define TEXTUTILS_H

#include "vtextedit_export.h"

#include <QRegularExpression>
#include <QString>

#include "global.h"

namespace vte {
class VTEXTEDIT_EXPORT TextUtils {
public:
  TextUtils() = delete;

  // Preserve UTF-16 source offsets. An explicit end is exclusive; -1 includes EOF matches.
  template <typename Visitor>
  static void forEachSearchMatch(QString p_content, const QString &p_text, FindFlags p_flags,
                                 int p_start, int p_end, Visitor p_visitor) {
    if (p_text.isEmpty() || p_start < 0 || p_end < -1) {
      return;
    }

    // Preserve UTF-16 offsets, NBSP, and all text except document separators.
    p_content.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
    p_content.replace(QChar::LineSeparator, QLatin1Char('\n'));
    const int size = p_content.size();
    const int end = p_end == -1 ? size : qMin(p_end, size);
    if (p_start > size || (p_end != -1 && p_start >= end)) {
      return;
    }

    auto pattern = p_text;
    if (!(p_flags & FindFlag::RegularExpression)) {
      TextUtils::transformLineEnding(pattern, LineEnding::CRLF, LineEnding::LF);
      TextUtils::transformLineEnding(pattern, LineEnding::CR, LineEnding::LF);
      pattern.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
      pattern.replace(QChar::LineSeparator, QLatin1Char('\n'));
      pattern = QRegularExpression::escape(pattern);
    }
    QRegularExpression::PatternOptions options = QRegularExpression::MultilineOption;
    if (!(p_flags & FindFlag::CaseSensitive)) {
      options |= QRegularExpression::CaseInsensitiveOption;
    }
    const QRegularExpression regex(pattern, options);
    if (!regex.isValid()) {
      return;
    }

    auto matches = regex.globalMatch(p_content, p_start);
    int emptyMatchStart = -1;
    while (matches.hasNext()) {
      const auto match = matches.next();
      const int start = match.capturedStart();
      const int matchEnd = match.capturedEnd();
      if (start > end || (p_end != -1 && start == end)) {
        break;
      }
      if (start < p_start || matchEnd > end || start == emptyMatchStart) {
        continue;
      }
      if ((p_flags & FindFlag::WholeWordOnly) &&
          ((start > 0 && p_content.at(start - 1).isLetterOrNumber()) ||
           (matchEnd < size && p_content.at(matchEnd).isLetterOrNumber()))) {
        continue;
      }
      if (start == matchEnd) {
        // globalMatch may retry a nonempty alternative at this same position.
        emptyMatchStart = start;
      }
      if (!p_visitor(match)) {
        break;
      }
    }
  }

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
