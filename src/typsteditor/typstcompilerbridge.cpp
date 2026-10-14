#include "typstcompilerbridge.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>
#include <QStandardPaths>
#include <QtEndian>

#include <cmath>
#include <limits>
#include <utility>

using namespace vte;

namespace {
constexpr quint32 c_maxHeader = 16U * 1024 * 1024;
constexpr quint32 c_maxPayload = 128U * 1024 * 1024;
constexpr double c_maxId = 9007199254740991.0;

quint64 identifier(const QJsonValue &p_value) {
  const double value = p_value.toDouble(-1);
  return p_value.isDouble() && std::isfinite(value) && value >= 1 && value <= c_maxId &&
                 std::floor(value) == value
             ? static_cast<quint64>(value)
             : 0;
}

int integer(const QJsonValue &p_value, int p_default = -1) {
  const double value = p_value.toDouble(std::numeric_limits<double>::quiet_NaN());
  return std::isfinite(value) && value >= std::numeric_limits<int>::min() &&
                 value <= std::numeric_limits<int>::max() && std::floor(value) == value
             ? static_cast<int>(value)
             : p_default;
}

bool finitePoint(const QPointF &p_point) {
  return std::isfinite(p_point.x()) && std::isfinite(p_point.y());
}

TypstSourceRange sourceRange(const QJsonObject &p_object) {
  TypstSourceRange range;
  range.m_file = p_object.value(QStringLiteral("file")).toString();
  range.m_start = integer(p_object.value(QStringLiteral("start")));
  range.m_end = integer(p_object.value(QStringLiteral("end")));
  return range;
}

QVector<TypstHighlight> highlights(const QJsonArray &p_array) {
  QVector<TypstHighlight> ranges;
  ranges.reserve(p_array.size());
  for (const auto &value : p_array) {
    const auto object = value.toObject();
    TypstHighlight range;
    range.m_source = sourceRange(object);
    range.m_kind = object.value(QStringLiteral("kind")).toString();
    if (range.m_source.isValid() && !range.m_kind.isEmpty()) {
      ranges.append(range);
    }
  }
  return ranges;
}

QVector<TypstDiagnostic> diagnostics(const QJsonArray &p_array) {
  QVector<TypstDiagnostic> result;
  result.reserve(p_array.size());
  for (const auto &value : p_array) {
    const auto object = value.toObject();
    TypstDiagnostic diagnostic;
    diagnostic.m_source = sourceRange(object);
    diagnostic.m_severity = object.value(QStringLiteral("severity")).toString();
    diagnostic.m_message = object.value(QStringLiteral("message")).toString();
    if (!diagnostic.m_message.isEmpty()) {
      result.append(diagnostic);
    }
  }
  return result;
}

QRectF rectangle(const QJsonObject &p_object) {
  const auto nan = std::numeric_limits<double>::quiet_NaN();
  return QRectF(p_object.value(QStringLiteral("x")).toDouble(nan),
                p_object.value(QStringLiteral("y")).toDouble(nan),
                p_object.value(QStringLiteral("width")).toDouble(nan),
                p_object.value(QStringLiteral("height")).toDouble(nan));
}

bool validRectangle(const QRectF &p_rect) {
  return finitePoint(p_rect.topLeft()) && finitePoint(p_rect.bottomRight()) && p_rect.width() > 0 &&
         p_rect.height() > 0;
}

TypstSnapshot snapshotFromJson(const QJsonObject &p_header) {
  auto result = QSharedPointer<TypstCompiledSnapshot>::create();
  result->m_epoch = identifier(p_header.value(QStringLiteral("epoch")));
  result->m_generation = identifier(p_header.value(QStringLiteral("generation")));
  result->m_id = identifier(p_header.value(QStringLiteral("snapshot")));
  result->m_activeFile = p_header.value(QStringLiteral("activeFile")).toString();
  const auto pages = p_header.value(QStringLiteral("pages")).toArray();
  if (!result->m_id || pages.isEmpty()) {
    return {};
  }
  for (const auto &value : pages) {
    const auto page = value.toArray();
    if (page.size() != 2) {
      return {};
    }
    const QSizeF size(page[0].toDouble(-1), page[1].toDouble(-1));
    if (!std::isfinite(size.width()) || !std::isfinite(size.height()) || size.width() <= 0 ||
        size.height() <= 0) {
      return {};
    }
    result->m_pages.append(size);
  }
  QSet<quint64> ids;
  for (const auto &value : p_header.value(QStringLiteral("previews")).toArray()) {
    const auto object = value.toObject();
    TypstPreviewOccurrence occurrence;
    occurrence.m_id = identifier(object.value(QStringLiteral("id")));
    occurrence.m_parent = identifier(object.value(QStringLiteral("parent")));
    occurrence.m_kind = object.value(QStringLiteral("kind")).toString();
    occurrence.m_status = object.value(QStringLiteral("status")).toString();
    occurrence.m_block = object.value(QStringLiteral("block")).toBool();
    occurrence.m_source = sourceRange(object);
    if (!occurrence.m_id || ids.contains(occurrence.m_id)) {
      return {};
    }
    ids.insert(occurrence.m_id);
    for (const auto &part : object.value(QStringLiteral("regions")).toArray()) {
      const auto regionObject = part.toObject();
      TypstPreviewRegion region;
      region.m_page = integer(regionObject.value(QStringLiteral("page")));
      region.m_rect = rectangle(regionObject);
      if (region.m_page < 0 || region.m_page >= result->m_pages.size() ||
          !validRectangle(region.m_rect)) {
        return {};
      }
      occurrence.m_regions.append(region);
    }
    result->m_previews.append(occurrence);
  }
  for (const auto &value : p_header.value(QStringLiteral("links")).toArray()) {
    const auto object = value.toObject();
    TypstLink link;
    link.m_page = integer(object.value(QStringLiteral("page")));
    link.m_rect = rectangle(object);
    link.m_targetPage = integer(object.value(QStringLiteral("targetPage")));
    link.m_target = QPointF(object.value(QStringLiteral("targetX")).toDouble(),
                            object.value(QStringLiteral("targetY")).toDouble());
    link.m_url = QUrl(object.value(QStringLiteral("url")).toString());
    if (link.m_page < 0 || link.m_page >= result->m_pages.size() || !validRectangle(link.m_rect) ||
        !finitePoint(link.m_target) || link.m_targetPage >= result->m_pages.size()) {
      return {};
    }
    result->m_links.append(link);
  }
  result->m_highlights = highlights(p_header.value(QStringLiteral("highlights")).toArray());
  result->m_diagnostics = diagnostics(p_header.value(QStringLiteral("diagnostics")).toArray());
  for (const auto &value : p_header.value(QStringLiteral("dependencies")).toArray()) {
    result->m_dependencies.append(value.toObject().value(QStringLiteral("path")).toString());
  }
  return result;
}
} // namespace

