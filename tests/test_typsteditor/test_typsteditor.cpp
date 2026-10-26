#include "test_typsteditor.h"

#include <QAbstractScrollArea>
#include <QAbstractTextDocumentLayout>
#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMetaMethod>
#include <QPainter>
#include <QPdfDocument>
#include <QPdfSelection>
#include <QPixmap>
#include <QPointer>
#include <QScopeGuard>
#include <QScrollBar>
#include <QSemaphore>
#include <QSet>
#include <QSignalSpy>
#include <QSplitter>
#include <QSvgRenderer>
#include <QSyntaxHighlighter>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <QThread>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <thread>

#include <vtextedit/typstcompiler.h>
#include <vtextedit/typsteditorconfig.h>
#include <vtextedit/typstfragmentrenderer.h>
#include <vtextedit/typstruntime.h>
#include <vtextedit/vtextedit.h>
#include <vtextedit/vtexteditor.h>
#include <vtextedit/vtypsteditor.h>
#include <vtextedit_typst.h>

using namespace tests;
using namespace vte;

namespace {
constexpr int operationTimeout = 90000;
constexpr int paintTimeout = 30000;

bool waitUntil(const std::function<bool()> &p_condition, int p_timeout = operationTimeout) {
  QElapsedTimer timer;
  timer.start();
  while (!p_condition()) {
    if (timer.elapsed() >= p_timeout)
      return false;
    QTest::qWait(10);
  }
  return true;
}

QJsonObject wireConfig(const QString &p_root) {
  return {{QStringLiteral("projectRoot"), p_root},
          {QStringLiteral("mainFile"), QStringLiteral("main.typ")},
          {QStringLiteral("fontPaths"), QJsonArray()},
          {QStringLiteral("packagePaths"), QJsonArray()},
          {QStringLiteral("allowDownloads"), false},
          {QStringLiteral("maxPdfBytes"), 16 * 1024 * 1024},
          {QStringLiteral("maxCacheBytes"), 32 * 1024 * 1024},
          {QStringLiteral("maxRasterPixels"), 1024 * 1024}};
}

struct Reply {
  std::shared_ptr<VteTypstReply> owner;
  QJsonObject header;
  QByteArray payload;
};

// Exercise the production ABI directly, including streamed parsed replies and
// owned binary payloads. No transport framing or substitute compiler is involved.
class NativeSession {
public:
  explicit NativeSession(const QString &p_root) : config(wireConfig(p_root)) {}
  ~NativeSession() { close(); }
  NativeSession(const NativeSession &) = delete;
  NativeSession &operator=(const NativeSession &) = delete;

  bool start() {
    if (vte_typst_abi_version() != 1 || vte_typst_session_create_v1(&session) != VTE_TYPST_OK)
      return fail(QStringLiteral("Could not create native Typst session"));
    if (!request({{QStringLiteral("op"), QStringLiteral("initialize")},
                  {QStringLiteral("config"), config}}))
      return false;
    return reply.header.value(QStringLiteral("op")) == QStringLiteral("ready") ||
           fail(QString::fromUtf8(QJsonDocument(reply.header).toJson()));
  }

  bool close() {
    auto *retired = session;
    session = nullptr;
    return !retired || vte_typst_session_destroy_v1(retired) == VTE_TYPST_OK;
  }

  bool compile(const QString &p_source, const QJsonArray &p_retained = {},
               const QJsonArray &p_imports = {}) {
    ++generation;
    QJsonArray overlays = p_imports;
    overlays.append(QJsonObject{{QStringLiteral("file"), QStringLiteral("main.typ")},
                                {QStringLiteral("text"), p_source}});
    return request({{QStringLiteral("op"), QStringLiteral("compile")},
                    {QStringLiteral("config"), config},
                    {QStringLiteral("activeFile"), QStringLiteral("main.typ")},
                    {QStringLiteral("overlays"), overlays},
                    {QStringLiteral("snapshots"), p_retained}});
  }

  bool request(QJsonObject p_header) {
    error.clear();
    parsed = {};
    reply = {};
    terminalReplies = 0;
    p_header.insert(QStringLiteral("v"), 1);
    p_header.insert(QStringLiteral("epoch"), epoch);
    p_header.insert(QStringLiteral("generation"), generation);
    p_header.insert(QStringLiteral("request"), ++requestId);
    requestHeader = p_header;
    const auto bytes = QJsonDocument(p_header).toJson(QJsonDocument::Compact);
    const auto status =
        vte_typst_session_execute_v1(session, reinterpret_cast<const uint8_t *>(bytes.constData()),
                                     size_t(bytes.size()), receive, this);
    if (status != VTE_TYPST_OK)
      return fail(error + QStringLiteral(" Native ABI status: %1").arg(status));
    return terminalReplies == 1 || fail(QStringLiteral("Missing terminal native reply"));
  }

  bool pdf(const QJsonValue &p_snapshot) {
    return request(
        {{QStringLiteral("op"), QStringLiteral("pdf")}, {QStringLiteral("snapshot"), p_snapshot}});
  }

  QString details() const {
    return error + QLatin1Char('\n') + QString::fromUtf8(QJsonDocument(reply.header).toJson());
  }

  QJsonObject config;
  Reply reply;
  QJsonObject parsed;
  std::function<void(const QJsonObject &)> onReply;

private:
  static int32_t receive(void *p_context, VteTypstReply *p_reply) noexcept {
    try {
      const std::shared_ptr<VteTypstReply> owned(
          p_reply, [](VteTypstReply *p_value) { vte_typst_reply_release(p_value); });
      auto &self = *static_cast<NativeSession *>(p_context);
      size_t headerSize = 0, payloadSize = 0;
      const auto *header = vte_typst_reply_header(owned.get(), &headerSize);
      const auto *payload = vte_typst_reply_payload(owned.get(), &payloadSize);
      if (!header || !headerSize || headerSize > 16 * 1024 * 1024 ||
          payloadSize > 128 * 1024 * 1024 || (!payload && payloadSize)) {
        self.fail(QStringLiteral("Invalid native reply spans"));
        return 1;
      }
      QJsonParseError parseError;
      const auto json = QJsonDocument::fromJson(
          QByteArray::fromRawData(reinterpret_cast<const char *>(header), qsizetype(headerSize)),
          &parseError);
      if (parseError.error != QJsonParseError::NoError || !json.isObject()) {
        self.fail(QStringLiteral("Invalid native reply metadata"));
        return 1;
      }
      const auto object = json.object();
      for (const auto *key : {"v", "epoch", "generation", "request"}) {
        if (object.value(QLatin1String(key)) != self.requestHeader.value(QLatin1String(key))) {
          self.fail(QStringLiteral("Native reply identity mismatch: ") + QLatin1String(key));
          return 1;
        }
      }
      const bool compile =
          self.requestHeader.value(QStringLiteral("op")) == QStringLiteral("compile");
      if (object.value(QStringLiteral("op")) == QStringLiteral("parsed")) {
        if (!compile || !self.parsed.isEmpty() || self.terminalReplies || payloadSize) {
          self.fail(QStringLiteral("Unexpected parsed event"));
          return 1;
        }
        self.parsed = object;
        if (self.onReply)
          self.onReply(object);
      } else {
        if (++self.terminalReplies != 1 || (compile && self.parsed.isEmpty())) {
          self.fail(QStringLiteral("Invalid native event order"));
          return 1;
        }
        self.reply = {owned, object,
                      QByteArray::fromRawData(reinterpret_cast<const char *>(payload),
                                              qsizetype(payloadSize))};
      }
      return 0;
    } catch (...) {
      // shared_ptr construction also invokes its deleter if allocation fails.
      return 1;
    }
  }
  bool fail(const QString &p_error) {
    error = p_error;
    return false;
  }
  VteTypstSession *session = nullptr;
  QString error;
  QJsonObject requestHeader;
  int terminalReplies = 0;
  // Deliberately exercise exact request identities beyond a 32-bit integer.
  const double epoch = 4294967301.0;
  double generation = 1;
  double requestId = 4294967311.0;
};

// Hold actual runtime workers at a queue boundary without replacing native work.
// Shared state survives early test returns until each queued lambda has retired.
class RuntimeLaneGate {
public:
  explicit RuntimeLaneGate(TypstRuntime &p_runtime, bool p_scanner = false)
      : state(std::make_shared<State>()), expected(p_scanner ? 1 : 2) {
    const QStringList names = p_scanner ? QStringList{QStringLiteral("typst-scanner")}
                                        : QStringList{QStringLiteral("typst-compile-0"),
                                                      QStringLiteral("typst-compile-1")};
    for (const auto &name : names) {
      auto *thread = p_runtime.findChild<QThread *>(name);
      auto *worker = thread ? thread->property("worker").value<QObject *>() : nullptr;
      if (worker && QMetaObject::invokeMethod(
                        worker,
                        [held = state] {
                          held->entered.release();
                          held->resume.acquire();
                        },
                        Qt::QueuedConnection))
        ++scheduled;
    }
  }
  ~RuntimeLaneGate() { release(); }
  bool entered() { return scheduled == expected && state->entered.tryAcquire(expected, 10000); }
  void release() {
    if (scheduled) {
      state->resume.release(scheduled);
      scheduled = 0;
    }
  }

private:
  struct State {
    QSemaphore entered;
    QSemaphore resume;
  };
  const std::shared_ptr<State> state;
  const int expected;
  int scheduled = 0;
};

class NativeDeliveryObserver : public QObject {
public:
  std::function<void()> beforeDelivery;
  std::function<bool()> shouldIntercept;

protected:
  bool eventFilter(QObject *, QEvent *p_event) Q_DECL_OVERRIDE {
    if (p_event->type() == QEvent::MetaCall && beforeDelivery &&
        (!shouldIntercept || shouldIntercept())) {
      const auto callback = std::move(beforeDelivery);
      callback();
    }
    return false;
  }
};

class Pdf {
public:
  explicit Pdf(const QByteArray &p_bytes)
      : bytes(p_bytes.constData(), p_bytes.size()), buffer(&bytes), document(nullptr) {}
  bool load() {
    if (!buffer.open(QIODevice::ReadOnly))
      return false;
    document.load(&buffer);
    return waitUntil([this]() { return document.status() == QPdfDocument::Status::Ready; },
                     paintTimeout);
  }
  QString text(int p_page = 0) { return document.getAllText(p_page).text().trimmed(); }
  QSizeF size(int p_page) const { return document.pagePointSize(p_page); }
  QByteArray bytes;
  QBuffer buffer;
  QPdfDocument document;
};

QString shortSource(const QString &p_text) {
  return QStringLiteral("#set page(width: 240pt, height: 160pt, margin: 16pt)\n") + p_text;
}

QSharedPointer<TypstEditorConfig> editorConfig(const QString &p_root, TypstRuntime *p_runtime) {
  auto config = QSharedPointer<TypstEditorConfig>::create();
  config->m_runtime = p_runtime;
  config->m_projectRoot = p_root;
  config->m_debounceMs = 10;
  config->m_compileTimeoutMs = operationTimeout;
  config->m_maxRasterPixels = 1024 * 1024;
  config->m_maxCacheBytes = 32 * 1024 * 1024;
  config->m_textEditorConfig->m_inputMode = InputMode::NormalMode;
  return config;
}

class EditorFixture {
public:
  explicit EditorFixture(TypstRuntime &p_runtime, const QString &p_root, const QString &p_source,
                         VTypstEditor::Mode p_mode = VTypstEditor::Mode::Read,
                         QSharedPointer<TypstEditorConfig> p_config = {}, bool p_inplace = true,
                         bool p_live = false)
      : editor(p_config ? p_config : editorConfig(p_root, &p_runtime),
               QSharedPointer<TextEditorParameters>::create()) {
    editor.getTextEditor()->setSpellCheckEnabled(false);
    editor.setInplacePreviewEnabled(p_inplace);
    editor.setLivePreviewEnabled(p_live);
    editor.setFileOverlay(QStringLiteral("main.typ"), p_source);
    editor.setMode(p_mode);
    editor.resize(1000, 650);
    editor.show();
    editor.activateWindow();
    QApplication::setActiveWindow(&editor);
  }
  QAbstractScrollArea *pane() const {
    return editor.findChild<QAbstractScrollArea *>(QStringLiteral("typstPdf"));
  }
  VTextEdit *edit() const { return editor.getTextEditor()->getTextEdit(); }
  QString selected() const { return pane()->property("selectedText").toString(); }
  QString selectPage() {
    pane()->setFocus();
    QTest::keyClick(pane(), Qt::Key_A, Qt::ControlModifier);
    return selected();
  }
  void clearSelection() { QTest::keyClick(pane(), Qt::Key_Escape); }
  bool ready() {
    return waitUntil([this]() {
             const auto state = editor.compileStatus();
             return state == VTypstEditor::CompileStatus::Ready ||
                    state == VTypstEditor::CompileStatus::Error ||
                    state == VTypstEditor::CompileStatus::Unavailable;
           }) &&
           editor.compileStatus() == VTypstEditor::CompileStatus::Ready;
  }
  VTypstEditor editor;
};

QString sharpPdfSource() {
  return QStringLiteral("#set page(width: 360pt, height: 900pt, margin: (x: 28pt, y: 96pt))\n"
                        "#set text(size: 12pt, fill: red)\n"
                        "Sharp text: affine office minimum integral.\n\n"
                        "Fractions and roots remain crisp at screen resolution.\n\n"
                        "$ (a^2 + b^2)/(1 + sqrt(1 + x^2)) = sum_(k=1)^8 1/k^2 $\n\n"
                        "Fine strokes, curved letters, and mathematical symbols.\n"
                        "#pagebreak()\n"
                        "Offscreen neighbor: prefetch must not soften the visible page.\n"
                        "$ integral_0^1 x^2 dif x = 1/3 $");
}

QObject *compilerBridge(QObject *p_editor) {
  for (auto *child : p_editor->findChildren<QObject *>()) {
    if (child->metaObject()->indexOfSignal("generationChanged(quint64,quint64)") >= 0)
      return child;
  }
  return nullptr;
}

std::unique_ptr<QSignalSpy> observePdfExports(QObject *p_editor) {
  for (auto *child : p_editor->findChildren<QObject *>()) {
    const auto *meta = child->metaObject();
    for (int index = meta->methodOffset(); index < meta->methodCount(); ++index) {
      const auto signal = meta->method(index);
      if (signal.methodType() == QMetaMethod::Signal && signal.name() == "pdfReady" &&
          signal.parameterCount() == 2 && signal.parameterTypes().at(1) == "QByteArray")
        return std::make_unique<QSignalSpy>(child, signal);
    }
  }
  return {};
}

bool fitFirstPdfPage(QAbstractScrollArea *p_pane) {
  return QMetaObject::invokeMethod(p_pane, "fitWidth", Qt::DirectConnection) &&
         QMetaObject::invokeMethod(p_pane, "goToPage", Qt::DirectConnection, Q_ARG(int, 0),
                                   Q_ARG(QPointF, QPointF()));
}

QRectF firstPdfPageRect(QAbstractScrollArea *p_pane, const QSizeF &p_points) {
  const qreal scale =
      p_pane->property("zoom").toDouble() * (qMax(qreal(1), qreal(p_pane->logicalDpiY())) / 72.0);
  const QSizeF size = p_points * scale;
  return QRectF(QPointF((p_pane->viewport()->width() - size.width()) / 2, 16), size);
}

QSize physicalPdfSize(const QRectF &p_page, qreal p_dpr) {
  return QSize(int(std::ceil(p_page.width() * p_dpr)), int(std::ceil(p_page.height() * p_dpr)));
}

QImage paintPdfOracle(Pdf &p_pdf, const QSize &p_rasterSize, const QRectF &p_page,
                      const QImage &p_frame) {
  const auto raster = p_pdf.document.render(0, p_rasterSize);
  if (raster.isNull())
    return {};
  QImage reference(p_frame.size(), QImage::Format_RGB32);
  reference.setDevicePixelRatio(p_frame.devicePixelRatio());
  reference.fill(Qt::white);
  QPainter painter(&reference);
  painter.setRenderHint(QPainter::SmoothPixmapTransform);
  painter.drawImage(p_page, raster, QRectF(raster.rect()));
  return reference;
}

QRect pdfInkInterior(const QRectF &p_page, const QSizeF &p_points, const QImage &p_frame) {
  const qreal scale = p_page.width() / p_points.width();
  const qreal dpr = p_frame.devicePixelRatio();
  // Text starts at 96pt. Ignore page edges and the transient top-left status banners,
  // but retain normal letters, fractions, roots, and thin mathematical strokes.
  const qreal top = qMax(qreal(96), p_page.top() + 84 * scale);
  const QRectF interior(p_page.left() + 20 * scale, top, p_page.width() - 40 * scale,
                        p_frame.height() / dpr - 16 - top);
  return QRectF(interior.topLeft() * dpr, interior.size() * dpr)
      .toAlignedRect()
      .intersected(p_frame.rect());
}

struct PdfInkDifference {
  qint64 pixels = 0;
  qreal meanError = 255;
};

PdfInkDifference pdfInkDifference(const QImage &p_frame, const QImage &p_reference,
                                  const QRect &p_interior) {
  PdfInkDifference difference;
  if (p_frame.isNull() || p_reference.isNull() || p_frame.size() != p_reference.size())
    return difference;
  const auto frame = p_frame.convertToFormat(QImage::Format_RGB32);
  const auto reference = p_reference.convertToFormat(QImage::Format_RGB32);
  qint64 error = 0;
  for (int y = p_interior.top(); y <= p_interior.bottom(); ++y) {
    const auto *actual = reinterpret_cast<const QRgb *>(frame.constScanLine(y));
    const auto *expected = reinterpret_cast<const QRgb *>(reference.constScanLine(y));
    for (int x = p_interior.left(); x <= p_interior.right(); ++x) {
      if (qMin(qRed(actual[x]), qMin(qGreen(actual[x]), qBlue(actual[x]))) >= 245 &&
          qMin(qRed(expected[x]), qMin(qGreen(expected[x]), qBlue(expected[x]))) >= 245)
        continue;
      ++difference.pixels;
      error += qMax(qAbs(qRed(actual[x]) - qRed(expected[x])),
                    qMax(qAbs(qGreen(actual[x]) - qGreen(expected[x])),
                         qAbs(qBlue(actual[x]) - qBlue(expected[x]))));
    }
  }
  if (difference.pixels)
    difference.meanError = qreal(error) / difference.pixels;
  return difference;
}

int textChangeCount(const QSignalSpy &p_changes) {
  int count = 0;
  for (const auto &change : p_changes) {
    // Layout/highlight-only notifications are not source mutations.
    if (change.at(1).toInt() || change.at(2).toInt())
      ++count;
  }
  return count;
}

QImage widgetImage(QWidget *p_widget) {
  QImage image(p_widget->size(), QImage::Format_ARGB32_Premultiplied);
  image.fill(Qt::transparent);
  p_widget->render(&image);
  return image;
}

int pixelsWithColor(const QImage &p_image, const QColor &p_color) {
  int count = 0;
  for (int y = 0; y < p_image.height(); ++y) {
    for (int x = 0; x < p_image.width(); ++x) {
      const auto color = p_image.pixelColor(x, y);
      // Typst's glyph rasterizer can leave nearly opaque ink one channel step short.
      if (std::abs(color.red() - p_color.red()) <= 3 &&
          std::abs(color.green() - p_color.green()) <= 3 &&
          std::abs(color.blue() - p_color.blue()) <= 3 &&
          std::abs(color.alpha() - p_color.alpha()) <= 3) {
        ++count;
      }
    }
  }
  return count;
}

QImage mathFragmentImage(const TypstFragmentRenderer::Result &p_result) {
  QSvgRenderer svg(p_result.m_svg);
  if (!svg.isValid() || p_result.m_sizePt.isEmpty())
    return {};
  const QSizeF pixels = p_result.m_sizePt * 4;
  QImage image(QSize(int(std::ceil(pixels.width())), int(std::ceil(pixels.height()))),
               QImage::Format_ARGB32_Premultiplied);
  image.fill(Qt::transparent);
  QPainter painter(&image);
  svg.render(&painter, QRectF(QPointF(), pixels));
  return image;
}

int coloredPixels(const QImage &p_image, bool p_red, QRect *p_bounds = nullptr) {
  int count = 0;
  if (p_bounds)
    *p_bounds = QRect();
  for (int y = 0; y < p_image.height(); ++y) {
    for (int x = 0; x < p_image.width(); ++x) {
      const auto color = p_image.pixelColor(x, y);
      if (color.alpha() > 128 && color.green() < 100 &&
          (p_red ? color.red() > 170 && color.blue() < 100
                 : color.blue() > 170 && color.red() < 100)) {
        ++count;
        if (p_bounds)
          *p_bounds |= QRect(x, y, 1, 1);
      }
    }
  }
  return count;
}

TypstResourceContext
resourceMenuAt(VTypstEditor &p_editor, QWidget *p_target, const QPoint &p_point,
               const std::function<void(const TypstResourceContext &)> &p_action = {}) {
  TypstResourceContext captured;
  bool invoked = false;
  const auto connection =
      QObject::connect(&p_editor, &VTypstEditor::resourceContextMenuRequested, &p_editor,
                       [&](const TypstResourceContext &p_context, QMenu *p_menu) {
                         captured = p_context;
                         auto *action = p_menu->addAction(QStringLiteral("Inspect resource"));
                         QObject::connect(action, &QAction::triggered, p_menu, [&, p_context]() {
                           invoked = true;
                           if (p_action)
                             p_action(p_context);
                         });
                         QTimer::singleShot(0, p_menu, [p_menu, action]() {
                           QTest::mouseClick(p_menu, Qt::LeftButton, Qt::NoModifier,
                                             p_menu->actionGeometry(action).center());
                         });
                         QTimer::singleShot(3000, p_menu, &QMenu::close);
                       });
  QContextMenuEvent event(QContextMenuEvent::Mouse, p_point, p_target->mapToGlobal(p_point));
  QCoreApplication::sendEvent(p_target, &event);
  QObject::disconnect(connection);
  if (!invoked)
    return {};
  return captured;
}

TypstResourceContext
sourceResourceAt(EditorFixture &p_fixture, int p_offset,
                 const std::function<void(const TypstResourceContext &)> &p_action = {}) {
  QTextCursor cursor(p_fixture.editor.document());
  cursor.setPosition(p_offset);
  return resourceMenuAt(p_fixture.editor, p_fixture.edit()->viewport(),
                        p_fixture.edit()->cursorRect(cursor).center(), p_action);
}

QWidget *resourceCard(VTypstEditor &p_editor, bool p_red, QPoint *p_point = nullptr) {
  for (auto *card : p_editor.findChildren<QWidget *>(QStringLiteral("typstPreviewCard"))) {
    if (!card->isVisible())
      continue;
    QRect bounds;
    if (coloredPixels(widgetImage(card), p_red, &bounds) > 20) {
      if (p_point)
        *p_point = bounds.center();
      return card;
    }
  }
  return nullptr;
}

bool writeResourceImage(const QString &p_path, const QColor &p_color,
                        QByteArray *p_bytes = nullptr) {
  QImage image(12, 8, QImage::Format_ARGB32);
  image.fill(p_color);
  QByteArray bytes;
  QBuffer buffer(&bytes);
  if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
    return false;
  QFile file(p_path);
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
    return false;
  if (p_bytes)
    *p_bytes = bytes;
  return true;
}

bool observeRasters(QObject *p_editor, TypstRasterObserver *p_observer) {
  QObject *host = nullptr;
  QObject *bridge = nullptr;
  for (auto *child : p_editor->findChildren<QObject *>()) {
    if (child->metaObject()->indexOfSignal("rasterRequested(quint64,quint64,int,qreal)") >= 0)
      host = child;
    if (child->metaObject()->indexOfSignal("rasterCancelled(quint64,quint64,int,qreal)") >= 0)
      bridge = child;
  }
  const auto connectSignal = [p_observer](QObject *p_sender, const char *p_signature) {
    if (!p_sender)
      return false;
    const auto *senderMeta = p_sender->metaObject();
    const auto *observerMeta = p_observer->metaObject();
    const int signal = senderMeta->indexOfSignal(p_signature);
    const int slot = observerMeta->indexOfSlot(p_signature);
    return signal >= 0 && slot >= 0 &&
           QObject::connect(p_sender, senderMeta->method(signal), p_observer,
                            observerMeta->method(slot), Qt::DirectConnection);
  };
  // Construction already connected the bridge directly to the host. Our direct
  // observer therefore edits after the real request is sent, before its reply.
  return connectSignal(host, "rasterRequested(quint64,quint64,int,qreal)") &&
         connectSignal(bridge, "rasterCancelled(quint64,quint64,int,qreal)") &&
         connectSignal(bridge, "pageImageReady(quint64,quint64,int,qreal,QImage)");
}

QList<QImage> previewImages(VTypstEditor &p_editor) {
  auto cards = p_editor.findChildren<QWidget *>(QStringLiteral("typstPreviewCard"));
  std::sort(cards.begin(), cards.end(), [](QWidget *p_a, QWidget *p_b) {
    return p_a->y() == p_b->y() ? p_a->x() < p_b->x() : p_a->y() < p_b->y();
  });
  QList<QImage> images;
  for (auto *card : cards) {
    if (card->isVisible())
      images.append(widgetImage(card));
  }
  return images;
}

bool previewHasOnlyColor(const QList<QImage> &p_images, bool p_red) {
  return p_images.size() == 1 && coloredPixels(p_images.front(), p_red) > 20 &&
         coloredPixels(p_images.front(), !p_red) == 0;
}

int inkPixels(const QImage &p_image, const QRect &p_rect) {
  int count = 0;
  const auto rect = p_rect.intersected(p_image.rect());
  for (int y = rect.top(); y <= rect.bottom(); ++y) {
    for (int x = rect.left(); x <= rect.right(); ++x) {
      const auto color = p_image.pixelColor(x, y);
      if (color.alpha() > 128 && qGray(color.rgb()) < 160)
        ++count;
    }
  }
  return count;
}

QJsonArray importOverlay(const QString &p_value) {
  return {QJsonObject{
      {QStringLiteral("file"), QStringLiteral("chapter.typ")},
      {QStringLiteral("text"), QStringLiteral("#let phrase = [") + p_value + QLatin1Char(']')}}};
}

bool writeFixture(const QString &p_path, const QByteArray &p_content) {
  QFile file(p_path);
  return file.open(QIODevice::WriteOnly) && file.write(p_content) == p_content.size();
}

QSharedPointer<Theme> textTheme(const QJsonObject &p_base, const QJsonObject &p_override) {
  const QJsonObject theme{
      {QStringLiteral("metadata"), QJsonObject{{QStringLiteral("type"), "vtextedit"}}},
      {QStringLiteral("editor-styles"), QJsonObject{{QStringLiteral("Text"), p_base}}},
      {QStringLiteral("typst-editor-styles"), QJsonObject{{QStringLiteral("Text"), p_override}}},
      {QStringLiteral("typst-syntax-styles"),
       QJsonObject{{QStringLiteral("string"), QJsonObject()}}}};
  return Theme::createThemeFromContent(QString::fromUtf8(QJsonDocument(theme).toJson()));
}

QImage exportedPdfImage(const QSignalSpy &p_exports) {
  if (p_exports.isEmpty())
    return {};
  Pdf pdf(p_exports.last().at(1).toByteArray());
  if (!pdf.load() || pdf.document.pageCount() != 1)
    return {};
  return pdf.document.render(0, QSize(480, 320));
}

QTextCharFormat syntaxFormatAt(QTextDocument *p_document, int p_position) {
  QTextCharFormat format;
  if (p_position < 0 || p_position >= p_document->characterCount() - 1)
    return format;
  const auto block = p_document->findBlock(p_position);
  if (!block.isValid() || !block.layout())
    return format;
  const int offset = p_position - block.position();
  for (const auto &range : block.layout()->formats()) {
    if (offset >= range.start && offset - range.start < range.length)
      format.merge(range.format);
  }
  return format;
}

bool writeTypstTheme(const QString &p_path, const QString &p_stringColor, bool p_decorated = true) {
  auto theme =
      QJsonDocument::fromJson(
          QStringLiteral(R"({
    "metadata": {"type": "vtextedit", "name": "TypstTest"},
    "editor-styles": {"Text": {"font-size": 11}},
    "markdown-syntax-styles": {
      "COMMENT": {"text-color": "#557755"},
      "H1": {"text-color": "#0033aa", "font-size": 19, "bold": %2},
      "STRONG": {"bold": %2},
      "EMPH": {"italic": %2}
    },
    "typst-syntax-styles": {
      "comment": "@markdown-syntax-styles:COMMENT",
      "heading": "@markdown-syntax-styles:H1",
      "strong": "@markdown-syntax-styles:STRONG",
      "emph": "@markdown-syntax-styles:EMPH",
      "keyword": {"text-color": "#0033aa", "bold": %2},
      "number": {"text-color": "#aa5500"},
      "string": {"text-color": "%1", "background-color": "#ffeeee", "font-size": 13,
                 "font-family": "Consolas", "underline": %2, "strike-through": %2},
      "function": {"text-color": "#663399"},
      "mathdelimiter": {"text-color": "#007777"}
    }
  })")
              .arg(p_stringColor, p_decorated ? QStringLiteral("true") : QStringLiteral("false"))
              .toUtf8())
          .object();
  if (!p_decorated) {
    auto styles = theme.value(QStringLiteral("typst-syntax-styles")).toObject();
    styles.insert(QStringLiteral("raw"), QJsonObject{{QStringLiteral("text-color"), "#886600"}});
    theme.insert(QStringLiteral("typst-syntax-styles"), styles);
  }
  return writeFixture(p_path, QJsonDocument(theme).toJson());
}

bool writeCodeSyntaxTheme(const QString &p_path, const QString &p_keywordColor,
                          const QString &p_controlFlowColor, const QString &p_stringColor,
                          bool p_decorated) {
  const QJsonObject colors{{QStringLiteral("Normal"), "#223344"},
                           {QStringLiteral("Keyword"), p_keywordColor},
                           {QStringLiteral("ControlFlow"), p_controlFlowColor},
                           {QStringLiteral("String"), p_stringColor},
                           {QStringLiteral("DataType"), "#007788"},
                           {QStringLiteral("BuiltIn"), "#882288"},
                           {QStringLiteral("Function"), "#663399"},
                           {QStringLiteral("Constant"), "#995500"},
                           {QStringLiteral("DecVal"), "#aa5500"},
                           {QStringLiteral("BaseN"), "#446600"},
                           {QStringLiteral("Float"), "#005599"},
                           {QStringLiteral("Char"), "#993344"},
                           {QStringLiteral("SpecialChar"), "#bb0066"},
                           {QStringLiteral("VerbatimString"), "#337744"},
                           {QStringLiteral("SpecialString"), "#995577"},
                           {QStringLiteral("Comment"), "#557755"},
                           {QStringLiteral("Documentation"), "#227766"},
                           {QStringLiteral("Attribute"), "#775500"},
                           {QStringLiteral("Preprocessor"), "#6655aa"},
                           {QStringLiteral("Import"), "#225599"},
                           {QStringLiteral("Variable"), "#555588"},
                           {QStringLiteral("Operator"), "#444444"},
                           {QStringLiteral("Error"), "#bb0000"},
                           {QStringLiteral("Others"), "#664488"},
                           {QStringLiteral("Information"), "#335577"}};
  QJsonObject styles;
  for (auto it = colors.begin(); it != colors.end(); ++it) {
    QJsonObject style{{QStringLiteral("text-color"), it.value()},
                      {QStringLiteral("bold"), p_decorated},
                      {QStringLiteral("italic"), p_decorated},
                      {QStringLiteral("underline"), p_decorated},
                      {QStringLiteral("strike-through"), p_decorated}};
    if (it.key() == QStringLiteral("String"))
      style.insert(QStringLiteral("background-color"), QStringLiteral("#ffeeee"));
    styles.insert(it.key(), style);
  }
  const QJsonObject theme{
      {QStringLiteral("metadata"),
       QJsonObject{{QStringLiteral("name"), QFileInfo(p_path).completeBaseName()}}},
      {QStringLiteral("text-styles"), styles}};
  return writeFixture(p_path, QJsonDocument(theme).toJson());
}

QString splitSource() {
  return QStringLiteral(
      "#set page(width: 100mm, height: 30mm, margin: 4mm)\n"
      "#set text(fill: red)\n"
      "#show math.equation: set block(breakable: true)\n"
      "Before\n$ x &= 1 \\ x &= 2 \\ x &= 3 \\ x &= 4 \\ x &= 5 \\ x &= 6 \\ x &= 7 $\nAfter");
}
} // namespace

void TestTypstEditor::nativeReplySurvivesSessionRetirement() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  Reply retained;
  {
    NativeSession native(root.path());
    QVERIFY2(native.start(), qPrintable(native.details()));
    QVERIFY2(native.compile(shortSource(QStringLiteral("Retained native bytes."))),
             qPrintable(native.details()));
    const auto snapshot = native.reply.header.value(QStringLiteral("snapshot"));
    QVERIFY2(native.pdf(snapshot), qPrintable(native.details()));
    QCOMPARE(native.reply.header.value(QStringLiteral("op")).toString(), QStringLiteral("pdf"));
    retained = native.reply;
    QVERIFY(native.close());
  }
  QVERIFY(retained.owner);
  QVERIFY(retained.payload.startsWith("%PDF-"));
  Pdf pdf(retained.payload);
  QVERIFY(pdf.load());
  QCOMPARE(pdf.text(), QStringLiteral("Retained native bytes."));
  // The ABI span aliases Rust-owned Bytes. Release on another thread after the
  // session is gone; the consumer PDF has already taken its independent copy.
  auto owner = std::move(retained.owner);
  retained = {};
  std::thread release([owned = std::move(owner)]() mutable { owned.reset(); });
  release.join();
  QCOMPARE(pdf.text(), QStringLiteral("Retained native bytes."));
}

void TestTypstEditor::nativeCallbackFailureRetiresRequest() {
  VteTypstSession *session = nullptr;
  QCOMPARE(vte_typst_session_create_v1(&session), VTE_TYPST_OK);
  const auto destroy = qScopeGuard([&] { vte_typst_session_destroy_v1(session); });
  QTemporaryDir root;
  QVERIFY(root.isValid());
  QJsonObject request{{QStringLiteral("v"), 1},
                      {QStringLiteral("epoch"), 1},
                      {QStringLiteral("generation"), 1},
                      {QStringLiteral("request"), 1},
                      {QStringLiteral("op"), QStringLiteral("initialize")},
                      {QStringLiteral("config"), wireConfig(root.path())}};
  auto bytes = QJsonDocument(request).toJson(QJsonDocument::Compact);
  int callbacks = 0;
  const auto reject = [](void *p_context, VteTypstReply *p_reply) noexcept -> int32_t {
    ++*static_cast<int *>(p_context);
    vte_typst_reply_release(p_reply);
    return 1;
  };
  QCOMPARE(vte_typst_session_execute_v1(session,
                                        reinterpret_cast<const uint8_t *>(bytes.constData()),
                                        size_t(bytes.size()), reject, &callbacks),
           VTE_TYPST_CALLBACK_FAILED);
  QCOMPARE(callbacks, 1);
  request[QStringLiteral("request")] = 2;
  request[QStringLiteral("op")] = QStringLiteral("compile");
  request[QStringLiteral("activeFile")] = QStringLiteral("main.typ");
  request[QStringLiteral("overlays")] = QJsonArray{
      QJsonObject{{QStringLiteral("file"), QStringLiteral("main.typ")},
                  {QStringLiteral("text"), shortSource(QStringLiteral("Not delivered"))}}};
  bytes = QJsonDocument(request).toJson(QJsonDocument::Compact);
  callbacks = 0;
  QCOMPARE(vte_typst_session_execute_v1(session,
                                        reinterpret_cast<const uint8_t *>(bytes.constData()),
                                        size_t(bytes.size()), reject, &callbacks),
           VTE_TYPST_CALLBACK_FAILED);
  QCOMPARE(callbacks, 1); // Rejecting parsed must stop further delivery.
  NativeSession recovered(root.path());
  QVERIFY2(recovered.start(), qPrintable(recovered.details()));
  QVERIFY2(recovered.compile(shortSource(QStringLiteral("Fresh session"))),
           qPrintable(recovered.details()));
  QVERIFY(recovered.reply.header.contains(QStringLiteral("snapshot")));
}

void TestTypstEditor::unavailableRuntimePreservesSource_data() {
  QTest::addColumn<bool>("stopped");
  QTest::newRow("missing-runtime") << false;
  QTest::newRow("stopped-runtime") << true;
}

void TestTypstEditor::unavailableRuntimePreservesSource() {
  QFETCH(bool, stopped);
  QTemporaryDir root;
  QVERIFY(root.isValid());
  TypstRuntime runtime;
  QVERIFY(runtime.isAvailable());
  if (stopped) {
    runtime.shutdown();
    runtime.shutdown(); // Drain is idempotent; it never restarts the runtime.
    QVERIFY(!runtime.isAvailable());
  }
  auto config = editorConfig(root.path(), stopped ? &runtime : nullptr);
  const auto source = shortSource(QStringLiteral("Source survives without compilation."));
  EditorFixture fixture(runtime, root.path(), source, VTypstEditor::Mode::Edit, config);
  QVERIFY(waitUntil(
      [&] { return fixture.editor.compileStatus() == VTypstEditor::CompileStatus::Unavailable; }));
  QCOMPARE(fixture.editor.document()->toPlainText(), source);
  QVERIFY(!fixture.edit()->isReadOnly());
  QTextCursor cursor(fixture.editor.document());
  cursor.movePosition(QTextCursor::End);
  cursor.insertText(QStringLiteral("\nStill editable."));
  QVERIFY(fixture.editor.document()->isUndoAvailable());
  fixture.editor.document()->undo();
  QCOMPARE(fixture.editor.document()->toPlainText(), source);
  fixture.editor.retryCompilation();
  QCOMPARE(fixture.editor.pdfPageCount(), 0);
  TypstCompiler compiler(*config);
  QSignalSpy completed(&compiler, &TypstCompiler::finished);
  QVERIFY(compiler.compileToSvg(source));
  QCOMPARE(completed.count(), 0);
  QVERIFY(waitUntil([&] { return completed.count() == 1; }));
  const auto failure = qvariant_cast<TypstCompiler::Result>(completed.first().first());
  QVERIFY(!failure.isSuccess());
  QVERIFY(!failure.m_error.isEmpty());
  QVERIFY(failure.m_svgPages.isEmpty());
  QVERIFY(!compiler.isCompiling());
}

void TestTypstEditor::runtimeSessionsRemainIndependent() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  const auto source = shortSource(QStringLiteral("#include \"shared.typ\""));
  EditorFixture first(m_runtime, root.path(), source);
  EditorFixture second(m_runtime, root.path(), source);
  first.editor.setFileOverlay(QStringLiteral("shared.typ"), QStringLiteral("= First session"));
  second.editor.setFileOverlay(QStringLiteral("shared.typ"), QStringLiteral("= Second session"));
  QVERIFY2(first.ready(), qPrintable(first.editor.compileMessage()));
  QVERIFY2(second.ready(), qPrintable(second.editor.compileMessage()));
  QVERIFY(waitUntil([&] { return first.editor.pdfPageCount() && second.editor.pdfPageCount(); }));
  QCOMPARE(first.selectPage().trimmed(), QStringLiteral("First session"));
  QCOMPARE(second.selectPage().trimmed(), QStringLiteral("Second session"));
  first.editor.setFileOverlay(QStringLiteral("shared.typ"), QStringLiteral("#let broken = ("));
  QVERIFY(waitUntil(
      [&] { return first.editor.compileStatus() == VTypstEditor::CompileStatus::Error; }));
  QVERIFY(!first.editor.diagnostics().isEmpty());
  QCOMPARE(second.editor.compileStatus(), VTypstEditor::CompileStatus::Ready);
  QCOMPARE(second.selectPage().trimmed(), QStringLiteral("Second session"));
  QVERIFY(second.editor.diagnostics().isEmpty());
  auto restricted = QSharedPointer<TypstEditorConfig>::create(first.editor.getConfig());
  restricted->m_maxPdfBytes = 64;
  first.editor.setConfig(restricted);
  first.editor.setFileOverlay(QStringLiteral("shared.typ"),
                              QStringLiteral("= First export failure"));
  QVERIFY(waitUntil(
      [&] { return first.editor.compileStatus() == VTypstEditor::CompileStatus::Error; }));
  QVERIFY(!first.editor.compileMessage().isEmpty());
  second.editor.setFileOverlay(QStringLiteral("shared.typ"),
                               QStringLiteral("= Second replacement"));
  QVERIFY(waitUntil([&] { return second.editor.hasPendingPdfUpdate(); }));
  QCOMPARE(second.selected().trimmed(), QStringLiteral("Second session"));
  QCOMPARE(first.selectPage().trimmed(), QStringLiteral("First session"));
  second.clearSelection();
  QVERIFY(waitUntil([&] { return !second.editor.hasPendingPdfUpdate(); }));
  QCOMPARE(second.selectPage().trimmed(), QStringLiteral("Second replacement"));
  QSignalSpy firstRecovered(first.pane(), SIGNAL(snapshotPublished(quint64)));
  first.editor.setConfig(editorConfig(root.path(), &m_runtime));
  first.editor.setFileOverlay(QStringLiteral("shared.typ"), QStringLiteral("= First recovered"));
  first.clearSelection();
  QVERIFY2(first.ready(), qPrintable(first.editor.compileMessage()));
  QVERIFY(waitUntil([&] { return firstRecovered.count() == 1; }));
  QCOMPARE(first.selectPage().trimmed(), QStringLiteral("First recovered"));
  QCOMPARE(second.selectPage().trimmed(), QStringLiteral("Second replacement"));
}

void TestTypstEditor::scannerFailsClosedWithoutEvaluation() {
  QString error;
  QVERIFY(TypstCompiler::collectImageReferences({QString()}, nullptr, &error).isEmpty());
  QVERIFY(!error.isEmpty());
  TypstRuntime runtime;
  QVERIFY(runtime.isAvailable());
  for (const auto &sources :
       {QStringList(), QStringList{QString(), QString(), QString(), QString()}}) {
    error.clear();
    QVERIFY(TypstCompiler::collectImageReferences(sources, &runtime, &error).isEmpty());
    QVERIFY(!error.isEmpty());
  }
  const auto scans = TypstCompiler::collectImageReferences(
      {QStringLiteral("#let unresolved = unknown_function()\n#image(\"kept.png\")"),
       QStringLiteral("#let load = image\n#load(\"uncertain.png\")"),
       QStringLiteral("#image(\"unterminated")},
      &runtime, &error);
  QVERIFY2(error.isEmpty(), qPrintable(error));
  QCOMPARE(scans.size(), 3);
  QVERIFY(scans[0].m_complete);
  QCOMPARE(scans[0].m_images, QStringList{QStringLiteral("kept.png")});
  QVERIFY(!scans[1].m_complete);
  QVERIFY(!scans[2].m_complete);
  runtime.shutdown();
  error.clear();
  QVERIFY(TypstCompiler::collectImageReferences({QString()}, &runtime, &error).isEmpty());
  QVERIFY(!error.isEmpty());
}

void TestTypstEditor::scannerDeadlineAndBusyFailClosed() {
  TypstRuntime runtime;
  RuntimeLaneGate gate(runtime, true);
  QVERIFY(gate.entered());
  bool guiEventDelivered = false;
  QObject receiver;
  QTimer::singleShot(0, &receiver, [&] { guiEventDelivered = true; });
  QString error;
  QElapsedTimer elapsed;
  elapsed.start();
  const auto abandoned = TypstCompiler::collectImageReferences(
      {QStringLiteral("#image(\"abandoned.png\")")}, &runtime, &error);
  QVERIFY(abandoned.isEmpty());
  QVERIFY(!error.isEmpty());
  QVERIFY(elapsed.elapsed() >= 9000);
  QVERIFY(elapsed.elapsed() < 15000);
  QVERIFY(!guiEventDelivered); // Accepted-close scanning must not pump nested GUI events.
  elapsed.restart();
  QVERIFY(TypstCompiler::collectImageReferences({QString()}, &runtime, &error).isEmpty());
  QVERIFY(!error.isEmpty());
  QVERIFY(elapsed.elapsed() < 1000); // Do not queue behind an abandoned scan.
  gate.release();
  QVector<TypstCompiler::ImageReferences> recovered;
  QVERIFY(waitUntil([&] {
    recovered = TypstCompiler::collectImageReferences({QStringLiteral("#image(\"current.png\")")},
                                                      &runtime, &error);
    return !recovered.isEmpty();
  }));
  QVERIFY2(error.isEmpty(), qPrintable(error));
  QCOMPARE(recovered.size(), 1);
  QVERIFY(recovered.first().m_complete);
  QCOMPARE(recovered.first().m_images, QStringList{QStringLiteral("current.png")});
}

void TestTypstEditor::runtimeDrainsRunningNativeCall() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  TypstRuntime runtime;
  auto *thread = runtime.findChild<QThread *>(QStringLiteral("typst-compile-0"));
  QVERIFY(thread);
  auto *worker = thread->property("worker").value<QObject *>();
  QVERIFY(worker);
  struct Invocation {
    QSemaphore parsed;
    QSemaphore resume;
    QSemaphore returned;
    bool success = false;
    QString error;
  };
  const auto invocation = std::make_shared<Invocation>();
  const auto releaseOnFailure = qScopeGuard([&] { invocation->resume.release(); });
  QVERIFY(QMetaObject::invokeMethod(
      worker,
      [invocation, project = root.path()] {
        NativeSession native(project);
        if (native.start()) {
          native.onReply = [invocation](const QJsonObject &) {
            invocation->parsed.release();
            invocation->resume.acquire();
          };
          invocation->success = native.compile(shortSource(QStringLiteral("Finite native drain")));
        }
        invocation->error = native.details();
        native.close(); // Session destruction is on the same lane as execution.
        invocation->returned.release();
      },
      Qt::QueuedConnection));
  QVERIFY2(invocation->parsed.tryAcquire(1, operationTimeout), "Native compile did not parse");
  QVERIFY(!invocation->returned.tryAcquire());

  // A real call is held inside its parsed C callback, not a mock operation or
  // unbounded Typst loop. The separate scanner and the GUI must remain usable.
  QString error;
  const auto scans = TypstCompiler::collectImageReferences(
      {QStringLiteral("#image(\"independent.png\")")}, &runtime, &error);
  QVERIFY2(error.isEmpty(), qPrintable(error));
  QCOMPARE(scans.size(), 1);
  bool responsive = false;
  QTimer::singleShot(0, &runtime, [&] { responsive = true; });
  QVERIFY(waitUntil([&] { return responsive; }));

  // isAvailable() reads the runtime's atomic admission flag. Release only after
  // shutdown has stopped admission: draining cannot depend on GUI acknowledgements.
  bool observedShutdown = false;
  std::thread release([&] {
    QElapsedTimer timeout;
    timeout.start();
    while (runtime.isAvailable() && timeout.elapsed() < 10000)
      std::this_thread::yield();
    observedShutdown = !runtime.isAvailable();
    invocation->resume.release();
  });
  runtime.shutdown();
  release.join();
  QVERIFY(observedShutdown);
  QVERIFY(invocation->returned.tryAcquire());
  QVERIFY2(invocation->success, qPrintable(invocation->error));
  QVERIFY(!runtime.isAvailable());
  for (auto *lane : runtime.findChildren<QThread *>())
    QVERIFY(!lane->isRunning());
  runtime.shutdown();
}

void TestTypstEditor::runtimeDropsRepliesForDeletedReceiver() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  auto fixture = std::make_unique<EditorFixture>(m_runtime, root.path(),
                                                 shortSource(QStringLiteral("Receiver baseline")));
  QVERIFY2(fixture->ready(), qPrintable(fixture->editor.compileMessage()));
  QPointer<VTypstEditor> receiver(&fixture->editor);
  auto *delivery = m_runtime.findChild<QObject *>(QStringLiteral("typst-delivery"));
  QVERIFY(delivery);
  NativeDeliveryObserver observer;
  delivery->installEventFilter(&observer);
  bool retired = false;
  observer.beforeDelivery = [&] {
    fixture.reset();
    retired = true;
  };
  fixture->editor.setFileOverlay(QStringLiteral("main.typ"),
                                 shortSource(QStringLiteral("Reply has no receiver")));
  QVERIFY(waitUntil([&] { return retired; }));
  QVERIFY(receiver.isNull());
  delivery->removeEventFilter(&observer);
  EditorFixture recovered(m_runtime, root.path(),
                          shortSource(QStringLiteral("Receiver recovered")));
  QVERIFY2(recovered.ready(), qPrintable(recovered.editor.compileMessage()));
  QVERIFY(waitUntil([&] { return recovered.editor.pdfPageCount() == 1; }));
  QCOMPARE(recovered.selectPage().trimmed(), QStringLiteral("Receiver recovered"));
}

void TestTypstEditor::compilerCompletionCanDestroyReceiver() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  auto compiler = std::make_unique<TypstCompiler>(*editorConfig(root.path(), &m_runtime));
  QPointer<TypstCompiler> receiver(compiler.get());
  int completions = 0;
  TypstCompiler::Result result;
  connect(compiler.get(), &TypstCompiler::finished, this,
          [&](const TypstCompiler::Result &p_result) {
            result = p_result;
            ++completions;
            compiler.reset();
          });
  QVERIFY(compiler->compileToSvg(shortSource(QStringLiteral("Delete on result"))));
  QVERIFY(waitUntil([&] { return receiver.isNull(); }));
  QCOMPARE(completions, 1);
  QVERIFY2(result.isSuccess(), qPrintable(result.m_error));
  QCOMPARE(result.m_svgPages.size(), 1);
  QSvgRenderer renderer(result.m_svgPages.first());
  QVERIFY(renderer.isValid());
}

void TestTypstEditor::mathFragmentsRenderWithBaseline_data() {
  QTest::addColumn<bool>("sensitive");
  QTest::newRow("ordinary") << false;
  QTest::newRow("sensitive") << true;
}

void TestTypstEditor::mathFragmentsRenderWithBaseline() {
  QFETCH(bool, sensitive);
  TypstFragmentRenderer renderer(&m_runtime);
  QSignalSpy completed(&renderer, &TypstFragmentRenderer::finished);
  QVERIFY(completed.isValid());
  // QtSvg does not support Typst's CSS Color 4 eight-digit hex alpha paints.
  // Alpha-channel rendering is exercised by the Markdown WebEngine tests.
  const QColor foreground(31, 83, 157);
  const QStringList sources = {QStringLiteral("frac(a^2 + b^2, sqrt(c))"),
                               QStringLiteral("sum_(i=1)^n i"), QStringLiteral("hat(x) + vec(y)")};
  for (const auto &source : sources) {
    QVector<QSizeF> sizes;
    QVector<QImage> images;
    for (const bool display : {false, true}) {
      const int previous = completed.count();
      QVERIFY(renderer.renderMathToSvg(source, display, foreground, sensitive));
      QVERIFY(renderer.isRendering());
      QCOMPARE(completed.count(), previous);
      QVERIFY(waitUntil([&] { return completed.count() == previous + 1; }));
      QVERIFY(!renderer.isRendering());
      const auto result = qvariant_cast<TypstFragmentRenderer::Result>(completed.last().first());
      QVERIFY2(result.isSuccess(), qPrintable(result.m_error));
      QCOMPARE(result.m_pageCount, 1);
      QVERIFY(!result.m_svg.isEmpty());
      QVERIFY(result.m_svg.size() <= 8 * 1024 * 1024);
      QVERIFY(std::isfinite(result.m_sizePt.width()));
      QVERIFY(std::isfinite(result.m_sizePt.height()));
      QVERIFY(std::isfinite(result.m_baselinePt));
      QVERIFY(result.m_sizePt.width() > 2 && result.m_sizePt.width() < 150);
      QVERIFY(result.m_sizePt.height() > 2 && result.m_sizePt.height() < 100);
      QVERIFY(result.m_baselinePt > 1);
      QVERIFY(result.m_baselinePt < result.m_sizePt.height() - 1);
      QSvgRenderer svg(result.m_svg);
      QVERIFY(svg.isValid());
      QCOMPARE(svg.viewBoxF().topLeft(), QPointF());
      // The native exporter rounds SVG coordinates, unlike the JSON geometry.
      QVERIFY(std::abs(svg.viewBoxF().width() - result.m_sizePt.width()) < 0.002);
      QVERIFY(std::abs(svg.viewBoxF().height() - result.m_sizePt.height()) < 0.002);

      const auto image = mathFragmentImage(result);
      QVERIFY(!image.isNull());
      QVERIFY(pixelsWithColor(image, foreground) > 30);
      QRect ink;
      int painted = 0;
      int longestStroke = 0;
      for (int y = 0; y < image.height(); ++y) {
        int run = 0;
        for (int x = 0; x < image.width(); ++x) {
          const int alpha = image.pixelColor(x, y).alpha();
          if (alpha) {
            ink |= QRect(x, y, 1, 1);
            ++painted;
          }
          run = alpha > 32 ? run + 1 : 0;
          longestStroke = std::max(longestStroke, run);
        }
      }
      QVERIFY(painted > 100);
      // At four pixels/point, even antialiased strokes and accents must leave
      // at least half of the native 1pt padding transparent on all four sides.
      QVERIFY2(ink.left() >= 2 && ink.top() >= 2 && ink.right() < image.width() - 2 &&
                   ink.bottom() < image.height() - 2,
               qPrintable(source));
      QVERIFY(ink.width() > image.width() / 4);
      QVERIFY(ink.height() > image.height() / 2);
      if (source == sources.first())
        QVERIFY(longestStroke > image.width() / 4); // Fraction/radical rules survive rasterization.
      sizes.append(result.m_sizePt);
      images.append(image);
    }
    if (source == sources.at(1)) {
      QVERIFY(sizes.at(1).height() > sizes.at(0).height());
      QVERIFY(images.at(1) != images.at(0)); // Display limits, not merely outer block placement.
    }
  }

  // The facade normalizes both newline forms without changing math geometry.
  QVector<TypstFragmentRenderer::Result> normalized;
  for (const auto &source :
       {QStringLiteral("frac(\na^2,\nb\n)"), QStringLiteral("frac(\r\na^2,\rb\r\n)")}) {
    const int previous = completed.count();
    QVERIFY(renderer.renderMathToSvg(source, false, foreground, sensitive));
    QVERIFY(waitUntil([&] { return completed.count() == previous + 1; }));
    const auto result = qvariant_cast<TypstFragmentRenderer::Result>(completed.last().first());
    QVERIFY2(result.isSuccess(), qPrintable(result.m_error));
    normalized.append(result);
  }
  QCOMPARE(normalized.at(0).m_sizePt, normalized.at(1).m_sizePt);
  QCOMPARE(normalized.at(0).m_baselinePt, normalized.at(1).m_baselinePt);
  QCOMPARE(mathFragmentImage(normalized.at(0)), mathFragmentImage(normalized.at(1)));
  QCoreApplication::sendPostedEvents();
  QCOMPARE(completed.count(), sources.size() * 2 + 2);
}

void TestTypstEditor::mathFragmentsFailClosed_data() {
  QTest::addColumn<bool>("sensitive");
  QTest::newRow("ordinary") << false;
  QTest::newRow("sensitive") << true;
}

void TestTypstEditor::mathFragmentsFailClosed() {
  QFETCH(bool, sensitive);
  QTemporaryDir root;
  QVERIFY(root.isValid());
  const auto textPath = root.filePath(QStringLiteral("private.txt"));
  const auto importPath = root.filePath(QStringLiteral("private.typ"));
  QVERIFY(writeFixture(textPath, QByteArrayLiteral("This file must not be readable")));
  QVERIFY(writeFixture(importPath, QByteArrayLiteral("#let value = $sqrt(x)$\n")));
  TypstFragmentRenderer renderer(&m_runtime);
  QSignalSpy completed(&renderer, &TypstFragmentRenderer::finished);
  const QColor foreground(31, 83, 157, 255);
  const QStringList invalid = {
      QString(),
      QStringLiteral(" \r\n\t"),
      QStringLiteral("x") + QChar(0) + QStringLiteral("y"),
      QString(64 * 1024 + 1, QLatin1Char('x')),
      QString(32 * 1024 + 1, QChar(0x00e9)), // UTF-8 bytes, not UTF-16 code units.
      QString(1, QChar(0xd800)),
      QStringLiteral("frac("),
      QStringLiteral("\\frac{a}{b}"),
      QStringLiteral("#read(\"%1\")").arg(textPath),
      QStringLiteral("#import \"%1\": *\n#value").arg(importPath),
      QStringLiteral("#import \"@preview/mitex:0.2.4\": *"),
      QStringLiteral("#link(\"https://example.com\")[$x$]"),
      QStringLiteral("#context counter(page).display()"),
      QStringLiteral("#box(width: 16384pt)[$x$]")};
  for (int index = 0; index <= invalid.size(); ++index) {
    const int previous = completed.count();
    // The last case exercises invalid QColor without changing source validity.
    const auto source = index < invalid.size() ? invalid.at(index) : QStringLiteral("sqrt(x)");
    const auto color = index < invalid.size() ? foreground : QColor();
    QVERIFY(renderer.renderMathToSvg(source, false, color, sensitive));
    QVERIFY(renderer.isRendering());
    QCOMPARE(completed.count(), previous);
    QVERIFY(!renderer.renderMathToSvg(QStringLiteral("sqrt(y)"), false, foreground, !sensitive));
    QVERIFY(waitUntil([&] { return completed.count() == previous + 1; }));
    const auto failed = qvariant_cast<TypstFragmentRenderer::Result>(completed.last().first());
    QVERIFY2(!failed.isSuccess(),
             qPrintable(QStringLiteral("Accepted invalid case %1").arg(index)));
    QVERIFY(!failed.m_error.isEmpty());
    QVERIFY(failed.m_svg.isEmpty());
    QVERIFY(failed.m_sizePt.isEmpty());
    QCOMPARE(failed.m_baselinePt, qreal(0));
    QCOMPARE(failed.m_pageCount, 0);
    QVERIFY(!renderer.isRendering());

    QVERIFY(renderer.renderMathToSvg(QStringLiteral("sqrt(x)"), false, foreground, !sensitive));
    QCOMPARE(completed.count(), previous + 1);
    QVERIFY(waitUntil([&] { return completed.count() == previous + 2; }));
    const auto recovered = qvariant_cast<TypstFragmentRenderer::Result>(completed.last().first());
    QVERIFY2(recovered.isSuccess(), qPrintable(recovered.m_error));
    QVERIFY(pixelsWithColor(mathFragmentImage(recovered), foreground) > 30);
  }
  const auto boundary = QStringLiteral("sqrt(x)") + QString(64 * 1024 - 7, QLatin1Char(' '));
  QCOMPARE(boundary.toUtf8().size(), 64 * 1024);
  const int previous = completed.count();
  QVERIFY(renderer.renderMathToSvg(boundary, false, foreground, sensitive));
  QVERIFY(waitUntil([&] { return completed.count() == previous + 1; }));
  const auto result = qvariant_cast<TypstFragmentRenderer::Result>(completed.last().first());
  QVERIFY2(result.isSuccess(), qPrintable(result.m_error));
  QVERIFY(pixelsWithColor(mathFragmentImage(result), foreground) > 30);
  QCoreApplication::sendPostedEvents();
  QCOMPARE(completed.count(), previous + 1);
}

void TestTypstEditor::mathCancellationAndDeadlineRecover_data() {
  QTest::addColumn<bool>("sensitive");
  QTest::newRow("ordinary-to-sensitive") << false;
  QTest::newRow("sensitive-to-ordinary") << true;
}

void TestTypstEditor::mathCancellationAndDeadlineRecover() {
  QFETCH(bool, sensitive);
  TypstRuntime runtime;
  QVERIFY(runtime.isAvailable());
  TypstFragmentRenderer renderer(&runtime);
  QSignalSpy completed(&renderer, &TypstFragmentRenderer::finished);
  const QColor foreground(31, 83, 157, 255);
  const auto replacement = QStringLiteral("sqrt(x)");
  QVERIFY(renderer.renderMathToSvg(replacement, false, foreground));
  QVERIFY(waitUntil([&] { return completed.count() == 1; }));
  const auto expected = qvariant_cast<TypstFragmentRenderer::Result>(completed.first().first());
  QVERIFY2(expected.isSuccess(), qPrintable(expected.m_error));
  completed.clear();
  auto *deadline =
      renderer.findChild<QTimer *>(QStringLiteral("typst-deadline"), Qt::FindDirectChildrenOnly);
  QVERIFY(deadline);
  QVERIFY(deadline->isSingleShot());
  QCOMPARE(deadline->interval(), 30000);
  {
    RuntimeLaneGate gate(runtime); // Both native compile lanes are occupied.
    QVERIFY(gate.entered());
    QVERIFY(renderer.renderMathToSvg(QStringLiteral("sum_(i=1)^n i"), true, foreground, sensitive));
    QVERIFY(!renderer.renderMathToSvg(replacement, false, foreground, !sensitive));
    QCoreApplication::processEvents();
    QCOMPARE(completed.count(), 0);
    QVERIFY(renderer.isRendering());
    renderer.cancel();
    renderer.cancel();
    QCOMPARE(completed.count(), 0);
    QVERIFY(waitUntil([&] { return completed.count() == 1; }));
    const auto cancelled = qvariant_cast<TypstFragmentRenderer::Result>(completed.last().first());
    QVERIFY(!cancelled.isSuccess());
    QVERIFY(!cancelled.m_error.isEmpty());
    QVERIFY(cancelled.m_svg.isEmpty());
    QVERIFY(cancelled.m_sizePt.isEmpty());
    QCOMPARE(cancelled.m_baselinePt, qreal(0));
    QVERIFY(!renderer.isRendering());

    // A replacement has its own queue-inclusive deadline while both lanes are
    // still held. No slow formula, native mock, or wall-clock race is needed.
    QVERIFY(renderer.renderMathToSvg(replacement, false, foreground, !sensitive));
    QCoreApplication::processEvents();
    QCOMPARE(completed.count(), 1);
    QVERIFY(deadline->isActive());
    QCOMPARE(deadline->interval(), 30000);
    deadline->setInterval(0);
    QVERIFY(waitUntil([&] { return completed.count() == 2; }));
    const auto timedOut = qvariant_cast<TypstFragmentRenderer::Result>(completed.last().first());
    QVERIFY(!timedOut.isSuccess());
    QVERIFY(!timedOut.m_error.isEmpty());
    QVERIFY(timedOut.m_svg.isEmpty());
    QVERIFY(timedOut.m_sizePt.isEmpty());
    QCOMPARE(timedOut.m_baselinePt, qreal(0));
    QVERIFY(!renderer.isRendering());

    QVERIFY(renderer.renderMathToSvg(replacement, false, foreground, sensitive));
    QCOMPARE(deadline->interval(), 30000);
    QVERIFY(!renderer.renderMathToSvg(QStringLiteral("sqrt(y)"), true, foreground, !sensitive));
    gate.release();
    QVERIFY(waitUntil([&] { return completed.count() == 3; }));
    const auto recovered = qvariant_cast<TypstFragmentRenderer::Result>(completed.last().first());
    QVERIFY2(recovered.isSuccess(), qPrintable(recovered.m_error));
    QCOMPARE(recovered.m_sizePt, expected.m_sizePt);
    QCOMPARE(recovered.m_baselinePt, expected.m_baselinePt);
    QCOMPARE(mathFragmentImage(recovered), mathFragmentImage(expected));
    QVERIFY(!renderer.isRendering());
    renderer.cancel();
    QCoreApplication::sendPostedEvents();
    QCOMPARE(completed.count(), 3); // Busy submissions and idle cancel emit nothing.
  }

  completed.clear();
  auto *delivery = runtime.findChild<QObject *>(QStringLiteral("typst-delivery"));
  QVERIFY(delivery);
  NativeDeliveryObserver observer;
  bool intercepted = false;
  bool cancelWasAsynchronous = false;
  bool replacementBeforeNativeDelivery = false;
  bool idleBeforeSignal = false;
  bool replacementAccepted = false;
  bool busyAccepted = true;
  bool replaceOnCancellation = true;
  const auto connection = connect(
      &renderer, &TypstFragmentRenderer::finished, &renderer,
      [&](const TypstFragmentRenderer::Result &p_result) {
        if (!replaceOnCancellation)
          return;
        replaceOnCancellation = false;
        idleBeforeSignal = !renderer.isRendering() && !p_result.isSuccess();
        // Capture a different policy while the retired native call still
        // owns its request; a rejected busy call must change neither one.
        replacementAccepted = renderer.renderMathToSvg(replacement, false, foreground, !sensitive);
        busyAccepted =
            renderer.renderMathToSvg(QStringLiteral("sqrt(y)"), false, foreground, sensitive);
      });
  observer.beforeDelivery = [&] {
    intercepted = true;
    // Native work has produced a real reply, but its provisional reply and
    // authoritative finished status have not reached the facade. Do not let
    // the nested logical completion pump any other native-delivery event.
    renderer.cancel();
    cancelWasAsynchronous = completed.isEmpty();
    QCoreApplication::sendPostedEvents(&renderer, QEvent::MetaCall);
    replacementBeforeNativeDelivery = completed.count() == 1 && replacementAccepted;
  };
  delivery->installEventFilter(&observer);
  QVERIFY(renderer.renderMathToSvg(QStringLiteral("frac(a^2 + b^2, sqrt(c))"), true, foreground,
                                   sensitive));
  QVERIFY(waitUntil([&] { return completed.count() == 2; }));
  QVERIFY(intercepted);
  QVERIFY(cancelWasAsynchronous);
  QVERIFY(replacementBeforeNativeDelivery);
  QVERIFY(idleBeforeSignal);
  QVERIFY(replacementAccepted);
  QVERIFY(!busyAccepted);
  const auto cancelled = qvariant_cast<TypstFragmentRenderer::Result>(completed.first().first());
  QVERIFY(!cancelled.isSuccess());
  QVERIFY(cancelled.m_svg.isEmpty());
  QVERIFY(cancelled.m_sizePt.isEmpty());
  QCOMPARE(cancelled.m_baselinePt, qreal(0));
  const auto recovered = qvariant_cast<TypstFragmentRenderer::Result>(completed.last().first());
  QVERIFY2(recovered.isSuccess(), qPrintable(recovered.m_error));
  QCOMPARE(recovered.m_sizePt, expected.m_sizePt);
  QCOMPARE(recovered.m_baselinePt, expected.m_baselinePt);
  QCOMPARE(mathFragmentImage(recovered), mathFragmentImage(expected));
  QVERIFY(!renderer.isRendering());
  delivery->removeEventFilter(&observer);
  disconnect(connection);
  QCoreApplication::sendPostedEvents();
  QCOMPARE(completed.count(), 2);
}

void TestTypstEditor::mathRendererLifetime_data() {
  QTest::addColumn<bool>("sensitive");
  QTest::addColumn<bool>("markup");
  QTest::newRow("math-ordinary") << false << false;
  QTest::newRow("math-sensitive") << true << false;
  QTest::newRow("markup-ordinary") << false << true;
  QTest::newRow("markup-sensitive") << true << true;
}

void TestTypstEditor::mathRendererLifetime() {
  QFETCH(bool, sensitive);
  QFETCH(bool, markup);
  const QColor foreground(31, 83, 157, 255);
  const auto source = markup ? QStringLiteral("= Heading\n$frac(a^2 + b^2, sqrt(c))$")
                             : QStringLiteral("frac(a^2 + b^2, sqrt(c))");
  const auto submit = [&](TypstFragmentRenderer &p_renderer, const QString &p_source,
                          bool p_sensitive) {
    return markup ? p_renderer.renderMarkupToSvg(p_source, foreground, p_sensitive)
                  : p_renderer.renderMathToSvg(p_source, false, foreground, p_sensitive);
  };
  {
    TypstFragmentRenderer missing(nullptr);
    QSignalSpy completed(&missing, &TypstFragmentRenderer::finished);
    QVERIFY(submit(missing, source, sensitive));
    QCOMPARE(completed.count(), 0);
    QVERIFY(!submit(missing, source, !sensitive));
    QVERIFY(waitUntil([&] { return completed.count() == 1; }));
    const auto result = qvariant_cast<TypstFragmentRenderer::Result>(completed.first().first());
    QVERIFY(!result.isSuccess());
    QVERIFY(!result.m_error.isEmpty());
    QVERIFY(result.m_svg.isEmpty());
    QVERIFY(result.m_sizePt.isEmpty());
    QCOMPARE(result.m_baselinePt, qreal(0));
    QVERIFY(!missing.isRendering());
    missing.cancel();
    QCoreApplication::sendPostedEvents();
    QCOMPARE(completed.count(), 1);
  }

  TypstFragmentRenderer::Result retained;
  {
    TypstRuntime runtime; // Borrowed runtime outlives the facade, including signal deletion.
    auto renderer = std::make_unique<TypstFragmentRenderer>(&runtime);
    QPointer<TypstFragmentRenderer> receiver(renderer.get());
    int completions = 0;
    bool idleBeforeSignal = false;
    connect(renderer.get(), &TypstFragmentRenderer::finished, this,
            [&](const TypstFragmentRenderer::Result &p_result) {
              retained = p_result;
              ++completions;
              idleBeforeSignal = !renderer->isRendering();
              renderer.reset();
            });
    QVERIFY(submit(*renderer, source, sensitive));
    QVERIFY(waitUntil([&] { return receiver.isNull(); }));
    QCOMPARE(completions, 1);
    QVERIFY(idleBeforeSignal);
    QVERIFY2(retained.isSuccess(), qPrintable(retained.m_error));
  }
  QVERIFY(pixelsWithColor(mathFragmentImage(retained), foreground) > 30);

  {
    int completions = 0;
    auto renderer = std::make_unique<TypstFragmentRenderer>(nullptr);
    connect(renderer.get(), &TypstFragmentRenderer::finished, this,
            [&](const TypstFragmentRenderer::Result &) { ++completions; });
    QVERIFY(submit(*renderer, QString(), sensitive));
    renderer.reset(); // Even an already queued validation failure is suppressed.
    QCoreApplication::sendPostedEvents();
    QCOMPARE(completions, 0);
  }

  {
    auto renderer = std::make_unique<TypstFragmentRenderer>(nullptr);
    QPointer<TypstFragmentRenderer> receiver(renderer.get());
    TypstFragmentRenderer::Result failed;
    int completions = 0;
    bool idleBeforeSignal = false;
    connect(renderer.get(), &TypstFragmentRenderer::finished, this,
            [&](const TypstFragmentRenderer::Result &p_result) {
              failed = p_result;
              ++completions;
              idleBeforeSignal = !renderer->isRendering();
              renderer.reset();
            });
    QVERIFY(submit(*renderer, source, sensitive));
    QCOMPARE(completions, 0);
    QVERIFY(waitUntil([&] { return receiver.isNull(); }));
    QVERIFY(idleBeforeSignal);
    QCOMPARE(completions, 1);
    QVERIFY(!failed.isSuccess());
    QVERIFY(!failed.m_error.isEmpty());
    QVERIFY(failed.m_svg.isEmpty());
    QVERIFY(failed.m_sizePt.isEmpty());
    QCOMPARE(failed.m_baselinePt, qreal(0));
  }

  {
    TypstRuntime runtime;
    int completions = 0;
    auto renderer = std::make_unique<TypstFragmentRenderer>(&runtime);
    QPointer<TypstFragmentRenderer> receiver(renderer.get());
    connect(renderer.get(), &TypstFragmentRenderer::finished, this,
            [&](const TypstFragmentRenderer::Result &) { ++completions; });
    auto *delivery = runtime.findChild<QObject *>(QStringLiteral("typst-delivery"));
    QVERIFY(delivery);
    NativeDeliveryObserver observer;
    observer.beforeDelivery = [&] { renderer.reset(); };
    delivery->installEventFilter(&observer);
    QVERIFY(submit(*renderer, source, sensitive));
    QVERIFY(waitUntil([&] { return receiver.isNull(); }));
    QCOMPARE(completions, 0);
    delivery->removeEventFilter(&observer);
    TypstFragmentRenderer recovered(&runtime);
    QSignalSpy completed(&recovered, &TypstFragmentRenderer::finished);
    QVERIFY(recovered.renderMathToSvg(QStringLiteral("sqrt(x)"), false, foreground, !sensitive));
    QVERIFY(waitUntil([&] { return completed.count() == 1; }));
    const auto result = qvariant_cast<TypstFragmentRenderer::Result>(completed.first().first());
    QVERIFY2(result.isSuccess(), qPrintable(result.m_error));
    QCOMPARE(completions, 0);
  }

  {
    TypstRuntime runtime;
    int completions = 0;
    auto renderer = std::make_unique<TypstFragmentRenderer>(&runtime);
    connect(renderer.get(), &TypstFragmentRenderer::finished, this,
            [&](const TypstFragmentRenderer::Result &) { ++completions; });
    RuntimeLaneGate gate(runtime);
    QVERIFY(gate.entered());
    QVERIFY(submit(*renderer, source, sensitive));
    QCoreApplication::processEvents();
    renderer.reset(); // Destruction must not wait for either held compile lane.
    QCOMPARE(completions, 0);
    TypstFragmentRenderer recovered(&runtime);
    QSignalSpy completed(&recovered, &TypstFragmentRenderer::finished);
    QVERIFY(recovered.renderMathToSvg(QStringLiteral("sqrt(x)"), false, foreground, !sensitive));
    gate.release();
    QVERIFY(waitUntil([&] { return completed.count() == 1; }));
    const auto result = qvariant_cast<TypstFragmentRenderer::Result>(completed.first().first());
    QVERIFY2(result.isSuccess(), qPrintable(result.m_error));
    QCoreApplication::sendPostedEvents();
    QCOMPARE(completions, 0);
  }

  {
    TypstRuntime runtime;
    TypstFragmentRenderer renderer(&runtime);
    QSignalSpy completed(&renderer, &TypstFragmentRenderer::finished);
    RuntimeLaneGate gate(runtime);
    QVERIFY(gate.entered());
    QVERIFY(submit(renderer, source, sensitive));
    QCoreApplication::processEvents();
    QCOMPARE(completed.count(), 0);
    bool observedShutdown = false;
    std::thread release([&] {
      QElapsedTimer timeout;
      timeout.start();
      while (runtime.isAvailable() && timeout.elapsed() < 10000)
        std::this_thread::yield();
      observedShutdown = !runtime.isAvailable();
      gate.release();
    });
    runtime.shutdown();
    release.join();
    QVERIFY(observedShutdown);
    QVERIFY(!runtime.isAvailable());
    QVERIFY(waitUntil([&] { return completed.count() == 1; }));
    for (int request = 0; request < 2; ++request) {
      const auto result = qvariant_cast<TypstFragmentRenderer::Result>(completed.last().first());
      QVERIFY(!result.isSuccess());
      QVERIFY(!result.m_error.isEmpty());
      QVERIFY(result.m_svg.isEmpty());
      QVERIFY(result.m_sizePt.isEmpty());
      QCOMPARE(result.m_baselinePt, qreal(0));
      QVERIFY(!renderer.isRendering());
      if (request == 0) {
        QVERIFY(submit(renderer, source, sensitive));
        QCOMPARE(completed.count(), 1);
        QVERIFY(waitUntil([&] { return completed.count() == 2; }));
      }
    }
    TypstFragmentRenderer stopped(&runtime);
    QSignalSpy stoppedCompleted(&stopped, &TypstFragmentRenderer::finished);
    QVERIFY(submit(stopped, source, sensitive));
    QCOMPARE(stoppedCompleted.count(), 0);
    QVERIFY(waitUntil([&] { return stoppedCompleted.count() == 1; }));
    const auto stoppedResult =
        qvariant_cast<TypstFragmentRenderer::Result>(stoppedCompleted.first().first());
    QVERIFY(!stoppedResult.isSuccess());
    QVERIFY(!stoppedResult.m_error.isEmpty());
    QVERIFY(stoppedResult.m_svg.isEmpty());
    QVERIFY(stoppedResult.m_sizePt.isEmpty());
    QCOMPARE(stoppedResult.m_baselinePt, qreal(0));
    QVERIFY(!stopped.isRendering());
    runtime.shutdown();
    renderer.cancel();
    QCoreApplication::sendPostedEvents();
    QCOMPARE(completed.count(), 2);
    for (auto *lane : runtime.findChildren<QThread *>())
      QVERIFY(!lane->isRunning());
  }
}

void TestTypstEditor::markupFragmentsRenderDocument_data() {
  QTest::addColumn<bool>("sensitive");
  QTest::newRow("ordinary") << false;
  QTest::newRow("sensitive") << true;
}

void TestTypstEditor::markupFragmentsRenderDocument() {
  QFETCH(bool, sensitive);
  TypstFragmentRenderer renderer(&m_runtime);
  const QColor foreground(31, 83, 157);
  const QColor red(211, 37, 53);
  const QColor green(13, 173, 71);
  const auto render = [&](const QString &p_source) {
    QSignalSpy completed(&renderer, &TypstFragmentRenderer::finished);
    if (!renderer.renderMarkupToSvg(p_source, foreground, sensitive) ||
        !waitUntil([&] { return completed.count() == 1; })) {
      TypstFragmentRenderer::Result failed;
      failed.m_error = QStringLiteral("Markup request did not complete");
      return failed;
    }
    return qvariant_cast<TypstFragmentRenderer::Result>(completed.first().first());
  };

  const auto source =
      QStringLiteral("#set page(width: 180pt, height: 180pt, margin: 8pt)\n"
                     "#show strong: it => text(fill: rgb(211, 37, 53), it)\n"
                     "= Heading\n*Styled* and _emphasized_ $sqrt(x)$\n"
                     "#let cell(n) = [#n]\n"
                     "#table(columns: (50pt, 50pt), [A], [B], cell(1), cell(2))\n"
                     "#context [Page #counter(page).display()]\n"
                     "#pagebreak()\n#set page(width: 100pt, height: 80pt)\n"
                     "#rect(width: 20pt, height: 12pt, fill: rgb(13, 173, 71), stroke: none)\n"
                     "#context [Page #counter(page).display()]");
  const auto document = render(source);
  QVERIFY2(document.isSuccess(), qPrintable(document.m_error));
  QCOMPARE(document.m_pageCount, 2);
  QCOMPARE(document.m_sizePt, QSizeF(180, 268));
  QCOMPARE(document.m_baselinePt, qreal(0));
  QSvgRenderer svg(document.m_svg);
  QVERIFY(svg.isValid());
  QCOMPARE(svg.viewBoxF(), QRectF(0, 0, 180, 268));
  auto image = mathFragmentImage(document);
  QCOMPARE(image.size(), QSize(720, 1072));
  const auto first = image.copy(0, 0, 720, 720);
  const auto second = image.copy(0, 752, 400, 320);
  QVERIFY(pixelsWithColor(first, foreground) > 100);
  QVERIFY(pixelsWithColor(first, red) > 100);
  QVERIFY(pixelsWithColor(second, foreground) > 100);
  QVERIFY(pixelsWithColor(second, green) > 2000);
  for (int y = 720; y < 752; ++y) {
    for (int x = 0; x < image.width(); ++x)
      QCOMPARE(image.pixelColor(x, y).alpha(), 0); // All eight gap points are transparent.
  }
  QCOMPARE(image.pixelColor(600, 850).alpha(), 0); // Narrow second page is not stretched.

  // Equivalent literal pages test real page counter convergence, not the
  // existence of an SVG or forwarding of markup as math.
  auto literal = source;
  const auto expression = QStringLiteral("#context [Page #counter(page).display()]");
  literal.replace(literal.indexOf(expression), expression.size(), QStringLiteral("Page 1"));
  literal.replace(literal.indexOf(expression), expression.size(), QStringLiteral("Page 2"));
  const auto literalDocument = render(literal);
  QVERIFY2(literalDocument.isSuccess(), qPrintable(literalDocument.m_error));
  QCOMPARE(mathFragmentImage(literalDocument), image);

  const auto small = render(QStringLiteral("#set text(size: 12pt)\nLetters"));
  const auto large = render(QStringLiteral("#set text(size: 24pt)\nLetters"));
  QVERIFY2(small.isSuccess(), qPrintable(small.m_error));
  QVERIFY2(large.isSuccess(), qPrintable(large.m_error));
  QVERIFY(large.m_sizePt.width() > small.m_sizePt.width() * 1.4);
  QVERIFY(pixelsWithColor(mathFragmentImage(large), foreground) >
          pixelsWithColor(mathFragmentImage(small), foreground) * 2);
  const auto authored = render(
      QStringLiteral("#set page(width: 90pt, height: 60pt, margin: 0pt, fill: rgb(17, 23, 29))\n"
                     "#set text(fill: rgb(211, 37, 53))\nAuthored"));
  QVERIFY2(authored.isSuccess(), qPrintable(authored.m_error));
  QCOMPARE(authored.m_sizePt, QSizeF(90, 60));
  image = mathFragmentImage(authored);
  QCOMPARE(image.pixelColor(1, 1), QColor(17, 23, 29));
  QVERIFY(pixelsWithColor(image, red) > 100);
  QCOMPARE(pixelsWithColor(image, foreground), 0);

  const auto linked = render(QStringLiteral("#link(\"https://example.com\")[Visible link]"));
  const auto plain = render(QStringLiteral("Visible link"));
  QVERIFY2(linked.isSuccess(), qPrintable(linked.m_error));
  QVERIFY2(plain.isSuccess(), qPrintable(plain.m_error));
  QVERIFY(linked.m_svg.contains("<a"));
  QCOMPARE(mathFragmentImage(linked), mathFragmentImage(plain));
  auto crlf = source;
  crlf.replace(QLatin1Char('\n'), QStringLiteral("\r\n"));
  const auto normalized = render(crlf);
  QVERIFY2(normalized.isSuccess(), qPrintable(normalized.m_error));
  QCOMPARE(mathFragmentImage(normalized), mathFragmentImage(document));
}

void TestTypstEditor::markupFragmentsFailClosed_data() {
  QTest::addColumn<bool>("sensitive");
  QTest::newRow("ordinary") << false;
  QTest::newRow("sensitive") << true;
}

void TestTypstEditor::markupFragmentsFailClosed() {
  QFETCH(bool, sensitive);
  QTemporaryDir root;
  QVERIFY(root.isValid());
  const auto privateFile = root.filePath(QStringLiteral("private.typ"));
  QVERIFY(writeFixture(privateFile, QByteArrayLiteral("#let secret = [private text]")));
  TypstFragmentRenderer renderer(&m_runtime);
  QSignalSpy completed(&renderer, &TypstFragmentRenderer::finished);
  const QColor foreground(31, 83, 157);
  const QStringList invalid = {
      QString(),
      QStringLiteral(" \r\n\t"),
      QStringLiteral("x") + QChar(0),
      QString(64 * 1024 + 1, QLatin1Char('x')),
      QString(32 * 1024 + 1, QChar(0x00e9)),
      QString(1, QChar(0xd800)),
      QStringLiteral("#let bad = ("),
      QStringLiteral("#read(\"%1\")").arg(privateFile),
      QStringLiteral("#include \"%1\"").arg(privateFile),
      QStringLiteral("#import \"%1\": *\n#secret").arg(privateFile),
      QStringLiteral("#import \"@preview/mitex:0.2.4\": *"),
      QStringLiteral("#read(\"fragment.typ\")"),
      QStringLiteral("#datetime.today()"),
      QStringLiteral(
          "#image(bytes(\"<svg xmlns='http://www.w3.org/2000/svg' width='8' height='8'><rect "
          "width='8' height='8' fill='red'/></svg>\"), format: \"svg\")"),
      QStringLiteral("#set page(width: 16385pt, height: 20pt, margin: 0pt)\nx"),
      QStringLiteral("#set page(width: 40pt, height: 9000pt, margin: 0pt)\nx #pagebreak() y"),
      QStringLiteral("#panic(\"x\" * 70000)")};
  for (int index = 0; index <= invalid.size(); ++index) {
    const int previous = completed.count();
    const auto source = index < invalid.size() ? invalid.at(index) : QStringLiteral("Valid text");
    QVERIFY(renderer.renderMarkupToSvg(source, index < invalid.size() ? foreground : QColor(),
                                       sensitive));
    QCOMPARE(completed.count(), previous);
    QVERIFY(!renderer.renderMathToSvg(QStringLiteral("sqrt(x)"), false, foreground, !sensitive));
    QVERIFY(waitUntil([&] { return completed.count() == previous + 1; }));
    const auto failed = qvariant_cast<TypstFragmentRenderer::Result>(completed.last().first());
    QVERIFY2(!failed.isSuccess(), qPrintable(QStringLiteral("Accepted markup case %1").arg(index)));
    QVERIFY(failed.m_svg.isEmpty());
    QVERIFY(failed.m_sizePt.isEmpty());
    QCOMPARE(failed.m_baselinePt, qreal(0));
    QCOMPARE(failed.m_pageCount, 0);
    QVERIFY(failed.m_error.toUtf8().size() < 64 * 1024);
    QVERIFY(renderer.renderMathToSvg(QStringLiteral("sqrt(x)"), false, foreground, !sensitive));
    QVERIFY(waitUntil([&] { return completed.count() == previous + 2; }));
    const auto recovered = qvariant_cast<TypstFragmentRenderer::Result>(completed.last().first());
    QVERIFY2(recovered.isSuccess(), qPrintable(recovered.m_error));
    QCOMPARE(recovered.m_pageCount, 1);
    QVERIFY(pixelsWithColor(mathFragmentImage(recovered), foreground) > 30);
  }
  const auto boundary = QStringLiteral("Text") + QString(64 * 1024 - 4, QLatin1Char(' '));
  const auto previous = completed.count();
  QVERIFY(renderer.renderMarkupToSvg(boundary, foreground, sensitive));
  QVERIFY(waitUntil([&] { return completed.count() == previous + 1; }));
  const auto result = qvariant_cast<TypstFragmentRenderer::Result>(completed.last().first());
  QVERIFY2(result.isSuccess(), qPrintable(result.m_error));
  QCOMPARE(result.m_pageCount, 1);
  QCOMPARE(result.m_baselinePt, qreal(0));
  QVERIFY(pixelsWithColor(mathFragmentImage(result), foreground) > 30);
}

void TestTypstEditor::fragmentKindsCancelAndReplace_data() {
  QTest::addColumn<bool>("firstMarkup");
  QTest::addColumn<bool>("sensitive");
  QTest::newRow("math-ordinary-to-markup-sensitive") << false << false;
  QTest::newRow("math-sensitive-to-markup-ordinary") << false << true;
  QTest::newRow("markup-ordinary-to-math-sensitive") << true << false;
  QTest::newRow("markup-sensitive-to-math-ordinary") << true << true;
}

void TestTypstEditor::fragmentKindsCancelAndReplace() {
  QFETCH(bool, firstMarkup);
  QFETCH(bool, sensitive);
  TypstRuntime runtime;
  QVERIFY(runtime.isAvailable());
  TypstFragmentRenderer renderer(&runtime);
  QSignalSpy completed(&renderer, &TypstFragmentRenderer::finished);
  const QColor foreground(31, 83, 157);
  const auto submit = [&](bool p_markup, bool p_sensitive) {
    return p_markup ? renderer.renderMarkupToSvg(
                          QStringLiteral(
                              "#set page(width: 80pt, height: 48pt, margin: 8pt)\n*Replacement*"),
                          foreground, p_sensitive)
                    : renderer.renderMathToSvg(QStringLiteral("frac(a^2, sqrt(b))"), false,
                                               foreground, p_sensitive);
  };
  QVERIFY(submit(!firstMarkup, !sensitive));
  QVERIFY(waitUntil([&] { return completed.count() == 1; }));
  const auto expected = qvariant_cast<TypstFragmentRenderer::Result>(completed.last().first());
  QVERIFY2(expected.isSuccess(), qPrintable(expected.m_error));
  QVERIFY(pixelsWithColor(mathFragmentImage(expected), foreground) > 30);
  completed.clear();
  auto *deadline = renderer.findChild<QTimer *>(QStringLiteral("typst-deadline"));
  QVERIFY(deadline);
  {
    RuntimeLaneGate gate(runtime);
    QVERIFY(gate.entered());
    QVERIFY(submit(firstMarkup, sensitive));
    QVERIFY(!submit(!firstMarkup, !sensitive));
    renderer.cancel();
    QCOMPARE(completed.count(), 0);
    QVERIFY(waitUntil([&] { return completed.count() == 1; }));
    const auto cancelled = qvariant_cast<TypstFragmentRenderer::Result>(completed.last().first());
    QVERIFY(!cancelled.isSuccess());
    QVERIFY(cancelled.m_svg.isEmpty());
    QCOMPARE(cancelled.m_pageCount, 0);
    QVERIFY(submit(!firstMarkup, !sensitive));
    QCOMPARE(deadline->interval(), 30000);
    deadline->setInterval(0);
    QVERIFY(waitUntil([&] { return completed.count() == 2; }));
    const auto expired = qvariant_cast<TypstFragmentRenderer::Result>(completed.last().first());
    QVERIFY(!expired.isSuccess());
    QVERIFY(expired.m_svg.isEmpty());
    QCOMPARE(expired.m_pageCount, 0);
    QVERIFY(submit(!firstMarkup, !sensitive));
    QVERIFY(!submit(firstMarkup, sensitive));
    QCOMPARE(deadline->interval(), 30000);
    gate.release();
    QVERIFY(waitUntil([&] { return completed.count() == 3; }));
    const auto recovered = qvariant_cast<TypstFragmentRenderer::Result>(completed.last().first());
    QVERIFY2(recovered.isSuccess(), qPrintable(recovered.m_error));
    QCOMPARE(recovered.m_pageCount, expected.m_pageCount);
    QCOMPARE(recovered.m_baselinePt, expected.m_baselinePt);
    QCOMPARE(recovered.m_sizePt, expected.m_sizePt);
    QCOMPARE(mathFragmentImage(recovered), mathFragmentImage(expected));
  }

  completed.clear();
  auto *delivery = runtime.findChild<QObject *>(QStringLiteral("typst-delivery"));
  QVERIFY(delivery);
  NativeDeliveryObserver observer;
  bool replacementAccepted = false;
  bool busyAccepted = true;
  bool idleAtCancellation = false;
  bool replacedBeforeDelivery = false;
  bool replace = true;
  const auto connection = connect(&renderer, &TypstFragmentRenderer::finished, &renderer,
                                  [&](const TypstFragmentRenderer::Result &p_result) {
                                    if (!replace)
                                      return;
                                    replace = false;
                                    idleAtCancellation =
                                        !renderer.isRendering() && !p_result.isSuccess();
                                    replacementAccepted = submit(!firstMarkup, !sensitive);
                                    busyAccepted = submit(firstMarkup, sensitive);
                                  });
  observer.beforeDelivery = [&] {
    renderer.cancel();
    QCoreApplication::sendPostedEvents(&renderer, QEvent::MetaCall);
    replacedBeforeDelivery = completed.count() == 1 && replacementAccepted;
  };
  delivery->installEventFilter(&observer);
  QVERIFY(submit(firstMarkup, sensitive));
  QVERIFY(waitUntil([&] { return completed.count() == 2; }));
  QVERIFY(idleAtCancellation);
  QVERIFY(replacedBeforeDelivery);
  QVERIFY(replacementAccepted);
  QVERIFY(!busyAccepted);
  const auto recovered = qvariant_cast<TypstFragmentRenderer::Result>(completed.last().first());
  QVERIFY2(recovered.isSuccess(), qPrintable(recovered.m_error));
  QCOMPARE(recovered.m_pageCount, expected.m_pageCount);
  QCOMPARE(recovered.m_baselinePt, expected.m_baselinePt);
  QCOMPARE(recovered.m_sizePt, expected.m_sizePt);
  QCOMPARE(mathFragmentImage(recovered), mathFragmentImage(expected));
  delivery->removeEventFilter(&observer);
  disconnect(connection);
  renderer.cancel();
  QCoreApplication::sendPostedEvents();
  QCOMPARE(completed.count(), 2);
}

void TestTypstEditor::runtimeShutdownCompletesAdmittedExport() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  TypstRuntime runtime;
  TypstCompiler compiler(*editorConfig(root.path(), &runtime));
  QSignalSpy completed(&compiler, &TypstCompiler::finished);
  RuntimeLaneGate gate(runtime);
  QVERIFY(gate.entered());
  const auto source = shortSource(QStringLiteral("Cancelled queued export"));
  QVERIFY(compiler.compileToSvg(source));
  QCoreApplication::processEvents(); // Dispatch initialization behind the held lane.
  QCOMPARE(completed.count(), 0);
  QVERIFY(compiler.isCompiling());
  std::thread release([&] {
    QElapsedTimer timeout;
    timeout.start();
    while (runtime.isAvailable() && timeout.elapsed() < 10000)
      std::this_thread::yield();
    gate.release();
  });
  runtime.shutdown();
  release.join();
  QVERIFY(!runtime.isAvailable());
  QVERIFY(waitUntil([&] { return completed.count() == 1; }));
  auto result = qvariant_cast<TypstCompiler::Result>(completed.first().first());
  QVERIFY(!result.isSuccess());
  QVERIFY(!result.m_error.isEmpty());
  QVERIFY(result.m_svgPages.isEmpty());
  QVERIFY(!compiler.isCompiling());
  QVERIFY(compiler.compileToBitmap(source));
  QVERIFY(waitUntil([&] { return completed.count() == 2; }));
  result = qvariant_cast<TypstCompiler::Result>(completed.last().first());
  QVERIFY(!result.isSuccess());
  QVERIFY(!result.m_error.isEmpty());
  QVERIFY(result.m_bitmapPages.isEmpty());
  runtime.shutdown();
  QCoreApplication::sendPostedEvents();
  QCOMPARE(completed.count(), 2);
}

void TestTypstEditor::deadlineRetryKeepsDispatchIdentityUntilCompletion() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  TypstRuntime runtime;
  EditorFixture fixture(runtime, root.path(), shortSource(QStringLiteral("Deadline baseline")),
                        VTypstEditor::Mode::Read, {}, false, false);
  QSignalSpy published(fixture.pane(), SIGNAL(snapshotPublished(quint64)));
  QVERIFY2(waitUntil([&] { return published.count() == 1; }),
           qPrintable(fixture.editor.compileMessage()));
  auto *bridge = compilerBridge(&fixture.editor);
  QVERIFY(bridge);
  auto *deadline = bridge->findChild<QTimer *>(QStringLiteral("typst-deadline"));
  QVERIFY(deadline);
  auto *delivery = runtime.findChild<QObject *>(QStringLiteral("typst-delivery"));
  QVERIFY(delivery);
  QSignalSpy generations(bridge, SIGNAL(generationChanged(quint64, quint64)));
  QVERIFY(generations.isValid());
  bool deadlineDelivered = false;
  bool errorObserved = false;
  bool epochRetained = false;
  bool intercepted = false;
  NativeDeliveryObserver observer;
  observer.shouldIntercept = [&] {
    return deadline->isActive() && !generations.isEmpty() &&
           fixture.editor.compileStatus() == VTypstEditor::CompileStatus::Compiling;
  };
  observer.beforeDelivery = [&] {
    intercepted = true;
    const auto epoch = generations.last()[0].toULongLong();
    const int previous = generations.count();
    // Exercise the real deadline handler immediately before a queued native
    // reply is delivered. The ABI may have returned already, but lane completion
    // has not been acknowledged: retry must preserve that dispatch identity.
    deadline->stop();
    deadlineDelivered = QMetaObject::invokeMethod(deadline, "timeout", Qt::DirectConnection);
    errorObserved = fixture.editor.compileStatus() == VTypstEditor::CompileStatus::Error;
    fixture.editor.retryCompilation();
    fixture.editor.retryCompilation();
    fixture.editor.setFileOverlay(QStringLiteral("main.typ"),
                                  shortSource(QStringLiteral("Latest source after retry")));
    epochRetained = published.count() == 1;
    for (int index = previous; index < generations.count(); ++index)
      epochRetained = epochRetained && generations[index][0].toULongLong() == epoch;
  };
  delivery->installEventFilter(&observer);
  fixture.editor.setFileOverlay(QStringLiteral("main.typ"),
                                shortSource(QStringLiteral("Timed out source must not publish")));
  QVERIFY(waitUntil([&] { return intercepted; }));
  QVERIFY(deadlineDelivered);
  QVERIFY(errorObserved);
  QVERIFY(epochRetained);
  QVERIFY2(waitUntil([&] { return published.count() == 2; }),
           qPrintable(fixture.editor.compileMessage()));
  QCOMPARE(fixture.selectPage().trimmed(), QStringLiteral("Latest source after retry"));
  QVERIFY(fixture.editor.diagnostics().isEmpty());
  delivery->removeEventFilter(&observer);
}

void TestTypstEditor::compilerGeometryAndScale() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  auto config = *editorConfig(root.path(), &m_runtime);
  TypstCompiler compiler(config);
  QSignalSpy completed(&compiler, &TypstCompiler::finished);
  QVERIFY(completed.isValid());
  const auto source = QStringLiteral(
      "#set page(width: 120pt, height: 80pt, margin: 0pt, fill: white, bleed: 12pt)\n"
      "#place(top + left, dx: 10pt, dy: 10pt)["
      "#rect(width: 40pt, height: 20pt, fill: rgb(255, 0, 0), stroke: none)]\n"
      "#pagebreak()\n"
      "#place(top + left, dx: 60pt, dy: 40pt)["
      "#rect(width: 40pt, height: 20pt, fill: rgb(0, 0, 255), stroke: none)]");

  QVERIFY(!compiler.isCompiling());
  QVERIFY(compiler.compileToSvg(source));
  QCOMPARE(completed.count(), 0);
  QVERIFY(compiler.isCompiling());
  QVERIFY(waitUntil([&]() { return completed.count() == 1; }));
  QVERIFY(!compiler.isCompiling());
  const auto svg = qvariant_cast<TypstCompiler::Result>(completed.at(0).at(0));
  QVERIFY2(svg.isSuccess(), qPrintable(svg.m_error));
  QVERIFY(svg.m_bitmapPages.isEmpty());
  QCOMPARE(svg.m_svgPages.size(), 2);
  for (int page = 0; page < svg.m_svgPages.size(); ++page) {
    QSvgRenderer renderer(svg.m_svgPages.at(page));
    QVERIFY(renderer.isValid());
    QCOMPARE(renderer.viewBoxF(), QRectF(0, 0, 120, 80));
    QImage image(QSize(120, 80), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    renderer.render(&painter);
    painter.end();
    QCOMPARE(image.pixelColor(20, 20), page == 0 ? QColor(Qt::red) : QColor(Qt::white));
    QCOMPARE(image.pixelColor(70, 50), page == 1 ? QColor(Qt::blue) : QColor(Qt::white));
    QCOMPARE(image.pixelColor(5, 5), QColor(Qt::white));
  }

  for (const qreal scale : {qreal(1), qreal(2)}) {
    completed.clear();
    QVERIFY(scale == 1 ? compiler.compileToBitmap(source)
                       : compiler.compileToBitmap(source, scale));
    QCOMPARE(completed.count(), 0);
    QVERIFY(waitUntil([&]() { return completed.count() == 1; }));
    const auto bitmap = qvariant_cast<TypstCompiler::Result>(completed.at(0).at(0));
    QVERIFY2(bitmap.isSuccess(), qPrintable(bitmap.m_error));
    QVERIFY(bitmap.m_svgPages.isEmpty());
    QCOMPARE(bitmap.m_bitmapPages.size(), 2);
    for (int page = 0; page < bitmap.m_bitmapPages.size(); ++page) {
      const auto &image = bitmap.m_bitmapPages.at(page);
      QCOMPARE(image.size(), QSize(int(120 * scale), int(80 * scale)));
      QCOMPARE(image.pixelColor(int(20 * scale), int(20 * scale)),
               page == 0 ? QColor(Qt::red) : QColor(Qt::white));
      QCOMPARE(image.pixelColor(int(70 * scale), int(50 * scale)),
               page == 1 ? QColor(Qt::blue) : QColor(Qt::white));
      QCOMPARE(image.pixelColor(int(5 * scale), int(5 * scale)), QColor(Qt::white));
    }
    QVERIFY(!compiler.isCompiling());
  }
}

void TestTypstEditor::compilerBitmapOutlivesRuntime() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  QImage retained;
  {
    TypstRuntime runtime;
    {
      auto config = *editorConfig(root.path(), &runtime);
      TypstCompiler compiler(config);
      QSignalSpy completed(&compiler, &TypstCompiler::finished);
      QVERIFY(compiler.compileToBitmap(
          QStringLiteral("#set page(width: 80pt, height: 60pt, margin: 0pt)\n"
                         "#rect(width: 40pt, height: 30pt, fill: rgb(0, 255, 0), stroke: none)")));
      QVERIFY(waitUntil([&] { return completed.count() == 1; }));
      const auto result = qvariant_cast<TypstCompiler::Result>(completed.first().first());
      QVERIFY2(result.isSuccess(), qPrintable(result.m_error));
      QCOMPARE(result.m_bitmapPages.size(), 1);
      retained = result.m_bitmapPages.first();
    }
    runtime.shutdown();
  }
  QCOMPARE(retained.size(), QSize(80, 60));
  QCOMPARE(retained.pixelColor(10, 10), QColor(Qt::green));
  auto changed = retained;
  changed.setPixelColor(10, 10, QColor(Qt::red));
  QCOMPARE(changed.pixelColor(10, 10), QColor(Qt::red));
  QCOMPARE(retained.pixelColor(10, 10), QColor(Qt::green));
}

void TestTypstEditor::previewThemeDefaultsRespectAuthoredStyles() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  QImage authoredImage(20, 10, QImage::Format_RGB32);
  authoredImage.fill(Qt::white);
  authoredImage.setPixelColor(5, 5, Qt::black);
  QVERIFY(authoredImage.save(root.filePath(QStringLiteral("authored.png"))));
  NativeSession native(root.path());
  const QColor dark(30, 32, 38), light(224, 226, 230), red(239, 51, 68);
  native.config.insert(QStringLiteral("previewBackground"), QJsonArray{30, 32, 38, 255});
  native.config.insert(QStringLiteral("previewForeground"), QJsonArray{224, 226, 230, 255});
  QVERIFY2(native.start(), qPrintable(native.details()));
  const auto source =
      QStringLiteral("#set page(width: 240pt, height: 160pt, margin: 10pt)\n"
                     "#set text(size: 20pt)\nDefault text\n\n"
                     "#text(fill: rgb(\"#ef3344\"))[Local override]\n"
                     "#place(bottom + right)[#image(\"authored.png\", width: 20pt, height: 10pt)]");
  const auto raster = [&](const QString &p_source) {
    if (!native.compile(p_source))
      return QImage();
    const auto snapshot = native.reply.header.value(QStringLiteral("snapshot"));
    if (!native.request({{QStringLiteral("op"), QStringLiteral("raster")},
                         {QStringLiteral("snapshot"), snapshot},
                         {QStringLiteral("page"), 0},
                         {QStringLiteral("scale"), 1}}))
      return QImage();
    const auto &header = native.reply.header;
    if (header.value(QStringLiteral("op")) != QStringLiteral("raster"))
      return QImage();
    return QImage(reinterpret_cast<const uchar *>(native.reply.payload.constData()),
                  header.value(QStringLiteral("width")).toInt(),
                  header.value(QStringLiteral("height")).toInt(),
                  QImage::Format_RGBA8888_Premultiplied)
        .copy();
  };
  auto image = raster(source);
  QVERIFY2(!image.isNull(), qPrintable(native.details()));
  QCOMPARE(image.pixelColor(0, 0), dark);
  QVERIFY(pixelsWithColor(image, light) > 30);
  QVERIFY(pixelsWithColor(image, red) > 30);
  QCOMPARE(image.pixelColor(211, 141), QColor(Qt::white));
  QCOMPARE(image.pixelColor(215, 145), QColor(Qt::black));

  // Same session and source: new library defaults must invalidate memoized layout.
  native.config[QStringLiteral("previewBackground")] = QJsonArray{246, 244, 238, 255};
  native.config[QStringLiteral("previewForeground")] = QJsonArray{42, 48, 52, 255};
  image = raster(source);
  QCOMPARE(image.pixelColor(0, 0), QColor(246, 244, 238));
  QVERIFY(pixelsWithColor(image, QColor(42, 48, 52)) > 30);
  QVERIFY(pixelsWithColor(image, red) > 30);

  // Explicit black/white and transparent fills are authored styles, not theme defaults.
  image = raster(QStringLiteral("#set page(fill: white)\n#set text(fill: black)\n") + source);
  QCOMPARE(image.pixelColor(0, 0), QColor(Qt::white));
  QVERIFY(pixelsWithColor(image, QColor(Qt::black)) > 30);
  QCOMPARE(pixelsWithColor(image, QColor(42, 48, 52)), 0);
  image = raster(QStringLiteral("#set page(fill: black)\n#set text(fill: white)\n") + source);
  QCOMPARE(image.pixelColor(0, 0), QColor(Qt::black));
  QVERIFY(pixelsWithColor(image, QColor(Qt::white)) > 200);
  image = raster(QStringLiteral("#set page(fill: none)\n") + source);
  QCOMPARE(image.pixelColor(0, 0).alpha(), 0);
}

void TestTypstEditor::previewThemeSwitchAndOptOutPreserveSource() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  auto config = editorConfig(root.path(), &m_runtime);
  config->m_pdfBackground = QColor(30, 32, 38);
  config->m_previewForeground = QColor(224, 226, 230);
  const auto setBackground = [&config](int p_step) {
    const QJsonObject base =
        p_step == 5 ? QJsonObject() : QJsonObject{{QStringLiteral("background-color"), "#dce7eb"}};
    const QJsonObject override = p_step >= 4 ? QJsonObject()
                                             : QJsonObject{{QStringLiteral("background-color"),
                                                            p_step == 1 ? "#f6f4ee" : "#30343c"}};
    config->m_textEditorConfig->m_theme = textTheme(base, override);
  };
  setBackground(0);
  const QColor backgrounds[] = {QColor("#30343c"), QColor("#f6f4ee"), QColor(Qt::white),
                                QColor("#30343c"), QColor("#dce7eb"), QColor(Qt::white)};
  const auto source = QStringLiteral("#set page(width: 320pt, height: 300pt, margin: 20pt)\n"
                                     "#set text(size: 24pt)\nTheme text\n\n$ x^2 + y^2 $\n\n"
                                     "#text(fill: rgb(\"#ef3344\"))[Explicit red]");
  EditorFixture fixture(m_runtime, root.path(), source, VTypstEditor::Mode::Edit, config, true,
                        true);
  auto *pane = fixture.pane();
  QSignalSpy published(pane, SIGNAL(snapshotPublished(quint64)));
  QVERIFY(waitUntil([&]() { return published.count() == 1; }));
  auto *document = fixture.editor.document();
  document->setModified(false);
  const int undoSteps = document->availableUndoSteps();
  for (int step = 0; step < 6; ++step) {
    if (step) {
      config = QSharedPointer<TypstEditorConfig>::create(*config);
      setBackground(step);
      config->m_enableThemeStyleInPreview = step != 2;
      config->m_pdfBackground = step == 1 ? QColor(246, 244, 238) : QColor(30, 32, 38);
      config->m_previewForeground =
          step == 1 || step >= 4 ? QColor(42, 48, 52) : QColor(224, 226, 230);
      const int count = published.count();
      fixture.editor.setConfig(config);
      QVERIFY(waitUntil([&]() { return published.count() > count; }));
    }
    const QColor background = backgrounds[step];
    const QColor foreground = step == 2 ? QColor(Qt::black) : config->m_previewForeground;
    QVERIFY(waitUntil(
        [&]() {
          const auto image = widgetImage(pane->viewport());
          return image.pixelColor(image.width() / 2, 24) == background &&
                 pixelsWithColor(image, foreground) > 30 &&
                 pixelsWithColor(image, QColor(239, 51, 68)) > 30;
        },
        paintTimeout));
    QCOMPARE(widgetImage(pane->viewport()).pixelColor(2, 2), config->m_pdfBackground);
    QVERIFY(waitUntil(
        [&]() {
          for (auto *card :
               fixture.editor.findChildren<QWidget *>(QStringLiteral("typstPreviewCard"))) {
            if (card->isVisible() && pixelsWithColor(widgetImage(card), foreground) > 10)
              return true;
          }
          return false;
        },
        paintTimeout));
    QCOMPARE(document->toPlainText(), source);
    QCOMPARE(document->availableUndoSteps(), undoSteps);
    QVERIFY(!document->isModified());
  }
  fixture.editor.setMode(VTypstEditor::Mode::Read);
  QVERIFY(waitUntil(
      [&]() {
        return pixelsWithColor(widgetImage(pane->viewport()), config->m_previewForeground) > 30;
      },
      paintTimeout));
}

void TestTypstEditor::exportsIgnorePreviewTheme() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  auto config = editorConfig(root.path(), &m_runtime);
  config->m_pdfBackground = QColor(30, 32, 38);
  config->m_previewForeground = QColor(224, 226, 230);
  config->m_enableThemeStyleInPreview = true;
  config->m_textEditorConfig->m_theme =
      textTheme({{QStringLiteral("background-color"), "#dce7eb"}},
                {{QStringLiteral("background-color"), "#30343c"}});
  TypstCompiler compiler(*config);
  TypstCompiler unthemed(*editorConfig(root.path(), &m_runtime));
  QSignalSpy finished(&compiler, &TypstCompiler::finished);
  QSignalSpy unthemedFinished(&unthemed, &TypstCompiler::finished);
  const auto source = QStringLiteral("#set page(width: 240pt, height: 120pt, margin: 10pt)\n"
                                     "#set text(size: 20pt)\nDefault text\n\n"
                                     "#text(fill: rgb(\"#ef3344\"))[Explicit red]");
  const auto renderedSvg = [](const QByteArray &p_svg) {
    QSvgRenderer renderer(p_svg);
    QImage image(QSize(240, 120), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    renderer.render(&painter);
    return image;
  };
  for (bool bitmap : {true, false}) {
    finished.clear();
    QVERIFY(bitmap ? compiler.compileToBitmap(source) : compiler.compileToSvg(source));
    QVERIFY(waitUntil([&]() { return finished.count() == 1; }));
    const auto result = qvariant_cast<TypstCompiler::Result>(finished.first().first());
    QVERIFY2(result.isSuccess(), qPrintable(result.m_error));
    QImage image;
    if (bitmap) {
      QCOMPARE(result.m_bitmapPages.size(), 1);
      image = result.m_bitmapPages.first();
    } else {
      QCOMPARE(result.m_svgPages.size(), 1);
      image = renderedSvg(result.m_svgPages.first());
    }
    QCOMPARE(image.pixelColor(0, 0), QColor(Qt::white));
    QVERIFY(pixelsWithColor(image, QColor(Qt::black)) > 30);
    QVERIFY(pixelsWithColor(image, QColor(239, 51, 68)) > 30);
    unthemedFinished.clear();
    QVERIFY(bitmap ? unthemed.compileToBitmap(source) : unthemed.compileToSvg(source));
    QVERIFY(waitUntil([&]() { return unthemedFinished.count() == 1; }));
    const auto original = qvariant_cast<TypstCompiler::Result>(unthemedFinished.first().first());
    QVERIFY2(original.isSuccess(), qPrintable(original.m_error));
    QCOMPARE(image,
             bitmap ? original.m_bitmapPages.first() : renderedSvg(original.m_svgPages.first()));
  }
}

void TestTypstEditor::textStyleOverridesMergeWithoutSharedMutation() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  const auto baseFamily = QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
  const auto otherFamily = QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
  const QJsonObject base{{QStringLiteral("font-family"), baseFamily},
                         {QStringLiteral("font-size"), 11},
                         {QStringLiteral("text-color"), "#223344"},
                         {QStringLiteral("background-color"), "#f0eee8"},
                         {QStringLiteral("selected-text-color"), "#fefefe"},
                         {QStringLiteral("selected-background-color"), "#445566"},
                         {QStringLiteral("bold"), true},
                         {QStringLiteral("italic"), true},
                         {QStringLiteral("underline"), true},
                         {QStringLiteral("strike-through"), true}};
  const QList<QJsonObject> overrides{
      {{QStringLiteral("font-size"), 19}},
      {{QStringLiteral("font-family"), otherFamily}},
      {{QStringLiteral("text-color"), "#993322"},
       {QStringLiteral("selected-background-color"), "#887766"},
       {QStringLiteral("bold"), false},
       {QStringLiteral("underline"), false}},
      {{QStringLiteral("background-color"), "#ddeeff"},
       {QStringLiteral("selected-text-color"), "#112233"},
       {QStringLiteral("italic"), false},
       {QStringLiteral("strike-through"), false}},
      {{QStringLiteral("font-family"), " ,  , "}, {QStringLiteral("font-size"), 0}},
      {}};
  const auto source = shortSource(QStringLiteral("#let label = \"Inherited style\"\n#label"));
  for (const auto &override : overrides) {
    auto config = editorConfig(root.path(), &m_runtime);
    config->m_textEditorConfig->m_theme = textTheme(base, override);
    EditorFixture fixture(m_runtime, root.path(), source, VTypstEditor::Mode::Edit, config, false,
                          true);
    QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
    auto *document = fixture.editor.document();
    const int position = source.indexOf(QStringLiteral("Inherited"));
    const auto verifySource = [&]() {
      const auto requestedFamily =
          override.value(QStringLiteral("font-family")).toString() == otherFamily ? otherFamily
                                                                                  : baseFamily;
      QCOMPARE(QFontInfo(fixture.edit()->font()).family(),
               QFontInfo(QFont(requestedFamily)).family());
      QCOMPARE(fixture.edit()->font().pointSize(),
               override.value(QStringLiteral("font-size")).toInt() > 0 ? 19 : 11);
      for (const auto &entry :
           {qMakePair(QPalette::Text, QStringLiteral("text-color")),
            qMakePair(QPalette::HighlightedText, QStringLiteral("selected-text-color")),
            qMakePair(QPalette::Highlight, QStringLiteral("selected-background-color"))}) {
        QCOMPARE(
            fixture.edit()->palette().color(entry.first),
            QColor(override.value(entry.second).toString(base.value(entry.second).toString())));
      }
      const auto format = syntaxFormatAt(document, position);
      QCOMPARE(QFontInfo(format.font()).family(), QFontInfo(fixture.edit()->font()).family());
      QCOMPARE(format.foreground().color(), fixture.edit()->palette().color(QPalette::Text));
      const QColor background(
          override.value(QStringLiteral("background-color"))
              .toString(base.value(QStringLiteral("background-color")).toString()));
      QCOMPARE(format.background().color(), background);
      const auto painted = widgetImage(fixture.edit()->viewport());
      QCOMPARE(painted.pixelColor(painted.width() / 2, painted.height() - 8), background);
      QCOMPARE(
          format.fontWeight(),
          int(override.value(QStringLiteral("bold")).toBool(true) ? QFont::Bold : QFont::Normal));
      QCOMPARE(format.fontItalic(), override.value(QStringLiteral("italic")).toBool(true));
      QCOMPARE(format.fontUnderline(), override.value(QStringLiteral("underline")).toBool(true));
      QCOMPARE(format.fontStrikeOut(),
               override.value(QStringLiteral("strike-through")).toBool(true));
    };
    verifySource();
    fixture.editor.setConfig(config);
    QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
    verifySource();

    // A later ordinary editor consumes the same input. Observe its live font and
    // palette, not the config pointer: the Typst specialization must stay private.
    VTextEditor ordinary(config->m_textEditorConfig,
                         QSharedPointer<TextEditorParameters>::create());
    ordinary.resize(600, 300);
    ordinary.show();
    QCOMPARE(QFontInfo(ordinary.getTextEdit()->font()).family(),
             QFontInfo(QFont(baseFamily)).family());
    QCOMPARE(ordinary.getTextEdit()->font().pointSize(), 11);
    QCOMPARE(ordinary.getTextEdit()->palette().color(QPalette::Text), QColor("#223344"));
    const auto painted = widgetImage(ordinary.getTextEdit()->viewport());
    QCOMPARE(painted.pixelColor(painted.width() / 2, painted.height() - 8), QColor("#f0eee8"));
    QCOMPARE(ordinary.getTextEdit()->palette().color(QPalette::HighlightedText), QColor("#fefefe"));
    QCOMPARE(ordinary.getTextEdit()->palette().color(QPalette::Highlight), QColor("#445566"));
  }

  // Omitting generic family/colors must restore Qt's inherited defaults too,
  // not the old override left on the widget by its generated stylesheet.
  auto config = editorConfig(root.path(), &m_runtime);
  const QJsonObject sparse{{QStringLiteral("font-size"), 11}};
  config->m_textEditorConfig->m_theme =
      textTheme(sparse, {{QStringLiteral("font-family"), otherFamily},
                         {QStringLiteral("text-color"), "#993322"},
                         {QStringLiteral("background-color"), "#ddeeff"},
                         {QStringLiteral("selected-text-color"), "#112233"},
                         {QStringLiteral("selected-background-color"), "#887766"}});
  EditorFixture changed(m_runtime, root.path(), source, VTypstEditor::Mode::Edit, config, false,
                        true);
  QVERIFY2(changed.ready(), qPrintable(changed.editor.compileMessage()));
  config->m_textEditorConfig->m_theme = textTheme(sparse, {});
  changed.editor.setConfig(config);
  EditorFixture fresh(m_runtime, root.path(), source, VTypstEditor::Mode::Edit, config, false,
                      true);
  QVERIFY2(changed.ready(), qPrintable(changed.editor.compileMessage()));
  QVERIFY2(fresh.ready(), qPrintable(fresh.editor.compileMessage()));
  QCOMPARE(changed.edit()->font(), fresh.edit()->font());
  for (const auto role :
       {QPalette::Text, QPalette::Base, QPalette::HighlightedText, QPalette::Highlight})
    QCOMPARE(changed.edit()->palette().color(role), fresh.edit()->palette().color(role));
  QCOMPARE(syntaxFormatAt(changed.editor.document(), source.indexOf(QStringLiteral("Inherited"))),
           syntaxFormatAt(fresh.editor.document(), source.indexOf(QStringLiteral("Inherited"))));
}

void TestTypstEditor::textFontDefaultsMatchAuthoredStyles_data() {
  QTest::addColumn<QJsonObject>("override");
  QTest::addColumn<QString>("authoredDefaults");
  QTest::addColumn<QString>("body");
  const QString words = QStringLiteral("Font defaults Wiim 0123");
  const QString font = QStringLiteral("#set text(font: \"DejaVu Sans Mono\")\n");
  const QString size = QStringLiteral("#set text(size: 19pt)\n");
  const QJsonObject both{{QStringLiteral("font-family"), "DejaVu Sans Mono"},
                         {QStringLiteral("font-size"), 19}};
  QTest::newRow("ordered-fallback")
      << QJsonObject{{QStringLiteral("font-family"),
                      " VTextEdit Missing Family , , DejaVu Sans Mono , "},
                     {QStringLiteral("font-size"), 19}}
      << QStringLiteral(
             "#set text(font: (\"VTextEdit Missing Family\", \"DejaVu Sans Mono\"), size: 19pt)\n")
      << words;
  QTest::newRow("family-only") << QJsonObject{{QStringLiteral("font-family"), "DejaVu Sans Mono"}}
                               << font << words;
  QTest::newRow("size-only") << QJsonObject{{QStringLiteral("font-size"), 19}} << size << words;
  QTest::newRow("omitted") << QJsonObject() << QString() << words;
  QTest::newRow("empty") << QJsonObject{{QStringLiteral("font-family"), " ,  "},
                                        {QStringLiteral("font-size"), 0}}
                         << QString() << words;
  QTest::newRow("authored-family")
      << both << size << QStringLiteral("#set text(font: \"Libertinus Serif\")\n") + words;
  QTest::newRow("authored-size") << both << font
                                 << QStringLiteral("#set text(size: 13pt)\n") + words;
  QTest::newRow("authored-set") << both << QString()
                                << QStringLiteral(
                                       "#set text(font: \"Libertinus Serif\", size: 13pt)\n") +
                                       words;
  QTest::newRow("authored-show")
      << both << QString()
      << QStringLiteral("#show text: set text(font: \"Libertinus Serif\", size: 13pt)\n") + words;
  QTest::newRow("authored-constructor")
      << both << QString()
      << QStringLiteral("#text(font: \"Libertinus Serif\", size: 13pt)[") + words +
             QLatin1Char(']');
}

void TestTypstEditor::textFontDefaultsMatchAuthoredStyles() {
  QFETCH(QJsonObject, override);
  QFETCH(QString, authoredDefaults);
  QFETCH(QString, body);
  QTemporaryDir root;
  QVERIFY(root.isValid());
  auto config = editorConfig(root.path(), &m_runtime);
  // Generic source fonts deliberately disagree with native Typst defaults.
  config->m_textEditorConfig->m_theme = textTheme(
      {{QStringLiteral("font-family"), "Arial"}, {QStringLiteral("font-size"), 43}}, override);
  config->m_pdfBackground = QColor("#202020");
  config->m_previewForeground = QColor("#eeeeee");
  const auto source = shortSource(body);
  const auto authored = authoredDefaults + source;
  TypstCompiler compiler(*config);
  TypstCompiler reference(*editorConfig(root.path(), &m_runtime));
  QSignalSpy finished(&compiler, &TypstCompiler::finished);
  QSignalSpy referenceFinished(&reference, &TypstCompiler::finished);
  const auto image = [](const TypstCompiler::Result &p_result, bool p_bitmap) {
    if (p_bitmap)
      return p_result.m_bitmapPages.size() == 1 ? p_result.m_bitmapPages.first() : QImage();
    if (p_result.m_svgPages.size() != 1)
      return QImage();
    QSvgRenderer renderer(p_result.m_svgPages.first());
    if (!renderer.isValid())
      return QImage();
    QImage rendered(QSize(480, 320), QImage::Format_ARGB32_Premultiplied);
    rendered.fill(Qt::transparent);
    QPainter painter(&rendered);
    renderer.render(&painter);
    return rendered;
  };
  for (bool bitmap : {true, false}) {
    finished.clear();
    referenceFinished.clear();
    QVERIFY(bitmap ? compiler.compileToBitmap(source) : compiler.compileToSvg(source));
    QVERIFY(bitmap ? reference.compileToBitmap(authored) : reference.compileToSvg(authored));
    QVERIFY(waitUntil([&]() { return finished.count() == 1 && referenceFinished.count() == 1; }));
    const auto actual = qvariant_cast<TypstCompiler::Result>(finished.first().first());
    const auto expected = qvariant_cast<TypstCompiler::Result>(referenceFinished.first().first());
    QVERIFY2(actual.isSuccess(), qPrintable(actual.m_error));
    QVERIFY2(expected.isSuccess(), qPrintable(expected.m_error));
    const auto rendered = image(actual, bitmap);
    QVERIFY(!rendered.isNull());
    QCOMPARE(rendered, image(expected, bitmap));
  }

  // Real preview PDF publication goes through a different bridge mode. The
  // color opt-out must not opt out of font defaults.
  config->m_enableThemeStyleInPreview = false;
  EditorFixture preview(m_runtime, root.path(), source, VTypstEditor::Mode::Read, config);
  EditorFixture oracle(m_runtime, root.path(), authored);
  const auto previewExports = observePdfExports(&preview.editor);
  const auto oracleExports = observePdfExports(&oracle.editor);
  QVERIFY(previewExports && oracleExports);
  QVERIFY(waitUntil([&]() { return !previewExports->isEmpty() && !oracleExports->isEmpty(); }));
  const auto rendered = exportedPdfImage(*previewExports);
  QVERIFY(!rendered.isNull());
  QCOMPARE(rendered, exportedPdfImage(*oracleExports));
}

void TestTypstEditor::textFontThemeSwitchAndZoomPreserveSource() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  const auto baseFamily = QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
  const QJsonObject base{{QStringLiteral("font-family"), baseFamily},
                         {QStringLiteral("font-size"), 11}};
  const QList<QJsonObject> overrides{
      {{QStringLiteral("font-family"), "DejaVu Sans Mono"}, {QStringLiteral("font-size"), 19}},
      {{QStringLiteral("font-size"), 23}},
      {},
      {}};
  const QStringList defaults{QStringLiteral("#set text(font: \"DejaVu Sans Mono\", size: 19pt)\n"),
                             QStringLiteral("#set text(size: 23pt)\n"), QString(), QString()};
  auto config = editorConfig(root.path(), &m_runtime);
  config->m_textEditorConfig->m_theme = textTheme(base, overrides.first());
  const auto original = shortSource(QStringLiteral("Font switch Wiim 0123"));
  EditorFixture fixture(m_runtime, root.path(), original, VTypstEditor::Mode::Edit, config, false,
                        true);
  const auto exports = observePdfExports(&fixture.editor);
  QVERIFY(exports);
  QVERIFY(waitUntil([&]() { return !exports->isEmpty(); }));
  auto *document = fixture.editor.document();
  document->setModified(false);
  QTextCursor cursor(document);
  cursor.movePosition(QTextCursor::End);
  cursor.insertText(QStringLiteral(" Edited."));
  const auto edited = document->toPlainText();
  cursor.setPosition(edited.size() - 1, QTextCursor::KeepAnchor);
  fixture.edit()->setTextCursor(cursor);
  const int undoSteps = document->availableUndoSteps();
  QVERIFY(undoSteps > 0);
  QVERIFY(document->isModified());
  QSignalSpy changes(document, &QTextDocument::contentsChange);
  QImage previous;
  for (int step = 0; step < overrides.size(); ++step) {
    config->m_textEditorConfig->m_theme = textTheme(base, overrides.at(step));
    config->m_enableThemeStyleInPreview = step % 2 == 0;
    fixture.editor.getTextEditor()->zoom(3);
    const int count = exports->count();
    fixture.editor.setConfig(config);
    QVERIFY(waitUntil([&]() { return exports->count() > count; }));
    QCOMPARE(fixture.edit()->font().pointSize(), step == 0 ? 22 : step == 1 ? 26 : 14);
    if (step > 0)
      QCOMPARE(fixture.edit()->font().family(), baseFamily);
    EditorFixture oracle(m_runtime, root.path(), defaults.at(step) + edited);
    const auto oracleExports = observePdfExports(&oracle.editor);
    QVERIFY(oracleExports);
    QVERIFY(waitUntil([&]() { return !oracleExports->isEmpty(); }));
    const auto rendered = exportedPdfImage(*exports);
    QVERIFY(!rendered.isNull());
    QCOMPARE(rendered, exportedPdfImage(*oracleExports));
    if (step == 1 || step == 2)
      QVERIFY(rendered != previous);
    if (step == 3)
      QCOMPARE(rendered, previous); // Toggling preview colors does not alter document fonts.
    previous = rendered;
    QCOMPARE(document->toPlainText(), edited);
    QCOMPARE(document->availableUndoSteps(), undoSteps);
    QCOMPARE(fixture.edit()->textCursor().anchor(), cursor.anchor());
    QCOMPARE(fixture.edit()->textCursor().position(), cursor.position());
    QVERIFY(document->isModified());
  }
  QCOMPARE(textChangeCount(changes), 0);
  fixture.editor.getTextEditor()->zoom(0);
  QCOMPARE(fixture.edit()->font().pointSize(), 11);
  fixture.edit()->undo();
  QCOMPARE(document->toPlainText(), original);
  fixture.edit()->redo();
  QCOMPARE(document->toPlainText(), edited);
}

void TestTypstEditor::compilerUnicodeImportsAndNormalization() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  const auto mainPath = root.filePath(QStringLiteral("main.typ"));
  const QByteArray diskMain("#let disk_source_must_not_be_compiled = (\r\n");
  QVERIFY(writeFixture(mainPath, diskMain));
  QVERIFY(writeFixture(root.filePath(QStringLiteral("chapter.typ")),
                       QStringLiteral("#let words = [H\u00e9llo \u03ba\u03cc\u03c3\u03bc\u03b5 "
                                      "\u041f\u0440\u0438\u0432\u0435\u0442]\n"
                                      "#let formula = [$ integral_0^1 x^2 dif x = 1/3 $]\n")
                           .toUtf8()));
  const auto source = QStringLiteral(
      "#set page(width: 300pt, height: 120pt, margin: 0pt, fill: white)\n"
      "#import \"chapter.typ\": words, formula\n"
      "#assert(not read(\"main.typ\").contains(\"\\r\"))\n"
      "#place(top + left, dx: 10pt, dy: 10pt)[#text(fill: rgb(255, 0, 0))[#words]]\n"
      "#place(top + left, dx: 10pt, dy: 65pt)[#text(fill: rgb(0, 0, 255))[#formula]]\n");
  auto mixedNewlines = source;
  mixedNewlines.replace(QStringLiteral("\n"), QStringLiteral("\r\n"));
  mixedNewlines.replace(QStringLiteral("formula\r\n"), QStringLiteral("formula\r"));
  QVERIFY(mixedNewlines.contains(QStringLiteral("\r\n")));
  QVERIFY(mixedNewlines.contains(QStringLiteral("formula\r#assert")));

  TypstCompiler compiler(*editorConfig(root.path(), &m_runtime));
  QSignalSpy completed(&compiler, &TypstCompiler::finished);
  QVERIFY(compiler.compileToSvg(mixedNewlines));
  QVERIFY(waitUntil([&]() { return completed.count() == 1; }));
  const auto svg = qvariant_cast<TypstCompiler::Result>(completed.at(0).at(0));
  QVERIFY2(svg.isSuccess(), qPrintable(svg.m_error));
  QCOMPARE(svg.m_svgPages.size(), 1);
  QVERIFY(svg.m_bitmapPages.isEmpty());
  QSvgRenderer renderer(svg.m_svgPages.first());
  QVERIFY(renderer.isValid());
  QCOMPARE(renderer.viewBoxF(), QRectF(0, 0, 300, 120));
  QImage rendered(QSize(300, 120), QImage::Format_ARGB32_Premultiplied);
  rendered.fill(Qt::transparent);
  QPainter painter(&rendered);
  renderer.render(&painter);
  painter.end();
  QRect wordsBounds, formulaBounds;
  QVERIFY(coloredPixels(rendered, true, &wordsBounds) > 40);
  QVERIFY(coloredPixels(rendered, false, &formulaBounds) > 40);
  QVERIFY(wordsBounds.bottom() < formulaBounds.top());

  QVector<QImage> normalizedPages;
  for (const auto &text : {source, mixedNewlines}) {
    completed.clear();
    QVERIFY(compiler.compileToBitmap(text));
    QVERIFY(waitUntil([&]() { return completed.count() == 1; }));
    const auto result = qvariant_cast<TypstCompiler::Result>(completed.at(0).at(0));
    QVERIFY2(result.isSuccess(), qPrintable(result.m_error));
    QVERIFY(result.m_svgPages.isEmpty());
    QCOMPARE(result.m_bitmapPages.size(), 1);
    const auto &image = result.m_bitmapPages.first();
    QCOMPARE(image.size(), QSize(300, 120));
    QVERIFY(coloredPixels(image, true, &wordsBounds) > 40);
    QVERIFY(coloredPixels(image, false, &formulaBounds) > 40);
    QVERIFY(wordsBounds.bottom() < formulaBounds.top());
    if (normalizedPages.isEmpty())
      normalizedPages = result.m_bitmapPages;
    else
      QCOMPARE(image, normalizedPages.first());
  }
  QFile mainFile(mainPath);
  QVERIFY(mainFile.open(QIODevice::ReadOnly));
  QCOMPARE(mainFile.readAll(), diskMain);
}

void TestTypstEditor::compilerAsyncFailureAndRecovery() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  TypstCompiler compiler(*editorConfig(root.path(), &m_runtime));
  QSignalSpy completed(&compiler, &TypstCompiler::finished);
  bool returnedFromSubmit = false;
  bool asynchronous = true;
  bool idleAtCompletion = true;
  bool resubmitted = false;
  int callbackCount = 0;
  connect(&compiler, &TypstCompiler::finished, &compiler, [&](const TypstCompiler::Result &) {
    asynchronous = asynchronous && returnedFromSubmit;
    idleAtCompletion = idleAtCompletion && !compiler.isCompiling();
    if (++callbackCount == 1)
      resubmitted = compiler.compileToSvg(shortSource(QStringLiteral("Recovered $x^2$")));
  });
  QVERIFY(compiler.compileToBitmap(QStringLiteral("#let broken = (")));
  QCOMPARE(completed.count(), 0);
  QVERIFY(compiler.isCompiling());
  QVERIFY(!compiler.compileToSvg(shortSource(QStringLiteral("Rejected SVG"))));
  QVERIFY(!compiler.compileToBitmap(shortSource(QStringLiteral("Rejected bitmap"))));
  QCOMPARE(completed.count(), 0);
  returnedFromSubmit = true;
  QVERIFY(waitUntil([&]() { return completed.count() == 2; }));
  QVERIFY(asynchronous);
  QVERIFY(idleAtCompletion);
  QVERIFY(resubmitted);
  QCOMPARE(callbackCount, 2);
  const auto failure = qvariant_cast<TypstCompiler::Result>(completed.at(0).at(0));
  QVERIFY(!failure.isSuccess());
  QVERIFY(!failure.m_error.isEmpty());
  QVERIFY(!failure.m_diagnostics.isEmpty());
  QVERIFY(!failure.m_diagnostics.join(QLatin1Char('\n')).trimmed().isEmpty());
  QVERIFY(failure.m_svgPages.isEmpty());
  QVERIFY(failure.m_bitmapPages.isEmpty());
  const auto recovered = qvariant_cast<TypstCompiler::Result>(completed.at(1).at(0));
  QVERIFY2(recovered.isSuccess(), qPrintable(recovered.m_error));
  QCOMPARE(recovered.m_svgPages.size(), 1);
  QVERIFY(recovered.m_bitmapPages.isEmpty());
  QVERIFY(!compiler.isCompiling());
  QTest::qWait(100);
  QCOMPARE(completed.count(), 2);
  QCOMPARE(callbackCount, 2);
}

void TestTypstEditor::compilerRejectsInvalidScaleAndPixelLimit_data() {
  QTest::addColumn<qreal>("scale");
  QTest::addColumn<int>("pixelLimit");
  QTest::newRow("nan") << std::numeric_limits<qreal>::quiet_NaN() << 4096;
  QTest::newRow("positive-infinity") << std::numeric_limits<qreal>::infinity() << 4096;
  QTest::newRow("negative-infinity") << -std::numeric_limits<qreal>::infinity() << 4096;
  QTest::newRow("zero") << qreal(0) << 4096;
  QTest::newRow("negative") << qreal(-1) << 4096;
  QTest::newRow("pixel-limit") << qreal(1) << 4095;
}

void TestTypstEditor::compilerRejectsInvalidScaleAndPixelLimit() {
  QFETCH(qreal, scale);
  QFETCH(int, pixelLimit);
  QTemporaryDir root;
  QVERIFY(root.isValid());
  auto config = *editorConfig(root.path(), &m_runtime);
  config.m_maxRasterPixels = pixelLimit;
  TypstCompiler compiler(config);
  QSignalSpy completed(&compiler, &TypstCompiler::finished);
  const auto source = QStringLiteral("#set page(width: 64pt, height: 64pt, margin: 0pt)\n"
                                     "#rect(width: 20pt, height: 20pt, fill: red)");
  QVERIFY(compiler.compileToBitmap(source, scale));
  QCOMPARE(completed.count(), 0);
  QVERIFY(compiler.isCompiling());
  QVERIFY(waitUntil([&]() { return completed.count() == 1; }));
  const auto failure = qvariant_cast<TypstCompiler::Result>(completed.at(0).at(0));
  QVERIFY(!failure.isSuccess());
  QVERIFY(!failure.m_error.isEmpty());
  QVERIFY(failure.m_svgPages.isEmpty());
  QVERIFY(failure.m_bitmapPages.isEmpty());
  QVERIFY(!compiler.isCompiling());
  // A valid smaller raster proves that rejection did not poison the instance.
  QVERIFY(compiler.compileToBitmap(source, 0.5));
  QVERIFY(waitUntil([&]() { return completed.count() == 2; }));
  const auto recovered = qvariant_cast<TypstCompiler::Result>(completed.at(1).at(0));
  QVERIFY2(recovered.isSuccess(), qPrintable(recovered.m_error));
  QCOMPARE(recovered.m_bitmapPages.size(), 1);
  QCOMPARE(recovered.m_bitmapPages.first().size(), QSize(32, 32));
  QVERIFY(recovered.m_svgPages.isEmpty());
  QTest::qWait(100);
  QCOMPARE(completed.count(), 2);
}

void TestTypstEditor::compilerResultBudgetClearsPartialPages_data() {
  QTest::addColumn<bool>("bitmap");
  QTest::newRow("svg") << false;
  QTest::newRow("bitmap") << true;
}

void TestTypstEditor::compilerResultBudgetClearsPartialPages() {
  QFETCH(bool, bitmap);
  QTemporaryDir root;
  QVERIFY(root.isValid());
  const auto page = QStringLiteral(
      "#for x in range(8) { for y in range(8) {\n"
      "  place(top + left, dx: (x * 24 + 10) * 1pt, dy: (y * 24 + 10) * 1pt,\n"
      "        rect(width: 12pt, height: 12pt, fill: rgb(255, 0, 0), stroke: none))\n"
      "}}\n");
  const auto singlePage =
      QStringLiteral("#set page(width: 256pt, height: 256pt, margin: 0pt, fill: white)\n") + page;
  const auto twoPages = singlePage + QStringLiteral("#pagebreak()\n") + page;
  const auto submit = [bitmap](TypstCompiler &p_compiler, const QString &p_source) {
    return bitmap ? p_compiler.compileToBitmap(p_source) : p_compiler.compileToSvg(p_source);
  };
  TypstCompiler baseline(*editorConfig(root.path(), &m_runtime));
  QSignalSpy baselineFinished(&baseline, &TypstCompiler::finished);
  QVERIFY(submit(baseline, twoPages));
  QVERIFY(waitUntil([&]() { return baselineFinished.count() == 1; }));
  const auto reference = qvariant_cast<TypstCompiler::Result>(baselineFinished.at(0).at(0));
  QVERIFY2(reference.isSuccess(), qPrintable(reference.m_error));
  QCOMPARE(bitmap ? reference.m_bitmapPages.size() : reference.m_svgPages.size(), 2);
  qint64 totalBytes = 0;
  qint64 largestPage = 0;
  for (int index = 0; index < 2; ++index) {
    const qint64 bytes = bitmap ? reference.m_bitmapPages.at(index).sizeInBytes()
                                : reference.m_svgPages.at(index).size();
    totalBytes += bytes;
    largestPage = qMax(largestPage, bytes);
  }
  auto config = *editorConfig(root.path(), &m_runtime);
  config.m_maxCacheBytes = largestPage + (totalBytes - largestPage) / 2;
  QVERIFY(config.m_maxCacheBytes > largestPage);
  QVERIFY(config.m_maxCacheBytes < totalBytes);
  QVERIFY(config.m_maxCacheBytes > twoPages.toUtf8().size());
  TypstCompiler limited(config);
  QSignalSpy completed(&limited, &TypstCompiler::finished);
  // Every individual page fits. Only retaining the whole result exceeds the cap.
  QVERIFY(submit(limited, singlePage));
  QVERIFY(waitUntil([&]() { return completed.count() == 1; }));
  const auto individual = qvariant_cast<TypstCompiler::Result>(completed.at(0).at(0));
  QVERIFY2(individual.isSuccess(), qPrintable(individual.m_error));
  QCOMPARE(bitmap ? individual.m_bitmapPages.size() : individual.m_svgPages.size(), 1);
  QVERIFY(submit(limited, twoPages));
  QVERIFY(waitUntil([&]() { return completed.count() == 2; }));
  const auto failure = qvariant_cast<TypstCompiler::Result>(completed.at(1).at(0));
  QVERIFY(!failure.isSuccess());
  QVERIFY(!failure.m_error.isEmpty());
  QVERIFY(failure.m_svgPages.isEmpty());
  QVERIFY(failure.m_bitmapPages.isEmpty());
  QVERIFY(!limited.isCompiling());
  QVERIFY(submit(limited, singlePage));
  QVERIFY(waitUntil([&]() { return completed.count() == 3; }));
  const auto recovered = qvariant_cast<TypstCompiler::Result>(completed.at(2).at(0));
  QVERIFY2(recovered.isSuccess(), qPrintable(recovered.m_error));
  QCOMPARE(bitmap ? recovered.m_bitmapPages.size() : recovered.m_svgPages.size(), 1);
}

void TestTypstEditor::overlaysDiagnosticsAndSnapshotRetention() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  const auto mainPath = root.filePath(QStringLiteral("main.typ"));
  const auto importPath = root.filePath(QStringLiteral("chapter.typ"));
  const QByteArray diskMain("Disk main must not be compiled.");
  const QByteArray diskImport("#let phrase = [Disk import must not win.]");
  QVERIFY(writeFixture(mainPath, diskMain));
  QVERIFY(writeFixture(importPath, diskImport));
  NativeSession native(root.path());
  QVERIFY2(native.start(), qPrintable(native.details()));

  QVERIFY2(native.compile(QStringLiteral("#let broken = (")), qPrintable(native.details()));
  QCOMPARE(native.reply.header.value(QStringLiteral("op")).toString(), QStringLiteral("compiled"));
  QVERIFY(!native.reply.header.contains(QStringLiteral("snapshot")));
  QVERIFY(!native.reply.header.contains(QStringLiteral("pages")));
  const auto diagnostics = native.reply.header.value(QStringLiteral("diagnostics")).toArray();
  QVERIFY(!diagnostics.isEmpty());
  QCOMPARE(diagnostics.first().toObject().value(QStringLiteral("file")).toString(),
           QStringLiteral("main.typ"));
  QCOMPARE(diagnostics.first().toObject().value(QStringLiteral("severity")).toString(),
           QStringLiteral("error"));
  QVERIFY2(native.pdf(1), qPrintable(native.details()));
  QCOMPARE(native.reply.header.value(QStringLiteral("op")).toString(), QStringLiteral("error"));
  QVERIFY(native.reply.payload.isEmpty());

  const auto source =
      shortSource(QStringLiteral("#import \"chapter.typ\": phrase\nOverlay main: #phrase"));
  QVERIFY2(native.compile(source, {}, importOverlay(QStringLiteral("First import."))),
           qPrintable(native.details()));
  const auto first = native.reply.header.value(QStringLiteral("snapshot"));
  QVERIFY2(first.toDouble() > 0, qPrintable(native.details()));
  QVERIFY2(native.pdf(first), qPrintable(native.details()));
  Pdf firstPdf(native.reply.payload);
  QVERIFY(firstPdf.load());
  QCOMPARE(firstPdf.text(), QStringLiteral("Overlay main: First import."));

  QVERIFY2(
      native.compile(source, {first}, importOverlay(QStringLiteral("Changed unsaved import."))),
      qPrintable(native.details()));
  const auto second = native.reply.header.value(QStringLiteral("snapshot"));
  QVERIFY(second.toDouble() > first.toDouble());
  QVERIFY2(native.pdf(second), qPrintable(native.details()));
  Pdf secondPdf(native.reply.payload);
  QVERIFY(secondPdf.load());
  QCOMPARE(secondPdf.text(), QStringLiteral("Overlay main: Changed unsaved import."));

  QVERIFY2(native.compile(QStringLiteral("#let broken = ("), {second}),
           qPrintable(native.details()));
  QVERIFY(!native.reply.header.contains(QStringLiteral("snapshot")));
  QVERIFY(!native.reply.header.value(QStringLiteral("diagnostics")).toArray().isEmpty());
  QVERIFY2(native.pdf(second), qPrintable(native.details()));
  Pdf retained(native.reply.payload);
  QVERIFY(retained.load());
  QCOMPARE(retained.text(), secondPdf.text());
  QVERIFY2(native.pdf(first), qPrintable(native.details()));
  QCOMPARE(native.reply.header.value(QStringLiteral("op")).toString(), QStringLiteral("error"));

  QVERIFY2(native.compile(shortSource(QStringLiteral("Recovered source.")), {second}),
           qPrintable(native.details()));
  const auto recovered = native.reply.header.value(QStringLiteral("snapshot"));
  QVERIFY(recovered.toDouble() > second.toDouble());
  QVERIFY(native.reply.header.value(QStringLiteral("diagnostics")).toArray().isEmpty());
  QVERIFY2(native.pdf(recovered), qPrintable(native.details()));
  Pdf recoveredPdf(native.reply.payload);
  QVERIFY(recoveredPdf.load());
  QCOMPARE(recoveredPdf.text(), QStringLiteral("Recovered source."));
  QVERIFY2(native.request({{QStringLiteral("op"), QStringLiteral("retain")},
                           {QStringLiteral("snapshots"), QJsonArray{recovered}}}),
           qPrintable(native.details()));
  QVERIFY2(native.pdf(second), qPrintable(native.details()));
  QCOMPARE(native.reply.header.value(QStringLiteral("op")).toString(), QStringLiteral("error"));
  QFile mainFile(mainPath), importedFile(importPath);
  QVERIFY(mainFile.open(QIODevice::ReadOnly));
  QVERIFY(importedFile.open(QIODevice::ReadOnly));
  QCOMPARE(mainFile.readAll(), diskMain);
  QCOMPARE(importedFile.readAll(), diskImport);
}

void TestTypstEditor::unicodeNavigationPdfPointsAndRasterLimits() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  NativeSession native(root.path());
  QVERIFY2(native.start(), qPrintable(native.details()));
  const auto source = shortSource(QString::fromUtf8(
      "// non-BMP: \xf0\x9f\x98\x80\nFirst page.\n#pagebreak()\nNavigationTarget"));
  QVERIFY2(native.compile(source), qPrintable(native.details()));
  const auto snapshot = native.reply.header.value(QStringLiteral("snapshot"));
  QVERIFY2(snapshot.toDouble() > 0, qPrintable(native.details()));
  const auto pages = native.reply.header.value(QStringLiteral("pages")).toArray();
  QCOMPARE(pages.size(), 2);
  QVERIFY2(native.pdf(snapshot), qPrintable(native.details()));
  Pdf pdf(native.reply.payload);
  QVERIFY(pdf.load());
  QCOMPARE(pdf.document.pageCount(), 2);
  QCOMPARE(pdf.text(0), QStringLiteral("First page."));
  QCOMPARE(pdf.text(1), QStringLiteral("NavigationTarget"));
  for (int page = 0; page < pages.size(); ++page) {
    const auto points = pages.at(page).toArray();
    QCOMPARE(points.size(), 2);
    QCOMPARE(pdf.size(page), QSizeF(240, 160));
    QVERIFY(std::abs(pdf.size(page).width() - points.at(0).toDouble()) < 0.01);
    QVERIFY(std::abs(pdf.size(page).height() - points.at(1).toDouble()) < 0.01);
  }

  const int start = source.indexOf(QStringLiteral("NavigationTarget"));
  QVERIFY2(native.request({{QStringLiteral("op"), QStringLiteral("sourceToOutput")},
                           {QStringLiteral("snapshot"), snapshot},
                           {QStringLiteral("file"), QStringLiteral("main.typ")},
                           {QStringLiteral("offset"), start + 4}}),
           qPrintable(native.details()));
  const auto locations = native.reply.header.value(QStringLiteral("locations")).toArray();
  QVERIFY(!locations.isEmpty());
  const auto location = locations.first().toObject();
  QCOMPARE(location.value(QStringLiteral("page")).toInt(), 1);
  const QPointF point(location.value(QStringLiteral("x")).toDouble(),
                      location.value(QStringLiteral("y")).toDouble());
  QRectF textBounds;
  for (const auto &polygon : pdf.document.getAllText(1).bounds())
    textBounds = textBounds.united(polygon.boundingRect());
  QVERIFY2(textBounds.adjusted(-4, -4, 4, 4).contains(point),
           "Source navigation must use the exported PDF's page points");
  QVERIFY2(native.request({{QStringLiteral("op"), QStringLiteral("outputToSource")},
                           {QStringLiteral("snapshot"), snapshot},
                           {QStringLiteral("page"), 1},
                           {QStringLiteral("x"), textBounds.center().x()},
                           {QStringLiteral("y"), textBounds.center().y()}}),
           qPrintable(native.details()));
  const auto mapped = native.reply.header.value(QStringLiteral("location")).toObject();
  QCOMPARE(mapped.value(QStringLiteral("file")).toString(), QStringLiteral("main.typ"));
  QVERIFY(mapped.value(QStringLiteral("start")).toInt(-1) >= start);
  QVERIFY(mapped.value(QStringLiteral("end")).toInt(-1) >=
          mapped.value(QStringLiteral("start")).toInt());
  QVERIFY(mapped.value(QStringLiteral("end")).toInt(-1) <=
          start + QStringLiteral("NavigationTarget").size());

  const int emoji = source.indexOf(QChar(0xd83d));
  QVERIFY(emoji >= 0);
  QVERIFY2(native.request({{QStringLiteral("op"), QStringLiteral("sourceToOutput")},
                           {QStringLiteral("snapshot"), snapshot},
                           {QStringLiteral("file"), QStringLiteral("main.typ")},
                           {QStringLiteral("offset"), emoji + 1}}),
           qPrintable(native.details()));
  QCOMPARE(native.reply.header.value(QStringLiteral("op")).toString(), QStringLiteral("error"));
  QElapsedTimer rejectionTime;
  rejectionTime.start();
  QVERIFY2(native.request({{QStringLiteral("op"), QStringLiteral("raster")},
                           {QStringLiteral("snapshot"), snapshot},
                           {QStringLiteral("page"), 1},
                           {QStringLiteral("scale"), 1000000}}),
           qPrintable(native.details()));
  QCOMPARE(native.reply.header.value(QStringLiteral("op")).toString(), QStringLiteral("error"));
  QVERIFY(native.reply.payload.isEmpty());
  QVERIFY2(rejectionTime.elapsed() < 5000,
           "Invalid raster dimensions must be rejected before rendering/allocation");
  QVERIFY2(native.request({{QStringLiteral("op"), QStringLiteral("raster")},
                           {QStringLiteral("snapshot"), snapshot},
                           {QStringLiteral("page"), 1},
                           {QStringLiteral("scale"), 2}}),
           qPrintable(native.details()));
  QCOMPARE(native.reply.header.value(QStringLiteral("op")).toString(), QStringLiteral("raster"));
  QCOMPARE(native.reply.header.value(QStringLiteral("width")).toInt(), 480);
  QCOMPARE(native.reply.header.value(QStringLiteral("height")).toInt(), 320);
  QCOMPARE(native.reply.header.value(QStringLiteral("stride")).toInt(), 1920);
  QCOMPARE(native.reply.payload.size(), 480 * 320 * 4);
  const QImage image(reinterpret_cast<const uchar *>(native.reply.payload.constData()), 480, 320,
                     1920, QImage::Format_RGBA8888_Premultiplied);
  const QRect textPixels = QRectF(textBounds.topLeft() * 2, textBounds.size() * 2).toAlignedRect();
  QVERIFY2(inkPixels(image, textPixels.adjusted(-2, -2, 2, 2)) > 50,
           "Original-page raster ink must agree with PDF text point geometry");
  QCOMPARE(inkPixels(image, QRect(0, 0, 20, 20)), 0);

  const auto bleedSource =
      QStringLiteral("#set page(width:200pt,height:100pt,margin:10pt,bleed:12pt)\n"
                     "= Target <target>\n#link(<target>)[Go]");
  QVERIFY2(native.compile(bleedSource), qPrintable(native.details()));
  const auto bleedSnapshot = native.reply.header.value(QStringLiteral("snapshot"));
  QVERIFY2(bleedSnapshot.toDouble() > 0, qPrintable(native.details()));
  const auto trimPages = native.reply.header.value(QStringLiteral("pages")).toArray();
  QCOMPARE(trimPages.size(), 1);
  QCOMPARE(trimPages.first().toArray(), QJsonArray({200, 100}));
  const auto links = native.reply.header.value(QStringLiteral("links")).toArray();
  QVERIFY(!links.isEmpty());
  QVERIFY2(native.pdf(bleedSnapshot), qPrintable(native.details()));
  Pdf trimPdf(native.reply.payload);
  QVERIFY(trimPdf.load());
  QCOMPARE(trimPdf.document.pageCount(), 1);
  QCOMPARE(trimPdf.size(0), QSizeF(200, 100));
  const auto extracted = trimPdf.document.getAllText(0).text();
  QVERIFY(extracted.contains(QStringLiteral("Target")));
  const int go = extracted.indexOf(QStringLiteral("Go"));
  QVERIFY(go >= 0);
  QRectF goBounds;
  for (const auto &polygon : trimPdf.document.getSelectionAtIndex(0, go, 2).bounds()) {
    goBounds = goBounds.united(polygon.boundingRect());
  }
  bool linkAgreesWithPdf = false;
  const QRectF trimPage(0, 0, 200, 100);
  for (const auto &value : links) {
    const auto link = value.toObject();
    QCOMPARE(link.value(QStringLiteral("page")).toInt(-1), 0);
    QCOMPARE(link.value(QStringLiteral("targetPage")).toInt(-1), 0);
    const QRectF hit(link.value(QStringLiteral("x")).toDouble(),
                     link.value(QStringLiteral("y")).toDouble(),
                     link.value(QStringLiteral("width")).toDouble(),
                     link.value(QStringLiteral("height")).toDouble());
    QVERIFY(trimPage.contains(hit));
    QVERIFY(trimPage.contains(QPointF(link.value(QStringLiteral("targetX")).toDouble(),
                                      link.value(QStringLiteral("targetY")).toDouble())));
    linkAgreesWithPdf |= hit.intersects(goBounds);
  }
  QVERIFY2(linkAgreesWithPdf,
           "Bleed must not shift PDF glyphs away from the trim-space link sidecar");

  EditorFixture navigation(m_runtime, root.path(), source, VTypstEditor::Mode::Edit, {}, true,
                           false);
  QVERIFY2(navigation.ready(), qPrintable(navigation.editor.compileMessage()));
  QTextCursor cursor(navigation.editor.document());
  cursor.setPosition(start + 4);
  navigation.edit()->setTextCursor(cursor);
  QSignalSpy published(navigation.pane(), SIGNAL(snapshotPublished(quint64)));
  navigation.editor.showOutputAtCursor();
  QVERIFY2(waitUntil([&]() { return published.count() == 1; }),
           qPrintable(navigation.editor.compileMessage()));
  QCOMPARE(navigation.editor.mode(), VTypstEditor::Mode::Edit);
  QVERIFY(navigation.editor.isLivePreviewEnabled());
  QVERIFY(navigation.editor.isInplacePreviewEnabled());
  QCOMPARE(navigation.selectPage().trimmed(), QStringLiteral("NavigationTarget"));
  QCOMPARE(navigation.edit()->textCursor().position(), start + 4);
  QCOMPARE(navigation.editor.document()->toPlainText(), source);
  navigation.clearSelection();
  navigation.editor.setInplacePreviewEnabled(false);
  navigation.editor.setLivePreviewEnabled(false);
  navigation.editor.showOutputAtCursor();
  QVERIFY(navigation.editor.isLivePreviewEnabled());
  QVERIFY(!navigation.editor.isInplacePreviewEnabled());
  QVERIFY(navigation.pane()->isVisible());
  QCOMPARE(navigation.selectPage().trimmed(), QStringLiteral("NavigationTarget"));
  QCOMPARE(navigation.edit()->textCursor().position(), start + 4);
  QCOMPARE(navigation.editor.document()->toPlainText(), source);
}

void TestTypstEditor::previewClickNavigatesSource() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  const auto target = QStringLiteral("NavigationTarget");
  const auto source = QStringLiteral("#set page(width: 240pt, height: 700pt, margin: 24pt)\n") +
                      QString::fromUtf8("// non-BMP: \xf0\x9f\x98\x80\n") +
                      QStringLiteral("// source padding\n").repeated(80) +
                      QStringLiteral("First page.\n#pagebreak()\n") + target;
  EditorFixture fixture(m_runtime, root.path(), source, VTypstEditor::Mode::Edit, {}, false, true);
  auto *pane = fixture.pane();
  auto exports = observePdfExports(&fixture.editor);
  QVERIFY(exports);
  QSignalSpy published(pane, SIGNAL(snapshotPublished(quint64)));
  QVERIFY2(waitUntil([&]() { return published.count() == 1; }),
           qPrintable(fixture.editor.compileMessage()));
  QVERIFY(!exports->isEmpty());
  Pdf pdf(exports->last().at(1).toByteArray());
  QVERIFY(pdf.load());
  QCOMPARE(pdf.text(1), target);
  const auto bounds = pdf.document.getAllText(1).boundingRectangle();
  QVERIFY(!bounds.isEmpty());
  auto *document = fixture.editor.document();
  document->setModified(false);
  const int undoSteps = document->availableUndoSteps();
  const int start = source.indexOf(target);

  std::unique_ptr<QSignalSpy> sourceReplies;
  for (auto *child : fixture.editor.findChildren<QObject *>()) {
    const auto *meta = child->metaObject();
    const int index = meta->indexOfSignal("sourceLocationReady(QString,int,int)");
    if (index >= 0) {
      sourceReplies = std::make_unique<QSignalSpy>(child, meta->method(index));
      break;
    }
  }
  QVERIFY(sourceReplies);
  QVERIFY(sourceReplies->isValid());
  QSignalSpy importedSource(&fixture.editor, &VTypstEditor::sourceLocationRequested);

  for (int step = 0; step < 5; ++step) {
    const auto mode = step == 1 || step == 2 ? VTypstEditor::Mode::Read : VTypstEditor::Mode::Edit;
    fixture.editor.setMode(mode);
    fixture.editor.setInplacePreviewEnabled(step >= 2);
    fixture.editor.setLivePreviewEnabled(step != 2);
    fixture.edit()->setTextCursor(QTextCursor(document));
    QTest::qWait(100); // Let the mode's viewport geometry settle before fitting.
    fixture.editor.fitPdfWidth();
    fixture.editor.setPdfZoom(fixture.editor.pdfZoom() * 0.85);
    fixture.editor.goToPdfPage(1);
    const auto page = firstPdfPageRect(pane, pdf.size(1));
    const QPoint click =
        (page.topLeft() + bounds.center() * (page.width() / pdf.size(1).width())).toPoint();
    QVERIFY(pane->viewport()->rect().contains(click));
    sourceReplies->clear();
    QTest::mouseClick(pane->viewport(), Qt::LeftButton, Qt::NoModifier, click);
    if (mode == VTypstEditor::Mode::Read || step == 4) {
      if (step == 4) {
        // Observe a real Edit-click reply only after Read has hidden the source.
        QVERIFY(sourceReplies->isEmpty());
        fixture.editor.setMode(VTypstEditor::Mode::Read);
        QVERIFY(waitUntil([&]() { return !sourceReplies->isEmpty(); }));
      } else {
        QTest::mouseClick(pane->viewport(), Qt::LeftButton, Qt::ControlModifier, click);
        QTest::qWait(200);
        QVERIFY(sourceReplies->isEmpty());
      }
      QCOMPARE(fixture.editor.mode(), VTypstEditor::Mode::Read);
      QVERIFY(!fixture.edit()->isVisible());
      QVERIFY(pane->isVisible());
      QCOMPARE(fixture.edit()->textCursor().position(), 0);
      QCOMPARE(fixture.edit()->textCursor().anchor(), 0);
      QVERIFY(importedSource.isEmpty());
      QCOMPARE(fixture.editor.isInplacePreviewEnabled(), step >= 2);
      QCOMPARE(fixture.editor.isLivePreviewEnabled(), step != 2);
      QCOMPARE(document->toPlainText(), source);
      QCOMPARE(document->availableUndoSteps(), undoSteps);
      QVERIFY(!document->isModified());
      continue;
    }
    QVERIFY2(waitUntil([&]() {
               const auto cursor = fixture.edit()->textCursor();
               return cursor.position() >= start && cursor.position() <= start + target.size() &&
                      fixture.edit()->hasFocus();
             }),
             qPrintable(fixture.editor.compileMessage()));
    QCOMPARE(fixture.editor.mode(), VTypstEditor::Mode::Edit);
    QCOMPARE(fixture.editor.isInplacePreviewEnabled(), step >= 2);
    QCOMPARE(fixture.editor.isLivePreviewEnabled(), step != 2);
    QVERIFY(pane->isVisible());
    QVERIFY(fixture.edit()->isVisible());
    QVERIFY(!fixture.edit()->textCursor().hasSelection());
    QVERIFY2(waitUntil(
                 [&]() {
                   return fixture.edit()->viewport()->rect().intersects(
                       fixture.edit()->cursorRect());
                 },
                 2000),
             qPrintable(QStringLiteral("mode=%1 cursorY=%2 viewportHeight=%3 scroll=%4/%5")
                            .arg(int(mode))
                            .arg(fixture.edit()->cursorRect().y())
                            .arg(fixture.edit()->viewport()->height())
                            .arg(fixture.edit()->verticalScrollBar()->value())
                            .arg(fixture.edit()->verticalScrollBar()->maximum())));
    QCOMPARE(document->toPlainText(), source);
    QCOMPARE(document->availableUndoSteps(), undoSteps);
    QVERIFY(!document->isModified());
  }
}

void TestTypstEditor::previewClickPreservesSelectionLinksAndStaleOutput() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  const auto target = QStringLiteral("Selection target.");
  const auto source =
      shortSource(target + QStringLiteral("\n\n#link(\"https://example.org/\")[Visit link]"));
  EditorFixture fixture(m_runtime, root.path(), source, VTypstEditor::Mode::Edit, {}, false, true);
  auto *pane = fixture.pane();
  auto *viewport = pane->viewport();
  auto exports = observePdfExports(&fixture.editor);
  QVERIFY(exports);
  QSignalSpy published(pane, SIGNAL(snapshotPublished(quint64)));
  QSignalSpy urls(&fixture.editor, &VTypstEditor::externalLinkRequested);
  QVERIFY2(waitUntil([&]() { return published.count() == 1; }),
           qPrintable(fixture.editor.compileMessage()));
  QVERIFY(!exports->isEmpty());
  Pdf pdf(exports->last().at(1).toByteArray());
  QVERIFY(pdf.load());
  QVERIFY(fitFirstPdfPage(pane));
  const auto page = firstPdfPageRect(pane, pdf.size(0));
  const qreal scale = page.width() / pdf.size(0).width();
  const auto point = [&](const QPointF &p_point) {
    return (page.topLeft() + p_point * scale).toPoint();
  };
  const auto bounds = pdf.document.getSelectionAtIndex(0, 0, target.size()).boundingRectangle();
  QVERIFY(!bounds.isEmpty());
  const auto press = point(QPointF(bounds.left() - 1, bounds.center().y()));
  const auto release = point(QPointF(bounds.right() + 1, bounds.center().y()));
  QVERIFY((release - press).manhattanLength() >= QApplication::startDragDistance());
  fixture.edit()->setTextCursor(QTextCursor(fixture.editor.document()));
  QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, press);
  QTest::mouseMove(viewport, release);
  QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, release);
  QCOMPARE(fixture.selected().trimmed(), target);
  QTest::keyClick(pane, Qt::Key_C, Qt::ControlModifier);
  QCOMPARE(QApplication::clipboard()->text().trimmed(), target);
  QCOMPARE(fixture.edit()->textCursor().position(), 0);
  fixture.clearSelection();

  const int linkIndex = pdf.document.getAllText(0).text().indexOf(QStringLiteral("Visit link"));
  QVERIFY(linkIndex >= 0);
  const auto link = pdf.document.getSelectionAtIndex(0, linkIndex, 10).boundingRectangle();
  QVERIFY(!link.isEmpty());
  QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier, point(link.center()));
  QCOMPARE(urls.count(), 1);
  QCOMPARE(urls.first().first().toUrl(), QUrl(QStringLiteral("https://example.org/")));
  QCOMPARE(urls.first().at(1).toBool(), false);
  QCOMPARE(fixture.edit()->textCursor().position(), 0);

  QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier, QPoint(1, 1));
  // Direct events avoid synthesizing a blocking context menu for the right-button check.
  const QPointF click = point(bounds.center());
  QMouseEvent rightPress(QEvent::MouseButtonPress, click, viewport->mapToGlobal(click.toPoint()),
                         Qt::RightButton, Qt::RightButton, Qt::NoModifier);
  QMouseEvent rightRelease(QEvent::MouseButtonRelease, click,
                           viewport->mapToGlobal(click.toPoint()), Qt::RightButton, Qt::NoButton,
                           Qt::NoModifier);
  QCoreApplication::sendEvent(viewport, &rightPress);
  QCoreApplication::sendEvent(viewport, &rightRelease);
  QCOMPARE(fixture.edit()->textCursor().position(), 0);
  QTest::mouseClick(viewport, Qt::LeftButton, Qt::ControlModifier, click.toPoint());
  const int start = source.indexOf(target);
  QVERIFY(waitUntil([&]() {
    const int position = fixture.edit()->textCursor().position();
    return position >= start && position <= start + target.size();
  }));
  QCOMPARE(fixture.editor.document()->toPlainText(), source);

  fixture.editor.setFileOverlay(QStringLiteral("main.typ"),
                                source + QStringLiteral("\n#let x = ("));
  QVERIFY(waitUntil(
      [&]() { return fixture.editor.compileStatus() == VTypstEditor::CompileStatus::Error; }));
  QCOMPARE(published.count(), 1); // Last-good PDF remains displayed, but its mapping is stale.
  const auto cursor = fixture.edit()->textCursor();
  QSignalSpy navigation(pane, SIGNAL(sourceLocationRequested(quint64, int, QPointF)));
  QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier, click.toPoint());
  QCOMPARE(navigation.count(), 0);
  QCOMPARE(fixture.edit()->textCursor().position(), cursor.position());
  QCOMPARE(fixture.edit()->textCursor().anchor(), cursor.anchor());
}

void TestTypstEditor::repeatedAndSplitOutputRecords() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  NativeSession native(root.path());
  QVERIFY2(native.start(), qPrintable(native.details()));
  const auto source = QStringLiteral("#set page(width: 100mm, height: 80mm, margin: 8mm)\n"
                                     "#for n in (2, 7, 11) { [Before $ E = #n c^2 $ after.] }");
  QVERIFY2(native.compile(source), qPrintable(native.details()));
  const auto records = native.reply.header.value(QStringLiteral("previews")).toArray();
  QCOMPARE(records.size(), 3);
  const auto first = records.first().toObject();
  const int start = first.value(QStringLiteral("start")).toInt();
  const int end = first.value(QStringLiteral("end")).toInt();
  QCOMPARE(source.mid(start, end - start), QStringLiteral("$ E = #n c^2 $"));
  QSet<double> ids;
  QVector<QRectF> regions;
  for (const auto &value : records) {
    const auto record = value.toObject();
    QCOMPARE(record.value(QStringLiteral("status")).toString(), QStringLiteral("ready"));
    QCOMPARE(record.value(QStringLiteral("start")).toInt(), start);
    QCOMPARE(record.value(QStringLiteral("end")).toInt(), end);
    ids.insert(record.value(QStringLiteral("id")).toDouble());
    const auto parts = record.value(QStringLiteral("regions")).toArray();
    QCOMPARE(parts.size(), 1);
    const auto region = parts.first().toObject();
    regions.append(QRectF(region.value(QStringLiteral("x")).toDouble(),
                          region.value(QStringLiteral("y")).toDouble(),
                          region.value(QStringLiteral("width")).toDouble(),
                          region.value(QStringLiteral("height")).toDouble()));
  }
  QCOMPARE(ids.size(), 3);
  QVERIFY(regions.at(0) != regions.at(1));
  QVERIFY(regions.at(1) != regions.at(2));
  const auto repeatedSnapshot = native.reply.header.value(QStringLiteral("snapshot"));
  QVERIFY2(native.pdf(repeatedSnapshot), qPrintable(native.details()));
  Pdf repeated(native.reply.payload);
  QVERIFY(repeated.load());
  QCOMPARE(repeated.text().count(QStringLiteral("Before")), 3);
  QVERIFY(repeated.text().contains(QStringLiteral("11")));
  QVERIFY(repeated.text().contains(QLatin1Char('7')));

  QVERIFY2(native.compile(splitSource()), qPrintable(native.details()));
  const auto split = native.reply.header.value(QStringLiteral("previews")).toArray();
  QCOMPARE(split.size(), 1);
  const auto occurrence = split.first().toObject();
  QCOMPARE(occurrence.value(QStringLiteral("status")).toString(), QStringLiteral("ready"));
  const auto parts = occurrence.value(QStringLiteral("regions")).toArray();
  QVERIFY(parts.size() >= 2);
  QSet<int> pages;
  int previousPage = -1;
  for (const auto &part : parts) {
    const int page = part.toObject().value(QStringLiteral("page")).toInt(-1);
    QVERIFY(page >= previousPage);
    previousPage = page;
    pages.insert(page);
  }
  QVERIFY(pages.size() >= 2);
  const auto splitSnapshot = native.reply.header.value(QStringLiteral("snapshot"));
  QVERIFY2(native.pdf(splitSnapshot), qPrintable(native.details()));
  Pdf splitPdf(native.reply.payload);
  QVERIFY(splitPdf.load());
  QVERIFY(splitPdf.document.pageCount() >= pages.size());
  for (const int page : pages)
    QVERIFY(
        splitPdf.text(page).normalized(QString::NormalizationForm_KC).contains(QLatin1Char('x')));
}

void TestTypstEditor::outlineIncludesEvaluatedHeadingsAndUnicodeRanges() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  const auto unicode = QString::fromUtf8("\xc3\xa9\xf0\x9f\x98\x80");
  const auto main = QStringLiteral("#set heading(numbering: \"1.\", offset: 1, bookmarked: false)\n"
                                   "// %1 before the first source range\n"
                                   "#let suffix = [%1]\n"
                                   "= *Main* #suffix\n"
                                   "#import \"chapter.typ\": chapter\n"
                                   "#chapter\n"
                                   "#for title in (\"First\", \"Second\") [\n"
                                   "  === #title\n"
                                   "]\n"
                                   "#heading(level: 8)[]\n"
                                   "#heading(outlined: false)[Excluded]\n"
                                   "#outline()\n")
                        .arg(unicode);
  const auto chapter = QStringLiteral("// %1 before the imported range\n"
                                      "#let chapter = [\n  == _Imported_ %1\n]\n")
                           .arg(unicode);
  EditorFixture fixture(m_runtime, root.path(), main);
  fixture.editor.setFileOverlay(QStringLiteral("chapter.typ"), chapter);
  QSignalSpy updates(&fixture.editor, &VTypstEditor::headingsUpdated);
  QVERIFY(updates.isValid());
  QVERIFY(fixture.editor.getHeadings().isEmpty());
  QVERIFY2(waitUntil([&]() { return !updates.isEmpty(); }),
           qPrintable(fixture.editor.compileMessage()));

  const QVector<TypstHeadingInfo> headings = fixture.editor.getHeadings();
  const auto payload = qvariant_cast<QVector<TypstHeadingInfo>>(updates.last().at(0));
  const QStringList titles{QStringLiteral("Main ") + unicode, QStringLiteral("Imported ") + unicode,
                           QStringLiteral("First"), QStringLiteral("Second"), QString()};
  const QVector<int> levels{2, 3, 4, 4, 8};
  QCOMPARE(headings.size(), titles.size());
  QCOMPARE(payload.size(), headings.size());
  for (int index = 0; index < headings.size(); ++index) {
    const auto &heading = headings.at(index);
    QCOMPARE(heading.m_title, titles.at(index));
    QCOMPARE(heading.m_level, levels.at(index));
    QCOMPARE(heading.m_file,
             index == 1 ? QStringLiteral("chapter.typ") : QStringLiteral("main.typ"));
    const auto &source = index == 1 ? chapter : main;
    QVERIFY(heading.m_startPos >= 0);
    QVERIFY(heading.m_endPos > heading.m_startPos);
    QVERIFY(heading.m_endPos <= source.size());
    QCOMPARE(payload.at(index).m_title, heading.m_title);
    QCOMPARE(payload.at(index).m_level, heading.m_level);
    QCOMPARE(payload.at(index).m_file, heading.m_file);
    QCOMPARE(payload.at(index).m_startPos, heading.m_startPos);
    QCOMPARE(payload.at(index).m_endPos, heading.m_endPos);
  }
  const auto mainHeading = QStringLiteral("= *Main* #suffix");
  QCOMPARE(headings.at(0).m_startPos, main.indexOf(mainHeading));
  QCOMPARE(headings.at(0).m_endPos, main.indexOf(mainHeading) + mainHeading.size());
  const auto importedHeading = QStringLiteral("== _Imported_ ") + unicode;
  QCOMPARE(headings.at(1).m_startPos, chapter.indexOf(importedHeading));
  QCOMPARE(headings.at(1).m_endPos, chapter.indexOf(importedHeading) + importedHeading.size());
  const auto generatedHeading = QStringLiteral("=== #title");
  for (const int index : {2, 3}) {
    QCOMPARE(headings.at(index).m_startPos, main.indexOf(generatedHeading));
    QCOMPARE(headings.at(index).m_endPos, main.indexOf(generatedHeading) + generatedHeading.size());
  }
}

void TestTypstEditor::outlineInvalidationFailureAndRecovery() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  EditorFixture fixture(m_runtime, root.path(), shortSource(QStringLiteral("= Before")),
                        VTypstEditor::Mode::Edit, {}, false, true);
  QSignalSpy updates(&fixture.editor, &VTypstEditor::headingsUpdated);
  QSignalSpy published(fixture.pane(), SIGNAL(snapshotPublished(quint64)));
  QVERIFY2(waitUntil([&]() {
             return !updates.isEmpty() && fixture.editor.getHeadings().size() == 1 &&
                    published.count() == 1;
           }),
           qPrintable(fixture.editor.compileMessage()));
  const QVector<TypstHeadingInfo> retained = fixture.editor.getHeadings();
  const auto retainedPayload = qvariant_cast<QVector<TypstHeadingInfo>>(updates.last().at(0));
  QCOMPARE(retained.first().m_title, QStringLiteral("Before"));
  QVERIFY(fixture.selectPage().contains(QStringLiteral("Before")));
  const auto selected = fixture.selected();

  const int beforeEdit = updates.count();
  QTextCursor cursor(fixture.editor.document());
  cursor.setPosition(fixture.editor.document()->toPlainText().indexOf(QStringLiteral("Before")));
  cursor.setPosition(cursor.position() + 6, QTextCursor::KeepAnchor);
  cursor.insertText(QStringLiteral("After"));
  QVERIFY(fixture.editor.getHeadings().isEmpty());
  QCOMPARE(updates.count(), beforeEdit + 1);
  QVERIFY(qvariant_cast<QVector<TypstHeadingInfo>>(updates.last().at(0)).isEmpty());
  QVERIFY2(waitUntil([&]() {
             const auto &headings = fixture.editor.getHeadings();
             return headings.size() == 1 && headings.first().m_title == QStringLiteral("After");
           }),
           qPrintable(fixture.editor.compileMessage()));
  // A selected old PDF can defer its own publication, but never the current outline.
  QVERIFY(waitUntil([&]() { return fixture.editor.hasPendingPdfUpdate(); }));
  QCOMPARE(published.count(), 1);
  QCOMPARE(fixture.selected(), selected);
  QCOMPARE(qvariant_cast<QVector<TypstHeadingInfo>>(updates.last().at(0)).first().m_title,
           QStringLiteral("After"));

  const int beforeFailure = updates.count();
  fixture.editor.setFileOverlay(QStringLiteral("main.typ"),
                                shortSource(QStringLiteral("= Invalid\n#let broken = (")));
  QVERIFY(fixture.editor.getHeadings().isEmpty());
  QCOMPARE(updates.count(), beforeFailure + 1);
  QVERIFY(waitUntil(
      [&]() { return fixture.editor.compileStatus() == VTypstEditor::CompileStatus::Error; }));
  QVERIFY(fixture.editor.getHeadings().isEmpty());
  QCOMPARE(updates.count(), beforeFailure + 1);
  QCOMPARE(fixture.selected(), selected);

  // Success must publish an empty list even when failure already cleared the getter.
  const int beforeEmpty = updates.count();
  fixture.editor.setFileOverlay(QStringLiteral("main.typ"),
                                shortSource(QStringLiteral("No headings here.")));
  QCOMPARE(updates.count(), beforeEmpty);
  QVERIFY2(waitUntil([&]() { return updates.count() > beforeEmpty; }),
           qPrintable(fixture.editor.compileMessage()));
  QCOMPARE(updates.count(), beforeEmpty + 1);
  QVERIFY(fixture.editor.getHeadings().isEmpty());
  QVERIFY(qvariant_cast<QVector<TypstHeadingInfo>>(updates.last().at(0)).isEmpty());
  QCOMPARE(fixture.editor.compileStatus(), VTypstEditor::CompileStatus::Ready);

  fixture.editor.setFileOverlay(QStringLiteral("main.typ"),
                                shortSource(QStringLiteral("= Recovered")));
  QVERIFY2(waitUntil([&]() {
             const auto &headings = fixture.editor.getHeadings();
             return headings.size() == 1 && headings.first().m_title == QStringLiteral("Recovered");
           }),
           qPrintable(fixture.editor.compileMessage()));
  QCOMPARE(qvariant_cast<QVector<TypstHeadingInfo>>(updates.last().at(0)).first().m_title,
           QStringLiteral("Recovered"));
  QCOMPARE(retained.first().m_title, QStringLiteral("Before"));
  QCOMPARE(retainedPayload.first().m_title, QStringLiteral("Before"));
  QCOMPARE(retainedPayload.first().m_startPos, retained.first().m_startPos);
  QCOMPARE(retainedPayload.first().m_endPos, retained.first().m_endPos);
}

void TestTypstEditor::outlineScopeAndModesPreserveSource() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  const auto main = QStringLiteral("= Main\n#include \"chapter.typ\"\n= End\n");
  const auto chapter = QStringLiteral("== Imported\n");
  EditorFixture fixture(m_runtime, root.path(), main, VTypstEditor::Mode::Edit, {}, false, true);
  fixture.editor.setFileOverlay(QStringLiteral("chapter.typ"), chapter);
  auto *document = fixture.editor.document();
  QTextCursor cursor(document);
  cursor.movePosition(QTextCursor::End);
  cursor.insertText(QStringLiteral("// Undo marker"));
  cursor.setPosition(2);
  cursor.setPosition(6, QTextCursor::KeepAnchor);
  fixture.edit()->setTextCursor(cursor);
  const auto source = document->toPlainText();
  const int undoSteps = document->availableUndoSteps();
  QVERIFY(undoSteps > 0);
  document->setModified(false);
  QSignalSpy changes(document, &QTextDocument::contentsChange);
  QSignalSpy updates(&fixture.editor, &VTypstEditor::headingsUpdated);
  QVERIFY2(
      waitUntil([&]() { return !updates.isEmpty() && fixture.editor.getHeadings().size() == 3; }),
      qPrintable(fixture.editor.compileMessage()));
  const QVector<TypstHeadingInfo> retained = fixture.editor.getHeadings();
  QCOMPARE(retained.at(0).m_title, QStringLiteral("Main"));
  QCOMPARE(retained.at(1).m_title, QStringLiteral("Imported"));
  QCOMPARE(retained.at(2).m_title, QStringLiteral("End"));
  const int beforeModes = updates.count();
  for (int step = 0; step < 4; ++step) {
    const auto mode = step == 1 || step == 2 ? VTypstEditor::Mode::Edit : VTypstEditor::Mode::Read;
    if (step == 1 || step == 2) {
      fixture.editor.setInplacePreviewEnabled(step == 1);
      fixture.editor.setLivePreviewEnabled(step == 2);
    }
    fixture.editor.setMode(mode);
    QCoreApplication::processEvents();
    const auto &headings = fixture.editor.getHeadings();
    QCOMPARE(headings.size(), retained.size());
    for (int index = 0; index < headings.size(); ++index) {
      QCOMPARE(headings.at(index).m_title, retained.at(index).m_title);
      QCOMPARE(headings.at(index).m_level, retained.at(index).m_level);
      QCOMPARE(headings.at(index).m_file, retained.at(index).m_file);
      QCOMPARE(headings.at(index).m_startPos, retained.at(index).m_startPos);
      QCOMPARE(headings.at(index).m_endPos, retained.at(index).m_endPos);
    }
  }
  QCOMPARE(updates.count(), beforeModes);
  QVERIFY(fixture.editor.getTextEditor()->isHidden());
  QVERIFY(fixture.edit()->isReadOnly());
  fixture.editor.hide();
  const auto changedChapter = QStringLiteral("== Changed import\n");
  fixture.editor.setFileOverlay(QStringLiteral("chapter.typ"), changedChapter);
  QVERIFY(fixture.editor.getHeadings().isEmpty());
  QCOMPARE(updates.count(), beforeModes + 1);
  QVERIFY2(waitUntil([&]() {
             const auto &headings = fixture.editor.getHeadings();
             return headings.size() == 3 &&
                    headings.at(1).m_title == QStringLiteral("Changed import");
           }),
           qPrintable(fixture.editor.compileMessage()));
  QCOMPARE(document->toPlainText(), source);
  QCOMPARE(document->availableUndoSteps(), undoSteps);
  QVERIFY(!document->isModified());
  QCOMPARE(fixture.edit()->textCursor().anchor(), cursor.anchor());
  QCOMPARE(fixture.edit()->textCursor().position(), cursor.position());
  QCOMPARE(textChangeCount(changes), 0);

  const int beforeActiveFile = updates.count();
  fixture.editor.setActiveFile(QStringLiteral("chapter.typ"));
  QVERIFY(fixture.editor.getHeadings().isEmpty());
  QCOMPARE(updates.count(), beforeActiveFile + 1);
  QCOMPARE(document->toPlainText(), source);
  fixture.editor.setFileOverlay(QStringLiteral("chapter.typ"), changedChapter);
  QVERIFY2(waitUntil([&]() { return fixture.editor.getHeadings().size() == 3; }),
           qPrintable(fixture.editor.compileMessage()));
  QCOMPARE(fixture.editor.activeFile(), QStringLiteral("chapter.typ"));
  QCOMPARE(document->toPlainText(), changedChapter);
  QCOMPARE(fixture.editor.getHeadings().at(0).m_title, QStringLiteral("Main"));
  QCOMPARE(fixture.editor.getHeadings().at(0).m_file, QStringLiteral("main.typ"));
  QCOMPARE(fixture.editor.getHeadings().at(1).m_file, QStringLiteral("chapter.typ"));
  QCOMPARE(fixture.editor.getHeadings().at(1).m_startPos, 0);
  QCOMPARE(fixture.editor.getHeadings().at(2).m_title, QStringLiteral("End"));

  fixture.editor.setFileOverlay(QStringLiteral("other.typ"),
                                QStringLiteral("= Other main\n#include \"chapter.typ\"\n"));
  QVERIFY2(waitUntil([&]() { return fixture.editor.getHeadings().size() == 3; }),
           qPrintable(fixture.editor.compileMessage()));
  const int beforeProject = updates.count();
  const int chapterUndoSteps = document->availableUndoSteps();
  const bool chapterModified = document->isModified();
  changes.clear();
  fixture.editor.setProject(root.path(), QStringLiteral("other.typ"));
  QVERIFY(fixture.editor.getHeadings().isEmpty());
  QCOMPARE(updates.count(), beforeProject + 1);
  QVERIFY2(waitUntil([&]() { return fixture.editor.getHeadings().size() == 2; }),
           qPrintable(fixture.editor.compileMessage()));
  QCOMPARE(fixture.editor.getHeadings().at(0).m_title, QStringLiteral("Other main"));
  QCOMPARE(fixture.editor.getHeadings().at(0).m_file, QStringLiteral("other.typ"));
  QCOMPARE(fixture.editor.getHeadings().at(1).m_title, QStringLiteral("Changed import"));
  QCOMPARE(fixture.editor.activeFile(), QStringLiteral("chapter.typ"));
  QCOMPARE(document->toPlainText(), changedChapter);
  QCOMPARE(document->availableUndoSteps(), chapterUndoSteps);
  QCOMPARE(document->isModified(), chapterModified);
  QCOMPARE(textChangeCount(changes), 0);
  QCOMPARE(retained.at(1).m_title, QStringLiteral("Imported"));
}

void TestTypstEditor::supersededCompileCannotPublishOutline() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  EditorFixture fixture(m_runtime, root.path(), shortSource(QStringLiteral("= Baseline")));
  QSignalSpy updates(&fixture.editor, &VTypstEditor::headingsUpdated);
  QVERIFY2(waitUntil([&]() { return fixture.editor.getHeadings().size() == 1; }),
           qPrintable(fixture.editor.compileMessage()));
  for (int round = 0; round < 2; ++round) {
    updates.clear();
    bool replaced = false;
    int compilingEvents = 0;
    const auto title = QStringLiteral("Newest %1").arg(round);
    const auto newest = shortSource(QStringLiteral("= ") + title);
    const auto connection = QObject::connect(
        &fixture.editor, &VTypstEditor::compileStatusChanged, &fixture.editor,
        [&](VTypstEditor::CompileStatus p_status, const QString &) {
          // Ready is emitted before outline publication. On a warm session the
          // second Compiling event follows the actual request write.
          if (p_status == VTypstEditor::CompileStatus::Compiling)
            ++compilingEvents;
          if (!replaced && ((round == 0 && p_status == VTypstEditor::CompileStatus::Ready) ||
                            (round == 1 && p_status == VTypstEditor::CompileStatus::Compiling &&
                             compilingEvents == 2))) {
            replaced = true;
            fixture.editor.setFileOverlay(QStringLiteral("main.typ"), newest);
          }
        });
    QTextCursor cursor(fixture.editor.document());
    cursor.beginEditBlock();
    cursor.select(QTextCursor::Document);
    cursor.insertText(shortSource(QStringLiteral("= Obsolete %1").arg(round)));
    cursor.endEditBlock();
    QVERIFY(fixture.editor.getHeadings().isEmpty());
    QVERIFY2(waitUntil([&]() {
               const auto &headings = fixture.editor.getHeadings();
               return headings.size() == 1 && headings.first().m_title == title;
             }),
             qPrintable(fixture.editor.compileMessage()));
    QObject::disconnect(connection);
    QVERIFY(replaced);
    QCOMPARE(fixture.editor.document()->toPlainText(), newest);
    int nonemptyUpdates = 0;
    for (const auto &notification : updates) {
      const auto headings = qvariant_cast<QVector<TypstHeadingInfo>>(notification.at(0));
      if (!headings.isEmpty()) {
        ++nonemptyUpdates;
        QCOMPARE(headings.size(), 1);
        QCOMPARE(headings.first().m_title, title);
      }
    }
    QCOMPARE(nonemptyUpdates, 1);
  }

  // An earlier consumer may invalidate synchronously without changing the payload
  // still being delivered to later consumers of the accepted snapshot's signal.
  QVector<TypstHeadingInfo> retainedSignal;
  QVector<TypstHeadingInfo> afterInvalidation;
  const auto connection = QObject::connect(
      &fixture.editor, &VTypstEditor::headingsUpdated, &fixture.editor,
      [&](const QVector<TypstHeadingInfo> &p_headings) {
        if (p_headings.size() == 1 && p_headings.first().m_title == QStringLiteral("Reentrant")) {
          retainedSignal = p_headings;
          fixture.editor.setFileOverlay(QStringLiteral("main.typ"),
                                        shortSource(QStringLiteral("= Final")));
          afterInvalidation = p_headings;
        }
      });
  QSignalSpy laterConsumer(&fixture.editor, &VTypstEditor::headingsUpdated);
  fixture.editor.setFileOverlay(QStringLiteral("main.typ"),
                                shortSource(QStringLiteral("= Reentrant")));
  QVERIFY2(waitUntil([&]() {
             const auto &headings = fixture.editor.getHeadings();
             return headings.size() == 1 && headings.first().m_title == QStringLiteral("Final");
           }),
           qPrintable(fixture.editor.compileMessage()));
  QObject::disconnect(connection);
  QCOMPARE(retainedSignal.size(), 1);
  QCOMPARE(afterInvalidation.size(), 1);
  QCOMPARE(afterInvalidation.first().m_title, QStringLiteral("Reentrant"));
  QCOMPARE(afterInvalidation.first().m_file, retainedSignal.first().m_file);
  QCOMPARE(afterInvalidation.first().m_startPos, retainedSignal.first().m_startPos);
  QCOMPARE(afterInvalidation.first().m_endPos, retainedSignal.first().m_endPos);
  QStringList deliveredTitles;
  for (const auto &notification : laterConsumer) {
    const auto headings = qvariant_cast<QVector<TypstHeadingInfo>>(notification.at(0));
    if (!headings.isEmpty())
      deliveredTitles.append(headings.first().m_title);
  }
  QCOMPARE(deliveredTitles, QStringList({QStringLiteral("Reentrant"), QStringLiteral("Final")}));
}

void TestTypstEditor::independentEditPreviews_data() {
  QTest::addColumn<bool>("inplace");
  QTest::addColumn<bool>("live");
  QTest::newRow("source-only") << false << false;
  QTest::newRow("cards-only") << true << false;
  QTest::newRow("pdf-only") << false << true;
  QTest::newRow("cards-and-pdf") << true << true;
}

void TestTypstEditor::independentEditPreviews() {
  QFETCH(bool, inplace);
  QFETCH(bool, live);
  QTemporaryDir root;
  QVERIFY(root.isValid());
  const auto source = shortSource(
      QStringLiteral("#set text(fill: rgb(\"#ff0000\"), size: 24pt)\n= Preview\n$ x^2 $\nAfter"));
  EditorFixture fixture(m_runtime, root.path(), source, VTypstEditor::Mode::Edit, {}, inplace,
                        live);
  QSignalSpy published(fixture.pane(), SIGNAL(snapshotPublished(quint64)));
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  QVERIFY(fixture.edit()->isVisible());
  QCOMPARE(fixture.pane()->isVisible(), live);
  if (inplace) {
    QVERIFY(waitUntil([&]() { return previewHasOnlyColor(previewImages(fixture.editor), true); }));
  } else {
    QVERIFY(previewImages(fixture.editor).isEmpty());
  }
  if (live) {
    QVERIFY(waitUntil([&]() { return published.count() == 1; }));
    QVERIFY(fixture.selectPage().contains(QStringLiteral("Preview")));
    fixture.clearSelection();
  }
  fixture.editor.setMode(VTypstEditor::Mode::Read);
  QVERIFY(waitUntil([&]() { return published.count() == 1; }));
  QVERIFY(!fixture.edit()->isVisible());
  QVERIFY(previewImages(fixture.editor).isEmpty());
  QVERIFY(fixture.pane()->isVisible());
  QVERIFY(fixture.selectPage().contains(QStringLiteral("Preview")));
  fixture.clearSelection();
  QCOMPARE(fixture.editor.isInplacePreviewEnabled(), inplace);
  QCOMPARE(fixture.editor.isLivePreviewEnabled(), live);
  // Read stores preferences without revealing source or hiding its PDF.
  fixture.editor.setInplacePreviewEnabled(!inplace);
  fixture.editor.setLivePreviewEnabled(!live);
  QVERIFY(!fixture.edit()->isVisible());
  QVERIFY(fixture.pane()->isVisible());
  fixture.editor.setInplacePreviewEnabled(inplace);
  fixture.editor.setLivePreviewEnabled(live);
  fixture.editor.setMode(VTypstEditor::Mode::Edit);
  QVERIFY(fixture.edit()->isVisible());
  QCOMPARE(fixture.pane()->isVisible(), live);
  if (inplace) {
    QVERIFY(waitUntil([&]() { return previewHasOnlyColor(previewImages(fixture.editor), true); }));
  } else {
    QVERIFY(previewImages(fixture.editor).isEmpty());
  }
  // Compile with both surfaces disabled, then reveal the latest rendered snapshot.
  fixture.editor.setInplacePreviewEnabled(false);
  fixture.editor.setLivePreviewEnabled(false);
  auto color = fixture.editor.document()->find(QStringLiteral("#ff0000"));
  QVERIFY(!color.isNull());
  color.insertText(QStringLiteral("#0000ff"));
  auto heading = fixture.editor.document()->find(QStringLiteral("Preview"));
  QVERIFY(!heading.isNull());
  heading.insertText(QStringLiteral("Latest"));
  QVERIFY2(waitUntil([&]() {
             const auto &headings = fixture.editor.getHeadings();
             return headings.size() == 1 && headings.first().m_title == QStringLiteral("Latest");
           }),
           qPrintable(fixture.editor.compileMessage()));
  QVERIFY(previewImages(fixture.editor).isEmpty());
  QCOMPARE(published.count(), 1);
  fixture.editor.setInplacePreviewEnabled(true);
  QVERIFY(waitUntil([&]() { return previewHasOnlyColor(previewImages(fixture.editor), false); }));
  QVERIFY(!fixture.pane()->isVisible());
  fixture.editor.setLivePreviewEnabled(true);
  QVERIFY(waitUntil([&]() { return published.count() == 2; }));
  fixture.editor.fitPdfPage();
  QVERIFY(waitUntil(
      [&]() { return coloredPixels(widgetImage(fixture.pane()->viewport()), false) > 20; }));
  QVERIFY(previewHasOnlyColor(previewImages(fixture.editor), false));
}

void TestTypstEditor::combinedPreviewsAdvanceWhilePdfSelectionIsDeferred() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  const auto source = shortSource(
      QStringLiteral("#set text(fill: rgb(\"#ff0000\"), size: 24pt)\n= Before\n$ x^2 $\nAfter"));
  EditorFixture fixture(m_runtime, root.path(), source, VTypstEditor::Mode::Edit, {}, true, true);
  QSignalSpy published(fixture.pane(), SIGNAL(snapshotPublished(quint64)));
  auto exports = observePdfExports(&fixture.editor);
  QVERIFY(exports);
  QVERIFY2(waitUntil([&]() {
             return published.count() == 1 &&
                    previewHasOnlyColor(previewImages(fixture.editor), true);
           }),
           qPrintable(fixture.editor.compileMessage()));
  QVERIFY(fixture.selectPage().contains(QStringLiteral("Before")));
  const auto selected = fixture.selected();
  const auto displayedSnapshot = published.first().at(0).toULongLong();
  auto heading = fixture.editor.document()->find(QStringLiteral("Before"));
  QVERIFY(!heading.isNull());
  heading.beginEditBlock();
  heading.insertText(QStringLiteral("Updated"));
  auto color = fixture.editor.document()->find(QStringLiteral("#ff0000"));
  QVERIFY(!color.isNull());
  color.insertText(QStringLiteral("#0000ff"));
  heading.endEditBlock();
  QVERIFY2(waitUntil([&]() {
             const auto &headings = fixture.editor.getHeadings();
             return headings.size() == 1 && headings.first().m_title == QStringLiteral("Updated") &&
                    previewHasOnlyColor(previewImages(fixture.editor), false) &&
                    fixture.editor.hasPendingPdfUpdate();
           }),
           qPrintable(fixture.editor.compileMessage()));
  QCOMPARE(published.count(), 1);
  QCOMPARE(fixture.selected(), selected);
  fixture.editor.setMode(VTypstEditor::Mode::Read);
  QCoreApplication::processEvents();
  QCOMPARE(fixture.selected(), selected);
  QVERIFY(fixture.editor.hasPendingPdfUpdate());
  QVERIFY(fixture.editor.isInplacePreviewEnabled());
  QVERIFY(fixture.editor.isLivePreviewEnabled());
  fixture.editor.setLivePreviewEnabled(false);
  QVERIFY(fixture.pane()->isVisible());
  fixture.editor.setMode(VTypstEditor::Mode::Edit);
  QCoreApplication::processEvents();
  QVERIFY(!fixture.pane()->isVisible());
  QCOMPARE(fixture.selected(), selected);
  QVERIFY(fixture.editor.hasPendingPdfUpdate());
  fixture.editor.setLivePreviewEnabled(true);
  fixture.editor.setInplacePreviewEnabled(false);
  QCoreApplication::processEvents();
  QVERIFY(previewImages(fixture.editor).isEmpty());
  QCOMPARE(fixture.selected(), selected);
  QVERIFY(fixture.editor.hasPendingPdfUpdate());
  fixture.editor.setInplacePreviewEnabled(true);
  QVERIFY(waitUntil([&]() { return previewHasOnlyColor(previewImages(fixture.editor), false); }));
  QCOMPARE(fixture.editor.getHeadings().first().m_title, QStringLiteral("Updated"));
  QCOMPARE(fixture.selected(), selected);
  QCOMPARE(published.count(), 1);
  QVERIFY(fixture.editor.hasPendingPdfUpdate());
  fixture.editor.publishPendingPdfUpdate();
  QVERIFY(waitUntil([&]() { return published.count() == 2; }));
  QVERIFY(published.last().at(0).toULongLong() != displayedSnapshot);
  QVERIFY(!fixture.editor.hasPendingPdfUpdate());
  QVERIFY(fixture.selected().isEmpty());
  QVERIFY(!exports->isEmpty());
  Pdf updated(exports->last().at(1).toByteArray());
  QVERIFY(updated.load());
  QVERIFY(updated.text().contains(QStringLiteral("Updated")));
  QVERIFY(!updated.text().contains(QStringLiteral("Before")));
  fixture.editor.fitPdfPage();
  QVERIFY(waitUntil(
      [&]() { return coloredPixels(widgetImage(fixture.pane()->viewport()), false) > 20; }));
  QVERIFY(fixture.selectPage().contains(QStringLiteral("Updated")));
  QVERIFY(!fixture.selected().contains(QStringLiteral("Before")));
}

void TestTypstEditor::pdfZoomModeNotifications() {
  using ZoomMode = VTypstEditor::PdfZoomMode;
  QTemporaryDir root;
  QVERIFY(root.isValid());
  auto config = editorConfig(root.path(), &m_runtime);
  config->m_initialZoom = 1.25;
  VTypstEditor editor(config, QSharedPointer<TextEditorParameters>::create());
  auto *pane = editor.findChild<QAbstractScrollArea *>(QStringLiteral("typstPdf"));
  QVERIFY(pane);
  QSignalSpy zooms(&editor, &VTypstEditor::pdfZoomChanged);
  const auto *meta = pane->metaObject();
  QSignalSpy nativeZooms(
      pane, meta->method(meta->indexOfSignal("zoomChanged(qreal,vte::VTypstEditor::PdfZoomMode)")));
  QVERIFY(zooms.isValid());
  QVERIFY(nativeZooms.isValid());
  auto *document = editor.document();
  QSignalSpy changes(document, &QTextDocument::contentsChange);
  const auto source = document->toPlainText();
  const bool modified = document->isModified();
  const int undoSteps = document->availableUndoSteps();
  QCOMPARE(editor.pdfPageCount(), 0);
  QCOMPARE(editor.pdfZoom(), qreal(1.25));
  QCOMPARE(editor.pdfZoomMode(), ZoomMode::Custom);
  editor.setPdfZoom(1.25);
  QCOMPARE(zooms.count(), 0);
  editor.fitPdfWidth();
  QCOMPARE(editor.pdfZoomMode(), ZoomMode::Width);
  QCOMPARE(zooms.count(), 1);
  editor.fitPdfWidth();
  QCOMPARE(zooms.count(), 1);
  editor.fitPdfPage();
  QCOMPARE(editor.pdfZoomMode(), ZoomMode::Page);
  QCOMPARE(zooms.count(), 2);
  editor.fitPdfPage();
  QCOMPARE(zooms.count(), 2);
  editor.setPdfZoom(editor.pdfZoom());
  QCOMPARE(editor.pdfZoomMode(), ZoomMode::Custom);
  QCOMPARE(zooms.count(), 3);
  QCOMPARE(zooms.at(0).at(0).toReal(), qreal(1.25));
  QCOMPARE(zooms.at(0).at(1).value<ZoomMode>(), ZoomMode::Width);
  QCOMPARE(zooms.at(1).at(0).toReal(), qreal(1.25));
  QCOMPARE(zooms.at(1).at(1).value<ZoomMode>(), ZoomMode::Page);
  QCOMPARE(zooms.at(2).at(0).toReal(), qreal(1.25));
  QCOMPARE(zooms.at(2).at(1).value<ZoomMode>(), ZoomMode::Custom);
  QCOMPARE(nativeZooms.count(), zooms.count());
  for (int i = 0; i < zooms.count(); ++i)
    QCOMPARE(nativeZooms.at(i), zooms.at(i));

  editor.setPdfZoom(0.875);
  QCOMPARE(zooms.count(), 4);
  config->m_initialZoom = 7;
  editor.setConfig(config);
  QCOMPARE(editor.pdfZoom(), qreal(0.875));
  QCOMPARE(editor.pdfZoomMode(), ZoomMode::Custom);
  QCOMPARE(zooms.count(), 4);
  editor.fitPdfWidth();
  editor.setConfig(config);
  editor.setMode(VTypstEditor::Mode::Read);
  editor.setMode(VTypstEditor::Mode::Edit);
  QCOMPARE(editor.pdfZoom(), qreal(0.875));
  QCOMPARE(editor.pdfZoomMode(), ZoomMode::Width);
  QCOMPARE(zooms.count(), 5);
  QCOMPARE(document->toPlainText(), source);
  QCOMPARE(document->isModified(), modified);
  QCOMPARE(document->availableUndoSteps(), undoSteps);
  QCOMPARE(textChangeCount(changes), 0);
}

void TestTypstEditor::pdfFitBeforePublication_data() {
  QTest::addColumn<bool>("fitWidth");
  QTest::newRow("width") << true;
  QTest::newRow("page") << false;
}

void TestTypstEditor::pdfFitBeforePublication() {
  QFETCH(bool, fitWidth);
  using ZoomMode = VTypstEditor::PdfZoomMode;
  const auto mode = fitWidth ? ZoomMode::Width : ZoomMode::Page;
  QTemporaryDir root;
  QVERIFY(root.isValid());
  const auto source = shortSource(QStringLiteral("Restored preview camera."));
  EditorFixture fixture(m_runtime, root.path(), source);
  auto *pane = fixture.pane();
  QVERIFY(pane);
  const auto *meta = pane->metaObject();
  QSignalSpy published(pane, meta->method(meta->indexOfSignal("snapshotPublished(quint64)")));
  QSignalSpy zooms(&fixture.editor, &VTypstEditor::pdfZoomChanged);
  QVERIFY(published.isValid());
  QVERIFY(zooms.isValid());
  auto *document = fixture.editor.document();
  document->setModified(false);
  const int undoSteps = document->availableUndoSteps();
  QSignalSpy changes(document, &QTextDocument::contentsChange);
  QCOMPARE(fixture.editor.pdfPageCount(), 0);
  if (fitWidth)
    fixture.editor.fitPdfWidth();
  else
    fixture.editor.fitPdfPage();
  QCOMPARE(fixture.editor.pdfZoomMode(), mode);
  QCOMPARE(zooms.count(), 1);
  QCOMPARE(zooms.first().at(0).toReal(), qreal(1));
  QCOMPARE(zooms.first().at(1).value<ZoomMode>(), mode);
  auto config = QSharedPointer<TypstEditorConfig>::create(fixture.editor.getConfig());
  config->m_initialZoom = 7;
  fixture.editor.setConfig(config);
  QCOMPARE(fixture.editor.pdfZoomMode(), mode);
  QCOMPARE(fixture.editor.pdfZoom(), qreal(1));
  QCOMPARE(zooms.count(), 1);
  auto fittedZoom = [&]() {
    const qreal dpiScale = qMax(qreal(1), qreal(pane->logicalDpiY())) / 72.0;
    const qreal width = qMax(qreal(1), qreal(pane->viewport()->width()) - 32) / 240;
    const qreal height = qMax(qreal(1), qreal(pane->viewport()->height()) - 32) / 160;
    return (fitWidth ? width : qMin(width, height)) / dpiScale;
  };
  QVERIFY2(waitUntil([&]() { return published.count() == 1; }),
           qPrintable(fixture.editor.compileMessage()));
  QCOMPARE(fixture.editor.pdfZoomMode(), mode);
  QVERIFY(qFuzzyCompare(fixture.editor.pdfZoom(), fittedZoom()));
  QCOMPARE(zooms.count(), 2);
  QCOMPARE(zooms.last().at(0).toReal(), fixture.editor.pdfZoom());
  QCOMPARE(zooms.last().at(1).value<ZoomMode>(), mode);

  const qreal initialZoom = fixture.editor.pdfZoom();
  zooms.clear();
  fixture.editor.resize(820, 520);
  QVERIFY(waitUntil([&]() { return !zooms.isEmpty(); }));
  QVERIFY(!qFuzzyCompare(fixture.editor.pdfZoom(), initialZoom));
  QVERIFY(qFuzzyCompare(fixture.editor.pdfZoom(), fittedZoom()));
  QCOMPARE(fixture.editor.pdfZoomMode(), mode);
  QCOMPARE(zooms.last().at(0).toReal(), fixture.editor.pdfZoom());
  QCOMPARE(zooms.last().at(1).value<ZoomMode>(), mode);
  QTest::qWait(100);
  zooms.clear();
  fixture.editor.resize(820, 520);
  fixture.editor.setConfig(config);
  fixture.editor.retryCompilation();
  QVERIFY2(waitUntil([&]() { return published.count() == 2; }),
           qPrintable(fixture.editor.compileMessage()));
  QCOMPARE(zooms.count(), 0);
  QCOMPARE(fixture.editor.pdfZoomMode(), mode);
  QVERIFY(qFuzzyCompare(fixture.editor.pdfZoom(), fittedZoom()));
  fixture.editor.setMode(VTypstEditor::Mode::Edit);
  fixture.editor.setLivePreviewEnabled(true);
  fixture.editor.setMode(VTypstEditor::Mode::Read);
  QCoreApplication::processEvents();
  QCOMPARE(fixture.editor.pdfZoomMode(), mode);
  QVERIFY(qFuzzyCompare(fixture.editor.pdfZoom(), fittedZoom()));
  QCOMPARE(document->toPlainText(), source);
  QVERIFY(!document->isModified());
  QCOMPARE(document->availableUndoSteps(), undoSteps);
  QCOMPARE(textChangeCount(changes), 0);
}

void TestTypstEditor::pdfZoomPublicationIsReentrant() {
  using ZoomMode = VTypstEditor::PdfZoomMode;
  QTemporaryDir root;
  QVERIFY(root.isValid());
  const auto source = QStringLiteral("#import \"size.typ\": width, height\n"
                                     "#set page(width: width, height: height, margin: 16pt)\n"
                                     "Camera publication remains reentrant.");
  EditorFixture fixture(m_runtime, root.path(), source);
  fixture.editor.setFileOverlay(QStringLiteral("size.typ"),
                                QStringLiteral("#let width = 240pt\n#let height = 160pt"));
  auto *pane = fixture.pane();
  QVERIFY(pane);
  const auto *meta = pane->metaObject();
  QSignalSpy published(pane, meta->method(meta->indexOfSignal("snapshotPublished(quint64)")));
  QVERIFY(published.isValid());
  fixture.editor.fitPdfWidth();
  QVERIFY2(waitUntil([&]() { return published.count() == 1; }),
           qPrintable(fixture.editor.compileMessage()));
  const qreal initialZoom = fixture.editor.pdfZoom();
  auto *document = fixture.editor.document();
  document->setModified(false);
  const int undoSteps = document->availableUndoSteps();
  QSignalSpy changes(document, &QTextDocument::contentsChange);
  QSignalSpy zooms(&fixture.editor, &VTypstEditor::pdfZoomChanged);
  bool reentered = false;
  const auto connection =
      connect(&fixture.editor, &VTypstEditor::pdfZoomChanged, &fixture.editor,
              [&](qreal p_zoom, ZoomMode p_mode) {
                if (reentered || p_mode != ZoomMode::Width)
                  return;
                reentered = true;
                QCOMPARE(published.count(), 2);
                QCOMPARE(fixture.editor.pdfPageCount(), 1);
                QCOMPARE(fixture.editor.pdfZoomMode(), p_mode);
                QCOMPARE(fixture.editor.pdfZoom(), p_zoom);
                QVERIFY(qFuzzyCompare(p_zoom, initialZoom / 2));
                fixture.editor.setPdfZoom(1.125);
                auto config = QSharedPointer<TypstEditorConfig>::create(fixture.editor.getConfig());
                config->m_initialZoom = 7;
                fixture.editor.setConfig(config);
              });
  fixture.editor.setFileOverlay(QStringLiteral("size.typ"),
                                QStringLiteral("#let width = 480pt\n#let height = 320pt"));
  QVERIFY2(waitUntil([&]() { return published.count() == 2; }),
           qPrintable(fixture.editor.compileMessage()));
  QVERIFY(reentered);
  QCOMPARE(zooms.count(), 2);
  QCOMPARE(zooms.first().at(1).value<ZoomMode>(), ZoomMode::Width);
  QCOMPARE(zooms.last().at(1).value<ZoomMode>(), ZoomMode::Custom);
  QCOMPARE(zooms.last().at(0).toReal(), qreal(1.125));
  QTest::qWait(100);
  QCOMPARE(fixture.editor.pdfZoom(), qreal(1.125));
  QCOMPARE(fixture.editor.pdfZoomMode(), ZoomMode::Custom);
  QCOMPARE(zooms.count(), 2);
  QObject::disconnect(connection);
  QCOMPARE(fixture.selectPage().trimmed(), QStringLiteral("Camera publication remains reentrant."));
  QCOMPARE(document->toPlainText(), source);
  QVERIFY(!document->isModified());
  QCOMPARE(document->availableUndoSteps(), undoSteps);
  QCOMPARE(textChangeCount(changes), 0);
}

void TestTypstEditor::pdfZoomWheelAndContextMenu() {
  using ZoomMode = VTypstEditor::PdfZoomMode;
  QTemporaryDir root;
  QVERIFY(root.isValid());
  const auto source = shortSource(QStringLiteral("Native camera gestures."));
  EditorFixture fixture(m_runtime, root.path(), source);
  auto *pane = fixture.pane();
  QVERIFY(pane);
  QVERIFY2(waitUntil([&]() { return fixture.editor.pdfPageCount() == 1; }),
           qPrintable(fixture.editor.compileMessage()));
  auto *document = fixture.editor.document();
  document->setModified(false);
  const int undoSteps = document->availableUndoSteps();
  const int sourceZoom = fixture.editor.getTextEditor()->zoomDelta();
  QSignalSpy changes(document, &QTextDocument::contentsChange);
  QSignalSpy zooms(&fixture.editor, &VTypstEditor::pdfZoomChanged);
  fixture.editor.fitPdfPage();
  const qreal fitted = fixture.editor.pdfZoom();
  zooms.clear();
  auto *viewport = pane->viewport();
  const QPoint center = viewport->rect().center();
  auto wheel = [&](int p_delta, Qt::KeyboardModifiers p_modifiers) {
    QWheelEvent event(QPointF(center), QPointF(viewport->mapToGlobal(center)), QPoint(),
                      QPoint(0, p_delta), Qt::NoButton, p_modifiers, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(viewport, &event);
  };
  wheel(120, Qt::ControlModifier);
  QCOMPARE(fixture.editor.pdfZoomMode(), ZoomMode::Custom);
  QVERIFY(qFuzzyCompare(fixture.editor.pdfZoom(), fitted * 1.2));
  QCOMPARE(zooms.count(), 1);
  QCOMPARE(zooms.last().at(1).value<ZoomMode>(), ZoomMode::Custom);
  QCOMPARE(zooms.last().at(0).toReal(), fixture.editor.pdfZoom());
  wheel(0, Qt::ControlModifier);
  wheel(-120, Qt::NoModifier);
  QCOMPARE(zooms.count(), 1);
  QCOMPARE(fixture.editor.getTextEditor()->zoomDelta(), sourceZoom);

  bool triggered = false;
  QTimer::singleShot(0, pane, [&]() {
    auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
    if (!menu)
      return;
    const auto label = QCoreApplication::translate(pane->metaObject()->className(), "Fit width");
    for (auto *action : menu->actions()) {
      if (action->text() == label) {
        triggered = true;
        action->trigger();
        break;
      }
    }
    menu->close();
  });
  QContextMenuEvent contextMenu(QContextMenuEvent::Mouse, center, viewport->mapToGlobal(center));
  QCoreApplication::sendEvent(viewport, &contextMenu);
  QVERIFY(triggered);
  QCOMPARE(fixture.editor.pdfZoomMode(), ZoomMode::Width);
  QCOMPARE(zooms.count(), 2);
  QCOMPARE(zooms.last().at(1).value<ZoomMode>(), ZoomMode::Width);
  QCOMPARE(zooms.last().at(0).toReal(), fixture.editor.pdfZoom());

  // Both fits are width-limited here: mode changes must still notify.
  fixture.editor.resize(600, 900);
  QCoreApplication::processEvents();
  const qreal widthZoom = fixture.editor.pdfZoom();
  zooms.clear();
  fixture.editor.fitPdfPage();
  QCOMPARE(fixture.editor.pdfZoom(), widthZoom);
  QCOMPARE(fixture.editor.pdfZoomMode(), ZoomMode::Page);
  QCOMPARE(zooms.count(), 1);
  fixture.editor.setPdfZoom(widthZoom);
  QCOMPARE(fixture.editor.pdfZoomMode(), ZoomMode::Custom);
  QCOMPARE(zooms.count(), 2);
  fixture.editor.setPdfZoom(widthZoom);
  QCOMPARE(zooms.count(), 2);
  fixture.editor.setMode(VTypstEditor::Mode::Edit);
  fixture.editor.setLivePreviewEnabled(true);
  fixture.editor.getTextEditor()->zoom(sourceZoom + 1);
  fixture.editor.setMode(VTypstEditor::Mode::Read);
  QCoreApplication::processEvents();
  QCOMPARE(fixture.editor.pdfZoom(), widthZoom);
  QCOMPARE(fixture.editor.pdfZoomMode(), ZoomMode::Custom);
  QCOMPARE(zooms.count(), 2);
  QCOMPARE(fixture.editor.getTextEditor()->zoomDelta(), sourceZoom + 1);
  QCOMPARE(document->toPlainText(), source);
  QVERIFY(!document->isModified());
  QCOMPARE(document->availableUndoSteps(), undoSteps);
  QCOMPARE(textChangeCount(changes), 0);
}

void TestTypstEditor::modesPreserveSourceUndoAndSelection() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  const auto original = shortSource(QStringLiteral("Same source document."));
  EditorFixture fixture(m_runtime, root.path(), original, VTypstEditor::Mode::Edit, {}, true,
                        false);
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  auto *document = fixture.editor.document();
  document->setModified(false);
  QTextCursor cursor(document);
  cursor.movePosition(QTextCursor::End);
  cursor.insertText(QStringLiteral(" Edited."));
  const auto edited = document->toPlainText();
  cursor.setPosition(edited.size());
  cursor.setPosition(edited.size() - 7, QTextCursor::KeepAnchor);
  fixture.edit()->setTextCursor(cursor);
  const int undoSteps = document->availableUndoSteps();
  QVERIFY(undoSteps > 0);
  QVERIFY(document->isModified());
  QSignalSpy changes(document, &QTextDocument::contentsChange);
  for (const bool readOnly : {false, true}) {
    fixture.editor.setReadOnly(readOnly);
    for (const bool inplace : {false, true}) {
      for (const bool live : {false, true}) {
        fixture.editor.setInplacePreviewEnabled(inplace);
        fixture.editor.setLivePreviewEnabled(live);
        for (const auto mode :
             {VTypstEditor::Mode::Edit, VTypstEditor::Mode::Read, VTypstEditor::Mode::Edit}) {
          fixture.editor.setMode(mode);
          QCoreApplication::processEvents();
          QCOMPARE(document->toPlainText(), edited);
          QCOMPARE(fixture.edit()->textCursor().anchor(), cursor.anchor());
          QCOMPARE(fixture.edit()->textCursor().position(), cursor.position());
          QCOMPARE(document->availableUndoSteps(), undoSteps);
          QVERIFY(document->isModified());
          QCOMPARE(fixture.editor.isReadOnly(), readOnly);
          QCOMPARE(fixture.edit()->isReadOnly(), readOnly || mode == VTypstEditor::Mode::Read);
          QCOMPARE(fixture.editor.getTextEditor()->isHidden(), mode == VTypstEditor::Mode::Read);
          QCOMPARE(fixture.editor.isInplacePreviewEnabled(), inplace);
          QCOMPARE(fixture.editor.isLivePreviewEnabled(), live);
        }
      }
    }
  }
  QCOMPARE(textChangeCount(changes), 0);
  fixture.editor.setReadOnly(true);
  fixture.editor.setMode(VTypstEditor::Mode::Read);
  fixture.editor.setInplacePreviewEnabled(true);
  fixture.editor.setLivePreviewEnabled(false);
  fixture.editor.setMode(VTypstEditor::Mode::Edit);
  QVERIFY(fixture.editor.isReadOnly());
  QVERIFY(fixture.edit()->isReadOnly());
  fixture.editor.setReadOnly(false);
  document->undo();
  QCOMPARE(document->toPlainText(), original);
  QVERIFY(!document->isModified());
  document->redo();
  QCOMPARE(document->toPlainText(), edited);
}

void TestTypstEditor::hiddenSourceAndImportChangesReachPdf() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  EditorFixture fixture(
      m_runtime, root.path(),
      shortSource(QStringLiteral("#import \"chapter.typ\": phrase\nInitial: #phrase")));
  fixture.editor.setFileOverlay(QStringLiteral("chapter.typ"),
                                QStringLiteral("#let phrase = [unsaved import]"));
  QVERIFY(fixture.pane());
  QSignalSpy published(fixture.pane(), SIGNAL(snapshotPublished(quint64)));
  QVERIFY(published.isValid());
  QVERIFY2(waitUntil([&]() { return published.count() == 1; }),
           qPrintable(fixture.editor.compileMessage()));
  QCOMPARE(fixture.selectPage().trimmed(), QStringLiteral("Initial: unsaved import"));
  fixture.clearSelection();
  QVERIFY(fixture.editor.getTextEditor()->isHidden());
  QVERIFY(fixture.edit()->isReadOnly());
  QTextCursor cursor(fixture.editor.document());
  const int start = fixture.editor.document()->toPlainText().indexOf(QStringLiteral("Initial"));
  cursor.setPosition(start);
  cursor.setPosition(start + 7, QTextCursor::KeepAnchor);
  cursor.insertText(QStringLiteral("Hidden edit"));
  fixture.editor.setFileOverlay(QStringLiteral("chapter.typ"),
                                QStringLiteral("#let phrase = [new import]"));
  QVERIFY2(waitUntil([&]() { return published.count() == 2; }),
           qPrintable(fixture.editor.compileMessage()));
  QCOMPARE(fixture.selectPage().trimmed(), QStringLiteral("Hidden edit: new import"));
  QVERIFY(fixture.editor.document()->isModified());
  QVERIFY(!QFileInfo::exists(root.filePath(QStringLiteral("main.typ"))));
  QVERIFY(!QFileInfo::exists(root.filePath(QStringLiteral("chapter.typ"))));
}

void TestTypstEditor::diskDependenciesRecoverAfterReplacementAndRecreation() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  const auto chapter = root.filePath(QStringLiteral("chapter.typ"));
  QVERIFY(writeFixture(chapter, QByteArrayLiteral("#let phrase = [Original disk import]")));
  EditorFixture fixture(m_runtime, root.path(),
                        shortSource(QStringLiteral("#import \"chapter.typ\": phrase\n#phrase")));
  QSignalSpy published(fixture.pane(), SIGNAL(snapshotPublished(quint64)));
  QVERIFY(published.isValid());
  QVERIFY2(waitUntil([&] { return published.count() == 1; }),
           qPrintable(fixture.editor.compileMessage()));
  QCOMPARE(fixture.selectPage().trimmed(), QStringLiteral("Original disk import"));
  fixture.clearSelection();

  const auto replacement = root.filePath(QStringLiteral("replacement.typ"));
  QVERIFY(writeFixture(replacement, QByteArrayLiteral("#let phrase = [Replaced disk import]")));
  QVERIFY(QFile::remove(chapter));
  QVERIFY(QFile::rename(replacement, chapter));
  QVERIFY2(waitUntil([&] { return published.count() == 2; }),
           qPrintable(fixture.editor.compileMessage()));
  QCOMPARE(fixture.selectPage().trimmed(), QStringLiteral("Replaced disk import"));
  fixture.clearSelection();

  QVERIFY(QFile::remove(chapter));
  QVERIFY(waitUntil(
      [&] { return fixture.editor.compileStatus() == VTypstEditor::CompileStatus::Error; }));
  QCOMPARE(published.count(), 2);
  QCOMPARE(fixture.selectPage().trimmed(), QStringLiteral("Replaced disk import"));
  fixture.clearSelection();
  QVERIFY(writeFixture(chapter, QByteArrayLiteral("#let phrase = [Recreated disk import]")));
  QVERIFY2(waitUntil([&] { return published.count() == 3; }),
           qPrintable(fixture.editor.compileMessage()));
  QCOMPARE(fixture.selectPage().trimmed(), QStringLiteral("Recreated disk import"));
  QVERIFY(fixture.editor.diagnostics().isEmpty());
}

void TestTypstEditor::diagnosticsPreserveLastGoodOutput() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  EditorFixture fixture(m_runtime, root.path(), QStringLiteral("#let broken = ("));
  QSignalSpy published(fixture.pane(), SIGNAL(snapshotPublished(quint64)));
  QVERIFY2(waitUntil([&]() {
             return fixture.editor.compileStatus() == VTypstEditor::CompileStatus::Error;
           }),
           qPrintable(fixture.editor.compileMessage()));
  QCOMPARE(published.count(), 0);
  QCOMPARE(fixture.selectPage(), QString());
  QVERIFY(
      fixture.editor.diagnostics().join(QLatin1Char('\n')).contains(QStringLiteral("main.typ")));
  fixture.editor.setFileOverlay(QStringLiteral("main.typ"),
                                shortSource(QStringLiteral("Last good output.")));
  QVERIFY2(waitUntil([&]() { return published.count() == 1; }),
           qPrintable(fixture.editor.compileMessage()));
  QCOMPARE(fixture.selectPage().trimmed(), QStringLiteral("Last good output."));
  fixture.editor.setFileOverlay(QStringLiteral("main.typ"), QStringLiteral("#let broken = ("));
  QVERIFY(waitUntil(
      [&]() { return fixture.editor.compileStatus() == VTypstEditor::CompileStatus::Error; }));
  QCOMPARE(published.count(), 1);
  QCOMPARE(fixture.selected().trimmed(), QStringLiteral("Last good output."));
  fixture.clearSelection();
  QCOMPARE(fixture.selectPage().trimmed(), QStringLiteral("Last good output."));
  QTest::keyClick(fixture.pane(), Qt::Key_C, Qt::ControlModifier);
  QCOMPARE(QApplication::clipboard()->text().trimmed(), QStringLiteral("Last good output."));
  fixture.clearSelection();
  fixture.editor.setFileOverlay(QStringLiteral("main.typ"),
                                shortSource(QStringLiteral("Recovered PDF.")));
  QVERIFY2(waitUntil([&]() { return published.count() == 2; }),
           qPrintable(fixture.editor.compileMessage()));
  QCOMPARE(fixture.selectPage().trimmed(), QStringLiteral("Recovered PDF."));
  QCOMPARE(fixture.editor.compileStatus(), VTypstEditor::CompileStatus::Ready);
  QVERIFY(fixture.editor.diagnostics().isEmpty());
}

void TestTypstEditor::selectionDefersPublicationAcrossScrollAndModes() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  auto config = editorConfig(root.path(), &m_runtime);
  config->m_maxCacheBytes = 8 * 1024 * 1024;
  const auto pages =
      QStringLiteral("#set page(width: 320pt, height: 400pt, margin: 20pt)\n"
                     "Before update.\n#pagebreak()\nMiddle page.\n#pagebreak()\nLast page.");
  EditorFixture fixture(m_runtime, root.path(), pages, VTypstEditor::Mode::Edit, config, false,
                        true);
  QSignalSpy published(fixture.pane(), SIGNAL(snapshotPublished(quint64)));
  QVERIFY2(waitUntil([&]() { return published.count() == 1; }),
           qPrintable(fixture.editor.compileMessage()));
  const auto selected = fixture.selectPage();
  QCOMPARE(selected.trimmed(), QStringLiteral("Before update."));
  fixture.editor.setPdfZoom(1.5);
  fixture.editor.setMode(VTypstEditor::Mode::Read);
  QCOMPARE(fixture.selected(), selected);
  QCOMPARE(fixture.editor.pdfZoom(), 1.5);
  auto changed = pages;
  changed.replace(QStringLiteral("Before update."), QStringLiteral("After update."));
  fixture.editor.setFileOverlay(QStringLiteral("main.typ"), changed);
  QVERIFY2(waitUntil([&]() { return fixture.editor.hasPendingPdfUpdate(); }),
           qPrintable(fixture.editor.compileMessage()));
  QCOMPARE(published.count(), 1);
  QCOMPARE(fixture.selected(), selected);
  auto *scroll = fixture.pane()->verticalScrollBar();
  QVERIFY(scroll->maximum() > 0);
  scroll->setValue(scroll->maximum());
  QTest::qWait(100);
  QCOMPARE(fixture.selected(), selected);
  QApplication::clipboard()->setText(QStringLiteral("clipboard sentinel"));
  QTest::keyClick(fixture.pane(), Qt::Key_C, Qt::ControlModifier);
  QCOMPARE(QApplication::clipboard()->text(), selected);
  fixture.editor.setInplacePreviewEnabled(false);
  fixture.editor.setLivePreviewEnabled(true);
  fixture.editor.setMode(VTypstEditor::Mode::Edit);
  QCOMPARE(fixture.selected(), selected);
  QCOMPARE(published.count(), 1);
  for (const bool live : {false, true}) {
    fixture.editor.setLivePreviewEnabled(live);
    QCoreApplication::processEvents();
    QCOMPARE(fixture.selected(), selected);
    QCOMPARE(published.count(), 1);
    QVERIFY(fixture.editor.hasPendingPdfUpdate());
  }
  fixture.editor.publishPendingPdfUpdate();
  QVERIFY2(waitUntil([&]() { return published.count() == 2; }),
           qPrintable(fixture.editor.compileMessage()));
  QVERIFY(fixture.selected().isEmpty());
  QVERIFY(!fixture.editor.hasPendingPdfUpdate());
  QCOMPARE(fixture.editor.pdfZoom(), 1.5);
  fixture.editor.goToPdfPage(0);
  QCOMPARE(fixture.selectPage().trimmed(), QStringLiteral("After update."));
  QTest::keyClick(fixture.pane(), Qt::Key_C, Qt::ControlModifier);
  QCOMPARE(QApplication::clipboard()->text().trimmed(), QStringLiteral("After update."));
  fixture.clearSelection();
  fixture.editor.goToPdfPage(2);
  QCOMPARE(fixture.selectPage().trimmed(), QStringLiteral("Last page."));
}

void TestTypstEditor::openingPreservesSourcePosition_data() {
  QTest::addColumn<int>("line");
  QTest::addColumn<bool>("reset");
  QTest::newRow("fresh-start") << 0 << false;
  QTest::newRow("restored-line") << 80 << false;
  QTest::newRow("rebound-start") << 0 << true;
  QTest::newRow("rebound-line") << 80 << true;
}

void TestTypstEditor::openingPreservesSourcePosition() {
  QFETCH(int, line);
  QFETCH(bool, reset);
  QTemporaryDir root;
  QVERIFY(root.isValid());
  QString source = QStringLiteral("#set page(width: 320pt, height: 440pt, margin: 20pt)\n");
  for (int index = 0; index < 120; ++index) {
    source += QStringLiteral("Line %1: remembered source position. $x^2$\n\n").arg(index);
  }
  EditorFixture fixture(m_runtime, root.path(),
                        reset ? shortSource(QStringLiteral("Outgoing")) : source,
                        VTypstEditor::Mode::Edit, {}, true, true);
  if (reset) {
    QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
    fixture.editor.resetForNewDocument();
    fixture.editor.getTextEditor()->setText(source);
    fixture.editor.finishDocumentReset();
  }
  auto *edit = fixture.edit();
  auto *text = fixture.editor.getTextEditor();
  if (line > 0)
    text->scrollToLine(line, true);
  QCOMPARE(edit->textCursor().blockNumber(), line);
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  QVERIFY(waitUntil([&]() { return fixture.editor.pdfPageCount() > 1; }));
  // Compilation, card publication, and cursor-to-PDF mapping are all asynchronous.
  QTest::qWait(500);
  QCOMPARE(edit->textCursor().blockNumber(), line);
  QVERIFY2(edit->viewport()->rect().intersects(edit->cursorRect()),
           qPrintable(QStringLiteral("line=%1 cursorY=%2 scroll=%3/%4")
                          .arg(line)
                          .arg(edit->cursorRect().y())
                          .arg(edit->verticalScrollBar()->value())
                          .arg(edit->verticalScrollBar()->maximum())));
  if (line == 0)
    QCOMPARE(edit->verticalScrollBar()->value(), 0);

  // Same-document reload, then immediate explicit navigation, must supersede old anchors.
  text->setText(source);
  text->scrollToLine(40, true);
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  QTest::qWait(500);
  QCOMPARE(edit->textCursor().blockNumber(), 40);
  QVERIFY(edit->viewport()->rect().intersects(edit->cursorRect()));
  QCOMPARE(fixture.editor.document()->toPlainText(), source);
}

void TestTypstEditor::sideBySideScrollSync() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  QString source = QStringLiteral("#set page(width: 320pt, height: 440pt, margin: 20pt)\n");
  for (int line = 0; line < 120; ++line) {
    source += QStringLiteral("Line %1: scrolling keeps both views together.\n\n").arg(line);
  }
  EditorFixture fixture(m_runtime, root.path(), source, VTypstEditor::Mode::Edit, {}, false, true);
  auto *pane = fixture.pane();
  auto *edit = fixture.edit();
  auto *sourceBar = edit->verticalScrollBar();
  auto *pdfBar = pane->verticalScrollBar();
  QSignalSpy published(pane, SIGNAL(snapshotPublished(quint64)));
  QVERIFY2(waitUntil([&]() { return published.count() == 1; }),
           qPrintable(fixture.editor.compileMessage()));
  QCoreApplication::processEvents();
  QVERIFY(sourceBar->maximum() > 0);
  QVERIFY(pdfBar->maximum() > 0);

  QTextCursor cursor(fixture.editor.document());
  cursor.setPosition(source.indexOf(QStringLiteral("Line 0")));
  cursor.setPosition(cursor.position() + 6, QTextCursor::KeepAnchor);
  edit->setTextCursor(cursor);
  const auto selectedPdf = fixture.selectPage();
  QVERIFY(selectedPdf.contains(QStringLiteral("Line 0")));
  auto *document = fixture.editor.document();
  document->setModified(false);
  const int undoSteps = document->availableUndoSteps();
  QSignalSpy changes(document, &QTextDocument::contentsChange);
  const auto *focus = QApplication::focusWidget();
  auto mapped = [](QScrollBar *p_from, QScrollBar *p_to) {
    const qreal progress =
        qreal(p_from->value() - p_from->minimum()) / (p_from->maximum() - p_from->minimum());
    return qRound(p_to->minimum() + progress * (p_to->maximum() - p_to->minimum()));
  };

  // Direct scrollbar moves cover dragging; the driver must not round-trip and drift.
  const int sourceMiddle = sourceBar->maximum() / 3;
  sourceBar->setValue(sourceMiddle);
  QCOMPARE(pdfBar->value(), mapped(sourceBar, pdfBar));
  QCOMPARE(sourceBar->value(), sourceMiddle);
  const int pdfMiddle = pdfBar->maximum() * 2 / 3;
  pdfBar->setValue(pdfMiddle);
  QCOMPARE(sourceBar->value(), mapped(pdfBar, sourceBar));
  QCOMPARE(pdfBar->value(), pdfMiddle);

  // PDF keyboard navigation bypasses scrollbar signals internally.
  QTest::keyClick(pane, Qt::Key_PageDown);
  QVERIFY(pdfBar->value() > pdfMiddle);
  QCOMPARE(sourceBar->value(), mapped(pdfBar, sourceBar));
  sourceBar->setValue(sourceBar->maximum());
  QCOMPARE(pdfBar->value(), pdfBar->maximum());
  pdfBar->setValue(0);
  QCOMPARE(sourceBar->value(), 0);

  // Zoom and horizontal-only panning must not pull the source to a new position.
  sourceBar->setValue(sourceMiddle);
  QVERIFY(pane->setProperty("zoom", 2.0));
  QCoreApplication::processEvents();
  QCOMPARE(sourceBar->value(), sourceMiddle);
  auto *horizontal = pane->horizontalScrollBar();
  QVERIFY(horizontal->maximum() > 0);
  horizontal->setValue(horizontal->value() == 0 ? horizontal->maximum() : 0);
  QCOMPARE(sourceBar->value(), sourceMiddle);
  QTest::keyClick(pane, Qt::Key_PageDown);
  QCOMPARE(sourceBar->value(), mapped(pdfBar, sourceBar));
  QVERIFY(QMetaObject::invokeMethod(pane, "goToPage", Qt::DirectConnection, Q_ARG(int, 0),
                                    Q_ARG(QPointF, QPointF())));
  QCOMPARE(sourceBar->value(), 0);
  QCOMPARE(edit->textCursor().anchor(), cursor.anchor());
  QCOMPARE(edit->textCursor().position(), cursor.position());
  QCOMPARE(fixture.selected(), selectedPdf);
  QCOMPARE(QApplication::focusWidget(), focus);
  QCOMPARE(document->toPlainText(), source);
  QCOMPARE(document->availableUndoSteps(), undoSteps);
  QVERIFY(!document->isModified());
  QCOMPARE(textChangeCount(changes), 0);

  // Hidden panes stay independent, including asynchronous mode-switch layout.
  fixture.editor.setMode(VTypstEditor::Mode::Read);
  QCoreApplication::processEvents();
  const int hiddenSource = sourceBar->value();
  pdfBar->setValue(pdfBar->maximum());
  QCOMPARE(sourceBar->value(), hiddenSource);
  fixture.editor.setInplacePreviewEnabled(false);
  fixture.editor.setLivePreviewEnabled(true);
  fixture.editor.setMode(VTypstEditor::Mode::Edit);
  QCoreApplication::processEvents();
  QCOMPARE(sourceBar->value(), hiddenSource);
  sourceBar->setValue(sourceBar->maximum() / 2);
  QCOMPARE(pdfBar->value(), mapped(sourceBar, pdfBar));
  fixture.editor.setInplacePreviewEnabled(true);
  fixture.editor.setLivePreviewEnabled(false);
  fixture.editor.setMode(VTypstEditor::Mode::Edit);
  QCoreApplication::processEvents();
  QCOMPARE(fixture.selected(), selectedPdf);
  const int hiddenPdf = pdfBar->value();
  sourceBar->setValue(0);
  QCOMPARE(pdfBar->value(), hiddenPdf);
  fixture.editor.setInplacePreviewEnabled(false);
  fixture.editor.setLivePreviewEnabled(true);
  fixture.editor.setMode(VTypstEditor::Mode::Edit);
  QCoreApplication::processEvents();
  QCOMPARE(pdfBar->value(), hiddenPdf);
  QCOMPARE(fixture.selected(), selectedPdf);

  // A source range clamp on resize is layout, not an instruction to scroll PDF.
  sourceBar->setValue(sourceBar->maximum());
  const int beforeResize = pdfBar->value();
  edit->resize(edit->width(), edit->height() + 120);
  QCoreApplication::processEvents();
  QCOMPARE(pdfBar->value(), beforeResize);
  sourceBar->setValue(sourceBar->maximum() / 4);
  QCOMPARE(pdfBar->value(), mapped(sourceBar, pdfBar));

  // A selected old PDF remains usable while compilation produces a new range.
  const int oldMaximum = pdfBar->maximum();
  QTextCursor append(document);
  append.movePosition(QTextCursor::End);
  append.insertText(QStringLiteral("\n#pagebreak()\nAdditional page after recompilation."));
  QVERIFY2(waitUntil([&]() { return pane->property("hasPendingUpdate").toBool(); }),
           qPrintable(fixture.editor.compileMessage()));
  QCOMPARE(published.count(), 1);
  sourceBar->setValue(sourceBar->maximum() / 2);
  QCOMPARE(pdfBar->value(), mapped(sourceBar, pdfBar));
  QCOMPARE(fixture.selected(), selectedPdf);
  const int beforePublication = sourceBar->value();
  fixture.clearSelection();
  QVERIFY2(waitUntil([&]() { return published.count() == 2; }),
           qPrintable(fixture.editor.compileMessage()));
  QCOMPARE(sourceBar->value(), beforePublication);
  QVERIFY(pdfBar->maximum() > oldMaximum);
  pdfBar->setValue(pdfBar->maximum());
  QCOMPARE(sourceBar->value(), sourceBar->maximum());
}

void TestTypstEditor::scrollSyncWithNonScrollableSource() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  const auto source = QStringLiteral("#set page(width: 320pt, height: 440pt)\n"
                                     "First\n#pagebreak()\nSecond\n#pagebreak()\nThird");
  EditorFixture fixture(m_runtime, root.path(), source, VTypstEditor::Mode::Edit, {}, false, true);
  QSignalSpy published(fixture.pane(), SIGNAL(snapshotPublished(quint64)));
  QVERIFY2(waitUntil([&]() { return published.count() == 1; }),
           qPrintable(fixture.editor.compileMessage()));
  auto *sourceBar = fixture.edit()->verticalScrollBar();
  auto *pdfBar = fixture.pane()->verticalScrollBar();
  QCOMPARE(sourceBar->maximum(), 0);
  QVERIFY(pdfBar->maximum() > 0);
  QTest::keyClick(fixture.pane(), Qt::Key_PageDown);
  QVERIFY(pdfBar->value() > 0);
  QCOMPARE(sourceBar->value(), 0);
  pdfBar->setValue(pdfBar->maximum());
  QCOMPARE(pdfBar->value(), pdfBar->maximum());
  QCOMPARE(fixture.editor.document()->toPlainText(), source);
}

void TestTypstEditor::cursorLocationFlashesThreeTimes() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  const auto source = shortSource(QString::fromUtf8("// \xf0\x9f\x98\x80\nMapped cursor target."));
  const int offset = source.indexOf(QStringLiteral("cursor")) + 2;
  NativeSession native(root.path());
  QVERIFY2(native.start(), qPrintable(native.details()));
  QVERIFY2(native.compile(source), qPrintable(native.details()));
  const auto snapshot = native.reply.header.value(QStringLiteral("snapshot"));
  QVERIFY2(native.request({{QStringLiteral("op"), QStringLiteral("sourceToOutput")},
                           {QStringLiteral("snapshot"), snapshot},
                           {QStringLiteral("file"), QStringLiteral("main.typ")},
                           {QStringLiteral("offset"), offset}}),
           qPrintable(native.details()));
  const auto locations = native.reply.header.value(QStringLiteral("locations")).toArray();
  QVERIFY(!locations.isEmpty());
  const auto location = locations.first().toObject();
  QCOMPARE(location.value(QStringLiteral("page")).toInt(), 0);
  const QPointF point(location.value(QStringLiteral("x")).toDouble(),
                      location.value(QStringLiteral("y")).toDouble());

  EditorFixture fixture(m_runtime, root.path(), source, VTypstEditor::Mode::Edit, {}, false, true);
  auto *pane = fixture.pane();
  QSignalSpy published(pane, SIGNAL(snapshotPublished(quint64)));
  QVERIFY2(waitUntil([&]() { return published.count() == 1; }),
           qPrintable(fixture.editor.compileMessage()));
  const auto selectedPdf = fixture.selectPage();
  QVERIFY(selectedPdf.contains(QStringLiteral("Mapped cursor target.")));
  const auto page = firstPdfPageRect(pane, QSizeF(240, 160));
  const QPointF center = page.topLeft() + point * (page.width() / 240);
  const QRect area = QRectF(center - QPointF(15, 15), QSizeF(30, 30)).toAlignedRect();
  auto frame = [&]() { return widgetImage(pane->viewport()).copy(area); };
  const auto baseline = frame();
  auto *document = fixture.editor.document();
  document->setModified(false);
  const int undoSteps = document->availableUndoSteps();
  const auto *focus = QApplication::focusWidget();
  const int scroll = pane->verticalScrollBar()->value();
  QTextCursor cursor(document);
  cursor.setPosition(offset - 2);
  cursor.setPosition(offset, QTextCursor::KeepAnchor);
  fixture.edit()->setTextCursor(cursor);

  // Observe the actual painted marker, not timer internals or a test-only property.
  int pulses = 0;
  int transitions = 0;
  bool visible = false;
  QElapsedTimer elapsed;
  elapsed.start();
  while (elapsed.elapsed() < 1800) {
    QTest::qWait(20);
    const bool nowVisible = frame() != baseline;
    if (nowVisible != visible) {
      ++transitions;
      pulses += nowVisible ? 1 : 0;
      visible = nowVisible;
    }
  }
  QCOMPARE(pulses, 3);
  QCOMPARE(transitions, 6);
  QVERIFY(!visible);
  QCOMPARE(pane->verticalScrollBar()->value(), scroll);
  QCOMPARE(fixture.selected(), selectedPdf);
  QCOMPARE(fixture.edit()->textCursor().position(), cursor.position());
  QCOMPARE(fixture.edit()->textCursor().anchor(), cursor.anchor());
  QCOMPARE(QApplication::focusWidget(), focus);
  QCOMPARE(document->toPlainText(), source);
  QCOMPARE(document->availableUndoSteps(), undoSteps);
  QVERIFY(!document->isModified());
}

void TestTypstEditor::cursorHighlightFollowsLatestPublication() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  const auto source =
      QStringLiteral("#set page(width: 240pt, height: 700pt, margin: (x: 24pt, y: 100pt))\n") +
      QStringLiteral("// source padding\n").repeated(100) +
      QStringLiteral("First target.\n#pagebreak()\nLast target.");
  EditorFixture fixture(m_runtime, root.path(), source, VTypstEditor::Mode::Edit, {}, false, true);
  auto *pane = fixture.pane();
  QSignalSpy published(pane, SIGNAL(snapshotPublished(quint64)));
  QVERIFY2(waitUntil([&]() { return published.count() == 1; }),
           qPrintable(fixture.editor.compileMessage()));
  const auto selected = fixture.selectPage();
  QVERIFY(selected.contains(QStringLiteral("First target.")));
  QTextCursor cursor(fixture.editor.document());
  cursor.setPosition(source.indexOf(QStringLiteral("First target")) + 2);
  fixture.edit()->setTextCursor(cursor);
  cursor.setPosition(source.indexOf(QStringLiteral("Last target")) + 2);
  fixture.edit()->setTextCursor(cursor);
  // Leave the target off-screen deliberately. Its reveal must not scroll the source back.
  pane->verticalScrollBar()->setValue(0);
  const int sourceScroll = fixture.edit()->verticalScrollBar()->value();
  int page = -1;
  QVERIFY(waitUntil(
      [&]() {
        QMetaObject::invokeMethod(pane, "currentPage", Qt::DirectConnection,
                                  Q_RETURN_ARG(int, page));
        return page == 1;
      },
      paintTimeout));
  QCOMPARE(fixture.edit()->verticalScrollBar()->value(), sourceScroll);
  QCOMPARE(fixture.edit()->textCursor().position(), cursor.position());
  QCOMPARE(fixture.selected(), selected);

  // Invalidate both the pulse and any mapping in flight. The selected PDF stays old.
  QTextCursor append(fixture.editor.document());
  append.movePosition(QTextCursor::End);
  append.insertText(QStringLiteral("\n// recompile without changing page geometry"));
  cursor.setPosition(source.indexOf(QStringLiteral("First target")) + 2);
  fixture.edit()->setTextCursor(cursor);
  QVERIFY(QMetaObject::invokeMethod(pane, "goToPage", Qt::DirectConnection, Q_ARG(int, 0),
                                    Q_ARG(QPointF, QPointF())));
  QVERIFY2(waitUntil([&]() { return pane->property("hasPendingUpdate").toBool(); }),
           qPrintable(fixture.editor.compileMessage()));
  QCOMPARE(published.count(), 1);
  // Exclude the pending-update banner, but include the first target's marker area.
  const QRect content(0, 100, pane->viewport()->width(), pane->viewport()->height() - 100);
  const auto staleFrame = widgetImage(pane->viewport()).copy(content);
  QTest::qWait(400);
  QCOMPARE(widgetImage(pane->viewport()).copy(content), staleFrame);

  // Publication resumes the still-pending cursor movement, rather than losing it at invalidate.
  TypstPdfObserver observer;
  QImage newFrame;
  observer.published = [&](quint64) { newFrame = widgetImage(pane->viewport()); };
  const auto *meta = pane->metaObject();
  QVERIFY(QObject::connect(pane, meta->method(meta->indexOfSignal("snapshotPublished(quint64)")),
                           &observer,
                           observer.metaObject()->method(
                               observer.metaObject()->indexOfSlot("snapshotPublished(quint64)"))));
  fixture.clearSelection();
  QVERIFY2(waitUntil([&]() { return published.count() == 2; }),
           qPrintable(fixture.editor.compileMessage()));
  QVERIFY(waitUntil(
      [&]() { return widgetImage(pane->viewport()).copy(content) != newFrame.copy(content); },
      paintTimeout));
  QCOMPARE(fixture.edit()->textCursor().position(), cursor.position());

  // Leaving side-by-side cancels a visible pulse; it must not restart in Read mode.
  fixture.editor.setMode(VTypstEditor::Mode::Read);
  QTest::qWait(300);
  const auto readFrame = widgetImage(pane->viewport());
  QTest::qWait(1000);
  QCOMPARE(widgetImage(pane->viewport()), readFrame);
  QCOMPARE(fixture.editor.mode(), VTypstEditor::Mode::Read);
}

void TestTypstEditor::headingCursorTargetsTitleInsteadOfOutline() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  const auto source =
      QStringLiteral("#set page(width: 320pt, height: 700pt, margin: 30pt)\n"
                     "#outline()\n#pagebreak()\n= Actual title\nBody below the title.");
  EditorFixture fixture(m_runtime, root.path(), source, VTypstEditor::Mode::Edit, {}, false, true);
  auto *pane = fixture.pane();
  QSignalSpy published(pane, SIGNAL(snapshotPublished(quint64)));
  QVERIFY2(waitUntil([&]() { return published.count() == 1; }),
           qPrintable(fixture.editor.compileMessage()));
  auto *document = fixture.editor.document();
  document->setModified(false);
  const int undoSteps = document->availableUndoSteps();
  QTextCursor cursor(document);
  cursor.setPosition(source.indexOf(QStringLiteral("Actual title")) + 3);
  fixture.edit()->setTextCursor(cursor);
  int page = -1;
  QVERIFY(waitUntil(
      [&]() {
        QMetaObject::invokeMethod(pane, "currentPage", Qt::DirectConnection,
                                  Q_RETURN_ARG(int, page));
        return page == 1;
      },
      paintTimeout));
  QVERIFY(fixture.selectPage().contains(QStringLiteral("Body below the title.")));

  // Explicit navigation shares the same mapping, including a cursor at heading EOL.
  QVERIFY(QMetaObject::invokeMethod(pane, "goToPage", Qt::DirectConnection, Q_ARG(int, 0),
                                    Q_ARG(QPointF, QPointF())));
  cursor.setPosition(source.indexOf(QStringLiteral("\nBody below")));
  fixture.edit()->setTextCursor(cursor);
  fixture.editor.showOutputAtCursor();
  QVERIFY(waitUntil(
      [&]() {
        QMetaObject::invokeMethod(pane, "currentPage", Qt::DirectConnection,
                                  Q_RETURN_ARG(int, page));
        return page == 1;
      },
      paintTimeout));
  QCOMPARE(fixture.edit()->textCursor().position(), cursor.position());
  QCOMPARE(document->toPlainText(), source);
  QCOMPARE(document->availableUndoSteps(), undoSteps);
  QVERIFY(!document->isModified());
}

void TestTypstEditor::inPlaceReservationsDoNotEditSource() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  const auto source =
      QStringLiteral("#set text(fill: red, size: 24pt)\nBefore\n$ x^2 + y^2 = z^2 $\nAfter");
  EditorFixture fixture(m_runtime, root.path(), source, VTypstEditor::Mode::Edit, {}, false, true);
  fixture.edit()->setLineWrapMode(QTextEdit::NoWrap);
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  auto *document = fixture.editor.document();
  document->setModified(false);
  const auto after = document->findBlock(source.indexOf(QStringLiteral("After")));
  QVERIFY(after.isValid());
  const auto top = [&]() { return document->documentLayout()->blockBoundingRect(after).top(); };
  const qreal baseline = top();
  const int undoSteps = document->availableUndoSteps();
  auto selection = fixture.edit()->textCursor();
  selection.setPosition(source.indexOf(QStringLiteral("Before")));
  selection.setPosition(selection.position() + 6, QTextCursor::KeepAnchor);
  fixture.edit()->setTextCursor(selection);
  QSignalSpy changes(document, &QTextDocument::contentsChange);
  const auto sourceSurface = widgetImage(fixture.edit()->viewport());
  fixture.editor.setInplacePreviewEnabled(true);
  fixture.editor.setLivePreviewEnabled(false);
  fixture.editor.setMode(VTypstEditor::Mode::Edit);
  QVERIFY(waitUntil([&]() { return top() > baseline; }, paintTimeout));
  QVERIFY2(waitUntil([&]() {
             auto *card = fixture.editor.findChild<QWidget *>(QStringLiteral("typstPreviewCard"));
             return card && card->isVisible() && coloredPixels(widgetImage(card), true) > 20;
           }),
           qPrintable(fixture.editor.compileMessage()));
  auto *card = fixture.editor.findChild<QWidget *>(QStringLiteral("typstPreviewCard"));
  const QPoint blankPoint(card->geometry().right() - 4, card->geometry().center().y());
  const auto previewSurface = widgetImage(fixture.edit()->viewport());
  // The source pane is narrower before switching out of side-by-side mode.
  const QPoint sourceBlank(sourceSurface.width() - 4, blankPoint.y());
  QCOMPARE(previewSurface.pixelColor(blankPoint), sourceSurface.pixelColor(sourceBlank));
  const auto viewportRect = card->rect();
  for (const auto &point :
       {viewportRect.center(), QPoint(viewportRect.right() - 1, viewportRect.center().y())}) {
    QTest::mouseClick(card, Qt::LeftButton, Qt::NoModifier, point);
    QCoreApplication::processEvents();
    QCOMPARE(fixture.editor.mode(), VTypstEditor::Mode::Edit);
    QCOMPARE(fixture.edit()->textCursor().anchor(), selection.anchor());
    QCOMPARE(fixture.edit()->textCursor().position(), selection.position());
  }
  QCOMPARE(document->toPlainText(), source);
  QCOMPARE(document->availableUndoSteps(), undoSteps);
  QVERIFY(!document->isModified());
  QCOMPARE(textChangeCount(changes), 0);
  fixture.editor.setInplacePreviewEnabled(false);
  fixture.editor.setLivePreviewEnabled(true);
  fixture.editor.setMode(VTypstEditor::Mode::Edit);
  QVERIFY(waitUntil([&]() { return std::abs(top() - baseline) < 1; }, paintTimeout));
  fixture.editor.setInplacePreviewEnabled(true);
  fixture.editor.setLivePreviewEnabled(false);
  fixture.editor.setMode(VTypstEditor::Mode::Edit);
  QVERIFY(waitUntil([&]() { return top() > baseline; }, paintTimeout));
  QCOMPARE(textChangeCount(changes), 0);
  QCOMPARE(document->toPlainText(), source);
}

void TestTypstEditor::occurrencesAndSplitPartsRenderSequentially() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  auto config = editorConfig(root.path(), &m_runtime);
  config->m_previewMaxHeight = 160;
  const auto source = QStringLiteral("#set text(size: 24pt)\n#for color in (rgb(\"#ff0000\"), "
                                     "rgb(\"#0000ff\")) { box(fill: color)[$x$] } $y$\nAfter");
  EditorFixture fixture(m_runtime, root.path(), source, VTypstEditor::Mode::Edit, config, true,
                        false);
  const auto repeated = [&]() -> QWidget * {
    for (auto *entry : fixture.editor.findChildren<QWidget *>(QStringLiteral("typstPreviewCard"))) {
      if (coloredPixels(widgetImage(entry), true) > 10)
        return entry;
    }
    return nullptr;
  };
  const auto inlineCard = [&]() -> QWidget * {
    auto *occurrences = repeated();
    for (auto *entry : fixture.editor.findChildren<QWidget *>(QStringLiteral("typstPreviewCard"))) {
      if (entry != occurrences)
        return entry;
    }
    return nullptr;
  };
  QVERIFY2(
      waitUntil([&]() { return repeated() && coloredPixels(widgetImage(repeated()), false) > 10; }),
      qPrintable(fixture.editor.compileMessage()));
  const auto sourceRect = [&](int p_position) {
    QTextCursor cursor(fixture.editor.document());
    cursor.setPosition(p_position);
    return fixture.edit()->cursorRect(cursor);
  };
  const auto scrollToSource = [&](int p_position) {
    QTextCursor cursor(fixture.editor.document());
    cursor.setPosition(p_position);
    fixture.edit()->setTextCursor(cursor);
    auto *bar = fixture.edit()->horizontalScrollBar();
    const int requested = bar->value() + sourceRect(p_position).left() -
                          qRound(fixture.editor.document()->documentMargin());
    bar->setValue(requested);
    return requested;
  };
  const int equationStart = source.indexOf(QStringLiteral("$x$"));
  QVERIFY(sourceRect(equationStart).left() > fixture.editor.document()->documentMargin() + 10);
  QVERIFY(std::abs(repeated()->geometry().left() - sourceRect(equationStart).left()) <= 1);
  QVERIFY(repeated()->width() >
          sourceRect(equationStart + 3).left() - sourceRect(equationStart).left());
  const auto image = widgetImage(repeated());
  QRect redBounds, blueBounds;
  QVERIFY(coloredPixels(image, true, &redBounds) > 10);
  QVERIFY(coloredPixels(image, false, &blueBounds) > 10);
  QVERIFY(redBounds.right() < blueBounds.left());
  QVERIFY(redBounds.top() <= blueBounds.bottom() && blueBounds.top() <= redBounds.bottom());
  QVERIFY(inlineCard());
  QVERIFY(!repeated()->geometry().intersects(inlineCard()->geometry()));
  const int compactWidth = inlineCard()->width();
  QVERIFY(compactWidth < fixture.edit()->viewport()->width() / 2);
  const int originalViewportWidth = fixture.edit()->viewport()->width();
  QCOMPARE(fixture.editor.document()->toPlainText(), source);

  fixture.editor.resize(1200, 650);
  QCoreApplication::processEvents();
  QVERIFY(waitUntil([&]() {
    return fixture.edit()->viewport()->width() > originalViewportWidth && inlineCard() &&
           repeated() && coloredPixels(widgetImage(repeated()), false) > 10;
  }));
  QCOMPARE(inlineCard()->width(), compactWidth);
  QVERIFY(!repeated()->geometry().intersects(inlineCard()->geometry()));
  QVERIFY(std::abs(repeated()->geometry().left() - sourceRect(equationStart).left()) <= 1);

  const int sourcePrefixWidth = sourceRect(equationStart).left();
  const int fullRowWidth = repeated()->width();
  const int spanWidth = sourceRect(equationStart + 3).left() - sourceRect(equationStart).left();
  const int chromeWidth = fixture.editor.width() - fixture.edit()->viewport()->width();
  const int remaining = (spanWidth + fullRowWidth) / 2;
  // Keep the equation on this visual line while testing its remaining-width fit.
  const auto wrapMode = fixture.edit()->wordWrapMode();
  fixture.edit()->setWordWrapMode(QTextOption::WrapAnywhere);
  fixture.editor.resize(chromeWidth + sourceRect(equationStart).left() + remaining +
                            qRound(fixture.editor.document()->documentMargin()) + 4,
                        650);
  const bool fitted = waitUntil([&]() {
    return repeated() && repeated()->width() < fullRowWidth &&
           coloredPixels(widgetImage(repeated()), false) > 10;
  });
  QString fitState;
  if (!fitted) {
    QDebug state(&fitState);
    state << "editor/viewport/full/span:" << fixture.editor.width()
          << fixture.edit()->viewport()->width() << fullRowWidth << spanWidth
          << "source:" << sourceRect(equationStart);
    for (auto *card : fixture.editor.findChildren<QWidget *>(QStringLiteral("typstPreviewCard"))) {
      const auto image = widgetImage(card);
      state << "card/visible/red/blue:" << card->geometry() << card->isVisible()
            << coloredPixels(image, true) << coloredPixels(image, false);
    }
  }
  QVERIFY2(fitted, qPrintable(fitState));
  QVERIFY(std::abs(repeated()->geometry().left() - sourceRect(equationStart).left()) <= 1);
  QVERIFY(repeated()->width() > spanWidth);
  QVERIFY(repeated()->geometry().right() < fixture.edit()->viewport()->width());

  // Leave a substantial prefix on the continuation at any native font DPI.
  // A fixed window width can wrap directly at the equation instead.
  fixture.edit()->setWordWrapMode(wrapMode);
  fixture.editor.resize(chromeWidth + sourcePrefixWidth * 3 / 4, 650);
  QVERIFY(waitUntil([&]() {
    const auto block = fixture.editor.document()->findBlock(equationStart);
    const auto line = block.layout()->lineForTextPosition(equationStart - block.position());
    return line.lineNumber() > 0 && repeated() &&
           coloredPixels(widgetImage(repeated()), false) > 10 &&
           std::abs(repeated()->geometry().left() - sourceRect(equationStart).left()) <= 1;
  }));
  const auto equationBlock = fixture.editor.document()->findBlock(equationStart);
  const int localStart = equationStart - equationBlock.position();
  const auto sourceLine = equationBlock.layout()->lineForTextPosition(localStart);
  QVERIFY(sourceLine.lineNumber() > 0);
  QVERIFY(sourceLine.textStart() < localStart);
  QVERIFY(sourceRect(equationStart).left() > fixture.editor.document()->documentMargin() + 10);
  QVERIFY(repeated()->width() >
          sourceRect(equationStart + 3).left() - sourceRect(equationStart).left());
  QCOMPARE(fixture.editor.document()->toPlainText(), source);
  fixture.editor.resize(1200, 650);

  // The combined natural row must shrink, not wrap or crop, in a narrow editor.
  const auto wide = QStringLiteral("#set page(width: 3000pt, height: 300pt, margin: 10pt)\n"
                                   "#set text(size: 32pt)\n"
                                   "#for color in (rgb(\"#ff0000\"), rgb(\"#0000ff\")) { "
                                   "text(fill: color)[$x+x+x+x+x+x+x+x+x+x$] }\nAfter");
  fixture.edit()->setLineWrapMode(QTextEdit::NoWrap);
  fixture.editor.setFileOverlay(QStringLiteral("main.typ"), wide);
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  const int wideStart = wide.indexOf(QStringLiteral("$x+"));
  auto *wideScroll = fixture.edit()->horizontalScrollBar();
  const auto rasterState = [&]() {
    QString state;
    QDebug debug(&state);
    debug << "scroll value/max:" << wideScroll->value() << wideScroll->maximum()
          << "cursor/source:" << fixture.edit()->textCursor().position() << sourceRect(wideStart)
          << "viewport/document:" << fixture.edit()->viewport()->size()
          << fixture.editor.document()->documentLayout()->documentSize();
    for (auto *card : fixture.editor.findChildren<QWidget *>(QStringLiteral("typstPreviewCard"))) {
      const auto image = widgetImage(card);
      debug << "card geometry/visible/red/blue:" << card->geometry() << card->isVisible()
            << coloredPixels(image, true) << coloredPixels(image, false);
    }
    return state;
  };
  QVERIFY2(waitUntil([&]() {
             const auto doc = fixture.editor.document();
             const auto block = doc->findBlock(wideStart);
             return wideScroll->maximum() > 0 &&
                    doc->documentLayout()->documentSize().width() >
                        block.layout()->lineAt(0).naturalTextWidth() + 2 * doc->documentMargin();
           }),
           qPrintable(rasterState()));
  const int scrollBefore = wideScroll->value();
  const int maximumBefore = wideScroll->maximum();
  const int scrollRequested = scrollToSource(wideStart);
  const int scrollAfter = wideScroll->value();
  QVERIFY2(
      waitUntil([&]() { return repeated() && coloredPixels(widgetImage(repeated()), false) > 10; }),
      qPrintable(QStringLiteral("scroll before/max/requested/after: %1 %2 %3 %4; %5")
                     .arg(scrollBefore)
                     .arg(maximumBefore)
                     .arg(scrollRequested)
                     .arg(scrollAfter)
                     .arg(rasterState())));
  const auto wideSize = repeated()->size();
  const qreal wideDocumentWidth =
      fixture.editor.document()->documentLayout()->documentSize().width();
  const auto wideSelection = fixture.edit()->textCursor();
  const auto wideUndo = fixture.editor.document()->availableUndoSteps();
  const auto wideModified = fixture.editor.document()->isModified();
  fixture.editor.resize(600, 650);
  QVERIFY(waitUntil([&]() {
    return fixture.editor.document()->documentLayout()->documentSize().width() < wideDocumentWidth;
  }));
  scrollToSource(wideStart);
  QVERIFY(waitUntil(
      [&]() {
        return repeated() && repeated()->width() < wideSize.width() &&
               coloredPixels(widgetImage(repeated()), false) > 10;
      },
      paintTimeout));
  const auto narrowImage = widgetImage(repeated());
  QVERIFY(coloredPixels(narrowImage, true, &redBounds) > 10);
  QVERIFY(coloredPixels(narrowImage, false, &blueBounds) > 10);
  QVERIFY(redBounds.right() < blueBounds.left());
  QVERIFY(redBounds.top() <= blueBounds.bottom() && blueBounds.top() <= redBounds.bottom());
  QVERIFY(repeated()->height() < wideSize.height());
  QVERIFY(repeated()->width() <= fixture.edit()->viewport()->width());
  QVERIFY(std::abs(repeated()->geometry().left() - sourceRect(wideStart).left()) <= 1);
  auto *horizontal = fixture.edit()->horizontalScrollBar();
  const int rowLeft = repeated()->geometry().left();
  const int previousScroll = horizontal->value();
  horizontal->setValue(qMax(0, previousScroll - 20));
  QCoreApplication::processEvents();
  QVERIFY(repeated());
  QCOMPARE(repeated()->geometry().left() - rowLeft, previousScroll - horizontal->value());
  scrollToSource(wideStart);
  QCoreApplication::processEvents();
  QVERIFY(repeated());
  QVERIFY(repeated()->geometry().right() < fixture.edit()->viewport()->width());
  QVERIFY(fixture.editor.document()->documentLayout()->documentSize().width() >=
          horizontal->value() + repeated()->geometry().right());
  QCOMPARE(fixture.editor.document()->toPlainText(), wide);
  QCOMPARE(fixture.edit()->textCursor().anchor(), wideSelection.anchor());
  QCOMPARE(fixture.edit()->textCursor().position(), wideSelection.position());
  QCOMPARE(fixture.editor.document()->availableUndoSteps(), wideUndo);
  QCOMPARE(fixture.editor.document()->isModified(), wideModified);

  // Clip past the short right occurrence while the tall left one is still visible.
  // Individual rectangle bottoms are not a searchable order in a horizontal row.
  const auto unequal =
      QStringLiteral("#for (color, size) in ((rgb(\"#ff0000\"), 72pt), (rgb(\"#0000ff\"), 24pt)) { "
                     "text(fill: color, size: size)[$x$] }\n") +
      QStringLiteral("After\n").repeated(80);
  fixture.editor.resize(1000, 650);
  fixture.editor.setFileOverlay(QStringLiteral("main.typ"), unequal);
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  // Publication restores the previous viewport anchor asynchronously. Wait for
  // the reservation before scrolling to the row that this case will clip.
  QVERIFY(waitUntil([&]() {
    const auto doc = fixture.editor.document();
    return doc->documentLayout()->blockBoundingRect(doc->begin()).height() >
           fixture.edit()->fontMetrics().height() * 2;
  }));
  fixture.edit()->moveCursor(QTextCursor::Start);
  fixture.edit()->verticalScrollBar()->setValue(0);
  scrollToSource(unequal.indexOf(QStringLiteral("$x$")));
  QVERIFY(waitUntil(
      [&]() { return repeated() && coloredPixels(widgetImage(repeated()), false) > 10; }));
  const auto unequalImage = widgetImage(repeated());
  QVERIFY(coloredPixels(unequalImage, true, &redBounds) > 10);
  QVERIFY(coloredPixels(unequalImage, false, &blueBounds) > 10);
  QVERIFY(redBounds.right() < blueBounds.left());
  QVERIFY(redBounds.bottom() > blueBounds.bottom() + 4);
  const auto clipSelection = fixture.edit()->textCursor();
  const auto clipUndo = fixture.editor.document()->availableUndoSteps();
  const auto clipModified = fixture.editor.document()->isModified();
  auto *clipScroll = fixture.edit()->verticalScrollBar();
  const int clipY = (redBounds.bottom() + blueBounds.bottom()) / 2;
  const int clipValue = clipScroll->value() + repeated()->geometry().top() + clipY;
  QVERIFY(clipValue <= clipScroll->maximum());
  clipScroll->setValue(clipValue);
  QVERIFY(waitUntil([&]() {
    return repeated() && repeated()->geometry().top() == 0 &&
           repeated()->height() < unequalImage.height() &&
           coloredPixels(widgetImage(repeated()), false) == 0;
  }));
  QVERIFY(coloredPixels(widgetImage(repeated()), true) > 10);
  QCOMPARE(fixture.editor.document()->toPlainText(), unequal);
  QCOMPARE(fixture.edit()->textCursor().anchor(), clipSelection.anchor());
  QCOMPARE(fixture.edit()->textCursor().position(), clipSelection.position());
  QCOMPARE(fixture.editor.document()->availableUndoSteps(), clipUndo);
  QCOMPARE(fixture.editor.document()->isModified(), clipModified);

  QString split = QStringLiteral("#set page(width: 100mm, height: 30mm, margin: 4mm)\n"
                                 "#set text(fill: red, size: 24pt)\n"
                                 "#show math.equation: set block(breakable: true)\nBefore\n$ ");
  split.reserve(1024);
  for (int row = 1; row <= 32; ++row) {
    if (row > 1)
      split += QStringLiteral(" \\ ");
    split += row == 32 ? QStringLiteral("#text(fill: rgb(\"#0000ff\"))[$x_32$] &= 32")
                       : QStringLiteral("x_%1 &= %1").arg(row);
  }
  split += QStringLiteral(" $\nAfter");
  fixture.edit()->setLineWrapMode(QTextEdit::NoWrap);
  fixture.editor.setFileOverlay(QStringLiteral("main.typ"), split);
  fixture.edit()->horizontalScrollBar()->setValue(0);
  auto *editorScroll = fixture.edit()->verticalScrollBar();
  editorScroll->setValue(0);
  const auto splitCard = [&]() {
    return fixture.editor.findChild<QWidget *>(QStringLiteral("typstPreviewCard"));
  };
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  QVERIFY(waitUntil([&]() {
    return splitCard() && editorScroll->maximum() > 0 &&
           coloredPixels(widgetImage(splitCard()), true) > 5;
  }));
  QCOMPARE(coloredPixels(widgetImage(splitCard()), false), 0);
  const auto selection = fixture.edit()->textCursor();
  const int undoSteps = fixture.editor.document()->availableUndoSteps();
  const bool modified = fixture.editor.document()->isModified();
  QSignalSpy changes(fixture.editor.document(), &QTextDocument::contentsChange);
  const int initialScroll = editorScroll->value();
  const QPoint point = splitCard()->rect().center();
  QWheelEvent wheel(QPointF(point), QPointF(splitCard()->mapToGlobal(point)), QPoint(),
                    QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
  QCoreApplication::sendEvent(splitCard(), &wheel);
  QVERIFY(waitUntil([&]() { return editorScroll->value() > initialScroll; }));

  const auto after = fixture.editor.document()->findBlock(split.indexOf(QStringLiteral("After")));
  QVERIFY(after.isValid());
  const auto afterRect = fixture.editor.document()->documentLayout()->blockBoundingRect(after);
  // Keep the end of the preview visible rather than scrolling into the editor's
  // trailing blank space beyond the final source line.
  editorScroll->setValue(
      qMax(0, int(afterRect.top()) - fixture.edit()->viewport()->height() * 3 / 4));
  QVERIFY2(waitUntil(
               [&]() { return splitCard() && coloredPixels(widgetImage(splitCard()), false) > 5; }),
           "Editor scrolling must reach the final blue page fragment");
  QCOMPARE(fixture.editor.document()->toPlainText(), split);
  QCOMPARE(fixture.edit()->textCursor().anchor(), selection.anchor());
  QCOMPARE(fixture.edit()->textCursor().position(), selection.position());
  QCOMPARE(fixture.editor.document()->availableUndoSteps(), undoSteps);
  QCOMPARE(fixture.editor.document()->isModified(), modified);
  QCOMPARE(textChangeCount(changes), 0);
}

void TestTypstEditor::supersededCompileCannotPublish() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  EditorFixture fixture(m_runtime, root.path(), shortSource(QStringLiteral("Baseline.")));
  QSignalSpy published(fixture.pane(), SIGNAL(snapshotPublished(quint64)));
  QVERIFY2(waitUntil([&]() { return published.count() == 1; }),
           qPrintable(fixture.editor.compileMessage()));
  const QStringList obsoleteInputs{shortSource(QStringLiteral("Obsolete successful output.")),
                                   shortSource(QStringLiteral("#let obsolete = ("))};
  for (int round = 0; round < obsoleteInputs.size(); ++round) {
    int compilingEvents = 0;
    int readyEvents = 0;
    bool replacedInFlight = false;
    const auto output = QStringLiteral("Only newest output %1.").arg(round);
    const auto newest = shortSource(output);
    // On a warm session invalidation emits the first Compiling event. The
    // next event follows the real request write, before any reply is read.
    const auto connection = QObject::connect(
        &fixture.editor, &VTypstEditor::compileStatusChanged, &fixture.editor,
        [&](VTypstEditor::CompileStatus state, const QString &) {
          if (state == VTypstEditor::CompileStatus::Ready)
            ++readyEvents;
          if (state == VTypstEditor::CompileStatus::Compiling && ++compilingEvents == 2) {
            replacedInFlight = true;
            fixture.editor.setFileOverlay(QStringLiteral("main.typ"), newest);
          }
        });
    QSignalSpy diagnostics(&fixture.editor, &VTypstEditor::diagnosticsChanged);
    // One document edit emits one invalidation, unlike setPlainText's
    // separate remove/insert notifications on some Qt versions.
    QTextCursor cursor(fixture.editor.document());
    cursor.beginEditBlock();
    cursor.select(QTextCursor::Document);
    cursor.insertText(obsoleteInputs.at(round));
    cursor.endEditBlock();
    QVERIFY2(waitUntil([&]() { return published.count() == round + 2; }),
             qPrintable(fixture.editor.compileMessage()));
    QObject::disconnect(connection);
    QVERIFY(replacedInFlight);
    QCOMPARE(readyEvents, 1);
    QCOMPARE(fixture.editor.document()->toPlainText(), newest);
    const int plainPosition = newest.indexOf(output);
    QVERIFY(plainPosition >= 0);
    QVERIFY(!syntaxFormatAt(fixture.editor.document(), plainPosition)
                 .hasProperty(QTextFormat::ForegroundBrush));
    auto *highlighter = fixture.editor.document()->findChild<QSyntaxHighlighter *>();
    QVERIFY(highlighter);
    highlighter->rehighlight();
    QCoreApplication::processEvents();
    QVERIFY(!syntaxFormatAt(fixture.editor.document(), plainPosition)
                 .hasProperty(QTextFormat::ForegroundBrush));
    QCOMPARE(fixture.selectPage().trimmed(), output);
    fixture.clearSelection();
    for (const auto &notification : diagnostics)
      QVERIFY(notification.at(0).toStringList().isEmpty());
    QCOMPARE(fixture.editor.compileStatus(), VTypstEditor::CompileStatus::Ready);
    QCOMPARE(published.count(), round + 2);
  }
}

void TestTypstEditor::syntaxHighlightsSurviveRehighlightAndThemeChanges() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  const auto lightPath = root.filePath(QStringLiteral("light.theme"));
  QVERIFY(writeTypstTheme(lightPath, QStringLiteral("#aa2233")));
  auto config = editorConfig(root.path(), &m_runtime);
  config->m_textEditorConfig->m_theme = Theme::createThemeFromFile(lightPath);
  QVERIFY(config->m_textEditorConfig->m_theme);
  config->m_textEditorConfig->m_syntaxTheme = QStringLiteral("Breeze Dark");
  const auto source = QStringLiteral("// comment\n= Heading\n#let value = 42\n"
                                     "#let label = \"string\"\n#text(\"call\")\n"
                                     "*strong* _emph_ `raw`\n$ x^2 $\nplain\n");
  EditorFixture fixture(m_runtime, root.path(), source, VTypstEditor::Mode::Edit, config, false,
                        true);
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  auto *document = fixture.editor.document();
  const int stringPosition = source.indexOf(QStringLiteral("string"));
  const int commentPosition = source.indexOf(QStringLiteral("comment"));
  const int numberPosition = source.indexOf(QStringLiteral("42"));
  const int functionPosition = source.indexOf(QStringLiteral("text("));
  const int headingPosition = source.indexOf(QStringLiteral("Heading"));
  const int strongPosition = source.indexOf(QStringLiteral("strong"));
  const int emphPosition = source.indexOf(QStringLiteral("emph"));
  const int mathPosition = source.indexOf(QLatin1Char('$'));
  const int rawPosition = source.indexOf(QStringLiteral("raw"));
  const int plainPosition = source.indexOf(QStringLiteral("plain"));
  QVERIFY(stringPosition >= 0 && commentPosition >= 0 && numberPosition >= 0 &&
          functionPosition >= 0 && headingPosition >= 0 && strongPosition >= 0 &&
          emphPosition >= 0 && mathPosition >= 0 && plainPosition >= 0);
  QCOMPARE(syntaxFormatAt(document, stringPosition).foreground().color(), QColor("#aa2233"));
  QCOMPARE(syntaxFormatAt(document, commentPosition).foreground().color(), QColor("#557755"));
  QCOMPARE(syntaxFormatAt(document, numberPosition).foreground().color(), QColor("#aa5500"));
  QCOMPARE(syntaxFormatAt(document, functionPosition).foreground().color(), QColor("#663399"));
  QCOMPARE(syntaxFormatAt(document, headingPosition).fontWeight(), int(QFont::Bold));
  QCOMPARE(syntaxFormatAt(document, strongPosition).fontWeight(), int(QFont::Bold));
  QVERIFY(syntaxFormatAt(document, emphPosition).fontItalic());
  QCOMPARE(syntaxFormatAt(document, mathPosition).foreground().color(), QColor("#007777"));
  QCOMPARE(syntaxFormatAt(document, headingPosition).fontPointSize(), 19.0);
  QCOMPARE(syntaxFormatAt(document, stringPosition).fontPointSize(), 13.0);
  QCOMPARE(syntaxFormatAt(document, stringPosition).fontFamily(), QStringLiteral("Consolas"));
  QCOMPARE(syntaxFormatAt(document, stringPosition).background().color(), QColor("#ffeeee"));
  QVERIFY(syntaxFormatAt(document, stringPosition).fontUnderline());
  QVERIFY(syntaxFormatAt(document, stringPosition).fontStrikeOut());
  QVERIFY(!syntaxFormatAt(document, rawPosition).hasProperty(QTextFormat::ForegroundBrush));
  QVERIFY(!syntaxFormatAt(document, plainPosition).hasProperty(QTextFormat::ForegroundBrush));

  auto *highlighter = document->findChild<QSyntaxHighlighter *>();
  QVERIFY(highlighter);
  auto selection = fixture.edit()->textCursor();
  selection.setPosition(stringPosition + 6);
  selection.setPosition(stringPosition, QTextCursor::KeepAnchor);
  fixture.edit()->setTextCursor(selection);
  const auto originalText = document->toPlainText();
  const int anchor = selection.anchor();
  const int position = selection.position();
  const bool modified = document->isModified();
  const int undoSteps = document->availableUndoSteps();
  QSignalSpy changes(document, &QTextDocument::contentsChange);
  for (int repeat = 0; repeat < 2; ++repeat) {
    highlighter->rehighlight();
    QCoreApplication::processEvents();
    QCOMPARE(syntaxFormatAt(document, stringPosition).foreground().color(), QColor("#aa2233"));
    QCOMPARE(syntaxFormatAt(document, commentPosition).foreground().color(), QColor("#557755"));
  }
  QTextCursor cursor(document);
  cursor.beginEditBlock();
  cursor.endEditBlock();
  highlighter->rehighlight();
  QCoreApplication::processEvents();
  QCOMPARE(syntaxFormatAt(document, stringPosition).foreground().color(), QColor("#aa2233"));
  QCOMPARE(syntaxFormatAt(document, commentPosition).foreground().color(), QColor("#557755"));

  const auto darkPath = root.filePath(QStringLiteral("dark.theme"));
  QVERIFY(writeTypstTheme(darkPath, QStringLiteral("#ff8899"), false));
  auto darkConfig = editorConfig(root.path(), &m_runtime);
  darkConfig->m_textEditorConfig->m_theme = Theme::createThemeFromFile(darkPath);
  QVERIFY(darkConfig->m_textEditorConfig->m_theme);
  fixture.editor.setConfig(darkConfig);
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  QVERIFY(waitUntil([&]() {
    return syntaxFormatAt(document, stringPosition).foreground().color() == QColor("#ff8899");
  }));
  highlighter->rehighlight();
  QCoreApplication::processEvents();
  QCOMPARE(syntaxFormatAt(document, stringPosition).foreground().color(), QColor("#ff8899"));
  QCOMPARE(syntaxFormatAt(document, commentPosition).foreground().color(), QColor("#557755"));
  QCOMPARE(syntaxFormatAt(document, rawPosition).foreground().color(), QColor("#886600"));
  QCOMPARE(syntaxFormatAt(document, headingPosition).fontWeight(), int(QFont::Normal));
  QCOMPARE(syntaxFormatAt(document, strongPosition).fontWeight(), int(QFont::Normal));
  QVERIFY(!syntaxFormatAt(document, emphPosition).fontItalic());
  QVERIFY(!syntaxFormatAt(document, stringPosition).fontUnderline());
  QVERIFY(!syntaxFormatAt(document, stringPosition).fontStrikeOut());
  // KSyntaxHighlighting theme changes must not affect Typst elements.
  darkConfig->m_textEditorConfig->m_syntaxTheme = QStringLiteral("Breeze Dark");
  fixture.editor.setConfig(darkConfig);
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  QCOMPARE(syntaxFormatAt(document, stringPosition).foreground().color(), QColor("#ff8899"));
  for (const int delta : {1, -1, 0}) {
    fixture.editor.getTextEditor()->zoom(delta);
    QCoreApplication::processEvents();
    QCOMPARE(syntaxFormatAt(document, stringPosition).foreground().color(), QColor("#ff8899"));
    QCOMPARE(syntaxFormatAt(document, commentPosition).foreground().color(), QColor("#557755"));
    QCOMPARE(syntaxFormatAt(document, headingPosition).fontPointSize(), 19.0 + delta);
    QCOMPARE(syntaxFormatAt(document, stringPosition).fontPointSize(), 13.0 + delta);
    QVERIFY(!syntaxFormatAt(document, commentPosition).hasProperty(QTextFormat::FontPointSize));
  }
  // Dropping the section removes formatting; restoring it needs no source mutation.
  auto unstyled = editorConfig(root.path(), &m_runtime);
  unstyled->m_textEditorConfig->m_theme = Theme::createThemeFromContent(QStringLiteral(
      R"({"metadata": {"type": "vtextedit"}, "editor-styles": {"Text": {"font-size": 11}}})"));
  fixture.editor.setConfig(unstyled);
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  QVERIFY(syntaxFormatAt(document, stringPosition).isEmpty());
  QVERIFY(syntaxFormatAt(document, headingPosition).isEmpty());
  fixture.editor.setConfig(darkConfig);
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  QCOMPARE(syntaxFormatAt(document, stringPosition).foreground().color(), QColor("#ff8899"));
  QCOMPARE(syntaxFormatAt(document, rawPosition).foreground().color(), QColor("#886600"));
  QCOMPARE(document->toPlainText(), originalText);
  QCOMPARE(fixture.edit()->textCursor().anchor(), anchor);
  QCOMPARE(fixture.edit()->textCursor().position(), position);
  QCOMPARE(document->isModified(), modified);
  QCOMPARE(document->availableUndoSteps(), undoSteps);
  QCOMPARE(textChangeCount(changes), 0);
}

void TestTypstEditor::rawCodeHighlightsUseSyntaxTheme() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  const auto editorPath = root.filePath(QStringLiteral("editor.theme"));
  QVERIFY(writeTypstTheme(editorPath, QStringLiteral("#cc2244")));
  QFile editorFile(editorPath);
  QVERIFY(editorFile.open(QIODevice::ReadOnly));
  auto editorTheme = QJsonDocument::fromJson(editorFile.readAll()).object();
  // Text and ordinary String defaults must not overwrite the authored raw base.
  editorTheme.insert(QStringLiteral("editor-styles"),
                     QJsonObject{{QStringLiteral("Text"),
                                  QJsonObject{{QStringLiteral("font-size"), 11},
                                              {QStringLiteral("font-family"), "Arial"},
                                              {QStringLiteral("bold"), false},
                                              {QStringLiteral("italic"), false},
                                              {QStringLiteral("underline"), false},
                                              {QStringLiteral("strike-through"), false},
                                              {QStringLiteral("background-color"), "#ffffff"}}}});
  auto editorStyles = editorTheme.value(QStringLiteral("typst-syntax-styles")).toObject();
  const QJsonObject rawStyle{{QStringLiteral("text-color"), "#886600"},
                             {QStringLiteral("background-color"), "#ddeeff"},
                             {QStringLiteral("font-family"), "Courier New"},
                             {QStringLiteral("font-size"), 17},
                             {QStringLiteral("bold"), true},
                             {QStringLiteral("italic"), true},
                             {QStringLiteral("underline"), true},
                             {QStringLiteral("strike-through"), true}};
  editorStyles.insert(QStringLiteral("raw"), rawStyle);
  editorTheme.insert(QStringLiteral("typst-syntax-styles"), editorStyles);
  const auto themeFromJson = [](const QJsonObject &p_theme) {
    return Theme::createThemeFromContent(QString::fromUtf8(QJsonDocument(p_theme).toJson()));
  };
  const auto copyConfig = [](const QSharedPointer<TypstEditorConfig> &p_config) {
    auto copy = QSharedPointer<TypstEditorConfig>::create(*p_config);
    copy->m_textEditorConfig =
        QSharedPointer<TextEditorConfig>::create(*p_config->m_textEditorConfig);
    return copy;
  };
  const auto syntaxAPath = root.filePath(QStringLiteral("TypstCodeA.theme"));
  const auto syntaxBPath = root.filePath(QStringLiteral("TypstCodeB.theme"));
  QVERIFY(writeCodeSyntaxTheme(syntaxAPath, QStringLiteral("#0033aa"), QStringLiteral("#a00060"),
                               QStringLiteral("#aa2233"), false));
  QVERIFY(writeCodeSyntaxTheme(syntaxBPath, QStringLiteral("#7766bb"), QStringLiteral("#0055cc"),
                               QStringLiteral("#cc5500"), true));
  auto configA = editorConfig(root.path(), &m_runtime);
  configA->m_textEditorConfig->m_theme = themeFromJson(editorTheme);
  configA->m_textEditorConfig->m_syntaxTheme = syntaxAPath;
  QVERIFY(configA->m_textEditorConfig->m_theme);
  auto configB = copyConfig(configA);
  configB->m_textEditorConfig->m_syntaxTheme = syntaxBPath;
  const auto source =
      QStringLiteral("```rust\n/// documentation\n#[derive(Debug)]\nstruct Widget;\n"
                     "fn user() {\n  let ty = String; let typed: u32 = 0;\n"
                     "  let n = 42; let hex = 0xff; let f = 1.5;\n"
                     "  let c = 'a'; let escaped = '\\n'; let raw = r\"raw text\";\n"
                     "  let message = \"rust \u00e9\U0001f600\";\n"
                     "  // ordinary code comment\n  if true { user(); }\n"
                     "  format!(\"{}\", n);\n}\n```\n"
                     "```python\nfrom math import pi\nclass Widget:\n"
                     "    \"\"\"python documentation\"\"\"\n    def size(self):\n"
                     "        value = 0xff + 1.5 + 42\n        if True:\n"
                     "            return len('\u00e9\U0001f600')\n"
                     "assigned = \"\"\"first verbatim\n\nlast verbatim\"\"\"\n```\n"
                     "```python\n# reset comment\nvalue = \"ok\"\n```\n"
                     "```cpp\n#define COUNT 0xff\n#include <vector>\n"
                     "#define TEXT \"directive string\" // directive comment\n"
                     "int answer() { if (true) return 1.5; }\n```\n"
                     "#let outside = \"ordinary\"\n// normal comment\n");
  // Each probe is located by context, not by an ambiguous first occurrence of a token.
  struct Probe {
    const char *context;
    const char *token;
    const char *style;
  };
  const Probe probes[] = {
      {"/// documentation", "documentation", "Documentation"},
      {"#[derive(Debug)]", "derive", "Attribute"},
      {"struct Widget", "Widget", "DataType"},
      {"let ty = String", "String", "DataType"},
      {"fn user", "fn", "Keyword"},
      {"let typed", "let", "Keyword"},
      {"typed: u32", "u32", "Keyword"},
      {"fn user", "user", "Function"},
      {"if true { user();", "user", "Function"},
      {"if true", "if", "ControlFlow"},
      {"if true", "true", "Constant"},
      {"let n = 42", "42", "DecVal"},
      {"let hex = 0xff", "0xff", "BaseN"},
      {"let f = 1.5", "1.5", "Float"},
      {"let c = 'a'", "'a'", "Char"},
      {"let escaped = '\\n'", "\\n", "SpecialChar"},
      {"r\"raw text\"", "raw text", "VerbatimString"},
      {"\"rust \u00e9\U0001f600\"", "rust \u00e9\U0001f600", "String"},
      {"// ordinary code comment", "ordinary code comment", "Comment"},
      {"format!(\"{}\"", "{}", "SpecialString"},
      {"from math import pi", "from", "Import"},
      {"from math import pi", "import", "Import"},
      {"class Widget", "Widget", "DataType"},
      {"\"\"\"python documentation\"\"\"", "python documentation", "Documentation"},
      {"def size(self)", "def", "Keyword"},
      {"def size(self)", "size", "Function"},
      {"value = 0xff", "0xff", "BaseN"},
      {"0xff + 1.5", "1.5", "Float"},
      {"1.5 + 42", "42", "DecVal"},
      {"if True", "if", "ControlFlow"},
      {"if True", "True", "Constant"},
      {"return len", "return", "ControlFlow"},
      {"return len", "len", "BuiltIn"},
      {"len('\u00e9\U0001f600')", "'\u00e9\U0001f600'", "String"},
      {"first verbatim", "first verbatim", "VerbatimString"},
      {"last verbatim", "last verbatim", "VerbatimString"},
      {"# reset comment", "reset comment", "Comment"},
      {"value = \"ok\"", "\"ok\"", "String"},
      {"#define COUNT", "define", "Preprocessor"},
      {"#define COUNT", "COUNT", "Constant"},
      {"COUNT 0xff", "0xff", "BaseN"},
      {"#include <vector>", "include", "Preprocessor"},
      {"<vector>", "vector", "String"},
      {"\"directive string\"", "directive string", "String"},
      {"// directive comment", "directive comment", "Comment"},
      {"if (true)", "if", "ControlFlow"},
      {"return 1.5", "return", "ControlFlow"},
      {"return 1.5", "1.5", "Float"}};
  for (const auto &probe : probes) {
    QVERIFY2(source.contains(QString::fromUtf8(probe.context)), probe.context);
    QVERIFY2(QString::fromUtf8(probe.context).contains(QString::fromUtf8(probe.token)),
             probe.token);
  }
  EditorFixture fixture(m_runtime, root.path(), source, VTypstEditor::Mode::Edit, configA, false,
                        false);
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  auto *document = fixture.editor.document();
  auto *highlighter = document->findChild<QSyntaxHighlighter *>();
  QVERIFY(highlighter);
  const auto at = [&](const QString &p_token) {
    return syntaxFormatAt(document, source.indexOf(p_token));
  };
  const auto probePosition = [&](const Probe &p_probe) {
    const auto context = QString::fromUtf8(p_probe.context);
    return source.indexOf(context) + context.indexOf(QString::fromUtf8(p_probe.token));
  };
  auto selection = fixture.edit()->textCursor();
  selection.setPosition(source.indexOf(QStringLiteral("message")) + 7);
  selection.setPosition(source.indexOf(QStringLiteral("message")), QTextCursor::KeepAnchor);
  fixture.edit()->setTextCursor(selection);
  const auto selected = selection.selectedText();
  const bool modified = document->isModified();
  const int undoSteps = document->availableUndoSteps();
  QSignalSpy changes(document, &QTextDocument::contentsChange);
  const auto checkUnchanged = [&]() {
    QCOMPARE(document->toPlainText(), source);
    QCOMPARE(fixture.edit()->textCursor().selectedText(), selected);
    QCOMPARE(fixture.edit()->textCursor().anchor(), selection.anchor());
    QCOMPARE(fixture.edit()->textCursor().position(), selection.position());
    QCOMPARE(document->isModified(), modified);
    QCOMPARE(document->availableUndoSteps(), undoSteps);
    QCOMPARE(textChangeCount(changes), 0);
  };
  const auto applyConfig = [&](const QSharedPointer<TypstEditorConfig> &p_config) {
    fixture.editor.setConfig(p_config);
    const bool ready = fixture.ready();
    QCoreApplication::processEvents();
    checkUnchanged();
    return ready;
  };
  const auto checkOrdinary = [&](int p_zoom, const QColor &p_color) {
    const auto ordinary = at(QStringLiteral("\"ordinary\""));
    QCOMPARE(ordinary.foreground().color(), p_color);
    QCOMPARE(ordinary.background().color(), QColor("#ffeeee"));
    QCOMPARE(ordinary.fontFamily(), QStringLiteral("Consolas"));
    QCOMPARE(ordinary.fontPointSize(), qreal(13 + p_zoom));
    QCOMPARE(ordinary.fontWeight(), int(QFont::Normal));
    QVERIFY(!ordinary.fontItalic());
    QVERIFY(ordinary.fontUnderline());
    QVERIFY(ordinary.fontStrikeOut());
    const auto comment = at(QStringLiteral("normal comment"));
    QCOMPARE(comment.foreground().color(), QColor("#557755"));
    QCOMPARE(comment.background().color(), QColor("#ffffff"));
    QCOMPARE(comment.fontFamily(), QStringLiteral("Arial"));
    QVERIFY(!comment.hasProperty(QTextFormat::FontPointSize));
  };
  const auto checkPalette = [&](const QString &p_keyword, const QString &p_controlFlow,
                                const QString &p_string, bool p_decorated, int p_zoom,
                                const QString &p_family, int p_size, const QColor &p_rawColor,
                                const QColor &p_background, const QColor &p_ordinaryColor) {
    const QJsonObject colors{{QStringLiteral("Keyword"), p_keyword},
                             {QStringLiteral("ControlFlow"), p_controlFlow},
                             {QStringLiteral("String"), p_string},
                             {QStringLiteral("DataType"), "#007788"},
                             {QStringLiteral("BuiltIn"), "#882288"},
                             {QStringLiteral("Function"), "#663399"},
                             {QStringLiteral("Constant"), "#995500"},
                             {QStringLiteral("DecVal"), "#aa5500"},
                             {QStringLiteral("BaseN"), "#446600"},
                             {QStringLiteral("Float"), "#005599"},
                             {QStringLiteral("Char"), "#993344"},
                             {QStringLiteral("SpecialChar"), "#bb0066"},
                             {QStringLiteral("VerbatimString"), "#337744"},
                             {QStringLiteral("SpecialString"), "#995577"},
                             {QStringLiteral("Comment"), "#557755"},
                             {QStringLiteral("Documentation"), "#227766"},
                             {QStringLiteral("Attribute"), "#775500"},
                             {QStringLiteral("Preprocessor"), "#6655aa"},
                             {QStringLiteral("Import"), "#225599"}};
    for (const auto &probe : probes) {
      const auto style = QString::fromUtf8(probe.style);
      const int start = probePosition(probe);
      const int length = QString::fromUtf8(probe.token).size();
      // Check every UTF-16 position, including quote/base/decimal punctuation and surrogate pairs.
      for (int offset = 0; offset < length; ++offset) {
        const auto format = syntaxFormatAt(document, start + offset);
        QCOMPARE(format.foreground().color(), QColor(colors.value(style).toString()));
        QCOMPARE(format.background().color(),
                 style == QStringLiteral("String") ? QColor("#ffeeee") : p_background);
        QCOMPARE(format.fontFamily(), p_family);
        QCOMPARE(format.fontPointSize(), qreal(p_size + p_zoom));
        QCOMPARE(format.fontWeight(), int(p_decorated ? QFont::Bold : QFont::Normal));
        QCOMPARE(format.fontItalic(), p_decorated);
        QCOMPARE(format.fontUnderline(), p_decorated);
        QCOMPARE(format.fontStrikeOut(), p_decorated);
      }
    }
    for (const auto &token : {QStringLiteral("```"), QStringLiteral("rust\n")}) {
      const auto format = at(token);
      QCOMPARE(format.foreground().color(), p_rawColor);
      QCOMPARE(format.background().color(), p_background);
      QCOMPARE(format.fontFamily(), p_family);
      QCOMPARE(format.fontPointSize(), qreal(p_size + p_zoom));
      QCOMPARE(format.fontWeight(), int(QFont::Bold));
      QVERIFY(format.fontItalic());
      QVERIFY(format.fontUnderline());
      QVERIFY(format.fontStrikeOut());
    }
    checkOrdinary(p_zoom, p_ordinaryColor);
    checkUnchanged();
  };
  const auto checkA = [&](int p_zoom) {
    checkPalette(QStringLiteral("#0033aa"), QStringLiteral("#a00060"), QStringLiteral("#aa2233"),
                 false, p_zoom, QStringLiteral("Courier New"), 17, QColor("#886600"),
                 QColor("#ddeeff"), QColor("#cc2244"));
  };
  checkA(0);
  highlighter->rehighlight();
  QCoreApplication::processEvents();
  checkA(0);
  QVector<QColor> syntaxAColors;
  for (const auto &probe : probes)
    syntaxAColors.append(syntaxFormatAt(document, probePosition(probe)).foreground().color());
  const auto ordinaryFormat = at(QStringLiteral("\"ordinary\""));
  const auto commentFormat = at(QStringLiteral("normal comment"));
  // Syntax-only A -> B -> A switches must preserve ordinary Typst formatting.
  for (const auto &config : {configA, configB, configA}) {
    QVERIFY2(applyConfig(config), qPrintable(fixture.editor.compileMessage()));
    QCOMPARE(at(QStringLiteral("\"ordinary\"")), ordinaryFormat);
    QCOMPARE(at(QStringLiteral("normal comment")), commentFormat);
    for (const int delta : {0, 2, -2, 0}) {
      fixture.editor.getTextEditor()->zoom(delta);
      QCoreApplication::processEvents();
      if (config == configB)
        checkPalette(QStringLiteral("#7766bb"), QStringLiteral("#0055cc"),
                     QStringLiteral("#cc5500"), true, delta, QStringLiteral("Courier New"), 17,
                     QColor("#886600"), QColor("#ddeeff"), QColor("#cc2244"));
      else
        checkA(delta);
    }
  }
  // Change only m_theme: raw typography/background and ordinary tokens change, not the palette.
  auto alternateTheme = editorTheme;
  auto alternateStyles = editorStyles;
  auto alternateRaw = rawStyle;
  alternateRaw.insert(QStringLiteral("font-family"), QStringLiteral("Consolas"));
  alternateRaw.insert(QStringLiteral("font-size"), 21);
  alternateRaw.insert(QStringLiteral("text-color"), QStringLiteral("#997700"));
  alternateRaw.insert(QStringLiteral("background-color"), QStringLiteral("#ccddee"));
  alternateStyles.insert(QStringLiteral("raw"), alternateRaw);
  auto alternateString = alternateStyles.value(QStringLiteral("string")).toObject();
  alternateString.insert(QStringLiteral("text-color"), QStringLiteral("#8844aa"));
  alternateStyles.insert(QStringLiteral("string"), alternateString);
  alternateTheme.insert(QStringLiteral("typst-syntax-styles"), alternateStyles);
  auto alternate = copyConfig(configA);
  alternate->m_textEditorConfig->m_theme = themeFromJson(alternateTheme);
  QVERIFY(alternate->m_textEditorConfig->m_theme);
  QVERIFY2(applyConfig(alternate), qPrintable(fixture.editor.compileMessage()));
  checkPalette(QStringLiteral("#0033aa"), QStringLiteral("#a00060"), QStringLiteral("#aa2233"),
               false, 0, QStringLiteral("Consolas"), 21, QColor("#997700"), QColor("#ccddee"),
               QColor("#8844aa"));
  QVERIFY2(applyConfig(configA), qPrintable(fixture.editor.compileMessage()));
  checkA(0);

  // A partial syntax theme falls back to Normal, not the previous decorated palette.
  const auto partialPath = root.filePath(QStringLiteral("TypstCodePartial.theme"));
  QVERIFY(writeFixture(
      partialPath,
      QJsonDocument(
          QJsonObject{{QStringLiteral("metadata"),
                       QJsonObject{{QStringLiteral("name"), "TypstCodePartial"}}},
                      {QStringLiteral("text-styles"),
                       QJsonObject{{QStringLiteral("Normal"),
                                    QJsonObject{{QStringLiteral("text-color"), "#223344"}}}}}})
          .toJson()));
  auto partial = copyConfig(configA);
  partial->m_textEditorConfig->m_syntaxTheme = partialPath;
  const auto checkPartial = [&](bool p_sized, int p_zoom) {
    for (const auto &probe : probes) {
      const auto format = syntaxFormatAt(document, probePosition(probe));
      QCOMPARE(format.foreground().color(), QColor("#223344"));
      QCOMPARE(format.background().color(), QColor("#ddeeff"));
      QCOMPARE(format.fontFamily(), QStringLiteral("Courier New"));
      QCOMPARE(format.fontWeight(), int(QFont::Normal));
      QVERIFY(!format.fontItalic());
      QVERIFY(!format.fontUnderline());
      QVERIFY(!format.fontStrikeOut());
      if (p_sized) {
        QCOMPARE(format.fontPointSize(), qreal(17 + p_zoom));
      } else {
        QVERIFY(!format.hasProperty(QTextFormat::FontPointSize));
        QCOMPARE(format.font().resolve(document->defaultFont()).pointSizeF(),
                 document->defaultFont().pointSizeF());
      }
    }
    checkOrdinary(p_zoom, QColor("#cc2244"));
    checkUnchanged();
  };
  QVERIFY2(applyConfig(configB), qPrintable(fixture.editor.compileMessage()));
  QVERIFY2(applyConfig(partial), qPrintable(fixture.editor.compileMessage()));
  checkPartial(true, 0);
  auto unsizedTheme = editorTheme;
  auto unsizedStyles = editorStyles;
  auto unsizedRaw = rawStyle;
  unsizedRaw.remove(QStringLiteral("font-size"));
  unsizedStyles.insert(QStringLiteral("raw"), unsizedRaw);
  unsizedTheme.insert(QStringLiteral("typst-syntax-styles"), unsizedStyles);
  auto unsized = copyConfig(partial);
  unsized->m_textEditorConfig->m_theme = themeFromJson(unsizedTheme);
  QVERIFY(unsized->m_textEditorConfig->m_theme);
  QVERIFY2(applyConfig(unsized), qPrintable(fixture.editor.compileMessage()));
  for (const int delta : {0, 2, -2, 0}) {
    fixture.editor.getTextEditor()->zoom(delta);
    QCoreApplication::processEvents();
    checkPartial(false, delta);
  }

  // The inner palette is independent of the presence of authored Typst styles.
  auto unstyledTheme = editorTheme;
  unstyledTheme.remove(QStringLiteral("typst-syntax-styles"));
  auto unstyled = copyConfig(configA);
  unstyled->m_textEditorConfig->m_theme = themeFromJson(unstyledTheme);
  QVERIFY(unstyled->m_textEditorConfig->m_theme);
  QVERIFY2(applyConfig(unstyled), qPrintable(fixture.editor.compileMessage()));
  QVERIFY(at(QStringLiteral("```")).isEmpty());
  QVERIFY(at(QStringLiteral("rust\n")).isEmpty());
  QVERIFY(at(QStringLiteral("\"ordinary\"")).isEmpty());
  QVERIFY(at(QStringLiteral("normal comment")).isEmpty());
  int unstyledIndex = 0;
  for (const auto &probe : probes) {
    const auto format = syntaxFormatAt(document, probePosition(probe));
    QCOMPARE(format.foreground().color(), syntaxAColors.at(unstyledIndex++));
    QCOMPARE(format.background().color(), QString::fromUtf8(probe.style) == QStringLiteral("String")
                                              ? QColor("#ffeeee")
                                              : QColor("#ffffff"));
    QCOMPARE(format.fontFamily(), QStringLiteral("Arial"));
    QCOMPARE(format.fontWeight(), int(QFont::Normal));
    QVERIFY(!format.fontItalic());
    QVERIFY(!format.fontUnderline());
    QVERIFY(!format.fontStrikeOut());
    QVERIFY(!format.hasProperty(QTextFormat::FontPointSize));
  }
  QVERIFY2(applyConfig(configA), qPrintable(fixture.editor.compileMessage()));
  checkA(0);

  // The production constructor, not a global test initializer, registers named syntax themes.
  QFile breezeFile(QStringLiteral(":/org.kde.syntax-highlighting/themes/breeze-dark.theme"));
  QVERIFY(breezeFile.open(QIODevice::ReadOnly));
  QJsonParseError parseError;
  const auto breeze = QJsonDocument::fromJson(breezeFile.readAll(), &parseError);
  QCOMPARE(parseError.error, QJsonParseError::NoError);
  QVERIFY(breeze.isObject());
  const auto breezeStyles = breeze.object().value(QStringLiteral("text-styles")).toObject();
  const QColor breezeKeyword(breezeStyles.value(QStringLiteral("Keyword"))
                                 .toObject()
                                 .value(QStringLiteral("text-color"))
                                 .toString());
  const QColor breezeControlFlow(breezeStyles.value(QStringLiteral("ControlFlow"))
                                     .toObject()
                                     .value(QStringLiteral("text-color"))
                                     .toString());
  QVERIFY(breezeKeyword.isValid());
  QVERIFY(breezeControlFlow.isValid());
  auto named = copyConfig(configA);
  named->m_textEditorConfig->m_syntaxTheme = QStringLiteral("Breeze Dark");
  QVERIFY2(applyConfig(named), qPrintable(fixture.editor.compileMessage()));
  QCOMPARE(at(QStringLiteral("fn user")).foreground().color(), breezeKeyword);
  QCOMPARE(at(QStringLiteral("if true")).foreground().color(), breezeControlFlow);
  checkOrdinary(0, QColor("#cc2244"));
  QCOMPARE(at(QStringLiteral("```")).foreground().color(), QColor("#886600"));

  auto defaultConfig = copyConfig(configA);
  defaultConfig->m_textEditorConfig->m_syntaxTheme.clear();
  QVERIFY2(applyConfig(defaultConfig), qPrintable(fixture.editor.compileMessage()));
  QVector<QTextCharFormat> defaultFormats;
  for (const auto &probe : probes)
    defaultFormats.append(syntaxFormatAt(document, probePosition(probe)));
  QVERIFY(at(QStringLiteral("fn user")).foreground().color() != QColor("#0033aa"));
  QVERIFY(at(QStringLiteral("if true")).foreground().color() != QColor("#a00060"));
  checkOrdinary(0, QColor("#cc2244"));
  for (const auto &selection : {QString(), QStringLiteral("No Such Typst Syntax Theme"),
                                root.filePath(QStringLiteral("missing-code.theme"))}) {
    // Prime with custom B each time so an invalid selection cannot silently keep its cache.
    QVERIFY2(applyConfig(configB), qPrintable(fixture.editor.compileMessage()));
    auto fallback = copyConfig(configA);
    fallback->m_textEditorConfig->m_syntaxTheme = selection;
    QVERIFY2(applyConfig(fallback), qPrintable(fixture.editor.compileMessage()));
    int index = 0;
    for (const auto &probe : probes)
      QCOMPARE(syntaxFormatAt(document, probePosition(probe)), defaultFormats.at(index++));
    checkOrdinary(0, QColor("#cc2244"));
    QCOMPARE(at(QStringLiteral("```")).foreground().color(), QColor("#886600"));
  }
  QVERIFY2(applyConfig(configA), qPrintable(fixture.editor.compileMessage()));
  checkA(0);
}

void TestTypstEditor::rawCodeHighlightsFollowEditsAndErrors() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  const auto path = root.filePath(QStringLiteral("raw.theme"));
  QVERIFY(writeTypstTheme(path, QStringLiteral("#aa2233"), false));
  const auto syntaxPath = root.filePath(QStringLiteral("RawEditCode.theme"));
  QVERIFY(writeCodeSyntaxTheme(syntaxPath, QStringLiteral("#0033aa"), QStringLiteral("#a00060"),
                               QStringLiteral("#aa2233"), false));
  auto config = editorConfig(root.path(), &m_runtime);
  config->m_textEditorConfig->m_syntaxTheme = syntaxPath;
  config->m_textEditorConfig->m_theme = Theme::createThemeFromFile(path);
  QVERIFY(config->m_textEditorConfig->m_theme);
  const auto source =
      QStringLiteral("```rust\nfn main() {\n  /* first\n  let n = 42;\n  second */\n"
                     "  let message = \"\u00e9\U0001f600\";\n  /// documentation\n"
                     "  if true { let value = 42; let base = 0xff; let f = 1.5; }\n}\n```\n");
  EditorFixture fixture(m_runtime, root.path(), source, VTypstEditor::Mode::Edit, config, false,
                        false);
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  auto *document = fixture.editor.document();
  const auto color = [&](const QString &p_token) {
    return syntaxFormatAt(document, document->toPlainText().indexOf(p_token)).foreground().color();
  };
  const auto replace = [&](const QString &p_old, const QString &p_new) {
    const int position = document->toPlainText().indexOf(p_old);
    QVERIFY(position >= 0);
    QTextCursor cursor(document);
    cursor.setPosition(position);
    cursor.setPosition(position + p_old.size(), QTextCursor::KeepAnchor);
    cursor.beginEditBlock();
    cursor.insertText(p_new);
    cursor.endEditBlock();
  };
  const auto checkCategories = [&]() {
    QCOMPARE(color(QStringLiteral("documentation")), QColor("#227766"));
    QCOMPARE(color(QStringLiteral("if true")), QColor("#a00060"));
    QCOMPARE(color(QStringLiteral("true")), QColor("#995500"));
    QCOMPARE(color(QStringLiteral("42; let base")), QColor("#aa5500"));
    QCOMPARE(color(QStringLiteral("0xff")), QColor("#446600"));
    QCOMPARE(color(QStringLiteral("1.5")), QColor("#005599"));
  };
  const auto checkUnknown = [&]() {
    for (const auto &token :
         {QStringLiteral("fn"), QStringLiteral("documentation"), QStringLiteral("if true"),
          QStringLiteral("0xff"), QStringLiteral("1.5"), QStringLiteral("\u00e9\U0001f600")})
      QCOMPARE(color(token), QColor("#886600"));
  };
  const auto checkRust = [&]() {
    checkCategories();
    QCOMPARE(color(QStringLiteral("fn")), QColor("#0033aa"));
    QCOMPARE(color(QStringLiteral("let n")), QColor("#557755"));
    QCOMPARE(color(QStringLiteral("\u00e9\U0001f600")), QColor("#aa2233"));
  };
  checkRust();
  replace(QStringLiteral("rust"), QStringLiteral("unknown-lang"));
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  QCOMPARE(color(QStringLiteral("fn")), QColor("#886600"));
  QCOMPARE(color(QStringLiteral("\u00e9\U0001f600")), QColor("#886600"));
  checkUnknown();
  document->undo();
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  checkRust();
  document->redo();
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  QCOMPARE(color(QStringLiteral("fn")), QColor("#886600"));
  document->undo();
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  replace(QStringLiteral("/*"), QStringLiteral("//"));
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  QCOMPARE(color(QStringLiteral("let n")), QColor("#0033aa"));
  QCOMPARE(color(QStringLiteral("42;\n")), QColor("#aa5500"));
  document->undo();
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  checkRust();
  document->redo();
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  QCOMPARE(color(QStringLiteral("let n")), QColor("#0033aa"));
  QCOMPARE(color(QStringLiteral("42;\n")), QColor("#aa5500"));
  document->undo();
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));

  config->m_debounceMs = 60000;
  fixture.editor.setConfig(config);
  QTextCursor cursor(document);
  cursor.setPosition(0);
  cursor.beginEditBlock();
  cursor.insertText(QStringLiteral("\u00e9\U0001f600\n"));
  cursor.endEditBlock();
  QCoreApplication::processEvents();
  QCOMPARE(fixture.editor.compileStatus(), VTypstEditor::CompileStatus::Compiling);
  // Probe the string inside the block, not the newly inserted Unicode prefix.
  const auto checkShifted = [&]() {
    checkCategories();
    const auto text = document->toPlainText();
    QCOMPARE(color(QStringLiteral("fn")), QColor("#0033aa"));
    QCOMPARE(color(QStringLiteral("let n")), QColor("#557755"));
    QCOMPARE(syntaxFormatAt(document, text.lastIndexOf(QStringLiteral("\u00e9\U0001f600")))
                 .foreground()
                 .color(),
             QColor("#aa2233"));
  };
  checkShifted();
  fixture.editor.retryCompilation();
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  checkShifted();
  document->undo();
  QCoreApplication::processEvents();
  QCOMPARE(document->toPlainText(), source);
  checkRust();
  fixture.editor.retryCompilation();
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  document->redo();
  QCoreApplication::processEvents();
  checkShifted();
  fixture.editor.retryCompilation();
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  document->undo();
  fixture.editor.retryCompilation();
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));

  cursor.movePosition(QTextCursor::End);
  cursor.insertText(QStringLiteral("\n#missing_name"));
  // Remove and restore the language while evaluation fails, proving fresh parsed ranges arrive.
  for (const auto &tag : {QStringLiteral("unknown-lang"), QStringLiteral("rust")}) {
    replace(tag == QStringLiteral("rust") ? QStringLiteral("unknown-lang") : QStringLiteral("rust"),
            tag);
    const auto expectedText = document->toPlainText();
    const int undoSteps = document->availableUndoSteps();
    const bool modified = document->isModified();
    QSignalSpy changes(document, &QTextDocument::contentsChange);
    fixture.editor.retryCompilation();
    QVERIFY(waitUntil(
        [&]() { return fixture.editor.compileStatus() == VTypstEditor::CompileStatus::Error; }));
    QCOMPARE(color(QStringLiteral("fn")),
             tag == QStringLiteral("rust") ? QColor("#0033aa") : QColor("#886600"));
    if (tag == QStringLiteral("rust"))
      checkRust();
    else
      checkUnknown();
    QCOMPARE(document->toPlainText(), expectedText);
    QCOMPARE(document->availableUndoSteps(), undoSteps);
    QCOMPARE(document->isModified(), modified);
    QCOMPARE(textChangeCount(changes), 0);
  }
  replace(QStringLiteral("\n#missing_name"), QString());
  fixture.editor.retryCompilation();
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  QCOMPARE(document->toPlainText(), source);
  checkRust();

  // Replace a real in-flight Rust request with an unknown-language generation.
  config->m_debounceMs = 10;
  fixture.editor.setConfig(config);
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  int compilingEvents = 0;
  int readyEvents = 0;
  bool replacedInFlight = false;
  const auto connection = QObject::connect(
      &fixture.editor, &VTypstEditor::compileStatusChanged, &fixture.editor,
      [&](VTypstEditor::CompileStatus p_state, const QString &) {
        if (p_state == VTypstEditor::CompileStatus::Ready)
          ++readyEvents;
        if (p_state == VTypstEditor::CompileStatus::Compiling && ++compilingEvents == 2) {
          replacedInFlight = true;
          replace(QStringLiteral("rust"), QStringLiteral("unknown-lang"));
        }
      });
  cursor.movePosition(QTextCursor::End);
  cursor.insertText(QStringLiteral("\n"));
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  QObject::disconnect(connection);
  QVERIFY(replacedInFlight);
  QCOMPARE(readyEvents, 1);
  checkUnknown();
  QCOMPARE(color(QStringLiteral("fn")), QColor("#886600"));
  auto *highlighter = document->findChild<QSyntaxHighlighter *>();
  QVERIFY(highlighter);
  highlighter->rehighlight();
  checkUnknown();
  QCOMPARE(color(QStringLiteral("fn")), QColor("#886600"));
  replace(QStringLiteral("unknown-lang"), QStringLiteral("rust"));
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  checkRust();
  QCOMPARE(fixture.editor.compileStatus(), VTypstEditor::CompileStatus::Ready);
}

void TestTypstEditor::lazyHighlightsFollowPendingEdits() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  const auto themePath = root.filePath(QStringLiteral("syntax.theme"));
  QVERIFY(writeTypstTheme(themePath, QStringLiteral("#aa2233")));
  auto config = editorConfig(root.path(), &m_runtime);
  config->m_textEditorConfig->m_theme = Theme::createThemeFromFile(themePath);
  const auto source = QStringLiteral("/* first\nsecond */\n#let label = \"string\"\nplain\n");
  EditorFixture fixture(m_runtime, root.path(), source, VTypstEditor::Mode::Edit, config, true,
                        false);
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  config->m_debounceMs = 60000;
  fixture.editor.setConfig(config);
  auto *document = fixture.editor.document();
  auto *highlighter = document->findChild<QSyntaxHighlighter *>();
  QVERIFY(highlighter);
  const auto checkFormats = [&]() {
    const auto text = document->toPlainText();
    QCOMPARE(fixture.editor.compileStatus(), VTypstEditor::CompileStatus::Compiling);
    QCOMPARE(syntaxFormatAt(document, text.indexOf(QStringLiteral("first"))).foreground().color(),
             QColor("#557755"));
    QCOMPARE(syntaxFormatAt(document, text.indexOf(QStringLiteral("second"))).foreground().color(),
             QColor("#557755"));
    QCOMPARE(syntaxFormatAt(document, text.indexOf(QStringLiteral("string"))).foreground().color(),
             QColor("#aa2233"));
    QVERIFY(syntaxFormatAt(document, text.indexOf(QStringLiteral("plain"))).isEmpty());
  };
  QCoreApplication::processEvents();
  checkFormats();
  QTextCursor cursor(document);
  cursor.setPosition(source.indexOf(QStringLiteral("string")));
  fixture.edit()->setTextCursor(cursor);
  QTest::keyClicks(fixture.edit(), "x");
  QCoreApplication::processEvents();
  checkFormats();
  QCOMPARE(syntaxFormatAt(document, cursor.position()).foreground().color(), QColor("#aa2233"));
  document->undo();
  QCoreApplication::processEvents();
  checkFormats();
  document->redo();
  QCoreApplication::processEvents();
  checkFormats();

  cursor.setPosition(0);
  cursor.insertText(QStringLiteral("\U0001F680\n"));
  QCoreApplication::processEvents();
  checkFormats();
  QVERIFY(syntaxFormatAt(document, 0).isEmpty());
  cursor.setPosition(document->toPlainText().indexOf(QStringLiteral("second")));
  cursor.insertText(QStringLiteral("\n"));
  QCoreApplication::processEvents();
  checkFormats();
  cursor.deletePreviousChar();
  QCoreApplication::processEvents();
  checkFormats();
  highlighter->rehighlight();
  checkFormats();

  const auto darkPath = root.filePath(QStringLiteral("dark.theme"));
  QVERIFY(writeTypstTheme(darkPath, QStringLiteral("#ff8899")));
  auto dark = QSharedPointer<TypstEditorConfig>::create(*config);
  dark->m_textEditorConfig = QSharedPointer<TextEditorConfig>::create(*config->m_textEditorConfig);
  dark->m_textEditorConfig->m_theme = Theme::createThemeFromFile(darkPath);
  fixture.editor.setConfig(dark);
  QCoreApplication::processEvents();
  QCOMPARE(syntaxFormatAt(document, document->toPlainText().indexOf(QStringLiteral("string")))
               .foreground()
               .color(),
           QColor("#ff8899"));

  cursor.setPosition(document->toPlainText().indexOf(QStringLiteral("/*")));
  cursor.deleteChar();
  cursor.deleteChar();
  QCoreApplication::processEvents();
  // Old comment semantics remain until the official parser replaces them.
  const int second = document->toPlainText().indexOf(QStringLiteral("second"));
  QCOMPARE(syntaxFormatAt(document, second).foreground().color(), QColor("#557755"));
  fixture.editor.retryCompilation();
  QVERIFY(waitUntil(
      [&]() { return fixture.editor.compileStatus() != VTypstEditor::CompileStatus::Compiling; }));
  QVERIFY(syntaxFormatAt(document, second).foreground().color() != QColor("#557755"));
}

void TestTypstEditor::lazyPreviewsFollowPendingEdits() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  auto config = editorConfig(root.path(), &m_runtime);
  const auto source = QStringLiteral("#set text(fill: red, size: 24pt)\nBefore\n"
                                     "$x^2$\nBetween\n$ y^2 $\nAfter");
  EditorFixture fixture(m_runtime, root.path(), source, VTypstEditor::Mode::Edit, config, true,
                        false);
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  const auto cards = [&]() {
    auto result = fixture.editor.findChildren<QWidget *>(QStringLiteral("typstPreviewCard"));
    std::sort(result.begin(), result.end(),
              [](QWidget *p_a, QWidget *p_b) { return p_a->y() < p_b->y(); });
    return result;
  };
  QVERIFY(waitUntil([&]() {
    const auto found = cards();
    return found.size() == 2 && coloredPixels(widgetImage(found[0]), true) > 20 &&
           coloredPixels(widgetImage(found[1]), true) > 20;
  }));
  config->m_debounceMs = 60000;
  fixture.editor.setConfig(config);
  QCoreApplication::processEvents();
  QPointer<QWidget> first = cards().value(0);
  QPointer<QWidget> second = cards().value(1);
  QVERIFY(first && second);
  const auto firstImage = widgetImage(first);
  const auto secondImage = widgetImage(second);
  auto *document = fixture.editor.document();
  auto cursor = document->find(QStringLiteral("red"));
  cursor.insertText(QStringLiteral("rgb(\"#0000ff\")"));
  cursor.setPosition(document->toPlainText().indexOf(QStringLiteral("$x")));
  cursor.insertText(QStringLiteral("\U0001F680\n\n"));
  cursor.setPosition(document->toPlainText().indexOf(QStringLiteral("x^2")) + 1);
  fixture.edit()->setTextCursor(cursor);
  QTest::keyClicks(fixture.edit(), "x");
  QCoreApplication::processEvents();
  QCOMPARE(fixture.editor.compileStatus(), VTypstEditor::CompileStatus::Compiling);
  QVERIFY(first && first->isVisible() && second && second->isVisible());
  QCOMPARE(widgetImage(first), firstImage);
  QCOMPARE(widgetImage(second), secondImage);
  cursor.setPosition(document->toPlainText().indexOf(QStringLiteral("$xx")));
  QVERIFY(first->geometry().bottom() < fixture.edit()->cursorRect(cursor).top());
  cursor.setPosition(document->toPlainText().indexOf(QStringLiteral("$ y")));
  QVERIFY(second->geometry().top() > fixture.edit()->cursorRect(cursor).bottom());
  cursor.setPosition(document->toPlainText().indexOf(QStringLiteral("After")));
  QVERIFY(second->geometry().bottom() < fixture.edit()->cursorRect(cursor).top());
  const int undoSteps = document->availableUndoSteps();
  document->undo();
  QCoreApplication::processEvents();
  QVERIFY(first && second);
  QCOMPARE(widgetImage(first), firstImage);
  QCOMPARE(widgetImage(second), secondImage);
  document->redo();
  QCoreApplication::processEvents();
  QCOMPARE(document->availableUndoSteps(), undoSteps);

  cursor = document->find(QStringLiteral("$xx^2$"));
  QVERIFY(!cursor.isNull());
  cursor.removeSelectedText();
  QCoreApplication::processEvents();
  QVERIFY(first.isNull());
  QVERIFY(second && second->isVisible());
  // Removing the first reservation changes fractional raster placement, not output.
  QVERIFY(coloredPixels(widgetImage(second), true) > 20);
  QCOMPARE(coloredPixels(widgetImage(second), false), 0);
  QCOMPARE(cards().size(), 1);
  cursor.movePosition(QTextCursor::End);
  cursor.insertText(QStringLiteral("\n#let broken = ("));
  fixture.editor.retryCompilation();
  QVERIFY(waitUntil(
      [&]() { return fixture.editor.compileStatus() == VTypstEditor::CompileStatus::Error; }));
  QVERIFY(waitUntil([&]() {
    const auto found = cards();
    return found.size() == 1 && found[0]->isVisible() &&
           coloredPixels(widgetImage(found[0]), true) > 20 &&
           coloredPixels(widgetImage(found[0]), false) == 0;
  }));
  cursor = document->find(QStringLiteral("#let broken = ("));
  QVERIFY(!cursor.isNull());
  cursor.removeSelectedText();
  fixture.editor.retryCompilation();
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  QVERIFY(waitUntil([&]() {
    const auto found = cards();
    return found.size() == 1 && coloredPixels(widgetImage(found[0]), false) > 20 &&
           coloredPixels(widgetImage(found[0]), true) == 0;
  }));
}

void TestTypstEditor::rasterHandoffRetainsLastGoodWhileTyping() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  auto source = splitSource();
  source.replace(QStringLiteral("fill: red"), QStringLiteral("fill: red, size: 14pt"));
  EditorFixture fixture(m_runtime, root.path(), source, VTypstEditor::Mode::Edit, {}, true, false);
  fixture.edit()->setLineWrapMode(QTextEdit::NoWrap);
  TypstRasterObserver observer;
  QVERIFY(observeRasters(&fixture.editor, &observer));
  QSet<int> baselinePages;
  observer.completed = [&](quint64, quint64, int p_page, qreal, const QImage &p_image) {
    if (!p_image.isNull())
      baselinePages.insert(p_page);
  };
  QVERIFY2(waitUntil([&]() {
             return baselinePages.size() >= 2 &&
                    previewHasOnlyColor(previewImages(fixture.editor), true);
           }),
           qPrintable(fixture.editor.compileMessage()));
  const auto lastGood = previewImages(fixture.editor);
  auto *document = fixture.editor.document();
  document->setModified(false);
  QSignalSpy changes(document, &QTextDocument::contentsChange);
  QString expectedSource;
  int expectedUndo = 0;
  int expectedChanges = 0;
  int enters = 0;
  int cancellations = 0;
  int obsoleteRequests = 0;
  bool retained = true;
  bool failedRaster = false;
  bool watching = true;
  QSet<quint64> intercepted;
  QSet<int> acceptedPages;
  observer.requested = [&](quint64, quint64 p_snapshot, int p_page, qreal) {
    if (!watching)
      return;
    retained = retained && previewImages(fixture.editor) == lastGood;
    if (intercepted.contains(p_snapshot)) {
      ++obsoleteRequests;
      return;
    }
    if (enters < 3) {
      intercepted.insert(p_snapshot);
      ++enters;
      QTest::keyClick(fixture.edit(), Qt::Key_Return);
      expectedSource += QLatin1Char('\n');
      expectedUndo = document->availableUndoSteps();
      expectedChanges = textChangeCount(changes);
      retained = retained && previewImages(fixture.editor) == lastGood;
    } else {
      acceptedPages.insert(p_page);
    }
  };
  observer.cancelled = [&](quint64, quint64 p_snapshot, int, qreal) {
    if (intercepted.contains(p_snapshot)) {
      ++cancellations;
      retained = retained && previewImages(fixture.editor) == lastGood;
    }
  };
  observer.completed = [&](quint64, quint64, int, qreal, const QImage &p_image) {
    failedRaster = failedRaster || p_image.isNull();
  };
  auto cursor = document->find(QStringLiteral("red"));
  QVERIFY(!cursor.isNull());
  cursor.insertText(QStringLiteral("rgb(\"#0000ff\")"));
  cursor = document->find(QStringLiteral("x &= 1"));
  QVERIFY(!cursor.isNull());
  cursor.insertText(QStringLiteral("omega^2 + y^2 &= 1"));
  cursor.movePosition(QTextCursor::End);
  fixture.edit()->setTextCursor(cursor);
  fixture.edit()->setFocus();
  expectedSource = document->toPlainText();
  QVERIFY2(waitUntil([&]() {
             return enters == 3 && previewHasOnlyColor(previewImages(fixture.editor), false);
           }),
           qPrintable(fixture.editor.compileMessage()));
  watching = false;
  QVERIFY2(retained, "An arriving or partially rasterized snapshot replaced the last-good pixels");
  QCOMPARE(intercepted.size(), 3);
  QCOMPARE(cancellations, 3);
  QCOMPARE(obsoleteRequests, 0);
  QVERIFY(!failedRaster);
  QVERIFY2(acceptedPages.size() >= 2, "The accepted preview must require multiple visible pages");
  const auto accepted = previewImages(fixture.editor);
  QRect oldBounds, newBounds;
  QVERIFY(coloredPixels(lastGood.front(), true, &oldBounds) > 20);
  QVERIFY(coloredPixels(accepted.front(), false, &newBounds) > 20);
  QVERIFY2(newBounds.width() > oldBounds.width() + 10,
           "The new glyph must use its new crop geometry");
  QCOMPARE(document->toPlainText(), expectedSource);
  QCOMPARE(document->availableUndoSteps(), expectedUndo);
  QCOMPARE(textChangeCount(changes), expectedChanges);
  QVERIFY(document->isModified());
  QCOMPARE(fixture.edit()->textCursor().position(), expectedSource.size());
  QCOMPARE(fixture.edit()->textCursor().anchor(), expectedSource.size());
}

void TestTypstEditor::rasterHandoffTransitions_data() {
  QTest::addColumn<int>("transition");
  QTest::newRow("replace-source") << 0;
  QTest::newRow("change-file-identity") << 1;
  QTest::newRow("side-by-side-and-back") << 2;
  QTest::newRow("restart-with-reused-snapshot-id") << 3;
}

void TestTypstEditor::rasterHandoffTransitions() {
  QFETCH(int, transition);
  QTemporaryDir root;
  QVERIFY(root.isValid());
  // Filled math crops remain measurable when native previews downscale small glyphs.
  const auto source =
      shortSource(QStringLiteral("#set text(size: 24pt)\n#box(fill: red)[$ x $]\nAfter"));
  const auto replacement = shortSource(QStringLiteral(
      "#set text(fill: rgb(\"#0000ff\"), size: 24pt)\n$ Omega^3 + Omega^2 + Omega $\nAfter"));
  EditorFixture fixture(m_runtime, root.path(), QString(), VTypstEditor::Mode::Edit, {}, true,
                        false);
  TypstRasterObserver observer;
  quint64 baselineEpoch = 0;
  quint64 baselineSnapshot = 0;
  observer.requested = [&](quint64 p_epoch, quint64 p_snapshot, int, qreal) {
    baselineEpoch = p_epoch;
    baselineSnapshot = p_snapshot;
  };
  // Native show/activation can pump events: subscribe before admitting the source.
  QVERIFY(observeRasters(&fixture.editor, &observer));
  fixture.editor.setFileOverlay(QStringLiteral("main.typ"), source);
  const bool baselineReady = waitUntil([&]() {
    return baselineSnapshot && previewHasOnlyColor(previewImages(fixture.editor), true);
  });
  QString baselineState;
  if (!baselineReady) {
    QDebug state(&baselineState);
    state << fixture.editor.compileMessage() << "epoch/snapshot:" << baselineEpoch
          << baselineSnapshot;
    for (const auto &image : previewImages(fixture.editor))
      state << "card/red/blue:" << image.size() << coloredPixels(image, true)
            << coloredPixels(image, false);
  }
  QVERIFY2(baselineReady, qPrintable(baselineState));
  const auto lastGood = previewImages(fixture.editor);
  auto *document = fixture.editor.document();
  const auto *paneMeta = fixture.pane()->metaObject();
  QSignalSpy published(fixture.pane(),
                       paneMeta->method(paneMeta->indexOfSignal("snapshotPublished(quint64)")));
  QVERIFY(published.isValid());
  QSignalSpy changes(document, &QTextDocument::contentsChange);
  bool armed = true;
  bool retained = true;
  bool cleared = false;
  bool reusedId = false;
  int expectedUndo = 0;
  int expectedChanges = 0;
  int expectedPosition = 0;
  int expectedAnchor = 0;
  QString expectedSource;
  observer.requested = [&](quint64 p_epoch, quint64 p_snapshot, int, qreal) {
    if (!armed) {
      if (transition == 3) {
        retained = retained && previewImages(fixture.editor) == lastGood;
        reusedId = reusedId || (p_epoch > baselineEpoch && p_snapshot == baselineSnapshot);
      }
      return;
    }
    armed = false;
    retained = previewImages(fixture.editor) == lastGood;
    switch (transition) {
    case 0:
      fixture.editor.setFileOverlay(QStringLiteral("main.typ"),
                                    QStringLiteral("Plain replacement."));
      cleared = previewImages(fixture.editor).isEmpty();
      break;
    case 1:
      fixture.editor.setProject(root.path(), QStringLiteral("other.typ"));
      fixture.editor.setActiveFile(QStringLiteral("other.typ"));
      fixture.editor.setFileOverlay(QStringLiteral("other.typ"), replacement);
      cleared = previewImages(fixture.editor).isEmpty();
      break;
    case 2:
      fixture.editor.setInplacePreviewEnabled(false);
      fixture.editor.setLivePreviewEnabled(true);
      fixture.editor.setMode(VTypstEditor::Mode::Edit);
      cleared = previewImages(fixture.editor).isEmpty();
      break;
    case 3:
      fixture.editor.retryCompilation();
      retained = retained && previewImages(fixture.editor) == lastGood;
      break;
    }
    expectedSource = document->toPlainText();
    expectedUndo = document->availableUndoSteps();
    expectedChanges = textChangeCount(changes);
    expectedPosition = fixture.edit()->textCursor().position();
    expectedAnchor = fixture.edit()->textCursor().anchor();
  };
  auto cursor = document->find(QStringLiteral("red"));
  QVERIFY(!cursor.isNull());
  cursor.insertText(QStringLiteral("rgb(\"#0000ff\")"));
  // Edit the equation, not the 'x' in the preceding text rule.
  cursor.setPosition(document->toPlainText().indexOf(QStringLiteral("$ x")) + 2);
  cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
  cursor.insertText(QStringLiteral("z^3 + y^2"));
  fixture.edit()->setTextCursor(cursor);
  QVERIFY2(waitUntil([&]() { return !armed; }), qPrintable(fixture.editor.compileMessage()));
  QVERIFY2(retained, "The pending raster discarded the previous preview before the transition");
  if (transition != 3)
    QVERIFY(cleared);
  if (transition == 0) {
    QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
    QVERIFY(previewImages(fixture.editor).isEmpty());
  } else {
    if (transition == 2) {
      QVERIFY2(waitUntil([&]() { return !published.isEmpty(); }),
               qPrintable(fixture.editor.compileMessage()));
      QCOMPARE(fixture.editor.mode(), VTypstEditor::Mode::Edit);
      QVERIFY(previewImages(fixture.editor).isEmpty());
      fixture.editor.setInplacePreviewEnabled(true);
      fixture.editor.setLivePreviewEnabled(false);
      fixture.editor.setMode(VTypstEditor::Mode::Edit);
    }
    QVERIFY2(waitUntil([&]() { return previewHasOnlyColor(previewImages(fixture.editor), false); }),
             qPrintable(fixture.editor.compileMessage()));
    if (transition == 1)
      QCOMPARE(fixture.editor.activeFile(), QStringLiteral("other.typ"));
    if (transition == 3) {
      QVERIFY2(reusedId, "Retry must exercise a new session epoch reusing its first snapshot ID");
      QVERIFY(retained);
    }
  }
  QCOMPARE(document->toPlainText(), expectedSource);
  QCOMPARE(document->availableUndoSteps(), expectedUndo);
  QCOMPARE(textChangeCount(changes), expectedChanges);
  QCOMPARE(fixture.edit()->textCursor().position(), expectedPosition);
  QCOMPARE(fixture.edit()->textCursor().anchor(), expectedAnchor);
}

void TestTypstEditor::lazyStateClearsOnReplacement_data() {
  QTest::addColumn<int>("replacement");
  QTest::newRow("buffer") << 0;
  QTest::newRow("active-file") << 1;
  QTest::newRow("project") << 2;
  QTest::newRow("config-project") << 3;
}

void TestTypstEditor::lazyStateClearsOnReplacement() {
  QFETCH(int, replacement);
  QTemporaryDir root;
  QVERIFY(root.isValid());
  auto config = editorConfig(root.path(), &m_runtime);
  EditorFixture fixture(m_runtime, root.path(), QStringLiteral("// comment\n$ x^2 $\n"),
                        VTypstEditor::Mode::Edit, config, true, false);
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  QVERIFY(waitUntil(
      [&]() { return fixture.editor.findChild<QWidget *>(QStringLiteral("typstPreviewCard")); }));
  config->m_debounceMs = 60000;
  fixture.editor.setConfig(config);
  // Reapplying the same identity must not clear the pending display.
  fixture.editor.setProject(root.path(), QStringLiteral("main.typ"));
  QCoreApplication::processEvents();
  QVERIFY(fixture.editor.findChild<QWidget *>(QStringLiteral("typstPreviewCard")));
  QVERIFY(!syntaxFormatAt(fixture.editor.document(), 3).isEmpty());
  switch (replacement) {
  case 0:
    fixture.editor.setFileOverlay(QStringLiteral("main.typ"), QStringLiteral("plain new text"));
    break;
  case 1:
    fixture.editor.setActiveFile(QStringLiteral("other.typ"));
    break;
  case 2:
    fixture.editor.setProject(root.path(), QStringLiteral("other.typ"));
    break;
  case 3:
    config->m_projectRoot = root.filePath(QStringLiteral("different"));
    fixture.editor.setConfig(config);
    break;
  }
  QCoreApplication::processEvents();
  QVERIFY(!fixture.editor.findChild<QWidget *>(QStringLiteral("typstPreviewCard")));
  auto *document = fixture.editor.document();
  for (int position = 0; position < document->characterCount() - 1; ++position)
    QVERIFY(syntaxFormatAt(document, position).isEmpty());
}

void TestTypstEditor::syntaxHighlightsFollowEditsAndErrors() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  const auto themePath = root.filePath(QStringLiteral("syntax.theme"));
  QVERIFY(writeTypstTheme(themePath, QStringLiteral("#aa2233")));
  auto config = editorConfig(root.path(), &m_runtime);
  config->m_textEditorConfig->m_theme = Theme::createThemeFromFile(themePath);
  QVERIFY(config->m_textEditorConfig->m_theme);
  const auto suffix = QStringLiteral("\U0001F680") + QStringLiteral(" #text(\"before\")\nplain\n");
  const auto source = QStringLiteral("/* first\nsecond */\n") + suffix;
  const auto uncommented = QStringLiteral(" first\nsecond \n") + suffix;
  EditorFixture fixture(m_runtime, root.path(), source, VTypstEditor::Mode::Edit, config, false,
                        true);
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  auto *document = fixture.editor.document();
  const int firstPosition = source.indexOf(QStringLiteral("first"));
  const int secondPosition = source.indexOf(QStringLiteral("second"));
  const int stringPosition = source.indexOf(QStringLiteral("before"));
  const int closingPosition = source.indexOf(QStringLiteral("*/"));
  const int shiftedSecond = uncommented.indexOf(QStringLiteral("second"));
  const int shiftedString = uncommented.indexOf(QStringLiteral("before"));
  QVERIFY(firstPosition >= 0 && secondPosition >= 0 && stringPosition >= 0 &&
          closingPosition >= 0 && shiftedSecond >= 0 && shiftedString >= 0);
  QCOMPARE(syntaxFormatAt(document, firstPosition).foreground().color(), QColor("#557755"));
  QCOMPARE(syntaxFormatAt(document, secondPosition).foreground().color(), QColor("#557755"));
  QCOMPARE(syntaxFormatAt(document, stringPosition).foreground().color(), QColor("#aa2233"));

  QTextCursor cursor(document);
  cursor.beginEditBlock();
  cursor.setPosition(closingPosition);
  cursor.setPosition(closingPosition + 2, QTextCursor::KeepAnchor);
  cursor.removeSelectedText();
  cursor.setPosition(0);
  cursor.setPosition(2, QTextCursor::KeepAnchor);
  cursor.removeSelectedText();
  cursor.endEditBlock();
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  QCOMPARE(document->toPlainText(), uncommented);
  QVERIFY(syntaxFormatAt(document, shiftedSecond).foreground().color() != QColor("#557755"));
  QCOMPARE(syntaxFormatAt(document, shiftedString).foreground().color(), QColor("#aa2233"));

  document->undo();
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  QCOMPARE(document->toPlainText(), source);
  QCOMPARE(syntaxFormatAt(document, firstPosition).foreground().color(), QColor("#557755"));
  QCOMPARE(syntaxFormatAt(document, secondPosition).foreground().color(), QColor("#557755"));
  QCOMPARE(syntaxFormatAt(document, stringPosition).foreground().color(), QColor("#aa2233"));
  document->redo();
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  QCOMPARE(document->toPlainText(), uncommented);
  QVERIFY(syntaxFormatAt(document, shiftedSecond).foreground().color() != QColor("#557755"));
  QCOMPARE(syntaxFormatAt(document, shiftedString).foreground().color(), QColor("#aa2233"));

  const auto incomplete = QStringLiteral("// retained comment\n#let label = \"still colored\"\n"
                                         "#let broken = (");
  cursor.beginEditBlock();
  cursor.select(QTextCursor::Document);
  cursor.insertText(incomplete);
  cursor.endEditBlock();
  QVERIFY2(waitUntil([&]() {
             return fixture.editor.compileStatus() == VTypstEditor::CompileStatus::Error;
           }),
           qPrintable(fixture.editor.compileMessage()));
  QCOMPARE(document->toPlainText(), incomplete);
  const int retainedPosition = incomplete.indexOf(QStringLiteral("retained comment"));
  const int stillColoredPosition = incomplete.indexOf(QStringLiteral("still colored"));
  QVERIFY(retainedPosition >= 0 && stillColoredPosition >= 0);
  QCOMPARE(syntaxFormatAt(document, retainedPosition).foreground().color(), QColor("#557755"));
  QCOMPARE(syntaxFormatAt(document, stillColoredPosition).foreground().color(), QColor("#aa2233"));

  cursor.beginEditBlock();
  cursor.select(QTextCursor::Document);
  cursor.removeSelectedText();
  cursor.endEditBlock();
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  QVERIFY(document->toPlainText().isEmpty());
  for (auto block = document->begin(); block.isValid(); block = block.next()) {
    for (const auto &range : block.layout()->formats())
      QVERIFY(range.length == 0 || range.format.isEmpty());
  }

  cursor.beginEditBlock();
  cursor.select(QTextCursor::Document);
  cursor.insertText(source);
  cursor.endEditBlock();
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  QCOMPARE(document->toPlainText(), source);
  QCOMPARE(syntaxFormatAt(document, firstPosition).foreground().color(), QColor("#557755"));
  QCOMPARE(syntaxFormatAt(document, secondPosition).foreground().color(), QColor("#557755"));
  QCOMPARE(syntaxFormatAt(document, stringPosition).foreground().color(), QColor("#aa2233"));
}

void TestTypstEditor::pdfVisiblePagesStaySharpAndRecover_data() {
  QTest::addColumn<bool>("sideBySide");
  QTest::newRow("read") << false;
  QTest::newRow("side-by-side") << true;
}

void TestTypstEditor::pdfVisiblePagesStaySharpAndRecover() {
  QFETCH(bool, sideBySide);
  QTemporaryDir root;
  QVERIFY(root.isValid());
  const auto mode = sideBySide ? VTypstEditor::Mode::Edit : VTypstEditor::Mode::Read;
  const auto source = sharpPdfSource();
  auto config = editorConfig(root.path(), &m_runtime);
  config->m_maxCacheBytes = 512 * 1024 * 1024;
  config->m_maxRasterPixels = 16 * 1024 * 1024;
  QSize viewportSize;
  QSize physicalSize;
  QSizeF pagePoints;
  qreal dpr = 0;
  qreal fittedZoom = 0;
  {
    // Measure a real, laid-out PDF before selecting the budget. Recreate the editor
    // afterward so an unrestricted cached image cannot satisfy the sharpness test.
    EditorFixture layout(m_runtime, root.path(), source, mode, config, false, sideBySide);
    auto exports = observePdfExports(&layout.editor);
    QVERIFY(exports && exports->isValid());
    const auto *meta = layout.pane()->metaObject();
    const int signal = meta->indexOfSignal("snapshotPublished(quint64)");
    QVERIFY(signal >= 0);
    QSignalSpy published(layout.pane(), meta->method(signal));
    QVERIFY(published.isValid());
    QVERIFY2(waitUntil([&]() { return published.count() == 1 && !exports->isEmpty(); }),
             qPrintable(layout.editor.compileMessage()));
    Pdf pdf(exports->last().at(1).toByteArray());
    QVERIFY(!pdf.bytes.isEmpty());
    QVERIFY(pdf.load());
    QCOMPARE(pdf.document.pageCount(), 2);
    pagePoints = pdf.size(0);
    QCOMPARE(pdf.size(1), pagePoints);
    QVERIFY(fitFirstPdfPage(layout.pane()));
    viewportSize = layout.pane()->viewport()->size();
    dpr = layout.pane()->viewport()->devicePixelRatioF();
    fittedZoom = layout.pane()->property("zoom").toDouble();
    const auto page = firstPdfPageRect(layout.pane(), pagePoints);
    QVERIFY(qAbs(page.width() - (viewportSize.width() - 32)) < 0.001);
    QVERIFY(page.intersects(QRectF(QPointF(), QSizeF(viewportSize))));
    QVERIFY(page.bottom() + 16 > viewportSize.height());
    QCOMPARE(layout.pane()->verticalScrollBar()->value(), 0);
    physicalSize = physicalPdfSize(page, dpr);
    QVERIFY(physicalSize.width() < 8192 && physicalSize.height() < 8192);
  }
  const qint64 pagePixels =
      qint64(std::ceil((viewportSize.width() - 32) * dpr)) * physicalSize.height();
  // P is ONE visible page at physical resolution, not a share of visible+prefetch
  // or displayed+pending+retiring slots. Bytes are deliberately non-binding.
  const int pixelBudget = int(pagePixels * 3 / 2);
  config->m_maxRasterPixels = pixelBudget;
  QVERIFY(qint64(config->m_maxCacheBytes) > qint64(pixelBudget) * 4 + 32 * 1024 * 1024);
  EditorFixture fixture(m_runtime, root.path(), source, mode, config, false, sideBySide);
  auto *pane = fixture.pane();
  QVERIFY(pane);
  auto exports = observePdfExports(&fixture.editor);
  QVERIFY(exports && exports->isValid());
  TypstPdfObserver observer;
  QList<QImage> publications;
  observer.published = [&](quint64) { publications.append(pane->viewport()->grab().toImage()); };
  const auto *paneMeta = pane->metaObject();
  const int signal = paneMeta->indexOfSignal("snapshotPublished(quint64)");
  const int slot = observer.metaObject()->indexOfSlot("snapshotPublished(quint64)");
  QVERIFY(signal >= 0 && slot >= 0);
  QVERIFY(QObject::connect(pane, paneMeta->method(signal), &observer,
                           observer.metaObject()->method(slot), Qt::DirectConnection));
  QSignalSpy published(pane, paneMeta->method(signal));
  QVERIFY(published.isValid());
  QVERIFY(fitFirstPdfPage(pane));
  QVERIFY2(waitUntil([&]() { return publications.size() == 1 && !exports->isEmpty(); }),
           qPrintable(fixture.editor.compileMessage()));
  QCOMPARE(pane->viewport()->size(), viewportSize);
  QCOMPARE(pane->viewport()->devicePixelRatioF(), dpr);
  QCOMPARE(pane->property("zoom").toDouble(), fittedZoom);
  QCOMPARE(pane->verticalScrollBar()->value(), 0);
  QCOMPARE(pane->horizontalScrollBar()->value(), 0);
  Pdf original(exports->last().at(1).toByteArray());
  QVERIFY(original.load());
  QCOMPARE(original.document.pageCount(), 2);
  QCOMPARE(original.size(0), pagePoints);
  const auto page = firstPdfPageRect(pane, pagePoints);
  QCOMPARE(physicalPdfSize(page, dpr), physicalSize);
  int currentPage = -1;
  QVERIFY(QMetaObject::invokeMethod(pane, "currentPage", Qt::DirectConnection,
                                    Q_RETURN_ARG(int, currentPage)));
  QCOMPARE(currentPage, 0);
  // With equal portrait pages and scroll offset zero, page 1 begins below the
  // viewport. It is lookahead only and must not divide page 0's raster allowance.
  QVERIFY(page.translated(0, page.height() + 16).top() > viewportSize.height());
  const auto interior = pdfInkInterior(page, pagePoints, publications.front());
  QVERIFY(!interior.isEmpty());
  const auto fullOriginal = paintPdfOracle(original, physicalSize, page, publications.front());
  QVERIFY(!fullOriginal.isNull());
  constexpr qreal fullTolerance = 3;
  const auto initial = pdfInkDifference(publications.front(), fullOriginal, interior);
  QVERIFY2(initial.pixels > 200, "The oracle must compare meaningful text/math, not blank paper");
  QVERIFY2(
      initial.meanError <= fullTolerance,
      qPrintable(QStringLiteral("Initial visible page is softer than physical-resolution PDF: %1")
                     .arg(initial.meanError)));
  // Prove the metric rejects a plausible half-area raster instead of averaging
  // its blurred edges away among the page's many white pixels.
  const QSize halfAreaSize(qMax(1, int(std::floor(physicalSize.width() / std::sqrt(2.0)))),
                           qMax(1, int(std::floor(physicalSize.height() / std::sqrt(2.0)))));
  const auto halfOriginal = paintPdfOracle(original, halfAreaSize, page, publications.front());
  QVERIFY(!halfOriginal.isNull());
  QVERIFY(pdfInkDifference(halfOriginal, fullOriginal, interior).meanError > 2 * fullTolerance);

  auto *document = fixture.editor.document();
  document->setModified(false);
  QSignalSpy changes(document, &QTextDocument::contentsChange);
  const auto selected = fixture.selectPage();
  QVERIFY(selected.contains(QStringLiteral("Sharp text")));
  const auto selectedFrame = pane->viewport()->grab().toImage().copy(interior);
  auto color = document->find(QStringLiteral("fill: red"));
  QVERIFY(!color.isNull());
  color.insertText(QStringLiteral("fill: rgb(\"#0000ff\")"));
  auto sourceSelection = document->find(QStringLiteral("Sharp text"));
  QVERIFY(!sourceSelection.isNull());
  fixture.edit()->setTextCursor(sourceSelection);
  const auto expectedSource = document->toPlainText();
  const int expectedUndo = document->availableUndoSteps();
  const int expectedChanges = textChangeCount(changes);
  const int expectedPosition = sourceSelection.position();
  const int expectedAnchor = sourceSelection.anchor();
  const qreal expectedZoom = pane->property("zoom").toDouble();
  QVERIFY2(waitUntil([&]() {
             return exports->size() >= 2 && pane->property("hasPendingUpdate").toBool();
           }),
           qPrintable(fixture.editor.compileMessage()));
  Pdf replacement(exports->last().at(1).toByteArray());
  QVERIFY(replacement.load());
  QCOMPARE(replacement.document.pageCount(), 2);
  QCOMPARE(replacement.size(0), pagePoints);
  QCOMPARE(fixture.selected(), selected);
  QCOMPARE(published.count(), 1);
  // The old full-P image stays displayed while selection pins its snapshot.
  // Compare the selected frame: the platform's selection tint changes glyph colors.
  const auto retained = pane->viewport()->grab().toImage().copy(interior);
  QCOMPARE(retained, selectedFrame);
  const auto fullReplacement =
      paintPdfOracle(replacement, physicalSize, page, publications.front());
  QVERIFY(!fullReplacement.isNull());
  fixture.clearSelection();
  QVERIFY2(waitUntil([&]() { return publications.size() == 2; }),
           qPrintable(fixture.editor.compileMessage()));
  const auto firstReplacement = publications.at(1).copy(interior);
  QVERIFY(coloredPixels(firstReplacement, false) > 20);
  QCOMPARE(coloredPixels(firstReplacement, true), 0);
  const auto temporary = pdfInkDifference(publications.at(1), fullReplacement, interior);
  QVERIFY2(temporary.meanError > 2 * fullTolerance,
           "The publication frame must be a temporary fallback while old full-P output is pinned");

  // No edit, zoom, configuration change, selection change, or explicit refresh
  // follows publication: freeing the old snapshot must itself restore sharpness.
  PdfInkDifference recovered;
  QVERIFY2(waitUntil(
               [&]() {
                 recovered = pdfInkDifference(pane->viewport()->grab().toImage(), fullReplacement,
                                              interior);
                 return recovered.pixels > 200 && recovered.meanError <= fullTolerance;
               },
               paintTimeout),
           qPrintable(QStringLiteral("Published fallback never recovered physical resolution: %1")
                          .arg(recovered.meanError)));
  QCOMPARE(published.count(), 2);
  QCOMPARE(fixture.selected(), QString());
  QCOMPARE(pane->property("zoom").toDouble(), expectedZoom);
  QCOMPARE(pane->viewport()->size(), viewportSize);
  QCOMPARE(document->toPlainText(), expectedSource);
  QCOMPARE(document->availableUndoSteps(), expectedUndo);
  QCOMPARE(textChangeCount(changes), expectedChanges);
  QCOMPARE(fixture.edit()->textCursor().position(), expectedPosition);
  QCOMPARE(fixture.edit()->textCursor().anchor(), expectedAnchor);
  QVERIFY(document->isModified());
}

void TestTypstEditor::pdfBudgetFailurePreservesOutputAndRecovers() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  EditorFixture fixture(m_runtime, root.path(),
                        shortSource(QStringLiteral("Preserved across export failure.")));
  QSignalSpy published(fixture.pane(), SIGNAL(snapshotPublished(quint64)));
  QVERIFY2(waitUntil([&]() { return published.count() == 1; }),
           qPrintable(fixture.editor.compileMessage()));
  QCOMPARE(fixture.selectPage().trimmed(), QStringLiteral("Preserved across export failure."));
  auto restricted = QSharedPointer<TypstEditorConfig>::create(fixture.editor.getConfig());
  restricted->m_maxPdfBytes = 64;
  fixture.editor.setConfig(restricted);
  QVERIFY2(waitUntil([&]() {
             return fixture.editor.compileStatus() == VTypstEditor::CompileStatus::Error;
           }),
           qPrintable(fixture.editor.compileMessage()));
  QCOMPARE(published.count(), 1);
  QCOMPARE(fixture.selected().trimmed(), QStringLiteral("Preserved across export failure."));
  fixture.clearSelection();
  QCOMPARE(fixture.selectPage().trimmed(), QStringLiteral("Preserved across export failure."));
  fixture.clearSelection();
  auto restored = QSharedPointer<TypstEditorConfig>::create(*restricted);
  restored->m_maxPdfBytes = 16 * 1024 * 1024;
  fixture.editor.setConfig(restored);
  fixture.editor.setFileOverlay(QStringLiteral("main.typ"),
                                shortSource(QStringLiteral("Export recovered.")));
  QVERIFY2(waitUntil([&]() { return published.count() == 2; }),
           qPrintable(fixture.editor.compileMessage()));
  QCOMPARE(fixture.selectPage().trimmed(), QStringLiteral("Export recovered."));
}

void TestTypstEditor::resourceRequestsAcrossDocumentReset_data() {
  QTest::addColumn<bool>("reset");
  QTest::newRow("ordinary-edit-keeps-inflight-displayed-image") << false;
  QTest::newRow("document-reset-cancels-inflight-and-queued-images") << true;
}

void TestTypstEditor::resourceRequestsAcrossDocumentReset() {
  QFETCH(bool, reset);
  QTemporaryDir root;
  QVERIFY(root.isValid());
  const auto path = root.filePath(QStringLiteral("picture.png"));
  QByteArray red, blue;
  QVERIFY(writeResourceImage(path, Qt::red, &red));
  const auto source =
      shortSource(QStringLiteral("Held selection\n\n#image(\"picture.png\", width: 60pt)"));
  EditorFixture fixture(m_runtime, root.path(), source, VTypstEditor::Mode::Read, {}, false, false);
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  QRect bounds;
  QVERIFY(waitUntil(
      [&] { return coloredPixels(widgetImage(fixture.pane()->viewport()), true, &bounds) > 20; }));
  const auto context = resourceMenuAt(fixture.editor, fixture.pane()->viewport(), bounds.center());
  QVERIFY(context.m_imageId);
  QVERIFY(!fixture.selectPage().isEmpty());
  QSignalSpy data(&fixture.editor, &VTypstEditor::imageDataReady);
  fixture.editor.requestImageData(context, 1);
  QVERIFY(waitUntil([&] { return data.count() == 1; }));
  QCOMPARE(data.first()[1].toByteArray(), red);

  // Delivery is queued: retire/reset before returning to the GUI event loop.
  // The first request is admitted, while the second is still bridge-queued.
  fixture.editor.requestImageData(context, 2);
  fixture.editor.requestImageData(context, 3);
  QCOMPARE(data.count(), 1);
  if (reset) {
    fixture.editor.resetForNewDocument();
    QVERIFY(!fixture.editor.isResourceContextValid(context));
    QCOMPARE(fixture.editor.pdfPageCount(), 0);
    QVERIFY(writeResourceImage(path, Qt::blue, &blue));
    fixture.editor.setProject(root.path(), QStringLiteral("B.typ"));
    fixture.editor.setActiveFile(QStringLiteral("B.typ"));
    fixture.editor.getTextEditor()->setText(source);
    fixture.editor.finishDocumentReset();
  } else {
    fixture.editor.setFileOverlay(QStringLiteral("main.typ"),
                                  source + QStringLiteral("\nNew source"));
    QVERIFY(fixture.editor.isResourceContextValid(context));
  }
  QVERIFY(waitUntil([&] { return data.count() == 3; }));
  for (const auto &reply : data) {
    const auto id = reply[0].toULongLong();
    if (id == 1)
      continue;
    if (id == 2 && !reset) {
      QCOMPARE(reply[1].toByteArray(), red);
      QVERIFY(reply[3].toString().isEmpty());
    } else {
      QVERIFY(reply[1].toByteArray().isEmpty());
      QVERIFY(!reply[3].toString().isEmpty());
    }
  }
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  QCOMPARE(data.count(), 3);
  if (reset) {
    QVERIFY(waitUntil([&] {
      return coloredPixels(widgetImage(fixture.pane()->viewport()), false, &bounds) > 20;
    }));
    const auto incoming =
        resourceMenuAt(fixture.editor, fixture.pane()->viewport(), bounds.center());
    QVERIFY(incoming.m_imageId);
    QVERIFY(!fixture.editor.isResourceContextValid(context));
    fixture.editor.requestImageData(incoming, 4);
    QVERIFY(waitUntil([&] { return data.count() == 4; }));
    QCOMPARE(data.last()[1].toByteArray(), blue);
    QVERIFY(data.last()[3].toString().isEmpty());
    QCOMPARE(fixture.editor.document()->toPlainText(), source);
  }
}

void TestTypstEditor::documentResetInvalidationCanDestroyEditor() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  auto fixture = std::make_unique<EditorFixture>(m_runtime, root.path(),
                                                 shortSource(QStringLiteral("Old document")));
  QVERIFY(fixture->ready());
  QPointer<VTypstEditor> editor(&fixture->editor);
  bool invalidated = false;
  connect(editor, &VTypstEditor::resourceContextsInvalidated, this, [&] {
    if (!invalidated) {
      invalidated = true;
      fixture.reset();
    }
  });
  editor->resetForNewDocument();
  QVERIFY(invalidated);
  QVERIFY(editor.isNull());
}

void TestTypstEditor::documentResetClearsPublishedState() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  QFile dependency(root.filePath(QStringLiteral("dependency.typ")));
  QVERIFY(dependency.open(QIODevice::WriteOnly));
  QVERIFY(dependency.write("Dependency A.") > 0);
  dependency.close();
  auto config = editorConfig(root.path(), &m_runtime);
  const auto pageSetup = QStringLiteral("#set page(width: 240pt, height: 700pt, margin: 16pt)\n");
  const auto source =
      pageSetup +
      QStringLiteral("= Heading A\n#include \"dependency.typ\"\n$ x^2 $\n#pagebreak()\nSearch A.");
  EditorFixture fixture(m_runtime, root.path(), source, VTypstEditor::Mode::Edit, config, true,
                        true);
  QSignalSpy published(fixture.pane(), SIGNAL(snapshotPublished(quint64)));
  QVERIFY2(waitUntil([&]() {
             return published.count() == 1 && !previewImages(fixture.editor).isEmpty();
           }),
           qPrintable(fixture.editor.compileMessage()));
  auto *sourceEditor = fixture.editor.getTextEditor();
  auto *document = fixture.editor.document();
  auto *pane = fixture.pane();
  auto *splitter = fixture.editor.findChild<QSplitter *>();
  QVERIFY(splitter);
  const auto *bridge = compilerBridge(&fixture.editor);
  QVERIFY(bridge);
  QSignalSpy generations(bridge, SIGNAL(generationChanged(quint64, quint64)));
  QVERIFY(generations.isValid());
  fixture.editor.setPdfZoom(1.4);
  splitter->setSizes({600, 400});
  const auto sizes = splitter->sizes();
  fixture.editor.setPdfSearchText(QStringLiteral("Search A"));
  fixture.editor.goToPdfPage(1);
  QCOMPARE(fixture.editor.currentPdfPage(), 1);
  QVERIFY(fixture.selectPage().contains(QStringLiteral("Search A")));
  QTextCursor cursor(document);
  cursor.movePosition(QTextCursor::End);
  cursor.insertText(QStringLiteral("\n#let broken_A = ("));
  QVERIFY(waitUntil(
      [&]() { return fixture.editor.compileStatus() == VTypstEditor::CompileStatus::Error; }));
  QVERIFY(!fixture.editor.diagnostics().isEmpty());
  QVERIFY(document->isUndoAvailable());
  const auto zoom = fixture.editor.pdfZoom();
  const auto zoomMode = fixture.editor.pdfZoomMode();
  fixture.editor.resetForNewDocument();
  QCOMPARE(fixture.editor.compileStatus(), VTypstEditor::CompileStatus::Idle);
  QCOMPARE(fixture.editor.pdfPageCount(), 0);
  QCOMPARE(fixture.editor.currentPdfPage(), -1);
  QVERIFY(!fixture.editor.hasPendingPdfUpdate());
  QVERIFY(fixture.selected().isEmpty());
  QVERIFY(fixture.editor.diagnostics().isEmpty());
  QVERIFY(fixture.editor.getHeadings().isEmpty());
  QVERIFY(document->isEmpty());
  QVERIFY(!document->isUndoAvailable());
  QVERIFY(!document->isRedoAvailable());
  QCOMPARE(fixture.edit()->textCursor().position(), 0);
  QCOMPARE(fixture.edit()->textCursor().anchor(), 0);
  QCOMPARE(pane->verticalScrollBar()->value(), 0);
  QCOMPARE(pane->horizontalScrollBar()->value(), 0);
  QVERIFY(fixture.editor.findChildren<QWidget *>(QStringLiteral("typstPreviewCard")).isEmpty());
  QCOMPARE(fixture.editor.mode(), VTypstEditor::Mode::Edit);
  QVERIFY(fixture.editor.isInplacePreviewEnabled());
  QVERIFY(fixture.editor.isLivePreviewEnabled());
  QCOMPARE(fixture.editor.pdfZoom(), zoom);
  QCOMPARE(fixture.editor.pdfZoomMode(), zoomMode);
  QCOMPARE(splitter->sizes(), sizes);

  // An old dependency event, Retry and individual identity/source setters must not
  // reopen admission. A host may fail its source load at any of these boundaries.
  generations.clear();
  QVERIFY(dependency.open(QIODevice::WriteOnly | QIODevice::Truncate));
  QVERIFY(dependency.write("Changed dependency A.") > 0);
  dependency.close();
  fixture.editor.retryCompilation();
  fixture.editor.setProject(root.path(), QStringLiteral("B.typ"));
  QTest::qWait(100);
  QCOMPARE(generations.count(), 0);
  QCOMPARE(fixture.editor.compileStatus(), VTypstEditor::CompileStatus::Idle);
  fixture.editor.setActiveFile(QStringLiteral("B.typ"));
  sourceEditor->setText(QStringLiteral("#let broken_B = ("));
  QTest::qWait(100);
  QCOMPARE(generations.count(), 0);
  QCOMPARE(fixture.editor.compileStatus(), VTypstEditor::CompileStatus::Idle);
  for (int position = 0; position < document->characterCount() - 1; ++position)
    QVERIFY(syntaxFormatAt(document, position).isEmpty());
  fixture.editor.finishDocumentReset();
  QVERIFY(waitUntil(
      [&]() { return fixture.editor.compileStatus() == VTypstEditor::CompileStatus::Error; }));
  QCOMPARE(published.count(), 1);
  QCOMPARE(fixture.editor.pdfPageCount(), 0);
  QCOMPARE(fixture.selectPage(), QString());
  QVERIFY(fixture.editor.getHeadings().isEmpty());
  QVERIFY(fixture.editor.findChildren<QWidget *>(QStringLiteral("typstPreviewCard")).isEmpty());
  QVERIFY(fixture.editor.diagnostics().join(QLatin1Char('\n')).contains(QStringLiteral("B.typ")));
  QVERIFY(
      !fixture.editor.diagnostics().join(QLatin1Char('\n')).contains(QStringLiteral("main.typ")));

  const auto incoming = pageSetup + QStringLiteral("= Heading B\nOnly B.\n#pagebreak()\nSearch A.");
  sourceEditor->setText(incoming);
  QVERIFY2(waitUntil([&]() { return published.count() == 2; }),
           qPrintable(fixture.editor.compileMessage()));
  QCOMPARE(fixture.editor.currentPdfPage(), 0);
  fixture.editor.findNextPdfMatch();
  QTest::qWait(50);
  QCOMPARE(fixture.editor.currentPdfPage(), 0);
  QVERIFY(fixture.selectPage().contains(QStringLiteral("Only B")));
  QVERIFY(!fixture.selected().contains(QStringLiteral("Heading A")));
  QCOMPARE(fixture.editor.getHeadings().size(), 1);
  QCOMPARE(fixture.editor.getHeadings().first().m_title, QStringLiteral("Heading B"));
  QCOMPARE(fixture.editor.getTextEditor(), sourceEditor);
  QCOMPARE(fixture.editor.document(), document);
  QCOMPARE(fixture.pane(), pane);
  QCOMPARE(compilerBridge(&fixture.editor), bridge);
  QCOMPARE(fixture.editor.getConfig().m_runtime, &m_runtime);
  QCOMPARE(fixture.editor.pdfZoom(), zoom);
  QCOMPARE(fixture.editor.pdfZoomMode(), zoomMode);
  QCOMPARE(splitter->sizes(), sizes);
}

void TestTypstEditor::documentResetRetiresPendingPdf() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  EditorFixture fixture(m_runtime, root.path(), shortSource(QStringLiteral("Published A.")));
  QSignalSpy published(fixture.pane(), SIGNAL(snapshotPublished(quint64)));
  QVERIFY2(waitUntil([&]() { return published.count() == 1; }),
           qPrintable(fixture.editor.compileMessage()));
  auto *pane = fixture.pane();
  auto *source = fixture.editor.getTextEditor();
  fixture.editor.fitPdfWidth();
  fixture.editor.setInplacePreviewEnabled(false);
  fixture.editor.setLivePreviewEnabled(true);
  for (int round = 0; round < 3; ++round) {
    QVERIFY(!fixture.selectPage().isEmpty());
    source->setText(shortSource(QStringLiteral("= Pending A %1\nNever publish this.").arg(round)));
    QVERIFY2(waitUntil([&]() { return fixture.editor.hasPendingPdfUpdate(); }),
             qPrintable(fixture.editor.compileMessage()));
    QCOMPARE(fixture.editor.getHeadings().size(), 1);
    // Resize schedules fresh work against both displayed and selection-held slots.
    fixture.editor.resize(1000 + round * 30, 650 + round * 20);
    QCoreApplication::processEvents();
    const int before = published.count();
    fixture.editor.resetForNewDocument();
    // Repeated reset before renderer completions must not destroy their devices.
    fixture.editor.resetForNewDocument();
    QCOMPARE(fixture.editor.pdfPageCount(), 0);
    QVERIFY(!fixture.editor.hasPendingPdfUpdate());
    QVERIFY(fixture.selected().isEmpty());
    QVERIFY(fixture.editor.getHeadings().isEmpty());
    QCOMPARE(fixture.editor.mode(), VTypstEditor::Mode::Read);
    QVERIFY(!fixture.editor.isInplacePreviewEnabled());
    QVERIFY(fixture.editor.isLivePreviewEnabled());
    QCOMPARE(fixture.editor.pdfZoomMode(), VTypstEditor::PdfZoomMode::Width);
    QTest::qWait(100);
    QCOMPARE(published.count(), before);
    QCOMPARE(fixture.editor.pdfPageCount(), 0);
    const auto file = QStringLiteral("B%1.typ").arg(round);
    fixture.editor.setProject(root.path(), file);
    fixture.editor.setActiveFile(file);
    source->setText(shortSource(QStringLiteral("Published B %1.").arg(round)));
    fixture.editor.finishDocumentReset();
    QVERIFY2(waitUntil([&]() { return published.count() == before + 1; }),
             qPrintable(fixture.editor.compileMessage()));
    QCOMPARE(fixture.selectPage().trimmed(), QStringLiteral("Published B %1.").arg(round));
    QCOMPARE(fixture.pane(), pane);
    QCOMPARE(fixture.editor.getTextEditor(), source);
    QCOMPARE(fixture.editor.getConfig().m_runtime, &m_runtime);
    QCOMPARE(fixture.editor.currentPdfPage(), 0);
    QCOMPARE(fixture.editor.pdfZoomMode(), VTypstEditor::PdfZoomMode::Width);
  }
}

void TestTypstEditor::documentResetDrainsTimedOutRequest() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  QFile diskSource(root.filePath(QStringLiteral("main.typ")));
  QVERIFY(diskSource.open(QIODevice::WriteOnly));
  QVERIFY(diskSource.write("Saved A dependency.") > 0);
  diskSource.close();
  auto config = editorConfig(root.path(), &m_runtime);
  EditorFixture fixture(m_runtime, root.path(), shortSource(QStringLiteral("Unsaved A overlay.")),
                        VTypstEditor::Mode::Read, config);
  QSignalSpy published(fixture.pane(), SIGNAL(snapshotPublished(quint64)));
  QVERIFY2(waitUntil([&] { return published.count() == 1; }),
           qPrintable(fixture.editor.compileMessage()));
  auto *source = fixture.editor.getTextEditor();
  auto *pane = fixture.pane();
  RuntimeLaneGate gate(m_runtime);
  QVERIFY(gate.entered());
  auto *delivery = m_runtime.findChild<QObject *>(QStringLiteral("typst-delivery"));
  QVERIFY(delivery);
  QCoreApplication::sendPostedEvents(delivery, QEvent::MetaCall);
  int ready = 0;
  const auto connection =
      connect(&fixture.editor, &VTypstEditor::compileStatusChanged, &fixture.editor,
              [&](VTypstEditor::CompileStatus p_status, const QString &) {
                if (p_status == VTypstEditor::CompileStatus::Ready)
                  ++ready;
              });
  // Hold both lanes before dispatch. A real soft deadline retires the admitted
  // old request; reset and Retry must not admit its superseded A source later.
  config->m_compileTimeoutMs = 50;
  fixture.editor.setConfig(config);
  QVERIFY2(waitUntil([&] {
             return fixture.editor.compileStatus() == VTypstEditor::CompileStatus::Error;
           }),
           qPrintable(fixture.editor.compileMessage()));
  source->setText(shortSource(QStringLiteral("Queued A must never compile.")));
  fixture.editor.resetForNewDocument();
  config->m_compileTimeoutMs = operationTimeout;
  fixture.editor.setConfig(config);
  fixture.editor.setProject(root.path(), QStringLiteral("B.typ"));
  fixture.editor.setActiveFile(QStringLiteral("B.typ"));
  source->setText(shortSource(QStringLiteral("#include \"main.typ\"\n\nOnly B.")));
  fixture.editor.finishDocumentReset();
  QCOMPARE(fixture.editor.pdfPageCount(), 0);
  QCOMPARE(ready, 0);
  QCOMPARE(published.count(), 1);
  bool guiResponsive = false;
  QTimer::singleShot(0, &fixture.editor, [&] { guiResponsive = true; });
  QVERIFY(waitUntil([&] { return guiResponsive; }));
  fixture.editor.retryCompilation();
  fixture.editor.retryCompilation();
  QCOMPARE(published.count(), 1);
  gate.release();
  QVERIFY2(waitUntil([&] { return published.count() == 2; }),
           qPrintable(fixture.editor.compileMessage()));
  disconnect(connection);
  QCOMPARE(ready, 1);
  const auto text = fixture.selectPage();
  QVERIFY(text.contains(QStringLiteral("Saved A dependency")));
  QVERIFY(text.contains(QStringLiteral("Only B")));
  QVERIFY(!text.contains(QStringLiteral("Unsaved A overlay")));
  QVERIFY(!text.contains(QStringLiteral("Queued A")));
  QCOMPARE(fixture.editor.getTextEditor(), source);
  QCOMPARE(fixture.pane(), pane);
  QCOMPARE(fixture.editor.getConfig().m_runtime, &m_runtime);
  QVERIFY(fixture.editor.diagnostics().isEmpty());
}

void TestTypstEditor::repeatedCloseDuringPendingOutput() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  for (int round = 0; round < 3; ++round) {
    auto fixture = std::unique_ptr<EditorFixture>(
        new EditorFixture(m_runtime, root.path(), shortSource(QStringLiteral("Close baseline."))));
    QPointer<VTypstEditor> guarded(&fixture->editor);
    QSignalSpy published(fixture->pane(), SIGNAL(snapshotPublished(quint64)));
    QVERIFY2(waitUntil([&]() { return published.count() == 1; }),
             qPrintable(fixture->editor.compileMessage()));
    QCOMPARE(fixture->selectPage().trimmed(), QStringLiteral("Close baseline."));
    fixture->editor.setFileOverlay(QStringLiteral("main.typ"),
                                   shortSource(QStringLiteral("Replacement one.")));
    QVERIFY2(waitUntil([&]() { return fixture->pane()->property("hasPendingUpdate").toBool(); }),
             qPrintable(fixture->editor.compileMessage()));
    // Close with displayed and pending slots, followed by another in-flight
    // compilation/render resize. CTest bounds teardown itself if it deadlocks.
    fixture->pane()->setProperty("zoom", 2.0 + round);
    fixture->editor.setFileOverlay(QStringLiteral("main.typ"),
                                   shortSource(QStringLiteral("Replacement two.")));
    QTest::qWait(20);
    fixture.reset();
    QVERIFY(guarded.isNull());
    QCoreApplication::processEvents();
  }
  EditorFixture finalFixture(m_runtime, root.path(),
                             shortSource(QStringLiteral("Alive after repeated close.")));
  QSignalSpy published(finalFixture.pane(), SIGNAL(snapshotPublished(quint64)));
  QVERIFY2(waitUntil([&]() { return published.count() == 1; }),
           qPrintable(finalFixture.editor.compileMessage()));
  QCOMPARE(finalFixture.selectPage().trimmed(), QStringLiteral("Alive after repeated close."));
}

void TestTypstEditor::resourceContextMetadataAndGestures() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  QVERIFY(writeResourceImage(root.filePath(QStringLiteral("red.png")), Qt::red));
  QVERIFY(writeResourceImage(root.filePath(QStringLiteral("blue.png")), Qt::blue));
  const auto source =
      QStringLiteral("#set page(width: 320pt, height: 300pt, margin: 20pt)\n"
                     "#let destination = \"https://example.test/\" + \"evaluated\"\n"
                     "#figure(rotate(8deg, image(\"red.png\", width: 72pt)), caption: [Nested])\n"
                     "#link(destination)[#image(\"blue.png\", width: 72pt)]\n"
                     "// #image(\"red.png\")\n`#image(\"red.png\")`\nUnrelated text\n");
  EditorFixture fixture(m_runtime, root.path(), source, VTypstEditor::Mode::Edit, {}, false, true);
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  QVERIFY(waitUntil([&]() { return fixture.editor.pdfPageCount() == 1; }));
  auto *document = fixture.editor.document();
  document->setModified(false);
  const int undoSteps = document->availableUndoSteps();
  QTextCursor selected(document);
  selected.setPosition(source.indexOf(QStringLiteral("Unrelated text")));
  selected.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
  fixture.edit()->setTextCursor(selected);
  const auto sourceImage = sourceResourceAt(fixture, source.indexOf(QStringLiteral("red.png")) + 2);
  QCOMPARE(sourceImage.m_surface, TypstResourceContext::Surface::Source);
  QVERIFY(sourceImage.m_imageId);
  QCOMPARE(sourceImage.m_imagePath, QStringLiteral("red.png"));
  QCOMPARE(sourceImage.m_imageSize, QSize(12, 8));
  QCOMPARE(sourceImage.m_width, 96);
  QVERIFY(sourceImage.m_imageResizable);
  QCOMPARE(fixture.edit()->textCursor().selectionStart(), selected.selectionStart());
  QCOMPARE(fixture.edit()->textCursor().selectionEnd(), selected.selectionEnd());
  const auto linked = sourceResourceAt(fixture, source.indexOf(QStringLiteral("blue.png")) + 2);
  QVERIFY(linked.m_imageId);
  QVERIFY(linked.m_linkIndex >= 0);
  QCOMPARE(linked.m_linkUrl, QUrl(QStringLiteral("https://example.test/evaluated")));
  QSignalSpy links(&fixture.editor, &VTypstEditor::externalLinkRequested);
  auto alteredLink = linked;
  alteredLink.m_linkUrl = QUrl(QStringLiteral("https://wrong.test/"));
  fixture.editor.openContextLink(alteredLink);
  QCOMPARE(links.count(), 1);
  QCOMPARE(links.first().first().toUrl(), linked.m_linkUrl);
  QCOMPARE(links.first().at(1).toBool(), true);
  for (const int offset : {source.indexOf(QStringLiteral("// #image")) + 5,
                           source.indexOf(QStringLiteral("`#image")) + 4,
                           source.indexOf(QStringLiteral("Unrelated text")) + 2}) {
    const auto context = sourceResourceAt(fixture, offset);
    QVERIFY(!context.m_imageId);
    QCOMPARE(context.m_linkIndex, -1);
  }
  QRect redBounds, blueBounds;
  QVERIFY(waitUntil([&]() {
    const auto image = widgetImage(fixture.pane()->viewport());
    return coloredPixels(image, true, &redBounds) > 20 &&
           coloredPixels(image, false, &blueBounds) > 20;
  }));
  const auto previewImage =
      resourceMenuAt(fixture.editor, fixture.pane()->viewport(), redBounds.center());
  QCOMPARE(previewImage.m_surface, TypstResourceContext::Surface::Preview);
  QCOMPARE(previewImage.m_imageId, sourceImage.m_imageId);
  const auto previewLinked =
      resourceMenuAt(fixture.editor, fixture.pane()->viewport(), blueBounds.center());
  QCOMPARE(previewLinked.m_imageId, linked.m_imageId);
  QCOMPARE(previewLinked.m_linkUrl, linked.m_linkUrl);
  QSignalSpy activated(&fixture.editor, &VTypstEditor::imageActivated);
  QTest::mouseDClick(fixture.pane()->viewport(), Qt::LeftButton, Qt::NoModifier,
                     blueBounds.center());
  QCOMPARE(activated.count(), 0);
  QTest::mouseDClick(fixture.pane()->viewport(), Qt::LeftButton, Qt::NoModifier,
                     redBounds.center());
  QCOMPARE(activated.count(), 1);
  QCOMPARE(activated.last().first().value<TypstResourceContext>().m_imageId, sourceImage.m_imageId);

  fixture.editor.setInplacePreviewEnabled(true);
  fixture.editor.setLivePreviewEnabled(false);
  QVERIFY(waitUntil(
      [&]() { return resourceCard(fixture.editor, true) && resourceCard(fixture.editor, false); }));
  QPoint point;
  auto *card = resourceCard(fixture.editor, true, &point);
  const auto inPlace = resourceMenuAt(fixture.editor, card, point);
  QCOMPARE(inPlace.m_surface, TypstResourceContext::Surface::InPlace);
  QCOMPARE(inPlace.m_imageId, sourceImage.m_imageId);
  // The enclosing figure card must resolve its nested image, not the figure raster.
  QCOMPARE(inPlace.m_imageSize, QSize(12, 8));
  QTest::mouseDClick(card, Qt::LeftButton, Qt::NoModifier, point);
  QCOMPARE(activated.count(), 2);
  card = resourceCard(fixture.editor, false, &point);
  const auto linkedCard = resourceMenuAt(fixture.editor, card, point);
  QCOMPARE(linkedCard.m_linkUrl, linked.m_linkUrl);
  QTest::mouseDClick(card, Qt::LeftButton, Qt::NoModifier, point);
  QCOMPARE(activated.count(), 2);
  fixture.editor.setReadOnly(true);
  const auto readOnly = resourceMenuAt(fixture.editor, card, point);
  QVERIFY(fixture.editor.isResourceContextValid(readOnly));
  QVERIFY(!readOnly.m_imageResizable);
  QVERIFY(!fixture.editor.resizeImage(readOnly, 192, 0));
  QCOMPARE(document->toPlainText(), source);
  QCOMPARE(document->availableUndoSteps(), undoSteps);
  QVERIFY(!document->isModified());
}

void TestTypstEditor::resourceLinkActivationOrigins() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  QVERIFY(writeResourceImage(root.filePath(QStringLiteral("picture.png")), Qt::red));
  QVERIFY(writeFixture(root.filePath(QStringLiteral("target.md")), QByteArray("# Beta\n")));
  const auto source =
      shortSource(QStringLiteral("#let destination = \"target\" + \".md#beta\"\n"
                                 "#link(destination)[#image(\"picture.png\", width: 60pt)]"));
  const QUrl destination(QStringLiteral("target.md#beta"));
  EditorFixture fixture(m_runtime, root.path(), source, VTypstEditor::Mode::Edit, {}, false, true);
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  auto *document = fixture.editor.document();
  document->setModified(false);
  const int undoSteps = document->availableUndoSteps();
  QSignalSpy links(&fixture.editor, &VTypstEditor::externalLinkRequested);
  const auto openFromMenu = [&](const TypstResourceContext &p_context) {
    fixture.editor.openContextLink(p_context, true);
  };

  // Host menu actions use a real captured source context and the evaluated URL.
  const auto sourceLink =
      sourceResourceAt(fixture, source.indexOf(QStringLiteral("picture.png")) + 2, openFromMenu);
  QVERIFY(sourceLink.m_linkNavigable);
  QCOMPARE(sourceLink.m_linkUrl, destination);
  QCOMPARE(links.count(), 1);
  QCOMPARE(links.last().at(0).toUrl(), destination);
  QCOMPARE(links.last().at(1).toBool(), true);
  links.clear();

  QRect bounds;
  auto *pane = fixture.pane();
  QVERIFY(waitUntil(
      [&]() { return coloredPixels(widgetImage(pane->viewport()), true, &bounds) > 20; }));
  QSignalSpy mapping(pane, SIGNAL(sourceLocationRequested(quint64, int, QPointF)));
  for (const auto modifiers : {Qt::NoModifier, Qt::ControlModifier}) {
    // Plain clicks use the PDF link; Ctrl-clicks exercise compiler hit mapping.
    QTest::mouseClick(pane->viewport(), Qt::LeftButton, modifiers, bounds.center());
    QVERIFY(waitUntil([&]() { return links.count() == 1; }));
    QCOMPARE(links.last().at(0).toUrl(), destination);
    QCOMPARE(links.last().at(1).toBool(), false);
    links.clear();
  }
  QCOMPARE(mapping.count(), 1);
  const auto paneLink =
      resourceMenuAt(fixture.editor, pane->viewport(), bounds.center(), openFromMenu);
  QCOMPARE(paneLink.m_surface, TypstResourceContext::Surface::Preview);
  QCOMPARE(paneLink.m_linkUrl, destination);
  QCOMPARE(links.count(), 1);
  QCOMPARE(links.last().at(0).toUrl(), destination);
  QCOMPARE(links.last().at(1).toBool(), true);
  links.clear();

  fixture.editor.setInplacePreviewEnabled(true);
  fixture.editor.setLivePreviewEnabled(false);
  QPoint point;
  QPointer<QWidget> card;
  QVERIFY(waitUntil([&]() {
    card = resourceCard(fixture.editor, true, &point);
    return !card.isNull();
  }));
  for (const auto modifiers : {Qt::NoModifier, Qt::ControlModifier}) {
    QVERIFY(card);
    QTest::mouseClick(card, Qt::LeftButton, modifiers, point);
    QCOMPARE(links.count(), 1);
    QCOMPARE(links.last().at(0).toUrl(), destination);
    QCOMPARE(links.last().at(1).toBool(), false);
    links.clear();
  }
  QVERIFY(card);
  const auto cardLink = resourceMenuAt(fixture.editor, card, point, openFromMenu);
  QCOMPARE(cardLink.m_surface, TypstResourceContext::Surface::InPlace);
  QCOMPARE(cardLink.m_linkUrl, destination);
  QCOMPARE(links.count(), 1);
  QCOMPARE(links.last().at(0).toUrl(), destination);
  QCOMPARE(links.last().at(1).toBool(), true);
  QCOMPARE(document->toPlainText(), source);
  QCOMPARE(document->availableUndoSteps(), undoSteps);
  QVERIFY(!document->isModified());
}

void TestTypstEditor::resourceImageDataRetentionAndCancellation() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  const auto path = root.filePath(QStringLiteral("picture.png"));
  QByteArray original;
  QVERIFY(writeResourceImage(path, Qt::red, &original));
  const auto source =
      shortSource(QStringLiteral("Retained label\n\n#image(\"picture.png\", width: 60pt)"));
  EditorFixture fixture(m_runtime, root.path(), source);
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  QRect bounds;
  QVERIFY(waitUntil([&]() {
    return coloredPixels(widgetImage(fixture.pane()->viewport()), true, &bounds) > 20;
  }));
  const auto context = resourceMenuAt(fixture.editor, fixture.pane()->viewport(), bounds.center());
  QVERIFY(context.m_imageId);
  QSignalSpy data(&fixture.editor, &VTypstEditor::imageDataReady);
  fixture.editor.requestImageData(context, 1);
  QCOMPARE(data.count(), 0); // Even cached/invalid calls complete asynchronously.
  QVERIFY(waitUntil([&]() { return data.count() == 1; }));
  QCOMPARE(data.last().at(0).toULongLong(), quint64(1));
  QCOMPARE(data.last().at(1).toByteArray(), original);
  QCOMPARE(data.last().at(2).toString(), QStringLiteral("image/png"));
  QVERIFY(data.last().at(3).toString().isEmpty());
  QVERIFY(fixture.selectPage().contains(QStringLiteral("Retained label")));
  QVERIFY(writeResourceImage(path, Qt::blue));
  fixture.editor.setFileOverlay(QStringLiteral("main.typ"),
                                source + QStringLiteral("\nNew generation"));
  QVERIFY(waitUntil([&]() {
    return fixture.editor.hasPendingPdfUpdate() &&
           fixture.editor.compileStatus() == VTypstEditor::CompileStatus::Ready;
  }));
  QVERIFY(fixture.editor.isResourceContextValid(context));
  QVERIFY(!fixture.editor.isResourceContextValid(context, true));
  fixture.editor.requestImageData(context, 2);
  QVERIFY(waitUntil([&]() { return data.count() == 2; }));
  QCOMPARE(data.last().at(1).toByteArray(), original);
  QVERIFY(data.last().at(3).toString().isEmpty());
  fixture.clearSelection();
  QVERIFY(waitUntil([&]() { return !fixture.editor.isResourceContextValid(context); }));
  fixture.editor.requestImageData(context, 3);
  QVERIFY(waitUntil([&]() { return data.count() == 3; }));
  QVERIFY(data.last().at(1).toByteArray().isEmpty());
  QVERIFY(!data.last().at(3).toString().isEmpty());
  QVERIFY(waitUntil([&]() {
    return coloredPixels(widgetImage(fixture.pane()->viewport()), false, &bounds) > 20;
  }));
  const auto latest = resourceMenuAt(fixture.editor, fixture.pane()->viewport(), bounds.center());
  QVERIFY(latest.m_imageId);
  auto forged = latest;
  forged.m_imageId = std::numeric_limits<quint64>::max();
  fixture.editor.requestImageData(forged, 4);
  QVERIFY(waitUntil([&]() { return data.count() == 4; }));
  QVERIFY(!data.last().at(3).toString().isEmpty());
  fixture.editor.requestImageData(latest, 5);
  fixture.editor.requestImageData(latest, 6);
  fixture.editor.retryCompilation();
  QVERIFY(waitUntil([&]() { return data.count() == 6; }));
  QSet<quint64> completions;
  for (const auto &reply : data) {
    QVERIFY(!completions.contains(reply[0].toULongLong()));
    completions.insert(reply[0].toULongLong());
    if (reply[0].toULongLong() >= 5) {
      QVERIFY(reply[1].toByteArray().isEmpty());
      QVERIFY(!reply[3].toString().isEmpty());
    }
  }
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  QCOMPARE(data.count(), 6);
}

void TestTypstEditor::resourceResizePreservesSourceUndo() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  QVERIFY(writeResourceImage(root.filePath(QStringLiteral("picture.png")), Qt::red));
  const auto source = shortSource(QStringLiteral("// 😀 é\n#image(\"picture.png\", alt: \"😀\", "
                                                 "width: /*keep*/ 1in, height: auto, /*tail*/)\n"));
  EditorFixture fixture(m_runtime, root.path(), source, VTypstEditor::Mode::Edit, {}, false, true);
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  auto *document = fixture.editor.document();
  const auto original =
      sourceResourceAt(fixture, source.indexOf(QStringLiteral("picture.png")) + 2);
  QVERIFY(original.m_imageResizable);
  QCOMPARE(original.m_width, 96);
  QCOMPARE(original.m_height, 0);
  const int steps = document->availableUndoSteps();
  QVERIFY(fixture.editor.resizeImage(original, 96, 0));
  QCOMPARE(document->availableUndoSteps(), steps);
  QCOMPARE(document->toPlainText(), source);
  auto forged = original;
  forged.m_sourceStart = 0;
  forged.m_sourceEnd = source.size();
  forged.m_width = 500;
  forged.m_imagePath = QStringLiteral("wrong.png");
  QVERIFY(fixture.editor.resizeImage(forged, 192, 48));
  const auto expected =
      QString(source)
          .replace(QStringLiteral("1in"), QStringLiteral("144.00pt"))
          .replace(QStringLiteral("height: auto"), QStringLiteral("height: 36.00pt"));
  QCOMPARE(document->toPlainText(), expected);
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  document->undo();
  QCOMPARE(document->toPlainText(), source);
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  document->redo();
  QCOMPARE(document->toPlainText(), expected);
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  auto context = sourceResourceAt(fixture, expected.indexOf(QStringLiteral("picture.png")) + 2);
  QVERIFY(fixture.editor.resizeImage(context, 0, 0));
  const auto cleared = QString(expected)
                           .replace(QStringLiteral("144.00pt"), QStringLiteral("auto"))
                           .replace(QStringLiteral("36.00pt"), QStringLiteral("auto"));
  QCOMPARE(document->toPlainText(), cleared);
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  for (const bool trailing : {false, true}) {
    const auto plain = shortSource(trailing ? QStringLiteral("#image(\"picture.png\", /*tail*/)\n")
                                            : QStringLiteral("#image(\"picture.png\")\n"));
    fixture.editor.setFileOverlay(QStringLiteral("main.typ"), plain);
    QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
    context = sourceResourceAt(fixture, plain.indexOf(QStringLiteral("picture.png")) + 2);
    QVERIFY(context.m_imageResizable);
    QVERIFY(fixture.editor.resizeImage(context, 96, 0));
    const auto imageSource = document->toPlainText().mid(plain.indexOf(QStringLiteral("#image")));
    QVERIFY(!imageSource.contains(QStringLiteral("height:")));
    QVERIFY(imageSource.contains(QStringLiteral("width: 72.00pt")));
    if (trailing)
      QVERIFY(document->toPlainText().contains(QStringLiteral("/*tail*/")));
    QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
    document->undo();
    QCOMPARE(document->toPlainText(), plain);
    QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  }
  context =
      sourceResourceAt(fixture, document->toPlainText().indexOf(QStringLiteral("picture.png")) + 2);
  QTextCursor edit(document);
  edit.beginEditBlock();
  edit.movePosition(QTextCursor::End);
  edit.insertText(QStringLiteral("changed"));
  const auto changed = document->toPlainText();
  QVERIFY(!fixture.editor.resizeImage(context, 200, 100));
  QCOMPARE(document->toPlainText(), changed);
  edit.endEditBlock();
}

void TestTypstEditor::resourceResizeRejectsUnsafeContexts() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  QVERIFY(writeResourceImage(root.filePath(QStringLiteral("picture.png")), Qt::red));
  QVERIFY(writeFixture(root.filePath(QStringLiteral("import.typ")),
                       QByteArray("#image(\"picture.png\", width: 60pt)")));
  EditorFixture fixture(m_runtime, root.path(), shortSource(QStringLiteral("Baseline")),
                        VTypstEditor::Mode::Edit, {}, false, true);
  QSignalSpy published(fixture.pane(), SIGNAL(snapshotPublished(quint64)));
  for (const auto &body :
       {QStringLiteral("#image(\"picture.png\", width: 50%)"),
        QStringLiteral("#let w = 60pt\n#image(\"picture.png\", width: w)"),
        QStringLiteral("#let other = image\n#other(\"picture.png\")"),
        QStringLiteral("#let image(..args) = std.image(..args)\n#image(\"picture.png\")"),
        QStringLiteral("#include \"import.typ\"")}) {
    const auto source = shortSource(body);
    const int previousPublication = published.count();
    fixture.editor.setFileOverlay(QStringLiteral("main.typ"), source);
    QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
    QRect bounds;
    QVERIFY(waitUntil([&]() {
      return published.count() > previousPublication && !fixture.editor.hasPendingPdfUpdate() &&
             coloredPixels(widgetImage(fixture.pane()->viewport()), true, &bounds) > 20;
    }));
    auto context = resourceMenuAt(fixture.editor, fixture.pane()->viewport(), bounds.center());
    QVERIFY(context.m_imageId);
    QVERIFY(!context.m_imageResizable);
    context.m_imageResizable = true;
    context.m_sourceFile = QStringLiteral("main.typ");
    QVERIFY(!fixture.editor.resizeImage(context, 96, 48));
    QCOMPARE(fixture.editor.document()->toPlainText(), source);
  }
  const auto ambiguous =
      shortSource(QStringLiteral("#for n in (0, 255) { image(bytes((n, 100, 0)), format: "
                                 "(encoding: \"rgb8\", width: 1, height: 1)); "
                                 "link(\"https://example.test/\" + str(n))[go] }"));
  fixture.editor.setFileOverlay(QStringLiteral("main.typ"), ambiguous);
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  auto context = sourceResourceAt(fixture, ambiguous.indexOf(QStringLiteral("image(bytes")) + 3);
  QVERIFY(!context.m_imageId);
  context = sourceResourceAt(fixture, ambiguous.indexOf(QStringLiteral("link(\"https")) + 3);
  QCOMPARE(context.m_linkIndex, -1);
}

void TestTypstEditor::resourceMenusFollowTheirSurface_data() {
  QTest::addColumn<bool>("preview");
  QTest::addColumn<bool>("image");
  QTest::newRow("source-image-survives-pdf-handoff") << false << true;
  QTest::newRow("source-standard-survives-pdf-handoff") << false << false;
  QTest::newRow("old-pdf-menu-closes-on-handoff") << true << true;
}

void TestTypstEditor::resourceMenusFollowTheirSurface() {
  QFETCH(bool, preview);
  QFETCH(bool, image);
  QTemporaryDir root;
  QVERIFY(root.isValid());
  QByteArray original;
  QVERIFY(writeResourceImage(root.filePath(QStringLiteral("picture.png")), Qt::red, &original));
  const auto source =
      shortSource(QStringLiteral("Held selection\n\n#image(\"picture.png\", width: 60pt)"));
  EditorFixture fixture(m_runtime, root.path(), source, VTypstEditor::Mode::Edit, {}, false, true);
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  QRect bounds;
  QVERIFY(waitUntil([&]() {
    return coloredPixels(widgetImage(fixture.pane()->viewport()), true, &bounds) > 20;
  }));
  QVERIFY(fixture.selectPage().contains(QStringLiteral("Held selection")));
  QTextCursor edit(fixture.editor.document());
  edit.movePosition(QTextCursor::End);
  edit.insertText(QStringLiteral("\nNew output"));
  QVERIFY(waitUntil([&]() {
    return fixture.editor.hasPendingPdfUpdate() &&
           fixture.editor.compileStatus() == VTypstEditor::CompileStatus::Ready;
  }));
  QWidget *target = fixture.pane()->viewport();
  QPoint point = bounds.center();
  if (!preview) {
    target = fixture.edit()->viewport();
    QTextCursor cursor(fixture.editor.document());
    cursor.setPosition(
        source.indexOf(image ? QStringLiteral("picture.png") : QStringLiteral("Held selection")) +
        2);
    point = fixture.edit()->cursorRect(cursor).center();
  }
  bool published = false;
  bool stayedOpen = false;
  bool clicked = false;
  TypstResourceContext captured;
  QSignalSpy data(&fixture.editor, &VTypstEditor::imageDataReady);
  const auto connection =
      connect(&fixture.editor, &VTypstEditor::resourceContextMenuRequested, &fixture.editor,
              [&](const TypstResourceContext &context, QMenu *menu) {
                captured = context;
                auto *action = menu->addAction(QStringLiteral("Use current resource"));
                connect(action, &QAction::triggered, menu, [&] { clicked = true; });
                QTimer::singleShot(0, menu, [&, menu, action] {
                  fixture.editor.publishPendingPdfUpdate();
                  published = waitUntil([&] { return !fixture.editor.hasPendingPdfUpdate(); });
                  stayedOpen = menu->isVisible();
                  if (stayedOpen) {
                    QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier,
                                      menu->actionGeometry(action).center());
                  }
                });
                QTimer::singleShot(3000, menu, &QMenu::close);
              });
  QContextMenuEvent event(QContextMenuEvent::Mouse, point, target->mapToGlobal(point));
  QCoreApplication::sendEvent(target, &event);
  disconnect(connection);
  QVERIFY(published);
  QCOMPARE(stayedOpen, !preview);
  QCOMPARE(clicked, !preview);
  if (image && !preview) {
    QVERIFY(fixture.editor.isResourceContextValid(captured));
    fixture.editor.requestImageData(captured, 77);
    QVERIFY(waitUntil([&] { return data.count() == 1; }));
    QCOMPARE(data.first()[1].toByteArray(), original);
    QVERIFY(data.first()[3].toString().isEmpty());
  } else if (preview) {
    QVERIFY(!fixture.editor.isResourceContextValid(captured));
  }
}

void TestTypstEditor::resourceMenusSurviveDeletion_data() {
  QTest::addColumn<int>("surface");
  QTest::newRow("source") << 0;
  QTest::newRow("in-place") << 1;
  QTest::newRow("pdf") << 2;
}

void TestTypstEditor::resourceMenusSurviveDeletion() {
  QFETCH(int, surface);
  QTemporaryDir root;
  QVERIFY(root.isValid());
  QVERIFY(writeResourceImage(root.filePath(QStringLiteral("picture.png")), Qt::red));
  const auto source = shortSource(QStringLiteral("#image(\"picture.png\", width: 60pt)"));
  auto fixture = std::make_unique<EditorFixture>(
      m_runtime, root.path(), source, VTypstEditor::Mode::Edit, QSharedPointer<TypstEditorConfig>(),
      surface == 1, surface == 2);
  QVERIFY2(fixture->ready(), qPrintable(fixture->editor.compileMessage()));
  QWidget *target = fixture->edit()->viewport();
  QPoint point;
  if (surface == 0) {
    QTextCursor cursor(fixture->editor.document());
    cursor.setPosition(source.indexOf(QStringLiteral("picture.png")) + 2);
    point = fixture->edit()->cursorRect(cursor).center();
  } else if (surface == 1) {
    QVERIFY(waitUntil([&]() { return resourceCard(fixture->editor, true); }));
    target = resourceCard(fixture->editor, true, &point);
  } else {
    QRect bounds;
    QVERIFY(waitUntil([&]() {
      return coloredPixels(widgetImage(fixture->pane()->viewport()), true, &bounds) > 20;
    }));
    target = fixture->pane()->viewport();
    point = bounds.center();
  }
  QPointer<VTypstEditor> editor(&fixture->editor);
  QPointer<QMenu> menu;
  bool imageContext = false;
  connect(editor, &VTypstEditor::resourceContextMenuRequested, this,
          [&](const TypstResourceContext &context, QMenu *popup) {
            imageContext = context.m_imageId != 0;
            menu = popup;
            popup->addAction(QStringLiteral("Keep popup open"));
            QTimer::singleShot(0, this, [&]() { fixture.reset(); });
          });
  QContextMenuEvent event(QContextMenuEvent::Mouse, point, target->mapToGlobal(point));
  QCoreApplication::sendEvent(target, &event);
  QVERIFY(imageContext);
  QVERIFY(editor.isNull());
  QVERIFY(menu.isNull());
  QCoreApplication::processEvents();
}

void TestTypstEditor::resourceIndependentSnapshotRetention() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  QByteArray red, blue;
  QVERIFY(writeResourceImage(root.filePath(QStringLiteral("red.png")), Qt::red, &red));
  QVERIFY(writeResourceImage(root.filePath(QStringLiteral("blue.png")), Qt::blue, &blue));
  const auto source = [](const QString &name, int width) {
    return shortSource(
        QStringLiteral("Retained selection\n\n#image(\"%1\", width: %2pt)").arg(name).arg(width));
  };
  EditorFixture fixture(m_runtime, root.path(), source(QStringLiteral("red.png"), 60),
                        VTypstEditor::Mode::Edit, {}, true, true);
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  QRect bounds;
  QVERIFY(waitUntil([&]() {
    return resourceCard(fixture.editor, true) &&
           coloredPixels(widgetImage(fixture.pane()->viewport()), true, &bounds) > 20;
  }));
  const auto pdfA = resourceMenuAt(fixture.editor, fixture.pane()->viewport(), bounds.center());
  QVERIFY(fixture.selectPage().contains(QStringLiteral("Retained selection")));
  auto edit = fixture.editor.document()->find(QStringLiteral("red.png"));
  QVERIFY(!edit.isNull());
  edit.insertText(QStringLiteral("blue.png"));
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  QVERIFY(waitUntil([&]() {
    return resourceCard(fixture.editor, false) && fixture.editor.hasPendingPdfUpdate();
  }));
  QPoint point;
  auto *card = resourceCard(fixture.editor, false, &point);
  const auto inPlaceB = resourceMenuAt(fixture.editor, card, point);
  QVERIFY(inPlaceB.m_imageId);
  QVERIFY(inPlaceB.m_snapshot != pdfA.m_snapshot);
  QVERIFY(fixture.editor.isResourceContextValid(pdfA));
  QSignalSpy data(&fixture.editor, &VTypstEditor::imageDataReady);
  TypstRasterObserver observer;
  QVERIFY(observeRasters(&fixture.editor, &observer));
  bool requested = false;
  observer.requested = [&](quint64, quint64 snapshot, int, qreal) {
    if (requested || snapshot == inPlaceB.m_snapshot)
      return;
    requested = true;
    // C is compiling/rendering, while A's PDF selection and B's card remain displayed.
    // Keep B's cached frame instead of handing C to the in-place surface.
    fixture.editor.setInplacePreviewEnabled(false);
    fixture.editor.requestImageData(pdfA, 10);
    fixture.editor.requestImageData(inPlaceB, 11);
  };
  edit = fixture.editor.document()->find(QStringLiteral("60pt"));
  QVERIFY(!edit.isNull());
  edit.insertText(QStringLiteral("72pt"));
  QVERIFY2(waitUntil([&]() { return requested && data.count() == 2; }),
           qPrintable(fixture.editor.compileMessage()));
  for (const auto &reply : data) {
    QVERIFY(reply[3].toString().isEmpty());
    QCOMPARE(reply[1].toByteArray(), reply[0].toULongLong() == 10 ? red : blue);
  }
  const auto sourceC = sourceResourceAt(
      fixture, fixture.editor.document()->toPlainText().indexOf(QStringLiteral("blue.png")) + 2);
  QVERIFY(sourceC.m_imageId);
  QVERIFY(sourceC.m_snapshot != pdfA.m_snapshot && sourceC.m_snapshot != inPlaceB.m_snapshot);
  edit.movePosition(QTextCursor::End);
  edit.insertText(QStringLiteral("\n#let broken = ("));
  QVERIFY(waitUntil(
      [&]() { return fixture.editor.compileStatus() == VTypstEditor::CompileStatus::Error; }));
  QVERIFY(fixture.editor.isResourceContextValid(pdfA));
  QVERIFY(fixture.editor.isResourceContextValid(inPlaceB));
  QVERIFY(!fixture.editor.isResourceContextValid(sourceC));
  fixture.editor.requestImageData(pdfA, 12);
  fixture.editor.requestImageData(inPlaceB, 13);
  fixture.editor.requestImageData(sourceC, 14);
  QVERIFY(waitUntil([&]() { return data.count() == 5; }));
  for (const auto &reply : data) {
    const auto id = reply[0].toULongLong();
    if (id == 14) {
      QVERIFY(reply[1].toByteArray().isEmpty());
      QVERIFY(!reply[3].toString().isEmpty());
    } else {
      QVERIFY(reply[3].toString().isEmpty());
      QCOMPARE(reply[1].toByteArray(), id == 10 || id == 12 ? red : blue);
    }
  }
  fixture.editor.document()->undo();
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  // Further compiles must retire the superseded newest, not accumulate a fourth snapshot.
  int previousWidth = 72;
  for (int width : {80, 88, 96}) {
    edit = fixture.editor.document()->find(QString::number(previousWidth) + QStringLiteral("pt"));
    QVERIFY(!edit.isNull());
    edit.insertText(QString::number(width) + QStringLiteral("pt"));
    previousWidth = width;
    QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  }
  QVERIFY(fixture.editor.isResourceContextValid(pdfA));
  QCOMPARE(fixture.editor.diagnostics().size(), 0);
}

void TestTypstEditor::resourceInternalLinkNavigation() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  QVERIFY(writeResourceImage(root.filePath(QStringLiteral("picture.png")), Qt::red));
  // currentPdfPage() identifies the page at the viewport center. Keep each page
  // taller than the viewport so returning to page zero cannot report page one.
  const auto source =
      QStringLiteral("#set page(width: 320pt, height: 640pt, margin: 16pt)\n") +
      QStringLiteral("#link(<target>)[#image(\"picture.png\", width: 60pt) Internal destination]\n"
                     "#pagebreak()\n= Target <target>");
  EditorFixture fixture(m_runtime, root.path(), source, VTypstEditor::Mode::Edit, {}, false, false);
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  QSignalSpy external(&fixture.editor, &VTypstEditor::externalLinkRequested);
  const auto context =
      sourceResourceAt(fixture, source.indexOf(QStringLiteral("Internal destination")) + 2,
                       [&](const TypstResourceContext &p_context) {
                         auto altered = p_context;
                         altered.m_linkPage = 0;
                         altered.m_linkPoint = QPointF(10000, 10000);
                         fixture.editor.openContextLink(altered, true);
                       });
  QVERIFY(context.m_linkIndex >= 0);
  QVERIFY(context.m_linkUrl.isEmpty());
  QCOMPARE(context.m_linkPage, 1);
  QVERIFY(waitUntil([&]() {
    return fixture.editor.isLivePreviewEnabled() && fixture.editor.currentPdfPage() == 1;
  }));
  QVERIFY(external.isEmpty());

  fixture.editor.goToPdfPage(0);
  QRect bounds;
  QVERIFY(waitUntil([&]() {
    return fixture.editor.currentPdfPage() == 0 &&
           coloredPixels(widgetImage(fixture.pane()->viewport()), true, &bounds) > 20;
  }));
  QTest::mouseClick(fixture.pane()->viewport(), Qt::LeftButton, Qt::NoModifier, bounds.center());
  QVERIFY(waitUntil([&]() { return fixture.editor.currentPdfPage() == 1; }));
  QVERIFY(external.isEmpty());

  fixture.editor.setInplacePreviewEnabled(true);
  fixture.editor.setLivePreviewEnabled(false);
  QPoint point;
  QPointer<QWidget> card;
  QVERIFY(waitUntil([&]() {
    card = resourceCard(fixture.editor, true, &point);
    return !card.isNull();
  }));
  QTest::mouseClick(card, Qt::LeftButton, Qt::NoModifier, point);
  QVERIFY(waitUntil([&]() {
    return fixture.editor.isLivePreviewEnabled() && fixture.editor.currentPdfPage() == 1;
  }));
  QVERIFY(external.isEmpty());
  QCOMPARE(fixture.editor.document()->toPlainText(), source);
}

void TestTypstEditor::resourceImageDataBoundsAndGeneratedPixels() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  const auto svg =
      QByteArray("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"8193\" height=\"1\"><rect "
                 "width=\"8193\" height=\"1\" fill=\"red\"/></svg>");
  QVERIFY(writeFixture(root.filePath(QStringLiteral("wide.svg")), svg));
  const auto source =
      shortSource(QStringLiteral("#image(\"wide.svg\", width: 60pt, height: 20pt)\n"
                                 "#image(bytes((255, 0, 0, 0, 0, 255)), format: (encoding: "
                                 "\"rgb8\", width: 2, height: 1), width: 60pt)"));
  EditorFixture fixture(m_runtime, root.path(), source, VTypstEditor::Mode::Edit, {}, false, true);
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  const auto oversized = sourceResourceAt(fixture, source.indexOf(QStringLiteral("wide.svg")) + 2);
  QVERIFY(oversized.m_imageId);
  QCOMPARE(oversized.m_imageSize.width(), 8193);
  QSignalSpy data(&fixture.editor, &VTypstEditor::imageDataReady);
  fixture.editor.requestImageData(oversized, 20);
  QVERIFY(waitUntil([&]() { return data.count() == 1; }));
  QVERIFY(data.last()[1].toByteArray().isEmpty());
  QVERIFY(!data.last()[3].toString().isEmpty());
  QCOMPARE(fixture.editor.compileStatus(), VTypstEditor::CompileStatus::Ready);
  const auto generated =
      sourceResourceAt(fixture, source.indexOf(QStringLiteral("image(bytes")) + 3);
  QVERIFY(generated.m_imageId);
  QVERIFY(generated.m_imagePath.isEmpty());
  QCOMPARE(generated.m_imageSize, QSize(2, 1));
  fixture.editor.requestImageData(generated, 21);
  QVERIFY(waitUntil([&]() { return data.count() == 2; }));
  QVERIFY(data.last()[3].toString().isEmpty());
  QCOMPARE(data.last()[2].toString(), QStringLiteral("image/png"));
  const auto image = QImage::fromData(data.last()[1].toByteArray());
  QCOMPARE(image.size(), QSize(2, 1));
  QCOMPARE(image.pixelColor(0, 0), QColor(Qt::red));
  QCOMPARE(image.pixelColor(1, 0), QColor(Qt::blue));
}

void TestTypstEditor::resourceImageCompletionSurvivesDeletion_data() {
  QTest::addColumn<bool>("valid");
  QTest::newRow("native-success") << true;
  QTest::newRow("invalid-context") << false;
}

void TestTypstEditor::resourceImageCompletionSurvivesDeletion() {
  QFETCH(bool, valid);
  QTemporaryDir root;
  QVERIFY(root.isValid());
  QByteArray original;
  QVERIFY(writeResourceImage(root.filePath(QStringLiteral("picture.png")), Qt::red, &original));
  const auto source = shortSource(QStringLiteral("#image(\"picture.png\", width: 60pt)"));
  auto fixture =
      std::make_unique<EditorFixture>(m_runtime, root.path(), source, VTypstEditor::Mode::Edit,
                                      QSharedPointer<TypstEditorConfig>(), false, false);
  QVERIFY2(fixture->ready(), qPrintable(fixture->editor.compileMessage()));
  auto context = sourceResourceAt(*fixture, source.indexOf(QStringLiteral("picture.png")) + 2);
  QVERIFY(context.m_imageId);
  if (!valid)
    context.m_imageId = std::numeric_limits<quint64>::max();
  QPointer<VTypstEditor> alive(&fixture->editor);
  QByteArray received;
  QString error;
  int completions = 0;
  connect(&fixture->editor, &VTypstEditor::imageDataReady, this,
          [&](quint64 request, const QByteArray &bytes, const QString &, const QString &message) {
            QCOMPARE(request, quint64(42));
            received = bytes;
            error = message;
            ++completions;
            fixture.reset();
          });
  fixture->editor.requestImageData(context, 42);
  QCOMPARE(completions, 0);
  QVERIFY(waitUntil([&]() { return completions == 1; }));
  QVERIFY(alive.isNull());
  QCOMPARE(received, valid ? original : QByteArray());
  QCOMPARE(error.isEmpty(), valid);
  QCoreApplication::processEvents();
  QCOMPARE(completions, 1);
}

void TestTypstEditor::resourceStaleInternalLinkEligibility() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  QVERIFY(writeResourceImage(root.filePath(QStringLiteral("picture.png")), Qt::red));
  auto config = editorConfig(root.path(), &m_runtime);
  const auto source = shortSource(
      QStringLiteral("= Target <target>\n#link(<target>)[#image(\"picture.png\", width: 60pt)]"));
  EditorFixture fixture(m_runtime, root.path(), source, VTypstEditor::Mode::Edit, config, true,
                        true);
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  QRect bounds;
  QVERIFY(waitUntil([&]() {
    return resourceCard(fixture.editor, true) &&
           coloredPixels(widgetImage(fixture.pane()->viewport()), true, &bounds) > 20;
  }));
  const auto pdfA = resourceMenuAt(fixture.editor, fixture.pane()->viewport(), bounds.center());
  QVERIFY(pdfA.m_linkIndex >= 0);
  QVERIFY(pdfA.m_linkNavigable);
  QVERIFY(pdfA.m_linkUrl.isEmpty());
  QVERIFY(!fixture.selectPage().isEmpty());
  auto edit = fixture.editor.document()->find(QStringLiteral("60pt"));
  QVERIFY(!edit.isNull());
  edit.insertText(QStringLiteral("80pt"));
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  TypstResourceContext current;
  QPoint point;
  QVERIFY(waitUntil([&]() {
    auto *card = resourceCard(fixture.editor, true, &point);
    if (!card)
      return false;
    current = resourceMenuAt(fixture.editor, card, point);
    return current.m_snapshot && current.m_snapshot != pdfA.m_snapshot;
  }));
  QVERIFY(current.m_linkNavigable);
  QPointer<QWidget> card = resourceCard(fixture.editor, true, &point);
  QVERIFY(card);
  config->m_debounceMs = 60000;
  fixture.editor.setConfig(config);
  // Drain the queued replacement before a menu can enter its nested event loop.
  QVERIFY(waitUntil([&]() { return card.isNull(); }));
  QVERIFY(waitUntil([&]() {
    card = resourceCard(fixture.editor, true, &point);
    return !card.isNull();
  }));
  auto stale = resourceMenuAt(fixture.editor, card, point);
  QVERIFY(card);
  QVERIFY(stale.m_imageId != 0);
  QVERIFY(stale.m_linkIndex >= 0);
  QVERIFY(!stale.m_linkNavigable);
  QVERIFY(fixture.editor.isResourceContextValid(stale));
  QSignalSpy activated(&fixture.editor, &VTypstEditor::imageActivated);
  QSignalSpy external(&fixture.editor, &VTypstEditor::externalLinkRequested);
  stale.m_linkNavigable = true;
  fixture.editor.openContextLink(stale);
  QVERIFY(external.isEmpty());
  QTest::mouseDClick(card, Qt::LeftButton, Qt::NoModifier, point);
  QVERIFY(activated.isEmpty());
  const auto retainedPdf =
      resourceMenuAt(fixture.editor, fixture.pane()->viewport(), bounds.center());
  QCOMPARE(retainedPdf.m_snapshot, pdfA.m_snapshot);
  QVERIFY(retainedPdf.m_linkNavigable);
  fixture.editor.openContextLink(retainedPdf);
  QVERIFY(external.isEmpty());
}

QTEST_MAIN(tests::TestTypstEditor)
