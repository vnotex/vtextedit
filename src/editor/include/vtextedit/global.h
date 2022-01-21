#ifndef VTEXTEDIT_GLOBAL_H
#define VTEXTEDIT_GLOBAL_H

#include <QObject>
#include <QKeySequence>

namespace vte
{
    enum InputMode
    {
        NormalMode = 0,
        ViMode,
        MaxInputMode
    };

    // Must align with KateViEditorInterface::ViewMode.
    enum EditorMode
    {
        NormalModeInsert = 0,    /**< Insert mode. Characters will be added. */
        NormalModeOverwrite = 1, /**< Overwrite mode. Characters will be replaced. */

        ViModeNormal = 10,
        ViModeInsert = 11,
        ViModeVisual = 12,
        ViModeVisualLine = 13,
        ViModeVisualBlock = 14,
        ViModeReplace = 15
    };

    class VTextEditTranslate : public QObject
    {
        Q_OBJECT
    };

    inline QString editorModeToString(EditorMode p_mode)
    {
        switch (p_mode) {
        case EditorMode::NormalModeInsert:
            return VTextEditTranslate::tr("Insert");

        case EditorMode::NormalModeOverwrite:
            return VTextEditTranslate::tr("Overwrite");

        case EditorMode::ViModeNormal:
            return VTextEditTranslate::tr("Normal (Vi)");

        case EditorMode::ViModeInsert:
            return VTextEditTranslate::tr("Insert (Vi)");

        case EditorMode::ViModeVisual:
            return VTextEditTranslate::tr("Visual (Vi)");

        case EditorMode::ViModeVisualLine:
            return VTextEditTranslate::tr("Visual Line (Vi)");

        case EditorMode::ViModeVisualBlock:
            return VTextEditTranslate::tr("Visual Block (Vi)");

        case EditorMode::ViModeReplace:
            return VTextEditTranslate::tr("Replace (Vi)");

        default:
            return VTextEditTranslate::tr("Unknown");
        }
    }

    enum CaretStyle
    {
        Line,
        Block,
        Underline,
        Half,
        MaxCaretStyle
    };

    typedef unsigned long long TimeStamp;

    enum CenterCursor
    {
        NeverCenter,
        AlwaysCenter,
        CenterOnBottom
    };

    enum WrapMode
    {
        NoWrap,
        WordWrap,
        WrapAnywhere,
        WordWrapOrAnywhere
    };

    enum FindFlag
    {
        None = 0,
        FindBackward = 0x1,
        CaseSensitive = 0x2,
        WholeWordOnly = 0x4,
        RegularExpression = 0x8
    };
    Q_DECLARE_FLAGS(FindFlags, FindFlag);

    enum class LineEndingPolicy
    {
        Platform,
        File,
        LF,
        CRLF,
        CR
    };

    enum class LineEnding
    {
        LF,
        CRLF,
        CR
    };

    struct Key
    {
        Key() = default;

        Key(int p_key, Qt::KeyboardModifiers p_modifiers)
            : m_key(p_key),
              m_modifiers(p_modifiers)
        {
        }

        Key(const QString &p_key)
        {
            QKeySequence seq(p_key);
            if (seq.count() == 0) {
                return;
            }

            const int keyMask = 0x01FFFFFF;
            m_key = seq[0] & keyMask;
            m_modifiers = static_cast<Qt::KeyboardModifiers>(seq[0] & (~keyMask));
        }

        int m_key = 0;

        Qt::KeyboardModifiers m_modifiers = Qt::NoModifier;
    };
}

Q_DECLARE_OPERATORS_FOR_FLAGS(vte::FindFlags)

#endif // VTEXTEDIT_GLOBAL_H