TypstCompilerBridge::TypstCompilerBridge(QObject *p_parent) : QObject(p_parent) {
  m_debounce.setSingleShot(true);
  m_timeout.setSingleShot(true);
  connect(&m_debounce, &QTimer::timeout, this, [this]() {
    m_compileDue = true;
    pump();
  });
  connect(&m_timeout, &QTimer::timeout, this,
          [this]() { fail(tr("Typst operation timed out. Edit the source or retry.")); });
  connect(&m_process, &QProcess::started, this, [this]() {
    QJsonObject request{{QStringLiteral("op"), QStringLiteral("initialize")},
                        {QStringLiteral("config"), configuration()}};
    send(request);
  });
  connect(&m_process, &QProcess::readyReadStandardOutput, this, &TypstCompilerBridge::readOutput);
  connect(&m_process, &QProcess::readyReadStandardError, this, [this]() {
    // Drain diagnostics without retaining an unbounded compiler log.
    m_process.setReadChannel(QProcess::StandardError);
    char bytes[4096];
    while (m_process.read(bytes, sizeof(bytes)) > 0) {
    }
    m_process.setReadChannel(QProcess::StandardOutput);
  });
  connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError p_error) {
    if (!m_stopping && !m_failed) {
      fail(tr("Typst helper failed: %1").arg(m_process.errorString()),
           p_error == QProcess::FailedToStart);
    }
  });
  connect(&m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
          [this](int, QProcess::ExitStatus) {
            if (!m_stopping && !m_failed) {
              fail(tr("Typst helper exited. Edit the source or retry."));
            }
          });
  const auto changed = [this](const QString &) { invalidate(); };
  connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, changed);
  connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, changed);
}

