#ifndef VTEXTEDIT_TYPSTFRAGMENTRENDERER_H
#define VTEXTEDIT_TYPSTFRAGMENTRENDERER_H

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

// Asynchronous, isolated Typst math and markup using the application-owned runtime.
// Use on its owning thread with a running Qt event loop. The runtime must outlive
// this object; destruction retires pending work without waiting for native calls.
class VTEXTEDIT_EXPORT TypstFragmentRenderer : public QObject {
  Q_OBJECT
public:
  struct Result {
    // Owning UTF-8 SVG and its intrinsic viewport. Math includes transparent
    // padding; markup merges all pages with an 8pt gap. Failure clears geometry.
    // Math baseline is top-relative and may lie outside the viewport; markup is 0.
    // Raw markup SVG can contain link annotations. Hosts must sanitize native SVG
    // before inserting it into a live browser document.
    QByteArray m_svg;
    QSizeF m_sizePt;
    qreal m_baselinePt = 0;
    int m_pageCount = 0;
    QString m_error;
    QStringList m_diagnostics;

    bool isSuccess() const { return m_error.isEmpty(); }
  };

  explicit TypstFragmentRenderer(TypstRuntime *p_runtime, QObject *p_parent = nullptr);
  ~TypstFragmentRenderer() Q_DECL_OVERRIDE;

  // Delimiter-free Typst math, normalized from CR/CRLF to LF, at a 12pt text size.
  // Source must be nonblank, NUL-free Unicode of at most 64 KiB UTF-8. Foreground
  // must be valid. Display changes math style, not Markdown block placement.
  // False means busy: no work queued and no finished signal for the rejected call.
  // Each accepted call has a 30-second deadline, including time queued for a lane.
  // Sensitive calls clear currently stored global comemo results when native work
  // returns, including failure/unwind. Cancellation does not interrupt execution;
  // concurrent calls may retain/reinsert results. This does not erase heap memory.
  bool renderMathToSvg(const QString &p_source, bool p_display, const QColor &p_foreground,
                       bool p_sensitive = false);
  // Full Typst markup with compact transparent page defaults below authored styles.
  // Only the virtual main source is readable; files, packages and dates are denied.
  // Shares the validation, busy/deadline/cancellation and sensitive policy above.
  bool renderMarkupToSvg(const QString &p_source, const QColor &p_foreground,
                         bool p_sensitive = false);
  bool isRendering() const;
  // Completes accepted work with a cancellation error; idle cancellation is a no-op.
  // Native work already executing is suppressed, never forcibly interrupted.
  void cancel();

signals:
  // Exactly once, asynchronously, per accepted call while this object is alive.
  // The renderer is idle here; slots may submit another request or destroy it.
  void finished(const vte::TypstFragmentRenderer::Result &p_result);

private:
  class Private;
  QScopedPointer<Private> d;
};

} // namespace vte

Q_DECLARE_METATYPE(vte::TypstFragmentRenderer::Result)

#endif // VTEXTEDIT_TYPSTFRAGMENTRENDERER_H
