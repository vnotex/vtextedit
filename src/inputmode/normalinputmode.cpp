#include "normalinputmode.h"

#include <QKeyEvent>
#include <QInputDialog>

#include "inputmodeeditorinterface.h"

using namespace vte;

NormalInputMode::NormalInputMode(InputModeEditorInterface *p_interface)
    : AbstractInputMode(p_interface)
{
}

QString NormalInputMode::name() const
{
    return QStringLiteral("normal");
}

InputMode NormalInputMode::mode() const
{
    return InputMode::NormalMode;
}

void NormalInputMode::activate()
{
}

void NormalInputMode::deactivate()
{
}

bool NormalInputMode::handleKeyPress(QKeyEvent *p_event)
{
    if (p_event->modifiers() == Qt::NoModifier) {
        switch (p_event->key()) {
        case Qt::Key_Insert:
            if (m_mode == EditorMode::NormalModeInsert) {
                enterOverwriteMode();
            } else {
                Q_ASSERT(m_mode == EditorMode::NormalModeOverwrite);
                enterInsertMode();
            }
            m_interface->notifyEditorModeChanged(editorMode());
            return true;

        case Qt::Key_Enter:
            Q_FALLTHROUGH();
        case Qt::Key_Return:
            if (m_interface->isCompletionActive()) {
                m_interface->completionExecute();
                return true;
            }
            break;

        default:
            break;
        }
    } else if (p_event->modifiers() == Qt::ControlModifier) {
        switch (p_event->key()) {
        case Qt::Key_Space:
            Q_FALLTHROUGH();
        case Qt::Key_N:
            commandCompleteNext();
            return true;

        case Qt::Key_P:
            commandCompletePrevious();
            return true;

        case Qt::Key_J:
            m_interface->scrollUp();
            return true;

        case Qt::Key_K:
            m_interface->scrollDown();
            return true;
        
        case Qt::Key_C:
            if (!m_interface->selection()) {
                copyCurrentLine(false);
                return true;
            }
            break;

        case Qt::Key_X:
            if (!m_interface->selection()) {
                copyCurrentLine(true);
                return true;
            }
            break;

        case Qt::Key_L:
            selectCurrentLine();
            return true;

        default:
            break;
        }
    } else if (p_event->modifiers() == (Qt::ShiftModifier | Qt::ControlModifier)) {
        switch (p_event->key()) {
        case Qt::Key_G:
            gotoLine();
            return true;

        default:
            break;
        }
    } else if (p_event->modifiers() == Qt::AltModifier) {
        switch (p_event->key()) {
        case Qt::Key_Up:
            moveLineUp();
            return true;

        case Qt::Key_Down:
            moveLineDown();
            return true;

        default:
            break;
        }
    } else if (p_event->modifiers() == (Qt::ShiftModifier | Qt::AltModifier)) {
        switch (p_event->key()) {
        case Qt::Key_Up:
            duplicateLineUp();
            return true;

        case Qt::Key_Down:
            duplicateLineDown();
            return true;

        default:
            break;
        }
    }

    return false;
}

bool NormalInputMode::stealShortcut(QKeyEvent *p_event)
{
    Q_UNUSED(p_event);
    return false;
}

EditorMode NormalInputMode::editorMode() const
{
    return m_mode;
}

void NormalInputMode::focusIn()
{
}

void NormalInputMode::focusOut()
{
}

void NormalInputMode::preKeyPressDefaultHandle(QKeyEvent *p_event)
{
    Q_UNUSED(p_event);
}

void NormalInputMode::postKeyPressDefaultHandle(QKeyEvent *p_event)
{
    Q_UNUSED(p_event);
}

void NormalInputMode::enterInsertMode()
{
    m_interface->setCaretStyle(CaretStyle::Line);
    m_interface->update();
    m_interface->setOverwriteMode(false);
    m_mode = EditorMode::NormalModeInsert;
}

void NormalInputMode::enterOverwriteMode()
{
    m_interface->setCaretStyle(CaretStyle::Half);
    m_interface->update();
    m_interface->setOverwriteMode(true);
    m_mode = EditorMode::NormalModeOverwrite;
}

void NormalInputMode::commandCompleteNext()
{
    if (m_interface->isCompletionActive()) {
        m_interface->completionNext(false);
    } else {
        m_interface->userInvokedCompletion(false);
    }
}

