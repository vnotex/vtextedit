#include "vscodeinputmodefactory.h"

#include "vscodeinputmode.h"
#include <vtextedit/global.h>

using namespace vte;

VscodeInputModeFactory::VscodeInputModeFactory() {}

QSharedPointer<AbstractInputMode>
VscodeInputModeFactory::createInputMode(InputModeEditorInterface *p_interface) {
  return QSharedPointer<VscodeInputMode>::create(p_interface);
}

QString VscodeInputModeFactory::name() const { return QStringLiteral("vscode"); }

QString VscodeInputModeFactory::description() const {
  return VTextEditTranslate::tr("VSCode input mode");
}
