#ifndef MARKDOWNUTILS_H
#define MARKDOWNUTILS_H

#include "vtextedit_export.h"

#include <QPixmap>
#include <QString>
#include <QVector>

#include <functional>

class QTextCursor;
class QTextBlock;

namespace vte {
class VTextEdit;

// One image link found in a Markdown document, with everything the callers
// need: where it is, what it points at, and where its destination may be
// rewritten.
struct VTEXTEDIT_EXPORT MarkdownLink {
  enum TypeFlag {
    None = 0x0,
    LocalRelativeInternal = 0x1,
    LocalRelativeExternal = 0x2,
    LocalAbsolute = 0x4,
    QtResource = 0x8,
    Remote = 0x10
  };
  Q_DECLARE_FLAGS(TypeFlags, TypeFlag);

  QString toString() const {
    return QStringLiteral("path (%1) urlInLink (%2) raw [%3, %4)")
        .arg(m_path, m_urlInLink)
        .arg(m_urlStart)
        .arg(m_urlEnd);
  }

  // Whether the destination occupies a span of source text that may be
  // rewritten. False for a reference-style image (`![a][r]`) and for an empty
  // destination (`![a]()`), neither of which spells a destination inside the
  // construct. The region is still valid in both cases.
  bool hasUrlSpan() const { return m_urlStart >= 0 && m_urlEnd > m_urlStart; }

  // The destination as cmark resolved it: backslash escapes and entities are
  // decoded and any angle brackets stripped.
  //
  // NEVER use this to compute a replacement length. `a\_b.png` occupies 8
  // source characters and resolves to 7; `<a b.png>` occupies 9 and resolves to
  // 7. Use `m_urlEnd - m_urlStart`.
  QString m_urlInLink;

  QString m_path;

  // Half-open span of the RAW destination in the source, exactly as spelled --
  // escapes, entities and angle brackets all still present. -1 when absent; see
  // hasUrlSpan().
  int m_urlStart = -1;
  int m_urlEnd = -1;

  // Half-open span of the whole `![alt](dest "title" =WxH)` construct. Always
  // valid for every image returned.
  //
  // Regions may NEST: CommonMark allows an image inside another image's
  // description (`![foo ![bar](/a)](/b)`), and both are reported. Destination
  // spans never overlap, so destination-only rewriting is always safe; a caller
  // that replaces whole REGIONS must skip images contained in another's region,
  // as MarkdownEditor::fetchImagesToLocalAndReplace() does.
  int m_regionStart = -1;
  int m_regionEnd = -1;

  QString m_alt;

  QString m_title;

  // Declared size from the `=WxH` extension; 0 means unspecified for that axis.
  int m_width = 0;
  int m_height = 0;

  // SYNTACTIC classification, derived from the shape of the destination alone.
  //
  // Deliberately independent of whether the file is there: a relative link to a
  // missing file is a broken LocalRelative* link, not a Remote one. Classifying
  // by existence made every typo'd path look remote, so a caller asking for
  // local images silently skipped exactly the images a user would want repaired.
  TypeFlags m_type = TypeFlag::None;

  // Whether m_path exists on disk. Only meaningful for the local flavors.
  bool m_exists = false;
};

class VTEXTEDIT_EXPORT MarkdownUtils {
public:
  MarkdownUtils() = delete;

  static QString unindentCodeBlockText(const QString &p_text);

  static bool isFencedCodeBlockStartMark(const QString &p_text);

  // Return the absolute path of @p_url according to @p_basePath.
  static QString linkUrlToPath(const QString &p_basePath, const QString &p_url);

  static QPixmap scaleImage(const QPixmap &p_img, int p_width, int p_height, qreal p_scaleFactor);

  // Insert or make selection heading at @p_level.
  // @p_level: 0 for none, and 1-6 for headings.
  static void typeHeading(VTextEdit *p_edit, int p_level);

  static void typeBold(VTextEdit *p_edit);

  static void typeItalic(VTextEdit *p_edit);

  static void typeStrikethrough(VTextEdit *p_edit);

  static void typeMark(VTextEdit *p_edit);

  static void typeUnorderedList(VTextEdit *p_edit);

  static void typeOrderedList(VTextEdit *p_edit);

  static void typeTodoList(VTextEdit *p_edit, bool p_checked);

  static void typeCode(VTextEdit *p_edit);

  static void typeCodeBlock(VTextEdit *p_edit);

  static void typeMath(VTextEdit *p_edit);

  static void typeMathBlock(VTextEdit *p_edit);

  static void typeQuote(VTextEdit *p_edit);

  static void typeLink(VTextEdit *p_edit, const QString &p_linkText, const QString &p_linkUrl);

  static QString generateImageLink(const QString &p_title, const QString &p_url,
                                   const QString &p_altText);

  // Every image link in @p_content, straight from cmark's AST.
  //
  // @p_contentBasePath: base path used to resolve a relative destination and to
  //   decide internal vs external.
  // @p_flags: which syntactic kinds to return. Pass every flag to get them all.
  //
  // An image whose resolved destination is empty (`![a]()`) is omitted: it
  // points at nothing, so there is no path, no type and nothing to act on. An
  // image nested in another image's description IS included; see m_regionStart.
  //
  // Returned DESCENDING by m_urlStart, so a caller may rewrite destinations in
  // order without invalidating the spans it has not reached yet. Entries with
  // no destination span (reference-style) sort LAST, since they cannot be
  // rewritten at all; the sort is stable, so their relative document order is
  // preserved. No deduplication: one file referenced twice yields two entries,
  // and both must be rewritten.
  static QVector<MarkdownLink> fetchImageLinks(const QString &p_content,
                                               const QString &p_contentBasePath,
                                               MarkdownLink::TypeFlags p_flags);

