#include "inputmodemgr.h"

#include "normalinputmodefactory.h"
#include "viinputmodefactory.h"
#include "vscodeinputmodefactory.h"

using namespace vte;

InputModeMgr::InputModeMgr() { m_factories.resize(InputMode::MaxInputMode); }

InputModeMgr &InputModeMgr::getInst() {
  static InputModeMgr mgr;
  return mgr;
}

const QSharedPointer<AbstractInputModeFactory> &InputModeMgr::getFactory(InputMode p_mode) {
  Q_ASSERT(p_mode < InputMode::MaxInputMode);
  if (!m_factories[p_mode]) {
    m_factories[p_mode] = createModeFactory(p_mode);
  }

  return m_factories[p_mode];
}

QSharedPointer<AbstractInputModeFactory> InputModeMgr::createModeFactory(InputMode p_mode) {
  switch (p_mode) {
  case InputMode::NormalMode:
    return QSharedPointer<NormalInputModeFactory>::create();
    break;

  case InputMode::ViMode:
    return QSharedPointer<ViInputModeFactory>::create();
    break;

  case InputMode::VscodeMode:
    return QSharedPointer<VscodeInputModeFactory>::create();
    break;

  default:
    Q_ASSERT(false);
    return nullptr;
  }
}
