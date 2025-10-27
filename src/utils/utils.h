#ifndef UTILS_H
#define UTILS_H

#include <QString>

class QToolButton;

namespace vte {
class Utils {
public:
  Utils() = delete;

  static void sleepWait(int p_milliseconds);

  static void removeMenuIndicator(QToolButton *p_btn);

  static bool isFilePath(const QString &p_name);
};
} // namespace vte

#endif // UTILS_H
