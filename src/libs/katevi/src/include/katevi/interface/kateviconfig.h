#ifndef KATEVICONFIG_H
#define KATEVICONFIG_H

#include <katevi/katevi_export.h>

namespace KateViI
{
    class KATEVI_EXPORT KateViConfig
    {
    public:
        int tabWidth() const
        {
            return m_tabWidth;
        }

        bool wordCompletionRemoveTail() const
        {
            return m_wordCompletionRemoveTailEnabled;
        }

        bool stealShortcut() const
        {
            return m_stealShortcut;
        }

    private:
        int m_tabWidth = 4;

        bool m_wordCompletionRemoveTailEnabled = false;

        bool m_stealShortcut = false;
    };
}

#endif // KATEVICONFIG_H
