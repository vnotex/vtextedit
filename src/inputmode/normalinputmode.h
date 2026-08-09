#ifndef NORMALINPUTMODE_H
#define NORMALINPUTMODE_H

#include "abstractinputmode.h"

namespace vte {
class NormalInputMode : public AbstractInputMode {
public:
  explicit NormalInputMode(InputModeEditorInterface *p_interface);

  QString name() const Q_DECL_OVERRIDE;

  InputMode mode() const Q_DECL_OVERRIDE;

  EditorMode editorMode() const Q_DECL_OVERRIDE;

  QSharedPointer<InputModeStatusWidget> statusWidget() Q_DECL_OVERRIDE;

  void activate() Q_DECL_OVERRIDE;

  void deactivate() Q_DECL_OVERRIDE;

  void focusIn() Q_DECL_OVERRIDE;

  void focusOut() Q_DECL_OVERRIDE;

  bool handleKeyPress(QKeyEvent *p_event) Q_DECL_OVERRIDE;

  bool stealShortcut(QKeyEvent *p_event) Q_DECL_OVERRIDE;

  void preKeyPressDefaultHandle(QKeyEvent *p_event) Q_DECL_OVERRIDE;

  void postKeyPressDefaultHandle(QKeyEvent *p_event) Q_DECL_OVERRIDE;

private:
  void enterInsertMode();

  void enterOverwriteMode();

  void commandCompleteNext();

  void commandCompletePrevious();

  EditorMode m_mode = EditorMode::NormalModeInsert;
};
} // namespace vte

#endif // NORMALINPUTMODE_H
