#ifndef KATEVICONFIG_H
#define KATEVICONFIG_H

#include <katevi/katevi_export.h>

#include <set>

namespace KateViI
{
    class KATEVI_EXPORT KateViConfig
    {
    public:
        KateViConfig()
        {
            initSkippedKeys();
        }

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

        bool shouldSkipKey(int p_key, Qt::KeyboardModifiers p_modifiers) const
        {
            if (m_skippedKeys.find(Key(p_key, p_modifiers)) != m_skippedKeys.end()) {
                return true;
            }

            return false;
        }

    private:
        struct Key
        {
            Key(int p_key, Qt::KeyboardModifiers p_modifiers)
                : m_key(p_key),
                  m_modifiers(p_modifiers)
            {
            }

            bool operator==(const Key &p_other) const
            {
                return m_key == p_other.m_key && m_modifiers == p_other.m_modifiers;
            }

            bool operator<(const Key &p_other) const
            {
                if (m_key < p_other.m_key) {
                    return true;
                } else if (m_key > p_other.m_key) {
                    return false;
                } else {
                    return m_modifiers < p_other.m_modifiers;
                }
            }

            int m_key = 0;

            Qt::KeyboardModifiers m_modifiers = Qt::NoModifier;
        };

        void initSkippedKeys()
        {
            m_skippedKeys.emplace(Qt::Key_Tab, Qt::ControlModifier);
            m_skippedKeys.emplace(Qt::Key_Backtab, Qt::ControlModifier | Qt::ShiftModifier);
        }

        int m_tabWidth = 4;

        bool m_wordCompletionRemoveTailEnabled = false;

        bool m_stealShortcut = false;

        // Keys to skip in Vi Normal/Visual mode.
        std::set<Key> m_skippedKeys;
    };
}

#endif // KATEVICONFIG_H
