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
#include <QPdfDocument>
#include <QPdfSelection>
#include <QPointer>
#include <QProcess>
#include <QScrollBar>
#include <QSet>
#include <QSignalSpy>
#include <QSyntaxHighlighter>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <QWheelEvent>
#include <QtEndian>

#include <cmath>
#include <functional>
#include <memory>

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

int coloredPixels(const QImage &p_image, bool p_red) {
  int count = 0;
  for (int y = 0; y < p_image.height(); ++y) {
    for (int x = 0; x < p_image.width(); ++x) {
      const auto color = p_image.pixelColor(x, y);
      if (color.alpha() > 128 && color.green() < 100 &&
          (p_red ? color.red() > 170 && color.blue() < 100
                 : color.blue() > 170 && color.red() < 100))
        ++count;
    }
  }
  return count;
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

bool writeSyntaxTheme(const QString &p_path, const QString &p_name,
                      const QString &p_stringColor) {
  const QJsonObject styles{
      {QStringLiteral("Normal"), QJsonObject{{QStringLiteral("text-color"), "#202020"}}},
      {QStringLiteral("Keyword"), QJsonObject{{QStringLiteral("text-color"), "#0033aa"},
                                             {QStringLiteral("bold"), true}}},
      {QStringLiteral("Comment"), QJsonObject{{QStringLiteral("text-color"), "#557755"}}},
      {QStringLiteral("Float"), QJsonObject{{QStringLiteral("text-color"), "#aa5500"}}},
      {QStringLiteral("String"), QJsonObject{{QStringLiteral("text-color"), p_stringColor}}},
      {QStringLiteral("Function"), QJsonObject{{QStringLiteral("text-color"), "#663399"}}},
      {QStringLiteral("VerbatimString"), QJsonObject{{QStringLiteral("text-color"), "#886600"}}},
      {QStringLiteral("SpecialString"), QJsonObject{{QStringLiteral("text-color"), "#007777"}}},
      {QStringLiteral("Operator"), QJsonObject{{QStringLiteral("text-color"), "#555555"}}}};
  const QJsonObject theme{
      {QStringLiteral("metadata"), QJsonObject{{QStringLiteral("name"), p_name},
                                              {QStringLiteral("revision"), 1}}},
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
  QVERIFY(fixture.pane()->setProperty("zoom", 1.5));
  fixture.editor.setMode(VTypstEditor::Mode::Read);
  QCOMPARE(fixture.selected(), selected);
  QCOMPARE(fixture.pane()->property("zoom").toDouble(), 1.5);
  auto changed = pages;
  changed.replace(QStringLiteral("Before update."), QStringLiteral("After update."));
  fixture.editor.setFileOverlay(QStringLiteral("main.typ"), changed);
  QVERIFY2(waitUntil([&]() { return fixture.pane()->property("hasPendingUpdate").toBool(); }),
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
  QVERIFY(QMetaObject::invokeMethod(fixture.pane(), "publishPending", Qt::DirectConnection));
  QVERIFY2(waitUntil([&]() { return published.count() == 2; }),
           qPrintable(fixture.editor.compileMessage()));
  QVERIFY(fixture.selected().isEmpty());
  QVERIFY(!fixture.pane()->property("hasPendingUpdate").toBool());
  QCOMPARE(fixture.pane()->property("zoom").toDouble(), 1.5);
  QVERIFY(QMetaObject::invokeMethod(fixture.pane(), "goToPage", Qt::DirectConnection, Q_ARG(int, 0),
                                    Q_ARG(QPointF, QPointF())));
  QCOMPARE(fixture.selectPage().trimmed(), QStringLiteral("After update."));
  QTest::keyClick(fixture.pane(), Qt::Key_C, Qt::ControlModifier);
  QCOMPARE(QApplication::clipboard()->text().trimmed(), QStringLiteral("After update."));
  fixture.clearSelection();
  QVERIFY(QMetaObject::invokeMethod(fixture.pane(), "goToPage", Qt::DirectConnection, Q_ARG(int, 2),
                                    Q_ARG(QPointF, QPointF())));
  QCOMPARE(fixture.selectPage().trimmed(), QStringLiteral("Last page."));
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
  const auto image = widgetImage(repeated());
  const auto upper = image.copy(0, 0, image.width(), image.height() / 2);
  const auto lower = image.copy(0, image.height() / 2, image.width(),
                                 image.height() - image.height() / 2);
  QVERIFY(coloredPixels(upper, true) > 10);
  QCOMPARE(coloredPixels(upper, false), 0);
  QVERIFY(coloredPixels(lower, false) > 10);
  QCOMPARE(coloredPixels(lower, true), 0);
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
  QVERIFY(writeSyntaxTheme(lightPath, QStringLiteral("TypstTestLight"), QStringLiteral("#aa2233")));
  auto config = editorConfig(root.path());
  config->m_textEditorConfig->m_syntaxTheme = lightPath;
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
  QVERIFY(writeSyntaxTheme(darkPath, QStringLiteral("TypstTestDark"), QStringLiteral("#ff8899")));
  auto darkConfig = editorConfig(root.path());
  darkConfig->m_textEditorConfig->m_syntaxTheme = darkPath;
  fixture.editor.setConfig(darkConfig);
  QVERIFY2(fixture.ready(), qPrintable(fixture.editor.compileMessage()));
  QVERIFY(waitUntil([&]() {
    return syntaxFormatAt(document, stringPosition).foreground().color() == QColor("#ff8899");
  }));
  highlighter->rehighlight();
  QCoreApplication::processEvents();
  QCOMPARE(syntaxFormatAt(document, stringPosition).foreground().color(), QColor("#ff8899"));
  QCOMPARE(syntaxFormatAt(document, commentPosition).foreground().color(), QColor("#557755"));
  for (const int delta : {1, -1}) {
    fixture.editor.getTextEditor()->zoom(delta);
    QCoreApplication::processEvents();
    QCOMPARE(syntaxFormatAt(document, stringPosition).foreground().color(), QColor("#ff8899"));
    QCOMPARE(syntaxFormatAt(document, commentPosition).foreground().color(), QColor("#557755"));
  }
  QCOMPARE(document->toPlainText(), originalText);
  QCOMPARE(fixture.edit()->textCursor().anchor(), anchor);
  QCOMPARE(fixture.edit()->textCursor().position(), position);
  QCOMPARE(document->isModified(), modified);
  QCOMPARE(document->availableUndoSteps(), undoSteps);
  QCOMPARE(textChangeCount(changes), 0);
}

void TestTypstEditor::syntaxHighlightsFollowEditsAndErrors() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  const auto themePath = root.filePath(QStringLiteral("syntax.theme"));
  QVERIFY(writeSyntaxTheme(themePath, QStringLiteral("TypstTestEdits"), QStringLiteral("#aa2233")));
  auto config = editorConfig(root.path());
  config->m_textEditorConfig->m_syntaxTheme = themePath;
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
