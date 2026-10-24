#ifndef VTEXTEDIT_TYPSTEDITORCONFIG_H
#define VTEXTEDIT_TYPSTEDITORCONFIG_H

#include <vtextedit/texteditorconfig.h>
#include <vtextedit/vtextedit_export.h>

#include <QColor>
#include <QSharedPointer>
#include <QStringList>

namespace vte {

class TypstRuntime;

class VTEXTEDIT_EXPORT TypstEditorConfig {
public:
  explicit TypstEditorConfig(
      const QSharedPointer<TextEditorConfig> &p_config = QSharedPointer<TextEditorConfig>())
      : m_textEditorConfig(p_config ? p_config : QSharedPointer<TextEditorConfig>::create()) {}

  // The theme's typst-editor-styles.Text fonts default both source and compiled output.
  QSharedPointer<TextEditorConfig> m_textEditorConfig;
  // Application-owned; must outlive every editor/compiler using this configuration.
  // A missing or stopped runtime leaves source editing available without compilation.
  TypstRuntime *m_runtime = nullptr;
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
  // Scaling bound for each page fragment, including padding. The complete
  // sequential preview is not height-capped and scrolls with the source editor.
  qreal m_previewMaxHeight = 480;
  qreal m_initialZoom = 1.0;
  // Canvas color; also the default page fill when theme styling is enabled.
  // Invalid leaves native Typst defaults and uses QPalette::Dark for the canvas.
  QColor m_pdfBackground;
  // Preview-only color defaults. Authored styles win; TypstCompiler exports ignore these.
  bool m_enableThemeStyleInPreview = true;
  QColor m_previewForeground;
};

} // namespace vte

#endif // VTEXTEDIT_TYPSTEDITORCONFIG_H