TypstCompilerBridge::~TypstCompilerBridge() { stop(); }

void TypstCompilerBridge::updateProject(const TypstEditorConfig &p_config,
                                        const QString &p_activeFile,
                                        const QMap<QString, QString> &p_overlays) {
  const bool executableChanged = m_config.m_helperExecutable != p_config.m_helperExecutable;
  m_config = p_config;
  m_activeFile = p_activeFile;
  m_overlays = p_overlays;
  if (executableChanged && m_process.state() != QProcess::NotRunning) {
    stop();
  }
  invalidate();
}

void TypstCompilerBridge::invalidate() {
  ++m_generation;
  m_latest.clear();
  auto queue = std::move(m_queue);
  m_queue.clear();
  m_failed = false;
  m_compilePending = true;
  m_compileDue = false;
  m_debounce.start(qMax(0, m_config.m_debounceMs));
  emit generationChanged(m_epoch, m_generation);
  for (const auto &request : queue) {
    completeRaster(request, true);
  }
}

void TypstCompilerBridge::retry() {
  stop();
  invalidate();
  m_debounce.stop();
  m_compileDue = true;
  pump();
}

void TypstCompilerBridge::stop(bool p_failure) {
  if (m_stopping) {
    return;
  }
  m_stopping = true;
  m_timeout.stop();
  m_debounce.stop();
  const auto epoch = m_epoch;
  const auto generation = m_generation;
  const auto snapshot = m_latest ? m_latest->m_id : 0;
  // Detach before emitting completions: their slots may enqueue or stop the bridge again.
  auto request = std::move(m_request);
  m_request = {};
  auto queue = std::move(m_queue);
  m_queue.clear();
  ++m_epoch;
  m_ready = false;
  m_displayed = 0;
  m_latest.clear();
  if (m_process.state() != QProcess::NotRunning) {
    m_process.kill();
    m_process.waitForFinished(1000);
  }
  m_prefixRead = m_headerRead = m_payloadRead = 0;
  m_header.clear();
  m_payload.clear();
  const auto complete = [this, p_failure, epoch, generation, snapshot](const QJsonObject &p_request) {
    const bool current = identifier(p_request.value(QStringLiteral("epoch"))) == epoch &&
                         identifier(p_request.value(QStringLiteral("generation"))) == generation &&
                         snapshot &&
                         identifier(p_request.value(QStringLiteral("snapshot"))) == snapshot;
    completeRaster(p_request, !p_failure || !current);
  };
  complete(request);
  for (const auto &queued : queue) {
    complete(queued);
  }
  m_stopping = false;
}

void TypstCompilerBridge::start() {
  QString executable = m_config.m_helperExecutable;
  if (executable.isEmpty()) {
    QString name = QStringLiteral("vtextedit-typst");
#ifdef Q_OS_WIN
    name += QStringLiteral(".exe");
#endif
    const auto beside = QDir(QCoreApplication::applicationDirPath()).filePath(name);
    executable = QFileInfo::exists(beside) ? beside : QStandardPaths::findExecutable(name);
  }
  if (executable.isEmpty()) {
    fail(tr("Typst helper was not found. Install vtextedit-typst or configure its path."), true);
    return;
  }
  emit stateChanged(State::Starting, tr("Starting Typst compiler…"));
  m_timeout.start(qMax(1, m_config.m_compileTimeoutMs));
  m_process.start(executable, QStringList());
}

