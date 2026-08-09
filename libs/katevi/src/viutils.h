#ifndef KATEVI_VIUTILS_H
#define KATEVI_VIUTILS_H

#include <QString>

namespace KateVi {
class ViUtils {
public:
  ViUtils() = delete;

  static bool isModifier(int p_keyCode);

  static bool isRegister(QChar p_char);

  // Judge the OS CTRL.
  static bool isControlModifier(int p_modifiers);

  static Qt::KeyboardModifier controlModifier();
};
} // namespace KateVi

#endif // KATEVI_VIUTILS_H
