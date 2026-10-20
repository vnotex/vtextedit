#ifndef VTE_TYPSTCOMPILERBRIDGE_H
#define VTE_TYPSTCOMPILERBRIDGE_H

#include "typstcompiledsnapshot.h"
#include <vtextedit/typsteditorconfig.h>

#include <QFileSystemWatcher>
#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QVector>

namespace vte {

// One serialized helper operation, one coalesced newest compile, no GUI-thread compiler work.
class TypstCompilerBridge : public QObject {
  Q_OBJECT
public:
  enum class State { Idle, Starting, Compiling, Ready, Error, Unavailable };
  enum class Mode { Preview, Export };
  explicit TypstCompilerBridge(QObject *p_parent = nullptr, Mode p_mode = Mode::Preview);
  ~TypstCompilerBridge() Q_DECL_OVERRIDE;

  void updateProject(const TypstEditorConfig &p_config, const QString &p_activeFile,
                     const QMap<QString, QString> &p_overlays);
  void resetForNewDocument();
  void finishDocumentReset();
  void retry();
  void setDisplayedSnapshot(const TypstSnapshot &p_snapshot);
  void setInplaceSnapshot(const TypstSnapshot &p_snapshot);
  void requestPdf(quint64 p_snapshot);
  void requestRaster(quint64 p_epoch, quint64 p_snapshot, int p_page, qreal p_scale);
  void requestSvg(quint64 p_epoch, quint64 p_snapshot, int p_page);
  void requestImageData(quint64 p_epoch, quint64 p_snapshot, quint64 p_image,
                        quint64 p_consumerRequest);
  // Zero requests explicit navigation; nonzero identifies a replaceable cursor highlight.
  void sourceToOutput(const QString &p_file, int p_offset, quint64 p_cursorRequest);
  void outputToSource(quint64 p_snapshot, int p_page, const QPointF &p_point);
  quint64 epoch() const { return m_epoch; }
  quint64 generation() const { return m_generation; }
  TypstSnapshot latestSnapshot() const { return m_latest; }

signals:
  void generationChanged(quint64 p_epoch, quint64 p_generation);
  void stateChanged(vte::TypstCompilerBridge::State p_state, const QString &p_message);
  void highlightsReady(quint64 p_generation, const QVector<vte::TypstHighlight> &p_ranges,
                       const QString &p_activeFile);
  void diagnosticsReady(const QVector<vte::TypstDiagnostic> &p_diagnostics);
  void snapshotReady(const vte::TypstSnapshot &p_snapshot);
  void pdfReady(const vte::TypstSnapshot &p_snapshot, const QByteArray &p_pdf);
  // Each admitted raster completes once: a ready image (null on failure), or cancellation.
  void pageImageReady(quint64 p_epoch, quint64 p_snapshot, int p_page, qreal p_scale,
                      const QImage &p_image);
  // Empty data completes a failed or cancelled SVG request.
  void pageSvgReady(quint64 p_epoch, quint64 p_snapshot, int p_page, const QByteArray &p_svg);
  // Superseded inputs are not failures and must not be retried against their old snapshot.
  void rasterCancelled(quint64 p_epoch, quint64 p_snapshot, int p_page, qreal p_scale);
  void imageDataReady(quint64 p_requestId, const QByteArray &p_data, const QString &p_mime,
                      const QString &p_error);
  void outputLocationReady(quint64 p_snapshot, int p_page, const QPointF &p_point);
  void cursorLocationReady(quint64 p_cursorRequest, quint64 p_snapshot, int p_page,
                           const QPointF &p_point);
  void sourceLocationReady(const QString &p_file, int p_start, int p_end);
  void externalLinkRequested(const QUrl &p_url);
  void operationFailed(const QString &p_operation, const QString &p_message);

private:
  void invalidate();
  void start();
  void stop(bool p_failure = false);
  void pump();
  void send(QJsonObject p_header);
  void enqueue(QJsonObject p_header);
  void readOutput();
  void handleReply(const QJsonObject &p_header, QByteArray p_payload);
  void fail(const QString &p_message, bool p_unavailable = false);
  void finishRequest();
  void completeRaster(const QJsonObject &p_request, bool p_cancelled);
  void completeImage(const QJsonObject &p_request, const QByteArray &p_data, const QString &p_mime,
                     const QString &p_error);
  void updateDependencies(const QJsonArray &p_dependencies);
  QJsonObject configuration() const;
  QJsonArray retained(bool p_forCompile) const;
  bool isCurrent(const QJsonObject &p_header) const;

  const Mode m_mode;
  TypstEditorConfig m_config;
  QString m_activeFile;
  QMap<QString, QString> m_overlays;
  QProcess m_process;
  QFileSystemWatcher m_watcher;
  QTimer m_debounce;
  QTimer m_timeout;
  quint64 m_epoch = 1;
  quint64 m_generation = 0;
  quint64 m_nextRequest = 1;
  quint64 m_displayed = 0;
  quint64 m_inplace = 0;
  TypstSnapshot m_latest;
  QJsonObject m_request;
  quint64 m_nextImageRequest = 1;
  QHash<quint64, quint64> m_imageRequests;
  QVector<QJsonObject> m_queue;
  bool m_ready = false;
  bool m_failed = false;
  bool m_compilePending = false;
  bool m_compileDue = false;
  bool m_stopping = false;
  bool m_documentReset = false;
  char m_prefix[8] = {};
  qint64 m_prefixRead = 0;
  QByteArray m_header;
  QByteArray m_payload;
  qint64 m_headerRead = 0;
  qint64 m_payloadRead = 0;
};

} // namespace vte

#endif // VTE_TYPSTCOMPILERBRIDGE_H
