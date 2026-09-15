#include "test_markdowneditor.h"

#include <QAbstractTextDocumentLayout>
#include <QBuffer>
#include <QClipboard>
#include <QDir>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QInputMethodEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QPainter>
#include <QPixmap>
#include <QScrollBar>
#include <QSharedPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextLayout>
#include <QTimer>

#include <QtMath>
#include <cmark.h>

#include <memory>

#include <vtextedit/markdowneditorconfig.h>
#include <vtextedit/markdownhighlighter.h>
#include <vtextedit/markdownutils.h>
#include <vtextedit/previewmgr.h>
#include <vtextedit/previewwidget.h>
#include <vtextedit/texteditorconfig.h>
#include <vtextedit/texteditutils.h>
#include <vtextedit/theme.h>
#include <vtextedit/vmarkdowneditor.h>
#include <vtextedit/vtextedit.h>

using namespace tests;
using namespace vte;

namespace {
QSharedPointer<MarkdownEditorConfig> makeConfig() {
  auto editorConfig = QSharedPointer<TextEditorConfig>::create();
  editorConfig->m_inputMode = InputMode::NormalMode;
  return QSharedPointer<MarkdownEditorConfig>::create(editorConfig);
}

// A VMarkdownEditor with the text set and the cursor placed at @p_block /
// @p_positionInBlock (a negative position means end of block).
class Fixture {
public:
  explicit Fixture(const QString &p_text, int p_block = -1, int p_positionInBlock = -1,
                   const QSharedPointer<MarkdownEditorConfig> &p_config = makeConfig())
      : m_editor(p_config, QSharedPointer<TextEditorParameters>::create(), nullptr) {
    m_editor.setText(p_text);
    auto doc = m_editor.document();
    const int blockNumber = p_block < 0 ? doc->blockCount() - 1 : p_block;
    const auto block = doc->findBlockByNumber(blockNumber);
    QTextCursor cursor(doc);
    const int inBlock =
        p_positionInBlock < 0 ? block.length() - 1 : qMin(p_positionInBlock, block.length() - 1);
    cursor.setPosition(block.position() + inBlock);
    edit()->setTextCursor(cursor);
  }

  VTextEdit *edit() const { return m_editor.getTextEdit(); }

  VMarkdownEditor *editor() { return &m_editor; }

  QString text() const { return m_editor.document()->toPlainText(); }

  QString blockText(int p_blockNumber) const {
    return m_editor.document()->findBlockByNumber(p_blockNumber).text();
  }

  // Document position of the end of block @p_blockNumber.
  int blockEnd(int p_blockNumber) const {
    const auto block = m_editor.document()->findBlockByNumber(p_blockNumber);
    return block.position() + block.length() - 1;
  }

  void pressReturn() { QTest::keyClick(edit(), Qt::Key_Return); }

  void pressShiftReturn() { QTest::keyClick(edit(), Qt::Key_Return, Qt::ShiftModifier); }

  void pressCtrlReturn() { QTest::keyClick(edit(), Qt::Key_Return, Qt::ControlModifier); }

  void moveTo(int p_blockNumber) {
    const auto block = m_editor.document()->findBlockByNumber(p_blockNumber);
    QTextCursor cursor(m_editor.document());
    cursor.setPosition(block.position() + block.length() - 1);
    edit()->setTextCursor(cursor);
  }

  // Select from (@p_startBlock, @p_startCol) to (@p_endBlock, @p_endCol).
  void select(int p_startBlock, int p_startCol, int p_endBlock, int p_endCol) {
    auto doc = m_editor.document();
    const auto startBlock = doc->findBlockByNumber(p_startBlock);
    const auto endBlock = doc->findBlockByNumber(p_endBlock);
    QTextCursor cursor(doc);
    cursor.setPosition(startBlock.position() + p_startCol);
    cursor.setPosition(endBlock.position() + p_endCol, QTextCursor::KeepAnchor);
    edit()->setTextCursor(cursor);
  }

  // Select the whole document content.
  void selectAll() {
    auto doc = m_editor.document();
    QTextCursor cursor(doc);
    cursor.setPosition(0);
    cursor.setPosition(blockEnd(doc->blockCount() - 1), QTextCursor::KeepAnchor);
    edit()->setTextCursor(cursor);
  }

  // Bump the document time stamp without changing the block structure, so the
  // existing parse result becomes stale but stays structurally usable.
  void makeAstStale() {
    auto cursor = edit()->textCursor();
    cursor.insertText(QStringLiteral("x"));
    edit()->setTextCursor(cursor);
  }

  // Wait until the asynchronous parse has produced a result matching the
  // current document state.
  void waitForFreshAst(int p_blockNumber) {
    auto highlighter = m_editor.getHighlighter();
    QTRY_VERIFY_WITH_TIMEOUT(highlighter->getBlockContext(p_blockNumber).m_fresh, 5000);
  }

  // BlockContext can be fresh from a sliced fast parse. List ownership requires
  // a full publication, which updateHighlight also republishes when current.
  void waitForFreshListAst() {
    auto highlighter = m_editor.getHighlighter();
    QSignalSpy publication(highlighter, &MarkdownHighlighter::previewElementsUpdated);
    highlighter->updateHighlight();
    QTRY_VERIFY_WITH_TIMEOUT(!publication.isEmpty(), 5000);
  }

private:
  VMarkdownEditor m_editor;
};
} // namespace

namespace {
static QSharedPointer<MarkdownEditorConfig> makeConcealConfig(bool p_vi = false) {
  auto textConfig = QSharedPointer<TextEditorConfig>::create();
  textConfig->m_theme = QSharedPointer<Theme>::create(*TextEditorConfig::defaultTheme());
  textConfig->m_inputMode = p_vi ? InputMode::ViMode : InputMode::NormalMode;
  textConfig->m_lineNumberType = VTextEditor::LineNumberType::None;
  auto config = QSharedPointer<MarkdownEditorConfig>::create(textConfig);
  // The default theme is shared by unrelated fixtures. Never mutate that instance.
  auto &textStyle = textConfig->m_theme->editorStyle(Theme::Text);
  textStyle.m_fontFamily = QStringLiteral("Arial");
  textStyle.m_fontPointSize = 14;
  config->m_inplacePreviewSources = MarkdownEditorConfig::NoInplacePreview;
  config->m_autoFoldPreviewedBlocksEnabled = false;
  config->m_autoNumberOrderedListsEnabled = false;
  config->m_autoFormatTableSourceEnabled = false;
  return config;
}

static void waitForConcealPublication(Fixture &p_fixture) {
  auto highlighter = p_fixture.editor()->getHighlighter();
  QObject receiver;
  int publications = 0;
  // A fast BlockContext is insufficient. This signal is emitted only for a
  // full result matching the current source, including an empty range vector.
  QObject::connect(
      highlighter, &MarkdownHighlighter::concealRangesUpdated, &receiver,
      [&publications](TimeStamp, const QVector<md::ConcealRange> &) { ++publications; });
  highlighter->updateHighlight();
  QTRY_VERIFY_WITH_TIMEOUT(publications > 0, 5000);
  // Full publication precedes the queued rehighlight/idle layout passes.
  QTest::qWait(100);
}

static void setConcealCursor(Fixture &p_fixture, int p_position) {
  QTextCursor cursor(p_fixture.editor()->document());
  cursor.setPosition(p_position);
  p_fixture.edit()->setTextCursor(cursor);
  // Deliberately synchronous: editing tests inspect invalidation before a parse.
}

static QImage renderConcealEditor(Fixture &p_fixture) {
  auto editor = p_fixture.editor();
  auto edit = p_fixture.edit();
  editor->setSpellCheckEnabled(false);
  if (!editor->isVisible()) {
    editor->resize(900, 480);
    editor->show();
    if (!QTest::qWaitForWindowExposed(editor)) {
      return QImage();
    }
  }
  // Preserve subsequent explicit sizes for wrapping and scrolling scenarios.
  editor->activateWindow();
  edit->setFocus();
  QTest::qWait(20);
  auto viewport = edit->viewport();
  const qreal dpr = viewport->devicePixelRatioF();
  QImage image(qCeil(viewport->width() * dpr), qCeil(viewport->height() * dpr),
               QImage::Format_ARGB32_Premultiplied);
  image.setDevicePixelRatio(dpr);
  image.fill(Qt::transparent);
  viewport->render(&image);
  return image;
}

static qreal concealConfigRangeWidth(Fixture &p_fixture, int p_start, int p_end) {
  auto doc = p_fixture.editor()->document();
  const auto block = doc->findBlock(p_start);
  doc->documentLayout()->blockBoundingRect(block);
  const auto line = block.layout()->lineForTextPosition(p_start - block.position());
  if (!line.isValid() || line.textStart() + line.textLength() < p_end - block.position()) {
    return -1;
  }
  return line.cursorToX(p_end - block.position()) - line.cursorToX(p_start - block.position());
}

static qreal concealConfigExpectedWidth(Fixture &p_fixture, int p_start, int p_end,
                                        const QString &p_display) {
  auto config = makeConcealConfig();
  *config->m_textEditorConfig = p_fixture.editor()->getConfig();
  config->m_textEditorConfig->m_theme =
      QSharedPointer<Theme>::create(*config->m_textEditorConfig->m_theme);
  config->m_textEditorConfig->m_inputMode = InputMode::NormalMode;
  config->m_concealElements = {};
  const auto source = p_fixture.text();
  const auto displayed = source.left(p_start) + p_display + source.mid(p_end);
  qreal expected;
  {
    // Keep Markdown syntax fonts, font fallback and source context identical;
    // only the source spelling and automatic concealment differ.
    Fixture reference(displayed, -1, -1, config);
    renderConcealEditor(reference);
    waitForConcealPublication(reference);
    expected = concealConfigRangeWidth(reference, p_start, p_start + p_display.size());
  }
  p_fixture.editor()->activateWindow();
  p_fixture.edit()->setFocus();
  return expected;
}

static void concealConfigVerifyWidth(Fixture &p_fixture, int p_start, int p_end,
                                     const QString &p_display) {
  const qreal actual = concealConfigRangeWidth(p_fixture, p_start, p_end);
  const qreal expected = concealConfigExpectedWidth(p_fixture, p_start, p_end, p_display);
  QVERIFY2(qAbs(actual - expected) < 1.0,
           qPrintable(QStringLiteral("source [%1,%2): width %3, expected %4 for %5")
                          .arg(p_start)
                          .arg(p_end)
                          .arg(actual)
                          .arg(expected)
                          .arg(p_display)));
}

static QRectF concealConfigViewportRange(Fixture &p_fixture, int p_start, int p_end) {
  QTextCursor cursor(p_fixture.editor()->document());
  cursor.setPosition(p_start);
  const auto start = p_fixture.edit()->cursorRect(cursor);
  cursor.setPosition(p_end);
  const auto end = p_fixture.edit()->cursorRect(cursor);
  return QRectF(start.left(), start.top(), end.left() - start.left(), start.height());
}

static int concealConfigColorCount(const QImage &p_image, const QRectF &p_rect,
                                   const QColor &p_color) {
  const qreal dpr = p_image.devicePixelRatio();
  const auto pixels =
      QRect(QPoint(qCeil(p_rect.left() * dpr), qCeil(p_rect.top() * dpr)),
            QPoint(qCeil(p_rect.right() * dpr) - 1, qCeil(p_rect.bottom() * dpr) - 1))
          .intersected(p_image.rect());
  int count = 0;
  for (int y = pixels.top(); !pixels.isEmpty() && y <= pixels.bottom(); ++y) {
    for (int x = pixels.left(); x <= pixels.right(); ++x) {
      count += p_image.pixelColor(x, y) == p_color;
    }
  }
  return count;
}

static QSharedPointer<Theme> concealConfigTheme(const QColor &p_foreground = QColor(),
                                                const QColor &p_background = QColor()) {
  const QJsonObject text{{QStringLiteral("font-family"), QStringLiteral("Arial")},
                         {QStringLiteral("font-size"), 14},
                         {QStringLiteral("text-color"), QStringLiteral("#202020")},
                         {QStringLiteral("background-color"), QStringLiteral("#ffffff")}};
  QJsonObject editorStyles{{QStringLiteral("Text"), text}};
  if (p_foreground.isValid() || p_background.isValid()) {
    QJsonObject concealed;
    if (p_foreground.isValid()) {
      concealed.insert(QStringLiteral("text-color"), p_foreground.name());
    }
    if (p_background.isValid()) {
      concealed.insert(QStringLiteral("background-color"), p_background.name());
    }
    editorStyles.insert(QStringLiteral("ConcealedText"), concealed);
  }
  const QJsonObject json{
      {QStringLiteral("metadata"),
       QJsonObject{{QStringLiteral("type"), QStringLiteral("vtextedit")}}},
      {QStringLiteral("editor-styles"), editorStyles},
      {QStringLiteral("markdown-syntax-styles"),
       QJsonObject{{QStringLiteral("LINK"),
                    QJsonObject{{QStringLiteral("text-color"), QStringLiteral("#174db5")}}},
                   {QStringLiteral("IMAGE"),
                    QJsonObject{{QStringLiteral("text-color"), QStringLiteral("#773388")}}},
                   {QStringLiteral("REFERENCE"),
                    QJsonObject{{QStringLiteral("text-color"), QStringLiteral("#946000")}}}}}};
  return Theme::createThemeFromContent(
      QString::fromUtf8(QJsonDocument(json).toJson(QJsonDocument::Compact)));
}

static void concealConfigSaveImage(const QImage &p_image, const QString &p_name) {
  const auto path = qEnvironmentVariable("VTE_CONCEAL_TEST_IMAGE_DIR");
  if (path.isEmpty()) {
    return;
  }
  QVERIFY(QDir().mkpath(path));
  QVERIFY(p_image.save(QDir(path).filePath(p_name), "PNG"));
}
} // namespace

void TestMarkdownEditor::testConcealMarkdownConfig() {
  const auto compact = QStringLiteral("abc\u00b7\u00b7\u00b7xyz");
  const auto alphabet = QStringLiteral("abcdefghijklmnopqrstuvwxyz");
  struct ThresholdCase {
    QString m_payload;
    QString m_display;
    int m_threshold;
  };
  const auto combining = QStringLiteral("e\u0301");
  const auto supplementary = QStringLiteral("\U0001f600");
  const auto dots = QStringLiteral("\u00b7\u00b7\u00b7");
  const ThresholdCase cases[] = {
      {alphabet.left(20), alphabet.left(20), 20},
      {alphabet.left(21), QStringLiteral("abc") + dots + QStringLiteral("stu"), 20},
      // UTF-16 length and code-point length must not substitute for graphemes.
      {combining.repeated(20), combining.repeated(20), 20},
      {combining.repeated(21), combining.repeated(3) + dots + combining.repeated(3), 20},
      {supplementary.repeated(20), supplementary.repeated(20), 20},
      {supplementary.repeated(21), supplementary.repeated(3) + dots + supplementary.repeated(3),
       20},
      {alphabet.left(8), alphabet.left(8), 0},
      {alphabet.left(9), alphabet.left(9), 0},
      {alphabet.left(10), QStringLiteral("abc") + dots + QStringLiteral("hij"), 0},
      {combining.repeated(9), combining.repeated(9), -7},
      {combining.repeated(10), combining.repeated(3) + dots + combining.repeated(3), -7}};
  for (const auto &item : cases) {
    auto config = makeConcealConfig();
    if (item.m_threshold != 20) {
      config->m_concealLengthThreshold = item.m_threshold;
    }
    const auto source = QStringLiteral("[x](") + item.m_payload + QStringLiteral(") tail\noutside");
    Fixture fixture(source, 1, 0, config);
    QVERIFY(!renderConcealEditor(fixture).isNull());
    waitForConcealPublication(fixture);
    concealConfigVerifyWidth(fixture, 4, 4 + item.m_payload.size(), item.m_display);
    QCOMPARE(fixture.text(), source);
    QVERIFY(!fixture.editor()->document()->isUndoAvailable());
    QVERIFY(!fixture.editor()->document()->isRedoAvailable());
  }

  auto config = makeConcealConfig();
  const auto source = QStringLiteral("[link](") + alphabet + QStringLiteral(") tail\n\n![image](") +
                      alphabet + QStringLiteral(") tail\n\n[ref]: ") + alphabet +
                      QStringLiteral(" \"title\"\n\n[use][ref]\n\noutside");
  Fixture fixture(source, -1, -1, config);
  QVERIFY(!renderConcealEditor(fixture).isNull());
  auto doc = fixture.editor()->document();
  // Preserve real undo AND redo history through all loaded-document reconfiguration.
  auto cursor = fixture.edit()->textCursor();
  cursor.beginEditBlock();
  cursor.insertText(QStringLiteral(" retained"));
  cursor.endEditBlock();
  cursor.beginEditBlock();
  cursor.insertText(QStringLiteral(" undone"));
  cursor.endEditBlock();
  fixture.edit()->undo();
  setConcealCursor(fixture, doc->characterCount() - 1);
  waitForConcealPublication(fixture);
  QVERIFY(doc->isUndoAvailable());
  QVERIFY(doc->isRedoAvailable());
  const auto preservedSource = fixture.text();
  const int characterCount = doc->characterCount();
  const int undoSteps = doc->availableUndoSteps();
  const int redoSteps = doc->availableRedoSteps();
  const bool modified = doc->isModified();
  QVector<int> sourceRevisions;
  for (auto block = doc->begin(); block.isValid(); block = block.next()) {
    sourceRevisions.append(block.revision());
  }
  auto verifySourceUnchanged = [&]() {
    QCOMPARE(fixture.text(), preservedSource);
    QCOMPARE(doc->characterCount(), characterCount);
    QCOMPARE(doc->availableUndoSteps(), undoSteps);
    QCOMPARE(doc->availableRedoSteps(), redoSteps);
    QCOMPARE(doc->isModified(), modified);
    QVector<int> current;
    for (auto block = doc->begin(); block.isValid(); block = block.next()) {
      current.append(block.revision());
    }
    // QTextDocument::revision also counts explicit syntax rehighlighting.
    QCOMPARE(current, sourceRevisions);
  };
  struct KindCase {
    MarkdownConcealElement m_kind;
    int m_start;
  };
  const KindCase kinds[] = {
      {MarkdownConcealElement::LinkUrl, static_cast<int>(source.indexOf(alphabet))},
      {MarkdownConcealElement::ImageUrl,
       static_cast<int>(source.indexOf(alphabet, source.indexOf(alphabet) + 1))},
      {MarkdownConcealElement::ReferenceUrl, static_cast<int>(source.lastIndexOf(alphabet))}};
  const MarkdownConcealElements all = MarkdownConcealElement::ImageUrl |
                                      MarkdownConcealElement::LinkUrl |
                                      MarkdownConcealElement::ReferenceUrl;
  // Actual default behavior: each authored destination is shortened, not labels/titles.
  for (const auto &item : kinds) {
    concealConfigVerifyWidth(fixture, item.m_start, item.m_start + alphabet.size(), compact);
  }
  for (const auto &disabled : kinds) {
    config->m_concealElements = all & ~MarkdownConcealElements(disabled.m_kind);
    fixture.editor()->setConfig(config);
    waitForConcealPublication(fixture);
    for (const auto &item : kinds) {
      concealConfigVerifyWidth(fixture, item.m_start, item.m_start + alphabet.size(),
                               item.m_kind == disabled.m_kind ? alphabet : compact);
    }
    verifySourceUnchanged();
  }
  config->m_concealElements = MarkdownConcealElements();
  fixture.editor()->setConfig(config);
  // Clearing is synchronous; no parse or source edit is needed to disable it.
  for (const auto &item : kinds) {
    concealConfigVerifyWidth(fixture, item.m_start, item.m_start + alphabet.size(), alphabet);
  }
  waitForConcealPublication(fixture);
  verifySourceUnchanged();
  config->m_concealElements = all;
  for (int threshold : {26, 25}) {
    config->m_concealLengthThreshold = threshold;
    fixture.editor()->setConfig(config);
    waitForConcealPublication(fixture);
    for (const auto &item : kinds) {
      concealConfigVerifyWidth(fixture, item.m_start, item.m_start + alphabet.size(),
                               threshold == 26 ? alphabet : compact);
    }
    verifySourceUnchanged();
  }

  const QColor firstForeground(QStringLiteral("#ce1464"));
  const QColor firstBackground(QStringLiteral("#e0fbd5"));
  const QColor secondForeground(QStringLiteral("#147a42"));
  const QColor secondBackground(QStringLiteral("#fbd5ed"));
  const int start = kinds[0].m_start;
  const int end = start + alphabet.size();
  const QPair<QColor, QColor> styles[] = {{firstForeground, firstBackground},
                                          {secondForeground, secondBackground}};
  for (const auto &style : styles) {
    config->m_textEditorConfig->m_theme = concealConfigTheme(style.first, style.second);
    QVERIFY(config->m_textEditorConfig->m_theme);
    fixture.editor()->setConfig(config);
    waitForConcealPublication(fixture);
    const auto image = renderConcealEditor(fixture);
    QVERIFY(!image.isNull());
    concealConfigVerifyWidth(fixture, start, end, compact);
    // Inspect each retained end AND the dots, not merely a colored overlay elsewhere.
    for (const auto &part :
         {qMakePair(start, start + 3), qMakePair(start + 3, end - 3), qMakePair(end - 3, end)}) {
      const auto rect = concealConfigViewportRange(fixture, part.first, part.second);
      QVERIFY(rect.width() > 0);
      QVERIFY(concealConfigColorCount(image, rect, style.first) > 0);
      QVERIFY(concealConfigColorCount(image, rect, style.second) > 0);
      if (style.first == secondForeground) {
        QCOMPARE(concealConfigColorCount(image, rect, firstForeground), 0);
        QCOMPARE(concealConfigColorCount(image, rect, firstBackground), 0);
      }
    }
    verifySourceUnchanged();
  }
  concealConfigSaveImage(renderConcealEditor(fixture), QStringLiteral("conceal-config-styled.png"));
  setConcealCursor(fixture, start);
  auto revealed = renderConcealEditor(fixture);
  concealConfigVerifyWidth(fixture, start, end, alphabet);
  const QColor sourceForeground(QStringLiteral("#174db5"));
  auto rect = concealConfigViewportRange(fixture, start, end);
  QVERIFY(concealConfigColorCount(revealed, rect, sourceForeground) > 0);
  QCOMPARE(concealConfigColorCount(revealed, rect, secondBackground), 0);
  setConcealCursor(fixture, doc->characterCount() - 1);

  // A real custom theme omitting ConcealedText inherits source syntax color;
  // neither a previous theme's overlay nor a conceal-specific background survives.
  config->m_textEditorConfig->m_theme = concealConfigTheme();
  QVERIFY(config->m_textEditorConfig->m_theme);
  fixture.editor()->setConfig(config);
  waitForConcealPublication(fixture);
  const auto omitted = renderConcealEditor(fixture);
  QVERIFY(!omitted.isNull());
  concealConfigVerifyWidth(fixture, start, end, compact);
  for (const auto &part :
       {qMakePair(start, start + 3), qMakePair(start + 3, end - 3), qMakePair(end - 3, end)}) {
    rect = concealConfigViewportRange(fixture, part.first, part.second);
    QVERIFY(concealConfigColorCount(omitted, rect, sourceForeground) > 0);
    QVERIFY(concealConfigColorCount(omitted, rect, QColor(Qt::white)) > 0);
    QCOMPARE(concealConfigColorCount(omitted, rect, secondForeground), 0);
    QCOMPARE(concealConfigColorCount(omitted, rect, secondBackground), 0);
  }
  verifySourceUnchanged();
  // History still performs the authored edits, not any display transformation.
  fixture.edit()->redo();
  QCOMPARE(fixture.text(), preservedSource + QStringLiteral(" undone"));
  fixture.edit()->undo();
  QCOMPARE(fixture.text(), preservedSource);
  fixture.edit()->undo();
  QCOMPARE(fixture.text(), source);
}

void TestMarkdownEditor::testConcealCaretAndViMotion() {
  const auto alphabet = QStringLiteral("abcdefghijklmnopqrstuvwxyz");
  const auto compact = QStringLiteral("abc\u00b7\u00b7\u00b7xyz");
  const auto source = QStringLiteral("[x](") + alphabet + QStringLiteral(") tail\nother block");
  const int start = source.indexOf(alphabet);
  const int end = start + alphabet.size();
  Fixture fixture(source, 0, start - 1, makeConcealConfig(true));
  QVERIFY(!renderConcealEditor(fixture).isNull());
  QTRY_VERIFY_WITH_TIMEOUT(fixture.edit()->hasFocus(), 5000);
  waitForConcealPublication(fixture);
  auto edit = fixture.edit();
  QTest::keyClick(edit, Qt::Key_Escape);
  setConcealCursor(fixture, start - 1);
  QCOMPARE(source.at(edit->textCursor().position()), QLatin1Char('('));
  concealConfigVerifyWidth(fixture, start, end, compact);
  concealConfigSaveImage(renderConcealEditor(fixture), QStringLiteral("conceal-vi-compact.png"));
  QTest::keyClicks(edit, QStringLiteral("5l"));
  QCOMPARE(edit->textCursor().position(), start + 4);
  concealConfigVerifyWidth(fixture, start, end, alphabet);
  QCOMPARE(fixture.text(), source);
  concealConfigSaveImage(renderConcealEditor(fixture), QStringLiteral("conceal-vi-revealed.png"));
  setConcealCursor(fixture, start);
  QTest::keyClicks(edit, QStringLiteral("5l"));
  QCOMPARE(edit->textCursor().position(), start + 5);
  concealConfigVerifyWidth(fixture, start, end, alphabet);

  // Crossing either retained endpoint by keyboard reveals the entire URL.
  setConcealCursor(fixture, start - 1);
  QTest::keyClicks(edit, QStringLiteral("l"));
  QCOMPARE(edit->textCursor().position(), start);
  concealConfigVerifyWidth(fixture, start, end, alphabet);
  QTest::keyClicks(edit, QStringLiteral("h"));
  QCOMPARE(edit->textCursor().position(), start - 1);
  concealConfigVerifyWidth(fixture, start, end, compact);
  setConcealCursor(fixture, end);
  QTest::keyClicks(edit, QStringLiteral("h"));
  QCOMPARE(edit->textCursor().position(), end - 1);
  concealConfigVerifyWidth(fixture, start, end, alphabet);
  QTest::keyClicks(edit, QStringLiteral("l"));
  QCOMPARE(edit->textCursor().position(), end);
  concealConfigVerifyWidth(fixture, start, end, compact);

  for (int endpoint : {start, end - 1}) {
    setConcealCursor(fixture, endpoint);
    QCOMPARE(edit->textCursor().position(), endpoint);
    concealConfigVerifyWidth(fixture, start, end, alphabet);
    setConcealCursor(fixture, fixture.blockEnd(1));
    concealConfigVerifyWidth(fixture, start, end, compact);
  }
  for (int endpoint : {start, end - 1}) {
    setConcealCursor(fixture, fixture.blockEnd(1));
    QVERIFY(!renderConcealEditor(fixture).isNull());
    const auto glyph = concealConfigViewportRange(fixture, endpoint, endpoint + 1);
    const QPoint point(qRound(glyph.left() + glyph.width() / 4), qRound(glyph.center().y()));
    QCOMPARE(edit->cursorForPosition(point).position(), endpoint);
    QTest::mouseClick(edit->viewport(), Qt::LeftButton, Qt::NoModifier, point);
    QCOMPARE(edit->textCursor().position(), endpoint);
    concealConfigVerifyWidth(fixture, start, end, alphabet);
    QTest::keyClicks(edit, QStringLiteral("$"));
    QCOMPARE(edit->textCursor().position(), fixture.blockEnd(0) - 1);
    concealConfigVerifyWidth(fixture, start, end, compact);
  }
  setConcealCursor(fixture, start + 4);
  QTest::keyClicks(edit, QStringLiteral("j"));
  QCOMPARE(edit->textCursor().blockNumber(), 1);
  concealConfigVerifyWidth(fixture, start, end, compact);
  setConcealCursor(fixture, start - 1);
  QTest::keyClicks(edit, QStringLiteral("999l"));
  QCOMPARE(edit->textCursor().position(), fixture.blockEnd(0) - 1);
  QTest::keyClicks(edit, QStringLiteral("5l"));
  QCOMPARE(edit->textCursor().position(), fixture.blockEnd(0) - 1);
  concealConfigVerifyWidth(fixture, start, end, compact);
  QCOMPARE(fixture.text(), source);
  QVERIFY(!fixture.editor()->document()->isUndoAvailable());

  // Inclusive Vi selection crosses both delimiters. Its moving source caret is
  // outside the URL, so selecting the middle does not itself reveal the range.
  setConcealCursor(fixture, start - 1);
  QTest::keyClicks(edit, QStringLiteral("v27l"));
  QCOMPARE(edit->textCursor().position(), end);
  QCOMPARE(edit->selectedText(), QStringLiteral("(") + alphabet + QLatin1Char(')'));
  concealConfigVerifyWidth(fixture, start, end, compact);
  QTest::keyClicks(edit, QStringLiteral("d"));
  const auto deleted = QStringLiteral("[x] tail\nother block");
  QCOMPARE(fixture.text(), deleted);
  QTest::keyClicks(edit, QStringLiteral("u"));
  QCOMPARE(fixture.text(), source);
  QTest::keyClick(edit, Qt::Key_R, Qt::ControlModifier);
  QCOMPARE(fixture.text(), deleted);
  QTest::keyClicks(edit, QStringLiteral("u"));
  QCOMPARE(fixture.text(), source);
  setConcealCursor(fixture, fixture.blockEnd(1));
  waitForConcealPublication(fixture);
  concealConfigVerifyWidth(fixture, start, end, compact);
}

void TestMarkdownEditor::testIsQuote() {
  struct Case {
    const char *m_text;
    const char *m_indentation;
    const char *m_prefix;
    const char *m_rest;
    int m_depth;
  };

  const Case cases[] = {
      {"> hello", "", "> ", "hello", 1},
      {">hello", "", ">", "hello", 1},
      {"  > hello", "  ", "> ", "hello", 1},
      {"> > a", "", "> > ", "a", 2},
      {">> a", "", ">> ", "a", 2},
      {"> ", "", "> ", "", 1},
      {">", "", ">", "", 1},
      {"> - item", "", "> ", "- item", 1},
  };

  for (const auto &c : cases) {
    QString indentation, prefix, rest;
    int depth = -1;
    QVERIFY2(MarkdownUtils::isQuote(QString::fromUtf8(c.m_text), indentation, prefix, rest, depth),
             c.m_text);
    QCOMPARE(indentation, QString::fromUtf8(c.m_indentation));
    QCOMPARE(prefix, QString::fromUtf8(c.m_prefix));
    QCOMPARE(rest, QString::fromUtf8(c.m_rest));
    QCOMPARE(depth, c.m_depth);
  }

  QString indentation, prefix, rest;
  int depth = -1;
  QVERIFY(!MarkdownUtils::isQuote(QStringLiteral("hello"), indentation, prefix, rest, depth));
  QVERIFY(!MarkdownUtils::isQuote(QString(), indentation, prefix, rest, depth));
}

void TestMarkdownEditor::testTypeQuoteUnchanged() {
  // c_quoteRegExp requires whitespace after the marker, so "> a" un-quotes
  // while ">a" is treated as unquoted and gets another marker.
  {
    Fixture fixture(QStringLiteral("> a"), 0);
    MarkdownUtils::typeQuote(fixture.edit());
    QCOMPARE(fixture.blockText(0), QStringLiteral("a"));
  }
  {
    Fixture fixture(QStringLiteral(">a"), 0);
    MarkdownUtils::typeQuote(fixture.edit());
    QCOMPARE(fixture.blockText(0), QStringLiteral("> >a"));
  }
}

void TestMarkdownEditor::testQuoteContinuation() {
  {
    Fixture fixture(QStringLiteral("> foo"), 0);
    fixture.pressReturn();
    QCOMPARE(fixture.blockText(1), QStringLiteral("> "));
    // The cursor sits right after the inserted prefix.
    QCOMPARE(fixture.edit()->textCursor().blockNumber(), 1);
    QCOMPARE(fixture.edit()->textCursor().positionInBlock(), 2);
  }
  {
    Fixture fixture(QStringLiteral("> > foo"), 0);
    fixture.pressReturn();
    QCOMPARE(fixture.blockText(1), QStringLiteral("> > "));
  }
  {
    Fixture fixture(QStringLiteral(">> foo"), 0);
    fixture.pressReturn();
    QCOMPARE(fixture.blockText(1), QStringLiteral(">> "));
  }
  {
    // Indented quote markers: the indentation is copied by AutoIndentHelper,
    // the prefix by the continuation.
    Fixture fixture(QStringLiteral("  > foo"), 0);
    fixture.pressReturn();
    QCOMPARE(fixture.blockText(1), QStringLiteral("  > "));
  }
}

