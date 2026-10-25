#include <vtextedit/typstfragmentrenderer.h>

#include "typstruntime_p.h"

#include <QByteArrayView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>

#include <cmath>
#include <limits>
#include <utility>

using namespace vte;

namespace {
constexpr qsizetype c_maxSource = 64 * 1024;
constexpr size_t c_maxHeader = 64 * 1024;
constexpr size_t c_maxSvg = 8 * 1024 * 1024;
constexpr double c_maxDimension = 16384.0;
constexpr quint64 c_maxId = 9007199254740991ULL;
constexpr int c_deadlineMs = 30000;

enum class FragmentKind { Math, Markup };

QString operationFor(FragmentKind p_kind) {
  return p_kind == FragmentKind::Math ? QStringLiteral("mathSvg") : QStringLiteral("markupSvg");
}

quint64 identifier(const QJsonValue &p_value) {
  const double value = p_value.toDouble(-1);
  return p_value.isDouble() && std::isfinite(value) && value >= 1 &&
                 value <= static_cast<double>(c_maxId) && std::floor(value) == value
             ? static_cast<quint64>(value)
             : 0;
}

quint64 nextIdentifier(quint64 p_value) { return p_value == c_maxId ? 1 : p_value + 1; }

QString sourceError(const QString &p_source) {
  // Bound normalized UTF-8 before allocating either the normalized text or JSON.
  qsizetype bytes = 0;
  bool nonblank = false;
  for (qsizetype idx = 0; idx < p_source.size(); ++idx) {
    const auto ch = p_source.at(idx);
    if (ch.isNull()) {
      return TypstFragmentRenderer::tr("Typst source contains a NUL.");
    }
    nonblank = nonblank || !ch.isSpace();
    if (ch.isHighSurrogate() && idx + 1 < p_source.size() &&
        p_source.at(idx + 1).isLowSurrogate()) {
      bytes += 4;
      ++idx;
    } else if (ch.isSurrogate()) {
      return TypstFragmentRenderer::tr("Typst source contains invalid Unicode.");
    } else {
      const auto code = ch.unicode();
      bytes += code < 0x80 ? 1 : code < 0x800 ? 2 : 3;
      if (ch == QLatin1Char('\r') && idx + 1 < p_source.size() &&
          p_source.at(idx + 1) == QLatin1Char('\n')) {
        ++idx;
      }
    }
    if (bytes > c_maxSource) {
      return TypstFragmentRenderer::tr("Typst source exceeds 64 KiB UTF-8.");
    }
  }
  return nonblank ? QString() : TypstFragmentRenderer::tr("Typst source must not be blank.");
}

void setFailure(TypstFragmentRenderer::Result &p_result, const QString &p_error) {
  p_result = {};
  p_result.m_sizePt = QSizeF(0, 0);
  p_result.m_error = p_error;
}
} // namespace

class TypstFragmentRenderer::Private {
public:
  Private(TypstFragmentRenderer *p_owner, TypstRuntime *p_runtime)
      : q(p_owner), m_runtime(p_runtime), m_deadline(p_owner) {
    m_deadline.setSingleShot(true);
    m_deadline.setInterval(c_deadlineMs);
    m_deadline.setObjectName(QStringLiteral("typst-deadline"));
    QObject::connect(&m_deadline, &QTimer::timeout, q,
                     [this]() { finish(TypstFragmentRenderer::tr("Typst rendering timed out.")); });
    if (m_runtime) {
      QObject::connect(m_runtime, &QObject::destroyed, q, [this]() { runtimeStopped(); });
    }
  }

  ~Private() {
    if (m_runtime) {
      QObject::disconnect(m_runtime, nullptr, q, nullptr);
    }
    // Endpoint destruction suppresses delivery and cancels queued work. The
    // runtime retains executing native state without making this destructor wait.
    delete m_session;
  }

