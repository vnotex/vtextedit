#ifndef INPUTMODEMGR_H
#define INPUTMODEMGR_H

#include "abstractinputmodefactory.h"
#include <QVector>
#include <utils/noncopyable.h>
#include <vtextedit/global.h>

namespace vte {
class InputModeMgr : public Noncopyable {
public:
  static InputModeMgr &getInst();

  const QSharedPointer<AbstractInputModeFactory> &getFactory(InputMode p_mode);

private:
  InputModeMgr();

  static QSharedPointer<AbstractInputModeFactory> createModeFactory(InputMode p_mode);

  QVector<QSharedPointer<AbstractInputModeFactory>> m_factories;
};
} // namespace vte

#endif // INPUTMODEMGR_H