void TestMarkdownEditor::testQuoteWithListContinuation() {
  {
    Fixture fixture(QStringLiteral("> - a"), 0);
    fixture.pressReturn();
    QCOMPARE(fixture.blockText(1), QStringLiteral("> - "));
  }
  {
    Fixture fixture(QStringLiteral("> 1. a"), 0);
    fixture.pressReturn();
    QCOMPARE(fixture.blockText(1), QStringLiteral("> 2. "));
  }
  {
    Fixture fixture(QStringLiteral("> - [x] a"), 0);
    fixture.pressReturn();
    QCOMPARE(fixture.blockText(1), QStringLiteral("> - [ ] "));
  }
}

void TestMarkdownEditor::testEmptyQuoteStartsANewQuoteLine() {
  // A bare quote line is a blank line inside the quote, so Enter continues the
  // quote instead of stripping a level.
  {
    Fixture fixture(QStringLiteral("> > "), 0);
    fixture.pressReturn();
    QCOMPARE(fixture.text(), QStringLiteral("> > \n> > "));
    QCOMPARE(fixture.edit()->textCursor().blockNumber(), 1);
    QCOMPARE(fixture.edit()->textCursor().positionInBlock(), 4);
  }
  {
    Fixture fixture(QStringLiteral("> "), 0);
    fixture.pressReturn();
    QCOMPARE(fixture.text(), QStringLiteral("> \n> "));
  }
  {
    // A bare ">" with no trailing space is continued verbatim too.
    Fixture fixture(QStringLiteral(">"), 0);
    fixture.pressReturn();
    QCOMPARE(fixture.text(), QStringLiteral(">\n>"));
  }
}

void TestMarkdownEditor::testEmptyListInQuoteExit() {
  Fixture fixture(QStringLiteral("> - "), 0);
  fixture.pressReturn();
  QCOMPARE(fixture.text(), QStringLiteral("> "));
}

void TestMarkdownEditor::testCursorBeforeTextDoesNotCollapse() {
  // "> |foo": the quote is not empty to the right, so Enter must split.
  Fixture fixture(QStringLiteral("> foo"), 0, 2);
  fixture.pressReturn();
  QCOMPARE(fixture.blockText(0), QStringLiteral("> "));
  QCOMPARE(fixture.blockText(1), QStringLiteral("> foo"));
}

void TestMarkdownEditor::testMidLineSplitInsideQuote() {
  // "> fo|o"
  Fixture fixture(QStringLiteral("> foo"), 0, 4);
  fixture.pressReturn();
  QCOMPARE(fixture.blockText(0), QStringLiteral("> fo"));
  QCOMPARE(fixture.blockText(1), QStringLiteral("> o"));
}

void TestMarkdownEditor::testSelectionFallsThrough() {
  // preKeyReturn never rewrites a selection: handleKeyReturn performs its
  // normal selection-replacing split, and the continuation then applies to the
  // resulting line.
  // Same-block selection, forward.
  {
    Fixture fixture(QStringLiteral("> - "), 0);
    auto cursor = fixture.edit()->textCursor();
    cursor.setPosition(2);
    cursor.setPosition(4, QTextCursor::KeepAnchor);
    fixture.edit()->setTextCursor(cursor);
    fixture.pressReturn();
    QCOMPARE(fixture.text(), QStringLiteral("> \n> "));
  }
  // Same-block selection, backward.
  {
    Fixture fixture(QStringLiteral("> - "), 0);
    auto cursor = fixture.edit()->textCursor();
    cursor.setPosition(4);
    cursor.setPosition(2, QTextCursor::KeepAnchor);
    fixture.edit()->setTextCursor(cursor);
    fixture.pressReturn();
    QCOMPARE(fixture.text(), QStringLiteral("> \n> "));
  }
  // Multi-block selection.
  {
    Fixture fixture(QStringLiteral("> - a\n> - b"), 0);
    auto cursor = fixture.edit()->textCursor();
    cursor.setPosition(4);
    cursor.setPosition(10, QTextCursor::KeepAnchor);
    fixture.edit()->setTextCursor(cursor);
    fixture.pressReturn();
    QCOMPARE(fixture.text(), QStringLiteral("> - \n> - b"));
  }
}

void TestMarkdownEditor::testSelectionDoesNotCarryAstContext() {
  // The probe is taken at the active end of the selection, which is not
  // necessarily the line that survives the selection-replacing split. Such a
  // context must never drive an insertion, in either selection direction.
  const QString text = QStringLiteral("plain\n> q");

  // Forward: anchor on the unquoted line, cursor on the quoted one.
  {
    Fixture fixture(text, 1);
    fixture.waitForFreshAst(1);
    QCOMPARE(fixture.editor()->getHighlighter()->getBlockContext(1).m_quoteDepth, 1);

    const int plainEnd = fixture.blockEnd(0);
    const int quoteEnd = fixture.blockEnd(1);
    auto cursor = fixture.edit()->textCursor();
    cursor.setPosition(plainEnd);
    cursor.setPosition(quoteEnd, QTextCursor::KeepAnchor);
    fixture.edit()->setTextCursor(cursor);
    fixture.pressReturn();
    QCOMPARE(fixture.text(), QStringLiteral("plain\n"));
  }
  // Backward: the same selection, opposite direction.
  {
    Fixture fixture(text, 1);
    fixture.waitForFreshAst(1);

    const int plainEnd = fixture.blockEnd(0);
    const int quoteEnd = fixture.blockEnd(1);
    auto cursor = fixture.edit()->textCursor();
    cursor.setPosition(quoteEnd);
    cursor.setPosition(plainEnd, QTextCursor::KeepAnchor);
    fixture.edit()->setTextCursor(cursor);
    fixture.pressReturn();
    QCOMPARE(fixture.text(), QStringLiteral("plain\n"));
  }
}

void TestMarkdownEditor::testLazyContinuation() {
  // "> a" followed by a lazy continuation line "b": the text of "b" carries no
  // marker, only the AST knows it is inside the quote.
  struct Case {
    const char *m_second;
    const char *m_expected;
  };
  const Case cases[] = {
      {"b", "> "},
      {" b", " > "},
      {"   b", "   > "},
      {"\tb", "\t> "},
  };

  for (const auto &c : cases) {
    const QString text = QStringLiteral("> a\n") + QString::fromUtf8(c.m_second);
    Fixture fixture(text, 1);
    fixture.waitForFreshAst(1);
    QCOMPARE(fixture.editor()->getHighlighter()->getBlockContext(1).m_quoteDepth, 1);
    fixture.pressReturn();
    // The new block carries the copied indentation plus the synthesized prefix.
    QCOMPARE(fixture.blockText(2), QString::fromUtf8(c.m_expected));
  }
}

void TestMarkdownEditor::testLazyContinuationDepths() {
  // Depth 0: no AST quote context, so nothing is synthesized.
  {
    Fixture fixture(QStringLiteral("a\nb"), 1);
    fixture.waitForFreshAst(1);
    QCOMPARE(fixture.editor()->getHighlighter()->getBlockContext(1).m_quoteDepth, 0);
    fixture.pressReturn();
    QCOMPARE(fixture.blockText(2), QString());
  }
  // Nested: a lazy continuation of a doubly nested quote.
  {
    Fixture fixture(QStringLiteral("> > a\nb"), 1);
    fixture.waitForFreshAst(1);
    QCOMPARE(fixture.editor()->getHighlighter()->getBlockContext(1).m_quoteDepth, 2);
    fixture.pressReturn();
    QCOMPARE(fixture.blockText(2), QStringLiteral("> > "));
  }
}

void TestMarkdownEditor::testStaleAstNeverInserts() {
  // A stale result may carry a positive quote depth, but inserting from it
  // would leave permanently wrong text, so it must never be used.
  Fixture fixture(QStringLiteral("> a\nb"), 1);
  fixture.waitForFreshAst(1);
  QCOMPARE(fixture.editor()->getHighlighter()->getBlockContext(1).m_quoteDepth, 1);

  fixture.makeAstStale();
  const auto context = fixture.editor()->getHighlighter()->getBlockContext(1);
  QVERIFY(context.m_valid);
  QVERIFY(!context.m_fresh);
  // The stale depth is still readable...
  QCOMPARE(context.m_quoteDepth, 1);

  // ...but must not produce any insertion.
  fixture.pressReturn();
  QCOMPARE(fixture.blockText(1), QStringLiteral("bx"));
  QCOMPARE(fixture.blockText(2), QString());
}

void TestMarkdownEditor::testFenceVeto() {
  const QString text = QStringLiteral("```\n> foo\n```");
  Fixture fixture(text, 1);
  fixture.waitForFreshAst(1);
  QVERIFY(fixture.editor()->getHighlighter()->getBlockContext(1).m_inFencedCode);

  fixture.pressReturn();
  // Nothing is continued inside a fence.
  QCOMPARE(fixture.blockText(2), QString());
}

void TestMarkdownEditor::testStaleFenceStillSuppresses() {
  // Suppression is the safe direction, so it is allowed to use stale data.
  Fixture fixture(QStringLiteral("```\n> foo\n```"), 1);
  fixture.waitForFreshAst(1);

  fixture.makeAstStale();
  const auto context = fixture.editor()->getHighlighter()->getBlockContext(1);
  QVERIFY(context.m_valid);
  QVERIFY(!context.m_fresh);
  QVERIFY(context.m_inFencedCode);

  // The regex would happily continue "> foox"; the veto wins.
  fixture.pressReturn();
  QCOMPARE(fixture.blockText(2), QString());
}

void TestMarkdownEditor::testIndentedQuoteIsStillContinued() {
  // Four-space indented text is CommonMark indented code, not a quote.
  // Detection is deliberately textual, so it is continued anyway (Decision 7).
  Fixture fixture(QStringLiteral("    > foo"), 0);
  fixture.pressReturn();
  QCOMPARE(fixture.blockText(1), QStringLiteral("    > "));
}

void TestMarkdownEditor::testShiftReturnDoesNotContinue() {
  Fixture fixture(QStringLiteral("> foo"), 0);
  fixture.pressShiftReturn();
  QCOMPARE(fixture.blockText(0), QStringLiteral("> foo  "));
  // Shift+Enter must not continue the quote.
  QCOMPARE(fixture.blockText(1), QString());

  // And the context cached by the Shift+Enter probe must not leak into the
  // next, ordinary Enter on an unquoted (empty) line.
  fixture.pressReturn();
  QCOMPARE(fixture.blockText(2), QString());
}

void TestMarkdownEditor::testCtrlReturnDoesNotLeakContext() {
  // Ctrl+Return probes the context but is swallowed by handleKeyReturn without
  // a matching postKeyReturn, so the cached value is never consumed. The next
  // ordinary Return must re-probe rather than reuse it.
  Fixture fixture(QStringLiteral("> a\nb\n\nc"), 1);
  fixture.waitForFreshAst(1);
  QCOMPARE(fixture.editor()->getHighlighter()->getBlockContext(1).m_quoteDepth, 1);

  const auto before = fixture.text();
  fixture.pressCtrlReturn();
  QCOMPARE(fixture.text(), before);

  // "c" is outside the quote; no prefix may be synthesized from the cached
  // depth of block 1.
  fixture.moveTo(3);
  QCOMPARE(fixture.editor()->getHighlighter()->getBlockContext(3).m_quoteDepth, 0);
  fixture.pressReturn();
  QCOMPARE(fixture.blockText(4), QString());
}

void TestMarkdownEditor::testWhitespaceOnlyQuoteContinues() {
  Fixture fixture(QStringLiteral(">   "), 0);
  fixture.pressReturn();
  // The prefix is reproduced verbatim, spacing included.
  QCOMPARE(fixture.text(), QStringLiteral(">   \n>   "));
}

void TestMarkdownEditor::testPlainListMidLineSplit() {
  {
    // "- fo|o"
    Fixture fixture(QStringLiteral("- foo"), 0, 4);
    fixture.pressReturn();
    QCOMPARE(fixture.text(), QStringLiteral("- fo\n- o"));
  }
  {
    // "1. fo|o"
    Fixture fixture(QStringLiteral("1. foo"), 0, 5);
    fixture.pressReturn();
    QCOMPARE(fixture.text(), QStringLiteral("1. fo\n2. o"));
  }
}

void TestMarkdownEditor::testPlainListContinuationRegression() {
  {
    Fixture fixture(QStringLiteral("- a"), 0);
    fixture.pressReturn();
    QCOMPARE(fixture.blockText(1), QStringLiteral("- "));
  }
  {
    Fixture fixture(QStringLiteral("1. a"), 0);
    fixture.pressReturn();
    QCOMPARE(fixture.blockText(1), QStringLiteral("2. "));
  }
  {
    Fixture fixture(QStringLiteral("  - [x] a"), 0);
    fixture.pressReturn();
    QCOMPARE(fixture.blockText(1), QStringLiteral("  - [ ] "));
  }
  {
    // Empty list item exit.
    Fixture fixture(QStringLiteral("  - "), 0);
    fixture.pressReturn();
    QCOMPARE(fixture.text(), QStringLiteral("  "));
  }
}

void TestMarkdownEditor::testRepeatedReturnDoesNotLeakContext() {
  Fixture fixture(QStringLiteral("> - "), 0);
  fixture.pressReturn();
  QCOMPARE(fixture.text(), QStringLiteral("> "));

  // The cached context of the first (handled) Return must not influence the
  // next one, which now continues the quote.
  fixture.pressReturn();
  QCOMPARE(fixture.text(), QStringLiteral("> \n> "));

  fixture.pressReturn();
  QCOMPARE(fixture.text(), QStringLiteral("> \n> \n> "));
}

void TestMarkdownEditor::testUndoIsASingleStep() {
  Fixture fixture(QStringLiteral("> foo"), 0);
  const auto before = fixture.text();

  fixture.pressReturn();
  QCOMPARE(fixture.blockText(1), QStringLiteral("> "));

  fixture.edit()->undo();
  QCOMPARE(fixture.text(), before);
}

namespace {
// Write a solid p_width x p_height PNG into p_dir and return its file name.
QString writeTestImage(const QTemporaryDir &p_dir, const QString &p_name, int p_width,
                       int p_height) {
  QImage img(p_width, p_height, QImage::Format_ARGB32);
  img.fill(Qt::red);
  const bool ok = img.save(QDir(p_dir.path()).filePath(p_name), "PNG");
  return ok ? p_name : QString();
}

// The resource name PreviewMgr composes for a link. The declared size is part
// of the key, which is what keeps two sizes of one file apart, and the
// destination is length-prefixed rather than fed through QString::arg() --
// a percent-encoded destination such as `a%2F.png` contains what arg() would
// read as a placeholder, which would make the key non-injective.
QString resourceName(const QString &p_shortUrl, int p_width, int p_height) {
  return QString::number(p_shortUrl.size()) + QLatin1Char(':') + p_shortUrl + QLatin1Char('_') +
         QString::number(p_width) + QLatin1Char('_') + QString::number(p_height);
}

// Drive an editor over a local image directory until its previews settle, then
// hand it to p_check.
template <typename Check>
void withImageEditor(const QString &p_markdown, const QString &p_basePath, Check p_check) {
  Fixture fixture(p_markdown, 0);
  fixture.editor()->setBasePath(p_basePath);
  // The base path changed after the first parse, so redo the preview pass.
  fixture.editor()->getHighlighter()->updateHighlight();
  fixture.waitForFreshAst(0);
  p_check(fixture);
}
} // namespace

// The image links must actually reach the highlighter's public channel.
//
// This is the one assertion that catches the construction-order trap in
// MarkdownHighlighterResult: every element-level test can pass while this is
// empty, and then the editor shows no previews at all.
void TestMarkdownEditor::testImageLinksArePublished() {
  Fixture fixture(QStringLiteral("![alt](a.png =500x300)\n"), 0);
  fixture.waitForFreshAst(0);

  const auto &links = fixture.editor()->getHighlighter()->getImageLinks();
  QCOMPARE(links.size(), 1);
  QCOMPARE(links.first().m_destination, QStringLiteral("a.png"));
  QCOMPARE(links.first().m_width, 500);
  QCOMPARE(links.first().m_height, 300);
  QVERIFY(links.first().m_region.m_startPos < links.first().m_region.m_endPos);
}

// The declared width is honored. Before this, `=500x` made the whole link fail
// to match the preview path's regular expression, so nothing was rendered at
// all; and even the matched case hardcoded a 0x0 (natural size) scale.
void TestMarkdownEditor::testSizedImagePreviewIsScaled() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  QVERIFY(!writeTestImage(dir, QStringLiteral("local.png"), 40, 20).isEmpty());

  withImageEditor(QStringLiteral("![](local.png =500x)\n"), dir.path(), [](Fixture &fixture) {
    const QPixmap *img = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((img = fixture.editor()->findImageFromDocumentResourceMgr(
                                  resourceName(QStringLiteral("local.png"), 500, 0))) != nullptr,
                             5000);
    // Width honored, aspect ratio preserved because the height is unspecified.
    QCOMPARE(img->width(), 500);
    QCOMPARE(img->height(), 250);
  });
}

// One destination at two declared sizes is two resources. Keying the cache on
// the URL alone would let whichever rendered first win for both.
void TestMarkdownEditor::testOneUrlAtTwoSizesGetsTwoResources() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  QVERIFY(!writeTestImage(dir, QStringLiteral("local.png"), 40, 20).isEmpty());

  withImageEditor(QStringLiteral("![](local.png =500x)\n\n![](local.png =250x)\n"), dir.path(),
                  [](Fixture &fixture) {
                    const QPixmap *big = nullptr;
                    const QPixmap *small = nullptr;
                    QTRY_VERIFY_WITH_TIMEOUT(
                        (big = fixture.editor()->findImageFromDocumentResourceMgr(
                             resourceName(QStringLiteral("local.png"), 500, 0))) != nullptr &&
                            (small = fixture.editor()->findImageFromDocumentResourceMgr(
                                 resourceName(QStringLiteral("local.png"), 250, 0))) != nullptr,
                        5000);
                    QCOMPARE(big->width(), 500);
                    QCOMPARE(small->width(), 250);
                  });
}

// A declared size is document-supplied and is multiplied again by the scale
// factor before allocation, so it is bounded.
void TestMarkdownEditor::testOversizedImageIsClamped() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  QVERIFY(!writeTestImage(dir, QStringLiteral("local.png"), 40, 20).isEmpty());

  withImageEditor(QStringLiteral("![](local.png =99999x)\n"), dir.path(), [](Fixture &fixture) {
    const QPixmap *img = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((img = fixture.editor()->findImageFromDocumentResourceMgr(
                                  resourceName(QStringLiteral("local.png"), 4096, 0))) != nullptr,
                             5000);
    QCOMPARE(img->width(), 4096);
    // The unclamped resource must not exist under any name.
    QVERIFY(!fixture.editor()->findImageFromDocumentResourceMgr(
        resourceName(QStringLiteral("local.png"), 99999, 0)));
  });
}

namespace {
// A solid p_width x p_height PNG as raw bytes.
QByteArray testImageData(int p_width, int p_height) {
  QImage img(p_width, p_height, QImage::Format_ARGB32);
  img.fill(Qt::blue);
  QByteArray data;
  QBuffer buffer(&data);
  buffer.open(QIODevice::WriteOnly);
  img.save(&buffer, "PNG");
  return data;
}
} // namespace

// Bytes handed over through seedImageData() are used instead of downloading the
// image back, at every declared size the document asks for.
void TestMarkdownEditor::testSeededImageAvoidsDownload() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());

  // Normalization-sensitive: percent-encoded plus a query string, spelled in a
  // form QUrl canonicalizes, so the raw string is NOT a usable key.
  const QString url = QStringLiteral("HTTPS://Example.invalid/img/a%20b.png?v=1");
  QVERIFY(MarkdownUtils::linkUrlToPath(dir.path(), url) != url);

  // Seed BEFORE the links exist, which is the production ordering: the seed has
  // to be in place when the re-highlight resolves the freshly inserted link, or
  // a download is issued.
  Fixture fixture(QString(), 0);
  fixture.editor()->setBasePath(dir.path());
  fixture.editor()->getPreviewMgr()->seedImageData(url, testImageData(60, 30));

  auto cursor = QTextCursor(fixture.editor()->document());
  cursor.insertText(QStringLiteral("![](") + url + QStringLiteral(")\n\n![](") + url +
                    QStringLiteral(" =300x)\n"));
  fixture.editor()->getHighlighter()->updateHighlight();
  fixture.waitForFreshAst(0);

  const QPixmap *natural = nullptr;
  const QPixmap *sized = nullptr;
  QTRY_VERIFY_WITH_TIMEOUT(
      (natural = fixture.editor()->findImageFromDocumentResourceMgr(resourceName(url, 0, 0))) !=
              nullptr &&
          (sized = fixture.editor()->findImageFromDocumentResourceMgr(resourceName(url, 300, 0))) !=
              nullptr,
      5000);
  QCOMPARE(natural->width(), 60);
  QCOMPARE(sized->width(), 300);
}

// The hand-off buffer is bounded and keyed: re-seeding one URL replaces, and a
// 5th distinct seed evicts the 1st.
void TestMarkdownEditor::testSeededImageBufferIsBounded() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());

  const QString first = QStringLiteral("https://example.invalid/1.png");
  const QString fifth = QStringLiteral("https://example.invalid/5.png");

  Fixture fixture(QString(), 0);
  fixture.editor()->setBasePath(dir.path());
  auto *previewMgr = fixture.editor()->getPreviewMgr();

  previewMgr->seedImageData(first, testImageData(10, 10));
  // Replacing in place must keep its slot, not append a duplicate.
  previewMgr->seedImageData(first, testImageData(20, 20));
  for (int i = 2; i <= 4; ++i) {
    previewMgr->seedImageData(QStringLiteral("https://example.invalid/%1.png").arg(i),
                              testImageData(10, 10));
  }

  // 4 distinct entries so far, the first still present (its re-seed did not
  // consume an extra slot) and carrying the REPLACED bytes.
  auto cursor = QTextCursor(fixture.editor()->document());
  cursor.insertText(QStringLiteral("![](") + first + QStringLiteral(")\n"));
  fixture.editor()->getHighlighter()->updateHighlight();
  fixture.waitForFreshAst(0);
  const QPixmap *img = nullptr;
  QTRY_VERIFY_WITH_TIMEOUT((img = fixture.editor()->findImageFromDocumentResourceMgr(
                                resourceName(first, 0, 0))) != nullptr,
                           5000);
  QCOMPARE(img->width(), 20);

  // A 5th distinct seed evicts the oldest, which is the first URL. Ask for BOTH
  // at a declared size that has never been built before: the 5th must render
  // from its seed, and the first - whose seed is gone and whose host does not
  // resolve - must stay absent even after the 5th has arrived.
  previewMgr->seedImageData(fifth, testImageData(30, 30));

  cursor.movePosition(QTextCursor::End);
  cursor.insertText(QStringLiteral("\n\n![](") + first + QStringLiteral(" =123x)\n\n![](") + fifth +
                    QStringLiteral(" =123x)\n"));
  fixture.editor()->getHighlighter()->updateHighlight();
  fixture.waitForFreshAst(0);

  const QPixmap *survivor = nullptr;
  QTRY_VERIFY_WITH_TIMEOUT((survivor = fixture.editor()->findImageFromDocumentResourceMgr(
                                resourceName(fifth, 123, 0))) != nullptr,
                           5000);
  QCOMPARE(survivor->width(), 123);
  QVERIFY(!fixture.editor()->findImageFromDocumentResourceMgr(resourceName(first, 123, 0)));
}

void TestMarkdownEditor::testMultiLineMarkerOnList() {
  Fixture fixture(QStringLiteral("1. a\n2. b\n3. c"), 0);
  fixture.selectAll();
  MarkdownUtils::typeStrikethrough(fixture.edit());
  QCOMPARE(fixture.text(), QStringLiteral("1. ~~a~~\n2. ~~b~~\n3. ~~c~~"));
}

void TestMarkdownEditor::testMultiLineMarkerToggleOff() {
  Fixture fixture(QStringLiteral("1. a\n2. b\n3. c"), 0);
  fixture.selectAll();
  MarkdownUtils::typeStrikethrough(fixture.edit());
  // Without rebuilding the selection: the restored selection is
  // syntax-inclusive, so the same action unwraps.
  MarkdownUtils::typeStrikethrough(fixture.edit());
  QCOMPARE(fixture.text(), QStringLiteral("1. a\n2. b\n3. c"));
}

void TestMarkdownEditor::testMultiLineMarkerMixedSelection() {
  Fixture fixture(QStringLiteral("~~a~~\nb"), 0);
  fixture.selectAll();
  MarkdownUtils::typeStrikethrough(fixture.edit());
  QCOMPARE(fixture.text(), QStringLiteral("~~a~~\n~~b~~"));
  // The first range received no insertion, but the selection still starts at
  // its opening marker.
  QCOMPARE(fixture.edit()->getSelection().start(), 0);
  QCOMPARE(fixture.edit()->getSelection().end(), fixture.blockEnd(1));

  // Normalizing toggle: the second press unwraps everything.
  MarkdownUtils::typeStrikethrough(fixture.edit());
  QCOMPARE(fixture.text(), QStringLiteral("a\nb"));
}

void TestMarkdownEditor::testMultiLineMarkerNesting() {
  Fixture fixture(QStringLiteral("1. a\n2. b\n3. c"), 0);
  fixture.selectAll();
  MarkdownUtils::typeStrikethrough(fixture.edit());
  QCOMPARE(fixture.edit()->getSelection().start(), 3);
  QCOMPARE(fixture.edit()->getSelection().end(), fixture.blockEnd(2));

  MarkdownUtils::typeBold(fixture.edit());
  QCOMPARE(fixture.text(), QStringLiteral("1. **~~a~~**\n2. **~~b~~**\n3. **~~c~~**"));
}

void TestMarkdownEditor::testMultiLineMarkerSkipsBlankLines() {
  Fixture fixture(QStringLiteral("a\n\nc"), 0);
  fixture.selectAll();
  MarkdownUtils::typeStrikethrough(fixture.edit());
  QCOMPARE(fixture.text(), QStringLiteral("~~a~~\n\n~~c~~"));
}

void TestMarkdownEditor::testMultiLineMarkerTrailingBlockBoundary() {
  Fixture fixture(QStringLiteral("a\nb\nc"), 0);
  fixture.select(0, 0, 2, 0);
  MarkdownUtils::typeStrikethrough(fixture.edit());
  QCOMPARE(fixture.text(), QStringLiteral("~~a~~\n~~b~~\nc"));
}

void TestMarkdownEditor::testMultiLineMarkerPartialEdges() {
  Fixture fixture(QStringLiteral("abcdef\nghijkl\nmnopqr"), 0);
  fixture.select(0, 2, 2, 3);
  MarkdownUtils::typeStrikethrough(fixture.edit());
  QCOMPARE(fixture.text(), QStringLiteral("ab~~cdef~~\n~~ghijkl~~\n~~mno~~pqr"));
}

void TestMarkdownEditor::testMultiLineMarkerPrefixes() {
  Fixture fixture(QStringLiteral("> quoted\n## Title\n## 1.2. Title\n- [ ] todo\n> - nested\n"
                                 "  ## Indented\n  ## 1.2. Indented\n> ## Quoted title"),
                  0);
  fixture.selectAll();
  MarkdownUtils::typeStrikethrough(fixture.edit());
  QCOMPARE(fixture.blockText(0), QStringLiteral("> ~~quoted~~"));
  QCOMPARE(fixture.blockText(1), QStringLiteral("## ~~Title~~"));
  QCOMPARE(fixture.blockText(2), QStringLiteral("## 1.2. ~~Title~~"));
  QCOMPARE(fixture.blockText(3), QStringLiteral("- [ ] ~~todo~~"));
  QCOMPARE(fixture.blockText(4), QStringLiteral("> - ~~nested~~"));
  QCOMPARE(fixture.blockText(5), QStringLiteral("  ## ~~Indented~~"));
  QCOMPARE(fixture.blockText(6), QStringLiteral("  ## 1.2. ~~Indented~~"));
  QCOMPARE(fixture.blockText(7), QStringLiteral("> ## ~~Quoted title~~"));
}

void TestMarkdownEditor::testMultiLineMarkerSingleBlockUnchanged() {
  // Single-block selection.
  {
    Fixture fixture(QStringLiteral("abc"), 0);
    fixture.select(0, 0, 0, 3);
    MarkdownUtils::typeStrikethrough(fixture.edit());
    QCOMPARE(fixture.text(), QStringLiteral("~~abc~~"));
    // The single-line path does not restore a syntax-inclusive selection, so
    // the selection has to be rebuilt to unwrap. Unchanged behavior.
    fixture.select(0, 0, 0, 7);
    MarkdownUtils::typeStrikethrough(fixture.edit());
    QCOMPARE(fixture.text(), QStringLiteral("abc"));
  }
  // No selection.
  {
    Fixture fixture(QStringLiteral("abc"), 0, 3);
    MarkdownUtils::typeStrikethrough(fixture.edit());
    QCOMPARE(fixture.text(), QStringLiteral("abc~~~~"));
    QCOMPARE(fixture.edit()->textCursor().positionInBlock(), 5);
  }
}

void TestMarkdownEditor::testMultiLineMarkerAllMarkers() {
  struct Case {
    void (*m_func)(VTextEdit *);
    const char *m_expected;
  };
  const Case cases[] = {
      {&MarkdownUtils::typeBold, "**a**\n**b**"},
      {&MarkdownUtils::typeItalic, "*a*\n*b*"},
      {&MarkdownUtils::typeStrikethrough, "~~a~~\n~~b~~"},
      {&MarkdownUtils::typeMark, "<mark>a</mark>\n<mark>b</mark>"},
      {&MarkdownUtils::typeCode, "`a`\n`b`"},
      {&MarkdownUtils::typeMath, "$a$\n$b$"},
  };

  for (const auto &c : cases) {
    Fixture fixture(QStringLiteral("a\nb"), 0);
    fixture.selectAll();
    c.m_func(fixture.edit());
    QCOMPARE(fixture.text(), QString::fromUtf8(c.m_expected));
    // And back.
    c.m_func(fixture.edit());
    QCOMPARE(fixture.text(), QStringLiteral("a\nb"));
  }
}

void TestMarkdownEditor::testMultiLineMarkerUndo() {
  Fixture fixture(QStringLiteral("1. a\n2. b\n3. c"), 0);
  const auto before = fixture.text();
  fixture.selectAll();
  MarkdownUtils::typeStrikethrough(fixture.edit());
  QVERIFY(fixture.text() != before);

  fixture.edit()->undo();
  QCOMPARE(fixture.text(), before);
}

void TestMarkdownEditor::testMultiLineMarkerBlankOnly() {
  Fixture fixture(QStringLiteral("  \n\n \n"), 0);
  fixture.selectAll();
  const auto before = fixture.text();
  const int revision = fixture.edit()->document()->revision();
  MarkdownUtils::typeStrikethrough(fixture.edit());
  QCOMPARE(fixture.text(), before);
  QCOMPARE(fixture.edit()->document()->revision(), revision);
}

void TestMarkdownEditor::testMultiLineMarkerOverriddenSelection() {
  Fixture fixture(QStringLiteral("a\nb"), 0, 0);
  fixture.edit()->setOverriddenSelection(0, fixture.blockEnd(1));
  QVERIFY(fixture.edit()->hasSelection());
  MarkdownUtils::typeStrikethrough(fixture.edit());
  QCOMPARE(fixture.text(), QStringLiteral("~~a~~\n~~b~~"));
}

// Converting a multi-line selection numbers the items sequentially instead of
// writing `1.` on every line.
void TestMarkdownEditor::testOrderedListSequentialNumbering() {
  Fixture fixture(QStringLiteral("a\nb\nc"), 0);
  fixture.selectAll();
  MarkdownUtils::typeOrderedList(fixture.edit());
  QCOMPARE(fixture.text(), QStringLiteral("1. a\n2. b\n3. c"));
}

void TestMarkdownEditor::testOrderedListFromOtherListTypes() {
  {
    Fixture fixture(QStringLiteral("* a\n* b\n* c"), 0);
    fixture.selectAll();
    MarkdownUtils::typeOrderedList(fixture.edit());
    QCOMPARE(fixture.text(), QStringLiteral("1. a\n2. b\n3. c"));
  }
  {
    Fixture fixture(QStringLiteral("- [ ] a\n- [x] b"), 0);
    fixture.selectAll();
    MarkdownUtils::typeOrderedList(fixture.edit());
    QCOMPARE(fixture.text(), QStringLiteral("1. a\n2. b"));
  }
}

