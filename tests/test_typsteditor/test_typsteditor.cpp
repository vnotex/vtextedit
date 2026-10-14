#include "test_typsteditor.h"

#include <QAbstractScrollArea>
#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaMethod>
#include <QPainter>
#include <QPdfDocument>
#include <QPdfSelection>
#include <QPixmap>
#include <QPointer>
#include <QProcess>
#include <QScrollBar>
#include <QSet>
#include <QSignalSpy>
#include <QSyntaxHighlighter>
#include <QSvgRenderer>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <QWheelEvent>
#include <QtEndian>

#include <cmath>
#include <algorithm>
#include <functional>
#include <memory>
#include <limits>

#include <vtextedit/typstcompiler.h>
#include <vtextedit/typsteditorconfig.h>
#include <vtextedit/vtextedit.h>
#include <vtextedit/vtexteditor.h>
#include <vtextedit/vtypsteditor.h>

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

QString helperPath() {
#ifdef VTEXTEDIT_TYPST_HELPER
  return QString::fromUtf8(VTEXTEDIT_TYPST_HELPER);
#else
#ifdef Q_OS_WIN
  return QDir(QCoreApplication::applicationDirPath())
      .filePath(QStringLiteral("vtextedit-typst.exe"));
#else
  return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("vtextedit-typst"));
#endif
#endif
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
  QJsonObject header;
  QByteArray payload;
};

// Every read/write shares one operation deadline. In particular, parsed is an
// intermediate frame, not the end of a compile, and stderr cannot fill a pipe.
class Helper {
public:
  explicit Helper(const QString &p_root) : config(wireConfig(p_root)) {}
  ~Helper() {
    if (process.state() != QProcess::NotRunning) {
      process.closeWriteChannel();
      if (!process.waitForFinished(1000)) {
        process.kill();
        process.waitForFinished(5000);
      }
    }
  }

  bool start() {
    if (!QFileInfo(helperPath()).isFile())
      return fail(QStringLiteral("Missing built helper: ") + helperPath());
    process.setProgram(helperPath());
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start();
    if (!process.waitForStarted(10000))
      return fail(process.errorString());
    if (!request({{QStringLiteral("op"), QStringLiteral("initialize")},
                  {QStringLiteral("config"), config}}))
      return false;
    return reply.header.value(QStringLiteral("op")) == QStringLiteral("ready") ||
           fail(QString::fromUtf8(QJsonDocument(reply.header).toJson()));
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
    p_header.insert(QStringLiteral("v"), 1);
    p_header.insert(QStringLiteral("epoch"), epoch);
    p_header.insert(QStringLiteral("generation"), generation);
    p_header.insert(QStringLiteral("request"), ++requestId);
    const auto bytes = QJsonDocument(p_header).toJson(QJsonDocument::Compact);
    char prefix[8];
    qToBigEndian<quint32>(quint32(bytes.size()), reinterpret_cast<uchar *>(prefix));
    qToBigEndian<quint32>(0, reinterpret_cast<uchar *>(prefix + 4));
    QElapsedTimer timer;
    timer.start();
    if (process.write(prefix, 8) != 8 || process.write(bytes) != bytes.size()) {
      return fail(QStringLiteral("Could not queue helper request"));
    }
    while (process.bytesToWrite()) {
      drainStderr();
      if (timer.elapsed() >= operationTimeout ||
          !process.waitForBytesWritten(qMax(1, operationTimeout - int(timer.elapsed())))) {
        return fail(QStringLiteral("Timed out writing helper request"));
      }
    }
    bool sawParsed = false;
    for (;;) {
      if (!readFrame(reply, timer))
        return false;
      for (const auto *key : {"v", "epoch", "generation", "request"}) {
        if (reply.header.value(QLatin1String(key)) != p_header.value(QLatin1String(key))) {
          return fail(QStringLiteral("Helper reply identity mismatch: ") + QLatin1String(key));
        }
      }
      const auto op = reply.header.value(QStringLiteral("op")).toString();
      if (op != QStringLiteral("parsed")) {
        if (p_header.value(QStringLiteral("op")) == QStringLiteral("compile") && !sawParsed) {
          return fail(QStringLiteral("Compile terminated without its parsed frame"));
        }
        return true;
      }
      if (sawParsed || p_header.value(QStringLiteral("op")) != QStringLiteral("compile") ||
          !reply.payload.isEmpty())
        return fail(QStringLiteral("Unexpected parsed frame"));
      sawParsed = true;
      parsed = reply.header;
    }
  }

  bool pdf(const QJsonValue &p_snapshot) {
    return request(
        {{QStringLiteral("op"), QStringLiteral("pdf")}, {QStringLiteral("snapshot"), p_snapshot}});
  }

  QString details() const {
    return error + QLatin1Char('\n') + QString::fromUtf8(QJsonDocument(reply.header).toJson()) +
           QLatin1Char('\n') + QString::fromUtf8(stderrBytes.right(4096));
  }

  QJsonObject config;
  Reply reply;
  QJsonObject parsed;

private:
  bool fail(const QString &p_error) {
    error = p_error;
    drainStderr();
    return false;
  }
  void drainStderr() { stderrBytes = (stderrBytes + process.readAllStandardError()).right(16384); }
  bool readBytes(QByteArray &p_bytes, int p_size, QElapsedTimer &p_timer) {
    p_bytes.clear();
    p_bytes.reserve(p_size);
    while (p_bytes.size() < p_size) {
      drainStderr();
      const auto chunk = process.read(p_size - p_bytes.size());
      p_bytes.append(chunk);
      if (p_bytes.size() == p_size)
        return true;
      if (p_timer.elapsed() >= operationTimeout)
        return fail(QStringLiteral("Timed out reading helper frame"));
      if (process.state() == QProcess::NotRunning)
        return fail(QStringLiteral("Helper exited during a frame"));
      process.waitForReadyRead(qMax(1, qMin(100, operationTimeout - int(p_timer.elapsed()))));
    }
    return true;
  }
  bool readFrame(Reply &p_reply, QElapsedTimer &p_timer) {
    QByteArray prefix;
    if (!readBytes(prefix, 8, p_timer))
      return false;
    const auto headerSize =
        qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(prefix.constData()));
    const auto payloadSize =
        qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(prefix.constData() + 4));
    if (!headerSize || headerSize > 16 * 1024 * 1024 || payloadSize > 128 * 1024 * 1024) {
      return fail(QStringLiteral("Helper returned oversized frame lengths"));
    }
    QByteArray header;
    if (!readBytes(header, int(headerSize), p_timer) ||
        !readBytes(p_reply.payload, int(payloadSize), p_timer))
      return false;
    QJsonParseError parseError;
    const auto json = QJsonDocument::fromJson(header, &parseError);
    if (parseError.error != QJsonParseError::NoError || !json.isObject())
      return fail(QStringLiteral("Invalid JSON helper frame"));
    p_reply.header = json.object();
    return true;
  }
  QProcess process;
  QString error;
  QByteArray stderrBytes;
  // Deliberately exercise exact wire IDs beyond a 32-bit integer.
  const double epoch = 4294967301.0;
  double generation = 1;
  double requestId = 4294967311.0;
};

