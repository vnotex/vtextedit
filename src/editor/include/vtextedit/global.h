#ifndef VTEXTEDIT_GLOBAL_H
#define VTEXTEDIT_GLOBAL_H

#include <QObject>

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

    static QString editorModeToString(EditorMode p_mode)
    {
        switch (p_mode) {
        case EditorMode::NormalModeInsert:
            return QObject::tr("Insert");

        case EditorMode::NormalModeOverwrite:
            return QObject::tr("Overwrite");

        case EditorMode::ViModeNormal:
            return QObject::tr("Normal");

        case EditorMode::ViModeInsert:
            return QObject::tr("Insert");

        case EditorMode::ViModeVisual:
            return QObject::tr("Visual");

        case EditorMode::ViModeVisualLine:
            return QObject::tr("Visual Line");

        case EditorMode::ViModeVisualBlock:
            return QObject::tr("Visual Block");

        case EditorMode::ViModeReplace:
            return QObject::tr("Replace");

        default:
            return QObject::tr("Error");
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
}

Q_DECLARE_OPERATORS_FOR_FLAGS(vte::FindFlags)

#endif // VTEXTEDIT_GLOBAL_H
