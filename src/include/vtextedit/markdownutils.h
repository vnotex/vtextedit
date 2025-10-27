#ifndef MARKDOWNUTILS_H
#define MARKDOWNUTILS_H

#include "vtextedit_export.h"

#include <QPixmap>
#include <QString>

#include <functional>

class QTextCursor;
class QTextBlock;

namespace vte {
class VTextEdit;

namespace peg {
struct ElementRegion;
}

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
    return QStringLiteral("path (%1) urlInLink (%2)").arg(m_path, m_urlInLink);
  }

  QString m_urlInLink;

  QString m_path;

  // Global position of m_urlInLink.
  int m_urlInLinkPos = -1;

  TypeFlags m_type = TypeFlag::None;
};

class VTEXTEDIT_EXPORT MarkdownUtils {
public:
  MarkdownUtils() = delete;

  static QString unindentCodeBlockText(const QString &p_text);

  static bool isFencedCodeBlockStartMark(const QString &p_text);

  static bool hasImageLink(const QString &p_text);

  // Fetch the image link's URL if there is only one link.
  static QString fetchImageLinkUrl(const QString &p_text, int &p_width, int &p_height);

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

  // @p_width/@p_height: 0 for no override.
  static QString generateImageLink(const QString &p_title, const QString &p_url,
                                   const QString &p_altText, int p_width = 0, int p_height = 0);

  // @p_content: Markdwon text content.
  // @p_contentBasePath: base path used to resolve image link and check if it is
  // internal image.
  // @p_flags: type of images want to fetch.
  // @Return: descending ordered by m_urlInLinkPos, without deduplication of
  // image path.
  static QVector<MarkdownLink> fetchImagesFromMarkdownText(const QString &p_content,
                                                           const QString &p_contentBasePath,
                                                           MarkdownLink::TypeFlags p_flags);

  // Use PegParser to parse @p_content and return the image regions.
  static QVector<peg::ElementRegion> fetchImageRegionsViaParser(const QString &p_content);

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

  static QString setOrderedListNumber(QString p_text, int p_number);

  static const QString c_fencedCodeBlockStartRegExp;

  static const QString c_fencedCodeBlockEndRegExp;

  // Regular expression for image link.
  // ![image title]( http://github.com/tamlok/vnote.jpg "alt text" =200x100)
  // Captured texts (need to be trimmed):
  // 1. Image Alt Text (Title);
  // 2. Image URL;
  // 3. Image Optional Title with double quotes or quotes;
  // 4. Unused;
  // 5. Unused;
  // 6. Width and height text;
  // 7. Width;
  // 8. Height;
  static const QString c_imageLinkRegExp;

  // Regular expression for image title.
  static const QString c_imageTitleRegExp;

  // Regular expression for image alt text.
  static const QString c_imageAltRegExp;

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
