#include <vtextedit/typstcompiler.h>

#include "typstcompilerbridge.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QScopeGuard>
#include <QTimer>
#include <QtEndian>

#include <cmath>
#include <utility>

using namespace vte;

class TypstCompiler::Private {
public:
  enum class Format { Svg, Bitmap };

  Private(TypstCompiler *p_owner, const TypstEditorConfig &p_config)
      : q(p_owner), m_config(p_config), m_bridge(nullptr, TypstCompilerBridge::Mode::Export) {
    m_config.m_projectRoot = QDir(m_config.m_projectRoot).absolutePath();
    QObject::connect(&m_bridge, &TypstCompilerBridge::diagnosticsReady, q,
                     [this](const QVector<TypstDiagnostic> &p_diagnostics) {
                       if (!m_busy || m_finishing) {
                         return;
                       }
                       m_result.m_diagnostics.clear();
                       for (const auto &diagnostic : p_diagnostics) {
                         const auto message =
                             diagnostic.m_severity + QStringLiteral(": ") + diagnostic.m_message;
                         m_result.m_diagnostics.append(diagnostic.m_source.isValid()
                                                           ? QStringLiteral("%1:%2: %3")
                                                                 .arg(diagnostic.m_source.m_file)
                                                                 .arg(diagnostic.m_source.m_start)
                                                                 .arg(message)
                                                           : message);
                       }
                     });
    QObject::connect(&m_bridge, &TypstCompilerBridge::stateChanged, q,
                     [this](TypstCompilerBridge::State p_state, const QString &p_message) {
                       if (p_state == TypstCompilerBridge::State::Error ||
                           p_state == TypstCompilerBridge::State::Unavailable) {
                         finish(p_message);
                       }
                     });
    QObject::connect(&m_bridge, &TypstCompilerBridge::operationFailed, q,
                     [this](const QString &, const QString &p_message) { finish(p_message); });
    QObject::connect(&m_bridge, &TypstCompilerBridge::snapshotReady, q,
                     [this](const TypstSnapshot &p_snapshot) {
                       if (!m_busy || m_finishing) {
                         return;
                       }
                       m_snapshot = p_snapshot;
                       if (m_format == Format::Svg) {
                         m_result.m_svgPages.reserve(m_snapshot->m_pages.size());
                       } else {
                         m_result.m_bitmapPages.reserve(m_snapshot->m_pages.size());
                       }
                       requestPage();
                     });
    QObject::connect(
        &m_bridge, &TypstCompilerBridge::pageSvgReady, q,
        [this](quint64 p_epoch, quint64 p_snapshot, int p_page, const QByteArray &p_svg) {
          if (m_format != Format::Svg || !acceptsPage(p_epoch, p_snapshot, p_page)) {
            return;
          }
          if (p_svg.isEmpty()) {
            finish(TypstCompiler::tr("Could not render Typst SVG page."));
          } else if (admitBytes(p_svg.size())) {
            m_result.m_svgPages.append(p_svg);
            ++m_page;
            requestPage();
          }
        });
    QObject::connect(
        &m_bridge, &TypstCompilerBridge::pageImageReady, q,
        [this](quint64 p_epoch, quint64 p_snapshot, int p_page, qreal, const QImage &p_image) {
          if (m_format != Format::Bitmap || !acceptsPage(p_epoch, p_snapshot, p_page)) {
            return;
          }
          if (p_image.isNull()) {
            finish(TypstCompiler::tr("Could not render Typst bitmap page."));
          } else if (admitBytes(p_image.sizeInBytes())) {
            m_result.m_bitmapPages.append(p_image);
            ++m_page;
            requestPage();
          }
        });
    QObject::connect(&m_bridge, &TypstCompilerBridge::rasterCancelled, q,
                     [this](quint64 p_epoch, quint64 p_snapshot, int p_page, qreal) {
                       if (acceptsPage(p_epoch, p_snapshot, p_page)) {
                         finish(TypstCompiler::tr("Typst bitmap rendering was cancelled."));
                       }
                     });
  }

  ~Private() { QObject::disconnect(&m_bridge, nullptr, q, nullptr); }

  bool start(const QString &p_text, Format p_format, qreal p_pixelsPerPoint) {
    if (m_busy) {
      return false;
    }
    m_busy = true;
    m_format = p_format;
    m_scale = p_pixelsPerPoint;
    m_page = 0;
    m_bytes = 0;
    if (p_format == Format::Bitmap && (!std::isfinite(m_scale) || m_scale <= 0)) {
      finish(TypstCompiler::tr("Typst bitmap scale must be finite and positive."));
      return true;
    }
    auto text = p_text;
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    m_bridge.updateProject(m_config, m_config.m_mainFile, {{m_config.m_mainFile, text}});
    return true;
  }

  bool acceptsPage(quint64 p_epoch, quint64 p_snapshot, int p_page) const {
    return m_busy && !m_finishing && m_snapshot && p_epoch == m_snapshot->m_epoch &&
           p_snapshot == m_snapshot->m_id && p_page == m_page;
  }