void TestMarkdownEditor::testOrderedListIndentationLevels() {
  // Each indentation level keeps its own counter.
  {
    Fixture fixture(QStringLiteral("a\n  b\n  c\nd"), 0);
    fixture.selectAll();
    MarkdownUtils::typeOrderedList(fixture.edit());
    QCOMPARE(fixture.text(), QStringLiteral("1. a\n  1. b\n  2. c\n2. d"));
  }
  // Leaving a deeper level discards its counter, so it restarts at 1.
  {
    Fixture fixture(QStringLiteral("  a\nb\n  c"), 0);
    fixture.selectAll();
    MarkdownUtils::typeOrderedList(fixture.edit());
    QCOMPARE(fixture.text(), QStringLiteral("  1. a\n1. b\n  1. c"));
  }
  // A toggled-off ordered line consumes no number but still resets the deeper
  // level below it.
  {
    Fixture fixture(QStringLiteral("  a\n1. parent\n  b"), 0);
    fixture.selectAll();
    MarkdownUtils::typeOrderedList(fixture.edit());
    QCOMPARE(fixture.text(), QStringLiteral("  1. a\nparent\n  1. b"));
  }
}

void TestMarkdownEditor::testOrderedListToggleOff() {
  Fixture fixture(QStringLiteral("1. a\n2. b\n3. c"), 0);
  fixture.selectAll();
  MarkdownUtils::typeOrderedList(fixture.edit());
  QCOMPARE(fixture.text(), QStringLiteral("a\nb\nc"));
}

void TestMarkdownEditor::testOrderedListSingleLine() {
  Fixture fixture(QStringLiteral("abc"), 0, 0);
  MarkdownUtils::typeOrderedList(fixture.edit());
  QCOMPARE(fixture.text(), QStringLiteral("1. abc"));
}

// Bounding only the DECLARED axis is not enough. scaleImage() preserves the
// aspect ratio when one axis is unspecified, so an extreme-ratio source turns
// an in-bounds `=4096x` into an enormous allocation along the other axis: a
// 1x8000 image would ask for 4096 x 32,768,000 px. The bound applies to the
// pixmap actually produced.
void TestMarkdownEditor::testAspectRatioDerivedAxisIsBounded() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  QVERIFY(!writeTestImage(dir, QStringLiteral("tall.png"), 1, 8000).isEmpty());

  withImageEditor(QStringLiteral("![](tall.png =4096x)\n"), dir.path(), [](Fixture &fixture) {
    const QPixmap *img = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((img = fixture.editor()->findImageFromDocumentResourceMgr(
                                  resourceName(QStringLiteral("tall.png"), 4096, 0))) != nullptr,
                             5000);
    // Neither axis may exceed the bound, whichever one the ratio derived.
    QVERIFY2(img->width() <= 4096 * 4, qPrintable(QString::number(img->width())));
    QVERIFY2(img->height() <= 4096 * 4, qPrintable(QString::number(img->height())));
  });
}

namespace {
const QString c_tableSource = QStringLiteral("| h1 | header2 |\n| --- | --- |\n| a | b |\n");
const QString c_tableSourceEdited = QStringLiteral("| h1 | header2 |\n| --- | --- |\n| z | b |\n");
const QString c_tableSourceAligned =
    QStringLiteral("| h1  | header2 |\n| --- | ------- |\n| z   | b       |\n");

QSharedPointer<MarkdownEditorConfig> makeTableSourceConfig(bool p_enabled = true) {
  auto config = makeConfig();
  config->m_autoFormatTableSourceEnabled = p_enabled;
  // Source formatting must not depend on the preview type mask or a sheet.
  config->m_inplacePreviewSources = MarkdownEditorConfig::NoInplacePreview;
  return config;
}

int tableSourcePosition(QTextDocument *p_doc, int p_block, int p_column) {
  return p_doc->findBlockByNumber(p_block).position() + p_column;
}

void selectTableSource(VMarkdownEditor &p_editor, int p_anchorBlock, int p_anchorColumn,
                       int p_positionBlock, int p_positionColumn) {
  QTextCursor cursor(p_editor.document());
  cursor.setPosition(tableSourcePosition(p_editor.document(), p_anchorBlock, p_anchorColumn));
  cursor.setPosition(tableSourcePosition(p_editor.document(), p_positionBlock, p_positionColumn),
                     QTextCursor::KeepAnchor);
  p_editor.getTextEdit()->setTextCursor(cursor);
}

void replaceTableSource(VMarkdownEditor &p_editor, int p_block, int p_column, int p_length,
                        const QString &p_text) {
  // Deliberately use an external cursor, not the widget's key-event pipeline.
  QTextCursor cursor(p_editor.document());
  cursor.setPosition(tableSourcePosition(p_editor.document(), p_block, p_column));
  cursor.setPosition(cursor.position() + p_length, QTextCursor::KeepAnchor);
  cursor.beginEditBlock();
  cursor.insertText(p_text);
  cursor.endEditBlock();
}
} // namespace

void TestMarkdownEditor::testTableSourceFormatDebounce() {
  for (bool enabled : {true, false}) {
    VMarkdownEditor editor(makeTableSourceConfig(enabled),
                           QSharedPointer<TextEditorParameters>::create());
    editor.setText(c_tableSource);
    QTRY_VERIFY_WITH_TIMEOUT(editor.getHighlighter()->getBlockContext(2).m_fresh, 5000);
    auto edit = editor.getTextEdit();
    selectTableSource(editor, 2, 2, 2, 3);
    QTest::keyClicks(edit, QStringLiteral("z"));
    QCOMPARE(editor.document()->toPlainText(), c_tableSourceEdited);
    QTest::qWait(200);
    QCOMPARE(editor.document()->toPlainText(), c_tableSourceEdited);

    const QString twiceEdited = QStringLiteral("| h1 | header2 |\n| --- | --- |\n| zx | b |\n");
    const QString aligned =
        QStringLiteral("| h1  | header2 |\n| --- | ------- |\n| zx  | b       |\n");
    QElapsedTimer sinceLastKey;
    qint64 firstFormattedAt = -1;
    const auto transition =
        connect(editor.document(), &QTextDocument::contentsChanged, &editor, [&]() {
          if (firstFormattedAt < 0 && editor.document()->toPlainText() == aligned) {
            firstFormattedAt = sinceLastKey.elapsed();
          }
        });
    sinceLastKey.start();
    QTest::keyClicks(edit, QStringLiteral("x"));
    QTest::qWait(200);
    QCOMPARE(editor.document()->toPlainText(), twiceEdited);
    if (enabled) {
      QTRY_COMPARE_WITH_TIMEOUT(editor.document()->toPlainText(), aligned, 5000);
      QVERIFY2(firstFormattedAt >= 500, qPrintable(QString::number(firstFormattedAt)));
    } else {
      QTest::qWait(800);
      QCOMPARE(editor.document()->toPlainText(), twiceEdited);
      QCOMPARE(firstFormattedAt, qint64(-1));
    }
    disconnect(transition);
  }
}

