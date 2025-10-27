#include "viinputmodefactory.h"

#include "viinputmode.h"

#include <katevi/globalstate.h>
#include <katevi/interface/kateviconfig.h>
#include <vtextedit/global.h>

using namespace vte;

ViInputModeFactory::ViInputModeFactory()
    : m_viGlobal(new KateVi::GlobalState()), m_viConfig(new KateViI::KateViConfig()) {}

ViInputModeFactory::~ViInputModeFactory() {}

QSharedPointer<AbstractInputMode>
ViInputModeFactory::createInputMode(InputModeEditorInterface *p_interface) {
  return QSharedPointer<ViInputMode>::create(p_interface, m_viGlobal, m_viConfig);
}

QString ViInputModeFactory::name() const { return QStringLiteral("vi"); }

QString ViInputModeFactory::description() const { return VTextEditTranslate::tr("Vi input mode"); }

void ViInputModeFactory::updateViConfig(const QSharedPointer<KateViI::KateViConfig> &p_config) {
  // Only update the contents.
  if (!p_config) {
    return;
  }
  *m_viConfig = *p_config;
}
