#ifndef VTE_TYPSTPREVIEWHOST_H
#define VTE_TYPSTPREVIEWHOST_H

#include "typstcompiledsnapshot.h"
#include <QImage>
#include <QObject>
#include <QScopedPointer>

namespace vte {
class TypstEditorConfig;
class TypstSourceEditor;

class TypstPreviewHost : public QObject {
  Q_OBJECT
public:
  TypstPreviewHost(TypstSourceEditor *p_editor, const TypstEditorConfig &p_config,
                   QObject *p_parent = nullptr);
  ~TypstPreviewHost() Q_DECL_OVERRIDE;

  void setCurrentGeneration(quint64 p_epoch, quint64 p_generation);
  void setSnapshot(const TypstSnapshot &p_snapshot);
  TypstSnapshot snapshot() const;
  void clear();
  void setEnabled(bool p_enabled);
  // A null image is a terminal failure for this request, not an automatic retry.
  void setPageImage(quint64 p_epoch, quint64 p_snapshot, int p_page, qreal p_scale,
                    const QImage &p_image);
  void cancelRaster(quint64 p_epoch, quint64 p_snapshot, int p_page, qreal p_scale);
  void setActiveFile(const QString &p_file);
  void applyConfig(const TypstEditorConfig &p_config);

signals:
  void rasterRequested(quint64 p_epoch, quint64 p_snapshot, int p_page, qreal p_scale);
  void errorOccurred(const QString &p_message);
  void snapshotPublished(const vte::TypstSnapshot &p_snapshot);
  void resourceContextMenuRequested(const vte::TypstResourceContext &p_context, QMenu *p_menu);
  void imageActivated(const vte::TypstResourceContext &p_context);
  void linkActivated(const vte::TypstResourceContext &p_context);
  void sourceLocationRequested(quint64 p_snapshot, int p_page, QPointF p_point);

protected:
  bool eventFilter(QObject *p_object, QEvent *p_event) Q_DECL_OVERRIDE;

private:
  class Private;
  QScopedPointer<Private> d;
};
} // namespace vte

#endif // VTE_TYPSTPREVIEWHOST_H