void TestMarkdownEditor::testTableSourceFormatProgrammaticEdits() {
  const QString firstGap = QStringLiteral("\nBetween first and middle.\n\n");
  const QString middle = QStringLiteral("| keep | untouched |\n| --- | --- |\n| mid | m |\n");
  const QString secondGap = QStringLiteral("\nBetween middle and last.\n\n");
  const QString last = QStringLiteral("| t | tail |\n|---|---|\n| c | d |\n");
  const QString source = c_tableSource + firstGap + middle + secondGap + last;
  const QString expected = c_tableSourceAligned + firstGap + middle + secondGap +
                           QStringLiteral("| t   | tail |\n| --- | ---- |\n| q   | d    |\n");
  VMarkdownEditor editor(makeTableSourceConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.setText(source);
  auto doc = editor.document();
  QTRY_VERIFY_WITH_TIMEOUT(editor.getHighlighter()->getBlockContext(14).m_fresh, 5000);
  selectTableSource(editor, 8, 3, 8, 3);

  // Qt reports one broad change spanning an untouched table in the middle.
  QTextCursor cursor(doc);
  cursor.beginEditBlock();
  cursor.setPosition(tableSourcePosition(doc, 14, 2));
  cursor.setPosition(cursor.position() + 1, QTextCursor::KeepAnchor);
  cursor.insertText(QStringLiteral("q"));
  cursor.setPosition(tableSourcePosition(doc, 2, 2));
  cursor.setPosition(cursor.position() + 1, QTextCursor::KeepAnchor);
  cursor.insertText(QStringLiteral("z"));
  cursor.endEditBlock();
  QTRY_COMPARE_WITH_TIMEOUT(doc->toPlainText(), expected, 5000);
  QCOMPARE(editor.getTextEdit()->textCursor().blockNumber(), 8);
  QCOMPARE(editor.getTextEdit()->textCursor().positionInBlock(), 3);

  // Retained block identity, rather than old absolute positions, determines dirtiness.
  cursor.setPosition(0);
  cursor.insertText(QStringLiteral("Preface.\n\n"));
  const QString shifted = QStringLiteral("Preface.\n\n") + expected;
  const int shiftedUndoSteps = doc->availableUndoSteps();
  QTest::qWait(800);
  QCOMPARE(doc->toPlainText(), shifted);
  QCOMPARE(doc->availableUndoSteps(), shiftedUndoSteps);
  QCOMPARE(editor.getTextEdit()->textCursor().blockNumber(), 10);
  QCOMPARE(editor.getTextEdit()->textCursor().positionInBlock(), 3);

  // Qt splits the first block when lines are inserted at its column zero.
  // Its old handle belongs to the prose, not the unchanged header following it.
  editor.setText(c_tableSource);
  QTRY_VERIFY_WITH_TIMEOUT(editor.getHighlighter()->getBlockContext(2).m_fresh, 5000);
  cursor = QTextCursor(doc);
  cursor.beginEditBlock();
  cursor.insertText(QStringLiteral("Intro.\n\n"));
  cursor.movePosition(QTextCursor::End);
  cursor.insertText(QStringLiteral("\nAfterward.\n"));
  cursor.endEditBlock();
  QTest::qWait(800);
  QCOMPARE(doc->toPlainText(),
           QStringLiteral("Intro.\n\n") + c_tableSource + QStringLiteral("\nAfterward.\n"));
  cursor.setPosition(0);
  cursor.setPosition(8, QTextCursor::KeepAnchor);
  cursor.removeSelectedText();
  QTest::qWait(800);
  QCOMPARE(doc->toPlainText(), c_tableSource + QStringLiteral("\nAfterward.\n"));

  // An inserted duplicate inherits the old header handle, but none of its
  // following rows. Only the new table formats; the original remains untouched.
  editor.setText(c_tableSource);
  QTRY_VERIFY_WITH_TIMEOUT(editor.getHighlighter()->getBlockContext(2).m_fresh, 5000);
  cursor = QTextCursor(doc);
  cursor.insertText(c_tableSource + QLatin1Char('\n'));
  const QString duplicate =
      QStringLiteral("| h1  | header2 |\n| --- | ------- |\n| a   | b       |\n\n") + c_tableSource;
  QTRY_COMPARE_WITH_TIMEOUT(doc->toPlainText(), duplicate, 5000);

  editor.setText(c_tableSource);
  QTRY_VERIFY_WITH_TIMEOUT(editor.getHighlighter()->getBlockContext(2).m_fresh, 5000);
  doc->setUndoRedoEnabled(false);
  replaceTableSource(editor, 2, 2, 1, QStringLiteral("z"));
  selectTableSource(editor, 2, 3, 2, 3);
  QTRY_COMPARE_WITH_TIMEOUT(doc->toPlainText(), c_tableSourceAligned, 5000);
  QVERIFY(!doc->isUndoRedoEnabled());
  QVERIFY(!doc->isUndoAvailable());
  QVERIFY(!doc->isRedoAvailable());
}

void TestMarkdownEditor::testTableSourceFormatLoadAndConfig() {
  // Each replacement clears a populated document, including the terminal character.
  for (int load = 0; load < 3; ++load) {
    VMarkdownEditor editor(makeTableSourceConfig(), QSharedPointer<TextEditorParameters>::create());
    editor.setText(QStringLiteral("previous document\n"));
    if (load == 0) {
      editor.setText(c_tableSource);
    } else if (load == 1) {
      editor.getTextEdit()->setPlainText(c_tableSource);
    } else {
      editor.document()->clear();
      editor.setText(c_tableSource);
    }
    auto doc = editor.document();
    const bool modified = doc->isModified();
    const bool undoAvailable = doc->isUndoAvailable();
    QTest::qWait(800);
    QCOMPARE(doc->toPlainText(), c_tableSource);
    QCOMPARE(doc->isModified(), modified);
    QCOMPARE(doc->isUndoAvailable(), undoAvailable);
  }

  auto config = makeTableSourceConfig(false);
  VMarkdownEditor editor(config, QSharedPointer<TextEditorParameters>::create());
  editor.setText(c_tableSource);
  auto doc = editor.document();
  QTRY_VERIFY_WITH_TIMEOUT(editor.getHighlighter()->getBlockContext(2).m_fresh, 5000);
  config->m_autoFormatTableSourceEnabled = true;
  editor.setConfig(config);
  const bool modified = doc->isModified();
  const int enabledUndoSteps = doc->availableUndoSteps();
  QTest::qWait(800);
  QCOMPARE(doc->toPlainText(), c_tableSource);
  QCOMPARE(doc->isModified(), modified);
  QCOMPARE(doc->availableUndoSteps(), enabledUndoSteps);

  replaceTableSource(editor, 2, 2, 1, QStringLiteral("z"));
  QTest::qWait(200);
  config->m_autoFormatTableSourceEnabled = false;
  editor.setConfig(config);
  QTest::qWait(800);
  QCOMPARE(doc->toPlainText(), c_tableSourceEdited);

  config->m_autoFormatTableSourceEnabled = true;
  editor.setConfig(config);
  QTest::qWait(800);
  QCOMPARE(doc->toPlainText(), c_tableSourceEdited);

  // No event-loop turn or first-parse wait between loading and a real edit.
  editor.setText(c_tableSource);
  replaceTableSource(editor, 2, 2, 1, QStringLiteral("z"));
  selectTableSource(editor, 2, 3, 2, 3);
  QTRY_COMPARE_WITH_TIMEOUT(doc->toPlainText(), c_tableSourceAligned, 5000);

  // A bare clear has no paired insertion; it must not swallow the next cursor edit.
  doc->clear();
  QTest::qWait(800);
  QCOMPARE(doc->toPlainText(), QString());
  QTextCursor cursor(doc);
  cursor.insertText(c_tableSourceEdited);
  selectTableSource(editor, 2, 3, 2, 3);
  QTRY_COMPARE_WITH_TIMEOUT(doc->toPlainText(), c_tableSourceAligned, 5000);

  // With undo disabled, bare clear still differs from setPlainText's paired
  // insertion. Even an immediate external cursor edit must not be swallowed.
  doc->setUndoRedoEnabled(false);
  doc->clear();
  cursor = QTextCursor(doc);
  cursor.insertText(c_tableSourceEdited);
  selectTableSource(editor, 2, 3, 2, 3);
  QTRY_COMPARE_WITH_TIMEOUT(doc->toPlainText(), c_tableSourceAligned, 5000);
  QVERIFY(!doc->isUndoRedoEnabled());
  QVERIFY(!doc->isUndoAvailable());
  doc->setUndoRedoEnabled(true);

  // Removing every visible character does not remove Qt's terminal character.
  editor.setText(QStringLiteral("replace all this visible source\n"));
  QTRY_VERIFY_WITH_TIMEOUT(editor.getHighlighter()->getBlockContext(0).m_fresh, 5000);
  cursor = QTextCursor(doc);
  cursor.select(QTextCursor::Document);
  cursor.insertText(c_tableSourceEdited);
  selectTableSource(editor, 2, 3, 2, 3);
  QTRY_COMPARE_WITH_TIMEOUT(doc->toPlainText(), c_tableSourceAligned, 5000);
}

void TestMarkdownEditor::testTableSourceFormatCursorAndSelection_data() {
  QTest::addColumn<QString>("source");
  QTest::addColumn<QString>("expected");
  QTest::addColumn<int>("anchorBlock");
  QTest::addColumn<int>("anchorColumn");
  QTest::addColumn<int>("positionBlock");
  QTest::addColumn<int>("positionColumn");
  QTest::addColumn<int>("mappedAnchorColumn");
  QTest::addColumn<int>("mappedPositionColumn");
  QTest::addColumn<QString>("selected");
  QTest::addColumn<bool>("overridden");

  QTest::newRow("caret-after-body-value")
      << c_tableSource << c_tableSourceAligned << 2 << 7 << 2 << 7 << 9 << 9 << QString() << false;
  QTest::newRow("forward-body-selection") << c_tableSource << c_tableSourceAligned << 2 << 6 << 2
                                          << 7 << 8 << 9 << QStringLiteral("b") << false;
  QTest::newRow("backward-body-selection") << c_tableSource << c_tableSourceAligned << 2 << 7 << 2
                                           << 6 << 9 << 8 << QStringLiteral("b") << false;
  QTest::newRow("caret-inside-header-content") << c_tableSource << c_tableSourceAligned << 0 << 10
                                               << 0 << 10 << 11 << 11 << QString() << false;
  QTest::newRow("cross-table-next-block-column-zero")
      << c_tableSource << c_tableSourceAligned << 2 << 6 << 3 << 0 << 8 << 0
      << QStringLiteral("b       |\u2029") << false;
  QTest::newRow("overridden-selection-with-unselected-caret")
      << c_tableSource << c_tableSourceAligned << 2 << 3 << 2 << 3 << 3 << 3 << QString() << true;

  QTest::newRow("duplicate-values-use-the-second-occurrence")
      << QStringLiteral("| h1 | header2 | third |\n| --- | --- | --- |\n| a | same | same |\n")
      << QStringLiteral("| h1  | header2 | third |\n| --- | ------- | ----- |\n"
                        "| z   | same    | same  |\n")
      << 2 << 13 << 2 << 17 << 18 << 22 << QStringLiteral("same") << false;
  QTest::newRow("escaped-pipe-is-content")
      << QStringLiteral("| h1 | header2 |\n| --- | --- |\n| a | c\\|d |\n")
      << QStringLiteral("| h1  | header2 |\n| --- | ------- |\n| z   | c\\|d    |\n") << 2 << 7 << 2
      << 10 << 9 << 12 << QStringLiteral("\\|d") << false;

  // The second value starts at UTF-16 column 6, then 8 after formatting:
  // CJK occupies one unit, e + combining acute two, and the emoji two.
  const QString unicodeSource =
      QStringLiteral("| h1 | header2 |\n| --- | --- |\n| a | \u4E2De\u0301\U0001F600 |\n");
  const QString unicodeExpected =
      QStringLiteral("| h1  | header2 |\n| --- | ------- |\n| z   | \u4E2De\u0301\U0001F600   |\n");
  QTest::newRow("cjk-code-unit-not-display-width")
      << unicodeSource << unicodeExpected << 2 << 6 << 2 << 7 << 8 << 9 << QStringLiteral("\u4E2D")
      << false;
  QTest::newRow("combining-mark-offset") << unicodeSource << unicodeExpected << 2 << 8 << 2 << 9
                                         << 10 << 11 << QStringLiteral("\u0301") << false;
  QTest::newRow("surrogate-pair-endpoints") << unicodeSource << unicodeExpected << 2 << 9 << 2 << 11
                                            << 11 << 13 << QStringLiteral("\U0001F600") << false;

  QTest::newRow("content-end-wins-over-unpadded-pipe")
      << QStringLiteral("| h1 | header2 |\n| --- | --- |\n| a |b|\n") << c_tableSourceAligned << 2
      << 6 << 2 << 6 << 9 << 9 << QString() << false;
  QTest::newRow("structural-pipe-keeps-its-column")
      << c_tableSource << c_tableSourceAligned << 2 << 4 << 2 << 4 << 6 << 6 << QString() << false;
  QTest::newRow("leading-padding-keeps-distance-to-content")
      << c_tableSource << c_tableSourceAligned << 2 << 5 << 2 << 5 << 7 << 7 << QString() << false;
  QTest::newRow("trailing-padding-keeps-distance-from-content")
      << QStringLiteral("| h1 | header2 |\n| --- | --- |\n| a | b  |\n") << c_tableSourceAligned
      << 2 << 8 << 2 << 8 << 10 << 10 << QString() << false;
  QTest::newRow("line-end-keeps-distance-after-final-pipe")
      << c_tableSource << c_tableSourceAligned << 2 << 9 << 2 << 9 << 17 << 17 << QString()
      << false;
  QTest::newRow("delimiter-right-colon-keeps-side")
      << QStringLiteral("| h1 | header2 |\n| :--- | ---: |\n| a | b |\n")
      << QStringLiteral("| h1   | header2 |\n| :--- | ------: |\n| z    |       b |\n") << 1 << 12
      << 1 << 12 << 15 << 15 << QString() << false;
}

void TestMarkdownEditor::testTableSourceFormatCursorAndSelection() {
  QFETCH(QString, source);
  QFETCH(QString, expected);
  QFETCH(int, anchorBlock);
  QFETCH(int, anchorColumn);
  QFETCH(int, positionBlock);
  QFETCH(int, positionColumn);
  QFETCH(int, mappedAnchorColumn);
  QFETCH(int, mappedPositionColumn);
  QFETCH(QString, selected);
  QFETCH(bool, overridden);
  VMarkdownEditor editor(makeTableSourceConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.setText(source);
  auto doc = editor.document();
  QTRY_VERIFY_WITH_TIMEOUT(editor.getHighlighter()->getBlockContext(2).m_fresh, 5000);
  replaceTableSource(editor, 2, 2, 1, QStringLiteral("z"));
  selectTableSource(editor, anchorBlock, anchorColumn, positionBlock, positionColumn);
  auto edit = editor.getTextEdit();
  if (overridden) {
    edit->setOverriddenSelection(tableSourcePosition(doc, 2, 6), tableSourcePosition(doc, 2, 7));
    QCOMPARE(edit->selectedText(), QStringLiteral("b"));
    QVERIFY(!edit->textCursor().hasSelection());
  }
  QTRY_COMPARE_WITH_TIMEOUT(doc->toPlainText(), expected, 5000);
  const auto cursor = edit->textCursor();
  QCOMPARE(cursor.anchor(), tableSourcePosition(doc, anchorBlock, mappedAnchorColumn));
  QCOMPARE(cursor.position(), tableSourcePosition(doc, positionBlock, mappedPositionColumn));
  QCOMPARE(cursor.hasSelection(), anchorBlock != positionBlock || anchorColumn != positionColumn);
  QCOMPARE(cursor.selectedText(), selected);
  if (overridden) {
    QCOMPARE(edit->getSelection().start(), tableSourcePosition(doc, 2, 8));
    QCOMPARE(edit->getSelection().end(), tableSourcePosition(doc, 2, 9));
    QCOMPARE(edit->selectedText(), QStringLiteral("b"));
    QVERIFY(!cursor.hasSelection());
  } else {
    QCOMPARE(edit->selectedText(), selected);
  }
}

void TestMarkdownEditor::testTableSourceFormatProtectedPositions() {
  struct ProtectedPosition {
    QString m_source;
    QString m_expected;
    int m_block;
    int m_column;
    int m_retainedColumn;
  };
  const ProtectedPosition cases[] = {
      {QStringLiteral("| h1 | header2 |\n| --- | --- |\n| a          | b |\n"),
       c_tableSourceAligned, 2, 10, 3},
      {QStringLiteral("| h1 | header2 |\n| --- | --- |\n| a |              |\n"),
       QStringLiteral("| h1  | header2 |\n| --- | ------- |\n| z   |         |\n"), 2, 15, 3},
      {QStringLiteral("| h1 | header2 |\n| ---------- | --- |\n| a | b |\n"), c_tableSourceAligned,
       1, 10, 3},
  };
  for (const auto &item : cases) {
    VMarkdownEditor editor(makeTableSourceConfig(), QSharedPointer<TextEditorParameters>::create());
    editor.setText(item.m_source);
    auto doc = editor.document();
    QTRY_VERIFY_WITH_TIMEOUT(editor.getHighlighter()->getBlockContext(2).m_fresh, 5000);
    replaceTableSource(editor, 2, 2, 1, QStringLiteral("z"));
    selectTableSource(editor, item.m_block, item.m_column, item.m_block, item.m_column);
    const QString edited = doc->toPlainText();
    const int position = editor.getTextEdit()->textCursor().position();
    QTest::qWait(800);
    QCOMPARE(doc->toPlainText(), edited);
    QCOMPARE(editor.getTextEdit()->textCursor().position(), position);
    QCOMPARE(editor.getTextEdit()->textCursor().anchor(), position);

    // Cursor movement, with no new source edit, releases only the deferred table.
    selectTableSource(editor, item.m_block, item.m_retainedColumn, item.m_block,
                      item.m_retainedColumn);
    QTRY_COMPARE_WITH_TIMEOUT(doc->toPlainText(), item.m_expected, 5000);
    QCOMPARE(editor.getTextEdit()->textCursor().position(),
             tableSourcePosition(doc, item.m_block, item.m_retainedColumn));
    QVERIFY(!editor.getTextEdit()->textCursor().hasSelection());
  }

  // An empty field maps by distance from its preceding pipe, not a guessed value start.
  {
    VMarkdownEditor editor(makeTableSourceConfig(), QSharedPointer<TextEditorParameters>::create());
    editor.setText(QStringLiteral("| h1 | header2 |\n| --- | --- |\n| a |  |\n"));
    QTRY_VERIFY_WITH_TIMEOUT(editor.getHighlighter()->getBlockContext(2).m_fresh, 5000);
    replaceTableSource(editor, 2, 2, 1, QStringLiteral("z"));
    selectTableSource(editor, 2, 6, 2, 6);
    QTRY_COMPARE_WITH_TIMEOUT(
        editor.document()->toPlainText(),
        QStringLiteral("| h1  | header2 |\n| --- | ------- |\n| z   |         |\n"), 5000);
    QCOMPARE(editor.getTextEdit()->textCursor().positionInBlock(), 8);
  }

  VMarkdownEditor editor(makeTableSourceConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.resize(640, 480);
  editor.show();
  editor.activateWindow();
  auto edit = editor.getTextEdit();
  edit->setFocus();
  QTRY_VERIFY_WITH_TIMEOUT(edit->hasFocus(), 5000);
  editor.setText(c_tableSource);
  auto doc = editor.document();
  QTRY_VERIFY_WITH_TIMEOUT(editor.getHighlighter()->getBlockContext(2).m_fresh, 5000);
  replaceTableSource(editor, 2, 2, 1, QStringLiteral("z"));
  selectTableSource(editor, 2, 3, 2, 3);
  QInputMethodEvent preedit(QStringLiteral("\u3042"), QList<QInputMethodEvent::Attribute>());
  QCoreApplication::sendEvent(edit, &preedit);
  const auto body = doc->findBlockByNumber(2);
  QVERIFY(body.layout());
  QCOMPARE(body.layout()->preeditAreaText(), QStringLiteral("\u3042"));
  const int preeditPosition = edit->textCursor().position();
  QTest::qWait(800);
  QCOMPARE(doc->toPlainText(), c_tableSourceEdited);
  QCOMPARE(body.layout()->preeditAreaText(), QStringLiteral("\u3042"));
  QCOMPARE(edit->textCursor().position(), preeditPosition);
  QVERIFY(edit->hasFocus());

  QInputMethodEvent commit;
  commit.setCommitString(QStringLiteral("\u3042"));
  QCoreApplication::sendEvent(edit, &commit);
  const QString committed = QStringLiteral("| h1 | header2 |\n| --- | --- |\n| z\u3042 | b |\n");
  const QString aligned =
      QStringLiteral("| h1  | header2 |\n| --- | ------- |\n| z\u3042 | b       |\n");
  QCOMPARE(doc->toPlainText(), committed);
  QTest::qWait(200);
  QCOMPARE(doc->toPlainText(), committed);
  QTRY_COMPARE_WITH_TIMEOUT(doc->toPlainText(), aligned, 5000);
  QCOMPARE(doc->findBlockByNumber(2).layout()->preeditAreaText(), QString());
  QCOMPARE(edit->textCursor().positionInBlock(), 4);
}

void TestMarkdownEditor::testTableSourceFormatUndoRedo() {
  for (bool programmatic : {false, true}) {
    VMarkdownEditor editor(makeTableSourceConfig(), QSharedPointer<TextEditorParameters>::create());
    editor.setText(c_tableSource);
    auto doc = editor.document();
    auto edit = editor.getTextEdit();
    QTRY_VERIFY_WITH_TIMEOUT(editor.getHighlighter()->getBlockContext(2).m_fresh, 5000);
    replaceTableSource(editor, 2, 2, 1, QStringLiteral("z"));
    selectTableSource(editor, 2, 3, 2, 3);
    QTRY_COMPARE_WITH_TIMEOUT(doc->toPlainText(), c_tableSourceAligned, 5000);

    if (programmatic) {
      doc->undo();
    } else {
      edit->undo();
    }
    QCOMPARE(doc->toPlainText(), c_tableSource);
    QVERIFY(doc->isRedoAvailable());
    const int redoSteps = doc->availableRedoSteps();
    QTest::qWait(800);
    QCOMPARE(doc->toPlainText(), c_tableSource);
    QCOMPARE(doc->availableRedoSteps(), redoSteps);
    QVERIFY(doc->isRedoAvailable());

    if (programmatic) {
      doc->redo();
    } else {
      edit->redo();
    }
    QCOMPARE(doc->toPlainText(), c_tableSourceAligned);
    const int undoSteps = doc->availableUndoSteps();
    QTest::qWait(800);
    QCOMPARE(doc->toPlainText(), c_tableSourceAligned);
    QCOMPARE(doc->availableUndoSteps(), undoSteps);
    QVERIFY(!doc->isRedoAvailable());

    // Undo a real edit before its debounce, then branch from the undone state.
    editor.setText(c_tableSource);
    QTRY_VERIFY_WITH_TIMEOUT(editor.getHighlighter()->getBlockContext(2).m_fresh, 5000);
    replaceTableSource(editor, 2, 2, 1, QStringLiteral("z"));
    QTest::qWait(200);
    if (programmatic) {
      doc->undo();
    } else {
      edit->undo();
    }
    QCOMPARE(doc->toPlainText(), c_tableSource);
    QVERIFY(doc->isRedoAvailable());
    QTest::qWait(800);
    QCOMPARE(doc->toPlainText(), c_tableSource);
    QVERIFY(doc->isRedoAvailable());

    replaceTableSource(editor, 2, 2, 1, QStringLiteral("y"));
    selectTableSource(editor, 2, 3, 2, 3);
    QVERIFY(!doc->isRedoAvailable());
    const QString branched =
        QStringLiteral("| h1  | header2 |\n| --- | ------- |\n| y   | b       |\n");
    QTRY_COMPARE_WITH_TIMEOUT(doc->toPlainText(), branched, 5000);
    if (programmatic) {
      doc->undo();
    } else {
      edit->undo();
    }
    QCOMPARE(doc->toPlainText(), c_tableSource);
  }
}

void TestMarkdownEditor::testTableSourceFormatSyntaxBoundaries() {
  {
    VMarkdownEditor editor(makeTableSourceConfig(), QSharedPointer<TextEditorParameters>::create());
    const QString source = QStringLiteral("> | left | center | right | \u4E2D\u6587 |\n"
                                          "> | :--- | :---: | ---: | --- |\n"
                                          "> | a | b | c\\|d | e |\n");
    // The existing sheet/parser fixture, with explicit full-source expectations.
    const QString expected =
        QStringLiteral("> | left               | center | right | \u4E2D\u6587 |\n"
                       "> | :----------------- | :----: | ----: | ---- |\n"
                       "> | a much wider value |   b    |  c\\|d | e    |\n");
    editor.setText(source);
    QTRY_VERIFY_WITH_TIMEOUT(editor.getHighlighter()->getBlockContext(2).m_fresh, 5000);
    replaceTableSource(editor, 2, 4, 1, QStringLiteral("a much wider value"));
    selectTableSource(editor, 2, 1, 2, 1);
    QTRY_COMPARE_WITH_TIMEOUT(editor.document()->toPlainText(), expected, 5000);
    QCOMPARE(editor.getTextEdit()->textCursor().positionInBlock(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(editor.getHighlighter()->getBlockContext(2).m_fresh, 5000);
  }

  {
    VMarkdownEditor editor(makeTableSourceConfig(), QSharedPointer<TextEditorParameters>::create());
    editor.setText(QStringLiteral("| h1 | header2 |\n| --- |  |\n| a | b |\n"));
    QTRY_VERIFY_WITH_TIMEOUT(editor.getHighlighter()->getBlockContext(2).m_fresh, 5000);
    replaceTableSource(editor, 1, 8, 0, QStringLiteral("---"));
    selectTableSource(editor, 2, 3, 2, 3);
    QTRY_COMPARE_WITH_TIMEOUT(
        editor.document()->toPlainText(),
        QStringLiteral("| h1  | header2 |\n| --- | ------- |\n| a   | b       |\n"), 5000);
  }

  struct UnsupportedSource {
    QString m_source;
    QString m_edited;
    int m_block;
    int m_column;
  };
  const UnsupportedSource unsupported[] = {
      {QStringLiteral("| h1 | header2 |\n| --- |  |\n| a | b |\n"),
       QStringLiteral("| h1 | header2 |\n| --- |  |\n| z | b |\n"), 2, 2},
      {QStringLiteral("| h1 | header2 |\nnot a delimiter\n| a | b |\n"),
       QStringLiteral("| h1 | header2 |\nnot a delimiter\n| z | b |\n"), 2, 2},
      {QStringLiteral("```markdown\n| h1 | header2 |\n| --- | --- |\n| a | b |\n```\n"),
       QStringLiteral("```markdown\n| h1 | header2 |\n| --- | --- |\n| z | b |\n```\n"), 3, 2},
      {QStringLiteral("<table>\n<tr><th>h1</th><th>header2</th></tr>\n"
                      "<tr><td>a</td><td>b</td></tr>\n</table>\n"),
       QStringLiteral("<table>\n<tr><th>h1</th><th>header2</th></tr>\n"
                      "<tr><td>z</td><td>b</td></tr>\n</table>\n"),
       2, 8},
      // These valid quote spellings have different continuation prefixes. Copying
      // the delimiter's prefix over the body would silently rewrite the container.
      {QStringLiteral("> | h1 | header2 |\n> | --- | --- |\n>| a | b |\n"),
       QStringLiteral("> | h1 | header2 |\n> | --- | --- |\n>| z | b |\n"), 2, 3},
      {QStringLiteral("| h1 | header2 |\n| --- | --- |\n| a | b | excess |\n"),
       QStringLiteral("| h1 | header2 |\n| --- | --- |\n| z | b | excess |\n"), 2, 2},
  };
  for (const auto &item : unsupported) {
    VMarkdownEditor editor(makeTableSourceConfig(), QSharedPointer<TextEditorParameters>::create());
    editor.setText(item.m_source);
    QTRY_VERIFY_WITH_TIMEOUT(editor.getHighlighter()->getBlockContext(item.m_block).m_fresh, 5000);
    replaceTableSource(editor, item.m_block, item.m_column, 1, QStringLiteral("z"));
    selectTableSource(editor, item.m_block, item.m_column + 1, item.m_block, item.m_column + 1);
    auto doc = editor.document();
    QCOMPARE(doc->toPlainText(), item.m_edited);
    const int undoSteps = doc->availableUndoSteps();
    QTest::qWait(800);
    QCOMPARE(doc->toPlainText(), item.m_edited);
    QCOMPARE(doc->availableUndoSteps(), undoSteps);
  }

  {
    VMarkdownEditor editor(makeTableSourceConfig(), QSharedPointer<TextEditorParameters>::create());
    editor.setText(QStringLiteral("| h1 | header2 |\n| --- | --- |\n| a |\n"));
    QTRY_VERIFY_WITH_TIMEOUT(editor.getHighlighter()->getBlockContext(2).m_fresh, 5000);
    replaceTableSource(editor, 2, 2, 1, QStringLiteral("z"));
    // The old last pipe closes the first field, not the newly inserted empty one.
    selectTableSource(editor, 2, 4, 2, 4);
    QTRY_COMPARE_WITH_TIMEOUT(
        editor.document()->toPlainText(),
        QStringLiteral("| h1  | header2 |\n| --- | ------- |\n| z   |         |\n"), 5000);
    QCOMPARE(editor.getTextEdit()->textCursor().positionInBlock(), 6);
  }

  {
    VMarkdownEditor editor(makeTableSourceConfig(), QSharedPointer<TextEditorParameters>::create());
    // 100 CJK characters (200 columns) plus one ASCII character exceed the
    // display-width ceiling despite taking only 101 UTF-16 code units.
    const QString wide = QString(100, QChar(0x4E2D)) + QLatin1Char('w');
    const QString source = QStringLiteral("| h1 | header2 |\n| --- | ------- |\n| a | ") + wide +
                           QStringLiteral(" |\n");
    const QString expected =
        QStringLiteral("| h1 | header2 |\n| --- | --- |\n| z | ") + wide + QStringLiteral(" |\n");
    editor.setText(source);
    QTRY_VERIFY_WITH_TIMEOUT(editor.getHighlighter()->getBlockContext(2).m_fresh, 5000);
    replaceTableSource(editor, 2, 2, 1, QStringLiteral("z"));
    selectTableSource(editor, 2, 3, 2, 3);
    QTRY_COMPARE_WITH_TIMEOUT(editor.document()->toPlainText(), expected, 5000);
  }
}

void TestMarkdownEditor::testTableSourceFormatIdempotence() {
  auto config = makeTableSourceConfig();
  VMarkdownEditor editor(config, QSharedPointer<TextEditorParameters>::create());
  editor.setText(c_tableSource);
  auto doc = editor.document();
  auto edit = editor.getTextEdit();
  QTRY_VERIFY_WITH_TIMEOUT(editor.getHighlighter()->getBlockContext(2).m_fresh, 5000);
  replaceTableSource(editor, 2, 2, 1, QStringLiteral("z"));
  selectTableSource(editor, 2, 7, 2, 6);
  QTest::qWait(200);
  // Applying the same enabled value must not cancel genuine pending work.
  editor.setConfig(config);
  QTRY_COMPARE_WITH_TIMEOUT(doc->toPlainText(), c_tableSourceAligned, 5000);
  QTRY_VERIFY_WITH_TIMEOUT(editor.getHighlighter()->getBlockContext(2).m_fresh, 5000);
  const auto cursor = edit->textCursor();
  // Fresh AST delivery precedes queued rehighlight/layout work, which itself
  // advances QTextDocument::revision() without changing source. Settle it first.
  QTest::qWait(250);
  const int revision = doc->revision();
  const int undoSteps = doc->availableUndoSteps();
  const int redoSteps = doc->availableRedoSteps();
  const bool undoAvailable = doc->isUndoAvailable();
  const bool redoAvailable = doc->isRedoAvailable();
  QTest::qWait(800);
  QCOMPARE(doc->toPlainText(), c_tableSourceAligned);
  QCOMPARE(edit->textCursor().anchor(), cursor.anchor());
  QCOMPARE(edit->textCursor().position(), cursor.position());
  QCOMPARE(doc->revision(), revision);
  QCOMPARE(doc->availableUndoSteps(), undoSteps);
  QCOMPARE(doc->availableRedoSteps(), redoSteps);
  QCOMPARE(doc->isUndoAvailable(), undoAvailable);
  QCOMPARE(doc->isRedoAvailable(), redoAvailable);

  editor.getHighlighter()->rehighlight();
  editor.setConfig(config);
  QTest::qWait(250);
  const int rehighlightRevision = doc->revision();
  const int reconfiguredUndoSteps = doc->availableUndoSteps();
  const int reconfiguredRedoSteps = doc->availableRedoSteps();
  QTest::qWait(800);
  QCOMPARE(doc->toPlainText(), c_tableSourceAligned);
  QCOMPARE(edit->textCursor().anchor(), cursor.anchor());
  QCOMPARE(edit->textCursor().position(), cursor.position());
  QCOMPARE(edit->textCursor().selectedText(), QStringLiteral("b"));
  QCOMPARE(doc->revision(), rehighlightRevision);
  QCOMPARE(doc->availableUndoSteps(), reconfiguredUndoSteps);
  QCOMPARE(doc->availableRedoSteps(), reconfiguredRedoSteps);

  // A real insert/delete sequence can finish at exactly the loaded baseline.
  // It must not align that otherwise-unaligned table or add an empty command.
  editor.setText(c_tableSource);
  QTRY_VERIFY_WITH_TIMEOUT(editor.getHighlighter()->getBlockContext(2).m_fresh, 5000);
  QTextCursor cancelling(doc);
  cancelling.setPosition(tableSourcePosition(doc, 2, 3));
  cancelling.insertText(QStringLiteral("x"));
  cancelling.deletePreviousChar();
  selectTableSource(editor, 2, 3, 2, 3);
  QTRY_VERIFY_WITH_TIMEOUT(editor.getHighlighter()->getBlockContext(2).m_fresh, 5000);
  QTest::qWait(250);
  const int cancelledRevision = doc->revision();
  const int cancelledUndoSteps = doc->availableUndoSteps();
  const int cancelledRedoSteps = doc->availableRedoSteps();
  QTest::qWait(800);
  QCOMPARE(doc->toPlainText(), c_tableSource);
  QCOMPARE(doc->revision(), cancelledRevision);
  QCOMPARE(doc->availableUndoSteps(), cancelledUndoSteps);
  QCOMPARE(doc->availableRedoSteps(), cancelledRedoSteps);
  QCOMPARE(edit->textCursor().positionInBlock(), 3);
  QVERIFY(!edit->textCursor().hasSelection());
}

namespace {
QSharedPointer<MarkdownEditorConfig> makeListSourceConfig(bool p_enabled = true,
                                                          bool p_tablesEnabled = false) {
  auto config = makeTableSourceConfig(p_tablesEnabled);
  config->m_autoNumberOrderedListsEnabled = p_enabled;
  return config;
}

QSharedPointer<MarkdownEditorConfig> makeViListSourceConfig(bool p_enabled = false) {
  auto config = makeListSourceConfig(p_enabled);
  config->m_textEditorConfig->m_inputMode = InputMode::ViMode;
  return config;
}

// Clipboard-based cut/paste must not leave state behind for another test.
class ClipboardRestore {
public:
  ClipboardRestore() : m_saved(new QMimeData) {
    const auto original = QGuiApplication::clipboard()->mimeData();
    if (original) {
      for (const auto &format : original->formats()) {
        m_saved->setData(format, original->data(format));
      }
    }
  }
  ~ClipboardRestore() { QGuiApplication::clipboard()->setMimeData(m_saved.release()); }

private:
  std::unique_ptr<QMimeData> m_saved;
};

// Compare consumer-visible trees and inert rendered HTML, independently of the
// editor's projection. Only explicitly permitted ordered-list starts may differ.
void verifyListStructurePreserved(const QString &p_before, const QString &p_after,
                                  const QVector<int> &p_starts = {}, bool p_compareXml = true) {
  const auto beforeUtf8 = p_before.toUtf8();
  const auto afterUtf8 = p_after.toUtf8();
  using Tree = std::unique_ptr<cmark_node, decltype(&cmark_node_free)>;
  Tree before(cmark_parse_document(beforeUtf8.constData(), beforeUtf8.size(), CMARK_OPT_DEFAULT),
              &cmark_node_free);
  Tree after(cmark_parse_document(afterUtf8.constData(), afterUtf8.size(), CMARK_OPT_DEFAULT),
             &cmark_node_free);
  QVERIFY(before);
  QVERIFY(after);
  if (!p_starts.isEmpty()) {
    std::unique_ptr<cmark_iter, decltype(&cmark_iter_free)> iter(cmark_iter_new(before.get()),
                                                                 &cmark_iter_free);
    int ordinal = 0;
    cmark_event_type event;
    while ((event = cmark_iter_next(iter.get())) != CMARK_EVENT_DONE) {
      auto node = cmark_iter_get_node(iter.get());
      if (event == CMARK_EVENT_ENTER && cmark_node_get_type(node) == CMARK_NODE_LIST &&
          cmark_node_get_list_type(node) == CMARK_ORDERED_LIST) {
        QVERIFY(ordinal < p_starts.size());
        QVERIFY(cmark_node_set_list_start(node, p_starts[ordinal++]));
      }
    }
    QCOMPARE(ordinal, p_starts.size());
  }
  auto release = cmark_get_default_mem_allocator()->free;
  using Render = std::unique_ptr<char, decltype(release)>;
  if (p_compareXml) {
    Render beforeXml(cmark_render_xml(before.get(), CMARK_OPT_DEFAULT), release);
    Render afterXml(cmark_render_xml(after.get(), CMARK_OPT_DEFAULT), release);
    QVERIFY(beforeXml && afterXml);
    QCOMPARE(QByteArray(afterXml.get()), QByteArray(beforeXml.get()));
  }
  Render beforeHtml(cmark_render_html(before.get(), CMARK_OPT_UNSAFE), release);
  Render afterHtml(cmark_render_html(after.get(), CMARK_OPT_UNSAFE), release);
  QVERIFY(beforeHtml && afterHtml);
  QCOMPARE(QByteArray(afterHtml.get()), QByteArray(beforeHtml.get()));
}
} // namespace

void TestMarkdownEditor::testOrderedListEnterTabStartsNestedList() {
  const QString afterReturn = QStringLiteral("1. first\n2. \n2. second");
  const QString afterTab = QStringLiteral("1. first\n    1. \n2. second");
  for (bool enabled : {false, true}) {
    for (bool fresh : {false, true}) {
      auto config = makeListSourceConfig(enabled);
      config->m_textEditorConfig->m_expandTab = true;
      config->m_textEditorConfig->m_tabStopWidth = 4;
      Fixture fixture(QStringLiteral("1. first\n2. second"), 0, -1, config);
      if (fresh) {
        fixture.waitForFreshListAst();
      }

      fixture.pressReturn();
      QCOMPARE(fixture.text(), afterReturn);
      // Do not pump events: Tab must reset the marker before asynchronous numbering runs.
      QTest::keyClick(fixture.edit(), Qt::Key_Tab);
      QCOMPARE(fixture.text(), afterTab);
      QCOMPARE(fixture.edit()->textCursor().blockNumber(), 1);
      QCOMPARE(fixture.edit()->textCursor().positionInBlock(), 7);
      QVERIFY(!fixture.edit()->textCursor().hasSelection());

      fixture.edit()->undo();
      QCOMPARE(fixture.text(), afterReturn);
      fixture.edit()->redo();
      QCOMPARE(fixture.text(), afterTab);
      QCOMPARE(fixture.edit()->textCursor().blockNumber(), 1);
      QCOMPARE(fixture.edit()->textCursor().positionInBlock(), 7);
      QVERIFY(!fixture.edit()->textCursor().hasSelection());

      QTest::keyClicks(fixture.edit(), QStringLiteral("child"));
      fixture.waitForFreshListAst();
      QTest::qWait(800);
      QCOMPARE(fixture.text(), QStringLiteral("1. first\n    1. child\n2. second"));

      // Empty markers cannot interrupt a paragraph either; inspect the tree after typing.
      const auto utf8 = fixture.text().toUtf8();
      using Tree = std::unique_ptr<cmark_node, decltype(&cmark_node_free)>;
      Tree tree(cmark_parse_document(utf8.constData(), utf8.size(), CMARK_OPT_DEFAULT),
                &cmark_node_free);
      QVERIFY(tree);
      QCOMPARE(cmark_node_get_type(tree.get()), CMARK_NODE_DOCUMENT);
      const auto outer = cmark_node_first_child(tree.get());
      QVERIFY(outer);
      QCOMPARE(cmark_node_get_type(outer), CMARK_NODE_LIST);
      QCOMPARE(cmark_node_get_list_type(outer), CMARK_ORDERED_LIST);
      QVERIFY(!cmark_node_next(outer));

      const auto first = cmark_node_first_child(outer);
      QVERIFY(first);
      QCOMPARE(cmark_node_get_type(first), CMARK_NODE_ITEM);
      const auto second = cmark_node_next(first);
      QVERIFY(second);
      QCOMPARE(cmark_node_get_type(second), CMARK_NODE_ITEM);
      QVERIFY(!cmark_node_next(second));

      const auto paragraph = cmark_node_first_child(first);
      QVERIFY(paragraph);
      QCOMPARE(cmark_node_get_type(paragraph), CMARK_NODE_PARAGRAPH);
      const auto nested = cmark_node_next(paragraph);
      QVERIFY(nested);
      QCOMPARE(cmark_node_get_type(nested), CMARK_NODE_LIST);
      QCOMPARE(cmark_node_get_list_type(nested), CMARK_ORDERED_LIST);
      QCOMPARE(cmark_node_get_list_start(nested), 1);
      QVERIFY(!cmark_node_next(nested));
      const auto child = cmark_node_first_child(nested);
      QVERIFY(child);
      QCOMPARE(cmark_node_get_type(child), CMARK_NODE_ITEM);
      QVERIFY(!cmark_node_next(child));

      const auto secondParagraph = cmark_node_first_child(second);
      QVERIFY(secondParagraph);
      QCOMPARE(cmark_node_get_type(secondParagraph), CMARK_NODE_PARAGRAPH);
      QVERIFY(!cmark_node_next(secondParagraph));
    }
  }
}

void TestMarkdownEditor::testListAstContinuation() {
  struct LocalCase {
    QString m_source;
    QString m_expected;
    int m_column;
  };
  const LocalCase local[] = {
      {QStringLiteral("3) a"), QStringLiteral("3) a\n4) "), 3},
      {QStringLiteral("> - [x] a"), QStringLiteral("> - [x] a\n> - [ ] "), 8},
      {QStringLiteral("+ a"), QStringLiteral("+ a\n+ "), 2},
      {QStringLiteral("* a"), QStringLiteral("* a\n* "), 2},
      {QStringLiteral("0. a"), QStringLiteral("0. a\n1. "), 3},
  };
  for (bool fresh : {false, true}) {
    for (const auto &item : local) {
      Fixture fixture(item.m_source, 0);
      if (fresh) {
        fixture.waitForFreshListAst();
      }
      fixture.pressReturn();
      QCOMPARE(fixture.text(), item.m_expected);
      QCOMPARE(fixture.edit()->textCursor().positionInBlock(), item.m_column);
      fixture.edit()->undo();
      QCOMPARE(fixture.text(), item.m_source);
    }
  }

  // The cold path knows only the visible outer marker/current-line indentation.
  for (bool fresh : {false, true}) {
    Fixture nested(QStringLiteral("- - a"), 0);
    if (fresh) {
      nested.waitForFreshListAst();
    }
    nested.pressReturn();
    QCOMPARE(nested.text(), fresh ? QStringLiteral("- - a\n  - ") : QStringLiteral("- - a\n- "));

    Fixture paragraph(QStringLiteral("- a\n  continuation"), 1);
    if (fresh) {
      paragraph.waitForFreshListAst();
    }
    paragraph.pressReturn();
    QCOMPARE(paragraph.text(), fresh ? QStringLiteral("- a\n  continuation\n- ")
                                     : QStringLiteral("- a\n  continuation\n  "));
  }
  {
    Fixture fixture(QStringLiteral("> - a\ncontinuation"), 1);
    fixture.waitForFreshListAst();
    fixture.pressReturn();
    QCOMPARE(fixture.text(), QStringLiteral("> - a\ncontinuation\n> - "));
  }
  {
    Fixture fixture(QStringLiteral("- first\n\n  3) a\n- second\n\n  7) b"), 2);
    fixture.waitForFreshListAst();
    fixture.pressReturn();
    QCOMPARE(fixture.text(), QStringLiteral("- first\n\n  3) a\n  4) \n- second\n\n  7) b"));
  }
  {
    // A non-one ordered marker cannot interrupt the parent's paragraph.
    Fixture fixture(QStringLiteral("- first\n  3) prose"), 1);
    fixture.waitForFreshListAst();
    fixture.pressReturn();
    QCOMPARE(fixture.text(), QStringLiteral("- first\n  3) prose\n- "));
  }
  for (bool enabled : {false, true}) {
    Fixture fixture(QStringLiteral("3. a\n9. b"), 1, -1, makeListSourceConfig(enabled));
    fixture.waitForFreshListAst();
    fixture.pressReturn();
    QCOMPARE(fixture.text(),
             enabled ? QStringLiteral("3. a\n9. b\n5. ") : QStringLiteral("3. a\n9. b\n10. "));
    if (enabled) {
      QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), QStringLiteral("3. a\n4. b\n5. "), 5000);
    } else {
      QTest::qWait(800);
      QCOMPARE(fixture.text(), QStringLiteral("3. a\n9. b\n10. "));
    }
  }
}

void TestMarkdownEditor::testListAstExitAndSelection() {
  struct ExitCase {
    QString m_source;
    int m_column;
    QString m_expected;
  };
  const ExitCase exits[] = {
      {QStringLiteral("  - "), 4, QStringLiteral("  ")},
      {QStringLiteral("> - "), 4, QStringLiteral("> ")},
      {QStringLiteral("  -   "), 4, QStringLiteral("  ")},
      {QStringLiteral("> - [x]   "), 10, QStringLiteral("> ")},
  };
  for (bool fresh : {false, true}) {
    for (const auto &item : exits) {
      Fixture fixture(item.m_source, 0, item.m_column);
      if (fresh) {
        fixture.waitForFreshListAst();
      }
      fixture.pressReturn();
      QCOMPARE(fixture.text(), item.m_expected);
      QCOMPARE(fixture.edit()->textCursor().position(), item.m_expected.size());
      fixture.edit()->undo();
      QCOMPARE(fixture.text(), item.m_source);
    }
    for (const auto &marker : {QStringLiteral("- "), QStringLiteral("1. ")}) {
      Fixture fixture(marker + QStringLiteral("text"), 0, marker.size());
      if (fresh) {
        fixture.waitForFreshListAst();
      }
      fixture.pressReturn();
      const auto next = marker == QStringLiteral("- ") ? marker : QStringLiteral("2. ");
      QCOMPARE(fixture.text(), marker + QLatin1Char('\n') + next + QStringLiteral("text"));
      QCOMPARE(fixture.edit()->textCursor().blockNumber(), 1);
      QCOMPARE(fixture.edit()->textCursor().positionInBlock(), next.size());
      fixture.edit()->undo();
      QCOMPARE(fixture.text(), marker + QStringLiteral("text"));
    }
    for (bool backward : {false, true}) {
      const QString source = QStringLiteral("3) alpha\n4) beta");
      Fixture fixture(source);
      if (fresh) {
        fixture.waitForFreshListAst();
      }
      if (backward) {
        fixture.select(1, 5, 0, 5);
      } else {
        fixture.select(0, 5, 1, 5);
      }
      fixture.pressReturn();
      QCOMPARE(fixture.text(), QStringLiteral("3) al\n4) ta"));
      QCOMPARE(fixture.edit()->textCursor().position(), 9);
      QVERIFY(!fixture.edit()->textCursor().hasSelection());
      fixture.edit()->undo();
      QCOMPARE(fixture.text(), source);

      Fixture plain(QStringLiteral("plain\n> - item"));
      if (fresh) {
        plain.waitForFreshListAst();
      }
      if (backward) {
        plain.select(1, 8, 0, 5);
      } else {
        plain.select(0, 5, 1, 8);
      }
      plain.pressReturn();
      QCOMPARE(plain.text(), QStringLiteral("plain\n"));
      QCOMPARE(plain.edit()->textCursor().position(), 6);
      plain.edit()->undo();
      QCOMPARE(plain.text(), QStringLiteral("plain\n> - item"));
    }
  }
  {
    Fixture fixture(QStringLiteral("- \n  - child"), 0);
    fixture.waitForFreshListAst();
    fixture.pressReturn();
    QCOMPARE(fixture.text(), QStringLiteral("- \n- \n  - child"));
    QCOMPARE(fixture.edit()->textCursor().blockNumber(), 1);
    fixture.edit()->undo();
    QCOMPARE(fixture.text(), QStringLiteral("- \n  - child"));
  }
  {
    Fixture fixture(QStringLiteral("- text"), 0, 0);
    fixture.waitForFreshListAst();
    fixture.pressReturn();
    QCOMPARE(fixture.text(), QStringLiteral("\n- text"));
    QCOMPARE(fixture.edit()->textCursor().position(), 1);
  }
}

void TestMarkdownEditor::testListAstCodeAndStaleness() {
  struct VetoCase {
    QString m_source;
    int m_block;
    QString m_expected;
  };
  const VetoCase cases[] = {
      {QStringLiteral("```\n- a\n```"), 1, QStringLiteral("```\n- a\n\n```")},
      {QStringLiteral("    - a"), 0, QStringLiteral("    - a\n    ")},
      {QStringLiteral("<div>\n- a\n</div>"), 1, QStringLiteral("<div>\n- a\n\n</div>")},
      {QStringLiteral("- - -"), 0, QStringLiteral("- - -\n")},
      {QStringLiteral("- a\n  # heading"), 1, QStringLiteral("- a\n  # heading\n  ")},
      {QStringLiteral("- a\n\n      - code"), 2, QStringLiteral("- a\n\n      - code\n      ")},
      {QStringLiteral("- a\n\n  <div>\n  - html\n  </div>"), 3,
       QStringLiteral("- a\n\n  <div>\n  - html\n  \n  </div>")},
      {QStringLiteral("$$\n- a\n$$"), 1, QStringLiteral("$$\n- a\n\n$$")},
      {QStringLiteral("- a\n  ```\n  1. code\n  ```"), 2,
       QStringLiteral("- a\n  ```\n  1. code\n  \n  ```")},
      {QStringLiteral("- a\n  > quoted"), 1, QStringLiteral("- a\n  > quoted\n  > ")},
      {QStringLiteral("- a\n\n  | h | v |\n  | --- | --- |\n  | x | y |"), 4,
       QStringLiteral("- a\n\n  | h | v |\n  | --- | --- |\n  | x | y |\n  ")},
  };
  for (const auto &item : cases) {
    Fixture fixture(item.m_source, item.m_block);
    fixture.waitForFreshListAst();
    fixture.pressReturn();
    QCOMPARE(fixture.text(), item.m_expected);
    fixture.edit()->undo();
    QCOMPARE(fixture.text(), item.m_source);
  }
  {
    Fixture fixture(QStringLiteral("```\n3) code\n```"), 1);
    fixture.waitForFreshListAst();
    fixture.makeAstStale();
    fixture.pressReturn();
    QCOMPARE(fixture.text(), QStringLiteral("```\n3) codex\n\n```"));
  }
  {
    Fixture fixture(QStringLiteral("3) a"), 0);
    fixture.pressReturn();
    QTest::keyClicks(fixture.edit(), QStringLiteral("b"));
    fixture.pressReturn();
    QTest::keyClicks(fixture.edit(), QStringLiteral("c"));
    fixture.pressReturn();
    QCOMPARE(fixture.text(), QStringLiteral("3) a\n4) b\n5) c\n6) "));
  }
  {
    Fixture fixture(QStringLiteral("3. a"), 0);
    fixture.waitForFreshListAst();
    replaceTableSource(*fixture.editor(), 0, 0, 3, QString());
    fixture.moveTo(0);
    fixture.pressReturn();
    QCOMPARE(fixture.text(), QStringLiteral("a\n"));
  }
  {
    Fixture fixture(QStringLiteral("- a\n  continuation"), 1);
    fixture.waitForFreshListAst();
    fixture.makeAstStale();
    fixture.pressReturn();
    QCOMPARE(fixture.text(), QStringLiteral("- a\n  continuationx\n  "));
  }
  for (bool fresh : {false, true}) {
    for (const auto &number : {QStringLiteral("999999999"), QStringLiteral("1000000000")}) {
      Fixture fixture(number + QStringLiteral(". a"), 0);
      if (fresh) {
        fixture.waitForFreshListAst();
      }
      fixture.pressReturn();
      QCOMPARE(fixture.text(), number + QStringLiteral(". a\n"));
    }
  }
  {
    Fixture fixture(QStringLiteral("- - -\n\n3) a"), 0);
    fixture.waitForFreshListAst();
    fixture.pressReturn(); // A fresh structural veto must be one-Return only.
    fixture.moveTo(3);
    fixture.pressCtrlReturn();
    QCOMPARE(fixture.text(), QStringLiteral("- - -\n\n\n3) a"));
    fixture.pressReturn();
    QCOMPARE(fixture.text(), QStringLiteral("- - -\n\n\n3) a\n4) "));
  }
  {
    Fixture fixture(QStringLiteral("3) a"), 0);
    fixture.waitForFreshListAst();
    fixture.pressShiftReturn();
    QCOMPARE(fixture.text(), QStringLiteral("3) a  \n"));
    fixture.moveTo(0);
    QTest::keyClick(fixture.edit(), Qt::Key_Return, Qt::KeypadModifier);
    QCOMPARE(fixture.text(), QStringLiteral("3) a  \n4) \n"));
  }
}

void TestMarkdownEditor::testViListOpenLines() {
  struct MarkerCase {
    QString m_source;
    QString m_above;
    QString m_below;
  };
  const MarkerCase cases[] = {
      {QStringLiteral("- alpha"), QStringLiteral("- "), QStringLiteral("- ")},
      {QStringLiteral("3. alpha"), QStringLiteral("3. "), QStringLiteral("4. ")},
      {QStringLiteral("7) alpha"), QStringLiteral("7) "), QStringLiteral("8) ")},
      {QStringLiteral("> - [x] alpha"), QStringLiteral("> - [ ] "), QStringLiteral("> - [ ] ")},
      {QStringLiteral("- [x] "), QStringLiteral("- [ ] "), QStringLiteral("- [ ] ")},
      {QStringLiteral("9. "), QStringLiteral("9. "), QStringLiteral("10. ")},
  };
  for (bool fresh : {false, true}) {
    for (bool above : {false, true}) {
      for (const auto &item : cases) {
        Fixture fixture(item.m_source, 0, item.m_source.size() / 2, makeViListSourceConfig());
        if (fresh) {
          fixture.waitForFreshListAst();
        }
        QTest::keyClicks(fixture.edit(), above ? QStringLiteral("O") : QStringLiteral("o"));
        const auto prefix = above ? item.m_above : item.m_below;
        QCOMPARE(fixture.text(), above ? prefix + QLatin1Char('\n') + item.m_source
                                       : item.m_source + QLatin1Char('\n') + prefix);
        QCOMPARE(fixture.edit()->textCursor().blockNumber(), above ? 0 : 1);
        QCOMPARE(fixture.edit()->textCursor().positionInBlock(), prefix.size());
      }
    }

    // The preceding block belongs to a different list. O must classify 1), not '-'.
    Fixture fixture(QStringLiteral("- previous\n1) alpha"), 1, 5, makeViListSourceConfig());
    if (fresh) {
      fixture.waitForFreshListAst();
    }
    QTest::keyClicks(fixture.edit(), QStringLiteral("O"));
    QCOMPARE(fixture.text(), QStringLiteral("- previous\n1) \n1) alpha"));
    QCOMPARE(fixture.edit()->textCursor().blockNumber(), 1);
    QCOMPARE(fixture.edit()->textCursor().positionInBlock(), 3);
  }
}

void TestMarkdownEditor::testViListOpenContext() {
  struct ContextCase {
    QString m_source;
    int m_block;
    QString m_cold;
    QString m_fresh;
  };
  const ContextCase cases[] = {
      // Even O on block zero uses the deepest same-line item when fresh.
      {QStringLiteral("- - alpha"), 0, QStringLiteral("- "), QStringLiteral("  - ")},
      {QStringLiteral("- parent\n  continuation"), 1, QStringLiteral("  "), QStringLiteral("- ")},
      {QStringLiteral("- parent\n\n  - child\n    continuation"), 3, QStringLiteral("    "),
       QStringLiteral("  - ")},
      {QStringLiteral("> - parent\ncontinuation"), 1, QString(), QStringLiteral("> - ")},
  };
  for (bool fresh : {false, true}) {
    for (bool above : {false, true}) {
      for (const auto &item : cases) {
        Fixture fixture(item.m_source, item.m_block, 4, makeViListSourceConfig());
        if (fresh) {
          fixture.waitForFreshListAst();
        }
        QTest::keyClicks(fixture.edit(), above ? QStringLiteral("O") : QStringLiteral("o"));
        const auto prefix = fresh ? item.m_fresh : item.m_cold;
        const int insertedBlock = item.m_block + (above ? 0 : 1);
        auto expected = item.m_source.split(QLatin1Char('\n'));
        expected.insert(insertedBlock, prefix);
        QCOMPARE(fixture.text(), expected.join(QLatin1Char('\n')));
        QCOMPARE(fixture.edit()->textCursor().blockNumber(), insertedBlock);
        QCOMPARE(fixture.edit()->textCursor().positionInBlock(), prefix.size());
      }

      // The blank separator makes 3) a nested list, not a lazy parent paragraph.
      const QString source = QStringLiteral("- parent\n\n  3) alpha\n  9) sibling\n- tail");
      Fixture nested(source, 2, 6, makeViListSourceConfig());
      if (fresh) {
        nested.waitForFreshListAst();
      }
      QTest::keyClicks(nested.edit(), above ? QStringLiteral("O") : QStringLiteral("o"));
      auto expected = source.split(QLatin1Char('\n'));
      expected.insert(above ? 2 : 3, above ? QStringLiteral("  3) ") : QStringLiteral("  4) "));
      QCOMPARE(nested.text(), expected.join(QLatin1Char('\n')));
      QCOMPARE(nested.edit()->textCursor().blockNumber(), above ? 2 : 3);
      QCOMPARE(nested.edit()->textCursor().positionInBlock(), 5);
    }
  }

  struct CodeCase {
    QString m_source;
    int m_block;
    QString m_indent;
  };
  const CodeCase code[] = {
      {QStringLiteral("```\n- code\n```"), 1, QString()},
      {QStringLiteral("    - code"), 0, QStringLiteral("    ")},
      {QStringLiteral("- parent\n\n      3) code"), 2, QStringLiteral("      ")},
  };
  for (bool above : {false, true}) {
    for (const auto &item : code) {
      Fixture fixture(item.m_source, item.m_block, 4, makeViListSourceConfig());
      fixture.waitForFreshListAst();
      QTest::keyClicks(fixture.edit(), above ? QStringLiteral("O") : QStringLiteral("o"));
      const int insertedBlock = item.m_block + (above ? 0 : 1);
      auto expected = item.m_source.split(QLatin1Char('\n'));
      expected.insert(insertedBlock, item.m_indent);
      QCOMPARE(fixture.text(), expected.join(QLatin1Char('\n')));
      QCOMPARE(fixture.edit()->textCursor().blockNumber(), insertedBlock);
      QCOMPARE(fixture.edit()->textCursor().positionInBlock(), item.m_indent.size());
    }
  }
}

void TestMarkdownEditor::testViListOpenNumbering() {
  const QString untouched = QStringLiteral("\n\nseparate\n\n7. keep\n3. odd");
  const QString source = QStringLiteral("5. alpha\n9. beta\n2. gamma") + untouched;
  for (bool enabled : {false, true}) {
    for (bool above : {false, true}) {
      Fixture fixture(source, 1, 5, makeViListSourceConfig(enabled));
      fixture.waitForFreshListAst();
      QTest::keyClicks(fixture.edit(), above ? QStringLiteral("O") : QStringLiteral("o"));
      const auto prefix =
          QString::number(enabled ? (above ? 6 : 7) : (above ? 9 : 10)) + QStringLiteral(". ");
      const int insertedBlock = above ? 1 : 2;
      auto lines = source.split(QLatin1Char('\n'));
      lines.insert(insertedBlock, prefix);
      QCOMPARE(fixture.text(), lines.join(QLatin1Char('\n')));
      QCOMPARE(fixture.edit()->textCursor().blockNumber(), insertedBlock);
      QCOMPARE(fixture.edit()->textCursor().positionInBlock(), prefix.size());
      QTest::keyClicks(fixture.edit(), QStringLiteral("new"));
      QTest::keyClick(fixture.edit(), Qt::Key_Escape);
      lines[insertedBlock] += QStringLiteral("new");
      QString expected = lines.join(QLatin1Char('\n'));
      QCOMPARE(fixture.text(), expected);
      if (enabled) {
        expected = (above ? QStringLiteral("5. alpha\n6. new\n7. beta\n8. gamma")
                          : QStringLiteral("5. alpha\n6. beta\n7. new\n8. gamma")) +
                   untouched;
        QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), expected, 5000);
      } else {
        QTest::qWait(800); // Disabled numbering must leave every authored sibling alone.
        QCOMPARE(fixture.text(), expected);
      }
      QTest::keyClick(fixture.edit(), Qt::Key_U);
      QCOMPARE(fixture.text(), source);
      QTest::keyClick(fixture.edit(), Qt::Key_R, Qt::ControlModifier);
      QCOMPARE(fixture.text(), expected);
    }
  }

  Fixture first(QStringLiteral("7) alpha\n3) beta"), 0, 5, makeViListSourceConfig(true));
  first.waitForFreshListAst();
  QTest::keyClicks(first.edit(), QStringLiteral("O"));
  QCOMPARE(first.text(), QStringLiteral("7) \n7) alpha\n3) beta"));
  QCOMPARE(first.edit()->textCursor().position(), 3);
  QTest::keyClicks(first.edit(), QStringLiteral("new"));
  QTest::keyClick(first.edit(), Qt::Key_Escape);
  QTRY_COMPARE_WITH_TIMEOUT(first.text(), QStringLiteral("7) new\n8) alpha\n9) beta"), 5000);
}

void TestMarkdownEditor::testViListOpenUndoAndReplay() {
  const QString origin = QStringLiteral("- [x] alpha\n");
  const QString tail = QStringLiteral("- [ ] tail");
  const QString source = origin + tail;
  const QString inserted = QStringLiteral("- [ ] new\n");
  for (bool above : {false, true}) {
    for (int count : {1, 3}) {
      Fixture fixture(source, 0, 8, makeViListSourceConfig());
      fixture.waitForFreshListAst();
      if (count > 1) {
        QTest::keyClicks(fixture.edit(), QString::number(count));
      }
      QTest::keyClicks(fixture.edit(), above ? QStringLiteral("O") : QStringLiteral("o"));
      QTest::keyClicks(fixture.edit(), QStringLiteral("new"));
      QTest::keyClick(fixture.edit(), Qt::Key_Escape);
      const auto expected =
          above ? inserted.repeated(count) + source : origin + inserted.repeated(count) + tail;
      QCOMPARE(fixture.text(), expected);
      QTest::keyClick(fixture.edit(), Qt::Key_U);
      QCOMPARE(fixture.text(), source);
      QTest::keyClick(fixture.edit(), Qt::Key_R, Qt::ControlModifier);
      QCOMPARE(fixture.text(), expected);

      // Repeats must capture only "new", not the generated unchecked task prefix.
      fixture.moveTo(above ? count - 1 : count);
      QTest::keyClick(fixture.edit(), Qt::Key_Period);
      QCOMPARE(fixture.text(), above ? inserted.repeated(count * 2) + source
                                     : origin + inserted.repeated(count * 2) + tail);
      QTest::keyClick(fixture.edit(), Qt::Key_U);
      QCOMPARE(fixture.text(), expected);
    }
  }
  // A cold provisional 10. shrinks to 3. while insert mode is still active.
  // The repeat start must follow that rewrite without capturing marker bytes.
  Fixture numbered(QStringLiteral("1. alpha\n9. beta\n10. gamma"), 1, 7,
                   makeViListSourceConfig(true));
  numbered.waitForFreshListAst();
  numbered.makeAstStale();
  QTest::keyClicks(numbered.edit(), QStringLiteral("3onew"));
  QCOMPARE(numbered.blockText(2), QStringLiteral("10. new"));
  QTRY_COMPARE_WITH_TIMEOUT(numbered.text(), QStringLiteral("1. alpha\n2. betax\n3. new\n4. gamma"),
                            5000);
  QTest::keyClick(numbered.edit(), Qt::Key_Escape);
  QTRY_COMPARE_WITH_TIMEOUT(numbered.text(),
                            QStringLiteral("1. alpha\n2. betax\n3. new\n4. new\n5. new\n6. gamma"),
                            5000);
}

void TestMarkdownEditor::testViListInsertReturn() {
  struct ReturnCase {
    QString m_source;
    int m_column;
    QString m_expected;
    int m_caret;
  };
  const ReturnCase cases[] = {
      {QStringLiteral("3) alpha"), 5, QStringLiteral("3) al\n4) pha"), 3},
      {QStringLiteral("- [x] alpha"), 8, QStringLiteral("- [x] al\n- [ ] pha"), 6},
      {QStringLiteral("> - [x] "), 8, QStringLiteral("> "), 2},
  };
  for (bool fresh : {false, true}) {
    for (const auto &item : cases) {
      Fixture fixture(item.m_source, 0, item.m_column, makeViListSourceConfig());
      if (fresh) {
        fixture.waitForFreshListAst();
      }
      QTest::keyClick(fixture.edit(), Qt::Key_I);
      fixture.pressReturn();
      QCOMPARE(fixture.text(), item.m_expected);
      QCOMPARE(fixture.edit()->textCursor().positionInBlock(), item.m_caret);
    }
  }
}

void TestMarkdownEditor::testListAutoNumberStructuralEdits() {
  {
    Fixture fixture(QStringLiteral("1. a\n2. b\n3. c"), 0, -1, makeListSourceConfig());
    fixture.waitForFreshListAst();
    fixture.pressReturn();
    QTest::keyClicks(fixture.edit(), QStringLiteral("x"));
    QCOMPARE(fixture.text(), QStringLiteral("1. a\n2. x\n2. b\n3. c"));
    QTest::qWait(200);
    QCOMPARE(fixture.text(), QStringLiteral("1. a\n2. x\n2. b\n3. c"));
    QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), QStringLiteral("1. a\n2. x\n3. b\n4. c"), 5000);
    QCOMPARE(fixture.edit()->textCursor().blockNumber(), 1);
    QCOMPARE(fixture.edit()->textCursor().positionInBlock(), 4);
  }
  for (bool fresh : {false, true}) {
    // The cold case deletes the first marker before the initial highlight or
    // asynchronously seeded baseline can be delivered.
    Fixture fixture(QStringLiteral("1. a\n2. b\n3. c"), 0, -1, makeListSourceConfig());
    if (fresh) {
      fixture.waitForFreshListAst();
    }
    fixture.select(0, 0, 1, 0);
    QTest::keyClick(fixture.edit(), Qt::Key_Backspace);
    QCOMPARE(fixture.text(), QStringLiteral("2. b\n3. c"));
    QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), QStringLiteral("1. b\n2. c"), 5000);
    fixture.edit()->undo();
    QCOMPARE(fixture.text(), QStringLiteral("1. a\n2. b\n3. c"));
  }
  {
    Fixture fixture(QStringLiteral("1. a\n8. b\n9. c"), 2, -1, makeListSourceConfig());
    fixture.waitForFreshListAst();
    fixture.select(1, 4, 2, 4);
    QTest::keyClick(fixture.edit(), Qt::Key_Delete);
    QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), QStringLiteral("1. a\n2. b"), 5000);
  }
  {
    Fixture fixture(QStringLiteral("1. a\n2. b\n3. c"), 0, -1, makeListSourceConfig());
    fixture.waitForFreshListAst();
    replaceTableSource(*fixture.editor(), 0, 0, 1, QStringLiteral("5"));
    QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), QStringLiteral("5. a\n6. b\n7. c"), 5000);
  }
  {
    Fixture fixture(QStringLiteral("1. a\n2. b\n3. c"), 0, -1, makeListSourceConfig());
    fixture.waitForFreshListAst();
    QTextCursor cursor(fixture.editor()->document());
    cursor.beginEditBlock();
    cursor.setPosition(5, QTextCursor::KeepAnchor);
    cursor.removeSelectedText();
    cursor.setPosition(0);
    cursor.setPosition(1, QTextCursor::KeepAnchor);
    cursor.insertText(QStringLiteral("5"));
    cursor.endEditBlock();
    // Explicitly changing the first survivor wins over the deleted old start.
    QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), QStringLiteral("5. b\n6. c"), 5000);
  }
  {
    ClipboardRestore clipboard;
    const QString source = QStringLiteral("1. a\n8. moved\n9. c\n\nseparator\n\n"
                                          "5. d\n9. e\n\nuntouched\n\n7. keep\n3. odd");
    Fixture fixture(source, 0, -1, makeListSourceConfig());
    fixture.waitForFreshListAst();
    fixture.select(1, 0, 2, 0);
    fixture.edit()->cut();
    QCOMPARE(fixture.text(), QStringLiteral("1. a\n9. c\n\nseparator\n\n"
                                            "5. d\n9. e\n\nuntouched\n\n7. keep\n3. odd"));
    fixture.select(6, 0, 6, 0);
    fixture.edit()->paste();
    QTRY_COMPARE_WITH_TIMEOUT(fixture.text(),
                              QStringLiteral("1. a\n2. c\n\nseparator\n\n5. d\n6. moved\n7. e\n\n"
                                             "untouched\n\n7. keep\n3. odd"),
                              5000);
  }
  {
    const QString source = QStringLiteral("1. a\n8. b\n\nmiddle\n\n7. keep\n3. odd\n\n"
                                          "last\n\n5. c\n9. d");
    Fixture fixture(source, 5, -1, makeListSourceConfig());
    fixture.waitForFreshListAst();
    auto doc = fixture.editor()->document();
    QTextCursor cursor(doc);
    cursor.beginEditBlock();
    cursor.setPosition(tableSourcePosition(doc, 11, 0));
    cursor.setPosition(cursor.position() + 1, QTextCursor::KeepAnchor);
    cursor.insertText(QStringLiteral("3"));
    cursor.setPosition(tableSourcePosition(doc, 1, 0));
    cursor.setPosition(cursor.position() + 1, QTextCursor::KeepAnchor);
    cursor.insertText(QStringLiteral("4"));
    cursor.endEditBlock();
    QTRY_COMPARE_WITH_TIMEOUT(fixture.text(),
                              QStringLiteral("1. a\n2. b\n\nmiddle\n\n7. keep\n3. odd\n\n"
                                             "last\n\n5. c\n6. d"),
                              5000);
    QCOMPARE(fixture.edit()->textCursor().blockNumber(), 5);
    fixture.edit()->undo();
    QCOMPARE(fixture.text(), source);
  }

  struct MembershipCase {
    QString m_source;
    int m_block;
    int m_column;
    int m_length;
    QString m_insert;
    QString m_expected;
  };
  const MembershipCase membership[] = {
      // Removing the separator merges two old lists; the earlier start wins.
      {QStringLiteral("3. a\n8. b\n\nseparator\n\n7. c\n2. d"), 1, 4, 13, QStringLiteral("\n"),
       QStringLiteral("3. a\n4. b\n5. c\n6. d")},
      // The earliest surviving fragment keeps its start; later fragments restart.
      {QStringLiteral("3. a\n8. b\n9. c\n2. d"), 2, 0, 0, QStringLiteral("\nseparator\n\n"),
       QStringLiteral("3. a\n4. b\n\nseparator\n\n1. c\n2. d")},
      // A newly authored first marker outranks the old list's start.
      {QStringLiteral("7. a\n9. b"), 0, 0, 0, QStringLiteral("2. new\n"),
       QStringLiteral("2. new\n3. a\n4. b")},
      // Inserting an identical list at column zero must not steal old identities.
      {QStringLiteral("7. a\n3. b"), 0, 0, 0, QStringLiteral("7. a\n3. b\n\nbreak\n\n"),
       QStringLiteral("7. a\n8. b\n\nbreak\n\n7. a\n3. b")},
      // A child edit does not dirty the parent's unchanged direct sibling order.
      {QStringLiteral("5. parent\n\n   2. child\n   8. sibling\n9. other"), 3, 0, 0,
       QStringLiteral("   1. added\n"),
       QStringLiteral("5. parent\n\n   2. child\n   3. added\n   4. sibling\n9. other")},
  };
  for (const auto &item : membership) {
    Fixture fixture(item.m_source, 0, -1, makeListSourceConfig());
    fixture.waitForFreshListAst();
    replaceTableSource(*fixture.editor(), item.m_block, item.m_column, item.m_length,
                       item.m_insert);
    QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), item.m_expected, 5000);
  }
  {
    Fixture fixture(QStringLiteral("7. a\n3. b"), 0, -1, makeListSourceConfig());
    fixture.waitForFreshListAst();
    QTextCursor cursor(fixture.editor()->document());
    cursor.beginEditBlock();
    cursor.insertText(QStringLiteral("Intro.\n\n"));
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(QStringLiteral("\n\nAfterward."));
    cursor.endEditBlock();
    const auto edited = fixture.text();
    const int undoSteps = fixture.editor()->document()->availableUndoSteps();
    QTest::qWait(800);
    QCOMPARE(fixture.text(), QStringLiteral("Intro.\n\n7. a\n3. b\n\nAfterward."));
    QCOMPARE(fixture.editor()->document()->availableUndoSteps(), undoSteps);
    QCOMPARE(fixture.text(), edited);
  }
  for (bool enabled : {false, true}) {
    Fixture fixture(QStringLiteral("5. parent\n8. \n9. tail"), 1, -1,
                    makeListSourceConfig(enabled));
    fixture.waitForFreshListAst();
    QTest::keyClick(fixture.edit(), Qt::Key_Tab);
    QCOMPARE(fixture.blockText(1), QStringLiteral("    1. "));
    if (enabled) {
      QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), QStringLiteral("5. parent\n    1. \n6. tail"),
                                5000);
    }
    QTest::keyClick(fixture.edit(), Qt::Key_Backtab, Qt::ShiftModifier);
    if (enabled) {
      QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), QStringLiteral("5. parent\n6. \n7. tail"), 5000);
    } else {
      QCOMPARE(fixture.text(), QStringLiteral("5. parent\n6. \n9. tail"));
    }
  }
  {
    Fixture fixture(QStringLiteral("01. a\n1. b\n0001. c"), 1, -1, makeListSourceConfig());
    fixture.waitForFreshListAst();
    QTest::keyClicks(fixture.edit(), QStringLiteral("x"));
    QTest::qWait(800);
    QCOMPARE(fixture.text(), QStringLiteral("01. a\n1. bx\n0001. c"));
    replaceTableSource(*fixture.editor(), 1, 0, 0, QStringLiteral("1. added\n"));
    QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), QStringLiteral("1. a\n2. added\n3. bx\n4. c"), 5000);
  }
  {
    ClipboardRestore clipboard;
    Fixture fixture(QStringLiteral("intro\n\n"), 2, 0, makeListSourceConfig());
    fixture.waitForFreshListAst();
    QGuiApplication::clipboard()->setText(QStringLiteral("0) a\n9) b"));
    fixture.edit()->paste();
    QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), QStringLiteral("intro\n\n0) a\n1) b"), 5000);
  }
}