  bool start(FragmentKind p_kind, const QString &p_source, bool p_display,
             const QColor &p_foreground, bool p_sensitive) {
    if (m_busy) {
      return false;
    }
    m_busy = true;
    m_result = {};
    m_requestId = nextIdentifier(m_requestId);
    m_deadline.start(c_deadlineMs);
    const auto error = sourceError(p_source);
    if (!error.isEmpty()) {
      finish(error);
      return true;
    }
    if (!p_foreground.isValid()) {
      finish(TypstFragmentRenderer::tr("Typst foreground must be a valid color."));
      return true;
    }
    auto source = p_source;
    source.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    source.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    m_pendingRequest = {
        {QStringLiteral("source"), source},
        {QStringLiteral("op"), operationFor(p_kind)},
        {QStringLiteral("sensitive"), p_sensitive},
        {QStringLiteral("foreground"), QJsonArray{p_foreground.red(), p_foreground.green(),
                                                  p_foreground.blue(), p_foreground.alpha()}}};
    if (p_kind == FragmentKind::Math) {
      m_pendingRequest.insert(QStringLiteral("display"), p_display);
    }
    pump();
    return true;
  }

  void resetSession() {
    Q_ASSERT(!m_nativeInFlight);
    delete m_session;
    m_session = nullptr;
    m_nativeRetired = false;
    m_replySeen = false;
  }

  void pump() {
    if (!m_busy || m_finishing || m_nativeInFlight) {
      return;
    }
    if (!m_runtime || !m_runtime->isAvailable()) {
      finish(TypstFragmentRenderer::tr("Typst runtime is unavailable."));
      return;
    }
    if (!m_session) {
      m_session = TypstRuntimePrivate::get(m_runtime)->createSession(q);
      if (!m_session) {
        finish(TypstFragmentRenderer::tr("Typst runtime is unavailable."));
        return;
      }
      m_epoch = nextIdentifier(m_epoch);
      QObject::connect(m_session, &TypstRuntimeSession::replyReady, q,
                       [this](const TypstRuntimeReply &p_reply) { receiveReply(p_reply); });
      QObject::connect(m_session, &TypstRuntimeSession::finished, q,
                       [this](int p_status) { nativeFinished(p_status); });
      QObject::connect(m_session, &TypstRuntimeSession::stopped, q, [this]() { runtimeStopped(); });
    }
    m_pendingRequest.insert(QStringLiteral("v"), 1);
    // Captured once per native dispatch. A replacement logical request cannot
    // change the kind of an executing retired call or its immutable request bytes.
    m_nativeKind = m_pendingRequest.value(QStringLiteral("op")) == QStringLiteral("mathSvg")
                       ? FragmentKind::Math
                       : FragmentKind::Markup;
    m_pendingRequest.insert(QStringLiteral("epoch"), static_cast<double>(m_epoch));
    // Each stateless fragment is its own generation; no project is initialized.
    m_pendingRequest.insert(QStringLiteral("generation"), static_cast<double>(m_requestId));
    m_pendingRequest.insert(QStringLiteral("request"), static_cast<double>(m_requestId));
    // Sensitivity is captured with the logical request, not mutable session state.
    // execute() owns these bytes until actual native return, even after cancellation.
    auto request = QJsonDocument(m_pendingRequest).toJson(QJsonDocument::Compact);
    m_pendingRequest = {};
    m_replySeen = false;
    m_nativeRetired = false;
    if (!m_session->execute(std::move(request))) {
      resetSession();
      finish(TypstFragmentRenderer::tr("Typst runtime is unavailable."));
      return;
    }
    m_nativeInFlight = true;
  }