  bool admitBytes(qint64 p_bytes) {
    if (p_bytes > m_config.m_maxCacheBytes - m_bytes) {
      finish(TypstCompiler::tr("Typst output exceeds the configured total page size limit."));
      return false;
    }
    m_bytes += p_bytes;
    return true;
  }

  void requestPage() {
    if (m_page == m_snapshot->m_pages.size()) {
      finish();
    } else if (m_format == Format::Svg) {
      m_bridge.requestSvg(m_snapshot->m_epoch, m_snapshot->m_id, m_page);
    } else {
      m_bridge.requestRaster(m_snapshot->m_epoch, m_snapshot->m_id, m_page, m_scale);
    }
  }

  void finish(const QString &p_error = QString()) {
    if (!m_busy) {
      return;
    }
    if (!p_error.isEmpty()) {
      // A transport failure may first complete a page with empty data, then
      // supply its more specific error. Keep that error but publish only once.
      m_result.m_error = p_error;
      m_result.m_svgPages.clear();
      m_result.m_bitmapPages.clear();
    }
    if (m_finishing) {
      return;
    }
    m_finishing = true;
    QTimer::singleShot(0, q, [this]() {
      auto result = std::move(m_result);
      m_result = {};
      m_snapshot.clear();
      m_finishing = false;
      m_busy = false;
      // No access to this after emission: the slot may destroy the compiler.
      emit q->finished(result);
    });
  }

  TypstCompiler *q;
  TypstEditorConfig m_config;
  TypstCompilerBridge m_bridge;
  Result m_result;
  TypstSnapshot m_snapshot;
  Format m_format = Format::Svg;
  qreal m_scale = 1.0;
  qint64 m_bytes = 0;
  int m_page = 0;
  bool m_busy = false;
  bool m_finishing = false;
};

TypstCompiler::TypstCompiler(const TypstEditorConfig &p_config, QObject *p_parent)
    : QObject(p_parent), d(new Private(this, p_config)) {
  qRegisterMetaType<TypstCompiler::Result>();
}

TypstCompiler::~TypstCompiler() = default;

bool TypstCompiler::compileToSvg(const QString &p_text) {
  return d->start(p_text, Private::Format::Svg, 1.0);
}

bool TypstCompiler::compileToBitmap(const QString &p_text, qreal p_pixelsPerPoint) {
  return d->start(p_text, Private::Format::Bitmap, p_pixelsPerPoint);
}

bool TypstCompiler::isCompiling() const { return d->m_busy; }

