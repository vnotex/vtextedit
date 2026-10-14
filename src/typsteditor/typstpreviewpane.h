#ifndef VTEXTEDIT_TYPSTPREVIEWPANE_H
#define VTEXTEDIT_TYPSTPREVIEWPANE_H

#include <QAbstractScrollArea>
#include <QScopedPointer>

#include "typstcompiledsnapshot.h"

namespace vte {

class TypstEditorConfig;
class TypstPreviewPanePrivate;

class TypstPreviewPane : public QAbstractScrollArea {
  Q_OBJECT
  Q_PROPERTY(QString selectedText READ selectedText NOTIFY selectionChanged)
  Q_PROPERTY(bool hasPendingUpdate READ hasPendingUpdate NOTIFY updateAvailableChanged)
  Q_PROPERTY(qreal zoom READ zoom WRITE setZoom)

public:
  explicit TypstPreviewPane(const TypstEditorConfig &p_config, QWidget *p_parent = nullptr);
  ~TypstPreviewPane() Q_DECL_OVERRIDE;

  void applyConfig(const TypstEditorConfig &p_config);

  void setCurrentGeneration(quint64 p_epoch, quint64 p_generation);
  void setSnapshot(const TypstSnapshot &p_snapshot, const QByteArray &p_pdf);
  TypstSnapshot snapshot() const;

  QString selectedText() const;
  bool hasSelection() const;
  bool hasPendingUpdate() const;

  void flashSourceLocation(quint64 p_snapshot, int p_page, const QPointF &p_point);
  void clearSourceHighlight();

public Q_SLOTS:
  void publishPending();
  void clearSelection();
  void copy();
  void selectPageText();

  void setZoom(qreal p_zoom);
  qreal zoom() const;
  void fitWidth();
  void fitPage();
  void setSearchText(const QString &p_text);
  void findNext(bool p_backwards = false);
  void goToPage(int p_page, const QPointF &p_point = QPointF());
  int currentPage() const;

signals:
  void sourceLocationRequested(quint64 p_snapshot, int p_page, QPointF p_point);
  void externalLinkRequested(QUrl p_url);
  void selectionChanged();
  // Actual vertical scrolling/navigation, not zoom, relayout or publication.
  void verticalScrollChanged();
  void updateAvailableChanged(bool p_available);
  void snapshotPublished(quint64 p_snapshot);
  void errorOccurred(QString p_message);

protected:
  bool event(QEvent *p_event) Q_DECL_OVERRIDE;
  void paintEvent(QPaintEvent *p_event) Q_DECL_OVERRIDE;
  void resizeEvent(QResizeEvent *p_event) Q_DECL_OVERRIDE;
  void scrollContentsBy(int p_dx, int p_dy) Q_DECL_OVERRIDE;
  void mousePressEvent(QMouseEvent *p_event) Q_DECL_OVERRIDE;
  void mouseMoveEvent(QMouseEvent *p_event) Q_DECL_OVERRIDE;
  void mouseReleaseEvent(QMouseEvent *p_event) Q_DECL_OVERRIDE;
  void keyPressEvent(QKeyEvent *p_event) Q_DECL_OVERRIDE;
  void wheelEvent(QWheelEvent *p_event) Q_DECL_OVERRIDE;
  void contextMenuEvent(QContextMenuEvent *p_event) Q_DECL_OVERRIDE;

private:
  QScopedPointer<TypstPreviewPanePrivate> m_d;
};

} // namespace vte

#endif
