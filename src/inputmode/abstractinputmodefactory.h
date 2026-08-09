#ifndef ABSTRACTINPUTMODEFACTORY_H
#define ABSTRACTINPUTMODEFACTORY_H

#include <QSharedPointer>
#include <QString>

namespace vte {
class AbstractInputMode;
class InputModeEditorInterface;

class AbstractInputModeFactory {
public:
  virtual ~AbstractInputModeFactory() {}

  virtual QSharedPointer<AbstractInputMode>
  createInputMode(InputModeEditorInterface *p_interface) = 0;

  virtual QString name() const = 0;

  virtual QString description() const = 0;
};
} // namespace vte

#endif // ABSTRACTINPUTMODEFACTORY_H