void TestMarkdownEditor::testListAutoNumberSplit() {
  const QString fenceList = QStringLiteral("1. Nested code block\n2. List item 2");
  const QString openFence = QStringLiteral("```cpp\n#include <iostream>\n");
  const QString fenced =
      QStringLiteral("1. Nested code block\n") + openFence + QStringLiteral("```\n2. List item 2");
  const QString fencedRestarted =
      QStringLiteral("1. Nested code block\n") + openFence + QStringLiteral("```\n1. List item 2");
  for (bool fresh : {false, true}) {
    // A parsed, still-open fence must not erase the later marker's identity.
    const QString ending = fresh ? QStringLiteral("\n") : QString();
    Fixture fixture(fenceList + ending, 0, -1, makeListSourceConfig());
    if (fresh) {
      fixture.waitForFreshListAst();
    }
    replaceTableSource(*fixture.editor(), 1, 0, 0, openFence);
    fixture.waitForFreshListAst();
    QTest::qWait(800); // Let the no-numbering idle pass run before closing the fence.
    QCOMPARE(fixture.text(), QStringLiteral("1. Nested code block\n") + openFence +
                                 QStringLiteral("2. List item 2") + ending);
    replaceTableSource(*fixture.editor(), 3, 0, 0, QStringLiteral("```\n"));
    QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), fencedRestarted + ending, 5000);
    verifyListStructurePreserved(fenced + ending, fixture.text(), {1, 1});
  }
  {
    // Other lists still normalize while the paused split retains its baseline.
    const QString prefix = QStringLiteral("3. before\n9. before tail\n\nbreak\n\n");
    const QString normalizedPrefix = QStringLiteral("3. before\n4. before tail\n\nbreak\n\n");
    Fixture fixture(prefix + fenceList, 0, -1, makeListSourceConfig());
    fixture.waitForFreshListAst();
    replaceTableSource(*fixture.editor(), 6, 0, 0, openFence);
    fixture.waitForFreshListAst();
    QTest::qWait(800);
    replaceTableSource(*fixture.editor(), 1, 0, 1, QStringLiteral("8"));
    const QString pending =
        QStringLiteral("1. Nested code block\n") + openFence + QStringLiteral("2. List item 2");
    QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), normalizedPrefix + pending, 5000);
    replaceTableSource(*fixture.editor(), 8, 0, 0, QStringLiteral("```\n"));
    QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), normalizedPrefix + fencedRestarted, 5000);
  }
  for (const auto &replacement : {QStringLiteral("7"), QStringLiteral("2.")}) {
    // Preserve an explicitly changed number or a wholly retyped marker, even
    // when it was edited while hidden inside the open fence.
    Fixture fixture(fenceList, 0, -1, makeListSourceConfig());
    fixture.waitForFreshListAst();
    replaceTableSource(*fixture.editor(), 1, 0, 0, openFence);
    fixture.waitForFreshListAst();
    QTest::qWait(800);
    replaceTableSource(*fixture.editor(), 3, 0, replacement.size(), replacement);
    fixture.waitForFreshListAst();
    QTest::qWait(800);
    replaceTableSource(*fixture.editor(), 3, 0, 0, QStringLiteral("```\n"));
    fixture.waitForFreshListAst();
    QTest::qWait(800);
    const QString marker =
        replacement == QStringLiteral("7") ? QStringLiteral("7.") : QStringLiteral("2.");
    QCOMPARE(fixture.text(), QStringLiteral("1. Nested code block\n") + openFence +
                                 QStringLiteral("```\n") + marker + QStringLiteral(" List item 2"));
  }

  const QString source = QStringLiteral("1. very simple questions\n2. Test the list;\n"
                                        "3. List item 2;\n4. very good\n5. hahahaha");
  const QString separated = QStringLiteral("1. very simple questions\n\nabcjdkejj\n\n"
                                           "2. Test the list;\n3. List item 2;\n"
                                           "4. very good\n5. hahahaha");
  const QString restarted = QStringLiteral("1. very simple questions\n\nabcjdkejj\n\n"
                                           "1. Test the list;\n2. List item 2;\n"
                                           "3. very good\n4. hahahaha");
  const QString independent = QStringLiteral("\n\nindependent\n\n7) keep\n3) authored");
  for (bool fresh : {false, true}) {
    Fixture fixture(source + independent, 0, -1, makeListSourceConfig());
    if (fresh) {
      fixture.waitForFreshListAst();
    }
    replaceTableSource(*fixture.editor(), 1, 0, 0, QStringLiteral("\nabcjdkejj\n\n"));
    QCOMPARE(fixture.text(), separated + independent);
    QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), restarted + independent, 5000);
    verifyListStructurePreserved(separated + independent, fixture.text(), {1, 1, 7});
    fixture.edit()->undo();
    QCOMPARE(fixture.text(), source + independent);
    fixture.waitForFreshListAst();
    QTest::qWait(800);
    QCOMPARE(fixture.text(), source + independent);
    QVERIFY(fixture.editor()->document()->isRedoAvailable());
    fixture.edit()->redo();
    QCOMPARE(fixture.text(), restarted + independent);
  }
  {
    // Already separate lists are authored input, not a structural split event.
    Fixture fixture(separated, 0, -1, makeListSourceConfig());
    const bool modified = fixture.editor()->document()->isModified();
    const int undoSteps = fixture.editor()->document()->availableUndoSteps();
    fixture.waitForFreshListAst();
    QTest::qWait(800);
    QCOMPARE(fixture.text(), separated);
    QCOMPARE(fixture.editor()->document()->isModified(), modified);
    QCOMPARE(fixture.editor()->document()->availableUndoSteps(), undoSteps);
  }
  {
    // A blank between nonempty items makes a loose list, not a second list.
    Fixture fixture(QStringLiteral("3. a\n8. b\n9. c"), 0, -1, makeListSourceConfig());
    fixture.waitForFreshListAst();
    replaceTableSource(*fixture.editor(), 1, 0, 0, QStringLiteral("\n"));
    QTest::qWait(800);
    QCOMPARE(fixture.text(), QStringLiteral("3. a\n\n8. b\n9. c"));
    replaceTableSource(*fixture.editor(), 1, 0, 0, QStringLiteral("\nbreak\n"));
    QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), QStringLiteral("3. a\n\nbreak\n\n1. b\n2. c"), 5000);
  }
  {
    // An explicit first-number edit in the split transaction takes precedence.
    Fixture fixture(QStringLiteral("3. a\n8. b\n9. c\n2. d"), 0, -1, makeListSourceConfig());
    fixture.waitForFreshListAst();
    QTextCursor cursor(fixture.editor()->document());
    const int start = tableSourcePosition(fixture.editor()->document(), 2, 0);
    cursor.beginEditBlock();
    cursor.setPosition(start);
    cursor.setPosition(start + 1, QTextCursor::KeepAnchor);
    cursor.insertText(QStringLiteral("5"));
    cursor.setPosition(start);
    cursor.insertText(QStringLiteral("\nbreak\n\n"));
    cursor.endEditBlock();
    QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), QStringLiteral("3. a\n4. b\n\nbreak\n\n5. c\n6. d"),
                              5000);
  }
  {
    // Both later fragments restart, while the original zero start survives.
    Fixture fixture(QStringLiteral("0. a\n8. b\n9. c\n2. d\n4. e\n5. f"), 0, -1,
                    makeListSourceConfig());
    fixture.waitForFreshListAst();
    QTextCursor cursor(fixture.editor()->document());
    cursor.beginEditBlock();
    cursor.setPosition(tableSourcePosition(fixture.editor()->document(), 4, 0));
    cursor.insertText(QStringLiteral("\nsecond break\n\n"));
    cursor.setPosition(tableSourcePosition(fixture.editor()->document(), 2, 0));
    cursor.insertText(QStringLiteral("\nfirst break\n\n"));
    cursor.endEditBlock();
    QTRY_COMPARE_WITH_TIMEOUT(fixture.text(),
                              QStringLiteral("0. a\n1. b\n\nfirst break\n\n1. c\n2. d\n\n"
                                             "second break\n\n1. e\n2. f"),
                              5000);
  }
  {
    // Restarting a two-digit fragment keeps its child under the same item.
    Fixture fixture(QStringLiteral("10. a\n11. b\n    - child\n12. c"), 0, -1,
                    makeListSourceConfig());
    fixture.waitForFreshListAst();
    replaceTableSource(*fixture.editor(), 1, 0, 0, QStringLiteral("\nbreak\n\n"));
    const QString authored = fixture.text();
    QTRY_COMPARE_WITH_TIMEOUT(fixture.text(),
                              QStringLiteral("10. a\n\nbreak\n\n1. b\n   - child\n2. c"), 5000);
    verifyListStructurePreserved(authored, fixture.text(), {10, 1});
  }
}

void TestMarkdownEditor::testListAutoNumberNestedWidths() {
  struct WidthCase {
    QString m_source;
    int m_block;
    int m_length;
    QString m_insert;
    QString m_expected;
    QVector<int> m_starts;
  };
  const WidthCase cases[] = {
      {QStringLiteral("9. a\n9. b\n   - child"),
       1,
       0,
       QStringLiteral("10. new\n"),
       QStringLiteral("9. a\n10. new\n11. b\n    - child"),
       {}},
      {QStringLiteral("9. gone\n10. b\n    - child\n11. c"),
       0,
       8,
       QString(),
       QStringLiteral("9. b\n   - child\n10. c"),
       {9}},
      // The old tab spans three consumed columns plus one child-relative column.
      {QStringLiteral("9. a\n9. b\n\t- child"),
       1,
       0,
       QStringLiteral("10. new\n"),
       QStringLiteral("9. a\n10. new\n11. b\n     - child"),
       {}},
      // Keep authored opening-line whitespace; padding is not just digit width.
      {QStringLiteral("9. a\n9.   b\n     - child"),
       1,
       0,
       QStringLiteral("10. new\n"),
       QStringLiteral("9. a\n10. new\n11.   b\n      - child"),
       {}},
      {QStringLiteral("9. a\n9.\tb\n    - child"),
       1,
       0,
       QStringLiteral("10. new\n"),
       QStringLiteral("9. a\n10. new\n11.\tb\n    - child"),
       {}},
      {QStringLiteral("[^n]: 9. a\n      9. b\n         - child\n\nref [^n]"),
       1,
       0,
       QStringLiteral("      10. new\n"),
       QStringLiteral("[^n]: 9. a\n      10. new\n      11. b\n          - child\n\nref [^n]"),
       {}},
      {QStringLiteral("- outer\n  > 9. a\n  > 9. b\n  >    - child"),
       2,
       0,
       QStringLiteral("  > 10. new\n"),
       QStringLiteral("- outer\n  > 9. a\n  > 10. new\n  > 11. b\n  >     - child"),
       {}},
      {QStringLiteral("9. a\n9. b\n   continuation\nlazy\n \t \n   ```cpp\n"
                      "   9. not a list\n   int n = 9;\n   ```\n\n   - child"),
       1,
       0,
       QStringLiteral("10. new\n"),
       QStringLiteral("9. a\n10. new\n11. b\n    continuation\nlazy\n \t \n    ```cpp\n"
                      "    9. not a list\n    int n = 9;\n    ```\n\n    - child"),
       {}},
      {QStringLiteral("9. a\n\n9. b\n\n   continuation"),
       2,
       0,
       QStringLiteral("10. new\n\n"),
       QStringLiteral("9. a\n\n10. new\n\n11. b\n\n    continuation"),
       {}},
      {QStringLiteral("0) a\n9) b"),
       1,
       0,
       QStringLiteral("8) new\n"),
       QStringLiteral("0) a\n1) new\n2) b"),
       {}},
      {QStringLiteral("999999998. a\n1. b"),
       1,
       1,
       QStringLiteral("2"),
       QStringLiteral("999999998. a\n999999999. b"),
       {}},
  };
  for (const auto &item : cases) {
    Fixture fixture(item.m_source, 0, -1, makeListSourceConfig());
    fixture.waitForFreshListAst();
    replaceTableSource(*fixture.editor(), item.m_block, 0, item.m_length, item.m_insert);
    const QString authored = fixture.text();
    QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), item.m_expected, 5000);
    verifyListStructurePreserved(authored, fixture.text(), item.m_starts);
    fixture.edit()->undo();
    QCOMPARE(fixture.text(), item.m_source);
  }
  {
    const QString source = QStringLiteral("9. 9. a\n   9. b\n      - child\n9. tail");
    Fixture fixture(source, 0, -1, makeListSourceConfig());
    fixture.waitForFreshListAst();
    auto doc = fixture.editor()->document();
    QTextCursor cursor(doc);
    cursor.beginEditBlock();
    cursor.setPosition(tableSourcePosition(doc, 3, 0));
    cursor.insertText(QStringLiteral("10. outer-new\n"));
    cursor.setPosition(tableSourcePosition(doc, 1, 0));
    cursor.insertText(QStringLiteral("   10. inner-new\n"));
    cursor.endEditBlock();
    const QString authored = fixture.text();
    QTRY_COMPARE_WITH_TIMEOUT(fixture.text(),
                              QStringLiteral("9. 9. a\n   10. inner-new\n   11. b\n       - child\n"
                                             "10. outer-new\n11. tail"),
                              5000);
    verifyListStructurePreserved(authored, fixture.text());
  }
  {
    Fixture fixture(QStringLiteral("3. a\n8. b\n2. c\n4. d"), 0, -1, makeListSourceConfig());
    fixture.waitForFreshListAst();
    QTextCursor cursor(fixture.editor()->document());
    cursor.beginEditBlock();
    cursor.setPosition(tableSourcePosition(fixture.editor()->document(), 2, 1));
    cursor.setPosition(cursor.position() + 1, QTextCursor::KeepAnchor);
    cursor.insertText(QStringLiteral(")"));
    cursor.setPosition(tableSourcePosition(fixture.editor()->document(), 1, 1));
    cursor.setPosition(cursor.position() + 1, QTextCursor::KeepAnchor);
    cursor.insertText(QStringLiteral(")"));
    cursor.endEditBlock();
    const QString authored = fixture.text();
    QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), QStringLiteral("3. a\n1) b\n2) c\n1. d"), 5000);
    verifyListStructurePreserved(authored, fixture.text(), {3, 1, 1});
  }
  {
    Fixture fixture(QStringLiteral("5. keep\n2. odd\n\nplain\n\n"), 5, 0, makeListSourceConfig());
    fixture.waitForFreshListAst();
    QTextCursor cursor(fixture.editor()->document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(QStringLiteral("> 000000000) a\n> 9) b"));
    const QString authored = fixture.text();
    QTRY_COMPARE_WITH_TIMEOUT(fixture.text(),
                              QStringLiteral("5. keep\n2. odd\n\nplain\n\n> 0) a\n> 1) b"), 5000);
    verifyListStructurePreserved(authored, fixture.text());
  }
  {
    // Overflow rejects its whole connected list, but not an independent valid one.
    Fixture fixture(QStringLiteral("999999999. a\n1. b\n   - child\n\nbreak\n\n3. c\n9. d"), 0, -1,
                    makeListSourceConfig());
    fixture.waitForFreshListAst();
    auto doc = fixture.editor()->document();
    QTextCursor cursor(doc);
    cursor.beginEditBlock();
    cursor.setPosition(tableSourcePosition(doc, 7, 0));
    cursor.setPosition(cursor.position() + 1, QTextCursor::KeepAnchor);
    cursor.insertText(QStringLiteral("8"));
    cursor.setPosition(tableSourcePosition(doc, 1, 0));
    cursor.setPosition(cursor.position() + 1, QTextCursor::KeepAnchor);
    cursor.insertText(QStringLiteral("2"));
    cursor.endEditBlock();
    const QString authored = fixture.text();
    const QString expected =
        QStringLiteral("999999999. a\n2. b\n   - child\n\nbreak\n\n3. c\n4. d");
    QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), expected, 5000);
    verifyListStructurePreserved(authored, fixture.text());
    const int undoSteps = doc->availableUndoSteps();
    QTest::qWait(800);
    QCOMPARE(fixture.text(), expected);
    QCOMPARE(doc->availableUndoSteps(), undoSteps);
  }
}

