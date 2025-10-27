#ifndef INPUTMODESTATUSWIDGET_H
#define INPUTMODESTATUSWIDGET_H

#include <QObject>
#include <QSharedPointer>

class QWidget;

namespace vte {
class InputModeStatusWidget : public QObject {
  Q_OBJECT
public:
  explicit InputModeStatusWidget(QObject *p_parent = nullptr) : QObject(p_parent) {}

  virtual ~InputModeStatusWidget() { Q_ASSERT(!parent()); }

  virtual QSharedPointer<QWidget> widget() = 0;

signals:
  // The status widget get the focus, like the command bar of Vi.
  void focusIn();

  // The status widget lose the focus. Editor should take care of this
  // and focus the right widget later.
  void focusOut();
};
} // namespace vte

#endif // INPUTMODESTATUSWIDGET_H
