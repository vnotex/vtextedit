#ifndef VTEXTEDIT_VRICHTEXTEDITOR_H
#define VTEXTEDIT_VRICHTEXTEDITOR_H

#include "vtextedit_export.h"

#include <QScopedPointer>
#include <QSharedPointer>
#include <QWidget>

#include <vtextedit/global.h>
#include <vtextedit/richtexteditorconfig.h>

class QTextDocument;

namespace vte {
class AbstractInputMode;
class InputModeStatusWidget;
class TextEditInputMode;
class VTextEdit;

// A rich text editor supporting the Normal/Vi/VSCode input modes.
class VTEXTEDIT_EXPORT VRichTextEditor : public QWidget {
  Q_OBJECT
public:
  explicit VRichTextEditor(const QSharedPointer<RichTextEditorConfig> &p_config = nullptr,
                           QWidget *p_parent = nullptr);

  ~VRichTextEditor();

  VTextEdit *getTextEdit() const;

  QTextDocument *document() const;

  void setInputMode(InputMode p_mode);

  QSharedPointer<AbstractInputMode> getInputMode() const;

  // Status widget published by the current input mode. Could be null.
  QSharedPointer<QWidget> inputModeStatusWidget() const;

  EditorMode getEditorMode() const;

  bool isReadOnly() const;
  void setReadOnly(bool p_enabled);

  void setHtml(const QString &p_html);
  QString toHtml() const;

  bool isModified() const;
  void setModified(bool p_modified);

signals:
  void modeChanged();

  void focusIn();

  void focusOut();

  // Emitted when the input mode publishes or withdraws its status widget.
  void inputModeStatusWidgetChanged(QSharedPointer<QWidget> p_widget);

private slots:
  void updateInputMethodEnabled();

private:
  void setupUI();

  void updateFromConfig();

  void updateInputModeStatusWidget();

  // Unmount the currently published input mode status widget, if any.
  void detachInputModeStatusWidget();

  QSharedPointer<RichTextEditorConfig> m_config;

  // Managed by QObject.
  VTextEdit *m_textEdit = nullptr;

  QScopedPointer<TextEditInputMode> m_inputModeInterface;

  // The status widget currently published to the host.
  QSharedPointer<InputModeStatusWidget> m_inputModeStatusWidget;

  QMetaObject::Connection m_inputModeFocusConnection;
};
} // namespace vte

#endif // VTEXTEDIT_VRICHTEXTEDITOR_H
