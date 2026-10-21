#ifndef VTEXTEDIT_TYPSTCOMPILER_H
#define VTEXTEDIT_TYPSTCOMPILER_H

#include <vtextedit/typsteditorconfig.h>
#include <vtextedit/vtextedit_export.h>

#include <QByteArray>
#include <QImage>
#include <QMetaType>
#include <QObject>
#include <QScopedPointer>
#include <QStringList>
#include <QVector>

namespace vte {

// Asynchronous, widget-independent compilation using the optional Typst helper.
// Use on its owning thread with a running Qt event loop; destruction cancels pending work.
class VTEXTEDIT_EXPORT TypstCompiler : public QObject {
  Q_OBJECT
public:
  struct Result {
    // One entry per page, in document order. Only the requested format is populated.
    // Failure leaves both vectors empty. Returned values own their data.
    QVector<QByteArray> m_svgPages;
    QVector<QImage> m_bitmapPages;
    QString m_error;
    // Compilation warnings/errors, including source identity and UTF-16 offset if available.
    QStringList m_diagnostics;

    bool isSuccess() const { return m_error.isEmpty(); }
  };

  struct ImageReferences {
    QStringList m_images;
    QStringList m_strings;
    bool m_complete = false;
  };

  // Parse 1–3 sources without evaluation, project access or the preview process.
  // Only complete scans can prove absence; incomplete scans still report known literals.
  // Requires an existing absolute helper path. Blocks without pumping GUI events,
  // with a single 10-second process deadline and 16 MiB request/response limits.
  // Failure returns an empty vector; p_error is cleared on entry.
  static QVector<ImageReferences> collectImageReferences(const QStringList &p_sources,
                                                         const QString &p_helperExecutable,
                                                         QString *p_error = nullptr);

  // Copies project, helper, font/package, timeout and resource settings.
  // Editor presentation settings and debounce are unused. Empty root uses the current directory.
  explicit TypstCompiler(const TypstEditorConfig &p_config = TypstEditorConfig(),
                         QObject *p_parent = nullptr);
  ~TypstCompiler() Q_DECL_OVERRIDE;

  // Text replaces m_mainFile in memory (CR/CRLF normalized to LF), never on disk.
  // Imports resolve within m_projectRoot. Pages retain their Typst size, excluding bleed.
  // False means busy: no work queued and no finished signal for the rejected call.
  bool compileToSvg(const QString &p_text);
  // Native RGBA bitmap; 1 pixel per point = 72 dpi. Scale must be finite and positive.
  bool compileToBitmap(const QString &p_text, qreal p_pixelsPerPoint = 1.0);
  bool isCompiling() const;

signals:
  // Exactly once, asynchronously, for each accepted call while this object is alive.
  // All-or-nothing result, with total page bytes bounded by m_maxCacheBytes.
  // The compiler is idle here; slots may immediately start another job.
  void finished(const vte::TypstCompiler::Result &p_result);

private:
  class Private;
  QScopedPointer<Private> d;
};

} // namespace vte

Q_DECLARE_METATYPE(vte::TypstCompiler::Result)

#endif // VTEXTEDIT_TYPSTCOMPILER_H
