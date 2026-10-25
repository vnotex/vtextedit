#include "typstcompilerbridge.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QPointer>
#include <QSet>

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

bool resourceSource(const QJsonObject &p_object, TypstSourceRange &p_range) {
  p_range = sourceRange(p_object);
  return p_object.value(QStringLiteral("file")).isString() &&
         integer(p_object.value(QStringLiteral("start")), -2) == p_range.m_start &&
         integer(p_object.value(QStringLiteral("end")), -2) == p_range.m_end &&
         (p_range.m_file.isEmpty() ? p_range.m_start == -1 && p_range.m_end == -1
                                   : p_range.m_start >= 0 && p_range.m_end > p_range.m_start);
}

bool resourceTarget(const QJsonObject &p_object, int p_pages, TypstLink &p_link) {
  const auto nan = std::numeric_limits<double>::quiet_NaN();
  p_link.m_targetPage = integer(p_object.value(QStringLiteral("targetPage")), -2);
  p_link.m_target = QPointF(p_object.value(QStringLiteral("targetX")).toDouble(nan),
                            p_object.value(QStringLiteral("targetY")).toDouble(nan));
  if (!p_object.value(QStringLiteral("url")).isString() || !finitePoint(p_link.m_target))
    return false;
  p_link.m_url = QUrl(p_object.value(QStringLiteral("url")).toString());
  return p_link.m_targetPage >= -1 && p_link.m_targetPage < p_pages &&
         (p_link.m_targetPage >= 0 ? p_link.m_url.isEmpty()
                                   : !p_link.m_url.isEmpty() && p_link.m_url.isValid());
}

bool resourcesFromJson(const QJsonObject &p_header, TypstCompiledSnapshot &p_snapshot,
                       bool p_required) {
  const auto imagesValue = p_header.value(QStringLiteral("images"));
  const auto linksValue = p_header.value(QStringLiteral("sourceLinks"));
  if ((!imagesValue.isArray() && (p_required || !imagesValue.isUndefined())) ||
      (!linksValue.isArray() && (p_required || !linksValue.isUndefined())))
    return false;
  const auto images = imagesValue.toArray();
  const auto links = linksValue.toArray();
  if (images.size() > 100000 || links.size() > 100000 ||
      (!p_required && (!images.isEmpty() || !links.isEmpty())))
    return false;
  QHash<quint64, int> occurrences;
  for (int i = 0; i < p_snapshot.m_previews.size(); ++i) {
    const auto &preview = p_snapshot.m_previews[i];
    if (preview.m_kind == QLatin1String("image"))
      occurrences.insert(preview.m_id, i);
  }
  p_snapshot.m_images.reserve(images.size());
  for (const auto &value : images) {
    if (!value.isObject())
      return false;
    const auto object = value.toObject();
    TypstImageResource image;
    image.m_id = identifier(object.value(QStringLiteral("id")));
    image.m_path = object.value(QStringLiteral("path")).toString();
    image.m_size = QSize(integer(object.value(QStringLiteral("width"))),
                         integer(object.value(QStringLiteral("height"))));
    image.m_previewIndex = occurrences.value(image.m_id, -1);
    if (!image.m_id || !occurrences.remove(image.m_id) ||
        !object.value(QStringLiteral("path")).isString() || image.m_path.contains(QChar::Null) ||
        image.m_size.width() < 0 || image.m_size.height() < 0 ||
        !resourceSource(object, image.m_source))
      return false;
    const auto resizeValue = object.value(QStringLiteral("resize"));
    if (!resizeValue.isNull()) {
      if (!resizeValue.isObject() || !image.m_source.isValid())
        return false;
      const auto resize = resizeValue.toObject();
      auto &plan = image.m_resize;
      plan.m_argumentsEnd = integer(resize.value(QStringLiteral("argumentsEnd")));
      plan.m_trailingComma = resize.value(QStringLiteral("trailingComma")).toBool();
      plan.m_widthStart = integer(resize.value(QStringLiteral("widthStart")), -2);
      plan.m_widthEnd = integer(resize.value(QStringLiteral("widthEnd")), -2);
      plan.m_heightStart = integer(resize.value(QStringLiteral("heightStart")), -2);
      plan.m_heightEnd = integer(resize.value(QStringLiteral("heightEnd")), -2);
      const auto nan = std::numeric_limits<double>::quiet_NaN();
      plan.m_widthPoints = resize.value(QStringLiteral("widthPoints")).toDouble(nan);
      plan.m_heightPoints = resize.value(QStringLiteral("heightPoints")).toDouble(nan);
      const auto validAxis = [&](int p_start, int p_end, double p_points) {
        return std::isfinite(p_points) && p_points >= 0 &&
               p_points <= std::numeric_limits<int>::max() * 0.75 &&
               ((p_start == -1 && p_end == -1 && p_points == 0) ||
                (p_start >= image.m_source.m_start && p_end > p_start &&
                 p_end <= plan.m_argumentsEnd));
      };
      if (!resize.value(QStringLiteral("trailingComma")).isBool() ||
          plan.m_argumentsEnd < image.m_source.m_start ||
          plan.m_argumentsEnd >= image.m_source.m_end ||
          !validAxis(plan.m_widthStart, plan.m_widthEnd, plan.m_widthPoints) ||
          !validAxis(plan.m_heightStart, plan.m_heightEnd, plan.m_heightPoints) ||
          (plan.m_widthStart >= 0 && plan.m_heightStart >= 0 &&
           plan.m_widthStart < plan.m_heightEnd && plan.m_heightStart < plan.m_widthEnd))
        return false;
    }
    p_snapshot.m_images.append(std::move(image));
  }
  p_snapshot.m_sourceLinks.reserve(links.size());
  for (const auto &value : links) {
    if (!value.isObject())
      return false;
    const auto object = value.toObject();
    TypstSourceLink link;
    if (!resourceSource(object, link.m_source) || !link.m_source.isValid() ||
        !resourceTarget(object, p_snapshot.m_pages.size(), link.m_link))
      return false;
    p_snapshot.m_sourceLinks.append(std::move(link));
  }
  return true;
}

