#ifndef VTEXTEDIT_VTEXTEDITOR_H
#define VTEXTEDIT_VTEXTEDITOR_H

#include <vtextedit/vtextedit_export.h>
#include <vtextedit/global.h>

#include <QWidget>
#include <QSharedPointer>
#include <QScopedPointer>
#include <QTextCursor>
#include <QPalette>
#include <QFont>

class QTextDocument;
class QMenu;
class QTimer;

namespace vte
{
    class TextEditorConfig;
    struct TextEditorParameters;
    class VTextEdit;
    class IndicatorsBorder;
    class EditorIndicatorsBorder;
    class TextFolding;
    class ExtraSelectionMgr;
    class EditorExtraSelection;
    class VSyntaxHighlighter;
    class Theme;
    class TextBlockRange;
    class EditorInputMode;
    class AbstractInputMode;
    class EditorCompleter;
    class Completer;
    class StatusIndicator;

    class VTEXTEDIT_EXPORT VTextEditor : public QWidget
    {
        Q_OBJECT

        friend class EditorIndicatorsBorder;
        friend class EditorExtraSelection;
        friend class EditorCompleter;

    public:
        enum class LineNumberType
        {
            None,
            Absolute,
            Relative
        };

        // Result of one find action.
        struct FindResult
        {
            int m_totalMatches = 0;

            int m_currentMatchIndex = -1;

            bool m_wrapped = false;
        };

        VTextEditor(const QSharedPointer<TextEditorConfig> &p_config,
                    const QSharedPointer<TextEditorParameters> &p_paras,
                    QWidget *p_parent = nullptr);

        virtual ~VTextEditor();

        VTextEdit *getTextEdit() const;

        virtual void setSyntax(const QString &p_syntax);
        virtual QString getSyntax() const;

        void setInputMode(InputMode p_mode);
        QSharedPointer<AbstractInputMode> getInputMode() const;

        TextFolding *getTextFolding() const;

        void triggerCompletion(bool p_reversed);

        bool isCompletionActive() const;

        void completionNext(bool p_reversed);

        void completionExecute();

        void abortCompletion();

        // Get the status widget of this editor, which could be inserted into
        // the status bar.
        // The caller should hold this pointer during the usage and explicitly
        // remove the widget from QObject system at the end.
        QSharedPointer<QWidget> statusWidget();

        EditorMode getEditorMode() const;

        QTextDocument *document() const;

        // Get current theme.
        QSharedPointer<Theme> theme() const;

        QString getText() const;
        void setText(const QString &p_text);

        bool isReadOnly() const;
        void setReadOnly(bool p_enabled);

        bool isModified() const;
        void setModified(bool p_modified);

        const QString &getBasePath() const;
        void setBasePath(const QString &p_basePath);

        const TextEditorConfig &getConfig() const;
        void setConfig(const QSharedPointer<TextEditorConfig> &p_config);

        // Get cursor position <line, positionInLine>, based on 0.
        QPair<int, int> getCursorPosition() const;

        // Get the top visible line number.
        int getTopLine() const;

        // Scroll to @p_lineNumber to let it be the first visible line.
        // If cursor is not visible then set the cursor to it if @p_forceCursor is false.
        void scrollToLine(int p_lineNumber, bool p_forceCursor);

        // For Vi mode, enter Insert mode.
        void enterInsertModeIfApplicable();

        void insertText(const QString &p_text);

        int zoomDelta() const;

        int editorFontPointSize() const;

        // @p_delta: font point size added to the base font size.
        virtual void zoom(int p_delta);

        void peekText(const QString &p_text, FindFlags p_flags);

        // @p_currentMatchLine: whether constrain current match on a given line.
        VTextEditor::FindResult findText(const QStringList &p_texts,
                                         FindFlags p_flags,
                                         int p_start = 0,
                                         int p_end = -1,
                                         int p_currentMatchLine = -1);

        VTextEditor::FindResult replaceText(const QString &p_text,
                                            FindFlags p_flags,
                                            const QString &p_replaceText,
                                            int p_start = 0,
                                            int p_end = -1);

        VTextEditor::FindResult replaceAll(const QString &p_text,
                                           FindFlags p_flags,
                                           const QString &p_replaceText,
                                           int p_start = 0,
                                           int p_end = -1);

        void clearIncrementalSearchHighlight();

        void clearSearchHighlight();

        // Enable internal context menu.
        // Used if you are not going to customize the context menu in a subclass.
        void enableInternalContextMenu();

        void updateIndicatorsBorder();

        void setLeaderKeyToSkip(int p_key, Qt::KeyboardModifiers p_modifiers);

        // Custom search paths for KSyntaxHighlighting Definition files.
        // Will search ./syntax and ./themes folder.
        static void addSyntaxCustomSearchPaths(const QStringList &p_paths);

    signals:
        void syntaxChanged();

        void modeChanged();

        void focusIn();

        void focusOut();

        void topLineChanged();

    protected:
        void focusInEvent(QFocusEvent *p_event) Q_DECL_OVERRIDE;

