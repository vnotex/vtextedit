#ifndef VTEXTEDIT_TYPSTMATHRENDERER_H
#define VTEXTEDIT_TYPSTMATHRENDERER_H

#include <vtextedit/vtextedit_export.h>

#include <QByteArray>
#include <QColor>
#include <QMetaType>
#include <QObject>
#include <QScopedPointer>
#include <QSizeF>
#include <QStringList>

namespace vte {

class TypstRuntime;

// Asynchronous, isolated Typst math using the application-owned runtime.
// Use on its owning thread with a running Qt event loop. The runtime must outlive
// this object; destruction retires pending work without waiting for native calls.
class VTEXTEDIT_EXPORT TypstMathRenderer : public QObject {
  Q_OBJECT
public:
  struct Result {
    // Owning UTF-8 SVG and its intrinsic viewport, including transparent padding.
    // Failure leaves SVG empty and geometry zero. Baseline is measured from the top
    // and may lie outside the viewport for an explicit Typst baseline override.
    QByteArray m_svg;
    QSizeF m_sizePt;
    qreal m_baselinePt = 0;
    QString m_error;
    QStringList m_diagnostics;

    bool isSuccess() const { return m_error.isEmpty(); }
  };

  explicit TypstMathRenderer(TypstRuntime *p_runtime, QObject *p_parent = nullptr);
  ~TypstMathRenderer() Q_DECL_OVERRIDE;

  // Delimiter-free Typst math, normalized from CR/CRLF to LF, at a 12pt text size.
  // Source must be nonblank, NUL-free Unicode of at most 64 KiB UTF-8. Foreground
  // must be valid. Display changes math style, not Markdown block placement.
  // False means busy: no work queued and no finished signal for the rejected call.
  // Each accepted call has a 30-second deadline, including time queued for a lane.
  bool renderToSvg(const QString &p_source, bool p_display, const QColor &p_foreground);
  bool isRendering() const;
  // Completes accepted work with a cancellation error; idle cancellation is a no-op.
  // Native work already executing is suppressed, never forcibly interrupted.
  void cancel();

signals:
  // Exactly once, asynchronously, per accepted call while this object is alive.
  // The renderer is idle here; slots may submit another request or destroy it.
  void finished(const vte::TypstMathRenderer::Result &p_result);

private:
  class Private;
  QScopedPointer<Private> d;
};

} // namespace vte

Q_DECLARE_METATYPE(vte::TypstMathRenderer::Result)

#endif // VTEXTEDIT_TYPSTMATHRENDERER_H