QJsonObject TypstCompilerBridge::configuration() const {
  return {{QStringLiteral("projectRoot"), QDir(m_config.m_projectRoot).absolutePath()},
          {QStringLiteral("mainFile"), m_config.m_mainFile},
          {QStringLiteral("fontPaths"), QJsonArray::fromStringList(m_config.m_fontPaths)},
          {QStringLiteral("packagePaths"), QJsonArray::fromStringList(m_config.m_packagePaths)},
          {QStringLiteral("allowDownloads"), m_config.m_allowDownloads},
          {QStringLiteral("maxPdfBytes"), static_cast<double>(m_config.m_maxPdfBytes)},
          {QStringLiteral("maxRasterPixels"), m_config.m_maxRasterPixels},
          {QStringLiteral("maxCacheBytes"), static_cast<double>(m_config.m_maxCacheBytes)}};
}

QJsonArray TypstCompilerBridge::retained(bool p_forCompile) const {
  QJsonArray result;
  if (m_displayed) {
    result.append(static_cast<double>(m_displayed));
  }
  if (!p_forCompile && m_latest && m_latest->m_id != m_displayed) {
    result.append(static_cast<double>(m_latest->m_id));
  }
  return result;
}

void TypstCompilerBridge::pump() {
  if (m_failed || !m_request.isEmpty() || m_stopping) {
    return;
  }
  if (m_process.state() == QProcess::NotRunning) {
    if (m_compilePending && m_compileDue) {
      start();
    }
    return;
  }
  if (!m_ready) {
    return;
  }
  if (m_compilePending && m_compileDue) {
    QJsonArray overlays;
    for (auto it = m_overlays.cbegin(); it != m_overlays.cend(); ++it) {
      overlays.append(
          QJsonObject{{QStringLiteral("file"), it.key()}, {QStringLiteral("text"), it.value()}});
    }
    m_compilePending = false;
    m_compileDue = false;
    send({{QStringLiteral("op"), QStringLiteral("compile")},
          {QStringLiteral("config"), configuration()},
          {QStringLiteral("activeFile"), m_activeFile},
          {QStringLiteral("overlays"), overlays},
          {QStringLiteral("snapshots"), retained(true)}});
    emit stateChanged(State::Compiling, tr("Compiling Typst…"));
    return;
  }
  if (!m_queue.isEmpty()) {
    auto request = m_queue.takeFirst();
    if (request.value(QStringLiteral("op")) == QStringLiteral("retain")) {
      request.insert(QStringLiteral("snapshots"), retained(false));
    }
    send(std::move(request));
  }
}

void TypstCompilerBridge::send(QJsonObject p_header) {
  p_header.insert(QStringLiteral("v"), 1);
  if (!p_header.contains(QStringLiteral("epoch"))) {
    p_header.insert(QStringLiteral("epoch"), static_cast<double>(m_epoch));
  }
  if (!p_header.contains(QStringLiteral("generation"))) {
    p_header.insert(QStringLiteral("generation"), static_cast<double>(m_generation));
  }
  p_header.insert(QStringLiteral("request"), static_cast<double>(m_nextRequest++));
  const auto bytes = QJsonDocument(p_header).toJson(QJsonDocument::Compact);
  m_request = std::move(p_header);
  if (bytes.size() > static_cast<int>(c_maxHeader)) {
    fail(tr("Typst project overlays exceed the protocol size limit."));
    return;
  }
  char prefix[8];
  qToBigEndian<quint32>(static_cast<quint32>(bytes.size()), reinterpret_cast<uchar *>(prefix));
  qToBigEndian<quint32>(0, reinterpret_cast<uchar *>(prefix + 4));
  if (m_process.write(prefix, 8) != 8 || m_process.write(bytes) != bytes.size()) {
    fail(tr("Could not write to the Typst helper."));
    return;
  }
  m_timeout.start(qMax(1, m_config.m_compileTimeoutMs));
}

