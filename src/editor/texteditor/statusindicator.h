#ifndef STATUSINDICATOR_H
#define STATUSINDICATOR_H

#include <QWidget>
#include <QSharedPointer>

#include <vtextedit/global.h>

class QLabel;

namespace vte
{
    class InputModeStatusWidget;

    class StatusIndicator : public QWidget
    {
        Q_OBJECT
    public:
        explicit StatusIndicator(QWidget *p_parent = nullptr);

        ~StatusIndicator();

        void updateCursor(int p_lineCount, int p_line, int p_column);

        void updateSyntax(const QString &p_syntax);

        void updateMode(const EditorMode& p_mode);

        void updateInputModeStatusWidget(const QSharedPointer<InputModeStatusWidget> &p_statusWidget);

        const QSharedPointer<InputModeStatusWidget> &getInputModeStatusWidget() const;

    signals:
        void focusIn();

        void focusOut();

    private slots:
        void hideInputModeStatusWidget();

    private:
        void setupUI();

        static QString generateCursorLabelText(int p_lineCount, int p_line, int p_column);

        QLabel *m_cursorLabel = nullptr;

        QLabel *m_syntaxLabel = nullptr;

        QLabel *m_modeLabel = nullptr;

        QSharedPointer<InputModeStatusWidget> m_inputModeWidget;

        static const char *c_cursorLabelProperty;

        static const char *c_syntaxLabelProperty;

        static const char *c_modeLabelProperty;
    };
}

#endif // STATUSINDICATOR_H
