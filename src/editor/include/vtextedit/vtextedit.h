#ifndef VTEXTEDIT_VTEXTEDIT_H
#define VTEXTEDIT_VTEXTEDIT_H

#include "vtextedit_export.h"

#include <QTextEdit>
#include <QVector>
#include <QTime>
#include <QScopedPointer>
#include <QTextCursor>

#include <vtextedit/global.h>

class QTimer;
class QMenu;

namespace vte
{
    class AbstractInputMode;

    // Use getSelections() to get current selection and selectedText() to
    // get selected text. QTextCursor may not reflect the real selection if it
    // is overridden.
    class VTEXTEDIT_EXPORT VTextEdit : public QTextEdit
    {
        Q_OBJECT
    public:
        class Selection
        {
        public:
            Selection() = default;

            Selection(int p_start, int p_end)
                : m_start(qMin(p_start, p_end)),
                  m_end(qMax(p_start, p_end))
            {
            }

            bool isValid() const
            {
                return m_start >= 0 && m_start < m_end;
            }

            int start() const
            {
                return m_start;
            }

            int end() const
            {
                return m_end;
            }

            void clear()
            {
                m_start = m_end = 0;
            }

            bool operator==(const Selection &p_other) const
            {
                return p_other.m_start == m_start && p_other.m_end == m_end;
            }

        private:
            int m_start = 0;
            int m_end = 0;
        };

        class Selections
        {
        public:
            friend class VTextEdit;

            const Selection &getSelection() const
            {
                if (m_overriddenSelection.isValid()) {
                    return m_overriddenSelection;
                } else {
                    return m_selection;
                }
            }

            const QVector<Selection> &getAdditionalSelections() const
            {
                return m_additionalSelections;
            }

        private:
            // Main selection.
            Selection m_selection;

            // Overridden main selection.
            Selection m_overriddenSelection;

            // For block mode selection.
            // Sorted by start().
            QVector<Selection> m_additionalSelections;
        };

        explicit VTextEdit(QWidget *p_parent = nullptr);

        quint64 getContentsSeq() const;

        // Must be public.
        void wheelEvent(QWheelEvent *p_event) Q_DECL_OVERRIDE;

        // Must be public.
        void mousePressEvent(QMouseEvent *p_event) Q_DECL_OVERRIDE;

        // Must be public.
        void mouseReleaseEvent(QMouseEvent *p_event) Q_DECL_OVERRIDE;

        // @p_end, -1 indicates the end of doc.
        QList<QTextCursor> findAllText(const QString &p_text,
                                       FindFlags p_flags,
                                       int p_start = 0,
                                       int p_end = -1);

        // Will search and wrap.
        QTextCursor findText(const QString &p_text,
                             FindFlags p_flags,
                             int p_start = 0);

        void setInputMode(const QSharedPointer<AbstractInputMode> &p_mode);
        QSharedPointer<AbstractInputMode> getInputMode() const;

        void setDrawCursorAsBlock(bool p_enabled, bool p_half);

        void repaintBlock(const QTextBlock &p_block);

        const Selections &getSelections() const;

        const Selection &getSelection() const;

        bool hasSelection() const;

        // Get selected text by main selection.
        QString selectedText() const;

        void removeSelectedText();

        void setOverriddenSelection(int p_start, int p_end);

        void clearOverriddenSelection();

        // [p_start, p_end).
        QString getTextByRange(int p_start, int p_end) const;

        void updateCursorWidth();

        // Override.
        void setCursorWidth(int p_width);

        void checkCenterCursor();

        void setCenterCursor(CenterCursor p_centerCursor);

        void setExpandTab(bool p_enable);

        bool isTabExpanded() const;

        void setTabStopWidthInSpaces(int p_spaces);

        int getTabStopWidthInSpaces() const;

        void setSpaceWidth(qreal p_width);

        void insertFromMimeDataOfBase(const QMimeData *p_source);

        QVariant inputMethodQuery(Qt::InputMethodQuery p_query) const Q_DECL_OVERRIDE;

        void setInputMethodEnabled(bool p_enabled);

    signals:
        void cursorLineChanged();

        void cursorWidthChanged();

        void resized();

        void contentsChanged();

        void mouseReleased(QMouseEvent *p_event);

        // Emit when canInsertFromMimeData() is called.
        // @p_allowed: whether we can insert @p_source.
        // @p_handled: whether it is handled.
        void canInsertFromMimeDataRequested(const QMimeData *p_source, bool *p_handled, bool *p_allowed);

        // Emit when insertFromMimeData() is called.
        // @p_handled: whether it is handled.
        void insertFromMimeDataRequested(const QMimeData *p_source, bool *p_handled);

        // Emit when contextMenuEvent() is called.
        // @p_handled: whether it is handled.
        // @p_menu: the menu to show if handled.
        void contextMenuEventRequested(QContextMenuEvent *p_event, bool *p_handled, QScopedPointer<QMenu> *p_menu);

        void preKeyReturn(int p_modifiers, bool *p_changed, bool *p_handled);

        void postKeyReturn(int p_modifiers);

        void preKeyTab(int p_modifiers, bool *p_handled);

        void preKeyBacktab(int p_modifiers, bool *p_handled);

    protected:
        void resizeEvent(QResizeEvent *p_event) Q_DECL_OVERRIDE;

        void keyPressEvent(QKeyEvent *p_event) Q_DECL_OVERRIDE;

