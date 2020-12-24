#ifndef MARKDOWNUTILS_H
#define MARKDOWNUTILS_H

#include "vtextedit_export.h"

#include <QString>
#include <QPixmap>

#include <functional>

class QTextEdit;
class QTextCursor;
class QTextBlock;

namespace vte
{
    namespace peg
    {
        struct ElementRegion;
    }

    struct VTEXTEDIT_EXPORT MarkdownLink
    {
        enum TypeFlag
        {
            None = 0x0,
            LocalRelativeInternal = 0x1,
            LocalRelativeExternal = 0x2,
            LocalAbsolute = 0x4,
            QtResource = 0x8,
            Remote = 0x10
        };
        Q_DECLARE_FLAGS(TypeFlags, TypeFlag);

        QString toString() const
        {
            return QString("path (%1) urlInLink (%2)").arg(m_path, m_urlInLink);
        }

        QString m_urlInLink;

        QString m_path;

        // Global position of m_urlInLink.
        int m_urlInLinkPos = -1;

        TypeFlags m_type = TypeFlag::None;
    };

    class VTEXTEDIT_EXPORT MarkdownUtils
    {
    public:
        MarkdownUtils() = delete;

        static QString unindentCodeBlockText(const QString &p_text);

        static bool isFencedCodeBlockStartMark(const QString &p_text);

        // Fetch the image link's URL if there is only one link.
        static QString fetchImageLinkUrl(const QString &p_text, int &p_width, int &p_height);

        // Return the absolute path of @p_url according to @p_basePath.
        static QString linkUrlToPath(const QString &p_basePath, const QString &p_url);

        static QPixmap scaleImage(const QPixmap &p_img,
                                  int p_width,
                                  int p_height,
                                  qreal p_scaleFactor);

        // Insert or make selection heading at @p_level.
        // @p_level: 0 for none, and 1-6 for headings.
        static void typeHeading(QTextEdit *p_edit, int p_level);

        static void typeBold(QTextEdit *p_edit);

        static void typeItalic(QTextEdit *p_edit);

        static void typeStrikethrough(QTextEdit *p_edit);

        static void typeUnorderedList(QTextEdit *p_edit);

        static void typeOrderedList(QTextEdit *p_edit);

        static void typeTodoList(QTextEdit *p_edit, bool p_checked);

        static void typeCode(QTextEdit *p_edit);

        static void typeCodeBlock(QTextEdit *p_edit);

        static void typeMath(QTextEdit *p_edit);

        static void typeMathBlock(QTextEdit *p_edit);

        static void typeQuote(QTextEdit *p_edit);

        static void typeLink(QTextEdit *p_edit, const QString &p_linkText, const QString &p_linkUrl);

        // @p_width/@p_height: 0 for no override.
        static QString generateImageLink(const QString &p_title,
                                         const QString &p_url,
                                         const QString &p_altText,
                                         int p_width = 0,
                                         int p_height = 0);

        // @p_content: Markdwon text content.
        // @p_contentBasePath: base path used to resolve image link and check if it is internal image.
        // @p_flags: type of images want to fetch.
        // @MarkdownLink: descending ordered by m_urlInLinkPos.
        static QVector<MarkdownLink> fetchImagesFromMarkdownText(const QString &p_content,
                                                                 const QString &p_contentBasePath,
                                                                 MarkdownLink::TypeFlags p_flags);

        // Use PegParser to parse @p_content and return the image regions.
        static QVector<peg::ElementRegion> fetchImageRegionsViaParser(const QString &p_content);

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

        // Regular expression for header block.
        // Captured texts:
        // 1. Header marker (##);
        // 2. Header Title (need to be trimmed);
        // 3. Header Sequence (1.1., 1.2., optional);
        // 4. Unused;
        static const QString c_headerRegExp;

        // Regular expression for header block.
        // Captured texts:
        // 1. prefix till the real header title content;
        static const QString c_headerPrefixRegExp;

    private:
        enum CursorPosition
        {
            StartMarker,
            NewLinebetweenMarkers,
            EndMarker
        };

        struct QuoteData
        {
            bool m_isFirstLine = true;
            bool m_insertQuote = true;
            int m_indentation = 0;
        };

        static bool isQrcPath(const QString &p_path);

        static bool insertHeading(QTextCursor &p_cursor,
                                  const QTextBlock &p_block,
                                  void *p_level);

        static void typeMarker(QTextEdit *p_edit,
                               const QString &p_startMarker,
                               const QString &p_endMarker,
                               bool p_allowSpacesAtTwoEnds = false);

        static void typeBlockMarker(QTextEdit *p_edit,
                                    const QString &p_startMarker,
                                    const QString &p_endMarker,
                                    CursorPosition p_cursorPosition);

        // Helper function to iterate all selected lines one by one or just current line.
        // @p_func: return true if there is change.
        static void doOnSelectedLinesOrCurrentLine(QTextEdit *p_edit,
            const std::function<bool(QTextCursor &, const QTextBlock &, void *)> &p_func,
            void *p_data = nullptr);

        static bool insertUnorderedList(QTextCursor &p_cursor,
                                        const QTextBlock &p_block,
                                        void *p_data);

        static bool insertOrderedList(QTextCursor &p_cursor,
                                      const QTextBlock &p_block,
                                      void *p_data);

        static bool insertTodoList(QTextCursor &p_cursor,
                                   const QTextBlock &p_block,
                                   void *p_checked);

        // @p_data: QuoteData.
        static bool insertQuote(QTextCursor &p_cursor,
                                const QTextBlock &p_block,
                                void *p_data);

        // Return relative path of @p_path to @p_dir.
        static QString relativePath(const QString &p_dir, const QString &p_path);

        // Whether @p_dir contains @p_path.
        static bool pathContains(const QString &p_dir, const QString &p_path);

        // Regular expression for todo list.
        // Captured texts:
        // 1. Indentation;
        // 2. List mark (- or *);
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
        // 2. List mark (- or *);
        // 3. List content;
        static const QString c_unorderedListRegExp;

        // Regular expression for quote.
        // Captured texts:
        // 1. Indentation;
        // 2. Quote content;
        static const QString c_quoteRegExp;
    };
}

Q_DECLARE_OPERATORS_FOR_FLAGS(vte::MarkdownLink::TypeFlags)

#endif // MARKDOWNUTILS_H
