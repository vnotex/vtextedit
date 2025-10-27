#ifndef KATEVICONFIG_H
#define KATEVICONFIG_H

#include <Qt>

#include <katevi/katevi_export.h>

#include <unordered_set>

namespace KateViI {
class KATEVI_EXPORT KateViConfig {
public:
  KateViConfig();

  int tabWidth() const;

  void setTabWidth(int p_width);

  bool wordCompletionRemoveTail() const;

  bool stealShortcut() const;

  bool shouldSkipKey(int p_key, Qt::KeyboardModifiers p_modifiers) const;

  void skipKey(int p_key, Qt::KeyboardModifiers p_modifiers);

  KateViConfig &operator=(const KateViConfig &p_other) = default;

private:
  struct Key {
    Key(int p_key, Qt::KeyboardModifiers p_modifiers);

    Key(const QString &p_key);

    bool operator==(const Key &p_other) const;

    bool operator<(const Key &p_other) const;

    QString toString() const;

    int m_key = 0;

    Qt::KeyboardModifiers m_modifiers = Qt::NoModifier;
  };

  class KeyHashFunc {
  public:
    size_t operator()(const Key &p_key) const { return p_key.m_key + (int)p_key.m_modifiers; }
  };

  void initSkippedKeys();

  int m_tabWidth = 4;

  bool m_wordCompletionRemoveTailEnabled = false;

  bool m_stealShortcut = false;

  // Keys to skip in Vi Normal/Visual mode.
  std::unordered_set<Key, KeyHashFunc> m_skippedKeys;
};
} // namespace KateViI

#endif // KATEVICONFIG_H
