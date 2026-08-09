#ifndef NORMALINPUTMODEFACTORY_H
#define NORMALINPUTMODEFACTORY_H

#include "abstractinputmodefactory.h"

namespace vte {
class NormalInputModeFactory : public AbstractInputModeFactory {
public:
  NormalInputModeFactory();

  QSharedPointer<AbstractInputMode>
  createInputMode(InputModeEditorInterface *p_interface) Q_DECL_OVERRIDE;

  QString name() const Q_DECL_OVERRIDE;

  QString description() const Q_DECL_OVERRIDE;
};
} // namespace vte

#endif // NORMALINPUTMODEFACTORY_H