void NormalInputMode::commandCompletePrevious()
{
    if (m_interface->isCompletionActive()) {
        m_interface->completionNext(true);
    } else {
        m_interface->userInvokedCompletion(true);
    }
}

QSharedPointer<InputModeStatusWidget> NormalInputMode::statusWidget()
{
    return nullptr;
}

void NormalInputMode::gotoLine()
{
    // Get current line number (1-based) and max line number
    int currentLine = m_interface->cursorPosition().line() + 1;
    int maxLine = m_interface->lastLine() + 1;

    bool ok = false;
    int line = QInputDialog::getInt(nullptr,
                                    QWidget::tr("Go to Line"),
                                    QWidget::tr("Line number (1-%1):").arg(maxLine),
                                    currentLine,
                                    1,
                                    maxLine,
                                    1,
                                    &ok);
    if (ok) {
        // Lines are 0-based internally
        m_interface->updateCursor(line - 1, 0);
    }
}

void NormalInputMode::copyCurrentLine(bool p_cut)
{
    int line = m_interface->cursorPosition().line();
    // Get the line text before removing it
    QString text = m_interface->line(line);
    // Copy to clipboard with newline to maintain line format when pasting
    m_interface->copyToClipboard(text + '\n');
    // Remove the line
    if (p_cut) {
        m_interface->removeLine(line);
    }
}

void NormalInputMode::selectCurrentLine()
{
    // Get current line number
    int line = m_interface->cursorPosition().line();
    // Check if this is the last line
    int lastLine = m_interface->lastLine();

    if (line < lastLine) {
        // Select from start of current line to start of next line to include newline
        m_interface->setSelection(line, 0, line + 1, 0);
    } else {
        // Last line - select to end of line since there's no newline
        int length = m_interface->lineLength(line);
        m_interface->setSelection(line, 0, line, length);
    }
}

void NormalInputMode::moveLineUp()
{
    int currentLine = m_interface->cursorPosition().line();
    int currentColumn = m_interface->cursorPosition().column();
    if (currentLine > 0) {
        // Get the content of both lines
        QString currentLineText = m_interface->line(currentLine);
        QString aboveLineText = m_interface->line(currentLine - 1);

        // Remove both lines from bottom to top to maintain correct line numbers
        m_interface->removeLine(currentLine);
        m_interface->removeLine(currentLine - 1);

        // Insert lines in swapped order
        m_interface->insertLine(currentLine - 1, currentLineText);
        m_interface->insertLine(currentLine, aboveLineText);

        // Move cursor to new line position
        m_interface->updateCursor(currentLine - 1, currentColumn);
    }
}

void NormalInputMode::moveLineDown()
{
    int currentLine = m_interface->cursorPosition().line();
    int currentColumn = m_interface->cursorPosition().column();
    if (currentLine < m_interface->lastLine()) {
        // Get the content of both lines
        QString currentLineText = m_interface->line(currentLine);
        QString belowLineText = m_interface->line(currentLine + 1);

        // Remove both lines from bottom to top to maintain correct line numbers
        m_interface->removeLine(currentLine + 1);
        m_interface->removeLine(currentLine);

        // Insert lines in swapped order
        m_interface->insertLine(currentLine, belowLineText);
        m_interface->insertLine(currentLine + 1, currentLineText);

        // Move cursor to new line position
        m_interface->updateCursor(currentLine + 1, currentColumn);
    }
}

void NormalInputMode::duplicateLineUp()
{
    int currentLine = m_interface->cursorPosition().line();
    int currentColumn = m_interface->cursorPosition().column();
    
    // Get the content of current line
    QString lineText = m_interface->line(currentLine);
    
    // Insert a copy of the line above the current line
    m_interface->insertLine(currentLine, lineText);
    
    // Keep cursor on the same column in the duplicated line
    m_interface->updateCursor(currentLine, currentColumn);
}

void NormalInputMode::duplicateLineDown()
{
    int currentLine = m_interface->cursorPosition().line();
    int currentColumn = m_interface->cursorPosition().column();
    
    // Get the content of current line
    QString lineText = m_interface->line(currentLine);
    
    // Insert a copy of the line below the current line
    m_interface->insertLine(currentLine + 1, lineText);
    
    // Move cursor to the duplicated line
    m_interface->updateCursor(currentLine + 1, currentColumn);
}