  struct HeaderMatch {
    bool m_matched = false;

    int m_level = -1;

    int m_spacesAfterMarker = 0;

    // The whole header including sequence.
    QString m_header;

    QString m_sequence;

    int m_spacesAfterSequence = 0;
  };
  static HeaderMatch matchHeader(const QString &p_text);

  static bool isTodoList(const QString &p_text, QChar &p_listMark, bool &p_empty);

  static bool isUnorderedList(const QString &p_text, QChar &p_listMark, bool &p_empty);

  static bool isOrderedList(const QString &p_text, QString &p_listNumber, bool &p_empty);

  // Return true if @p_text starts with a block quote prefix.
  // @p_indentation: leading whitespace before the first '>'.
  // @p_prefix: the matched quote prefix, verbatim, excluding @p_indentation.
  // @p_rest: the text after the quote prefix.
  // @p_depth: number of '>' markers in @p_prefix.
  static bool isQuote(const QString &p_text, QString &p_indentation, QString &p_prefix,
                      QString &p_rest, int &p_depth);

  static QString setOrderedListNumber(QString p_text, int p_number);

  static const QString c_fencedCodeBlockStartRegExp;

  static const QString c_fencedCodeBlockEndRegExp;

  // Regular expression for link.
  // [link text]( http://github.com/tamlok "alt text")
  // Captured texts (need to be trimmed):
  // 1. Link Alt Text (Title);
  // 2. Link URL;
  // 3. Link Optional Title with double quotes or quotes;
  // 4. Unused;
  // 5. Unused;
  static const QString c_linkRegExp;

private:
  enum CursorPosition { StartMarker, NewLinebetweenMarkers, EndMarker };

  struct QuoteData {
    bool m_isFirstLine = true;
    bool m_insertQuote = true;
    int m_indentation = 0;
  };

  static bool isQrcPath(const QString &p_path);

  static bool insertHeading(QTextCursor &p_cursor, const QTextBlock &p_block, void *p_level);

  static void typeMarker(VTextEdit *p_edit, const QString &p_startMarker,
                         const QString &p_endMarker, bool p_allowSpacesAtTwoEnds = false);

  // Absolute document positions of the marker-able content of one line.
  struct MarkerRange {
    int m_start = -1;
    int m_end = -1;
    bool isValid() const { return m_start >= 0 && m_start < m_end; }
  };

  // Content range of @p_block clipped to [@p_selStart, @p_selEnd), with leading
  // indentation, list/quote/heading prefix and trailing whitespace excluded.
  // Return false for blank lines or an empty clipped range.
  static bool markerRangeOfBlock(const QTextBlock &p_block, int p_selStart, int p_selEnd,
                                 MarkerRange &p_range);

  // Apply/remove one marker pair per selected line. Called by typeMarker when
  // the selection crosses blocks.
  static void typeMarkerOnLines(VTextEdit *p_edit, const QString &p_startMarker,
                                const QString &p_endMarker);

  static void typeBlockMarker(VTextEdit *p_edit, const QString &p_startMarker,
                              const QString &p_endMarker, CursorPosition p_cursorPosition);

  // Helper function to iterate all selected lines one by one or just current
  // line.
  // @p_func: return true if there is change.
  static void doOnSelectedLinesOrCurrentLine(
      VTextEdit *p_edit,
      const std::function<bool(QTextCursor &, const QTextBlock &, void *)> &p_func,
      void *p_data = nullptr);

  static bool insertUnorderedList(QTextCursor &p_cursor, const QTextBlock &p_block, void *p_data);

  static bool insertOrderedList(QTextCursor &p_cursor, const QTextBlock &p_block, void *p_data);

  static bool insertTodoList(QTextCursor &p_cursor, const QTextBlock &p_block, void *p_checked);

  // @p_data: QuoteData.
  static bool insertQuote(QTextCursor &p_cursor, const QTextBlock &p_block, void *p_data);

  // Return relative path of @p_path to @p_dir.
  static QString relativePath(const QString &p_dir, const QString &p_path);

  // Whether @p_dir contains @p_path.
  static bool pathContains(const QString &p_dir, const QString &p_path);

  // Regular expression for todo list.
  // Captured texts:
  // 1. Indentation;
  // 2. List mark (- or * or +);
  // 3. Checked mark (x or space);
  // 4. List content;
  static const QString c_todoListRegExp;

  // Regular expression for ordered list.
  // Captured texts:
  // 1. Indentation;
  // 2. List number;
  // 3. List content;
  static const QString c_orderedListRegExp;

  // Regular expression for unordered list.
  // Captured texts:
  // 1. Indentation;
  // 2. List mark (- or * or +);
  // 3. List content;
  static const QString c_unorderedListRegExp;

  // Regular expression for quote.
  // Captured texts:
  // 1. Indentation;
  // 2. Quote content;
  static const QString c_quoteRegExp;

  // Regular expression for a (possibly nested) block quote prefix.
  // Captured texts:
  // 1. Indentation;
  // 2. Quote prefix, verbatim;
  // 3. Remainder of the line;
  static const QString c_quotePrefixRegExp;

  // Regular expression for header block.
  // Captured texts:
  // 1. Header marker (##);
  // 2. Spaces after marker;
  // 3. Header Title (need to be trimmed, all text after marker and spaces);
  // 4. Header Sequence (1.1., 1.2., optional);
  // 5. Spaces after header sequence;
  static const QString c_headerRegExp;
};
} // namespace vte

Q_DECLARE_OPERATORS_FOR_FLAGS(vte::MarkdownLink::TypeFlags)

#endif // MARKDOWNUTILS_H
