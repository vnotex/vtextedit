#ifndef VTEXTEDIT_TYPSTRUNTIME_H
#define VTEXTEDIT_TYPSTRUNTIME_H

#include <vtextedit/vtextedit_export.h>

#include <QObject>
#include <QScopedPointer>

namespace vte {

class TypstRuntimePrivate;

// Application-owned native Typst workers. Construct after QApplication and before
// all editors/compilers, and keep alive until those consumers have been destroyed.
// No implicit global runtime is created for an editor with a missing runtime.
class VTEXTEDIT_EXPORT TypstRuntime : public QObject {
  Q_OBJECT
public:
  explicit TypstRuntime(QObject *p_parent = nullptr);
  ~TypstRuntime() Q_DECL_OVERRIDE;

  bool isAvailable() const;
  // Stops admission and joins native work without processing GUI events. A native
  // call cannot be forcibly interrupted, so draining may wait indefinitely. This
  // is application shutdown, not editor-close cancellation. Call on the owning thread.
  void shutdown();

private:
  Q_DISABLE_COPY(TypstRuntime)
  friend class TypstRuntimePrivate;
  QScopedPointer<TypstRuntimePrivate> d;
};

} // namespace vte

#endif // VTEXTEDIT_TYPSTRUNTIME_H
