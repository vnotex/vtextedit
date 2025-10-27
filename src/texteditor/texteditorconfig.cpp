#include <vtextedit/texteditorconfig.h>

using namespace vte;

QSharedPointer<Theme> TextEditorConfig::defaultTheme() {
  static QSharedPointer<Theme> theme = nullptr;

  if (!theme) {
    theme =
        Theme::createThemeFromFile(QStringLiteral(":/vtextedit/editor/data/themes/default.theme"));
  }

  return theme;
}
