#ifndef VIINPUTMODEFACTORY_H
#define VIINPUTMODEFACTORY_H

#include "abstractinputmodefactory.h"

#include <QSharedPointer>

namespace KateVi {
class GlobalState;
}

namespace KateViI {
class KateViConfig;
}

namespace vte {
class ViInputModeFactory : public AbstractInputModeFactory {
public:
  ViInputModeFactory();

  ~ViInputModeFactory();

  QSharedPointer<AbstractInputMode>
  createInputMode(InputModeEditorInterface *p_interface) Q_DECL_OVERRIDE;

  QString name() const Q_DECL_OVERRIDE;

  QString description() const Q_DECL_OVERRIDE;

  void updateViConfig(const QSharedPointer<KateViI::KateViConfig> &p_config);

private:
  // All Vi instances share the same global state and config.
  // Should only update the contents of them.
  const QSharedPointer<KateVi::GlobalState> m_viGlobal;

  const QSharedPointer<KateViI::KateViConfig> m_viConfig;
};
} // namespace vte

#endif // VIINPUTMODEFACTORY_H