TypstSnapshot snapshotFromJson(const QJsonObject &p_header, bool p_resourcesRequired) {
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
  const auto headingsValue = p_header.value(QStringLiteral("headings"));
  if (!headingsValue.isArray()) {
    return {};
  }
  const auto headings = headingsValue.toArray();
  result->m_headings.reserve(headings.size());
  for (const auto &value : headings) {
    const auto object = value.toObject();
    const auto range = sourceRange(object);
    const int level = integer(object.value(QStringLiteral("level")));
    if (!object.value(QStringLiteral("title")).isString() ||
        !object.value(QStringLiteral("file")).isString() || level < 1 ||
        !object.value(QStringLiteral("start")).isDouble() ||
        !object.value(QStringLiteral("end")).isDouble() ||
        (range.m_file.isEmpty() ? (object.value(QStringLiteral("start")).toDouble() != -1 ||
                                   object.value(QStringLiteral("end")).toDouble() != -1)
                                : !range.isValid())) {
      return {};
    }
    TypstHeadingInfo heading;
    heading.m_startPos = range.m_start;
    heading.m_endPos = range.m_end;
    heading.m_level = level;
    heading.m_title = object.value(QStringLiteral("title")).toString();
    heading.m_file = range.m_file;
    result->m_headings.append(std::move(heading));
  }
  if (!resourcesFromJson(p_header, *result, p_resourcesRequired))
    return {};
  result->m_highlights = highlights(p_header.value(QStringLiteral("highlights")).toArray());
  result->m_diagnostics = diagnostics(p_header.value(QStringLiteral("diagnostics")).toArray());
  for (const auto &value : p_header.value(QStringLiteral("dependencies")).toArray()) {
    result->m_dependencies.append(value.toObject().value(QStringLiteral("path")).toString());
  }
  return result;
}
} // namespace

TypstCompilerBridge::TypstCompilerBridge(QObject *p_parent, Mode p_mode)
    : QObject(p_parent), m_mode(p_mode) {
  m_debounce.setSingleShot(true);
  m_timeout.setSingleShot(true);
  m_timeout.setParent(this);
  m_timeout.setObjectName(QStringLiteral("typst-deadline"));
  connect(&m_debounce, &QTimer::timeout, this, [this]() {
    m_compileDue = true;
    pump();
  });
  connect(&m_timeout, &QTimer::timeout, this, [this]() {
    if (!m_request.isEmpty() &&
        m_request.value(QStringLiteral("op")) != QStringLiteral("initialize") &&
        !isCurrent(m_request)) {
      const QPointer<TypstCompilerBridge> alive(this);
      retireRequest(false);
      if (alive)
        emit stateChanged(State::Error, tr("Waiting for the previous Typst operation to finish."));
      return;
    }
    fail(tr("Typst operation timed out. Edit the source or retry."));
  });
  const auto changed = [this](const QString &) { invalidate(); };
  connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, changed);
  connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, changed);
}

TypstCompilerBridge::~TypstCompilerBridge() {
  // The runtime retains active native state; editor destruction never waits.
  delete m_session;
}