class Pdf {
public:
  explicit Pdf(const QByteArray &p_bytes) : bytes(p_bytes), buffer(&bytes), document(nullptr) {}
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

QSharedPointer<TypstEditorConfig> editorConfig(const QString &p_root) {
  auto config = QSharedPointer<TypstEditorConfig>::create();
  config->m_helperExecutable = helperPath();
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
  explicit EditorFixture(const QString &p_root, const QString &p_source,
                         VTypstEditor::Mode p_mode = VTypstEditor::Mode::Read,
                         QSharedPointer<TypstEditorConfig> p_config = {})
      : editor(p_config ? p_config : editorConfig(p_root),
               QSharedPointer<TextEditorParameters>::create()) {
    editor.getTextEditor()->setSpellCheckEnabled(false);
    editor.setFileOverlay(QStringLiteral("main.typ"), p_source);
    editor.setMode(p_mode);
    editor.resize(1000, 650);
    editor.show();
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
  return QStringLiteral(
      "#set page(width: 360pt, height: 900pt, margin: (x: 28pt, y: 96pt))\n"
      "#set text(size: 12pt, fill: red)\n"
      "Sharp text: affine office minimum integral.\n\n"
      "Fractions and roots remain crisp at screen resolution.\n\n"
      "$ (a^2 + b^2)/(1 + sqrt(1 + x^2)) = sum_(k=1)^8 1/k^2 $\n\n"
      "Fine strokes, curved letters, and mathematical symbols.\n"
      "#pagebreak()\n"
      "Offscreen neighbor: prefetch must not soften the visible page.\n"
      "$ integral_0^1 x^2 dif x = 1/3 $");
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
  const qreal scale = p_pane->property("zoom").toDouble() *
                      (qMax(qreal(1), qreal(p_pane->logicalDpiY())) / 72.0);
  const QSizeF size = p_points * scale;
  return QRectF(QPointF((p_pane->viewport()->width() - size.width()) / 2, 16), size);
}

QSize physicalPdfSize(const QRectF &p_page, qreal p_dpr) {
  return QSize(int(std::ceil(p_page.width() * p_dpr)),
               int(std::ceil(p_page.height() * p_dpr)));
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
  return QRectF(interior.topLeft() * dpr, interior.size() * dpr).toAlignedRect()
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

bool writeTypstTheme(const QString &p_path, const QString &p_stringColor,
                     bool p_decorated = true) {
  auto theme = QJsonDocument::fromJson(QStringLiteral(R"({
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
  })").arg(p_stringColor, p_decorated ? QStringLiteral("true") : QStringLiteral("false"))
                   .toUtf8()).object();
  if (!p_decorated) {
    auto styles = theme.value(QStringLiteral("typst-syntax-styles")).toObject();
    styles.insert(QStringLiteral("raw"), QJsonObject{{QStringLiteral("text-color"), "#886600"}});
    theme.insert(QStringLiteral("typst-syntax-styles"), styles);
  }
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

void TestTypstEditor::compilerGeometryAndScale() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  auto config = *editorConfig(root.path());
  TypstCompiler compiler(config);
  // Construction takes a copy: later caller changes must not change this compiler.
  config.m_helperExecutable = root.filePath(QStringLiteral("missing-helper"));
  config.m_maxRasterPixels = 1;
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

  TypstCompiler compiler(*editorConfig(root.path()));
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
  TypstCompiler compiler(*editorConfig(root.path()));
  QSignalSpy completed(&compiler, &TypstCompiler::finished);
  bool returnedFromSubmit = false;
  bool asynchronous = true;
  bool idleAtCompletion = true;
  bool resubmitted = false;
  int callbackCount = 0;
  connect(&compiler, &TypstCompiler::finished, &compiler,
          [&](const TypstCompiler::Result &) {
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
  auto config = *editorConfig(root.path());
  config.m_maxRasterPixels = pixelLimit;
  TypstCompiler compiler(config);
  QSignalSpy completed(&compiler, &TypstCompiler::finished);
  const auto source = QStringLiteral(
      "#set page(width: 64pt, height: 64pt, margin: 0pt)\n"
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
  TypstCompiler baseline(*editorConfig(root.path()));
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
  auto config = *editorConfig(root.path());
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
  Helper helper(root.path());
  QVERIFY2(helper.start(), qPrintable(helper.details()));

  QVERIFY2(helper.compile(QStringLiteral("#let broken = (")), qPrintable(helper.details()));
  QCOMPARE(helper.reply.header.value(QStringLiteral("op")).toString(), QStringLiteral("compiled"));
  QVERIFY(!helper.reply.header.contains(QStringLiteral("snapshot")));
  QVERIFY(!helper.reply.header.contains(QStringLiteral("pages")));
  const auto diagnostics = helper.reply.header.value(QStringLiteral("diagnostics")).toArray();
  QVERIFY(!diagnostics.isEmpty());
  QCOMPARE(diagnostics.first().toObject().value(QStringLiteral("file")).toString(),
           QStringLiteral("main.typ"));
  QCOMPARE(diagnostics.first().toObject().value(QStringLiteral("severity")).toString(),
           QStringLiteral("error"));
  QVERIFY2(helper.pdf(1), qPrintable(helper.details()));
  QCOMPARE(helper.reply.header.value(QStringLiteral("op")).toString(), QStringLiteral("error"));
  QVERIFY(helper.reply.payload.isEmpty());

  const auto source =
      shortSource(QStringLiteral("#import \"chapter.typ\": phrase\nOverlay main: #phrase"));
  QVERIFY2(helper.compile(source, {}, importOverlay(QStringLiteral("First import."))),
           qPrintable(helper.details()));
  const auto first = helper.reply.header.value(QStringLiteral("snapshot"));
  QVERIFY2(first.toDouble() > 0, qPrintable(helper.details()));
  QVERIFY2(helper.pdf(first), qPrintable(helper.details()));
  Pdf firstPdf(helper.reply.payload);
  QVERIFY(firstPdf.load());
  QCOMPARE(firstPdf.text(), QStringLiteral("Overlay main: First import."));

  QVERIFY2(
      helper.compile(source, {first}, importOverlay(QStringLiteral("Changed unsaved import."))),
      qPrintable(helper.details()));
  const auto second = helper.reply.header.value(QStringLiteral("snapshot"));
  QVERIFY(second.toDouble() > first.toDouble());
  QVERIFY2(helper.pdf(second), qPrintable(helper.details()));
  Pdf secondPdf(helper.reply.payload);
  QVERIFY(secondPdf.load());
  QCOMPARE(secondPdf.text(), QStringLiteral("Overlay main: Changed unsaved import."));

  QVERIFY2(helper.compile(QStringLiteral("#let broken = ("), {second}),
           qPrintable(helper.details()));
  QVERIFY(!helper.reply.header.contains(QStringLiteral("snapshot")));
  QVERIFY(!helper.reply.header.value(QStringLiteral("diagnostics")).toArray().isEmpty());
  QVERIFY2(helper.pdf(second), qPrintable(helper.details()));
  Pdf retained(helper.reply.payload);
  QVERIFY(retained.load());
  QCOMPARE(retained.text(), secondPdf.text());
  QVERIFY2(helper.pdf(first), qPrintable(helper.details()));
  QCOMPARE(helper.reply.header.value(QStringLiteral("op")).toString(), QStringLiteral("error"));

  QVERIFY2(helper.compile(shortSource(QStringLiteral("Recovered source.")), {second}),
           qPrintable(helper.details()));
  const auto recovered = helper.reply.header.value(QStringLiteral("snapshot"));
  QVERIFY(recovered.toDouble() > second.toDouble());
  QVERIFY(helper.reply.header.value(QStringLiteral("diagnostics")).toArray().isEmpty());
  QVERIFY2(helper.pdf(recovered), qPrintable(helper.details()));
  Pdf recoveredPdf(helper.reply.payload);
  QVERIFY(recoveredPdf.load());
  QCOMPARE(recoveredPdf.text(), QStringLiteral("Recovered source."));
  QVERIFY2(helper.request({{QStringLiteral("op"), QStringLiteral("retain")},
                           {QStringLiteral("snapshots"), QJsonArray{recovered}}}),
           qPrintable(helper.details()));
  QVERIFY2(helper.pdf(second), qPrintable(helper.details()));
  QCOMPARE(helper.reply.header.value(QStringLiteral("op")).toString(), QStringLiteral("error"));
  QFile mainFile(mainPath), importedFile(importPath);
  QVERIFY(mainFile.open(QIODevice::ReadOnly));
  QVERIFY(importedFile.open(QIODevice::ReadOnly));
  QCOMPARE(mainFile.readAll(), diskMain);
  QCOMPARE(importedFile.readAll(), diskImport);
}

void TestTypstEditor::unicodeNavigationPdfPointsAndRasterLimits() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  Helper helper(root.path());
  QVERIFY2(helper.start(), qPrintable(helper.details()));
  const auto source = shortSource(QString::fromUtf8(
      "// non-BMP: \xf0\x9f\x98\x80\nFirst page.\n#pagebreak()\nNavigationTarget"));
  QVERIFY2(helper.compile(source), qPrintable(helper.details()));
  const auto snapshot = helper.reply.header.value(QStringLiteral("snapshot"));
  QVERIFY2(snapshot.toDouble() > 0, qPrintable(helper.details()));
  const auto pages = helper.reply.header.value(QStringLiteral("pages")).toArray();
  QCOMPARE(pages.size(), 2);
  QVERIFY2(helper.pdf(snapshot), qPrintable(helper.details()));
  Pdf pdf(helper.reply.payload);
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
  QVERIFY2(helper.request({{QStringLiteral("op"), QStringLiteral("sourceToOutput")},
                           {QStringLiteral("snapshot"), snapshot},
                           {QStringLiteral("file"), QStringLiteral("main.typ")},
                           {QStringLiteral("offset"), start + 4}}),
           qPrintable(helper.details()));
  const auto locations = helper.reply.header.value(QStringLiteral("locations")).toArray();
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
  QVERIFY2(helper.request({{QStringLiteral("op"), QStringLiteral("outputToSource")},
                           {QStringLiteral("snapshot"), snapshot},
                           {QStringLiteral("page"), 1},
                           {QStringLiteral("x"), textBounds.center().x()},
                           {QStringLiteral("y"), textBounds.center().y()}}),
           qPrintable(helper.details()));
  const auto mapped = helper.reply.header.value(QStringLiteral("location")).toObject();
  QCOMPARE(mapped.value(QStringLiteral("file")).toString(), QStringLiteral("main.typ"));
  QVERIFY(mapped.value(QStringLiteral("start")).toInt(-1) >= start);
  QVERIFY(mapped.value(QStringLiteral("end")).toInt(-1) >=
          mapped.value(QStringLiteral("start")).toInt());
  QVERIFY(mapped.value(QStringLiteral("end")).toInt(-1) <=
          start + QStringLiteral("NavigationTarget").size());

  const int emoji = source.indexOf(QChar(0xd83d));
  QVERIFY(emoji >= 0);
  QVERIFY2(helper.request({{QStringLiteral("op"), QStringLiteral("sourceToOutput")},
                           {QStringLiteral("snapshot"), snapshot},
                           {QStringLiteral("file"), QStringLiteral("main.typ")},
                           {QStringLiteral("offset"), emoji + 1}}),
           qPrintable(helper.details()));
  QCOMPARE(helper.reply.header.value(QStringLiteral("op")).toString(), QStringLiteral("error"));
  QElapsedTimer rejectionTime;
  rejectionTime.start();
  QVERIFY2(helper.request({{QStringLiteral("op"), QStringLiteral("raster")},
                           {QStringLiteral("snapshot"), snapshot},
                           {QStringLiteral("page"), 1},
                           {QStringLiteral("scale"), 1000000}}),
           qPrintable(helper.details()));
  QCOMPARE(helper.reply.header.value(QStringLiteral("op")).toString(), QStringLiteral("error"));
  QVERIFY(helper.reply.payload.isEmpty());
  QVERIFY2(rejectionTime.elapsed() < 5000,
           "Invalid raster dimensions must be rejected before rendering/allocation");
  QVERIFY2(helper.request({{QStringLiteral("op"), QStringLiteral("raster")},
                           {QStringLiteral("snapshot"), snapshot},
                           {QStringLiteral("page"), 1},
                           {QStringLiteral("scale"), 2}}),
           qPrintable(helper.details()));
  QCOMPARE(helper.reply.header.value(QStringLiteral("op")).toString(), QStringLiteral("raster"));
  QCOMPARE(helper.reply.header.value(QStringLiteral("width")).toInt(), 480);
  QCOMPARE(helper.reply.header.value(QStringLiteral("height")).toInt(), 320);
  QCOMPARE(helper.reply.header.value(QStringLiteral("stride")).toInt(), 1920);
  QCOMPARE(helper.reply.payload.size(), 480 * 320 * 4);
  const QImage image(reinterpret_cast<const uchar *>(helper.reply.payload.constData()), 480, 320,
                     1920, QImage::Format_RGBA8888_Premultiplied);
  const QRect textPixels = QRectF(textBounds.topLeft() * 2, textBounds.size() * 2).toAlignedRect();
  QVERIFY2(inkPixels(image, textPixels.adjusted(-2, -2, 2, 2)) > 50,
           "Original-page raster ink must agree with PDF text point geometry");
  QCOMPARE(inkPixels(image, QRect(0, 0, 20, 20)), 0);

  const auto bleedSource =
      QStringLiteral("#set page(width:200pt,height:100pt,margin:10pt,bleed:12pt)\n"
                     "= Target <target>\n#link(<target>)[Go]");
  QVERIFY2(helper.compile(bleedSource), qPrintable(helper.details()));
  const auto bleedSnapshot = helper.reply.header.value(QStringLiteral("snapshot"));
  QVERIFY2(bleedSnapshot.toDouble() > 0, qPrintable(helper.details()));
  const auto trimPages = helper.reply.header.value(QStringLiteral("pages")).toArray();
  QCOMPARE(trimPages.size(), 1);
  QCOMPARE(trimPages.first().toArray(), QJsonArray({200, 100}));
  const auto links = helper.reply.header.value(QStringLiteral("links")).toArray();
  QVERIFY(!links.isEmpty());
  QVERIFY2(helper.pdf(bleedSnapshot), qPrintable(helper.details()));
  Pdf trimPdf(helper.reply.payload);
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

  EditorFixture navigation(root.path(), source, VTypstEditor::Mode::InPlacePreview);
  QVERIFY2(navigation.ready(), qPrintable(navigation.editor.compileMessage()));
  QTextCursor cursor(navigation.editor.document());
  cursor.setPosition(start + 4);
  navigation.edit()->setTextCursor(cursor);
  QSignalSpy published(navigation.pane(), SIGNAL(snapshotPublished(quint64)));
  navigation.editor.showOutputAtCursor();
  QVERIFY2(waitUntil([&]() { return published.count() == 1; }),
           qPrintable(navigation.editor.compileMessage()));
  QCOMPARE(navigation.editor.mode(), VTypstEditor::Mode::SideBySidePreview);
  QCOMPARE(navigation.selectPage().trimmed(), QStringLiteral("NavigationTarget"));
  QCOMPARE(navigation.edit()->textCursor().position(), start + 4);
  QCOMPARE(navigation.editor.document()->toPlainText(), source);
}

void TestTypstEditor::previewClickNavigatesSource() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  const auto target = QStringLiteral("NavigationTarget");
  const auto source =
      QStringLiteral("#set page(width: 240pt, height: 700pt, margin: 24pt)\n") +
      QString::fromUtf8("// non-BMP: \xf0\x9f\x98\x80\n") +
      QStringLiteral("// source padding\n").repeated(80) +
      QStringLiteral("First page.\n#pagebreak()\n") + target;
  EditorFixture fixture(root.path(), source, VTypstEditor::Mode::SideBySidePreview);
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

  for (const auto mode : {VTypstEditor::Mode::SideBySidePreview, VTypstEditor::Mode::Read}) {
    fixture.editor.setMode(mode);
    fixture.edit()->setTextCursor(QTextCursor(document));
    QTest::qWait(100); // Let the mode's viewport geometry settle before fitting.
    fixture.editor.fitPdfWidth();
    fixture.editor.setPdfZoom(fixture.editor.pdfZoom() * 0.85);
    fixture.editor.goToPdfPage(1);
    const auto page = firstPdfPageRect(pane, pdf.size(1));
    const QPoint click = (page.topLeft() + bounds.center() * (page.width() / pdf.size(1).width()))
                             .toPoint();
    QVERIFY(pane->viewport()->rect().contains(click));
    QTest::mouseClick(pane->viewport(), Qt::LeftButton, Qt::NoModifier, click);
    QVERIFY2(waitUntil([&]() {
      const auto cursor = fixture.edit()->textCursor();
      return cursor.position() >= start && cursor.position() <= start + target.size() &&
             fixture.edit()->hasFocus();
    }), qPrintable(fixture.editor.compileMessage()));
    QCOMPARE(fixture.editor.mode(), VTypstEditor::Mode::SideBySidePreview);
    QVERIFY(fixture.edit()->isVisible());
    QVERIFY(!fixture.edit()->textCursor().hasSelection());
    QVERIFY2(waitUntil([&]() {
      return fixture.edit()->viewport()->rect().intersects(fixture.edit()->cursorRect());
    }, 2000), qPrintable(QStringLiteral("mode=%1 cursorY=%2 viewportHeight=%3 scroll=%4/%5")
                            .arg(int(mode)).arg(fixture.edit()->cursorRect().y())
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
  const auto source = shortSource(target + QStringLiteral(
      "\n\n#link(\"https://example.org/\")[Visit link]"));
  EditorFixture fixture(root.path(), source, VTypstEditor::Mode::SideBySidePreview);
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
  QCOMPARE(fixture.edit()->textCursor().position(), 0);

  QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier, QPoint(1, 1));
  // Direct events avoid synthesizing a blocking context menu for the right-button check.
  const QPointF click = point(bounds.center());
  QMouseEvent rightPress(QEvent::MouseButtonPress, click, viewport->mapToGlobal(click.toPoint()),
                        Qt::RightButton, Qt::RightButton, Qt::NoModifier);
  QMouseEvent rightRelease(QEvent::MouseButtonRelease, click,
                          viewport->mapToGlobal(click.toPoint()), Qt::RightButton,
                          Qt::NoButton, Qt::NoModifier);
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

  fixture.editor.setFileOverlay(QStringLiteral("main.typ"), source + QStringLiteral("\n#let x = ("));
  QVERIFY(waitUntil([&]() {
    return fixture.editor.compileStatus() == VTypstEditor::CompileStatus::Error;
  }));
  QCOMPARE(published.count(), 1); // Last-good PDF remains displayed, but its mapping is stale.
  const auto cursor = fixture.edit()->textCursor();
  QSignalSpy navigation(pane, SIGNAL(sourceLocationRequested(quint64,int,QPointF)));
  QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier, click.toPoint());
  QCOMPARE(navigation.count(), 0);
  QCOMPARE(fixture.edit()->textCursor().position(), cursor.position());
  QCOMPARE(fixture.edit()->textCursor().anchor(), cursor.anchor());
}

void TestTypstEditor::repeatedAndSplitOutputRecords() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  Helper helper(root.path());
  QVERIFY2(helper.start(), qPrintable(helper.details()));
  const auto source = QStringLiteral("#set page(width: 100mm, height: 80mm, margin: 8mm)\n"
                                     "#for n in (2, 7, 11) { [Before $ E = #n c^2 $ after.] }");
  QVERIFY2(helper.compile(source), qPrintable(helper.details()));
  const auto records = helper.reply.header.value(QStringLiteral("previews")).toArray();
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
  const auto repeatedSnapshot = helper.reply.header.value(QStringLiteral("snapshot"));
  QVERIFY2(helper.pdf(repeatedSnapshot), qPrintable(helper.details()));
  Pdf repeated(helper.reply.payload);
  QVERIFY(repeated.load());
  QCOMPARE(repeated.text().count(QStringLiteral("Before")), 3);
  QVERIFY(repeated.text().contains(QStringLiteral("11")));
  QVERIFY(repeated.text().contains(QLatin1Char('7')));

  QVERIFY2(helper.compile(splitSource()), qPrintable(helper.details()));
  const auto split = helper.reply.header.value(QStringLiteral("previews")).toArray();
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
  const auto splitSnapshot = helper.reply.header.value(QStringLiteral("snapshot"));
  QVERIFY2(helper.pdf(splitSnapshot), qPrintable(helper.details()));
  Pdf splitPdf(helper.reply.payload);
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
  const auto main = QStringLiteral(
      "#set heading(numbering: \"1.\", offset: 1, bookmarked: false)\n"
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
      "#outline()\n").arg(unicode);
  const auto chapter = QStringLiteral("// %1 before the imported range\n"
                                      "#let chapter = [\n  == _Imported_ %1\n]\n").arg(unicode);
  EditorFixture fixture(root.path(), main);
  fixture.editor.setFileOverlay(QStringLiteral("chapter.typ"), chapter);
  QSignalSpy updates(&fixture.editor, &VTypstEditor::headingsUpdated);
  QVERIFY(updates.isValid());
  QVERIFY(fixture.editor.getHeadings().isEmpty());
  QVERIFY2(waitUntil([&]() { return !updates.isEmpty(); }),
           qPrintable(fixture.editor.compileMessage()));

  const QVector<TypstHeadingInfo> headings = fixture.editor.getHeadings();
  const auto payload = qvariant_cast<QVector<TypstHeadingInfo>>(updates.last().at(0));
  const QStringList titles{QStringLiteral("Main ") + unicode,
                           QStringLiteral("Imported ") + unicode,
                           QStringLiteral("First"), QStringLiteral("Second"), QString()};
  const QVector<int> levels{2, 3, 4, 4, 8};
  QCOMPARE(headings.size(), titles.size());
  QCOMPARE(payload.size(), headings.size());
  for (int index = 0; index < headings.size(); ++index) {
    const auto &heading = headings.at(index);
    QCOMPARE(heading.m_title, titles.at(index));
    QCOMPARE(heading.m_level, levels.at(index));
    QCOMPARE(heading.m_file, index == 1 ? QStringLiteral("chapter.typ")
                                      : QStringLiteral("main.typ"));
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
    QCOMPARE(headings.at(index).m_endPos,
             main.indexOf(generatedHeading) + generatedHeading.size());
  }
}

void TestTypstEditor::outlineInvalidationFailureAndRecovery() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  EditorFixture fixture(root.path(), shortSource(QStringLiteral("= Before")),
                        VTypstEditor::Mode::SideBySidePreview);
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
  QVERIFY(waitUntil([&]() {
    return fixture.editor.compileStatus() == VTypstEditor::CompileStatus::Error;
  }));
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
  EditorFixture fixture(root.path(), main, VTypstEditor::Mode::SideBySidePreview);
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
  QVERIFY2(waitUntil([&]() {
             return !updates.isEmpty() && fixture.editor.getHeadings().size() == 3;
           }),
           qPrintable(fixture.editor.compileMessage()));
  const QVector<TypstHeadingInfo> retained = fixture.editor.getHeadings();
  QCOMPARE(retained.at(0).m_title, QStringLiteral("Main"));
  QCOMPARE(retained.at(1).m_title, QStringLiteral("Imported"));
  QCOMPARE(retained.at(2).m_title, QStringLiteral("End"));
  const int beforeModes = updates.count();
  for (const auto mode : {VTypstEditor::Mode::Read, VTypstEditor::Mode::InPlacePreview,
                          VTypstEditor::Mode::SideBySidePreview, VTypstEditor::Mode::Read}) {
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
  EditorFixture fixture(root.path(), shortSource(QStringLiteral("= Baseline")));
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

void TestTypstEditor::modesPreserveSourceUndoAndSelection() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  const auto original = shortSource(QStringLiteral("Same source document."));
  EditorFixture fixture(root.path(), original, VTypstEditor::Mode::InPlacePreview);
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
  for (const auto mode :
       {VTypstEditor::Mode::SideBySidePreview, VTypstEditor::Mode::Read,
        VTypstEditor::Mode::SideBySidePreview, VTypstEditor::Mode::InPlacePreview}) {
    fixture.editor.setMode(mode);
    QCoreApplication::processEvents();
    QCOMPARE(document->toPlainText(), edited);
    QCOMPARE(fixture.edit()->textCursor().anchor(), cursor.anchor());
    QCOMPARE(fixture.edit()->textCursor().position(), cursor.position());
    QCOMPARE(document->availableUndoSteps(), undoSteps);
    QVERIFY(document->isModified());
    QCOMPARE(fixture.edit()->isReadOnly(), mode == VTypstEditor::Mode::Read);
    QCOMPARE(fixture.editor.getTextEditor()->isHidden(), mode == VTypstEditor::Mode::Read);
  }
  QCOMPARE(textChangeCount(changes), 0);
  fixture.editor.setReadOnly(true);
  fixture.editor.setMode(VTypstEditor::Mode::Read);
  fixture.editor.setMode(VTypstEditor::Mode::InPlacePreview);
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
  EditorFixture fixture(root.path(), shortSource(QStringLiteral(
                                         "#import \"chapter.typ\": phrase\nInitial: #phrase")));
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

void TestTypstEditor::diagnosticsPreserveLastGoodOutput() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  EditorFixture fixture(root.path(), QStringLiteral("#let broken = ("));
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
  auto config = editorConfig(root.path());
  config->m_maxCacheBytes = 8 * 1024 * 1024;
  const auto pages =
      QStringLiteral("#set page(width: 320pt, height: 400pt, margin: 20pt)\n"
                     "Before update.\n#pagebreak()\nMiddle page.\n#pagebreak()\nLast page.");
  EditorFixture fixture(root.path(), pages, VTypstEditor::Mode::SideBySidePreview, config);
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
  fixture.editor.setMode(VTypstEditor::Mode::SideBySidePreview);
  QCOMPARE(fixture.selected(), selected);
  QCOMPARE(published.count(), 1);
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

void TestTypstEditor::sideBySideScrollSync() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  QString source = QStringLiteral("#set page(width: 320pt, height: 440pt, margin: 20pt)\n");
  for (int line = 0; line < 120; ++line) {
    source += QStringLiteral("Line %1: scrolling keeps both views together.\n\n").arg(line);
  }
  EditorFixture fixture(root.path(), source, VTypstEditor::Mode::SideBySidePreview);
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
    const qreal progress = qreal(p_from->value() - p_from->minimum()) /
                           (p_from->maximum() - p_from->minimum());
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
  fixture.editor.setMode(VTypstEditor::Mode::SideBySidePreview);
  QCoreApplication::processEvents();
  QCOMPARE(sourceBar->value(), hiddenSource);
  sourceBar->setValue(sourceBar->maximum() / 2);
  QCOMPARE(pdfBar->value(), mapped(sourceBar, pdfBar));
  fixture.editor.setMode(VTypstEditor::Mode::InPlacePreview);
  QCoreApplication::processEvents();
  QCOMPARE(fixture.selected(), selectedPdf);
  const int hiddenPdf = pdfBar->value();
  sourceBar->setValue(0);
  QCOMPARE(pdfBar->value(), hiddenPdf);
  fixture.editor.setMode(VTypstEditor::Mode::SideBySidePreview);
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
  EditorFixture fixture(root.path(), source, VTypstEditor::Mode::SideBySidePreview);
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
  Helper helper(root.path());
  QVERIFY2(helper.start(), qPrintable(helper.details()));
  QVERIFY2(helper.compile(source), qPrintable(helper.details()));
  const auto snapshot = helper.reply.header.value(QStringLiteral("snapshot"));
  QVERIFY2(helper.request({{QStringLiteral("op"), QStringLiteral("sourceToOutput")},
                           {QStringLiteral("snapshot"), snapshot},
                           {QStringLiteral("file"), QStringLiteral("main.typ")},
                           {QStringLiteral("offset"), offset}}), qPrintable(helper.details()));
  const auto locations = helper.reply.header.value(QStringLiteral("locations")).toArray();
  QVERIFY(!locations.isEmpty());
  const auto location = locations.first().toObject();
  QCOMPARE(location.value(QStringLiteral("page")).toInt(), 0);
  const QPointF point(location.value(QStringLiteral("x")).toDouble(),
                      location.value(QStringLiteral("y")).toDouble());

  EditorFixture fixture(root.path(), source, VTypstEditor::Mode::SideBySidePreview);
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
  const auto source = QStringLiteral(
                          "#set page(width: 240pt, height: 700pt, margin: (x: 24pt, y: 100pt))\n") +
                      QStringLiteral("// source padding\n").repeated(100) +
                      QStringLiteral("First target.\n#pagebreak()\nLast target.");
  EditorFixture fixture(root.path(), source, VTypstEditor::Mode::SideBySidePreview);
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
  QVERIFY(waitUntil([&]() {
    QMetaObject::invokeMethod(pane, "currentPage", Qt::DirectConnection, Q_RETURN_ARG(int, page));
    return page == 1;
  }, paintTimeout));
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
                           &observer, observer.metaObject()->method(
                               observer.metaObject()->indexOfSlot("snapshotPublished(quint64)"))));
  fixture.clearSelection();
  QVERIFY2(waitUntil([&]() { return published.count() == 2; }),
           qPrintable(fixture.editor.compileMessage()));
  QVERIFY(waitUntil([&]() {
    return widgetImage(pane->viewport()).copy(content) != newFrame.copy(content);
  }, paintTimeout));
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
  const auto source = QStringLiteral(
      "#set page(width: 320pt, height: 700pt, margin: 30pt)\n"
      "#outline()\n#pagebreak()\n= Actual title\nBody below the title.");
  EditorFixture fixture(root.path(), source, VTypstEditor::Mode::SideBySidePreview);
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
  QVERIFY(waitUntil([&]() {
    QMetaObject::invokeMethod(pane, "currentPage", Qt::DirectConnection, Q_RETURN_ARG(int, page));
    return page == 1;
  }, paintTimeout));
  QVERIFY(fixture.selectPage().contains(QStringLiteral("Body below the title.")));

  // Explicit navigation shares the same mapping, including a cursor at heading EOL.
  QVERIFY(QMetaObject::invokeMethod(pane, "goToPage", Qt::DirectConnection, Q_ARG(int, 0),
                                    Q_ARG(QPointF, QPointF())));
  cursor.setPosition(source.indexOf(QStringLiteral("\nBody below")));
  fixture.edit()->setTextCursor(cursor);
  fixture.editor.showOutputAtCursor();
  QVERIFY(waitUntil([&]() {
    QMetaObject::invokeMethod(pane, "currentPage", Qt::DirectConnection, Q_RETURN_ARG(int, page));
    return page == 1;
  }, paintTimeout));
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
  EditorFixture fixture(root.path(), source, VTypstEditor::Mode::SideBySidePreview);
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
  fixture.editor.setMode(VTypstEditor::Mode::InPlacePreview);
  QVERIFY(waitUntil([&]() { return top() > baseline; }, paintTimeout));
  QVERIFY2(
      waitUntil([&]() {
        auto *card =
            fixture.editor.findChild<QWidget *>(QStringLiteral("typstPreviewCard"));
        return card && card->isVisible() && coloredPixels(widgetImage(card), true) > 20;
      }),
      qPrintable(fixture.editor.compileMessage()));
  auto *card =
      fixture.editor.findChild<QWidget *>(QStringLiteral("typstPreviewCard"));
  const QPoint blankPoint(card->geometry().right() - 4, card->geometry().center().y());
  const auto previewSurface = widgetImage(fixture.edit()->viewport());
  // The source pane is narrower before switching out of side-by-side mode.
  const QPoint sourceBlank(sourceSurface.width() - 4, blankPoint.y());
  QCOMPARE(previewSurface.pixelColor(blankPoint), sourceSurface.pixelColor(sourceBlank));
  const auto viewportRect = card->rect();
  for (const auto &point : {viewportRect.center(),
                            QPoint(viewportRect.right() - 1, viewportRect.center().y())}) {
    QTest::mouseClick(card, Qt::LeftButton, Qt::NoModifier, point);
    QCoreApplication::processEvents();
    QCOMPARE(fixture.editor.mode(), VTypstEditor::Mode::InPlacePreview);
    QCOMPARE(fixture.edit()->textCursor().anchor(), selection.anchor());
    QCOMPARE(fixture.edit()->textCursor().position(), selection.position());
  }
  QCOMPARE(document->toPlainText(), source);
  QCOMPARE(document->availableUndoSteps(), undoSteps);
  QVERIFY(!document->isModified());
  QCOMPARE(textChangeCount(changes), 0);
  fixture.editor.setMode(VTypstEditor::Mode::SideBySidePreview);
  QVERIFY(waitUntil([&]() { return std::abs(top() - baseline) < 1; }, paintTimeout));
  fixture.editor.setMode(VTypstEditor::Mode::InPlacePreview);
  QVERIFY(waitUntil([&]() { return top() > baseline; }, paintTimeout));
  QCOMPARE(textChangeCount(changes), 0);
  QCOMPARE(document->toPlainText(), source);
}

void TestTypstEditor::occurrencesAndSplitPartsRenderSequentially() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  auto config = editorConfig(root.path());
  config->m_previewMaxHeight = 160;
  const auto source = QStringLiteral("#set text(size: 24pt)\n#for color in (rgb(\"#ff0000\"), "
                                     "rgb(\"#0000ff\")) { text(fill: color)[$x$] } $y$\nAfter");
  EditorFixture fixture(root.path(), source, VTypstEditor::Mode::InPlacePreview, config);
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
  QVERIFY2(waitUntil([&]() {
             return repeated() && coloredPixels(widgetImage(repeated()), false) > 10;
           }),
           qPrintable(fixture.editor.compileMessage()));
  const auto sourceRect = [&](int p_position) {
    QTextCursor cursor(fixture.editor.document());
    cursor.setPosition(p_position);
    return fixture.edit()->cursorRect(cursor);
  };
  const auto scrollToSource = [&](int p_position) {
    auto *bar = fixture.edit()->horizontalScrollBar();
    bar->setValue(bar->value() + sourceRect(p_position).left() -
                  qRound(fixture.editor.document()->documentMargin()));
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

  const int fullRowWidth = repeated()->width();
  const int spanWidth = sourceRect(equationStart + 3).left() - sourceRect(equationStart).left();
  const int chromeWidth = fixture.editor.width() - fixture.edit()->viewport()->width();
  const int remaining = (spanWidth + fullRowWidth) / 2;
  fixture.editor.resize(chromeWidth + sourceRect(equationStart).left() + remaining +
                            qRound(fixture.editor.document()->documentMargin()) + 4, 650);
  QVERIFY(waitUntil([&]() {
    return repeated() && repeated()->width() < fullRowWidth &&
           coloredPixels(widgetImage(repeated()), false) > 10;
  }));
  QVERIFY(std::abs(repeated()->geometry().left() - sourceRect(equationStart).left()) <= 1);
  QVERIFY(repeated()->width() > spanWidth);
  QVERIFY(repeated()->geometry().right() < fixture.edit()->viewport()->width());

  // The claimed source is mid-line on a soft-wrapped continuation, not at its origin.
  fixture.editor.resize(450, 650);
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
  QVERIFY(waitUntil([&]() {
    const auto doc = fixture.editor.document();
    const auto block = doc->findBlock(wideStart);
    return doc->documentLayout()->documentSize().width() >
           block.layout()->lineAt(0).naturalTextWidth() + 2 * doc->documentMargin();
  }));
  scrollToSource(wideStart);
  QVERIFY(waitUntil([&]() {
    return repeated() && coloredPixels(widgetImage(repeated()), false) > 10;
  }));
  const auto wideSize = repeated()->size();
  const qreal wideDocumentWidth = fixture.editor.document()->documentLayout()->documentSize().width();
  const auto wideSelection = fixture.edit()->textCursor();
  const auto wideUndo = fixture.editor.document()->availableUndoSteps();
  const auto wideModified = fixture.editor.document()->isModified();
  fixture.editor.resize(600, 650);
  QVERIFY(waitUntil([&]() {
    return fixture.editor.document()->documentLayout()->documentSize().width() < wideDocumentWidth;
  }));
  scrollToSource(wideStart);
  QVERIFY(waitUntil([&]() {
    return repeated() && repeated()->width() < wideSize.width() &&
           coloredPixels(widgetImage(repeated()), false) > 10;
  }, paintTimeout));
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
  const auto unequal = QStringLiteral(
      "#for (color, size) in ((rgb(\"#ff0000\"), 72pt), (rgb(\"#0000ff\"), 24pt)) { "
      "text(fill: color, size: size)[$x$] }\n") + QStringLiteral("After\n").repeated(80);
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
  QVERIFY(waitUntil([&]() {
    return repeated() && coloredPixels(widgetImage(repeated()), false) > 10;
  }));
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
  editorScroll->setValue(qMax(0, int(afterRect.top()) -
                                  fixture.edit()->viewport()->height() * 3 / 4));
  QVERIFY2(waitUntil([&]() {
             return splitCard() && coloredPixels(widgetImage(splitCard()), false) > 5;
           }),
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
  EditorFixture fixture(root.path(), shortSource(QStringLiteral("Baseline.")));
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
  auto config = editorConfig(root.path());
  config->m_textEditorConfig->m_theme = Theme::createThemeFromFile(lightPath);
  QVERIFY(config->m_textEditorConfig->m_theme);
  config->m_textEditorConfig->m_syntaxTheme = QStringLiteral("Breeze Dark");
  const auto source = QStringLiteral("// comment\n= Heading\n#let value = 42\n"
                                     "#let label = \"string\"\n#text(\"call\")\n"
                                     "*strong* _emph_ `raw`\n$ x^2 $\nplain\n");
  EditorFixture fixture(root.path(), source, VTypstEditor::Mode::SideBySidePreview, config);
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
  auto darkConfig = editorConfig(root.path());
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
  auto unstyled = editorConfig(root.path());
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

void TestTypstEditor::lazyHighlightsFollowPendingEdits() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  const auto themePath = root.filePath(QStringLiteral("syntax.theme"));
  QVERIFY(writeTypstTheme(themePath, QStringLiteral("#aa2233")));
  auto config = editorConfig(root.path());
  config->m_textEditorConfig->m_theme = Theme::createThemeFromFile(themePath);
  const auto source = QStringLiteral("/* first\nsecond */\n#let label = \"string\"\nplain\n");
  EditorFixture fixture(root.path(), source, VTypstEditor::Mode::InPlacePreview, config);
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
               .foreground().color(), QColor("#ff8899"));

  cursor.setPosition(document->toPlainText().indexOf(QStringLiteral("/*")));
  cursor.deleteChar();
  cursor.deleteChar();
  QCoreApplication::processEvents();
  // Old comment semantics remain until the official parser replaces them.
  const int second = document->toPlainText().indexOf(QStringLiteral("second"));
  QCOMPARE(syntaxFormatAt(document, second).foreground().color(), QColor("#557755"));
  fixture.editor.retryCompilation();
  QVERIFY(waitUntil([&]() {
    return fixture.editor.compileStatus() != VTypstEditor::CompileStatus::Compiling;
  }));
  QVERIFY(syntaxFormatAt(document, second).foreground().color() != QColor("#557755"));
}

void TestTypstEditor::lazyPreviewsFollowPendingEdits() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  auto config = editorConfig(root.path());
  const auto source = QStringLiteral("#set text(fill: red, size: 24pt)\nBefore\n"
                                     "$x^2$\nBetween\n$ y^2 $\nAfter");
  EditorFixture fixture(root.path(), source, VTypstEditor::Mode::InPlacePreview, config);
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  const auto cards = [&]() {
    auto result = fixture.editor.findChildren<QWidget *>(QStringLiteral("typstPreviewCard"));
    std::sort(result.begin(), result.end(), [](QWidget *p_a, QWidget *p_b) {
      return p_a->y() < p_b->y();
    });
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
  QVERIFY(waitUntil([&]() {
    return fixture.editor.compileStatus() == VTypstEditor::CompileStatus::Error;
  }));
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
  EditorFixture fixture(root.path(), source, VTypstEditor::Mode::InPlacePreview);
  fixture.edit()->setLineWrapMode(QTextEdit::NoWrap);
  TypstRasterObserver observer;
  QVERIFY(observeRasters(&fixture.editor, &observer));
  QSet<int> baselinePages;
  observer.completed = [&](quint64, quint64, int p_page, qreal, const QImage &p_image) {
    if (!p_image.isNull())
      baselinePages.insert(p_page);
  };
  QVERIFY2(waitUntil([&]() {
    return baselinePages.size() >= 2 && previewHasOnlyColor(previewImages(fixture.editor), true);
  }), qPrintable(fixture.editor.compileMessage()));
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
  }), qPrintable(fixture.editor.compileMessage()));
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
  QVERIFY2(newBounds.width() > oldBounds.width() + 10, "The new glyph must use its new crop geometry");
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
  const auto source = shortSource(QStringLiteral("#set text(fill: red, size: 24pt)\n$ x $\nAfter"));
  const auto replacement = shortSource(QStringLiteral(
      "#set text(fill: rgb(\"#0000ff\"), size: 24pt)\n$ Omega^3 + Omega^2 + Omega $\nAfter"));
  EditorFixture fixture(root.path(), source, VTypstEditor::Mode::InPlacePreview);
  TypstRasterObserver observer;
  QVERIFY(observeRasters(&fixture.editor, &observer));
  quint64 baselineEpoch = 0;
  quint64 baselineSnapshot = 0;
  observer.requested = [&](quint64 p_epoch, quint64 p_snapshot, int, qreal) {
    baselineEpoch = p_epoch;
    baselineSnapshot = p_snapshot;
  };
  QVERIFY2(waitUntil([&]() {
    return baselineSnapshot && previewHasOnlyColor(previewImages(fixture.editor), true);
  }), qPrintable(fixture.editor.compileMessage()));
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
      fixture.editor.setFileOverlay(QStringLiteral("main.typ"), QStringLiteral("Plain replacement."));
      cleared = previewImages(fixture.editor).isEmpty();
      break;
    case 1:
      fixture.editor.setProject(root.path(), QStringLiteral("other.typ"));
      fixture.editor.setActiveFile(QStringLiteral("other.typ"));
      fixture.editor.setFileOverlay(QStringLiteral("other.typ"), replacement);
      cleared = previewImages(fixture.editor).isEmpty();
      break;
    case 2:
      fixture.editor.setMode(VTypstEditor::Mode::SideBySidePreview);
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
      QCOMPARE(fixture.editor.mode(), VTypstEditor::Mode::SideBySidePreview);
      QVERIFY(previewImages(fixture.editor).isEmpty());
      fixture.editor.setMode(VTypstEditor::Mode::InPlacePreview);
    }
    QVERIFY2(waitUntil([&]() {
      return previewHasOnlyColor(previewImages(fixture.editor), false);
    }), qPrintable(fixture.editor.compileMessage()));
    if (transition == 1)
      QCOMPARE(fixture.editor.activeFile(), QStringLiteral("other.typ"));
    if (transition == 3) {
      QVERIFY2(reusedId, "Retry must exercise a new helper epoch reusing its first snapshot ID");
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
  auto config = editorConfig(root.path());
  EditorFixture fixture(root.path(), QStringLiteral("// comment\n$ x^2 $\n"),
                        VTypstEditor::Mode::InPlacePreview, config);
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  QVERIFY(waitUntil([&]() {
    return fixture.editor.findChild<QWidget *>(QStringLiteral("typstPreviewCard"));
  }));
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
  auto config = editorConfig(root.path());
  config->m_textEditorConfig->m_theme = Theme::createThemeFromFile(themePath);
  QVERIFY(config->m_textEditorConfig->m_theme);
  const auto suffix = QStringLiteral("\U0001F680") + QStringLiteral(" #text(\"before\")\nplain\n");
  const auto source = QStringLiteral("/* first\nsecond */\n") + suffix;
  const auto uncommented = QStringLiteral(" first\nsecond \n") + suffix;
  EditorFixture fixture(root.path(), source, VTypstEditor::Mode::SideBySidePreview, config);
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
  const auto mode = sideBySide ? VTypstEditor::Mode::SideBySidePreview : VTypstEditor::Mode::Read;
  const auto source = sharpPdfSource();
  auto config = editorConfig(root.path());
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
    EditorFixture layout(root.path(), source, mode, config);
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
  const qint64 pagePixels = qint64(std::ceil((viewportSize.width() - 32) * dpr)) *
                            physicalSize.height();
  // P is ONE visible page at physical resolution, not a share of visible+prefetch
  // or displayed+pending+retiring slots. Bytes are deliberately non-binding.
  const int pixelBudget = int(pagePixels * 3 / 2);
  config->m_maxRasterPixels = pixelBudget;
  QVERIFY(qint64(config->m_maxCacheBytes) > qint64(pixelBudget) * 4 + 32 * 1024 * 1024);
  EditorFixture fixture(root.path(), source, mode, config);
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
  QVERIFY2(initial.meanError <= fullTolerance,
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
  }), qPrintable(fixture.editor.compileMessage()));
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
  const auto fullReplacement = paintPdfOracle(replacement, physicalSize, page, publications.front());
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
  QVERIFY2(waitUntil([&]() {
    recovered = pdfInkDifference(pane->viewport()->grab().toImage(), fullReplacement, interior);
    return recovered.pixels > 200 && recovered.meanError <= fullTolerance;
  }, paintTimeout),
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
  EditorFixture fixture(root.path(),
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

void TestTypstEditor::repeatedCloseDuringPendingOutput() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  for (int round = 0; round < 3; ++round) {
    auto fixture = std::unique_ptr<EditorFixture>(
        new EditorFixture(root.path(), shortSource(QStringLiteral("Close baseline."))));
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
  EditorFixture finalFixture(root.path(),
                             shortSource(QStringLiteral("Alive after repeated close.")));
  QSignalSpy published(finalFixture.pane(), SIGNAL(snapshotPublished(quint64)));
  QVERIFY2(waitUntil([&]() { return published.count() == 1; }),
           qPrintable(finalFixture.editor.compileMessage()));
  QCOMPARE(finalFixture.selectPage().trimmed(), QStringLiteral("Alive after repeated close."));
}

QTEST_MAIN(tests::TestTypstEditor)
