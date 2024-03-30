#ifndef INPUTMODEEDITORINTERFACE_H
#define INPUTMODEEDITORINTERFACE_H

#include <katevi/interface/katevieditorinterface.h>

#include <vtextedit/global.h>

namespace vte
{
    // Interface used by InputMode to interact with editor.
    // To be simple, we will implement the KateViEditorInterface here, too.
    class InputModeEditorInterface : public KateViI::KateViEditorInterface
    {
    public:
        virtual ~InputModeEditorInterface()
        {
        }

        virtual void setCaretStyle(CaretStyle p_style) = 0;

        virtual void clearSelection() = 0;

        virtual void updateCursor(int p_line, int p_column) = 0;

        virtual int linesDisplayed() = 0;

        // Notifiers.
        virtual void notifyEditorModeChanged(EditorMode p_mode) = 0;

        virtual void scrollUp() = 0;

        virtual void scrollDown() = 0;
    };
}

#endif // INPUTMODEEDITORINTERFACE_H
