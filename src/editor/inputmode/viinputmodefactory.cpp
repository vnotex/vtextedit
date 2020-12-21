#include "viinputmodefactory.h"

#include "viinputmode.h"

#include <katevi/globalstate.h>
#include <katevi/interface/kateviconfig.h>
#include <vtextedit/global.h>

using namespace vte;

ViInputModeFactory::ViInputModeFactory()
    : m_viGlobal(new KateVi::GlobalState()),
      m_viConfig(new KateViI::KateViConfig())
{
}

ViInputModeFactory::~ViInputModeFactory()
{
    delete m_viGlobal;
    delete m_viConfig;
}

QSharedPointer<AbstractInputMode> ViInputModeFactory::createInputMode(InputModeEditorInterface *p_interface)
{
    return QSharedPointer<ViInputMode>::create(p_interface, m_viGlobal, m_viConfig);
}

QString ViInputModeFactory::name() const
{
    return QStringLiteral("vi");
}

QString ViInputModeFactory::description() const
{
    return VTextEditTranslate::tr("Vi input mode");
}