        void focusOutEvent(QFocusEvent *p_event) Q_DECL_OVERRIDE;

        bool eventFilter(QObject *p_obj, QEvent *p_event) Q_DECL_OVERRIDE;

        bool appendSpellCheckMenu(QContextMenuEvent *p_event, QMenu *p_menu);

        void updateSpellCheck();

    private slots:
        void updateCursorOfStatusWidget();

        void updateSyntaxOfStatusWidget();

        void updateModeOfStatusWidget();

        void updateInputModeStatusWidget();

        void clearFindResultCache();

    private:
        void setupUI();

        void setupIndicatorsBorder();

        void setupTextEdit();

        void setupTextFolding();

        void setupExtraSelection();

        void updateExtraSelectionMgrFromConfig();

        void updateIndicatorsBorderFromConfig();

        QSharedPointer<TextBlockRange> fetchSyntaxFoldingRangeStartingOnBlock(int p_blockNumber);

        void setupCompleter();

        Completer *completer() const;

        StatusIndicator *createStatusWidget();

        void updateStatusWidget();

        void updateFromConfig();

        void setFontPointSizeByStyleSheet(int p_ptSize);

        void setFontAndPaletteByStyleSheet(const QFont &p_font, const QPalette &p_palette);

        const QList<QTextCursor> &findAllText(const QStringList &p_texts, FindFlags p_flags, int p_start, int p_end);

        void highlightSearch(const QList<QTextCursor> &p_results, int p_currentIdx);

        // @p_skipCurrent: if current cursor locates right at a match, whether skip it.
        // @p_cursor: pass in current cursor and get the cursor of current match.
        VTextEditor::FindResult findTextHelper(const QStringList &p_texts,
                                               FindFlags p_flags,
                                               int p_start,
                                               int p_end,
                                               bool p_skipCurrent,
                                               QTextCursor &p_cursor);

        void updateInputMethodEnabled();

        void updateSpaceWidth();

        static void resolveBackReferenceInReplaceText(QString &p_replaceText,
                                                      QString p_text,
                                                      const QRegularExpression &p_regExp);

        // @p_cursors is in ascending order.
        // If @p_forward is true, find the smallest cursor whose selection start is greater
        // than @p_pos or the first cursor if wrapped.
        // Otherwise, find the largest cursor whose selection start is smaller than @p_pos
        // or the last cursor if wrapped.
        static int selectCursor(const QList<QTextCursor> &p_cursors,
                                int p_pos,
                                bool p_skipCurrent,
                                bool p_forward,
                                bool &p_isWrapped);

    protected:
        // Managed by QObject.
        VTextEdit *m_textEdit = nullptr;

        // Managed by QObject.
        VSyntaxHighlighter *m_highlighter = nullptr;

    private:
        struct FindResultCache
        {
            void clear();

            bool matched(const QStringList &p_texts, FindFlags p_flags, int p_start, int p_end) const;

            void update(const QStringList &p_texts,
                        FindFlags p_flags,
                        int p_start,
                        int p_end,
                        const QList<QTextCursor> &p_result);

            // Find range [m_start, m_end).
            int m_start = -1;
            int m_end = -1;

            QStringList m_texts;

            FindFlags m_flags = FindFlag::None;

            QList<QTextCursor> m_result;
        };

        QSharedPointer<TextEditorConfig> m_config;

        QSharedPointer<TextEditorParameters> m_parameters;

        // Managed by QObject.
        IndicatorsBorder *m_indicatorsBorder = nullptr;

        QScopedPointer<EditorIndicatorsBorder> m_indicatorsBorderInterface;

        // Managed by QObject.
        ExtraSelectionMgr *m_extraSelectionMgr = nullptr;

        QScopedPointer<EditorExtraSelection> m_extraSelectionInterface;

        // Extra selection registered for incremental search.
        int m_incrementalSearchExtraSelection = -1;

        // Extra selection registered for search.
        int m_searchExtraSelection = -1;

        // Extra selection registered for search under cursor.
        int m_searchUnderCursorExtraSelection = -1;

        // Managed by QObject.
        TextFolding *m_folding = nullptr;

        // Syntax for highlighter.
        QString m_syntax;

        QScopedPointer<EditorInputMode> m_inputModeInterface;

        QScopedPointer<EditorCompleter> m_completerInterface;

        QSharedPointer<StatusIndicator> m_statusIndicator;

        // Path to search for resources, such as images.
        QString m_basePath;

        int m_zoomDelta = 0;

        // Used to indicate current font point size of editor.
        int m_editorFontPointSize = 0;

        FindResultCache m_findResultCache;

        // When using style sheet, m_textEdit->font() and palette() won't return the actual ones.
        // We store them on our own.
        QFont m_themeFont;
        QPalette m_themePalette;

        LineEnding m_lineEnding = LineEnding::LF;

        QTimer *m_topLineChangedTimer = nullptr;

        static int s_instanceCount;

        // Completer shared among all instances.
        static Completer *s_completer;
    };
}

#endif