void TypstCompilerBridge::updateProject(const TypstEditorConfig &p_config,
                                        const QString &p_activeFile,
                                        const QMap<QString, QString> &p_overlays) {
  const bool runtimeChanged = m_config.m_runtime != p_config.m_runtime;
  m_config = p_config;
  m_activeFile = p_activeFile;
  m_overlays = p_overlays;
  const QPointer<TypstCompilerBridge> alive(this);
  if (runtimeChanged && m_session)
    stop();
  if (alive)
    invalidate();
}

void TypstCompilerBridge::resetForNewDocument() {
  // Close admission before watcher removal or any cancellation signals can re-enter us.
  m_documentReset = true;
  m_debounce.stop();
  m_compilePending = false;
  m_compileDue = false;
  ++m_generation;
  m_displayed = 0;
  m_inplace = 0;
  m_latest.clear();
  m_overlays.clear();
  m_activeFile.clear();
  auto queue = std::move(m_queue);
  m_queue.clear();
  updateDependencies({});
  // Keep the original dispatch identity and session epoch until native return.
  const QPointer<TypstCompilerBridge> alive(this);
  if (m_request.value(QStringLiteral("op")) != QStringLiteral("initialize"))
    retireRequest(false, false);
  if (!alive)
    return;
  emit generationChanged(m_epoch, m_generation);
  if (!alive)
    return;
  emit diagnosticsReady({});
  if (!alive)
    return;
  emit stateChanged(State::Idle, QString());
  if (!alive)
    return;
  for (const auto &request : queue) {
    completeRaster(request, true);
    if (!alive)
      return;
  }
}

void TypstCompilerBridge::finishDocumentReset() {
  if (!m_documentReset) {
    return;
  }
  m_documentReset = false;
  invalidate();
}

void TypstCompilerBridge::invalidate() {
  if (m_documentReset) {
    return;
  }
  ++m_generation;
  m_latest.clear();
  auto queue = std::move(m_queue);
  m_queue.clear();
  m_failed = false;
  m_compilePending = true;
  m_compileDue = false;
  m_debounce.start(m_mode == Mode::Export ? 0 : qMax(0, m_config.m_debounceMs));
  if (m_session && m_request.value(QStringLiteral("op")) == QStringLiteral("compile") &&
      m_session->cancelQueued()) {
    m_requestRetired = true;
    m_session->suppressReplies();
  }
  const QPointer<TypstCompilerBridge> alive(this);
  emit generationChanged(m_epoch, m_generation);
  if (!alive)
    return;
  for (const auto &request : queue) {
    completeRaster(request, true);
    if (!alive)
      return;
  }
}

void TypstCompilerBridge::retry() {
  if (m_documentReset) {
    return;
  }
  const QPointer<TypstCompilerBridge> alive(this);
  stop();
  if (!alive)
    return;
  invalidate();
  if (!alive)
    return;
  m_debounce.stop();
  m_compileDue = true;
  pump();
}

void TypstCompilerBridge::retireRequest(bool p_failure, bool p_stopTimeout) {
  if (p_stopTimeout)
    m_timeout.stop();
  m_terminalReply = {};
  m_terminalHeader = {};
  m_requestRetired = true;
  if (m_session) {
    m_session->suppressReplies();
    m_session->cancelQueued();
  }
  if (m_requestCompleted || m_request.isEmpty())
    return;
  m_requestCompleted = true;
  const bool current = isCurrent(m_request) && m_latest &&
                       identifier(m_request.value(QStringLiteral("snapshot"))) == m_latest->m_id;
  completeRaster(m_request, !p_failure || !current);
}

void TypstCompilerBridge::resetSession() {
  Q_ASSERT(!m_nativeInFlight);
  delete m_session;
  m_session = nullptr;
  m_request = {};
  m_terminalReply = {};
  m_terminalHeader = {};
  m_requestRetired = false;
  m_requestCompleted = false;
  m_restartPending = false;
  ++m_epoch;
  m_ready = false;
  m_displayed = 0;
  m_inplace = 0;
  m_latest.clear();
}

void TypstCompilerBridge::stop(bool p_failure) {
  if (m_stopping)
    return;
  m_stopping = true;
  m_debounce.stop();
  m_restartPending = true;
  const auto epoch = m_epoch;
  const auto generation = m_generation;
  const auto snapshot = m_latest ? m_latest->m_id : 0;
  auto queue = std::move(m_queue);
  m_queue.clear();
  const QPointer<TypstCompilerBridge> alive(this);
  retireRequest(p_failure);
  if (!alive)
    return;
  m_ready = false;
  m_displayed = 0;
  m_inplace = 0;
  m_latest.clear();
  // Timeout/retry may retire delivery, never a still-executing session.
  if (!m_nativeInFlight)
    resetSession();
  for (const auto &request : queue) {
    const bool current = identifier(request.value(QStringLiteral("epoch"))) == epoch &&
                         identifier(request.value(QStringLiteral("generation"))) == generation &&
                         snapshot &&
                         identifier(request.value(QStringLiteral("snapshot"))) == snapshot;
    completeRaster(request, !p_failure || !current);
    if (!alive)
      return;
  }
  m_stopping = false;
}

