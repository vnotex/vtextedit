#ifndef VTEXTEDIT_VTEXTEDITOR_H
#define VTEXTEDIT_VTEXTEDITOR_H

#include <vtextedit/vtextedit_export.h>
#include <vtextedit/global.h>

#include <QWidget>
#include <QSharedPointer>
#include <QScopedPointer>

class QTextDocument;

namespace vte
{
    class TextEditorConfig;
    class VTextEdit;
    class IndicatorsBorder;
    class EditorIndicatorsBorder;
    class TextFolding;
    class ExtraSelectionMgr;
    class EditorExtraSelection;
    class SyntaxHighlighter;
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

        VTextEditor(const QSharedPointer<TextEditorConfig> &p_config,
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

        // @p_delta: font point size added to the base font size.
        virtual void zoom(int p_delta);

        // Custom search paths for KSyntaxHighlighting Definition files.
        // Will search ./syntax and ./themes folder.
        static void addSyntaxCustomSearchPaths(const QStringList &p_paths);

    signals:
        void syntaxChanged();

        void modeChanged();

        void focusIn();

        void focusOut();

    protected:
        void focusInEvent(QFocusEvent *p_event) Q_DECL_OVERRIDE;

        void focusOutEvent(QFocusEvent *p_event) Q_DECL_OVERRIDE;

        bool eventFilter(QObject *p_obj, QEvent *p_event) Q_DECL_OVERRIDE;

    private slots:
        void updateCursorOfStatusWidget();

        void updateSyntaxOfStatusWidget();

        void updateModeOfStatusWidget();

        void updateInputModeStatusWidget();

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

        StatusIndicator *createStatusWidget() const;

        void updateStatusWidget();

        void updateFromConfig();

        void setFontPointSizeByStyleSheet(int p_ptSize);

        void setFontAndPaletteByStyleSheet(const QFont &p_font, const QPalette &p_palette);

    protected:
        // Managed by QObject.
        VTextEdit *m_textEdit = nullptr;

    private:
        QSharedPointer<TextEditorConfig> m_config;

        // Managed by QObject.
        IndicatorsBorder *m_indicatorsBorder = nullptr;

        QScopedPointer<EditorIndicatorsBorder> m_indicatorsBorderInterface;

        // Managed by QObject.
        ExtraSelectionMgr *m_extraSelectionMgr = nullptr;

        QScopedPointer<EditorExtraSelection> m_extraSelectionInterface;

        // Managed by QObject.
        TextFolding *m_folding = nullptr;

        // Managed by QObject.
        SyntaxHighlighter *m_highlighter = nullptr;

        // Syntax for highlighter.
        QString m_syntax;

        QScopedPointer<EditorInputMode> m_inputModeInterface;

        QScopedPointer<EditorCompleter> m_completerInterface;

        QSharedPointer<StatusIndicator> m_statusIndicator;

        // Path to search for resources, such as images.
        QString m_basePath;

        int m_zoomDelta = 0;

        static int s_instanceCount;

        // Completer shared among all instances.
        static Completer *s_completer;
    };
}

#endif
