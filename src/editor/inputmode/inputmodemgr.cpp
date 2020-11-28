#include "inputmodemgr.h"

#include "normalinputmodefactory.h"
#include "viinputmodefactory.h"

using namespace vte;

InputModeMgr::InputModeMgr()
{
    m_factories.resize(InputMode::MaxInputMode);

    // Normal mode.
    m_factories[InputMode::NormalMode] = QSharedPointer<NormalInputModeFactory>::create();

    // Vi mode.
    m_factories[InputMode::ViMode] = QSharedPointer<ViInputModeFactory>::create();
}

InputModeMgr &InputModeMgr::getInst()
{
    static InputModeMgr mgr;
    return mgr;
}

const QVector<QSharedPointer<AbstractInputModeFactory>> &InputModeMgr::getInputModeFactories() const
{
    return m_factories;
}

const QSharedPointer<AbstractInputModeFactory> &InputModeMgr::getInputModeFactory(InputMode p_mode) const
{
    if (p_mode >= InputMode::MaxInputMode) {
        p_mode = InputMode::NormalMode;
    }

    return m_factories[p_mode];
}