void TypstCompilerBridge::start() {
  if (!m_config.m_runtime || !m_config.m_runtime->isAvailable()) {
    fail(tr("Typst runtime is unavailable."), true);
    return;
  }
  m_session = TypstRuntimePrivate::get(m_config.m_runtime)->createSession(this);
  if (!m_session) {
    fail(tr("Typst runtime is unavailable."), true);
    return;
  }
  connect(m_session, &TypstRuntimeSession::replyReady, this, &TypstCompilerBridge::receiveReply);
  connect(m_session, &TypstRuntimeSession::finished, this, &TypstCompilerBridge::nativeFinished);
  connect(m_session, &TypstRuntimeSession::stopped, this, [this]() {
    // Runtime shutdown has joined every lane, including an active invocation.
    m_nativeInFlight = false;
    fail(tr("Typst runtime is unavailable."), true);
  });
  const QPointer<TypstCompilerBridge> alive(this);
  send({{QStringLiteral("op"), QStringLiteral("initialize")},
        {QStringLiteral("config"), configuration()}});
  if (alive && m_nativeInFlight)
    emit stateChanged(State::Starting, tr("Starting Typst compiler…"));
}

QJsonObject TypstCompilerBridge::configuration() const {
  QJsonObject config{
      {QStringLiteral("projectRoot"), QDir(m_config.m_projectRoot).absolutePath()},
      {QStringLiteral("mainFile"), m_config.m_mainFile},
      {QStringLiteral("fontPaths"), QJsonArray::fromStringList(m_config.m_fontPaths)},
      {QStringLiteral("packagePaths"), QJsonArray::fromStringList(m_config.m_packagePaths)},
      {QStringLiteral("allowDownloads"), m_config.m_allowDownloads},
      {QStringLiteral("maxPdfBytes"), static_cast<double>(m_config.m_maxPdfBytes)},
      {QStringLiteral("maxRasterPixels"), m_config.m_maxRasterPixels},
      {QStringLiteral("maxCacheBytes"), static_cast<double>(m_config.m_maxCacheBytes)}};
  const auto theme = m_config.m_textEditorConfig && m_config.m_textEditorConfig->m_theme
                         ? m_config.m_textEditorConfig->m_theme
                         : TextEditorConfig::defaultTheme();
  const auto &text = theme->typstTextStyle();
  if (!text.m_fontFamilies.isEmpty())
    config.insert(QStringLiteral("textFontFamilies"),
                  QJsonArray::fromStringList(text.m_fontFamilies));
  if (text.m_fontPointSize > 0)
    config.insert(QStringLiteral("textFontSize"), text.m_fontPointSize);
  if (m_mode == Mode::Preview && m_config.m_enableThemeStyleInPreview) {
    const auto addColor = [&config](const QString &p_key, const QColor &p_color) {
      if (p_color.isValid()) {
        config.insert(p_key,
                      QJsonArray{p_color.red(), p_color.green(), p_color.blue(), p_color.alpha()});
      }
    };
    addColor(QStringLiteral("previewBackground"), m_config.getPreviewBackground());
    addColor(QStringLiteral("previewForeground"), m_config.m_previewForeground);
  }
  return config;
}

QJsonArray TypstCompilerBridge::retained(bool p_forCompile) const {
  QJsonArray result;
  if (m_displayed) {
    result.append(static_cast<double>(m_displayed));
  }
  if (m_inplace && m_inplace != m_displayed)
    result.append(static_cast<double>(m_inplace));
  if (!p_forCompile && m_latest && m_latest->m_id != m_displayed && m_latest->m_id != m_inplace) {
    result.append(static_cast<double>(m_latest->m_id));
  }
  return result;
}

