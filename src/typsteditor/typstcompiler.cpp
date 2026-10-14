#include <vtextedit/typstcompiler.h>

#include "typstcompilerbridge.h"

#include <QDir>
#include <QTimer>

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