void TypstCompilerBridge::enqueue(QJsonObject p_header) {
  if (!p_header.contains(QStringLiteral("epoch"))) {
    p_header.insert(QStringLiteral("epoch"), static_cast<double>(m_epoch));
  }
  p_header.insert(QStringLiteral("generation"), static_cast<double>(m_generation));
  if (m_stopping) {
    completeRaster(p_header, true);
    return;
  }
  if (m_failed) {
    completeRaster(p_header, false);
    return;
  }
  const auto duplicate = [&p_header](const QJsonObject &p_other) {
    auto other = p_other;
    other.remove(QStringLiteral("v"));
    other.remove(QStringLiteral("request"));
    return other == p_header;
  };
  if (duplicate(m_request)) {
    return;
  }
  for (const auto &queued : m_queue) {
    if (duplicate(queued)) {
      return;
    }
  }
  if (m_queue.size() >= 64) {
    completeRaster(p_header, false);
    emit operationFailed(p_header.value(QStringLiteral("op")).toString(),
                         tr("Typst request queue is full."));
    return;
  }
  m_queue.append(std::move(p_header));
  pump();
}

void TypstCompilerBridge::setDisplayedSnapshot(const TypstSnapshot &p_snapshot) {
  m_displayed = p_snapshot && p_snapshot->m_epoch == m_epoch ? p_snapshot->m_id : 0;
  if (m_ready) {
    enqueue({{QStringLiteral("op"), QStringLiteral("retain")}});
  }
}

void TypstCompilerBridge::requestPdf(quint64 p_snapshot) {
  if (m_latest && m_latest->m_id == p_snapshot) {
    enqueue({{QStringLiteral("op"), QStringLiteral("pdf")},
             {QStringLiteral("snapshot"), static_cast<double>(p_snapshot)}});
  }
}

void TypstCompilerBridge::requestRaster(quint64 p_epoch, quint64 p_snapshot, int p_page,
                                        qreal p_scale) {
  if (p_epoch != m_epoch || !m_latest || m_latest->m_epoch != p_epoch ||
      m_latest->m_id != p_snapshot) {
    emit rasterCancelled(p_epoch, p_snapshot, p_page, p_scale);
    return;
  }
  if (p_page < 0 || p_page >= m_latest->m_pages.size() || !std::isfinite(p_scale) || p_scale <= 0) {
    emit pageImageReady(p_epoch, p_snapshot, p_page, p_scale, QImage());
    return;
  }
  enqueue({{QStringLiteral("op"), QStringLiteral("raster")},
           {QStringLiteral("epoch"), static_cast<double>(p_epoch)},
           {QStringLiteral("snapshot"), static_cast<double>(p_snapshot)},
           {QStringLiteral("page"), p_page},
           {QStringLiteral("scale"), p_scale}});
}

void TypstCompilerBridge::sourceToOutput(const QString &p_file, int p_offset,
                                        quint64 p_cursorRequest) {
  if (m_latest && p_offset >= 0) {
    if (p_cursorRequest) {
      // Keep at most the newest queued cursor request, without dropping explicit navigation.
      for (int i = m_queue.size() - 1; i >= 0; --i) {
        if (identifier(m_queue.at(i).value(QStringLiteral("cursorRequest")))) {
          m_queue.removeAt(i);
        }
      }
    }
    enqueue({{QStringLiteral("op"), QStringLiteral("sourceToOutput")},
             {QStringLiteral("snapshot"), static_cast<double>(m_latest->m_id)},
             {QStringLiteral("file"), p_file},
             {QStringLiteral("offset"), p_offset},
             {QStringLiteral("cursorRequest"), static_cast<double>(p_cursorRequest)}});
  }
}

void TypstCompilerBridge::outputToSource(quint64 p_snapshot, int p_page, const QPointF &p_point) {
  if (m_latest && m_latest->m_id == p_snapshot && p_page >= 0 &&
      p_page < m_latest->m_pages.size() && finitePoint(p_point)) {
    enqueue({{QStringLiteral("op"), QStringLiteral("outputToSource")},
             {QStringLiteral("snapshot"), static_cast<double>(p_snapshot)},
             {QStringLiteral("page"), p_page},
             {QStringLiteral("x"), p_point.x()},
             {QStringLiteral("y"), p_point.y()}});
  }
}

