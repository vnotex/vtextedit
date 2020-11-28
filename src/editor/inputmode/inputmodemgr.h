#ifndef INPUTMODEMGR_H
#define INPUTMODEMGR_H

#include <QVector>
#include "abstractinputmodefactory.h"
#include <vtextedit/global.h>

namespace vte
{
    class InputModeMgr
    {
    public:
        static InputModeMgr &getInst();

        const QVector<QSharedPointer<AbstractInputModeFactory>> &getInputModeFactories() const;

        const QSharedPointer<AbstractInputModeFactory> &getInputModeFactory(InputMode p_mode) const;

    private:
        InputModeMgr();

        QVector<QSharedPointer<AbstractInputModeFactory>> m_factories;
    };
}

#endif // INPUTMODEMGR_H
