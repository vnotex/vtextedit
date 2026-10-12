#ifndef VTEXTEDIT_TYPSTEDITORCONFIG_H
#define VTEXTEDIT_TYPSTEDITORCONFIG_H

#include <vtextedit/texteditorconfig.h>
#include <vtextedit/vtextedit_export.h>

#include <QSharedPointer>
#include <QStringList>

namespace vte {

class VTEXTEDIT_EXPORT TypstEditorConfig {
public:
  explicit TypstEditorConfig(
      const QSharedPointer<TextEditorConfig> &p_config = QSharedPointer<TextEditorConfig>())
      : m_textEditorConfig(p_config ? p_config : QSharedPointer<TextEditorConfig>::create()) {}

  QSharedPointer<TextEditorConfig> m_textEditorConfig;
  // Empty uses the helper installed beside the application, then PATH.
  QString m_helperExecutable;
  QString m_projectRoot;
  QString m_mainFile = QStringLiteral("main.typ");
  QStringList m_fontPaths;
  QStringList m_packagePaths;
  bool m_allowDownloads = false;
  int m_debounceMs = 150;
  int m_compileTimeoutMs = 30000;
  qint64 m_maxPdfBytes = 64LL * 1024 * 1024;
  qint64 m_maxCacheBytes = 256LL * 1024 * 1024;
  int m_maxRasterPixels = 16 * 1024 * 1024;
  qreal m_previewMaxHeight = 480;
  qreal m_initialZoom = 1.0;
};

} // namespace vte

#endif // VTEXTEDIT_TYPSTEDITORCONFIG_H