void TestMarkdownEditor::testListAutoNumberConfigAndReplay() {
  const QString source = QStringLiteral("1. a\n8. b\n9. c");
  const QString edited = QStringLiteral("1. a\n7. b\n9. c");
  const QString numbered = QStringLiteral("1. a\n2. b\n3. c");
  for (int load = 0; load < 3; ++load) {
    Fixture fixture(QStringLiteral("previous document"), 0, -1, makeListSourceConfig());
    if (load == 0) {
      fixture.editor()->setText(source);
    } else if (load == 1) {
      fixture.edit()->setPlainText(source);
    } else {
      fixture.editor()->document()->clear();
      fixture.editor()->setText(source);
    }
    auto doc = fixture.editor()->document();
    const bool modified = doc->isModified();
    const int undoSteps = doc->availableUndoSteps();
    const int redoSteps = doc->availableRedoSteps();
    fixture.waitForFreshListAst();
    QTest::qWait(800);
    QCOMPARE(fixture.text(), source);
    QCOMPARE(doc->isModified(), modified);
    QCOMPARE(doc->availableUndoSteps(), undoSteps);
    QCOMPARE(doc->availableRedoSteps(), redoSteps);
  }
  {
    auto config = makeListSourceConfig(false);
    Fixture fixture(source, 0, -1, config);
    fixture.waitForFreshListAst();
    fixture.pressReturn();
    QTest::keyClicks(fixture.edit(), QStringLiteral("x"));
    QTest::qWait(800);
    QCOMPARE(fixture.text(), QStringLiteral("1. a\n2. x\n8. b\n9. c"));
    // Automatic enablement never gates an explicit toolbar conversion.
    fixture.editor()->setText(QStringLiteral("a\nb"));
    fixture.selectAll();
    MarkdownUtils::typeOrderedList(fixture.edit());
    QCOMPARE(fixture.text(), QStringLiteral("1. a\n2. b"));

    fixture.editor()->setText(source);
    fixture.waitForFreshListAst();
    auto doc = fixture.editor()->document();
    const bool modified = doc->isModified();
    config->m_autoNumberOrderedListsEnabled = true;
    fixture.editor()->setConfig(config);
    // Config application can itself add Qt format commands, independently of
    // automatic source numbering. No later idle work may add another command.
    const int undoSteps = doc->availableUndoSteps();
    fixture.waitForFreshListAst();
    QTest::qWait(800);
    QCOMPARE(fixture.text(), source);
    QCOMPARE(doc->isModified(), modified);
    QCOMPARE(doc->availableUndoSteps(), undoSteps);

    replaceTableSource(*fixture.editor(), 1, 0, 1, QStringLiteral("7"));
    QTest::qWait(200);
    config->m_autoNumberOrderedListsEnabled = false;
    fixture.editor()->setConfig(config);
    QTest::qWait(800);
    QCOMPARE(fixture.text(), edited);
    config->m_autoNumberOrderedListsEnabled = true;
    fixture.editor()->setConfig(config);
    fixture.waitForFreshListAst();
    const int enabledUndoSteps = doc->availableUndoSteps();
    QTest::qWait(800);
    QCOMPARE(fixture.text(), edited);
    QCOMPARE(doc->availableUndoSteps(), enabledUndoSteps);

    replaceTableSource(*fixture.editor(), 2, 0, 1, QStringLiteral("6"));
    QTest::qWait(200);
    fixture.editor()->setConfig(config); // Same values must keep the pending edit.
    QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), numbered, 5000);
    fixture.waitForFreshListAst();
    const auto cursor = fixture.edit()->textCursor();
    fixture.editor()->getHighlighter()->rehighlight();
    fixture.editor()->setConfig(config);
    const int settledUndoSteps = doc->availableUndoSteps();
    const int settledRedoSteps = doc->availableRedoSteps();
    QTest::qWait(800);
    QCOMPARE(fixture.text(), numbered);
    QCOMPARE(fixture.edit()->textCursor().position(), cursor.position());
    QCOMPARE(fixture.edit()->textCursor().anchor(), cursor.anchor());
    QCOMPARE(doc->availableUndoSteps(), settledUndoSteps);
    QCOMPARE(doc->availableRedoSteps(), settledRedoSteps);
  }
  for (bool programmatic : {false, true}) {
    Fixture fixture(source, 1, -1, makeListSourceConfig());
    fixture.waitForFreshListAst();
    auto doc = fixture.editor()->document();
    replaceTableSource(*fixture.editor(), 1, 0, 1, QStringLiteral("7"));
    QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), numbered, 5000);
    if (programmatic) {
      doc->undo();
    } else {
      fixture.edit()->undo();
    }
    QCOMPARE(fixture.text(), source);
    QVERIFY(doc->isRedoAvailable());
    const int redoSteps = doc->availableRedoSteps();
    QTest::qWait(800);
    QCOMPARE(fixture.text(), source);
    QCOMPARE(doc->availableRedoSteps(), redoSteps);
    if (programmatic) {
      doc->redo();
    } else {
      fixture.edit()->redo();
    }
    QCOMPARE(fixture.text(), numbered);
    const int undoSteps = doc->availableUndoSteps();
    QTest::qWait(800);
    QCOMPARE(fixture.text(), numbered);
    QCOMPARE(doc->availableUndoSteps(), undoSteps);

    fixture.editor()->setText(source);
    fixture.waitForFreshListAst();
    replaceTableSource(*fixture.editor(), 1, 0, 1, QStringLiteral("7"));
    if (programmatic) {
      doc->undo();
    } else {
      fixture.edit()->undo();
    }
    QCOMPARE(fixture.text(), source);
    const int pendingRedoSteps = doc->availableRedoSteps();
    QVERIFY(doc->isRedoAvailable());
    QTest::qWait(800);
    QCOMPARE(fixture.text(), source);
    QCOMPARE(doc->availableRedoSteps(), pendingRedoSteps);
    if (programmatic) {
      doc->redo();
    } else {
      fixture.edit()->redo();
    }
    QCOMPARE(fixture.text(), edited); // Replay itself must not normalize.
    QTest::qWait(800);
    QCOMPARE(fixture.text(), edited);
    replaceTableSource(*fixture.editor(), 2, 0, 1, QStringLiteral("6"));
    QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), numbered, 5000);
    if (programmatic) {
      doc->undo();
    } else {
      fixture.edit()->undo();
    }
    QCOMPARE(fixture.text(), edited);
    replaceTableSource(*fixture.editor(), 1, 0, 1, QStringLiteral("4"));
    QVERIFY(!doc->isRedoAvailable());
    QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), numbered, 5000);
  }
  {
    Fixture fixture(source, 1, -1, makeListSourceConfig());
    fixture.waitForFreshListAst();
    auto doc = fixture.editor()->document();
    QTextCursor cursor(doc);
    cursor.setPosition(tableSourcePosition(doc, 1, 0));
    cursor.insertText(QStringLiteral("4. temporary\n"));
    cursor.setPosition(tableSourcePosition(doc, 1, 0), QTextCursor::KeepAnchor);
    cursor.removeSelectedText();
    QCOMPARE(fixture.text(), source);
    fixture.waitForFreshListAst();
    const int undoSteps = doc->availableUndoSteps();
    const int redoSteps = doc->availableRedoSteps();
    QTest::qWait(800);
    QCOMPARE(fixture.text(), source);
    QCOMPARE(doc->availableUndoSteps(), undoSteps);
    QCOMPARE(doc->availableRedoSteps(), redoSteps);
  }
  {
    // A bare cursor edit may have its own formatting undo step, but none is lost.
    Fixture fixture(source, 1, -1, makeListSourceConfig());
    fixture.waitForFreshListAst();
    auto doc = fixture.editor()->document();
    QTextCursor cursor(doc);
    cursor.setPosition(tableSourcePosition(doc, 1, 0));
    cursor.insertText(QStringLiteral("4. new\n"));
    const QString authored = fixture.text();
    const QString expected = QStringLiteral("1. a\n2. new\n3. b\n4. c");
    QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), expected, 5000);
    doc->undo();
    QVERIFY(fixture.text() == source || fixture.text() == authored);
    if (fixture.text() == authored) {
      doc->undo();
    }
    QCOMPARE(fixture.text(), source);
    QTest::qWait(800);
    QCOMPARE(fixture.text(), source);
    doc->redo();
    if (fixture.text() == authored) {
      doc->redo();
    }
    QCOMPARE(fixture.text(), expected);
  }
  for (bool undoEnabled : {false, true}) {
    Fixture fixture(source, 0, -1, makeListSourceConfig());
    fixture.waitForFreshListAst();
    auto doc = fixture.editor()->document();
    doc->setUndoRedoEnabled(undoEnabled);
    doc->clear();
    if (undoEnabled) {
      QTest::qWait(800); // Also cover a clear with no paired insertion at all.
      QCOMPARE(fixture.text(), QString());
    }
    QTextCursor cursor(doc);
    cursor.insertText(QStringLiteral("0. a\n8. b"));
    QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), QStringLiteral("0. a\n1. b"), 5000);
    QCOMPARE(doc->isUndoRedoEnabled(), undoEnabled);
    if (!undoEnabled) {
      QVERIFY(!doc->isUndoAvailable());
      QVERIFY(!doc->isRedoAvailable());
    }
  }
  {
    Fixture fixture(QStringLiteral("replace all visible text\n"), 0, -1, makeListSourceConfig());
    fixture.waitForFreshListAst();
    QTextCursor cursor(fixture.editor()->document());
    cursor.select(QTextCursor::Document);
    cursor.insertText(QStringLiteral("5. a\n1. b"));
    QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), QStringLiteral("5. a\n6. b"), 5000);
  }
}

void TestMarkdownEditor::testListAutoNumberProtectedPositions() {
  const QString source = QStringLiteral("9. a\n9. \u4E2De\u0301\U0001F600 tail\n   continuation");
  const QString expected =
      QStringLiteral("9. a\n10. new\n11. \u4E2De\u0301\U0001F600 tail\n    continuation");
  struct PositionCase {
    int m_anchorBlock;
    int m_anchorColumn;
    int m_positionBlock;
    int m_positionColumn;
    int m_mappedAnchor;
    int m_mappedPosition;
    QString m_selected;
  };
  const PositionCase positions[] = {
      {2, 8, 2, 8, 9, 9, QString()}, // Caret after the surrogate pair.
      {2, 4, 2, 8, 5, 9, QStringLiteral("e\u0301\U0001F600")},
      {2, 8, 2, 4, 9, 5, QStringLiteral("e\u0301\U0001F600")},
      {1, 7, 2, 8, 7, 9, QStringLiteral("\u202911. \u4E2De\u0301\U0001F600")},
      {2, 8, 1, 7, 9, 7, QStringLiteral("\u202911. \u4E2De\u0301\U0001F600")},
  };
  for (const auto &item : positions) {
    Fixture fixture(source, 0, -1, makeListSourceConfig());
    fixture.waitForFreshListAst();
    replaceTableSource(*fixture.editor(), 1, 0, 0, QStringLiteral("10. new\n"));
    selectTableSource(*fixture.editor(), item.m_anchorBlock, item.m_anchorColumn,
                      item.m_positionBlock, item.m_positionColumn);
    QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), expected, 5000);
    const auto cursor = fixture.edit()->textCursor();
    auto doc = fixture.editor()->document();
    QCOMPARE(cursor.anchor(), tableSourcePosition(doc, item.m_anchorBlock, item.m_mappedAnchor));
    QCOMPARE(cursor.position(),
             tableSourcePosition(doc, item.m_positionBlock, item.m_mappedPosition));
    QCOMPARE(cursor.selectedText(), item.m_selected);
  }
  {
    Fixture fixture(source, 0, -1, makeListSourceConfig());
    fixture.waitForFreshListAst();
    replaceTableSource(*fixture.editor(), 1, 0, 0, QStringLiteral("10. new\n"));
    auto doc = fixture.editor()->document();
    selectTableSource(*fixture.editor(), 0, 4, 0, 4);
    fixture.edit()->setOverriddenSelection(tableSourcePosition(doc, 2, 4),
                                           tableSourcePosition(doc, 2, 8));
    QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), expected, 5000);
    QCOMPARE(fixture.edit()->getSelection().start(), tableSourcePosition(doc, 2, 5));
    QCOMPARE(fixture.edit()->getSelection().end(), tableSourcePosition(doc, 2, 9));
    QCOMPARE(fixture.edit()->selectedText(), QStringLiteral("e\u0301\U0001F600"));
    QCOMPARE(fixture.edit()->textCursor().position(), 4);
    QVERIFY(!fixture.edit()->textCursor().hasSelection());
  }
  {
    Fixture fixture(QStringLiteral("9. gone\n10. body\n    continuation"), 0, -1,
                    makeListSourceConfig());
    fixture.waitForFreshListAst();
    replaceTableSource(*fixture.editor(), 0, 0, 8, QString());
    selectTableSource(*fixture.editor(), 0, 1, 0, 1);
    QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), QStringLiteral("9. body\n   continuation"), 5000);
    QCOMPARE(fixture.edit()->textCursor().positionInBlock(), 1);
  }
  {
    const QString suffix =
        QStringLiteral("\n\n") + QStringLiteral("ordinary prose\n").repeated(200);
    Fixture fixture(source + suffix, 1, 8, makeListSourceConfig());
    fixture.editor()->resize(640, 480);
    fixture.editor()->show();
    fixture.editor()->activateWindow();
    fixture.edit()->setFocus();
    QTRY_VERIFY_WITH_TIMEOUT(fixture.edit()->hasFocus(), 5000);
    fixture.waitForFreshListAst();
    auto scroll = fixture.edit()->verticalScrollBar();
    QTRY_VERIFY_WITH_TIMEOUT(scroll->maximum() > 20, 5000);
    replaceTableSource(*fixture.editor(), 1, 0, 1, QStringLiteral("8"));
    fixture.waitForFreshListAst();
    QTest::qWait(100);
    scroll->setValue(scroll->maximum() / 2);
    const int savedScroll = scroll->value();
    const QString numbered =
        QStringLiteral("9. a\n10. \u4E2De\u0301\U0001F600 tail\n    continuation") + suffix;
    QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), numbered, 5000);
    QCOMPARE(scroll->value(), savedScroll);
    QCOMPARE(fixture.edit()->textCursor().positionInBlock(), 9);
  }
  {
    Fixture fixture(QStringLiteral("1. a\n8. b\n9. c"), 1, -1, makeListSourceConfig());
    fixture.editor()->resize(640, 480);
    fixture.editor()->show();
    fixture.editor()->activateWindow();
    auto edit = fixture.edit();
    edit->setFocus();
    QTRY_VERIFY_WITH_TIMEOUT(edit->hasFocus(), 5000);
    fixture.waitForFreshListAst();
    replaceTableSource(*fixture.editor(), 1, 0, 1, QStringLiteral("7"));
    fixture.moveTo(1);
    QInputMethodEvent preedit(QStringLiteral("\u3042"), QList<QInputMethodEvent::Attribute>());
    QCoreApplication::sendEvent(edit, &preedit);
    const auto body = fixture.editor()->document()->findBlockByNumber(1);
    QVERIFY(body.layout());
    QCOMPARE(body.layout()->preeditAreaText(), QStringLiteral("\u3042"));
    const int position = edit->textCursor().position();
    QTest::qWait(800);
    QCOMPARE(fixture.text(), QStringLiteral("1. a\n7. b\n9. c"));
    QCOMPARE(body.layout()->preeditAreaText(), QStringLiteral("\u3042"));
    QCOMPARE(edit->textCursor().position(), position);
    QVERIFY(edit->hasFocus());

    QInputMethodEvent commit;
    commit.setCommitString(QStringLiteral("\u3042"));
    QCoreApplication::sendEvent(edit, &commit);
    QCOMPARE(fixture.text(), QStringLiteral("1. a\n7. b\u3042\n9. c"));
    QTest::qWait(200);
    QCOMPARE(fixture.text(), QStringLiteral("1. a\n7. b\u3042\n9. c"));
    QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), QStringLiteral("1. a\n2. b\u3042\n3. c"), 5000);
    QCOMPARE(body.layout()->preeditAreaText(), QString());
    QCOMPARE(edit->textCursor().positionInBlock(), 5);
  }
  {
    Fixture fixture(QStringLiteral("1. a\n8. b"), 1, -1, makeListSourceConfig());
    fixture.waitForFreshListAst();
    replaceTableSource(*fixture.editor(), 1, 0, 1, QStringLiteral("7"));
    fixture.edit()->setReadOnly(true);
    QTest::qWait(800);
    QCOMPARE(fixture.text(), QStringLiteral("1. a\n7. b"));
    fixture.edit()->setReadOnly(false);
    fixture.moveTo(0);
    QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), QStringLiteral("1. a\n2. b"), 5000);
  }
}

void TestMarkdownEditor::testListAndTableSourceFormatting() {
  const QString source = QStringLiteral("9. a\n9. b\n\n   | h1 | header2 |\n"
                                        "   | --- | --- |\n   | a | b |\n");
  const QString aligned = QStringLiteral("9. a\n10. new\n11. b\n\n    | h1  | header2 |\n"
                                         "    | --- | ------- |\n    | z   | b       |\n");
  for (int disabledMode : {0, 1, 2}) {
    auto config = makeListSourceConfig(true, true);
    Fixture fixture(source, 1, -1, config);
    fixture.waitForFreshListAst();
    auto doc = fixture.editor()->document();
    QTextCursor cursor(doc);
    cursor.beginEditBlock();
    cursor.setPosition(tableSourcePosition(doc, 5, 5));
    cursor.setPosition(cursor.position() + 1, QTextCursor::KeepAnchor);
    cursor.insertText(QStringLiteral("z"));
    cursor.setPosition(tableSourcePosition(doc, 1, 0));
    cursor.insertText(QStringLiteral("10. new\n"));
    cursor.endEditBlock();
    const QString authored = fixture.text();
    fixture.moveTo(1);
    if (disabledMode != 0) {
      QTest::qWait(200);
      if (disabledMode == 1) {
        config->m_autoFormatTableSourceEnabled = false;
      } else {
        config->m_autoNumberOrderedListsEnabled = false;
      }
      fixture.editor()->setConfig(config);
    }
    QString expected = aligned;
    if (disabledMode == 1) {
      expected = QStringLiteral("9. a\n10. new\n11. b\n\n    | h1 | header2 |\n"
                                "    | --- | --- |\n    | z | b |\n");
    } else if (disabledMode == 2) {
      expected = QStringLiteral("9. a\n10. new\n9. b\n\n   | h1  | header2 |\n"
                                "   | --- | ------- |\n   | z   | b       |\n");
    }
    QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), expected, 5000);
    fixture.waitForFreshListAst();
    // Exact source, including the four-column container, keeps this a table
    // inside b rather than accidentally turning it into a top-level/code block.
    // Alignment intentionally rewrites delimiter-cell source in the XML tree.
    verifyListStructurePreserved(authored, fixture.text(), {}, false);
    const int undoSteps = doc->availableUndoSteps();
    const int redoSteps = doc->availableRedoSteps();
    const auto savedCursor = fixture.edit()->textCursor();
    QTest::qWait(800);
    QCOMPARE(fixture.text(), expected);
    QCOMPARE(doc->availableUndoSteps(), undoSteps);
    QCOMPARE(doc->availableRedoSteps(), redoSteps);
    QCOMPARE(fixture.edit()->textCursor().position(), savedCursor.position());
    QCOMPARE(fixture.edit()->textCursor().anchor(), savedCursor.anchor());
  }
  {
    // Disjoint plans share one undo transaction rather than competing schedulers.
    const QString prefix = QStringLiteral("1. a\n8. b\n\nseparate\n\n");
    Fixture fixture(prefix + c_tableSource, 0, -1, makeListSourceConfig(true, true));
    fixture.waitForFreshListAst();
    auto doc = fixture.editor()->document();
    QTextCursor cursor(doc);
    cursor.beginEditBlock();
    cursor.setPosition(tableSourcePosition(doc, 7, 2));
    cursor.setPosition(cursor.position() + 1, QTextCursor::KeepAnchor);
    cursor.insertText(QStringLiteral("z"));
    cursor.setPosition(tableSourcePosition(doc, 1, 0));
    cursor.setPosition(cursor.position() + 1, QTextCursor::KeepAnchor);
    cursor.insertText(QStringLiteral("7"));
    cursor.endEditBlock();
    const QString expected = QStringLiteral("1. a\n2. b\n\nseparate\n\n") + c_tableSourceAligned;
    QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), expected, 5000);
    fixture.edit()->undo();
    QCOMPARE(fixture.text(), prefix + c_tableSource);
    fixture.edit()->redo();
    QCOMPARE(fixture.text(), expected);
    QTest::qWait(800);
    QCOMPARE(fixture.text(), expected);
  }
}

