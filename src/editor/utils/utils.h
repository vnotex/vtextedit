#ifndef UTILS_H
#define UTILS_H

class QToolButton;

namespace vte
{
    class Utils
    {
    public:
        Utils() = delete;

        static void sleepWait(int p_milliseconds);

        static void removeMenuIndicator(QToolButton *p_btn);
    };
}

#endif // UTILS_H