void TypstCompilerBridge::pump() {
  if (m_documentReset || m_failed || m_nativeInFlight || !m_request.isEmpty() || m_stopping) {
    return;
  }
  if (!m_session) {
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
    const QPointer<TypstCompilerBridge> alive(this);
    send({{QStringLiteral("op"), QStringLiteral("compile")},
          {QStringLiteral("config"), configuration()},
          {QStringLiteral("activeFile"), m_activeFile},
          {QStringLiteral("overlays"), overlays},
          {QStringLiteral("previewMetadata"), m_mode == Mode::Preview},
          {QStringLiteral("snapshots"), retained(true)}});
    if (alive && m_nativeInFlight)
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
  auto bytes = QJsonDocument(p_header).toJson(QJsonDocument::Compact);
  m_request = std::move(p_header);
  m_requestRetired = false;
  m_requestCompleted = false;
  if (bytes.size() > static_cast<int>(c_maxHeader)) {
    fail(tr("Typst project overlays exceed the native request size limit."));
    return;
  }
  if (!m_session || !m_session->execute(std::move(bytes))) {
    fail(tr("Typst runtime is unavailable."), true);
    return;
  }
  m_nativeInFlight = true;
  m_timeout.start(qMax(1, m_config.m_compileTimeoutMs));
}

void TypstCompilerBridge::enqueue(QJsonObject p_header) {
  if (!p_header.contains(QStringLiteral("epoch"))) {
    p_header.insert(QStringLiteral("epoch"), static_cast<double>(m_epoch));
  }
  p_header.insert(QStringLiteral("generation"), static_cast<double>(m_generation));
  if (m_documentReset || m_stopping) {
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
    const QPointer<TypstCompilerBridge> alive(this);
    completeRaster(p_header, false);
    if (alive)
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

void TypstCompilerBridge::setInplaceSnapshot(const TypstSnapshot &p_snapshot) {
  m_inplace = p_snapshot && p_snapshot->m_epoch == m_epoch ? p_snapshot->m_id : 0;
  if (m_ready)
    enqueue({{QStringLiteral("op"), QStringLiteral("retain")}});
}

void TypstCompilerBridge::requestImageData(quint64 p_epoch, quint64 p_snapshot, quint64 p_image,
                                           quint64 p_consumerRequest) {
  const quint64 token = m_nextImageRequest++;
  m_imageRequests.insert(token, p_consumerRequest);
  const QJsonObject request{{QStringLiteral("op"), QStringLiteral("imageData")},
                            {QStringLiteral("epoch"), static_cast<double>(p_epoch)},
                            {QStringLiteral("snapshot"), static_cast<double>(p_snapshot)},
                            {QStringLiteral("image"), static_cast<double>(p_image)},
                            {QStringLiteral("consumerToken"), static_cast<double>(token)}};
  if (p_epoch != m_epoch || !p_image ||
      (p_snapshot != m_displayed && p_snapshot != m_inplace &&
       (!m_latest || p_snapshot != m_latest->m_id))) {
    completeImage(request, {}, {}, tr("The Typst image snapshot is no longer retained."));
    return;
  }
  enqueue(request);
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

void TypstCompilerBridge::requestSvg(quint64 p_epoch, quint64 p_snapshot, int p_page) {
  if (p_epoch != m_epoch || !m_latest || m_latest->m_epoch != p_epoch ||
      m_latest->m_id != p_snapshot || p_page < 0 || p_page >= m_latest->m_pages.size()) {
    emit pageSvgReady(p_epoch, p_snapshot, p_page, QByteArray());
    return;
  }
  enqueue({{QStringLiteral("op"), QStringLiteral("svg")},
           {QStringLiteral("epoch"), static_cast<double>(p_epoch)},
           {QStringLiteral("snapshot"), static_cast<double>(p_snapshot)},
           {QStringLiteral("page"), p_page}});
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

void TypstCompilerBridge::receiveReply(const TypstRuntimeReply &p_reply) {
  if (!m_nativeInFlight || m_requestRetired)
    return;
  size_t headerSize = 0;
  size_t payloadSize = 0;
  const auto *bytes = p_reply.header(&headerSize);
  p_reply.payload(&payloadSize);
  if (!bytes || headerSize == 0 || headerSize > c_maxHeader || payloadSize > c_maxPayload ||
      (m_request.value(QStringLiteral("op")) == QStringLiteral("imageData") &&
       payloadSize > 32U * 1024 * 1024) ||
      static_cast<qint64>(payloadSize) > m_config.m_maxCacheBytes) {
    fail(tr("Typst backend returned an oversized reply."));
    return;
  }
  QJsonParseError error;
  const auto json = QJsonDocument::fromJson(
      QByteArray::fromRawData(reinterpret_cast<const char *>(bytes), qsizetype(headerSize)),
      &error);
  if (error.error != QJsonParseError::NoError || !json.isObject()) {
    fail(tr("Typst backend returned invalid reply metadata."));
    return;
  }
  const auto header = json.object();
  if (header.value(QStringLiteral("op")) == QStringLiteral("parsed")) {
    handleReply(header, {});
    return;
  }
  if (m_terminalReply) {
    fail(tr("Typst backend returned more than one final reply."));
    return;
  }
  // A final event is not completion: the ABI may still fail after emitting it.
  m_terminalReply = p_reply;
  m_terminalHeader = header;
}

void TypstCompilerBridge::nativeFinished(int p_status) {
  m_nativeInFlight = false;
  m_timeout.stop();
  const QPointer<TypstCompilerBridge> alive(this);
  if (!m_session || !m_session->isAvailable()) {
    fail(tr("Typst runtime is unavailable."), true);
    return;
  }
  if (m_restartPending) {
    resetSession();
    emit generationChanged(m_epoch, m_generation);
    if (alive)
      pump();
    return;
  }
  if (p_status != VTE_TYPST_OK && p_status != TypstRuntimePrivate::Cancelled) {
    if (m_requestRetired || (!isCurrent(m_request) && m_request.value(QStringLiteral("op")) !=
                                                          QStringLiteral("initialize"))) {
      stop();
      if (!alive)
        return;
      m_compileDue = m_compilePending;
      emit generationChanged(m_epoch, m_generation);
      if (alive)
        pump();
    } else {
      fail(
          tr("Typst native operation failed (status %1). Edit the source or retry.").arg(p_status));
    }
    return;
  }
  if (m_requestRetired || p_status == TypstRuntimePrivate::Cancelled) {
    retireRequest(false);
    if (alive)
      finishRequest();
    return;
  }
  auto reply = std::move(m_terminalReply);
  auto header = std::move(m_terminalHeader);
  m_terminalReply = {};
  m_terminalHeader = {};
  if (!reply) {
    fail(tr("Typst backend completed without a final reply."));
    return;
  }
  handleReply(header, reply);
}

bool TypstCompilerBridge::isCurrent(const QJsonObject &p_header) const {
  return identifier(p_header.value(QStringLiteral("epoch"))) == m_epoch &&
         identifier(p_header.value(QStringLiteral("generation"))) == m_generation;
}

void TypstCompilerBridge::handleReply(const QJsonObject &p_header,
                                      const TypstRuntimeReply &p_reply) {
  const QPointer<TypstCompilerBridge> alive(this);
  const auto requestId = identifier(p_header.value(QStringLiteral("request")));
  const auto pending = [this, requestId]() {
    return m_session && m_session->isAvailable() &&
           identifier(m_request.value(QStringLiteral("request"))) == requestId;
  };
  if (p_header.value(QStringLiteral("v")).toInt() != 1 || m_request.isEmpty() ||
      identifier(p_header.value(QStringLiteral("request"))) !=
          identifier(m_request.value(QStringLiteral("request"))) ||
      identifier(p_header.value(QStringLiteral("epoch"))) !=
          identifier(m_request.value(QStringLiteral("epoch"))) ||
      identifier(p_header.value(QStringLiteral("generation"))) !=
          identifier(m_request.value(QStringLiteral("generation")))) {
    fail(tr("Typst backend response does not match its request."));
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
  bool obsolete = !current;
  if (requested == QStringLiteral("imageData")) {
    // Ordinary edits keep displayed image snapshots readable. A document reset
    // clears all retained IDs, even while the old serial reply is still draining.
    const auto snapshot = identifier(m_request.value(QStringLiteral("snapshot")));
    obsolete = m_documentReset || !snapshot ||
               identifier(m_request.value(QStringLiteral("epoch"))) != m_epoch ||
               (snapshot != m_displayed && snapshot != m_inplace &&
                (!m_latest || snapshot != m_latest->m_id));
  }
  if (obsolete && requested != QStringLiteral("initialize")) {
    // Only the matching final reply releases the serial request. Parsed is intermediate.
    auto request = std::move(m_request);
    m_request = {};
    m_timeout.stop();
    completeRaster(request, true);
    if (alive)
      pump();
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
    if (requested == QStringLiteral("imageData")) {
      const auto message = p_header.value(QStringLiteral("message")).toString();
      completeImage(request, {}, {},
                    message.isEmpty() ? tr("Typst image request failed.") : message);
      pump();
      return;
    }
    completeRaster(request, !current);
    if (!alive)
      return;
    if (current && isCurrent(p_header)) {
      updateDependencies(p_header.value(QStringLiteral("dependencies")).toArray());
      if (!alive)
        return;
      emit diagnosticsReady(diagnostics(p_header.value(QStringLiteral("diagnostics")).toArray()));
      if (!alive || !isCurrent(p_header))
        return;
      const auto message = p_header.value(QStringLiteral("message")).toString();
      emit operationFailed(requested, message);
      if (!alive)
        return;
      if (isCurrent(p_header))
        emit stateChanged(State::Error, message);
    }
    if (alive)
      pump();
    return;
  }
  // Native Bytes cross the boundary without a frame copy. Only a live Qt
  // consumer gets an independent owning payload; obsolete replies need no copy.
  size_t payloadSize = 0;
  const auto *payloadBytes = p_reply ? p_reply.payload(&payloadSize) : nullptr;
  QByteArray p_payload;
  if (payloadSize && operation != QStringLiteral("raster"))
    p_payload = QByteArray(reinterpret_cast<const char *>(payloadBytes), qsizetype(payloadSize));
  if (operation == QStringLiteral("ready") && requested == QStringLiteral("initialize")) {
    const auto capabilities = p_header.value(QStringLiteral("capabilities")).toArray();
    const auto requiredCapabilities =
        m_mode == Mode::Export
            ? QStringList{QStringLiteral("svg"), QStringLiteral("raster")}
            : QStringList{QStringLiteral("pdf"),        QStringLiteral("raster"),
                          QStringLiteral("navigation"), QStringLiteral("previews"),
                          QStringLiteral("highlights"), QStringLiteral("headings"),
                          QStringLiteral("resources")};
    for (const auto &required : requiredCapabilities) {
      if (!capabilities.contains(required)) {
        fail(tr("Typst backend is incompatible: missing %1.").arg(required));
        return;
      }
    }
    m_ready = true;
  } else if (operation == QStringLiteral("compiled") && requested == QStringLiteral("compile")) {
    if (current) {
      updateDependencies(p_header.value(QStringLiteral("dependencies")).toArray());
      if (!alive || !pending())
        return;
      if (p_header.value(QStringLiteral("invalidated")).toBool()) {
        if (m_mode == Mode::Export) {
          emit diagnosticsReady(
              diagnostics(p_header.value(QStringLiteral("diagnostics")).toArray()));
          if (!alive || !pending())
            return;
          emit stateChanged(State::Error, tr("Typst dependencies changed during compilation."));
        } else {
          invalidate();
        }
        if (alive)
          finishRequest(requestId);
        return;
      }
      const auto messages = diagnostics(p_header.value(QStringLiteral("diagnostics")).toArray());
      emit diagnosticsReady(messages);
      if (!alive || !pending())
        return;
      if (!isCurrent(p_header)) {
        finishRequest(requestId);
        return;
      }
      emit highlightsReady(m_generation,
                           highlights(p_header.value(QStringLiteral("highlights")).toArray()),
                           m_activeFile);
      if (!alive || !pending())
        return;
      // Diagnostics/highlight consumers may edit the source synchronously.
      if (!isCurrent(p_header)) {
        finishRequest(requestId);
        return;
      }
      if (p_header.contains(QStringLiteral("snapshot"))) {
        m_latest = snapshotFromJson(p_header, m_mode == Mode::Preview);
        if (!m_latest) {
          fail(tr("Typst backend returned invalid compiled geometry."));
          return;
        }
        const auto snapshot = m_latest;
        emit stateChanged(State::Ready, tr("Typst preview is up to date."));
        if (!alive || !pending())
          return;
        // Ready consumers can also invalidate this result before publication.
        if (isCurrent(p_header) && m_latest == snapshot) {
          emit snapshotReady(snapshot);
        }
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
        const auto snapshot = m_latest;
        emit pdfReady(snapshot, p_payload);
      }
    }
  } else if (operation == QStringLiteral("imageData") && requested == operation) {
    const int width = integer(p_header.value(QStringLiteral("width")));
    const int height = integer(p_header.value(QStringLiteral("height")));
    const auto mime = p_header.value(QStringLiteral("mime")).toString();
    if (identifier(p_header.value(QStringLiteral("snapshot"))) !=
            identifier(m_request.value(QStringLiteral("snapshot"))) ||
        identifier(p_header.value(QStringLiteral("image"))) !=
            identifier(m_request.value(QStringLiteral("image"))) ||
        width <= 0 || height <= 0 || width > 8192 || height > 8192 ||
        qint64(width) * height > 16 * 1024 * 1024 || p_payload.isEmpty() ||
        p_payload.size() > 32 * 1024 * 1024 ||
        (mime != QLatin1String("image/png") && mime != QLatin1String("image/jpeg") &&
         mime != QLatin1String("image/gif") && mime != QLatin1String("image/webp"))) {
      fail(tr("Typst backend returned invalid image data."));
      return;
    }
    auto request = std::move(m_request);
    m_request = {};
    m_timeout.stop();
    // Retained bytes are independent of today's live file and source generation.
    completeImage(request, p_payload, mime, {});
    pump();
    return;
  } else if (operation == QStringLiteral("svg") && requested == operation) {
    const auto id = identifier(p_header.value(QStringLiteral("snapshot")));
    const int page = integer(p_header.value(QStringLiteral("page")));
    if (id != identifier(m_request.value(QStringLiteral("snapshot"))) ||
        page != integer(m_request.value(QStringLiteral("page"))) || p_payload.isEmpty()) {
      fail(tr("Typst backend returned an invalid SVG page."));
      return;
    }
    auto request = std::move(m_request);
    m_request = {};
    m_timeout.stop();
    emit pageSvgReady(identifier(request.value(QStringLiteral("epoch"))), id, page,
                      current && m_latest && m_latest->m_id == id ? p_payload : QByteArray());
    if (alive)
      pump();
    return;
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
        static_cast<qint64>(width) * height > m_config.m_maxRasterPixels || !payloadBytes ||
        static_cast<quint64>(stride) * height != payloadSize) {
      fail(tr("Typst backend returned invalid raster dimensions."));
      return;
    }
    auto request = std::move(m_request);
    m_request = {};
    m_timeout.stop();
    if (current && m_latest && m_latest->m_id == id) {
      auto *storage = new TypstRuntimeReply(p_reply);
      // Const storage makes writes detach rather than mutate Typst's shared cache.
      QImage image(
          reinterpret_cast<const uchar *>(payloadBytes), width, height, stride,
          QImage::Format_RGBA8888_Premultiplied,
          [](void *p_storage) { delete static_cast<TypstRuntimeReply *>(p_storage); }, storage);
      emit pageImageReady(identifier(request.value(QStringLiteral("epoch"))),
                          identifier(request.value(QStringLiteral("snapshot"))),
                          integer(request.value(QStringLiteral("page"))),
                          request.value(QStringLiteral("scale")).toDouble(), image);
    } else {
      completeRaster(request, true);
    }
    if (alive)
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
      // The backend echoes request identity, not this caller-local metadata.
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
        if (!alive || !pending())
          return;
      }
      const QUrl url(p_header.value(QStringLiteral("url")).toString());
      if (!url.isEmpty() && url.isValid()) {
        emit externalLinkRequested(url);
      }
    }
  } else if (!(operation == QStringLiteral("retained") && requested == QStringLiteral("retain"))) {
    fail(tr("Typst backend returned an unexpected operation."));
    return;
  }
  if (alive)
    finishRequest(requestId);
}

void TypstCompilerBridge::finishRequest(quint64 p_request) {
  if (p_request && identifier(m_request.value(QStringLiteral("request"))) != p_request)
    return;
  Q_ASSERT(!m_nativeInFlight);
  m_timeout.stop();
  m_request = {};
  m_requestRetired = false;
  m_requestCompleted = false;
  pump();
}

void TypstCompilerBridge::completeImage(const QJsonObject &p_request, const QByteArray &p_data,
                                        const QString &p_mime, const QString &p_error) {
  const auto token = identifier(p_request.value(QStringLiteral("consumerToken")));
  auto it = m_imageRequests.find(token);
  if (it == m_imageRequests.end())
    return;
  const auto consumer = it.value();
  m_imageRequests.erase(it);
  QTimer::singleShot(0, this, [this, consumer, p_data, p_mime, p_error]() {
    emit imageDataReady(consumer, p_data, p_mime, p_error);
  });
}

void TypstCompilerBridge::completeRaster(const QJsonObject &p_request, bool p_cancelled) {
  if (p_request.value(QStringLiteral("op")) == QStringLiteral("imageData")) {
    completeImage(p_request, {}, {},
                  p_cancelled ? tr("Typst image request cancelled.")
                              : tr("Typst image request failed."));
    return;
  }
  if (p_request.value(QStringLiteral("op")) == QStringLiteral("svg")) {
    emit pageSvgReady(identifier(p_request.value(QStringLiteral("epoch"))),
                      identifier(p_request.value(QStringLiteral("snapshot"))),
                      integer(p_request.value(QStringLiteral("page"))), QByteArray());
  } else if (p_request.value(QStringLiteral("op")) == QStringLiteral("raster")) {
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
  m_failed = true;
  m_compilePending = false;
  m_compileDue = false;
  const auto generation = m_generation;
  const QPointer<TypstCompilerBridge> alive(this);
  stop(true);
  if (!alive)
    return;
  emit generationChanged(m_epoch, m_generation);
  if (alive && m_failed && generation == m_generation)
    emit stateChanged(p_unavailable ? State::Unavailable : State::Error, p_message);
}

void TypstCompilerBridge::updateDependencies(const QJsonArray &p_dependencies) {
  if (m_mode == Mode::Export) {
    return;
  }
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
      continue;
    }
    // Existing dependencies notify their own changes/removal. Watching their
    // parents too would revoke resources for unrelated notebook bookkeeping.
    // Missing dependencies need an ancestor watch so creation can recover them.
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