void TypstCompilerBridge::readOutput() {
  if (m_stopping) {
    return;
  }
  while (!m_failed && m_process.bytesAvailable() > 0) {
    if (m_prefixRead < 8) {
      const auto read = m_process.read(m_prefix + m_prefixRead, 8 - m_prefixRead);
      if (read <= 0) {
        return;
      }
      m_prefixRead += read;
      if (m_prefixRead < 8) {
        continue;
      }
      const auto headerSize = qFromBigEndian<quint32>(reinterpret_cast<uchar *>(m_prefix));
      const auto payloadSize = qFromBigEndian<quint32>(reinterpret_cast<uchar *>(m_prefix + 4));
      if (!headerSize || headerSize > c_maxHeader || payloadSize > c_maxPayload ||
          static_cast<qint64>(payloadSize) > m_config.m_maxCacheBytes) {
        fail(tr("Typst helper returned an oversized frame."));
        return;
      }
      m_header.resize(static_cast<int>(headerSize));
      m_payload.resize(static_cast<int>(payloadSize));
    }
    if (m_headerRead < m_header.size()) {
      const auto read =
          m_process.read(m_header.data() + m_headerRead, m_header.size() - m_headerRead);
      if (read <= 0) {
        return;
      }
      m_headerRead += read;
      if (m_headerRead < m_header.size()) {
        continue;
      }
    }
    if (m_payloadRead < m_payload.size()) {
      const auto read =
          m_process.read(m_payload.data() + m_payloadRead, m_payload.size() - m_payloadRead);
      if (read <= 0) {
        return;
      }
      m_payloadRead += read;
      if (m_payloadRead < m_payload.size()) {
        continue;
      }
    }
    QJsonParseError error;
    const auto json = QJsonDocument::fromJson(m_header, &error);
    if (error.error != QJsonParseError::NoError || !json.isObject()) {
      fail(tr("Typst helper returned an invalid protocol header."));
      return;
    }
    auto payload = std::move(m_payload);
    m_header.clear();
    m_payload.clear();
    m_prefixRead = m_headerRead = m_payloadRead = 0;
    handleReply(json.object(), std::move(payload));
  }
}

bool TypstCompilerBridge::isCurrent(const QJsonObject &p_header) const {
  return identifier(p_header.value(QStringLiteral("epoch"))) == m_epoch &&
         identifier(p_header.value(QStringLiteral("generation"))) == m_generation;
}