QVector<TypstCompiler::ImageReferences>
TypstCompiler::collectImageReferences(const QStringList &p_sources,
                                      const QString &p_helperExecutable, QString *p_error) {
  if (p_error) {
    p_error->clear();
  }
  const auto fail = [p_error](const QString &p_message) {
    if (p_error) {
      *p_error = p_message;
    }
    return QVector<ImageReferences>();
  };
  constexpr qsizetype c_headerLimit = 16 * 1024 * 1024;
  constexpr qint64 c_chunkSize = 64 * 1024;
  const QFileInfo executable(p_helperExecutable);
  if (p_helperExecutable.contains(QChar::Null) || !executable.isAbsolute() ||
      !executable.isFile() || p_sources.isEmpty() || p_sources.size() > 3) {
    return fail(tr("Image reference scan requires 1–3 sources and an explicit helper file"));
  }

  // Bound the encoded JSON before QJsonDocument allocates its serialized header.
  qsizetype size =
      QByteArrayLiteral("{\"v\":1,\"op\":\"scanImageReferences\",\"sources\":[]}").size() +
      3 * p_sources.size() - 1;
  for (const auto &source : p_sources) {
    for (qsizetype idx = 0; idx < source.size(); ++idx) {
      const auto ch = source.at(idx);
      const auto code = ch.unicode();
      if (ch.isNull()) {
        return fail(tr("Image reference source contains a NUL"));
      }
      if (ch.isHighSurrogate() && idx + 1 < source.size() && source.at(idx + 1).isLowSurrogate()) {
        size += 4;
        ++idx;
      } else if (ch.isSurrogate()) {
        return fail(tr("Image reference source contains invalid Unicode"));
      } else if (code == '"' || code == '\\' || code == '\b' || code == '\f' || code == '\n' ||
                 code == '\r' || code == '\t') {
        size += 2;
      } else {
        size += code < 0x20 ? 6 : code < 0x80 ? 1 : code < 0x800 ? 2 : 3;
      }
      if (size > c_headerLimit) {
        return fail(tr("Image reference request exceeds 16 MiB"));
      }
    }
  }
  const auto header =
      QJsonDocument(QJsonObject{{QStringLiteral("v"), 1},
                                {QStringLiteral("op"), QStringLiteral("scanImageReferences")},
                                {QStringLiteral("sources"), QJsonArray::fromStringList(p_sources)}})
          .toJson(QJsonDocument::Compact);
  if (header.size() > c_headerLimit) {
    return fail(tr("Image reference request exceeds 16 MiB"));
  }
  char lengths[8] = {};
  qToBigEndian<quint32>(quint32(header.size()), lengths);
  QElapsedTimer deadline;
  deadline.start();
  const auto remaining = [&] { return int(qMax(qint64(0), 10000 - deadline.elapsed())); };
  QProcess scanner;
  // Scanner errors are reported as failures, never parsed as protocol bytes. Avoid an
  // unbounded stderr buffer even if an explicit but broken executable is supplied.
  scanner.setStandardErrorFile(QProcess::nullDevice());
  const auto reap = qScopeGuard([&] {
    if (scanner.state() != QProcess::NotRunning) {
      scanner.kill();
      scanner.waitForFinished(1000);
    }
  });
  scanner.start(p_helperExecutable, {QStringLiteral("--scan-image-references")});
  if (!scanner.waitForStarted(remaining())) {
    return fail(tr("Unable to start image reference scanner: %1").arg(scanner.errorString()));
  }
  const auto send = [&](const char *p_data, qint64 p_size) {
    while (p_size > 0) {
      if (remaining() == 0) {
        return false;
      }
      const auto chunk = qMin(p_size, c_chunkSize);
      if (scanner.write(p_data, chunk) != chunk) {
        return false;
      }
      while (scanner.bytesToWrite() > 0) {
        if (remaining() == 0 || !scanner.waitForBytesWritten(remaining())) {
          return false;
        }
      }
      p_data += chunk;
      p_size -= chunk;
    }
    return true;
  };
  if (!send(lengths, sizeof(lengths)) || !send(header.constData(), header.size())) {
    return fail(tr("Unable to write image reference request within its deadline"));
  }
  scanner.closeWriteChannel();
  QByteArray response;
  qsizetype expected = 8;
  bool haveLengths = false;
  while (scanner.state() != QProcess::NotRunning || scanner.bytesAvailable() > 0) {
    if (remaining() == 0) {
      return fail(tr("Image reference scan timed out"));
    }
    if (scanner.bytesAvailable() == 0) {
      scanner.waitForReadyRead(remaining());
      continue;
    }
    if (response.size() == expected) {
      return fail(tr("Image reference scanner returned trailing data"));
    }
    const auto chunk = scanner.read(qMin(c_chunkSize, qint64(expected - response.size())));
    if (chunk.isEmpty()) {
      return fail(tr("Unable to read image reference response"));
    }
    response.append(chunk);
    if (!haveLengths && response.size() == 8) {
      const auto jsonSize = qFromBigEndian<quint32>(response.constData());
      const auto binarySize = qFromBigEndian<quint32>(response.constData() + 4);
      if (jsonSize == 0 || jsonSize > c_headerLimit || binarySize != 0) {
        return fail(tr("Invalid image reference response lengths"));
      }
      expected += jsonSize;
      response.reserve(expected);
      haveLengths = true;
    }
  }
  if (remaining() == 0 || scanner.exitStatus() != QProcess::NormalExit || scanner.exitCode() != 0 ||
      !haveLengths || response.size() != expected) {
    return fail(tr("Image reference scanner failed or returned an incomplete response"));
  }
  QJsonParseError parseError;
  const auto document = QJsonDocument::fromJson(
      QByteArray::fromRawData(response.constData() + 8, response.size() - 8), &parseError);
  const auto object = document.object();
  const auto sources = object.value(QStringLiteral("sources"));
  if (parseError.error != QJsonParseError::NoError || !document.isObject() ||
      object.value(QStringLiteral("v")) != QJsonValue(1) ||
      object.value(QStringLiteral("op")) != QJsonValue(QStringLiteral("imageReferences")) ||
      !sources.isArray() || sources.toArray().size() != p_sources.size()) {
    return fail(tr("Invalid image reference response"));
  }
  const auto strings = [](const QJsonValue &p_value, QStringList &p_output) {
    if (!p_value.isArray()) {
      return false;
    }
    const auto array = p_value.toArray();
    p_output.reserve(array.size());
    for (const auto &entry : array) {
      if (!entry.isString() || entry.toString().contains(QChar::Null)) {
        return false;
      }
      p_output.append(entry.toString());
    }
    return true;
  };
  QVector<ImageReferences> result;
  result.reserve(p_sources.size());
  for (const auto &entry : sources.toArray()) {
    const auto source = entry.toObject();
    ImageReferences references;
    if (!entry.isObject() || !source.value(QStringLiteral("complete")).isBool() ||
        !strings(source.value(QStringLiteral("images")), references.m_images) ||
        !strings(source.value(QStringLiteral("strings")), references.m_strings)) {
      return fail(tr("Invalid image reference result fields"));
    }
    references.m_complete = source.value(QStringLiteral("complete")).toBool();
    result.append(std::move(references));
  }
  return result;
}