  void receiveReply(const TypstRuntimeReply &p_reply) {
    if (!m_nativeInFlight || m_nativeRetired || !m_busy || m_finishing) {
      return;
    }
    if (m_replySeen) {
      finish(TypstFragmentRenderer::tr("Typst backend returned more than one final reply."));
      return;
    }
    m_replySeen = true;
    size_t headerSize = 0;
    size_t payloadSize = 0;
    const auto *headerBytes = p_reply ? p_reply.header(&headerSize) : nullptr;
    const auto *payloadBytes = p_reply ? p_reply.payload(&payloadSize) : nullptr;
    if (!headerBytes || headerSize == 0 || headerSize > c_maxHeader || payloadSize > c_maxSvg ||
        (payloadSize != 0 && !payloadBytes)) {
      finish(TypstFragmentRenderer::tr("Typst backend returned invalid reply lengths."));
      return;
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(
        QByteArray::fromRawData(reinterpret_cast<const char *>(headerBytes), qsizetype(headerSize)),
        &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
      finish(TypstFragmentRenderer::tr("Typst backend returned invalid reply metadata."));
      return;
    }
    const auto header = document.object();
    const auto operation = header.value(QStringLiteral("op")).toString();
    if (identifier(header.value(QStringLiteral("v"))) != 1 ||
        identifier(header.value(QStringLiteral("epoch"))) != m_epoch ||
        identifier(header.value(QStringLiteral("generation"))) != m_requestId ||
        identifier(header.value(QStringLiteral("request"))) != m_requestId ||
        (operation != operationFor(m_nativeKind) && operation != QStringLiteral("error"))) {
      finish(TypstFragmentRenderer::tr("Typst reply does not match its request."));
      return;
    }
    if (operation == QStringLiteral("error")) {
      const auto message = header.value(QStringLiteral("message"));
      if (header.value(QStringLiteral("for")) != operationFor(m_nativeKind) || payloadSize != 0 ||
          !message.isString() || message.toString().isEmpty()) {
        finish(TypstFragmentRenderer::tr("Typst backend returned an invalid error."));
        return;
      }
      setFailure(m_result, message.toString());
      return;
    }
    const auto width = header.value(QStringLiteral("widthPt"));
    const auto height = header.value(QStringLiteral("heightPt"));
    const auto baseline = header.value(QStringLiteral("baselinePt"));
    const auto diagnostics = header.value(QStringLiteral("diagnostics"));
    const auto pageCount = identifier(header.value(QStringLiteral("pageCount")));
    if (!width.isDouble() || !height.isDouble() || !baseline.isDouble() ||
        !std::isfinite(width.toDouble()) || !std::isfinite(height.toDouble()) ||
        !std::isfinite(baseline.toDouble()) || width.toDouble() <= 0 || height.toDouble() <= 0 ||
        width.toDouble() > c_maxDimension || height.toDouble() > c_maxDimension ||
        !diagnostics.isArray() || payloadSize == 0 || pageCount == 0 ||
        pageCount > quint64(std::numeric_limits<int>::max()) ||
        (m_nativeKind == FragmentKind::Math && pageCount != 1) ||
        (m_nativeKind == FragmentKind::Markup && baseline.toDouble() != 0)) {
      finish(TypstFragmentRenderer::tr("Typst backend returned invalid geometry or content."));
      return;
    }
    const QByteArrayView svg(reinterpret_cast<const char *>(payloadBytes), qsizetype(payloadSize));
    if (!svg.isValidUtf8() || svg.contains('\0')) {
      finish(TypstFragmentRenderer::tr("Typst backend returned invalid UTF-8 SVG."));
      return;
    }
    const auto messages = diagnostics.toArray();
    m_result.m_diagnostics.reserve(messages.size());
    for (const auto &message : messages) {
      if (!message.isString()) {
        finish(TypstFragmentRenderer::tr("Typst backend returned invalid diagnostics."));
        return;
      }
      m_result.m_diagnostics.append(message.toString());
    }
    m_result.m_sizePt = QSizeF(width.toDouble(), height.toDouble());
    m_result.m_baselinePt = baseline.toDouble();
    m_result.m_pageCount = int(pageCount);
    // Copy while the reply owns its bytes, then retain no native reply handle.
    // This is provisional until the native call's authoritative status arrives.
    m_result.m_svg = QByteArray(svg.data(), svg.size());
  }

  void nativeFinished(int p_status) {
    if (!m_nativeInFlight) {
      return;
    }
    m_nativeInFlight = false;
    if (!m_runtime || !m_runtime->isAvailable() || !m_session->isAvailable()) {
      resetSession();
      finish(TypstFragmentRenderer::tr("Typst runtime is unavailable."));
      return;
    }
    if (m_nativeRetired) {
      // A cancelled/deadline-expired call may still have occupied its lane. Only
      // now replace its session and start the single waiting logical request.
      // That request keeps the deadline established when it was accepted.
      resetSession();
      pump();
      return;
    }
    if (p_status != VTE_TYPST_OK) {
      resetSession();
      finish(TypstFragmentRenderer::tr("Typst native operation failed (status %1).").arg(p_status));
      return;
    }
    if (!m_replySeen) {
      resetSession();
      finish(TypstFragmentRenderer::tr("Typst backend completed without a final reply."));
      return;
    }
    finish();
  }

  void runtimeStopped() {
    // Shutdown has joined the lanes, so even suppressed native work is retired.
    m_nativeInFlight = false;
    resetSession();
    finish(TypstFragmentRenderer::tr("Typst runtime is unavailable."));
  }

  void finish(const QString &p_error = QString()) {
    if (!m_busy) {
      return;
    }
    if (m_finishing) {
      // Cancellation/shutdown may invalidate a success before it is delivered,
      // but the first terminal failure is not replaced by subsequent events.
      if (!p_error.isEmpty() && m_result.isSuccess()) {
        setFailure(m_result, p_error);
      }
      return;
    }
    if (!p_error.isEmpty()) {
      setFailure(m_result, p_error);
    }
    if (!m_result.isSuccess() && m_nativeInFlight) {
      m_nativeRetired = true;
      m_session->suppressReplies();
      m_session->cancelQueued();
    }
    m_deadline.stop();
    m_pendingRequest = {};
    m_finishing = true;
    QTimer::singleShot(0, q, [this]() {
      if (m_result.isSuccess() && (!m_runtime || !m_runtime->isAvailable())) {
        setFailure(m_result, TypstFragmentRenderer::tr("Typst runtime is unavailable."));
      }
      auto result = std::move(m_result);
      m_result = {};
      m_finishing = false;
      m_busy = false;
      // No access to this after emission: a slot may delete or resubmit the facade.
      emit q->finished(result);
    });
  }

  TypstFragmentRenderer *q;
  QPointer<TypstRuntime> m_runtime;
  QTimer m_deadline;
  TypstRuntimeSession *m_session = nullptr;
  QJsonObject m_pendingRequest;
  Result m_result;
  quint64 m_epoch = 0;
  quint64 m_requestId = 0;
  FragmentKind m_nativeKind = FragmentKind::Math;
  bool m_busy = false;
  bool m_finishing = false;
  bool m_nativeInFlight = false;
  bool m_nativeRetired = false;
  bool m_replySeen = false;
};

TypstFragmentRenderer::TypstFragmentRenderer(TypstRuntime *p_runtime, QObject *p_parent)
    : QObject(p_parent), d(new Private(this, p_runtime)) {
  qRegisterMetaType<TypstFragmentRenderer::Result>();
}

TypstFragmentRenderer::~TypstFragmentRenderer() = default;

bool TypstFragmentRenderer::renderMathToSvg(const QString &p_source, bool p_display,
                                            const QColor &p_foreground, bool p_sensitive) {
  return d->start(FragmentKind::Math, p_source, p_display, p_foreground, p_sensitive);
}

bool TypstFragmentRenderer::renderMarkupToSvg(const QString &p_source, const QColor &p_foreground,
                                              bool p_sensitive) {
  return d->start(FragmentKind::Markup, p_source, false, p_foreground, p_sensitive);
}

bool TypstFragmentRenderer::isRendering() const { return d->m_busy; }

void TypstFragmentRenderer::cancel() { d->finish(tr("Typst rendering was cancelled.")); }
