#ifndef INPUTMODEMGR_H
#define INPUTMODEMGR_H

#include <QVector>
#include "abstractinputmodefactory.h"
#include <vtextedit/global.h>
#include <utils/noncopyable.h>

namespace vte
{
    class InputModeMgr : public Noncopyable
    {
    public:
        static InputModeMgr &getInst();

        const QSharedPointer<AbstractInputModeFactory> &getFactory(InputMode p_mode);

    private:
        InputModeMgr();

        static QSharedPointer<AbstractInputModeFactory> createModeFactory(InputMode p_mode);

        QVector<QSharedPointer<AbstractInputModeFactory>> m_factories;
    };
}

#endif // INPUTMODEMGR_H
