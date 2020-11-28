#ifndef KATEVI_VIUTILS_H
#define KATEVI_VIUTILS_H

#include <QString>

namespace KateVi
{
    class ViUtils
    {
    public:
        ViUtils() = delete;

        static bool isModifier(int p_keyCode);

        static bool isRegister(QChar p_char);
    };
}

#endif // KATEVI_VIUTILS_H
