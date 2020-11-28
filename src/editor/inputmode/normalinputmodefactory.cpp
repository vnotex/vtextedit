#include "normalinputmodefactory.h"

#include <QObject>

#include "normalinputmode.h"

using namespace vte;

NormalInputModeFactory::NormalInputModeFactory()
{
}

QSharedPointer<AbstractInputMode> NormalInputModeFactory::createInputMode(InputModeEditorInterface *p_interface)
{
    return QSharedPointer<NormalInputMode>::create(p_interface);
}

QString NormalInputModeFactory::name() const
{
    return QStringLiteral("normal");
}

QString NormalInputModeFactory::description() const
{
    return QObject::tr("Normal input mode");
}
