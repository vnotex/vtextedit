#ifndef VTEXTEDIT_THEME_H
#define VTEXTEDIT_THEME_H

#include "vtextedit_export.h"

#include <QColor>
#include <QObject>
#include <QSharedPointer>
#include <QVector>
#include <QStringList>
#include <QTextCharFormat>
#include <QMap>

class QJsonObject;

namespace vte
{
    struct VTEXTEDIT_EXPORT Format
    {
        Format()
            : m_bold(false),
              m_italic(false),
              m_underline(false),
              m_strikeThrough(false),
              m_hasBold(false),
              m_hasItalic(false),
              m_hasUnderline(false),
              m_hasStrikeThrough(false)
        {
        }

        QColor textColor() const
        {
            if (m_textColor > 0) {
                return QColor(m_textColor);
            } else {
                return QColor();
            }
        }

        QColor backgroundColor() const
        {
            if (m_backgroundColor > 0) {
                return QColor(m_backgroundColor);
            } else {
                return QColor();
            }
        }

        QColor selectedTextColor() const
        {
            if (m_selectedTextColor > 0) {
                return QColor(m_selectedTextColor);
            } else {
                return QColor();
            }
        }

        QColor selectedBackgroundColor() const
        {
            if (m_selectedBackgroundColor > 0) {
                return QColor(m_selectedBackgroundColor);
            } else {
                return QColor();
            }
        }

        QTextCharFormat toTextCharFormat() const;

        QStringList m_fontFamilies;

        // The first available font family form m_fontFamilies.
        QString m_fontFamily;

        int m_fontPointSize = 0;

        // 0x0 indicates it is not specified.
        QRgb m_textColor = 0x0;
        QRgb m_backgroundColor = 0x0;
        QRgb m_selectedTextColor = 0x0;
        QRgb m_selectedBackgroundColor = 0x0;

        bool m_bold :1;
        bool m_italic :1;
        bool m_underline :1;
        bool m_strikeThrough :1;

        bool m_hasBold :1;
        bool m_hasItalic :1;
        bool m_hasUnderline :1;
        bool m_hasStrikeThrough :1;
    };

    class VTEXTEDIT_EXPORT Theme
    {
        Q_GADGET
    public:
        enum EditorStyle
        {
            Text = 0,
            CursorLine,
            TrailingSpace,
            Tab,
            SelectedText,
            IndicatorsBorder,
            CurrentLineNumber,
            Folding,
            FoldedFolding,
            FoldingHighlight,
            MaxEditorStyle
        };
        Q_ENUM(EditorStyle)

        // Used by PegMarkdownHighlighter.
        // Should be exactly aligned with pmh_element_type defined in pmh_definitions.h.
        enum MarkdownSyntaxStyle
        {
            LINK = 0, // Explicit link.
            AUTO_LINK_URL, // Implicit URL link.
            AUTO_LINK_EMAIL, // Implicit email link.
            IMAGE, // Image definition.
            CODE, // Code (inline).
            HTML, // HTML.
            HTML_ENTITY, // HTML special entity definition.
            EMPH, // Emphasized text.
            STRONG, // Strong text.
            LIST_BULLET, // Bullet for an unordered list item.
            LIST_ENUMERATOR, // Enumerator for an ordered list item.
            COMMENT, // (HTML) Comment.
            H1, // Header, level 1.
            H2, // Header, level 2.
            H3, // Header, level 3.
            H4, // Header, level 4.
            H5, // Header, level 5.
            H6, // Header, level 6.
            BLOCKQUOTE, // Blockquote.
            VERBATIM, // Verbatim (e.g. block of code).
            HTMLBLOCK, // Block of HTML.
            HRULE, // Horizontal rule.
            REFERENCE, // Reference.
            FENCEDCODEBLOCK, // Fenced code block.
            NOTE, // Note.
            STRIKE, // Strike-through.
            FRONTMATTER, // Front matter.
            DISPLAYFORMULA, // Math display formula.
            INLINEEQUATION, // Math inline equation.
            MARK, // HTML <mark> tag content.
            TABLE, // GFM table.
            TABLEHEADER, // GFM table header.
            TABLEBORDER, // GFM table border |.
            MaxMarkdownSyntaxStyle
        };
        Q_ENUM(MarkdownSyntaxStyle)

        const QString &name() const;

        Format &editorStyle(EditorStyle p_style);

        const Format &editorStyle(EditorStyle p_style) const;

        const QMap<EditorStyle, Format> &markdownEditorStyles() const;

        const QSharedPointer<QVector<Format>> &markdownSyntaxStyles() const;

        static QSharedPointer<Theme> createThemeFromFile(const QString &p_filePath);

    private:
        void load(const QJsonObject &p_obj);

        void loadMetadata(const QJsonObject &p_obj);

        void loadEditorStyles(const QJsonObject &p_obj);

        void loadMarkdownEditorStyles(const QJsonObject &p_obj);

        void loadMarkdownSyntaxStyles(const QJsonObject &p_obj);

        Format loadStyleFormat(const QJsonObject &p_obj);

        QString m_filePath;

        QString m_name;

        int m_revision = 0;

        QString m_type;

        Format m_editorStyles[EditorStyle::MaxEditorStyle];

        // Used to override m_editorStyles.
        QMap<EditorStyle, Format> m_markdownEditorStyles;

        QSharedPointer<QVector<Format>> m_markdownSyntaxStyles;
    };
}

#endif // THEME_H
