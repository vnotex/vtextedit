#ifndef VIINPUTMODEFACTORY_H
#define VIINPUTMODEFACTORY_H

#include "abstractinputmodefactory.h"

namespace KateVi
{
    class GlobalState;
}

namespace KateViI
{
    class KateViConfig;
}

namespace vte
{
    class ViInputModeFactory : public AbstractInputModeFactory
    {
    public:
        ViInputModeFactory();

        ~ViInputModeFactory();

        QSharedPointer<AbstractInputMode> createInputMode(InputModeEditorInterface *p_interface) Q_DECL_OVERRIDE;

        QString name() const Q_DECL_OVERRIDE;

        QString description() const Q_DECL_OVERRIDE;

    private:
        KateVi::GlobalState *m_viGlobal = nullptr;

        KateViI::KateViConfig *m_viConfig = nullptr;
    };
}

#endif // VIINPUTMODEFACTORY_H