void TestMarkdownEditor::testListAutoNumberStaleWorker() {
  QString source;
  for (int i = 0; i < 1500; ++i) {
    source += QStringLiteral("1. item%1\n").arg(i);
  }
  source.chop(1);
  const QString inserted = QStringLiteral("1. new\n");
  QString withInsertion = QStringLiteral("1. item0\n2. new\n");
  for (int i = 1; i < 1500; ++i) {
    withInsertion += QStringLiteral("%1. item%2\n").arg(i + 2).arg(i);
  }
  withInsertion.chop(1);

  // Schedule a competing consumer operation around the idle boundary. The
  // contract is the final source, not whether a particular machine has already
  // started/finished the worker when this event is delivered.
  for (int action : {0, 1, 2}) {
    auto config = makeListSourceConfig();
    Fixture fixture(source, 0, -1, config);
    fixture.waitForFreshListAst();
    auto doc = fixture.editor()->document();
    replaceTableSource(*fixture.editor(), 1, 0, 0, inserted);
    bool delivered = false;
    QString allowedAtDisable;
    bool loadedUndoAvailable = false;
    const QString replacement = QStringLiteral("7. replacement\n2. stays authored");
    QTimer::singleShot(500, fixture.editor(), [&]() {
      if (action == 0) {
        replaceTableSource(*fixture.editor(), 1500, 0, doc->findBlockByNumber(1500).text().size(),
                           QStringLiteral("7. item1499 changed"));
      } else if (action == 1) {
        fixture.editor()->setText(replacement);
        loadedUndoAvailable = doc->isUndoAvailable();
      } else {
        config->m_autoNumberOrderedListsEnabled = false;
        fixture.editor()->setConfig(config);
        allowedAtDisable = fixture.text();
      }
      delivered = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(delivered, 5000);
    if (action == 0) {
      QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), withInsertion + QStringLiteral(" changed"), 5000);
      // The final authored edit and any formatter step remain replayable.
      const QString settled = fixture.text();
      doc->undo();
      const QString undone = fixture.text();
      QVERIFY(undone != settled);
      QVERIFY(doc->isRedoAvailable());
      const int redoSteps = doc->availableRedoSteps();
      QTest::qWait(800);
      QCOMPARE(fixture.text(), undone);
      QCOMPARE(doc->availableRedoSteps(), redoSteps);
      doc->redo();
      QCOMPARE(fixture.text(), settled);
    } else if (action == 1) {
      fixture.waitForFreshListAst();
      QTest::qWait(800);
      QCOMPARE(fixture.text(), replacement);
      QVERIFY(!doc->isModified());
      QCOMPARE(doc->isUndoAvailable(), loadedUndoAvailable);
      QVERIFY(!doc->isRedoAvailable());
      // The new epoch remains usable after an old completion has been discarded.
      replaceTableSource(*fixture.editor(), 1, 0, 1, QStringLiteral("4"));
      QTRY_COMPARE_WITH_TIMEOUT(fixture.text(), QStringLiteral("7. replacement\n8. stays authored"),
                                5000);
      fixture.edit()->undo();
      QCOMPARE(fixture.text(), replacement);
    } else {
      const int undoSteps = doc->availableUndoSteps();
      QTest::qWait(800);
      QCOMPARE(fixture.text(), allowedAtDisable);
      QCOMPARE(doc->availableUndoSteps(), undoSteps);
      // Config application may put Qt format commands above the source edit.
      int undos = 0;
      do {
        QVERIFY(doc->isUndoAvailable());
        doc->undo();
        ++undos;
      } while (fixture.text() == allowedAtDisable && undos < 8);
      QVERIFY(fixture.text() != allowedAtDisable);
      while (undos-- > 0) {
        QVERIFY(doc->isRedoAvailable());
        doc->redo();
      }
      QCOMPARE(fixture.text(), allowedAtDisable);
    }
  }
  {
    auto closing = std::make_unique<Fixture>(source, 0, -1, makeListSourceConfig());
    closing->waitForFreshListAst();
    replaceTableSource(*closing->editor(), 1, 0, 0, inserted);
    bool closed = false;
    QObject lifetime;
    QTimer::singleShot(500, &lifetime, [&]() {
      closing.reset();
      closed = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(closed, 5000);
    Fixture successor(QStringLiteral("4. a\n9. b"), 0, -1, makeListSourceConfig());
    successor.waitForFreshListAst();
    replaceTableSource(*successor.editor(), 1, 0, 1, QStringLiteral("7"));
    QTRY_COMPARE_WITH_TIMEOUT(successor.text(), QStringLiteral("4. a\n5. b"), 5000);
    successor.edit()->undo();
    QCOMPARE(successor.text(), QStringLiteral("4. a\n9. b"));
  }
}

namespace {
const QColor c_listGuideColor(QStringLiteral("#ed00a8"));
const QColor c_listActiveColor(QStringLiteral("#a4efd0"));
const QColor c_listSelectionColor(QStringLiteral("#315cb7"));
const QColor c_listCursorLineColor(QStringLiteral("#e7cc74"));
const QColor c_listSyntaxColor(QStringLiteral("#efb572"));

QSharedPointer<MarkdownEditorConfig>
makeListDecorationConfig(const QColor &p_guide = c_listGuideColor,
                         const QColor &p_active = c_listActiveColor,
                         const QColor &p_cursorLine = QColor(), int p_fontSize = 12) {
  QJsonObject styles;
  styles.insert(
      QStringLiteral("Text"),
      QJsonObject{{QStringLiteral("font-family"), QStringLiteral("Arial")},
                  {QStringLiteral("font-size"), p_fontSize},
                  {QStringLiteral("text-color"), QStringLiteral("#202020")},
                  {QStringLiteral("selected-text-color"), QStringLiteral("#ffffff")},
                  {QStringLiteral("selected-background-color"), c_listSelectionColor.name()},
                  {QStringLiteral("background-color"), QStringLiteral("#ffffff")}});
  styles.insert(QStringLiteral("SelectedText"),
                QJsonObject{{QStringLiteral("text-color"), QStringLiteral("#ffffff")},
                            {QStringLiteral("background-color"), c_listSelectionColor.name()}});
  if (p_cursorLine.isValid()) {
    styles.insert(QStringLiteral("CursorLine"),
                  QJsonObject{{QStringLiteral("background-color"), p_cursorLine.name()}});
  }
  QJsonObject markdownStyles;
  if (p_guide.isValid()) {
    markdownStyles.insert(QStringLiteral("ListItemGuide"),
                          QJsonObject{{QStringLiteral("text-color"), p_guide.name()}});
  }
  if (p_active.isValid()) {
    markdownStyles.insert(QStringLiteral("ActiveListItem"),
                          QJsonObject{{QStringLiteral("background-color"), p_active.name()}});
  }
  const QJsonObject json{
      {QStringLiteral("metadata"),
       QJsonObject{{QStringLiteral("type"), QStringLiteral("vtextedit")}}},
      {QStringLiteral("editor-styles"), styles},
      {QStringLiteral("markdown-editor-styles"), markdownStyles},
      {QStringLiteral("markdown-syntax-styles"),
       QJsonObject{{QStringLiteral("CODE"),
                    QJsonObject{{QStringLiteral("background-color"), c_listSyntaxColor.name()}}}}}};
  auto textConfig = QSharedPointer<TextEditorConfig>::create();
  textConfig->m_theme = Theme::createThemeFromContent(
      QString::fromUtf8(QJsonDocument(json).toJson(QJsonDocument::Compact)));
  Q_ASSERT(textConfig->m_theme);
  textConfig->m_inputMode = InputMode::NormalMode;
  textConfig->m_lineNumberType = VTextEditor::LineNumberType::None;
  auto config = QSharedPointer<MarkdownEditorConfig>::create(textConfig);
  config->m_inplacePreviewSources = MarkdownEditorConfig::NoInplacePreview;
  config->m_autoFoldPreviewedBlocksEnabled = false;
  config->m_autoNumberOrderedListsEnabled = false;
  config->m_autoFormatTableSourceEnabled = false;
  return config;
}

void showListDecorationFixture(Fixture &p_fixture, const QSize &p_size = QSize(640, 480)) {
  p_fixture.editor()->setSpellCheckEnabled(false);
  p_fixture.editor()->resize(p_size);
  p_fixture.editor()->show();
  QVERIFY(QTest::qWaitForWindowExposed(p_fixture.editor()));
  p_fixture.editor()->activateWindow();
  p_fixture.edit()->setFocus();
  QTRY_VERIFY_WITH_TIMEOUT(p_fixture.edit()->hasFocus(), 5000);
  p_fixture.waitForFreshListAst();
  QTest::qWait(50);
  QCoreApplication::processEvents();
}

// Coordinates remain document-local even when the image represents a clipped
// viewport or has a different device pixel ratio. No internal layout access.
struct ListDecorationRaster {
  QImage m_image;
  QRectF m_clip;
  qreal m_dpr = 1;
};

ListDecorationRaster renderListDecorations(Fixture &p_fixture, qreal p_dpr = 1,
                                           QRectF p_clip = QRectF(), bool p_selections = false) {
  auto doc = p_fixture.editor()->document();
  auto layout = doc->documentLayout();
  if (p_clip.isNull()) {
    const auto size = layout->documentSize();
    p_clip = QRectF(0, 0, qMax(size.width(), doc->pageSize().width()), size.height() + 16);
  }
  ListDecorationRaster result;
  result.m_clip = p_clip;
  result.m_dpr = p_dpr;
  result.m_image = QImage(qCeil(p_clip.width() * p_dpr), qCeil(p_clip.height() * p_dpr),
                          QImage::Format_ARGB32_Premultiplied);
  result.m_image.setDevicePixelRatio(p_dpr);
  result.m_image.fill(Qt::white);
  QPainter painter(&result.m_image);
  painter.translate(-p_clip.topLeft());
  painter.setClipRect(p_clip);
  QAbstractTextDocumentLayout::PaintContext context;
  context.clip = p_clip;
  context.palette = p_fixture.edit()->palette();
  // Most checks deliberately inspect the underlay without a selection. The
  // precedence checks pass the editor's real extra selections and selection.
  if (p_selections) {
    for (const auto &extra : p_fixture.edit()->extraSelections()) {
      QAbstractTextDocumentLayout::Selection selection;
      selection.cursor = extra.cursor;
      selection.format = extra.format;
      context.selections.append(selection);
    }
    if (p_fixture.edit()->textCursor().hasSelection()) {
      QAbstractTextDocumentLayout::Selection selection;
      selection.cursor = p_fixture.edit()->textCursor();
      selection.format.setForeground(context.palette.brush(QPalette::HighlightedText));
      selection.format.setBackground(context.palette.brush(QPalette::Highlight));
      context.selections.append(selection);
    }
  }
  layout->draw(&painter, context);
  return result;
}

QRect listDecorationPixels(const ListDecorationRaster &p_raster, QRectF p_rect = QRectF()) {
  if (p_rect.isNull()) {
    return p_raster.m_image.rect();
  }
  p_rect = p_rect.intersected(p_raster.m_clip);
  if (p_rect.isEmpty()) {
    return QRect();
  }
  p_rect.translate(-p_raster.m_clip.topLeft());
  return QRect(QPoint(qCeil(p_rect.left() * p_raster.m_dpr), qCeil(p_rect.top() * p_raster.m_dpr)),
               QPoint(qCeil(p_rect.right() * p_raster.m_dpr) - 1,
                      qCeil(p_rect.bottom() * p_raster.m_dpr) - 1))
      .intersected(p_raster.m_image.rect());
}

int listDecorationColorCount(const ListDecorationRaster &p_raster, const QColor &p_color,
                             const QRectF &p_rect = QRectF()) {
  const auto pixels = listDecorationPixels(p_raster, p_rect);
  int count = 0;
  for (int y = pixels.top(); !pixels.isEmpty() && y <= pixels.bottom(); ++y) {
    for (int x = pixels.left(); x <= pixels.right(); ++x) {
      count += p_raster.m_image.pixelColor(x, y) == p_color;
    }
  }
  return count;
}

QRectF listDecorationColorBounds(const ListDecorationRaster &p_raster, const QColor &p_color) {
  QRect bounds;
  for (int y = 0; y < p_raster.m_image.height(); ++y) {
    for (int x = 0; x < p_raster.m_image.width(); ++x) {
      if (p_raster.m_image.pixelColor(x, y) == p_color) {
        bounds |= QRect(x, y, 1, 1);
      }
    }
  }
  if (bounds.isEmpty()) {
    return QRectF();
  }
  return QRectF(bounds.x() / p_raster.m_dpr, bounds.y() / p_raster.m_dpr,
                bounds.width() / p_raster.m_dpr, bounds.height() / p_raster.m_dpr)
      .translated(p_raster.m_clip.topLeft());
}

QRectF listDecorationLineBand(Fixture &p_fixture, int p_block, int p_line = 0) {
  auto doc = p_fixture.editor()->document();
  const auto block = doc->findBlockByNumber(p_block);
  const auto line = block.layout()->lineAt(p_line);
  return QRectF(0, doc->documentLayout()->blockBoundingRect(block).top() + line.y(),
                qMax(doc->pageSize().width(), doc->documentLayout()->documentSize().width()),
                line.height());
}

qreal listDecorationMarkerX(Fixture &p_fixture, int p_block, int p_start, int p_end) {
  const auto block = p_fixture.editor()->document()->findBlockByNumber(p_block);
  const auto line = block.layout()->lineForTextPosition(p_start);
  return (line.cursorToX(p_start) + line.cursorToX(p_end)) / 2;
}

void verifyListGuideBand(const ListDecorationRaster &p_raster, qreal p_x, const QRectF &p_band,
                         bool p_present, const QColor &p_color = c_listGuideColor) {
  // Exactly one physical pixel of horizontal tolerance, independent of DPR.
  const int center = qFloor((p_x - p_raster.m_clip.left()) * p_raster.m_dpr);
  const auto band = listDecorationPixels(p_raster, p_band.adjusted(0, 1, 0, -1));
  QVERIFY(!band.isEmpty());
  for (int y : {band.top(), band.center().y(), band.bottom()}) {
    int count = 0;
    for (int x = qMax(0, center - 1); x <= qMin(center + 1, p_raster.m_image.width() - 1); ++x) {
      count += p_raster.m_image.pixelColor(x, y) == p_color;
    }
    if (p_present) {
      QVERIFY2(count == 1,
               qPrintable(QStringLiteral("hairline count %1 at x=%2 y=%3 bandTop=%4 DPR=%5")
                              .arg(count)
                              .arg(p_x)
                              .arg(y)
                              .arg(p_band.top())
                              .arg(p_raster.m_dpr)));
    } else {
      QCOMPARE(count, 0);
    }
  }
}

void verifyListActiveRow(Fixture &p_fixture, const ListDecorationRaster &p_raster, int p_block,
                         bool p_present, const QColor &p_color = c_listActiveColor) {
  const auto band = listDecorationLineBand(p_fixture, p_block);
  // Well inside the content edge, away from this fixture's short source text.
  const qreal x = band.width() - p_fixture.editor()->document()->documentMargin() - 24;
  const int px = qFloor((x - p_raster.m_clip.left()) * p_raster.m_dpr);
  const int py = qFloor((band.center().y() - p_raster.m_clip.top()) * p_raster.m_dpr);
  QVERIFY(p_raster.m_image.rect().contains(px, py));
  QCOMPARE(p_raster.m_image.pixelColor(px, py) == p_color, p_present);
}

void verifyListInkPreserved(const ListDecorationRaster &p_plain,
                            const ListDecorationRaster &p_guided) {
  QCOMPARE(p_plain.m_image.size(), p_guided.m_image.size());
  QCOMPARE(p_plain.m_clip, p_guided.m_clip);
  int ink = 0;
  for (int y = 0; y < p_plain.m_image.height(); ++y) {
    for (int x = 0; x < p_plain.m_image.width(); ++x) {
      const auto pixel = p_plain.m_image.pixel(x, y);
      if (pixel != qRgb(255, 255, 255)) {
        ++ink;
        QVERIFY2(pixel == p_guided.m_image.pixel(x, y),
                 qPrintable(QStringLiteral("guide altered glyph pixel (%1, %2)").arg(x).arg(y)));
      }
    }
  }
  QVERIFY(ink > 0);
}

QVector<QRectF> listDecorationBlockRects(Fixture &p_fixture) {
  QVector<QRectF> result;
  auto doc = p_fixture.editor()->document();
  for (auto block = doc->firstBlock(); block.isValid(); block = block.next()) {
    result.append(doc->documentLayout()->blockBoundingRect(block));
  }
  return result;
}

QSharedPointer<PreviewItem> makeListDecorationPreview(QTextDocument *p_doc, int p_block,
                                                      int p_height) {
  const auto block = p_doc->findBlockByNumber(p_block);
  auto item = QSharedPointer<PreviewItem>::create();
  item->m_blockNumber = p_block;
  item->m_blockPos = block.position();
  item->m_startPos = block.position();
  item->m_endPos = block.position() + qMax(1, block.length() - 1);
  item->m_isBlockwise = true;
  item->m_name = QStringLiteral("list_decoration_preview_%1_%2").arg(p_block).arg(p_height);
  item->m_image = QPixmap(120, p_height);
  item->m_image.fill(Qt::transparent);
  // Transparent interior makes a misplaced guide observable even though image
  // painting itself follows the guide pass. The border locates the reservation.
  QPainter painter(&item->m_image);
  painter.fillRect(0, 0, 120, 2, Qt::red);
  painter.fillRect(0, p_height - 2, 120, 2, Qt::red);
  painter.fillRect(0, 0, 2, p_height, Qt::red);
  painter.fillRect(118, 0, 2, p_height, Qt::red);
  return item;
}
} // namespace

void TestMarkdownEditor::testListItemGuides() {
  // This slot uses no active color and can run before active tint is implemented.
  const QString source = QStringLiteral("1. parent\n   continuation\n   - child\n"
                                        "     child text\n2. sibling\n\noutside");
  Fixture fixture(source, 1, -1, makeListDecorationConfig(c_listGuideColor, QColor()));
  showListDecorationFixture(fixture);
  const qreal parentX = listDecorationMarkerX(fixture, 0, 0, 2);
  const qreal childX = listDecorationMarkerX(fixture, 2, 3, 4);
  for (qreal dpr : {1.0, 2.0}) {
    const auto raster = renderListDecorations(fixture, dpr);
    for (int block : {1, 2, 3}) {
      verifyListGuideBand(raster, parentX, listDecorationLineBand(fixture, block), true);
    }
    verifyListGuideBand(raster, childX, listDecorationLineBand(fixture, 1), false);
    verifyListGuideBand(raster, childX, listDecorationLineBand(fixture, 2), false);
    verifyListGuideBand(raster, childX, listDecorationLineBand(fixture, 3), true);
    for (int block : {4, 6}) {
      QCOMPARE(listDecorationColorCount(raster, c_listGuideColor,
                                        listDecorationLineBand(fixture, block)),
               0);
    }
  }
  QCOMPARE(fixture.text(), source);

  {
    Fixture single(QStringLiteral("- only"), 0, -1,
                   makeListDecorationConfig(c_listGuideColor, QColor()));
    showListDecorationFixture(single);
    QCOMPARE(listDecorationColorCount(renderListDecorations(single), c_listGuideColor), 0);
  }
  {
    Fixture markers(QStringLiteral("9) parent\n   continuation\n\n- [x] task\n"
                                   "  continuation\n\noutside"),
                    0, -1, makeListDecorationConfig(c_listGuideColor, QColor()));
    showListDecorationFixture(markers);
    const auto raster = renderListDecorations(markers, 2);
    verifyListGuideBand(raster, listDecorationMarkerX(markers, 0, 0, 2),
                        listDecorationLineBand(markers, 1), true);
    verifyListGuideBand(raster, listDecorationMarkerX(markers, 3, 0, 1),
                        listDecorationLineBand(markers, 4), true);
    verifyListGuideBand(raster, listDecorationMarkerX(markers, 3, 2, 5),
                        listDecorationLineBand(markers, 4), false);
  }

  // A quote marker is text, while the following tab is a real whitespace run.
  // Surrogates and bidi text must retain exactly their undecorated glyph pixels.
  const QString quote = QStringLiteral(
      "> - \U0001F600 abc \u05d0\u05d1\n>\t continued abc\n>   - child\n>     tail\n\noutside");
  const QString wrapped = QStringLiteral("- parent\n  ") + QString(220, QLatin1Char('W')) +
                          QStringLiteral("\n\noutside");
  for (const auto &text :
       {quote, QStringLiteral("- parent\nlazy continuation\n\noutside"), wrapped}) {
    Fixture occlusion(text, 1, -1, makeListDecorationConfig(QColor(), QColor()));
    showListDecorationFixture(occlusion, QSize(260, 480));
    const auto rects = listDecorationBlockRects(occlusion);
    const auto plain1 = renderListDecorations(occlusion);
    const auto plain2 = renderListDecorations(occlusion, 2);
    occlusion.editor()->setConfig(makeListDecorationConfig(c_listGuideColor, QColor()));
    occlusion.waitForFreshListAst();
    QCOMPARE(listDecorationBlockRects(occlusion), rects);
    const auto guided1 = renderListDecorations(occlusion);
    const auto guided2 = renderListDecorations(occlusion, 2);
    verifyListInkPreserved(plain1, guided1);
    verifyListInkPreserved(plain2, guided2);
    const qreal x =
        listDecorationMarkerX(occlusion, 0, text == quote ? 2 : 0, text == quote ? 3 : 1);
    if (text == quote) {
      verifyListGuideBand(guided2, x, listDecorationLineBand(occlusion, 1), true);
      verifyListGuideBand(guided2, x, listDecorationLineBand(occlusion, 3), true);
    } else if (text == wrapped) {
      const auto layout = occlusion.editor()->document()->findBlockByNumber(1).layout();
      QVERIFY(layout->lineCount() > 2);
      verifyListGuideBand(guided2, x, listDecorationLineBand(occlusion, 1), true);
      for (int line = 1; line < layout->lineCount(); ++line) {
        verifyListGuideBand(guided2, x, listDecorationLineBand(occlusion, 1, line), false);
      }
    } else {
      verifyListGuideBand(guided1, x, listDecorationLineBand(occlusion, 1), false);
      verifyListGuideBand(guided2, x, listDecorationLineBand(occlusion, 1), false);
    }
    QCOMPARE(occlusion.text(), text);
  }

  QString longItem = QStringLiteral("- parent\n");
  for (int i = 0; i < 70; ++i) {
    longItem += QStringLiteral("  continuation %1 with enough words to wrap on resize\n").arg(i);
  }
  longItem += QStringLiteral("\noutside");
  Fixture scrolled(longItem, 22, -1, makeListDecorationConfig(c_listGuideColor, QColor()));
  showListDecorationFixture(scrolled, QSize(460, 260));
  for (int zoom : {0, 2, 0}) {
    scrolled.editor()->resize(zoom == 2 ? 300 : 460, 260);
    scrolled.editor()->zoom(zoom);
    QCoreApplication::processEvents();
    scrolled.moveTo(22);
    TextEditUtils::scrollBlockInPage(scrolled.edit(), 20, TextEditUtils::PagePosition::Top);
    QCoreApplication::processEvents();
    const int first = TextEditUtils::firstVisibleBlock(scrolled.edit()).blockNumber();
    QVERIFY(first > 0);
    const QRectF clip(0, TextEditUtils::contentOffsetAtTop(scrolled.edit()),
                      scrolled.edit()->viewport()->width(), scrolled.edit()->viewport()->height());
    QVERIFY(listDecorationLineBand(scrolled, 0).bottom() < clip.top());
    const qreal x = listDecorationMarkerX(scrolled, 0, 0, 1);
    for (qreal dpr : {1.0, 2.0}) {
      const auto raster = renderListDecorations(scrolled, dpr, clip);
      verifyListGuideBand(raster, x, listDecorationLineBand(scrolled, first + 1), true);
    }
    auto doc = scrolled.editor()->document();
    const auto block = doc->findBlockByNumber(first + 1);
    const auto line = block.layout()->lineAt(0);
    const QPointF point(line.cursorToX(5),
                        listDecorationLineBand(scrolled, first + 1).center().y());
    QCOMPARE(doc->documentLayout()->hitTest(point, Qt::FuzzyHit), block.position() + 5);
    QCOMPARE(scrolled.text(), longItem);
  }
}

void TestMarkdownEditor::testListItemActiveBackground() {
  const QString source = QStringLiteral("1. parent\n   continuation\n   - child\n"
                                        "     child text\n2. sibling\n\noutside");
  Fixture fixture(source, 1, -1, makeListDecorationConfig());
  showListDecorationFixture(fixture);
  auto verifyScope = [&](const QList<int> &rows) {
    const auto raster = renderListDecorations(fixture);
    for (int block : {0, 1, 2, 3, 4, 6}) {
      verifyListActiveRow(fixture, raster, block, rows.contains(block));
    }
  };
  verifyScope({0, 1, 2, 3});
  fixture.moveTo(3);
  verifyScope({2, 3});
  fixture.moveTo(4);
  verifyScope({4});
  fixture.moveTo(6);
  verifyScope({});
  fixture.select(1, 3, 3, 5);
  verifyScope({2, 3});
  fixture.select(3, 5, 1, 3);
  verifyScope({0, 1, 2, 3});
  fixture.edit()->setOverriddenSelection(
      fixture.editor()->document()->findBlockByNumber(3).position(), fixture.blockEnd(3));
  verifyScope({0, 1, 2, 3});
  fixture.edit()->clearOverriddenSelection();
  QCOMPARE(fixture.text(), source);

  {
    Fixture sameLine(QStringLiteral("- - child\n    child text\n\n  outer tail\n\noutside"), 0, 1,
                     makeListDecorationConfig());
    showListDecorationFixture(sameLine);
    auto raster = renderListDecorations(sameLine);
    for (int block : {0, 1, 2, 3}) {
      verifyListActiveRow(sameLine, raster, block, true);
    }
    sameLine.select(0, 4, 0, 4);
    raster = renderListDecorations(sameLine);
    verifyListActiveRow(sameLine, raster, 0, true);
    verifyListActiveRow(sameLine, raster, 1, true);
    verifyListActiveRow(sameLine, raster, 3, false);
    sameLine.select(0, 4, 0, 1);
    verifyListActiveRow(sameLine, renderListDecorations(sameLine), 3, true);
    sameLine.select(0, 1, 0, 4);
    verifyListActiveRow(sameLine, renderListDecorations(sameLine), 3, false);
  }
  {
    Fixture indented(QStringLiteral("  - parent\n    body\n\noutside"), 0, 0,
                     makeListDecorationConfig());
    showListDecorationFixture(indented);
    QCOMPARE(listDecorationColorCount(renderListDecorations(indented), c_listActiveColor), 0);
    indented.select(0, 2, 0, 2);
    verifyListActiveRow(indented, renderListDecorations(indented), 1, true);
  }
  for (const auto &source : {QStringLiteral("- parent\nlazy continuation\n\noutside"),
                             QStringLiteral("- parent\n\n  continuation\n\noutside")}) {
    Fixture owned(source, 0, -1, makeListDecorationConfig());
    showListDecorationFixture(owned);
    const auto raster = renderListDecorations(owned, 2);
    verifyListActiveRow(owned, raster, 1, true); // Lazy text or an ITEM-owned blank row.
    verifyListActiveRow(owned, raster, owned.editor()->document()->blockCount() - 1, false);
  }
  {
    Fixture wrapped(QStringLiteral("- parent\n  ") + QString(180, QLatin1Char('W')), 1, -1,
                    makeListDecorationConfig());
    showListDecorationFixture(wrapped, QSize(260, 480));
    const auto layout = wrapped.editor()->document()->findBlockByNumber(1).layout();
    QVERIFY(layout->lineCount() > 2);
    const auto raster = renderListDecorations(wrapped, 2);
    for (int line = 0; line < layout->lineCount(); ++line) {
      QVERIFY(listDecorationColorCount(raster, c_listActiveColor,
                                       listDecorationLineBand(wrapped, 1, line)) > 0);
    }
  }
}

void TestMarkdownEditor::testListItemDecorationsFreshness() {
  {
    Fixture typing(QStringLiteral("- parent\n  before\n  after\n- sibling\n"
                                  "  typing\n  untouched\n\noutside"),
                   4, -1, makeListDecorationConfig());
    showListDecorationFixture(typing);
    const qreal parentX = listDecorationMarkerX(typing, 0, 0, 1);
    const qreal siblingX = listDecorationMarkerX(typing, 3, 0, 1);
    // Render synchronously after each edit, before another full parse can run.
    // Guides in the untouched item and the active item's untouched rows stay visible.
    for (const auto character : QStringLiteral("xyz")) {
      typing.edit()->insertPlainText(QString(character));
      const auto raster = renderListDecorations(typing);
      for (int block : {1, 2}) {
        verifyListGuideBand(raster, parentX, listDecorationLineBand(typing, block), true);
        verifyListActiveRow(typing, raster, block, false);
      }
      for (int block : {4, 5}) {
        verifyListGuideBand(raster, siblingX, listDecorationLineBand(typing, block), true);
      }
      for (int block : {3, 4, 5}) {
        verifyListActiveRow(typing, raster, block, true);
      }
      verifyListActiveRow(typing, raster, 7, false);
    }
    QCOMPARE(typing.blockText(4), QStringLiteral("  typingxyz"));
  }

  {
    const QString source = QStringLiteral("intro\n\n1. parent\n   body\n   - - child\n"
                                          "       tail\n\n     outer tail\n2. sibling\n"
                                          "   end\n\noutside");
    Fixture stable(source, 2, -1, makeListDecorationConfig());
    showListDecorationFixture(stable);
    const qreal parentX = listDecorationMarkerX(stable, 2, 0, 2);
    const qreal outerX = listDecorationMarkerX(stable, 4, 3, 4);
    const qreal innerX = listDecorationMarkerX(stable, 4, 5, 6);
    const qreal siblingX = listDecorationMarkerX(stable, 8, 0, 2);
    auto verifyStable = [&]() {
      // Lookup must use the same block/column ordering as the cached anchors,
      // including two markers on one line after an edit in a preceding block.
      for (int column : {4, 6}) {
        stable.select(4, column, 4, column);
        for (qreal dpr : {1.0, 2.0}) {
          const auto raster = renderListDecorations(stable, dpr);
          verifyListGuideBand(raster, parentX, listDecorationLineBand(stable, 3), true);
          for (qreal x : {parentX, outerX, innerX}) {
            verifyListGuideBand(raster, x, listDecorationLineBand(stable, 5), true);
          }
          verifyListGuideBand(raster, siblingX, listDecorationLineBand(stable, 9), true);
          verifyListActiveRow(stable, raster, 3, false);
          verifyListActiveRow(stable, raster, 5, true);
          verifyListActiveRow(stable, raster, 7, column == 4);
          verifyListActiveRow(stable, raster, 9, false);
        }
      }
    };
    verifyStable();
    auto doc = stable.editor()->document();
    const QString added = QStringLiteral("xyz\U0001F680");
    for (int block : {0, 2, 3, 7}) {
      QTextCursor edit(doc->findBlockByNumber(block));
      edit.movePosition(QTextCursor::EndOfBlock);
      const int end = edit.position();
      edit.insertText(added);
      verifyStable(); // No event processing or parse between the edit and draw.
      edit.setPosition(end, QTextCursor::KeepAnchor);
      edit.removeSelectedText();
      verifyStable();
    }
    QCOMPARE(stable.text(), source);
    stable.waitForFreshListAst();
    verifyStable();
  }

  const QString source = QStringLiteral("- parent\n  continuation\n\noutside");
  Fixture fixture(source, 1, -1, makeListDecorationConfig());
  showListDecorationFixture(fixture);
  auto doc = fixture.editor()->document();
  auto verifyAbsent = [&]() {
    const auto raster = renderListDecorations(fixture);
    QCOMPARE(listDecorationColorCount(raster, c_listGuideColor), 0);
    QCOMPARE(listDecorationColorCount(raster, c_listActiveColor), 0);
  };
  auto verifyRestored = [&]() {
    fixture.moveTo(1);
    const auto raster = renderListDecorations(fixture);
    verifyListGuideBand(raster, listDecorationMarkerX(fixture, 0, 0, 1),
                        listDecorationLineBand(fixture, 1), true);
    verifyListActiveRow(fixture, raster, 1, true);
  };
  verifyRestored();
  QTextCursor edit(doc);
  edit.setPosition(0);
  edit.setPosition(2, QTextCursor::KeepAnchor);
  edit.removeSelectedText();
  // Structural edits may leave stale decoration until the full result replaces it.
  verifyListActiveRow(fixture, renderListDecorations(fixture), 1, true);
  fixture.waitForFreshListAst();
  verifyAbsent();
  doc->undo();
  verifyAbsent();
  fixture.waitForFreshListAst();
  QCOMPARE(fixture.text(), source);
  verifyRestored();
  doc->redo();
  verifyListActiveRow(fixture, renderListDecorations(fixture), 1, true);
  fixture.waitForFreshListAst();
  verifyAbsent();

  edit.setPosition(0);
  edit.insertText(QStringLiteral("- "));
  verifyAbsent(); // The retained result has no list until a new full result arrives.
  fixture.waitForFreshListAst();
  verifyRestored();
  fixture.waitForFreshListAst(); // Matched updateHighlight() re-publication remains visible.
  verifyRestored();

  const auto rects = listDecorationBlockRects(fixture);
  const auto size = doc->documentLayout()->documentSize();
  const int revision = doc->revision();
  const int undoSteps = doc->availableUndoSteps();
  const int redoSteps = doc->availableRedoSteps();
  const bool modified = doc->isModified();
  for (int block : {0, 1, 3, 1, 0, 3}) {
    fixture.moveTo(block);
    renderListDecorations(fixture);
    renderListDecorations(fixture, 2);
  }
  QCoreApplication::processEvents();
  QCOMPARE(fixture.text(), source);
  QCOMPARE(doc->revision(), revision);
  QCOMPARE(doc->availableUndoSteps(), undoSteps);
  QCOMPARE(doc->availableRedoSteps(), redoSteps);
  QCOMPARE(doc->isModified(), modified);
  QCOMPARE(doc->documentLayout()->documentSize(), size);
  QCOMPARE(listDecorationBlockRects(fixture), rects);

  fixture.editor()->setText(QStringLiteral("1. parent\n   continuation\n2. sibling\n   tail"));
  fixture.waitForFreshListAst();
  edit = QTextCursor(doc);
  edit.setPosition(doc->findBlockByNumber(2).position());
  edit.insertText(QStringLiteral("\nabcjdkejj\n\n")); // testListAutoNumberSplit separator.
  fixture.waitForFreshListAst();
  fixture.moveTo(1);
  auto raster = renderListDecorations(fixture);
  verifyListActiveRow(fixture, raster, 1, true);
  verifyListActiveRow(fixture, raster, 3, false);
  QCOMPARE(listDecorationColorCount(raster, c_listGuideColor, listDecorationLineBand(fixture, 3)),
           0);
  fixture.moveTo(6);
  raster = renderListDecorations(fixture);
  verifyListActiveRow(fixture, raster, 0, false);
  verifyListActiveRow(fixture, raster, 5, true);
  verifyListActiveRow(fixture, raster, 6, true);
  verifyListGuideBand(raster, listDecorationMarkerX(fixture, 5, 0, 2),
                      listDecorationLineBand(fixture, 6), true);
  QCOMPARE(fixture.text(), QStringLiteral("1. parent\n   continuation\n\nabcjdkejj\n\n"
                                          "2. sibling\n   tail")); // Numbering is disabled.
  doc->clear();
  fixture.waitForFreshListAst();
  verifyAbsent();
  fixture.editor()->setText(source);
  verifyAbsent();
  fixture.waitForFreshListAst();
  verifyRestored();
}

void TestMarkdownEditor::testListItemDecorationsGeometry() {
  const QString source = QStringLiteral("1. parent \U0001F600 \u05d0\u05d1\n"
                                        "   continuation\n   - child\n     child text\n\noutside");
  Fixture fixture(source, 1, -1, makeListDecorationConfig(QColor(), QColor()));
  showListDecorationFixture(fixture);
  auto doc = fixture.editor()->document();
  const auto rects = listDecorationBlockRects(fixture);
  const auto size = doc->documentLayout()->documentSize();
  const auto cursorRect = fixture.edit()->cursorRect();
  const int cursorPosition = fixture.edit()->textCursor().position();
  const auto body = doc->findBlockByNumber(1);
  const auto line = body.layout()->lineAt(0);
  const QPointF hitPoint(line.cursorToX(5), listDecorationLineBand(fixture, 1).center().y());
  const int hit = doc->documentLayout()->hitTest(hitPoint, Qt::FuzzyHit);
  QCOMPARE(hit, body.position() + 5);
  const QColor otherGuide(QStringLiteral("#004ee8"));
  const QColor otherActive(QStringLiteral("#f0c1e5"));
  const QList<QPair<QColor, QColor>> colors{{c_listGuideColor, QColor()},
                                            {QColor(), c_listActiveColor},
                                            {c_listGuideColor, c_listActiveColor},
                                            {otherGuide, otherActive},
                                            {QColor(), QColor()}};
  for (const auto &colorsForConfig : colors) {
    // A complete new config/theme, not a mutation of TextEditorConfig::defaultTheme().
    fixture.editor()->setConfig(
        makeListDecorationConfig(colorsForConfig.first, colorsForConfig.second));
    fixture.waitForFreshListAst();
    QCoreApplication::processEvents();
    QCOMPARE(listDecorationBlockRects(fixture), rects);
    QCOMPARE(doc->documentLayout()->documentSize(), size);
    QCOMPARE(fixture.edit()->cursorRect(), cursorRect);
    QCOMPARE(fixture.edit()->textCursor().position(), cursorPosition);
    QCOMPARE(doc->documentLayout()->hitTest(hitPoint, Qt::FuzzyHit), hit);
    for (qreal dpr : {1.0, 2.0}) {
      const auto raster = renderListDecorations(fixture, dpr);
      if (colorsForConfig.first.isValid()) {
        verifyListGuideBand(raster, listDecorationMarkerX(fixture, 0, 0, 2),
                            listDecorationLineBand(fixture, 1), true, colorsForConfig.first);
      }
      if (colorsForConfig.second.isValid()) {
        verifyListActiveRow(fixture, raster, 1, true, colorsForConfig.second);
        verifyListActiveRow(fixture, raster, 3, true, colorsForConfig.second);
      }
      for (const auto &oldColor : {c_listGuideColor, otherGuide}) {
        if (oldColor != colorsForConfig.first) {
          QCOMPARE(listDecorationColorCount(raster, oldColor), 0);
        }
      }
      for (const auto &oldColor : {c_listActiveColor, otherActive}) {
        if (oldColor != colorsForConfig.second) {
          QCOMPARE(listDecorationColorCount(raster, oldColor), 0);
        }
      }
    }
    QCOMPARE(fixture.text(), source);
  }

  {
    Fixture precedence(
        QStringLiteral("- parent\n  `code` body\n  tail\n\noutside"), 1, -1,
        makeListDecorationConfig(c_listGuideColor, c_listActiveColor, c_listCursorLineColor));
    showListDecorationFixture(precedence);
    auto raster = renderListDecorations(precedence, 1, QRectF(), true);
    verifyListActiveRow(precedence, raster, 1, true, c_listCursorLineColor);
    verifyListActiveRow(precedence, raster, 2, true);
    precedence.select(1, 9, 1, 13);
    raster = renderListDecorations(precedence, 1, QRectF(), true);
    const auto selectionBand = listDecorationLineBand(precedence, 1);
    QVERIFY(listDecorationColorCount(raster, c_listSelectionColor, selectionBand) > 0);
    // Syntax background survives the tint on a different, non-current source row.
    precedence.moveTo(2);
    raster = renderListDecorations(precedence, 2, QRectF(), true);
    QVERIFY(listDecorationColorCount(raster, c_listSyntaxColor,
                                     listDecorationLineBand(precedence, 1)) > 0);
    verifyListActiveRow(precedence, raster, 1, true);
  }

  QString sourceRows = QStringLiteral("- parent\n");
  for (int i = 0; i < 60; ++i) {
    sourceRows += QStringLiteral("  row %1 with wrapping words and more wrapping words\n").arg(i);
  }
  sourceRows += QStringLiteral("\noutside");
  Fixture scrolled(sourceRows, 25, -1, makeListDecorationConfig());
  showListDecorationFixture(scrolled, QSize(340, 260));
  for (int zoom : {2, 0}) {
    scrolled.editor()->zoom(zoom);
    scrolled.editor()->resize(zoom == 2 ? 280 : 340, 260);
    QCoreApplication::processEvents();
    scrolled.moveTo(25);
    TextEditUtils::scrollBlockInPage(scrolled.edit(), 23, TextEditUtils::PagePosition::Top);
    QCoreApplication::processEvents();
    const int top = TextEditUtils::contentOffsetAtTop(scrolled.edit());
    const int first = TextEditUtils::firstVisibleBlock(scrolled.edit()).blockNumber();
    QVERIFY(first > 0);
    const QRectF clip(0, top, scrolled.edit()->viewport()->width(),
                      scrolled.edit()->viewport()->height());
    const auto raster = renderListDecorations(scrolled, 2, clip);
    const auto band = listDecorationLineBand(scrolled, first + 1);
    verifyListGuideBand(raster, listDecorationMarkerX(scrolled, 0, 0, 1), band, true);
    QVERIFY(listDecorationColorCount(raster, c_listActiveColor, band) > 0);
    const auto enabledRects = listDecorationBlockRects(scrolled);
    const auto enabledCursor = scrolled.edit()->cursorRect();
    // setConfig() applies the theme's font independently of zoom. Keep the
    // presented font identical so this comparison isolates decoration colors.
    const int pointSize = scrolled.editor()->editorFontPointSize();
    scrolled.editor()->setConfig(makeListDecorationConfig(QColor(), QColor(), QColor(), pointSize));
    scrolled.waitForFreshListAst();
    QCOMPARE(listDecorationBlockRects(scrolled), enabledRects);
    QCOMPARE(scrolled.edit()->cursorRect(), enabledCursor);
    QCOMPARE(TextEditUtils::contentOffsetAtTop(scrolled.edit()), top);
    const auto disabled = renderListDecorations(scrolled, 2, clip);
    QCOMPARE(listDecorationColorCount(disabled, c_listGuideColor), 0);
    QCOMPARE(listDecorationColorCount(disabled, c_listActiveColor), 0);
    scrolled.editor()->setConfig(
        makeListDecorationConfig(c_listGuideColor, c_listActiveColor, QColor(), pointSize));
    scrolled.waitForFreshListAst();
    QCOMPARE(scrolled.text(), sourceRows);
  }
}

void TestMarkdownEditor::testListItemDecorationsFoldingAndPreviews() {
  {
    const QString source = QStringLiteral("- parent\n  ```text\n  alpha\n  beta\n  ```\n"
                                          "  after\n\noutside");
    Fixture fixture(source, 2, -1, makeListDecorationConfig());
    showListDecorationFixture(fixture);
    auto doc = fixture.editor()->document();
    const qreal expandedHeight = doc->documentLayout()->documentSize().height();
    const auto expandedRects = listDecorationBlockRects(fixture);
    QVERIFY(fixture.editor()->foldAtCursor());
    QTRY_VERIFY_WITH_TIMEOUT(!doc->findBlockByNumber(2).isVisible(), 5000);
    QVERIFY(!doc->findBlockByNumber(3).isVisible());
    const qreal foldedHeight = doc->documentLayout()->documentSize().height();
    QVERIFY(foldedHeight < expandedHeight);
    QCOMPARE(doc->documentLayout()->blockBoundingRect(doc->findBlockByNumber(2)).height(), 0.0);
    QCOMPARE(doc->documentLayout()->blockBoundingRect(doc->findBlockByNumber(3)).height(), 0.0);
    auto raster = renderListDecorations(fixture);
    verifyListGuideBand(raster, listDecorationMarkerX(fixture, 0, 0, 1),
                        listDecorationLineBand(fixture, 5), true);
    verifyListActiveRow(fixture, raster, 5, true);
    QCOMPARE(doc->documentLayout()->documentSize().height(), foldedHeight);
    QVERIFY(fixture.editor()->unfoldAtCursor());
    QTRY_VERIFY_WITH_TIMEOUT(doc->findBlockByNumber(2).isVisible(), 5000);
    QVERIFY(doc->findBlockByNumber(3).isVisible());
    QCOMPARE(listDecorationBlockRects(fixture), expandedRects);
    QCOMPARE(doc->documentLayout()->documentSize().height(), expandedHeight);
    fixture.moveTo(2);
    raster = renderListDecorations(fixture);
    verifyListActiveRow(fixture, raster, 2, true);

    auto mgr = fixture.editor()->getPreviewMgr();
    mgr->setPreviewEnabled(PreviewData::CodeBlock, true);
    qreal previousAfter = 0;
    for (int height : {60, 130}) {
      mgr->updateCodeBlocks({makeListDecorationPreview(doc, 2, height)});
      QCoreApplication::processEvents();
      raster = renderListDecorations(fixture, 2);
      const auto imageRect = listDecorationColorBounds(raster, Qt::red);
      QVERIFY(!imageRect.isEmpty());
      QVERIFY2(qAbs(imageRect.height() - height) <= 1,
               qPrintable(QStringLiteral("painted height %1, requested %2")
                              .arg(imageRect.height())
                              .arg(height)));
      const qreal x = listDecorationMarkerX(fixture, 0, 0, 1);
      QVERIFY(imageRect.left() <= x && x <= imageRect.right());
      QCOMPARE(listDecorationColorCount(raster, c_listGuideColor, imageRect), 0);
      verifyListGuideBand(raster, x, listDecorationLineBand(fixture, 5), true);
      verifyListActiveRow(fixture, raster, 5, true);
      const qreal after = listDecorationLineBand(fixture, 5).top();
      if (previousAfter > 0) {
        QVERIFY(qAbs(after - previousAfter - 70) <= 1);
      }
      previousAfter = after;
      QCOMPARE(fixture.text(), source);
    }
  }
  {
    // The terminal preview bottom, not the last block's extra padding, ends both decorations.
    Fixture ending(QStringLiteral("- parent\n  body"), 1, -1, makeListDecorationConfig());
    showListDecorationFixture(ending);
    auto doc = ending.editor()->document();
    ending.editor()->getPreviewMgr()->setPreviewEnabled(PreviewData::CodeBlock, true);
    ending.editor()->getPreviewMgr()->updateCodeBlocks({makeListDecorationPreview(doc, 1, 60)});
    QCoreApplication::processEvents();
    const auto raster = renderListDecorations(ending, 2);
    const auto imageRect = listDecorationColorBounds(raster, Qt::red);
    QVERIFY(!imageRect.isEmpty());
    const QRectF below(0, imageRect.bottom() + 1, raster.m_clip.width(),
                       raster.m_clip.bottom() - imageRect.bottom() - 1);
    QCOMPARE(listDecorationColorCount(raster, c_listGuideColor, below), 0);
    QCOMPARE(listDecorationColorCount(raster, c_listActiveColor, below), 0);
  }
  {
    auto config = makeListDecorationConfig();
    config->m_inplacePreviewSources = MarkdownEditorConfig::Table;
    const QString source = QStringLiteral("- parent\n\n  | h1 | h2 |\n  | --- | --- |\n"
                                          "  | a | b |\n\n  tail\n\noutside");
    Fixture table(source, 6, -1, config);
    showListDecorationFixture(table, QSize(680, 560));
    auto widgets = [&]() {
      return table.edit()->viewport()->findChildren<PreviewWidget *>(QString(),
                                                                     Qt::FindDirectChildrenOnly);
    };
    QTRY_COMPARE_WITH_TIMEOUT(widgets().size(), 1, 5000);
    auto widget = widgets().first();
    QTRY_VERIFY_WITH_TIMEOUT(widget->isVisible(), 5000);
    auto sheet = widget->findChild<QTextEdit *>();
    QVERIFY(sheet);
    table.moveTo(6);
    const auto raster = renderListDecorations(table, 2);
    const QRectF widgetRect = QRectF(widget->geometry())
                                  .translated(table.edit()->horizontalScrollBar()->value(),
                                              TextEditUtils::contentOffsetAtTop(table.edit()));
    const qreal x = listDecorationMarkerX(table, 0, 0, 1);
    QVERIFY(widgetRect.left() <= x && x <= widgetRect.right());
    QCOMPARE(listDecorationColorCount(raster, c_listGuideColor, widgetRect), 0);
    verifyListGuideBand(raster, x, listDecorationLineBand(table, 6), true);
    verifyListActiveRow(table, raster, 0, true);
    auto verifyChildSurface = [&]() {
      // The source underlay must not recolor the embedded sheet itself.
      ListDecorationRaster child;
      child.m_image = widget->grab().toImage();
      QVERIFY(!child.m_image.isNull());
      QCOMPARE(listDecorationColorCount(child, c_listActiveColor), 0);
      QCOMPARE(listDecorationColorCount(child, c_listGuideColor), 0);
    };
    verifyChildSurface();
    sheet->setFocus();
    QTRY_VERIFY_WITH_TIMEOUT(table.edit()->isViewportWidgetFocused(), 5000);
    const auto focused = renderListDecorations(table, 2);
    QCOMPARE(listDecorationColorCount(focused, c_listActiveColor), 0);
    verifyListGuideBand(focused, x, listDecorationLineBand(table, 6), true);
    verifyChildSurface();
    table.edit()->setFocus();
    QTRY_VERIFY_WITH_TIMEOUT(table.edit()->hasFocus(), 5000);
    verifyListActiveRow(table, renderListDecorations(table, 2), 0, true);
    QCOMPARE(table.text(), source);
  }
  {
    const QString source =
        QStringLiteral("- parent\n  continuation\n  - child\n    child text\n\noutside");
    Fixture ime(source, 1, -1, makeListDecorationConfig());
    showListDecorationFixture(ime);
    auto doc = ime.editor()->document();
    const QString composition = QStringLiteral("\u3042");
    for (int block : {1, 0}) {
      ime.moveTo(block);
      const int cursor = ime.edit()->textCursor().position();
      QInputMethodEvent preedit(composition, QList<QInputMethodEvent::Attribute>());
      QCoreApplication::sendEvent(ime.edit(), &preedit);
      QCOMPARE(doc->findBlockByNumber(block).layout()->preeditAreaText(), composition);
      // Composition keeps cached ownership; paint omits only its preedit block.
      const auto raster = renderListDecorations(ime, 2);
      const auto band = listDecorationLineBand(ime, block);
      QCOMPARE(listDecorationColorCount(raster, c_listGuideColor, band), 0);
      QCOMPARE(listDecorationColorCount(raster, c_listActiveColor, band), 0);
      const qreal parentX = listDecorationMarkerX(ime, 0, 0, 1);
      if (block == 0) {
        verifyListGuideBand(raster, parentX, listDecorationLineBand(ime, 1), false);
      }
      verifyListGuideBand(raster, listDecorationMarkerX(ime, 2, 2, 3),
                          listDecorationLineBand(ime, 3), true);
      verifyListActiveRow(ime, raster, 3, true);
      QCOMPARE(ime.text(), source);
      QCOMPARE(ime.edit()->textCursor().position(), cursor);
      QCOMPARE(doc->findBlockByNumber(block).layout()->preeditAreaText(), composition);
      QInputMethodEvent cancel;
      QCoreApplication::sendEvent(ime.edit(), &cancel);
      QCOMPARE(doc->findBlockByNumber(block).layout()->preeditAreaText(), QString());
      const auto restored = renderListDecorations(ime, 2);
      verifyListGuideBand(restored, listDecorationMarkerX(ime, 0, 0, 1),
                          listDecorationLineBand(ime, 1), true);
      verifyListActiveRow(ime, restored, block, true);
    }
    ime.moveTo(1);
    QInputMethodEvent preedit(composition, QList<QInputMethodEvent::Attribute>());
    QCoreApplication::sendEvent(ime.edit(), &preedit);
    QInputMethodEvent commit;
    commit.setCommitString(composition);
    QCoreApplication::sendEvent(ime.edit(), &commit);
    verifyListActiveRow(ime, renderListDecorations(ime), 1, true);
    ime.waitForFreshListAst();
    QCOMPARE(ime.blockText(1), QStringLiteral("  continuation\u3042"));
    QCOMPARE(doc->findBlockByNumber(1).layout()->preeditAreaText(), QString());
    verifyListActiveRow(ime, renderListDecorations(ime), 1, true);
  }
}

namespace {
QSharedPointer<MarkdownEditorConfig>
concealEditConfig(bool p_enabled = true,
                  const QColor &p_background = QColor(QStringLiteral("#d7e6ff"))) {
  auto config = makeConcealConfig();
  config->m_textEditorConfig->m_lineNumberType = VTextEditor::LineNumberType::None;
  config->m_textEditorConfig->m_textFoldingEnabled = false;
  const QJsonObject json{
      {QStringLiteral("metadata"),
       QJsonObject{{QStringLiteral("type"), QStringLiteral("vtextedit")}}},
      {QStringLiteral("editor-styles"),
       QJsonObject{{QStringLiteral("Text"),
                    QJsonObject{{QStringLiteral("font-family"), QStringLiteral("Courier New")},
                                {QStringLiteral("font-size"), 14},
                                {QStringLiteral("text-color"), QStringLiteral("#252525")},
                                {QStringLiteral("background-color"), QStringLiteral("#ffffff")}}},
                   {QStringLiteral("ConcealedText"),
                    QJsonObject{{QStringLiteral("text-color"), QStringLiteral("#124aab")},
                                {QStringLiteral("background-color"), p_background.name()}}}}},
      {QStringLiteral("markdown-syntax-styles"),
       QJsonObject{
           {QStringLiteral("LINK"),
            QJsonObject{{QStringLiteral("text-color"), QStringLiteral("#94134a")},
                        {QStringLiteral("background-color"), QStringLiteral("#f5d8c5")}}},
           {QStringLiteral("CODE"),
            QJsonObject{{QStringLiteral("text-color"), QStringLiteral("#175522")},
                        {QStringLiteral("background-color"), QStringLiteral("#ccf0d0")}}}}}};
  config->m_textEditorConfig->m_theme = Theme::createThemeFromContent(
      QString::fromUtf8(QJsonDocument(json).toJson(QJsonDocument::Compact)));
  Q_ASSERT(config->m_textEditorConfig->m_theme);
  if (!p_enabled) {
    config->m_concealElements = MarkdownConcealElements();
  }
  return config;
}

QRect concealEditCursorRect(Fixture &p_fixture, int p_position) {
  QTextCursor cursor(p_fixture.editor()->document());
  cursor.setPosition(p_position);
  return p_fixture.edit()->cursorRect(cursor);
}

void concealEditCompareRect(const QRect &p_actual, const QRect &p_expected) {
  QVERIFY(qAbs(p_actual.left() - p_expected.left()) <= 1);
  QVERIFY(qAbs(p_actual.top() - p_expected.top()) <= 1);
  QVERIFY(qAbs(p_actual.right() - p_expected.right()) <= 1);
  QVERIFY(qAbs(p_actual.bottom() - p_expected.bottom()) <= 1);
}

// Interior to the leading quarter of a glyph, never an exact/fuzzy boundary.
QPoint concealEditGlyphPoint(Fixture &p_fixture, int p_position) {
  const auto left = concealEditCursorRect(p_fixture, p_position);
  const auto right = concealEditCursorRect(p_fixture, p_position + 1);
  return QPoint(left.left() + qMax(1, (right.left() - left.left()) / 4), left.center().y());
}

QImage concealEditCrop(const QImage &p_image, const QRect &p_rect) {
  const qreal dpr = p_image.devicePixelRatio();
  return p_image.copy(QRect(qRound(p_rect.x() * dpr), qRound(p_rect.y() * dpr),
                            qRound(p_rect.width() * dpr), qRound(p_rect.height() * dpr)));
}

QRect concealEditColorBounds(const QImage &p_image, const QColor &p_color) {
  QRect bounds;
  for (int y = 0; y < p_image.height(); ++y) {
    for (int x = 0; x < p_image.width(); ++x) {
      if (p_image.pixelColor(x, y).rgb() == p_color.rgb()) {
        bounds = bounds.united(QRect(x, y, 1, 1));
      }
    }
  }
  return bounds;
}

void concealEditSaveImage(const QImage &p_image, const QString &p_name) {
  const QString directory = qEnvironmentVariable("VTE_CONCEAL_TEST_IMAGE_DIR");
  if (!directory.isEmpty()) {
    QVERIFY(QDir().mkpath(directory));
    QVERIFY(p_image.save(QDir(directory).filePath(p_name)));
  }
}
} // namespace

void TestMarkdownEditor::testConcealEditingAndGeometry() {
  const QString url = QStringLiteral("abcdefghijklmnopqrstuvwxyz");
  const QString compactUrl = QStringLiteral("abc\u00b7\u00b7\u00b7xyz");
  const QString source = QStringLiteral("[x](") + url + QStringLiteral(") tail\noutside");
  const QString compact = QStringLiteral("[x](") + compactUrl + QStringLiteral(") tail\noutside");
  const int start = source.indexOf(url);
  const int hidden = start + 3;

  {
    Fixture reference(compact, -1, -1, concealEditConfig(false));
    waitForConcealPublication(reference);
    renderConcealEditor(reference);
    const auto markerPoint = concealEditGlyphPoint(reference, hidden + 1);
    const auto compactTail =
        concealEditCursorRect(reference, compact.indexOf(QStringLiteral("tail")));
    Fixture revealed(source, -1, -1, concealEditConfig(false));
    waitForConcealPublication(revealed);
    renderConcealEditor(revealed);
    const auto fullTail = concealEditCursorRect(revealed, source.indexOf(QStringLiteral("tail")));
    Fixture fixture(source, -1, -1, concealEditConfig());
    waitForConcealPublication(fixture);
    const auto compactImage = renderConcealEditor(fixture);
    concealEditSaveImage(compactImage, QStringLiteral("conceal-edit-compact.png"));
    auto doc = fixture.editor()->document();
    doc->clearUndoRedoStacks();
    concealEditCompareRect(concealEditCursorRect(fixture, source.indexOf(QStringLiteral("tail"))),
                           compactTail);
    QTest::mouseClick(fixture.edit()->viewport(), Qt::LeftButton, Qt::NoModifier, markerPoint);
    QCOMPARE(fixture.edit()->textCursor().position(), hidden);
    QCOMPARE(fixture.text(), source);
    concealEditCompareRect(concealEditCursorRect(fixture, source.indexOf(QStringLiteral("tail"))),
                           fullTail);
    concealEditSaveImage(renderConcealEditor(fixture), QStringLiteral("conceal-edit-revealed.png"));

    QTest::keyClicks(fixture.edit(), QStringLiteral("UV"), Qt::NoModifier, 0);
    QString inserted = source;
    inserted.insert(hidden, QStringLiteral("UV"));
    QCOMPARE(fixture.text(), inserted);
    QCOMPARE(fixture.edit()->textCursor().position(), hidden + 2);
    QTest::keyClick(fixture.edit(), Qt::Key_Backspace, Qt::NoModifier, 0);
    QString erased = source;
    erased.insert(hidden, QStringLiteral("U"));
    QCOMPARE(fixture.text(), erased);
    QCOMPARE(fixture.edit()->textCursor().position(), hidden + 1);

    // Deletion and the preceding contiguous typing are separate source edits;
    // revealing, parsing and reconcealing must not create intervening commands.
    waitForConcealPublication(fixture);
    QTest::keyClick(fixture.edit(), Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(fixture.text(), inserted);
    QTest::keyClick(fixture.edit(), Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(fixture.text(), source);
    QVERIFY(!doc->isUndoAvailable());
    const int redoSteps = doc->availableRedoSteps();
    setConcealCursor(fixture, source.size());
    waitForConcealPublication(fixture);
    QCOMPARE(doc->availableRedoSteps(), redoSteps);
    concealEditCompareRect(concealEditCursorRect(fixture, source.indexOf(QStringLiteral("tail"))),
                           compactTail);
    fixture.edit()->redo();
    QCOMPARE(fixture.text(), inserted);
    fixture.edit()->redo();
    QCOMPARE(fixture.text(), erased);
    QVERIFY(!doc->isRedoAvailable());
    const int undoSteps = doc->availableUndoSteps();
    setConcealCursor(fixture, erased.size());
    waitForConcealPublication(fixture);
    QCOMPARE(doc->availableUndoSteps(), undoSteps);
    concealEditCompareRect(concealEditCursorRect(fixture, erased.indexOf(QStringLiteral("tail"))),
                           compactTail);
  }

  // Prepare both references before the edit. The first geometry assertion is
  // synchronous: no event processing or fresh full parse can repair stale spans.
  for (int editPosition : {0, start + 7}) {
    QString edited = source;
    edited.insert(editPosition, QStringLiteral("Z"));
    QString editedUrl = url;
    if (editPosition >= start) {
      editedUrl.insert(editPosition - start, QStringLiteral("Z"));
    }
    QString editedCompact = edited;
    editedCompact.replace(edited.indexOf(editedUrl), editedUrl.size(), compactUrl);
    Fixture fullReference(edited, -1, -1, concealEditConfig(false));
    waitForConcealPublication(fullReference);
    renderConcealEditor(fullReference);
    const auto fullTail =
        concealEditCursorRect(fullReference, edited.indexOf(QStringLiteral("tail")));
    Fixture compactReference(editedCompact, -1, -1, concealEditConfig(false));
    waitForConcealPublication(compactReference);
    renderConcealEditor(compactReference);
    const auto compactTail =
        concealEditCursorRect(compactReference, editedCompact.indexOf(QStringLiteral("tail")));
    Fixture fixture(source, -1, -1, concealEditConfig());
    waitForConcealPublication(fixture);
    renderConcealEditor(fixture);
    auto doc = fixture.editor()->document();
    const int beforeRevision = doc->firstBlock().revision();
    const int outsideRevision = doc->lastBlock().revision();
    setConcealCursor(fixture, editPosition);
    QTest::keyClicks(fixture.edit(), QStringLiteral("Z"), Qt::NoModifier, 0);
    setConcealCursor(fixture, edited.size());
    QCOMPARE(fixture.text(), edited);
    QVERIFY(doc->firstBlock().revision() != beforeRevision);
    QCOMPARE(doc->lastBlock().revision(), outsideRevision);
    concealEditCompareRect(concealEditCursorRect(fixture, edited.indexOf(QStringLiteral("tail"))),
                           fullTail);
    const int editedRevision = doc->firstBlock().revision();
    const int undoSteps = doc->availableUndoSteps();
    waitForConcealPublication(fixture);
    concealEditCompareRect(concealEditCursorRect(fixture, edited.indexOf(QStringLiteral("tail"))),
                           compactTail);
    QCOMPARE(fixture.text(), edited);
    QCOMPARE(doc->firstBlock().revision(), editedRevision);
    QCOMPARE(doc->availableUndoSteps(), undoSteps);
  }

  {
    const QString styledSource =
        QStringLiteral("[x](") + url + QStringLiteral(") `syntax` tail\noutside");
    Fixture fixture(styledSource, -1, -1, concealEditConfig());
    waitForConcealPublication(fixture);
    const auto image = renderConcealEditor(fixture);
    const auto line = concealEditCursorRect(fixture, 0);
    const QRect band(0, line.top(), fixture.edit()->viewport()->width(), line.height());
    const auto baseline = concealEditCrop(image, band);
    const QColor original(QStringLiteral("#d7e6ff"));
    const QColor replacement(QStringLiteral("#f1d1f7"));
    const QColor syntax(QStringLiteral("#f5d8c5"));
    const QColor code(QStringLiteral("#ccf0d0"));
    QVERIFY(!concealEditColorBounds(baseline, original).isEmpty());
    QVERIFY(!concealEditColorBounds(baseline, syntax).isEmpty());
    QVERIFY(!concealEditColorBounds(baseline, code).isEmpty());
    auto doc = fixture.editor()->document();
    const int sourceRevision = doc->firstBlock().revision();
    const int outsideRevision = doc->lastBlock().revision();
    const int undoSteps = doc->availableUndoSteps();
    const int redoSteps = doc->availableRedoSteps();
    const auto tail = concealEditCursorRect(fixture, styledSource.indexOf(QStringLiteral("tail")));
    for (const QColor &background : {replacement, original, replacement, original}) {
      fixture.editor()->getHighlighter()->rehighlight();
      waitForConcealPublication(fixture);
      fixture.editor()->setConfig(concealEditConfig(true, background));
      waitForConcealPublication(fixture);
      const auto rendered = concealEditCrop(renderConcealEditor(fixture), band);
      concealEditCompareRect(
          concealEditCursorRect(fixture, styledSource.indexOf(QStringLiteral("tail"))), tail);
      QVERIFY(!concealEditColorBounds(rendered, background).isEmpty());
      QVERIFY(concealEditColorBounds(rendered, background == original ? replacement : original)
                  .isEmpty());
      QCOMPARE(concealEditColorBounds(rendered, syntax), concealEditColorBounds(baseline, syntax));
      QCOMPARE(concealEditColorBounds(rendered, code), concealEditColorBounds(baseline, code));
      if (background == original) {
        QCOMPARE(rendered, baseline);
      }
      QCOMPARE(fixture.text(), styledSource);
      QCOMPARE(doc->firstBlock().revision(), sourceRevision);
      QCOMPARE(doc->lastBlock().revision(), outsideRevision);
      QCOMPARE(doc->availableUndoSteps(), undoSteps);
      QCOMPARE(doc->availableRedoSteps(), redoSteps);
    }
    // The URL's original syntax format, not the last conceal overlay, returns.
    Fixture reference(styledSource, -1, -1, concealEditConfig(false));
    waitForConcealPublication(reference);
    renderConcealEditor(reference);
    setConcealCursor(fixture, start);
    const auto revealed = concealEditCrop(renderConcealEditor(fixture), band);
    const auto sourceSyntax = concealEditCrop(renderConcealEditor(reference), band);
    QVERIFY(concealEditColorBounds(revealed, original).isEmpty());
    QCOMPARE(concealEditColorBounds(revealed, syntax),
             concealEditColorBounds(sourceSyntax, syntax));
    QCOMPARE(concealEditColorBounds(revealed, code), concealEditColorBounds(sourceSyntax, code));
  }

  {
    const QString secondUrl = url.toUpper();
    QString geometrySource = QStringLiteral("[x](") + url + QStringLiteral(") [y](") + secondUrl +
                             QStringLiteral(") tailWWWW wrapping words and more trailing words "
                                            "to cross several visual lines\n");
    for (int row = 0; row < 24; ++row) {
      geometrySource +=
          QStringLiteral("row%1 WWWW wrapping text\n").arg(row, 2, 10, QLatin1Char('0'));
    }
    geometrySource += QStringLiteral("last WWWW");
    QString geometryCompact = geometrySource;
    geometryCompact.replace(url, compactUrl);
    geometryCompact.replace(secondUrl, compactUrl.toUpper());
    const int removed = geometrySource.size() - geometryCompact.size();
    const int tail = geometrySource.indexOf(QStringLiteral("tailWWWW"));
    const int referenceTail = geometryCompact.indexOf(QStringLiteral("tailWWWW"));
    Fixture reference(geometryCompact, 0, referenceTail, concealEditConfig(false));
    waitForConcealPublication(reference);
    renderConcealEditor(reference);
    Fixture fixture(geometrySource, 0, tail, concealEditConfig());
    waitForConcealPublication(fixture);
    renderConcealEditor(fixture);
    auto verifyTrailingGlyphs = [&](int p_sourcePosition, int p_referencePosition) {
      concealEditCompareRect(concealEditCursorRect(fixture, p_sourcePosition),
                             concealEditCursorRect(reference, p_referencePosition));
      const auto point = concealEditGlyphPoint(reference, p_referencePosition);
      QVERIFY(fixture.edit()->viewport()->rect().contains(point));
      QCOMPARE(reference.edit()->cursorForPosition(point).position(), p_referencePosition);
      QCOMPARE(fixture.edit()->cursorForPosition(point).position(), p_sourcePosition);
      const auto left = concealEditCursorRect(reference, p_referencePosition);
      const auto right = concealEditCursorRect(reference, p_referencePosition + 3);
      const QRect glyphBand(left.left(), left.top(), right.left() - left.left(), left.height());
      const auto referenceImage = concealEditCrop(renderConcealEditor(reference), glyphBand);
      const auto actualImage = concealEditCrop(renderConcealEditor(fixture), glyphBand);
      const auto expectedInk =
          concealEditColorBounds(referenceImage, QColor(QStringLiteral("#252525")));
      const auto actualInk = concealEditColorBounds(actualImage, QColor(QStringLiteral("#252525")));
      QVERIFY(!expectedInk.isEmpty());
      QVERIFY(!actualInk.isEmpty());
      concealEditCompareRect(actualInk, expectedInk);
    };
    verifyTrailingGlyphs(tail + 4, referenceTail + 4);
    concealEditCompareRect(fixture.edit()->cursorRect(), reference.edit()->cursorRect());

    // Twenty monospace cells put each three-dot marker away from a wrap edge.
    // The ordinary short-source reference may then use Qt's normal wrapping.
    const int cellWidth = concealEditCursorRect(reference, referenceTail + 5).left() -
                          concealEditCursorRect(reference, referenceTail + 4).left();
    for (auto item : {&fixture, &reference}) {
      item->editor()->resize(320, 220);
      item->edit()->setWordWrapMode(QTextOption::WrapAnywhere);
      item->edit()->setLineWrapMode(QTextEdit::FixedPixelWidth);
      item->edit()->setLineWrapColumnOrWidth(20 * cellWidth);
    }
    QCoreApplication::processEvents();
    setConcealCursor(fixture, tail + 4);
    setConcealCursor(reference, referenceTail + 4);
    fixture.edit()->ensureCursorVisible();
    reference.edit()->ensureCursorVisible();
    QVERIFY(fixture.editor()->document()->firstBlock().layout()->lineCount() >= 3);
    QCOMPARE(fixture.editor()->document()->firstBlock().layout()->lineCount(),
             reference.editor()->document()->firstBlock().layout()->lineCount());
    concealEditCompareRect(fixture.edit()->cursorRect(), reference.edit()->cursorRect());
    for (auto key : {Qt::Key_Down, Qt::Key_Up}) {
      QTest::keyClick(fixture.edit(), key);
      QTest::keyClick(reference.edit(), key);
      QCOMPARE(fixture.edit()->textCursor().position(),
               reference.edit()->textCursor().position() + removed);
      concealEditCompareRect(fixture.edit()->cursorRect(), reference.edit()->cursorRect());
    }
    QCOMPARE(fixture.edit()->textCursor().position(), tail + 4);
    QTest::keyClick(fixture.edit(), Qt::Key_End, Qt::ControlModifier);
    QTest::keyClick(reference.edit(), Qt::Key_End, Qt::ControlModifier);
    QCoreApplication::processEvents();
    QCOMPARE(fixture.edit()->textCursor().position(), geometrySource.size());
    QVERIFY(fixture.edit()->verticalScrollBar()->value() > 0);
    concealEditCompareRect(fixture.edit()->cursorRect(), reference.edit()->cursorRect());
    verifyTrailingGlyphs(geometrySource.lastIndexOf(QStringLiteral("WWWW")),
                         geometryCompact.lastIndexOf(QStringLiteral("WWWW")));
    concealEditSaveImage(renderConcealEditor(fixture), QStringLiteral("conceal-edit-scrolled.png"));
    QCOMPARE(fixture.text(), geometrySource);
  }

  {
    const QString composition = QStringLiteral("e\u0301");
    const int insertion = source.indexOf(QStringLiteral("tail")) + 2;
    QString committed = source;
    committed.insert(insertion, composition);
    QString committedCompact = committed;
    committedCompact.replace(url, compactUrl);
    Fixture compactReference(committedCompact, -1, -1, concealEditConfig(false));
    waitForConcealPublication(compactReference);
    renderConcealEditor(compactReference);
    const auto committedEnd = concealEditCursorRect(compactReference, compactReference.blockEnd(0));
    Fixture reference(source, 0, insertion, concealEditConfig(false));
    waitForConcealPublication(reference);
    renderConcealEditor(reference);
    Fixture fixture(source, 0, insertion, concealEditConfig());
    waitForConcealPublication(fixture);
    renderConcealEditor(fixture);
    auto doc = fixture.editor()->document();
    doc->clearUndoRedoStacks();
    const int sourceRevision = doc->firstBlock().revision();
    const int count = doc->characterCount();
    const auto compactEnd = concealEditCursorRect(fixture, fixture.blockEnd(0));
    for (auto item : {&fixture, &reference}) {
      QInputMethodEvent preedit(composition, QList<QInputMethodEvent::Attribute>());
      QCoreApplication::sendEvent(item->edit(), &preedit);
    }
    // Even with the source caret after the URL, preedit reveals its whole block.
    concealEditCompareRect(concealEditCursorRect(fixture, fixture.blockEnd(0)),
                           concealEditCursorRect(reference, reference.blockEnd(0)));
    concealEditCompareRect(fixture.edit()->cursorRect(), reference.edit()->cursorRect());
    QCOMPARE(fixture.text(), source);
    QCOMPARE(doc->characterCount(), count);
    QCOMPARE(doc->firstBlock().revision(), sourceRevision);
    QCOMPARE(fixture.edit()->textCursor().position(), insertion);
    QVERIFY(!doc->isUndoAvailable());
    concealEditSaveImage(renderConcealEditor(fixture), QStringLiteral("conceal-edit-preedit.png"));
    for (auto item : {&fixture, &reference}) {
      QInputMethodEvent cancel;
      QCoreApplication::sendEvent(item->edit(), &cancel);
    }
    concealEditCompareRect(concealEditCursorRect(fixture, fixture.blockEnd(0)), compactEnd);
    QCOMPARE(fixture.text(), source);
    QCOMPARE(doc->firstBlock().revision(), sourceRevision);
    QVERIFY(!doc->isUndoAvailable());
    for (auto item : {&fixture, &reference}) {
      QInputMethodEvent preedit(composition, QList<QInputMethodEvent::Attribute>());
      QCoreApplication::sendEvent(item->edit(), &preedit);
      QInputMethodEvent commit;
      commit.setCommitString(composition);
      QCoreApplication::sendEvent(item->edit(), &commit);
    }
    QCOMPARE(fixture.text(), committed);
    QCOMPARE(fixture.edit()->textCursor().position(), insertion + composition.size());
    QCOMPARE(doc->characterCount(), count + composition.size());
    concealEditCompareRect(concealEditCursorRect(fixture, fixture.blockEnd(0)),
                           concealEditCursorRect(reference, reference.blockEnd(0)));
    waitForConcealPublication(fixture);
    concealEditCompareRect(concealEditCursorRect(fixture, fixture.blockEnd(0)), committedEnd);
    QCOMPARE(fixture.text(), committed);
    fixture.edit()->undo();
    QCOMPARE(fixture.text(), source);
    QVERIFY(!doc->isUndoAvailable());
    fixture.edit()->redo();
    QCOMPARE(fixture.text(), committed);
    QVERIFY(!doc->isRedoAvailable());
  }
}

QTEST_MAIN(tests::TestMarkdownEditor)