void TypstCompilerBridge::handleReply(const QJsonObject &p_header, QByteArray p_payload) {
  if (p_header.value(QStringLiteral("v")).toInt() != 1 || m_request.isEmpty() ||
      identifier(p_header.value(QStringLiteral("request"))) !=
          identifier(m_request.value(QStringLiteral("request"))) ||
      identifier(p_header.value(QStringLiteral("epoch"))) !=
          identifier(m_request.value(QStringLiteral("epoch"))) ||
      identifier(p_header.value(QStringLiteral("generation"))) !=
          identifier(m_request.value(QStringLiteral("generation")))) {
    fail(tr("Typst helper response does not match its request."));
    return;
  }
  const auto operation = p_header.value(QStringLiteral("op")).toString();
  const auto requested = m_request.value(QStringLiteral("op")).toString();
  const bool current = isCurrent(p_header);
  if (operation == QStringLiteral("parsed") && requested == QStringLiteral("compile")) {
    if (current) {
      emit highlightsReady(m_generation,
                           highlights(p_header.value(QStringLiteral("highlights")).toArray()),
                           m_activeFile);
    }
    return;
  }
  if (operation == QStringLiteral("error")) {
    if (requested == QStringLiteral("initialize")) {
      fail(p_header.value(QStringLiteral("message")).toString(), true);
      return;
    }
    auto request = std::move(m_request);
    m_request = {};
    m_timeout.stop();
    completeRaster(request, !current);
    if (current) {
      updateDependencies(p_header.value(QStringLiteral("dependencies")).toArray());
      emit diagnosticsReady(diagnostics(p_header.value(QStringLiteral("diagnostics")).toArray()));
      const auto message = p_header.value(QStringLiteral("message")).toString();
      emit operationFailed(requested, message);
      emit stateChanged(State::Error, message);
    }
    pump();
    return;
  }
  if (operation == QStringLiteral("ready") && requested == QStringLiteral("initialize")) {
    const auto capabilities = p_header.value(QStringLiteral("capabilities")).toArray();
    for (const auto &required : {"pdf", "raster", "navigation", "previews", "highlights"}) {
      if (!capabilities.contains(QString::fromLatin1(required))) {
        fail(tr("Typst helper is incompatible: missing %1.").arg(QString::fromLatin1(required)));
        return;
      }
    }
    m_ready = true;
  } else if (operation == QStringLiteral("compiled") && requested == QStringLiteral("compile")) {
    if (current) {
      updateDependencies(p_header.value(QStringLiteral("dependencies")).toArray());
      if (p_header.value(QStringLiteral("invalidated")).toBool()) {
        invalidate();
        finishRequest();
        return;
      }
      const auto messages = diagnostics(p_header.value(QStringLiteral("diagnostics")).toArray());
      emit diagnosticsReady(messages);
      emit highlightsReady(m_generation,
                           highlights(p_header.value(QStringLiteral("highlights")).toArray()),
                           m_activeFile);
      if (p_header.contains(QStringLiteral("snapshot"))) {
        m_latest = snapshotFromJson(p_header);
        if (!m_latest) {
          fail(tr("Typst helper returned invalid compiled geometry."));
          return;
        }
        emit stateChanged(State::Ready, tr("Typst preview is up to date."));
        emit snapshotReady(m_latest);
      } else {
        emit stateChanged(State::Error, messages.isEmpty() ? tr("Typst compilation failed.")
                                                           : messages.first().m_message);
      }
    }
  } else if (operation == QStringLiteral("pdf") && requested == operation) {
    if (current && m_latest &&
        identifier(p_header.value(QStringLiteral("snapshot"))) == m_latest->m_id) {
      if (p_payload.isEmpty() || p_payload.size() > m_config.m_maxPdfBytes) {
        emit operationFailed(operation, tr("Typst PDF exceeds the configured size limit."));
      } else {
        emit pdfReady(m_latest, p_payload);
      }
    }
  } else if (operation == QStringLiteral("raster") && requested == operation) {
    const auto id = identifier(p_header.value(QStringLiteral("snapshot")));
    const int page = integer(p_header.value(QStringLiteral("page")));
    const qreal scale = p_header.value(QStringLiteral("scale")).toDouble();
    const int width = integer(p_header.value(QStringLiteral("width")));
    const int height = integer(p_header.value(QStringLiteral("height")));
    const int stride = integer(p_header.value(QStringLiteral("stride")));
    if (id != identifier(m_request.value(QStringLiteral("snapshot"))) ||
        page != integer(m_request.value(QStringLiteral("page"))) ||
        !qFuzzyCompare(scale, m_request.value(QStringLiteral("scale")).toDouble()) || width <= 0 ||
        height <= 0 || width > std::numeric_limits<int>::max() / 4 || stride != width * 4 ||
        static_cast<qint64>(width) * height > m_config.m_maxRasterPixels ||
        static_cast<qint64>(stride) * height != p_payload.size()) {
      fail(tr("Typst helper returned invalid raster dimensions."));
      return;
    }
    auto request = std::move(m_request);
    m_request = {};
    m_timeout.stop();
    if (current && m_latest && m_latest->m_id == id) {
      auto *storage = new QByteArray(std::move(p_payload));
      QImage image(
          reinterpret_cast<uchar *>(storage->data()), width, height, stride,
          QImage::Format_RGBA8888_Premultiplied,
          [](void *p_storage) { delete static_cast<QByteArray *>(p_storage); }, storage);
      emit pageImageReady(identifier(request.value(QStringLiteral("epoch"))),
                          identifier(request.value(QStringLiteral("snapshot"))),
                          integer(request.value(QStringLiteral("page"))),
                          request.value(QStringLiteral("scale")).toDouble(), image);
    } else {
      completeRaster(request, true);
    }
    pump();
    return;
  } else if (operation == QStringLiteral("sourceToOutput") && requested == operation) {
    if (current && m_latest) {
      const auto locations = p_header.value(QStringLiteral("locations")).toArray();
      int page = -1;
      QPointF point;
      if (!locations.isEmpty()) {
        const auto location = locations.first().toObject();
        page = integer(location.value(QStringLiteral("page")));
        point = QPointF(location.value(QStringLiteral("x")).toDouble(),
                        location.value(QStringLiteral("y")).toDouble());
        if (page < 0 || page >= m_latest->m_pages.size() || !finitePoint(point)) {
          page = -1;
        }
      }
      // The helper echoes protocol identity, not this caller-local metadata.
      const auto cursorRequest = identifier(m_request.value(QStringLiteral("cursorRequest")));
      if (cursorRequest) {
        emit cursorLocationReady(cursorRequest, m_latest->m_id, page, point);
      } else if (page >= 0) {
        emit outputLocationReady(m_latest->m_id, page, point);
      }
    }
  } else if (operation == QStringLiteral("outputToSource") && requested == operation) {
    if (current) {
      const auto range = sourceRange(p_header.value(QStringLiteral("location")).toObject());
      if (range.isValid()) {
        emit sourceLocationReady(range.m_file, range.m_start, range.m_end);
      }
      const QUrl url(p_header.value(QStringLiteral("url")).toString());
      if (!url.isEmpty() && url.isValid()) {
        emit externalLinkRequested(url);
      }
    }
  } else if (!(operation == QStringLiteral("retained") && requested == QStringLiteral("retain"))) {
    fail(tr("Typst helper returned an unexpected operation."));
    return;
  }
  finishRequest();
}

