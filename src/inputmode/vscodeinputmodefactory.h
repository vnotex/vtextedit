#ifndef VSCODEINPUTMODEFACTORY_H
#define VSCODEINPUTMODEFACTORY_H

#include "abstractinputmodefactory.h"

namespace vte {
class VscodeInputModeFactory : public AbstractInputModeFactory {
public:
  VscodeInputModeFactory();

  QSharedPointer<AbstractInputMode>
  createInputMode(InputModeEditorInterface *p_interface) Q_DECL_OVERRIDE;

  QString name() const Q_DECL_OVERRIDE;

  QString description() const Q_DECL_OVERRIDE;
};
} // namespace vte

#endif // VSCODEINPUTMODEFACTORY_H
