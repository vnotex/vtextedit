#include "abstractinputmode.h"
#include "inputmodeeditorinterface.h"

using namespace vte;

AbstractInputMode::AbstractInputMode(InputModeEditorInterface *p_interface)
    : m_interface(p_interface) {}

AbstractInputMode::~AbstractInputMode() {}
