#ifndef STATUSINDICATOR_H
#define STATUSINDICATOR_H

#include <QSharedPointer>
#include <QWidget>

#include <vtextedit/global.h>

class QLabel;
class QToolButton;

namespace vte {
class InputModeStatusWidget;

class StatusIndicator : public QWidget {
  Q_OBJECT
public:
  explicit StatusIndicator(QWidget *p_parent = nullptr);

  ~StatusIndicator();

  void updateCursor(int p_lineCount, int p_line, int p_column);

  void updateSyntax(const QString &p_syntax);

  void updateMode(const EditorMode &p_mode);

  void updateInputModeStatusWidget(const QSharedPointer<InputModeStatusWidget> &p_statusWidget);

  void updateSpellCheck(bool p_spellCheckEnabled, bool p_autoDetectLanguageEnabled,
                        const QString &p_currentLanguage,
                        const QMap<QString, QString> &p_dictionaries);

  const QSharedPointer<InputModeStatusWidget> &getInputModeStatusWidget() const;

signals:
  void focusIn();

  void focusOut();

  void spellCheckChanged(bool p_enabled, bool p_autoDetect, const QString &p_currentLang);

private slots:
  void hideInputModeStatusWidget();

private:
  void setupUI();

  void signalSpellCheckChanged();

  static QString generateCursorLabelText(int p_lineCount, int p_line, int p_column);

  QToolButton *m_spellCheckBtn = nullptr;

  QLabel *m_cursorLabel = nullptr;

  QLabel *m_syntaxLabel = nullptr;

  QLabel *m_modeLabel = nullptr;

  QSharedPointer<InputModeStatusWidget> m_inputModeWidget;

  bool m_spellCheckEnabled = false;

  bool m_autoDetectLanguageEnabled = false;

  QString m_defaultSpellCheckLanguage;

  static const char *c_cursorLabelProperty;

  static const char *c_syntaxLabelProperty;

  static const char *c_modeLabelProperty;
};
} // namespace vte

#endif // STATUSINDICATOR_H