void TypstCompilerBridge::finishRequest() {
  m_timeout.stop();
  m_request = {};
  pump();
}

void TypstCompilerBridge::completeRaster(const QJsonObject &p_request, bool p_cancelled) {
  if (p_request.value(QStringLiteral("op")) == QStringLiteral("raster")) {
    const auto epoch = identifier(p_request.value(QStringLiteral("epoch")));
    const auto snapshot = identifier(p_request.value(QStringLiteral("snapshot")));
    const int page = integer(p_request.value(QStringLiteral("page")));
    const qreal scale = p_request.value(QStringLiteral("scale")).toDouble();
    if (p_cancelled) {
      emit rasterCancelled(epoch, snapshot, page, scale);
    } else {
      emit pageImageReady(epoch, snapshot, page, scale, QImage());
    }
  }
}

void TypstCompilerBridge::fail(const QString &p_message, bool p_unavailable) {
  stop(true);
  m_failed = true;
  m_compilePending = false;
  m_compileDue = false;
  emit generationChanged(m_epoch, m_generation);
  emit stateChanged(p_unavailable ? State::Unavailable : State::Error, p_message);
}

void TypstCompilerBridge::updateDependencies(const QJsonArray &p_dependencies) {
  const auto oldFiles = m_watcher.files();
  const auto oldDirectories = m_watcher.directories();
  if (!oldFiles.isEmpty()) {
    m_watcher.removePaths(oldFiles);
  }
  if (!oldDirectories.isEmpty()) {
    m_watcher.removePaths(oldDirectories);
  }
  QSet<QString> paths;
  for (const auto &value : p_dependencies) {
    const auto path = value.toObject().value(QStringLiteral("path")).toString();
    if (path.isEmpty()) {
      continue;
    }
    QFileInfo file(path);
    if (file.exists()) {
      paths.insert(file.absoluteFilePath());
    }
    auto directory = file.absoluteDir();
    while (!directory.exists() && directory.cdUp()) {
    }
    if (directory.exists()) {
      paths.insert(directory.absolutePath());
    }
  }
  if (!paths.isEmpty()) {
    const auto failed = m_watcher.addPaths(paths.values());
    if (!failed.isEmpty()) {
      emit operationFailed(
          QStringLiteral("watch"),
          tr("Could not watch Typst dependencies: %1").arg(failed.join(QStringLiteral(", "))));
    }
  }
}
