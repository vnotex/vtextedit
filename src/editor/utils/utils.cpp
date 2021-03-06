#include "utils.h"

#include <QElapsedTimer>
#include <QCoreApplication>

using namespace vte;

void Utils::sleepWait(int p_milliseconds)
{
    if (p_milliseconds <= 0) {
        return;
    }

    QElapsedTimer t;
    t.start();
    while (t.elapsed() < p_milliseconds) {
        QCoreApplication::processEvents();
    }
}
