#ifndef ABSTRACTINPUTMODE_H
#define ABSTRACTINPUTMODE_H

#include <QSharedPointer>
#include <QString>

#include <vtextedit/global.h>

class QKeyEvent;

namespace vte {
class InputModeEditorInterface;
class InputModeStatusWidget;

class AbstractInputMode {
public:
  explicit AbstractInputMode(InputModeEditorInterface *p_interface);

  virtual ~AbstractInputMode();

  virtual QString name() const = 0;

  virtual InputMode mode() const = 0;

  virtual EditorMode editorMode() const = 0;

  // Get status widget of input mode.
  // Could be NULL.
  // The caller should hold this pointer during the usage and explicitly
  // remove the widget from QObject system at the end.
  virtual QSharedPointer<InputModeStatusWidget> statusWidget() = 0;

  virtual void activate() = 0;

  virtual void deactivate() = 0;

  virtual void focusIn() = 0;

  virtual void focusOut() = 0;

  // Return true if the key press is handled inside.
  virtual bool handleKeyPress(QKeyEvent *p_event) = 0;

  // Return true if the shortcut key event is handled inside.
  // If stolen, Qt will replay this key event immediately as an ordinary
  // KeyPress.
  virtual bool stealShortcut(QKeyEvent *p_event) = 0;

  // Hook that will be called before/after the default KeyPress event handle.
  virtual void preKeyPressDefaultHandle(QKeyEvent *p_event) = 0;
  virtual void postKeyPressDefaultHandle(QKeyEvent *p_event) = 0;

protected:
  InputModeEditorInterface *m_interface = nullptr;
};
} // namespace vte

#endif // ABSTRACTINPUTMODE_H