        bool eventFilter(QObject *p_obj, QEvent *p_event) Q_DECL_OVERRIDE;

        bool canInsertFromMimeData(const QMimeData *p_source) const Q_DECL_OVERRIDE;

        void insertFromMimeData(const QMimeData *p_source) Q_DECL_OVERRIDE;

        void contextMenuEvent(QContextMenuEvent *p_event) Q_DECL_OVERRIDE;

    private slots:
        void handleCursorPositionChange();

        void updateCursorWidthToNextChar();

        // Handle key press that input mode does not handle.
        virtual void handleDefaultKeyPress(QKeyEvent *p_event);

    private:
        enum class DrawCursorAsBlock
        {
            None,
            Half,
            Full
        };

        template <typename T>
        QList<QTextCursor> findAllTextInDocument(const T &p_text,
                                                 QTextDocument::FindFlags p_flags,
                                                 int p_start,
                                                 int p_end);

        template <typename T>
        QTextCursor findTextInDocument(const T &p_text,
                                       QTextDocument::FindFlags p_flags,
                                       int p_start);

        QString getSelectedText(const Selection &p_selection) const;

        // Return true if the event is handled.
        bool handleKeyTab(QKeyEvent *p_event);

        // Return true if the event is handled.
        bool handleKeyBacktab(QKeyEvent *p_event);

        // Insert a closing bracket after an opening bracket if needed.
        bool handleOpeningBracket(const QChar &p_open, const QChar &p_close);

        // Skip a closing bracket if needed.
        bool handleClosingBracket(const QChar &p_open, const QChar &p_close);

        bool handleBracketRemoval();

        bool handleKeyReturn(QKeyEvent *p_event);

        // Whether the char at @p_pib is escpaed.
        static bool isEscaped(const QString &p_text, int p_pib);

        static QChar matchingClosingBracket(const QChar &p_open);

        int m_cursorLine = -1;

        // Input mode.
        QSharedPointer<AbstractInputMode> m_inputMode;

        // Whether draw cursor as block instead of a thin line.
        // QPlaintTextEdit will draw cursor as block in overwrite mode, while
        // QTextEdit will not.
        // Here we will set the cursor width to the width of next char to appear
        // as a block cursor.
        DrawCursorAsBlock m_drawCursorAsBlock = DrawCursorAsBlock::None;

        // Managed by QObject.
        QTimer *m_updateCursorWidthTimer = nullptr;

        VTextEdit::Selections m_selections;

        // To calculate the interval after last cursor position change.
        QTime m_cursorPositionChangeTime;

        const int c_updateCursorWidthTimerInterval = 50;

        CenterCursor m_centerCursor = CenterCursor::NeverCenter;

        // Whether expand Tab into spaces.
        bool m_expandTab = true;

        // Translate Tab into spaces.
        int m_tabStopWidthInSpaces = 4;

        // Font point size of the width of space char.
        qreal m_spaceWidth = 0;

        // The document revision that has changes to the contents.
        int m_lastDocumentRevisionWithChanges = 0;

        // Whether enable input method.
        bool m_inputMethodEnabled = true;

        bool m_autoBracketsEnabled = true;

        bool m_selectionChangedByOverride = false;
    };

    template <typename T>
    QList<QTextCursor> VTextEdit::findAllTextInDocument(const T &p_text,
                                                        QTextDocument::FindFlags p_flags,
                                                        int p_start,
                                                        int p_end)
    {
        QList<QTextCursor> results;
        auto doc = document();
        int start = p_start;
        int end = p_end == -1 ? doc->characterCount() + 1 : p_end;

        // BUG: QTextDocument::find() does not work with FindFlag::FindBackward.
        p_flags &= ~QTextDocument::FindFlag::FindBackward;
        while (start < end) {
            QTextCursor cursor = doc->find(p_text, start, p_flags);
            if (cursor.isNull()) {
                break;
            } else {
                start = cursor.selectionEnd();
                if (start <= end) {
                    results.append(cursor);
                }
            }
        }

        return results;
    }

    template <typename T>
    QTextCursor VTextEdit::findTextInDocument(const T &p_text,
                                              QTextDocument::FindFlags p_flags,
                                              int p_start)
    {
        // BUG: QTextDocument::find() does not work with FindFlag::FindBackward.
        if (p_flags & QTextDocument::FindFlag::FindBackward) {
            p_flags &= ~QTextDocument::FindFlag::FindBackward;
            // Find the last match that locates before @p_start.
            QTextCursor lastMatch;
            auto doc = document();
            int start = 0;
            bool wrapped = false;
            while (true) {
                // find() will only search from start to the end.
                auto cursor = doc->find(p_text, start, p_flags);
                if (cursor.isNull()) {
                    break;
                }

                if (cursor.selectionStart() < p_start) {
                    lastMatch = cursor;
                } else {
                    if (wrapped) {
                        // Search till the end and get the last one.
                        lastMatch = cursor;
                    } else {
                        if (lastMatch.isNull()) {
                            wrapped = true;
                        } else {
                            break;
                        }
                    }
                }
                start = cursor.selectionEnd();
            }
            return lastMatch;
        } else {
            auto cursor = document()->find(p_text, p_start, p_flags);
            if (p_start > 0 && cursor.isNull()) {
                // Wrap.
                cursor = document()->find(p_text, 0, p_flags);
            }
            return cursor;
        }
    }
}
#endif
