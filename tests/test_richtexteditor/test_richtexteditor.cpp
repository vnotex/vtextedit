#include "test_richtexteditor.h"

#include <QApplication>
#include <QGuiApplication>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QSignalSpy>
#include <QStyleHints>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextTable>
#include <QVBoxLayout>

#include <inputmode/abstractinputmode.h>
#include <vtextedit/markdowneditorconfig.h>
#include <vtextedit/richtexteditorconfig.h>
#include <vtextedit/vmarkdowneditor.h>
#include <vtextedit/vrichtexteditor.h>
#include <vtextedit/vtextedit.h>

using namespace tests;
using namespace vte;

namespace {
// createMimeDataFromSelection() is protected API; a subclass is the supported
// way to reach it.
class ExposedTextEdit : public VTextEdit {
public:
  using VTextEdit::createMimeDataFromSelection;
};

QSharedPointer<RichTextEditorConfig> configWithMode(InputMode p_mode) {
  auto config = QSharedPointer<RichTextEditorConfig>::create();
  config->m_inputMode = p_mode;
  return config;
}

// Own the published status widget separately from the host, including on assertion failure.
struct ViSearchHost {
  QWidget m_host;
  VRichTextEditor *m_editor;
  VTextEdit *m_edit;
  QSharedPointer<QWidget> m_status;

  explicit ViSearchHost(const QString &p_source) {
    auto layout = new QVBoxLayout(&m_host);
    m_editor = new VRichTextEditor(configWithMode(InputMode::ViMode), &m_host);
    m_edit = m_editor->getTextEdit();
    m_edit->setPlainText(p_source);
    layout->addWidget(m_editor);
    m_status = m_editor->inputModeStatusWidget();
    layout->addWidget(m_status.data());
    m_status->show();
    m_host.show();
    m_host.activateWindow();
    m_edit->setFocus();
  }

  ~ViSearchHost() { m_status->setParent(nullptr); }

  void setPosition(int p_position) {
    auto cursor = m_edit->textCursor();
    cursor.setPosition(p_position);
    m_edit->setTextCursor(cursor);
  }

  bool search(const QString &p_keys, const QString &p_pattern) {
    QTest::keyClicks(m_edit, p_keys);
    auto prompt = qobject_cast<QLineEdit *>(QApplication::focusWidget());
    if (!prompt || !prompt->isVisible() ||
        prompt->objectName() != QStringLiteral("CommandText.EmulatedCommandBar.KateVi")) {
      return false;
    }
    QTest::keyClicks(prompt, p_pattern);
    return prompt->hasFocus();
  }

  bool close(int p_key = Qt::Key_Return, Qt::KeyboardModifiers p_modifiers = Qt::NoModifier) {
    QTest::keyClick(QApplication::focusWidget(), static_cast<Qt::Key>(p_key), p_modifiers);
    return m_edit->hasFocus();
  }

  int position() const { return m_edit->textCursor().position(); }
};

// A short document with a bold run and a bullet list.
const char *c_html = "<p>plain <b>bold</b> tail</p>"
                     "<ul><li>alpha</li><li>beta</li></ul>";

bool isBoldAt(QTextDocument *p_doc, int p_position) {
  QTextCursor cursor(p_doc);
  cursor.setPosition(p_position);
  return cursor.charFormat().fontWeight() > QFont::Normal;
}
} // namespace

void TestRichTextEditor::testSearchAcrossParagraphs() {
  VRichTextEditor editor;
  editor.setHtml(QStringLiteral("<p><b>alpha</b></p><p>beta</p><p>gamma</p>"));
  auto edit = editor.getTextEdit();
  const auto html = editor.document()->toHtml();
  const int revision = editor.document()->revision();
  const auto regex = edit->findAllText(QStringLiteral("alpha\\nbeta"), FindFlag::RegularExpression);
  QCOMPARE(regex.size(), 1);
  QCOMPARE(regex[0].selectionStart(), 0);
  QCOMPARE(regex[0].selectionEnd(), 10);
  for (const auto &query : {QStringLiteral("alpha\nbeta"), QStringLiteral("alpha\r\nbeta"),
                            QStringLiteral("alpha\rbeta")}) {
    const auto literal = edit->findAllText(query, FindFlag::None);
    QCOMPARE(literal.size(), 1);
    QCOMPARE(literal[0].selectionStart(), 0);
    QCOMPARE(literal[0].selectionEnd(), 10);
  }
  QVERIFY(edit->findAllText(QStringLiteral("alpha\\nbeta"), FindFlag::None).isEmpty());
  QCOMPARE(editor.document()->toHtml(), html);
  QCOMPARE(editor.document()->revision(), revision);

  VTextEdit source;
  const auto nbsp = QStringLiteral("a\u00a0b");
  source.setPlainText(nbsp);
  const auto matches = source.findAllText(nbsp, FindFlag::None);
  QCOMPARE(matches.size(), 1);
  QCOMPARE(matches[0].selectedText(), nbsp);
  QVERIFY(source.findAllText(QStringLiteral("a b"), FindFlag::None).isEmpty());
  source.setPlainText(QStringLiteral("a\u2028b"));
  QCOMPARE(source.findAllText(QStringLiteral("a\nb"), FindFlag::None).size(), 1);
}

void TestRichTextEditor::testSearchRegexOptions() {
  VTextEdit edit;
  edit.setPlainText(QStringLiteral("alpha\nbeta"));
  const auto flags = FindFlag::RegularExpression;
  const auto anchor = edit.findAllText(QStringLiteral("^beta$"), flags);
  QCOMPARE(anchor.size(), 1);
  QCOMPARE(anchor[0].selectionStart(), 6);
  QCOMPARE(anchor[0].selectionEnd(), 10);
  QVERIFY(edit.findAllText(QStringLiteral("alpha.*beta"), flags).isEmpty());
  const auto dotAll = edit.findAllText(QStringLiteral("(?s)alpha.*beta"), flags);
  QCOMPARE(dotAll.size(), 1);
  QCOMPARE(dotAll[0].selectionStart(), 0);
  QCOMPARE(dotAll[0].selectionEnd(), 10);
  QCOMPARE(edit.findAllText(QStringLiteral("ALPHA"), flags).size(), 1);
  QVERIFY(edit.findAllText(QStringLiteral("ALPHA"), flags | FindFlag::CaseSensitive).isEmpty());
  QVERIFY(edit.findAllText(QStringLiteral("(?-i)ALPHA"), flags).isEmpty());
  QVERIFY(edit.findAllText(QStringLiteral("["), flags).isEmpty());
  edit.setPlainText(QStringLiteral("xcaty (cat) _cat_"));
  const auto words = edit.findAllText(QStringLiteral("cat"), flags | FindFlag::WholeWordOnly);
  QCOMPARE(words.size(), 2);
  QCOMPARE(words[0].selectionStart(), 7);
  QCOMPARE(words[1].selectionStart(), 13);
}

void TestRichTextEditor::testSearchRangesAndZeroLengthMatches() {
  VTextEdit edit;
  edit.setPlainText(QStringLiteral("A\nB\nC"));
  const auto flags = FindFlag::RegularExpression;
  QCOMPARE(edit.findAllText(QStringLiteral("A\\nB"), flags, 0, 3).size(), 1);
  QVERIFY(edit.findAllText(QStringLiteral("A\\nB"), flags, 0, 2).isEmpty());
  QList<QRegularExpressionMatch> captures;
  const auto behind =
      edit.findAllText(QStringLiteral("(?<=A\\n)(B)(?=\\nC)"), flags, 2, 3, &captures);
  QCOMPARE(behind.size(), 1);
  QCOMPARE(behind[0].selectionStart(), 2);
  QCOMPARE(behind[0].selectionEnd(), 3);
  QCOMPARE(captures.size(), 1);
  QCOMPARE(captures[0].captured(1), QStringLiteral("B"));
  const auto eof = edit.findAllText(QStringLiteral("\\z"), flags);
  QCOMPARE(eof.size(), 1);
  QVERIFY(!eof[0].isNull());
  QCOMPARE(eof[0].selectionStart(), 5);
  QCOMPARE(eof[0].selectionEnd(), 5);
  QVERIFY(edit.findAllText(QStringLiteral("\\z"), flags, 0, 5).isEmpty());
  QCOMPARE(edit.findAllText(QStringLiteral("C"), flags, 0, 99).size(), 1);
  for (const auto &range : {QPair<int, int>(-1, -1), {6, -1}, {0, -2}, {3, 3}, {3, 2}}) {
    QVERIFY(edit.findAllText(QStringLiteral("."), flags, range.first, range.second, &captures)
                .isEmpty());
    QVERIFY(captures.isEmpty());
  }
  edit.findAllText(QStringLiteral("(.)"), flags, 0, -1, &captures);
  edit.findAllText(QStringLiteral("A"), FindFlag::None, 0, -1, &captures);
  QVERIFY(captures.isEmpty());
  edit.findAllText(QStringLiteral("(.)"), flags, 0, -1, &captures);
  QVERIFY(edit.findAllText(QStringLiteral("["), flags, 0, -1, &captures).isEmpty());
  QVERIFY(captures.isEmpty());
  edit.clear();
  const auto empty = edit.findAllText(QStringLiteral("^"), flags);
  QCOMPARE(empty.size(), 1);
  QVERIFY(!empty[0].isNull());
  QCOMPARE(empty[0].position(), 0);
  QVERIFY(edit.findAllText(QString(), flags).isEmpty());
  edit.setPlainText(QString::fromUtf8("\xf0\x9f\x98\x80x"));
  const auto unicode = edit.findAllText(QStringLiteral("^|."), flags);
  QCOMPARE(unicode.size(), 2);
  QCOMPARE(unicode[0].selectionStart(), 0);
  QCOMPARE(unicode[0].selectionEnd(), 0);
  QCOMPARE(unicode[1].selectionStart(), 2);
  QCOMPARE(unicode[1].selectionEnd(), 3);
}

void TestRichTextEditor::testSearchForwardBackwardWrap() {
  VTextEdit edit;
  edit.setPlainText(QStringLiteral("A\nB\nA\nB"));
  const auto flags = FindFlag::RegularExpression;
  const auto pattern = QStringLiteral("A\\nB");
  const auto matches = edit.findAllText(pattern, flags | FindFlag::FindBackward);
  QCOMPARE(matches.size(), 2);
  QCOMPARE(matches[0].selectionStart(), 0);
  QCOMPARE(matches[1].selectionStart(), 4);
  QCOMPARE(edit.findText(pattern, flags, 1).selectionStart(), 4);
  QCOMPARE(edit.findText(pattern, flags, 7).selectionStart(), 0);
  QCOMPARE(edit.findText(pattern, flags | FindFlag::FindBackward, 4).selectionStart(), 0);
  QCOMPARE(edit.findText(pattern, flags | FindFlag::FindBackward, 0).selectionStart(), 4);
  QVERIFY(edit.findText(pattern, flags, -1).isNull());
  QVERIFY(edit.findText(pattern, flags, 8).isNull());
  const auto eof = edit.findText(QStringLiteral("\\z"), flags | FindFlag::FindBackward, 0);
  QVERIFY(!eof.isNull());
  QCOMPARE(eof.position(), 7);
  QCOMPARE(edit.findText(QStringLiteral("\\z"), flags, 7).position(), 7);
}

void TestRichTextEditor::testFoldKeysFallThroughWithoutFolding() {
  VRichTextEditor editor(configWithMode(InputMode::ViMode));
  editor.setHtml(QString::fromUtf8(c_html));
  auto edit = editor.getTextEdit();

  const auto before = editor.document()->toPlainText();

  // Vi normal mode: zc/zo are fold commands. With no text folding they must be
  // ignored rather than swallowed into an edit.
  QTest::keyClicks(edit, QStringLiteral("zc"));
  QTest::keyClicks(edit, QStringLiteral("zo"));

  QCOMPARE(editor.document()->toPlainText(), before);
  QCOMPARE(editor.getEditorMode(), EditorMode::ViModeNormal);
}

void TestRichTextEditor::testCompletionKeysDoNothing() {
  VRichTextEditor editor(configWithMode(InputMode::ViMode));
  editor.setHtml(QString::fromUtf8(c_html));
  auto edit = editor.getTextEdit();

  // Enter insert mode, then ask for a completion. There is no completer, so
  // nothing may be inserted.
  QTest::keyClick(edit, Qt::Key_I);
  QCOMPARE(editor.getEditorMode(), EditorMode::ViModeInsert);

  const auto before = editor.document()->toPlainText();
  QTest::keyClick(edit, Qt::Key_P, Qt::ControlModifier);
  QTest::keyClick(edit, Qt::Key_N, Qt::ControlModifier);
  QCOMPARE(editor.document()->toPlainText(), before);

  QTest::keyClick(edit, Qt::Key_Escape);
  QCOMPARE(editor.getEditorMode(), EditorMode::ViModeNormal);
}

void TestRichTextEditor::testSetInputModeSwitchesAndIsIdempotent() {
  VRichTextEditor editor;
  QCOMPARE(editor.getInputMode()->mode(), InputMode::NormalMode);

  QSignalSpy modeSpy(&editor, &VRichTextEditor::modeChanged);

  editor.setInputMode(InputMode::VscodeMode);
  QCOMPARE(editor.getInputMode()->mode(), InputMode::VscodeMode);
  QCOMPARE(modeSpy.count(), 1);

  // Idempotent: no mode object is created, no signal.
  editor.setInputMode(InputMode::VscodeMode);
  QCOMPARE(modeSpy.count(), 1);

  modeSpy.clear();
  editor.setInputMode(InputMode::ViMode);
  QCOMPARE(editor.getInputMode()->mode(), InputMode::ViMode);
  // Exactly one host emission per call: entering Vi normal mode is reported by
  // the interface relay before the final host emission, and the relay is
  // suppressed because the mode did not change afterwards.
  QCOMPARE(modeSpy.count(), 1);
  QCOMPARE(editor.getEditorMode(), EditorMode::ViModeNormal);

  modeSpy.clear();
  editor.setInputMode(InputMode::NormalMode);
  QCOMPARE(modeSpy.count(), 1);
}

void TestRichTextEditor::testLineMappingIsIdentity() {
  VRichTextEditor editor(configWithMode(InputMode::ViMode));
  editor.setHtml(QStringLiteral("<p>one</p><p>two</p><p>three</p><p>four</p>"));
  auto edit = editor.getTextEdit();
  auto doc = editor.document();
  QCOMPARE(doc->blockCount(), 4);

  auto cursor = edit->textCursor();
  cursor.movePosition(QTextCursor::Start);
  edit->setTextCursor(cursor);

  // G/gg go through lineToVisibleLine()/visibleLineToLine(); without folding
  // the mapping is the identity, so they must land on the real last/first
  // block.
  QTest::keyClicks(edit, QStringLiteral("G"));
  QCOMPARE(edit->textCursor().blockNumber(), 3);

  QTest::keyClicks(edit, QStringLiteral("gg"));
  QCOMPARE(edit->textCursor().blockNumber(), 0);
}

void TestRichTextEditor::testStatusWidgetEmissionsFollowTheTransitions() {
  VRichTextEditor editor;
  QSignalSpy spy(&editor, &VRichTextEditor::inputModeStatusWidgetChanged);

  // Normal <-> VSCode: neither mode publishes a status widget.
  editor.setInputMode(InputMode::VscodeMode);
  QCOMPARE(spy.count(), 0);
  editor.setInputMode(InputMode::NormalMode);
  QCOMPARE(spy.count(), 0);

  // non-Vi -> Vi: exactly one non-null emission.
  editor.setInputMode(InputMode::ViMode);
  QCOMPARE(spy.count(), 1);
  QVERIFY(!spy.at(0).at(0).value<QSharedPointer<QWidget>>().isNull());
  QVERIFY(!editor.inputModeStatusWidget().isNull());

  // Vi -> non-Vi: exactly one null emission, from the detach.
  spy.clear();
  editor.setInputMode(InputMode::NormalMode);
  QCOMPARE(spy.count(), 1);
  QVERIFY(spy.at(0).at(0).value<QSharedPointer<QWidget>>().isNull());
  QVERIFY(editor.inputModeStatusWidget().isNull());
}

void TestRichTextEditor::testDestructionWithAMountedViStatusWidget() {
  QWidget host;
  auto layout = new QVBoxLayout(&host);

  {
    VRichTextEditor editor(configWithMode(InputMode::ViMode), &host);
    layout->addWidget(&editor);

    auto statusWidget = editor.inputModeStatusWidget();
    QVERIFY(!statusWidget.isNull());
    // Mount it, the way a host status bar does.
    layout->addWidget(statusWidget.data());
    QVERIFY(statusWidget->parent() != nullptr);

    // Switching away must unmount it before the mode object dies.
    editor.setInputMode(InputMode::NormalMode);
    QVERIFY(statusWidget->parent() == nullptr);
  }

  // And a destruction directly in Vi mode.
  {
    auto editor = new VRichTextEditor(configWithMode(InputMode::ViMode), &host);
    layout->addWidget(editor);
    auto statusWidget = editor->inputModeStatusWidget();
    QVERIFY(!statusWidget.isNull());
    layout->addWidget(statusWidget.data());

    delete editor;
    QVERIFY(statusWidget->parent() == nullptr);
  }
}

void TestRichTextEditor::testConstructedInViMode() {
  VRichTextEditor editor(configWithMode(InputMode::ViMode));

  QCOMPARE(editor.getInputMode()->mode(), InputMode::ViMode);
  QCOMPARE(editor.getEditorMode(), EditorMode::ViModeNormal);

  // The status widget is available right away.
  QVERIFY(!editor.inputModeStatusWidget().isNull());

  // The input method is already disabled in Vi normal mode.
  QCOMPARE(editor.getTextEdit()->inputMethodQuery(Qt::ImEnabled).toBool(), false);
}

void TestRichTextEditor::testFocusTransitionsFireOncePerConsumer() {
  QWidget host;
  auto layout = new QVBoxLayout(&host);
  auto other = new QWidget(&host);
  other->setFocusPolicy(Qt::StrongFocus);
  auto editor = new VRichTextEditor(configWithMode(InputMode::ViMode), &host);
  layout->addWidget(other);
  layout->addWidget(editor);

  host.show();
  QVERIFY(QTest::qWaitForWindowExposed(&host));

  QSignalSpy inSpy(editor, &VRichTextEditor::focusIn);
  QSignalSpy outSpy(editor, &VRichTextEditor::focusOut);

  // All three consumers ride the same two TextEditInputMode signals: the
  // public focusIn()/focusOut() asserted here, AbstractInputMode::focusIn/Out
  // (observed below through the Vi cursor blinking side effect) and the KateVi
  // command-response callbacks registered through connectFocusIn/Out(). One
  // emission is therefore exactly one fire for each of them.
  other->setFocus();
  QTRY_COMPARE(other->hasFocus(), true);

  editor->setFocus();
  QTRY_COMPARE(inSpy.count(), 1);
  QCOMPARE(outSpy.count(), 0);

  // Vi normal mode suppresses the cursor blinking while it has the focus.
  QCOMPARE(QGuiApplication::styleHints()->cursorFlashTime(), 0);

  other->setFocus();
  QTRY_COMPARE(outSpy.count(), 1);
  QCOMPARE(inSpy.count(), 1);

  // And restores the application flash time on focus out.
  QVERIFY(QGuiApplication::styleHints()->cursorFlashTime() > 0);
}

void TestRichTextEditor::testViSearchDirectionsCountsAndRepeat() {
  for (bool readOnly : {false, true}) {
    const QString source = QStringLiteral("one two one\none");
    ViSearchHost host(source);
    QVERIFY(QTest::qWaitForWindowExposed(&host.m_host));
    host.m_edit->setReadOnly(readOnly);
    const auto html = host.m_edit->toHtml();
    const int revision = host.m_edit->document()->revision();
    QVERIFY(host.search("/", "one"));
    QCOMPARE(host.position(), 8);
    QVERIFY(host.close());
    QCOMPARE(host.position(), 8);
    QTest::keyClicks(host.m_edit, "n");
    QCOMPARE(host.position(), 12);
    QTest::keyClicks(host.m_edit, "n");
    QCOMPARE(host.position(), 0);
    QTest::keyClicks(host.m_edit, "N");
    QCOMPARE(host.position(), 12);
    QVERIFY(host.search("?", "one"));
    QCOMPARE(host.position(), 8);
    QVERIFY(host.close());
    QTest::keyClicks(host.m_edit, "n");
    QCOMPARE(host.position(), 0);
    QTest::keyClicks(host.m_edit, "N");
    QCOMPARE(host.position(), 8);
    host.setPosition(0);
    QVERIFY(host.search("2/", "one"));
    QCOMPARE(host.position(), 12);
    QVERIFY(host.close());
    QVERIFY(host.search("/", ""));
    QVERIFY(host.close());
    QCOMPARE(host.position(), 0);
    QVERIFY(host.search("/", "one"));
    QVERIFY(host.close(Qt::Key_Escape));
    QCOMPARE(host.position(), 0);
    QVERIFY(host.search("/", "one"));
    QVERIFY(host.close());
    QCOMPARE(host.position(), 8);
    QCOMPARE(host.m_edit->toPlainText(), source);
    QCOMPARE(host.m_edit->toHtml(), html);
    QCOMPARE(host.m_edit->document()->revision(), revision);
  }
}

void TestRichTextEditor::testViSearchCancelAndInvalidPattern() {
  ViSearchHost host(QStringLiteral("one two one\none"));
  QVERIFY(QTest::qWaitForWindowExposed(&host.m_host));
  QVERIFY(host.search("/", "one"));
  QVERIFY(host.close());
  QCOMPARE(host.position(), 8);
  QVERIFY(host.search("/", "two"));
  QCOMPARE(host.position(), 4);
  QVERIFY(host.close(Qt::Key_Escape));
  QCOMPARE(host.position(), 8);
  QTest::keyClicks(host.m_edit, "n");
  QCOMPARE(host.position(), 12);
  for (int key : {Qt::Key_C, Qt::Key_BracketLeft}) {
    QVERIFY(host.search("/", "two"));
    QCOMPARE(host.position(), 4);
    QVERIFY(host.close(key, Qt::ControlModifier));
    QCOMPARE(host.position(), 12);
  }
  QVERIFY(host.search("/", ""));
  QVERIFY(host.close(Qt::Key_Backspace));
  QCOMPARE(host.position(), 12);
  QTest::keyClicks(host.m_edit, "n");
  QCOMPARE(host.position(), 0);

  const auto original = host.m_edit->toPlainText();
  for (const auto &pattern : {QStringLiteral("["), QStringLiteral("absent")}) {
    host.setPosition(0);
    QVERIFY(host.search("d/", pattern));
    auto feedback = host.m_status->findChild<QLabel *>(
        QStringLiteral("CommandResponseMessage.EmulatedCommandBar.KateVi"));
    QVERIFY(feedback && feedback->isVisible());
    QVERIFY(host.close());
    QCOMPARE(host.position(), 0);
    QCOMPARE(host.m_edit->toPlainText(), original);
    QTest::keyClicks(host.m_edit, "n");
    QCOMPARE(host.position(), pattern == QStringLiteral("[") ? 8 : 0);
    host.setPosition(0);
    QTest::keyClicks(host.m_edit, "x");
    QCOMPARE(host.m_edit->toPlainText(), original.mid(1));
    QTest::keyClicks(host.m_edit, "u");
    QCOMPARE(host.m_edit->toPlainText(), original);
  }

  ViSearchHost fresh(QStringLiteral("a/b"));
  QVERIFY(QTest::qWaitForWindowExposed(&fresh.m_host));
  QVERIFY(fresh.search("/", ""));
  QVERIFY(fresh.close());
  QCOMPARE(fresh.position(), 0);
  QCOMPARE(fresh.m_edit->getInputMode()->editorMode(), EditorMode::ViModeNormal);
  QTest::keyClicks(fresh.m_edit, "f/");
  QCOMPARE(fresh.position(), 1);
  QVERIFY(fresh.m_edit->hasFocus());
  QTest::keyClicks(fresh.m_edit, "r?");
  QCOMPARE(fresh.m_edit->toPlainText(), QStringLiteral("a?b"));
  QVERIFY(fresh.m_edit->hasFocus());
  QTest::keyClicks(fresh.m_edit, "i/?");
  QCOMPARE(fresh.m_edit->toPlainText(), QStringLiteral("a/??b"));
  QCOMPARE(fresh.m_edit->getInputMode()->editorMode(), EditorMode::ViModeInsert);
}

void TestRichTextEditor::testViSearchRegexBoundaries() {
  ViSearchHost host(QStringLiteral("alpha ALPHA Alpha"));
  QVERIFY(QTest::qWaitForWindowExposed(&host.m_host));
  QVERIFY(host.search("/", "alpha"));
  QCOMPARE(host.position(), 6);
  QVERIFY(host.close());
  host.setPosition(0);
  QVERIFY(host.search("/", "Alpha"));
  QCOMPARE(host.position(), 12);
  QVERIFY(host.close());

  host.m_edit->setPlainText(QStringLiteral("xx\nalpha\nbeta\nalpha\nbeta"));
  host.setPosition(10); // line 2, column 1, inside the multiline match.
  QVERIFY(host.search("?", QStringLiteral("alpha\\nbeta")));
  QCOMPARE(host.m_edit->textCursor().blockNumber(), 1);
  QCOMPARE(host.m_edit->textCursor().positionInBlock(), 0);
  QVERIFY(host.close());

  const QString anchors = QStringLiteral("a\nb\n");
  host.m_edit->setPlainText(anchors);
  const int revision = host.m_edit->document()->revision();
  QVERIFY(host.search("/", "^"));
  QCOMPARE(host.position(), 2);
  QVERIFY(host.close());
  // Qt/PCRE excludes the terminal-newline EOF from ^; \z names that boundary explicitly.
  QTest::keyClicks(host.m_edit, "n");
  QCOMPARE(host.position(), 0);
  QTest::keyClicks(host.m_edit, "N");
  QCOMPARE(host.position(), 2);
  host.setPosition(0);
  QVERIFY(host.search("/", QStringLiteral("^|\\z")));
  QCOMPARE(host.position(), 2);
  QVERIFY(host.close());
  QTest::keyClicks(host.m_edit, "n");
  QCOMPARE(host.position(), 4);
  QTest::keyClicks(host.m_edit, "N");
  QCOMPARE(host.position(), 2);
  QTest::keyClicks(host.m_edit, "1000000n");
  QCOMPARE(host.position(), 4);
  QCOMPARE(host.m_edit->toPlainText(), anchors);
  QCOMPARE(host.m_edit->document()->revision(), revision);

  host.m_edit->setPlainText(QStringLiteral("a\nb"));
  QVERIFY(host.search("/", "$"));
  QCOMPARE(host.position(), 1);
  QVERIFY(host.close());
  QCOMPARE(host.position(), 1);
  QTest::keyClicks(host.m_edit, "n");
  QCOMPARE(host.position(), 3);
  host.setPosition(0);
  QVERIFY(host.search("/", QStringLiteral("\\z")));
  QCOMPARE(host.position(), host.m_edit->document()->characterCount() - 1);
  QVERIFY(host.close());

  host.m_edit->setPlainText(QString());
  const int emptyRevision = host.m_edit->document()->revision();
  QVERIFY(host.search("/", "^"));
  QVERIFY(host.close());
  QCOMPARE(host.position(), 0);
  QCOMPARE(host.m_edit->toPlainText(), QString());
  QCOMPARE(host.m_edit->document()->revision(), emptyRevision);

  const QString unicode = QStringLiteral("x\U0001F600needle");
  host.m_edit->setPlainText(unicode);
  QVERIFY(host.search("/", "needle"));
  QCOMPARE(host.m_edit->textCursor().positionInBlock(), 3);
  QVERIFY(host.close());
  QCOMPARE(host.m_edit->toPlainText(), unicode);
}

void TestRichTextEditor::testViSearchComposesWithOperatorsAndVisualMode() {
  const QString source = QStringLiteral("zero one two one");
  ViSearchHost host(source);
  QVERIFY(QTest::qWaitForWindowExposed(&host.m_host));
  QVERIFY(host.search("d/", "one"));
  QCOMPARE(host.m_edit->toPlainText(), source); // Preview never edits.
  QVERIFY(host.close());
  QCOMPARE(host.m_edit->toPlainText(), QStringLiteral("one two one"));
  QTest::keyClicks(host.m_edit, ".");
  QCOMPARE(host.m_edit->toPlainText(), QStringLiteral("one"));
  QTest::keyClicks(host.m_edit, "u");
  QCOMPARE(host.m_edit->toPlainText(), QStringLiteral("one two one"));
  QTest::keyClicks(host.m_edit, "u");
  QCOMPARE(host.m_edit->toPlainText(), source);

  for (bool cancel : {true, false}) {
    host.setPosition(0);
    QVERIFY(host.search("d/", cancel ? QStringLiteral("one") : QStringLiteral("absent")));
    QVERIFY(host.close(cancel ? Qt::Key_Escape : Qt::Key_Return));
    QCOMPARE(host.m_edit->toPlainText(), source);
    QCOMPARE(host.position(), 0);
    QTest::keyClicks(host.m_edit, "x");
    QCOMPARE(host.m_edit->toPlainText(), source.mid(1));
    QTest::keyClicks(host.m_edit, "u");
    QCOMPARE(host.m_edit->toPlainText(), source);
  }

  host.m_edit->setPlainText(QStringLiteral("one two one tail"));
  host.setPosition(8);
  QVERIFY(host.search("c?", "one"));
  QVERIFY(host.close());
  QCOMPARE(host.m_edit->toPlainText(), QStringLiteral("one tail"));
  QCOMPARE(host.position(), 0);
  QCOMPARE(host.m_edit->getInputMode()->editorMode(), EditorMode::ViModeInsert);
  QTest::keyClicks(host.m_edit, "X");
  QCOMPARE(host.m_edit->toPlainText(), QStringLiteral("Xone tail"));
  QTest::keyClick(host.m_edit, Qt::Key_Escape);

  host.m_edit->setPlainText(source);
  for (bool reversed : {false, true}) {
    host.setPosition(reversed ? 12 : 1);
    QTest::keyClicks(host.m_edit, reversed ? "vhh" : "vl");
    const auto selection = host.m_edit->getSelection();
    const int position = host.position();
    QVERIFY(selection.end() > selection.start());
    QVERIFY(host.search(reversed ? "?" : "/", "one"));
    QVERIFY(host.position() != position);
    QVERIFY(host.close(Qt::Key_Escape));
    QCOMPARE(host.position(), position);
    QCOMPARE(host.m_edit->getSelection().start(), selection.start());
    QCOMPARE(host.m_edit->getSelection().end(), selection.end());
    QCOMPARE(host.m_edit->getInputMode()->editorMode(), EditorMode::ViModeVisual);
    QVERIFY(host.search(reversed ? "?" : "/", "one"));
    QVERIFY(host.close());
    QCOMPARE(host.position(), 5);
    QCOMPARE(host.m_edit->getSelection().start(), reversed ? 5 : selection.start());
    QCOMPARE(host.m_edit->getSelection().end(), reversed ? selection.end() : 6);
    QCOMPARE(host.m_edit->getInputMode()->editorMode(), EditorMode::ViModeVisual);
    QCOMPARE(host.m_edit->toPlainText(), source);
    QTest::keyClick(host.m_edit, Qt::Key_Escape);
  }
}

void TestRichTextEditor::testViSearchRevealsFoldedSource() {
  for (bool interior : {false, true}) {
    QWidget host;
    auto layout = new QVBoxLayout(&host);
    auto config = QSharedPointer<TextEditorConfig>::create();
    config->m_inputMode = InputMode::ViMode;
    auto markdownConfig = QSharedPointer<MarkdownEditorConfig>::create(config);
    auto parameters = QSharedPointer<TextEditorParameters>::create();
    parameters->m_spellCheckEnabled = false;
    auto editor = new VMarkdownEditor(markdownConfig, parameters, &host);
    layout->addWidget(editor);
    auto status = editor->statusWidget();
    layout->addWidget(status.data());
    status->show();
    struct Unmount {
      QWidget *m_widget;
      ~Unmount() { m_widget->setParent(nullptr); }
    } unmount{status.data()};
    // Folding keeps the final block visible; the second fixture moves that boundary
    // past needle so reaching line 4 must open BOTH enclosing folds.
    const QString source =
        interior
            ? QStringLiteral("# Outer\nanchor\n## Inner\nhidden\nneedle\nboundary\n# End\nneedle")
            : QStringLiteral("# Outer\nanchor\n## Inner\nhidden\nneedle\n# End\nneedle");
    editor->setText(source);
    auto edit = editor->getTextEdit();
    host.resize(800, 600);
    host.show();
    host.activateWindow();
    edit->setFocus();
    QVERIFY(QTest::qWaitForWindowExposed(&host));
    QTextCursor cursor(edit->document()->findBlockByNumber(2));
    edit->setTextCursor(cursor);
    // foldAtCursor() also returns true before parsing has produced any ranges.
    QVERIFY(QTest::qWaitFor(
        [editor, edit]() {
          editor->foldAtCursor();
          return !edit->document()->findBlockByNumber(3).isVisible();
        },
        5000));
    cursor.setPosition(0);
    edit->setTextCursor(cursor);
    QVERIFY(editor->foldAtCursor());
    QCOMPARE(edit->document()->findBlockByNumber(4).isVisible(), !interior);
    QTest::keyClicks(edit, "/");
    auto prompt = qobject_cast<QLineEdit *>(QApplication::focusWidget());
    QVERIFY(prompt && prompt->isVisible());
    QCOMPARE(prompt->objectName(), QStringLiteral("CommandText.EmulatedCommandBar.KateVi"));
    QTest::keyClicks(prompt, "needle");
    QCOMPARE(edit->textCursor().blockNumber(), 4);
    QCOMPARE(edit->textCursor().positionInBlock(), 0);
    QVERIFY(edit->document()->findBlockByNumber(4).isVisible());
    if (interior) {
      QVERIFY(edit->document()->findBlockByNumber(3).isVisible());
    }
    QTest::keyClick(prompt, Qt::Key_Return);
    QVERIFY(edit->hasFocus());
    QCOMPARE(edit->textCursor().blockNumber(), 4);
    QTest::keyClicks(edit, "n");
    QCOMPARE(edit->textCursor().blockNumber(), interior ? 7 : 6);
    QCOMPARE(edit->textCursor().positionInBlock(), 0);
    QCOMPARE(editor->getText(), source);
  }
}

void TestRichTextEditor::testFocusThroughTheViCommandBar() {
  QWidget host;
  auto layout = new QVBoxLayout(&host);
  auto other = new QWidget(&host);
  other->setFocusPolicy(Qt::StrongFocus);
  layout->addWidget(other);
  auto editor = new VRichTextEditor(configWithMode(InputMode::ViMode), &host);
  layout->addWidget(editor);

  auto statusWidget = editor->inputModeStatusWidget();
  QVERIFY(!statusWidget.isNull());
  layout->addWidget(statusWidget.data());

  host.show();
  QVERIFY(QTest::qWaitForWindowExposed(&host));

  other->setFocus();
  QTRY_COMPARE(other->hasFocus(), true);

  QSignalSpy inSpy(editor, &VRichTextEditor::focusIn);
  QSignalSpy outSpy(editor, &VRichTextEditor::focusOut);

  auto edit = editor->getTextEdit();
  edit->setFocus();
  QTRY_COMPARE(inSpy.count(), 1);

  // Open the Vi command bar; the editor loses the focus to it.
  QTest::keyClicks(edit, QStringLiteral(":"));
  QTRY_COMPARE(outSpy.count(), 1);
  QCOMPARE(inSpy.count(), 1);

  // Leaving the command bar hands the focus back to the editor.
  QTest::keyClick(QApplication::focusWidget(), Qt::Key_Escape);
  QTRY_COMPARE(inSpy.count(), 2);
  QCOMPARE(outSpy.count(), 1);
  QVERIFY(edit->hasFocus());

  // The status widget must be unparented before the editor goes away.
  statusWidget->setParent(nullptr);
}

void TestRichTextEditor::testViMotionsKeepFormatting() {
  VRichTextEditor editor(configWithMode(InputMode::ViMode));
  editor.setHtml(QString::fromUtf8(c_html));
  auto edit = editor.getTextEdit();
  auto doc = editor.document();

  auto cursor = edit->textCursor();
  cursor.movePosition(QTextCursor::Start);
  edit->setTextCursor(cursor);

  const auto firstBlock = doc->firstBlock().text();
  const int boldIndex = firstBlock.indexOf(QStringLiteral("bold"));
  QVERIFY(boldIndex > 0);
  const int boldPosition = doc->firstBlock().position() + boldIndex + 1;
  QVERIFY(isBoldAt(doc, boldPosition));

  // Delete the very first (unformatted) character.
  QTest::keyClick(edit, Qt::Key_X);
  QCOMPARE(doc->firstBlock().text(), firstBlock.mid(1));

  // The bold run is untouched.
  QVERIFY(isBoldAt(doc, boldPosition - 1));

  // A motion down and back must not modify anything.
  const auto snapshot = doc->toPlainText();
  QTest::keyClicks(edit, QStringLiteral("jk"));
  QTest::keyClicks(edit, QStringLiteral("lh"));
  QCOMPARE(doc->toPlainText(), snapshot);
  QVERIFY(isBoldAt(doc, boldPosition - 1));
}

void TestRichTextEditor::testViEditCommandsKeepFormatting() {
  VRichTextEditor editor(configWithMode(InputMode::ViMode));
  editor.setHtml(QStringLiteral("<p>head</p><p>plain <b>bold</b> tail</p>"
                                "<ul><li>alpha</li><li>beta</li></ul>"));
  auto edit = editor.getTextEdit();
  auto doc = editor.document();

  auto cursor = edit->textCursor();
  cursor.movePosition(QTextCursor::Start);
  edit->setTextCursor(cursor);

  const int blockCount = doc->blockCount();
  const auto formattedLine = doc->findBlockByNumber(1).text();

  // dd on the first line leaves the formatted line untouched.
  QTest::keyClicks(edit, QStringLiteral("dd"));
  QCOMPARE(doc->blockCount(), blockCount - 1);
  QCOMPARE(doc->firstBlock().text(), formattedLine);
  const int boldIndex = formattedLine.indexOf(QStringLiteral("bold"));
  QVERIFY(boldIndex > 0);
  QVERIFY(isBoldAt(doc, doc->firstBlock().position() + boldIndex + 1));

  // yy/p duplicate a line. The put is plain text by design (D5), but the
  // source line keeps its formatting.
  QTest::keyClicks(edit, QStringLiteral("yy"));
  QTest::keyClicks(edit, QStringLiteral("p"));
  QCOMPARE(doc->blockCount(), blockCount);
  QCOMPARE(doc->firstBlock().text(), formattedLine);
  QVERIFY(isBoldAt(doc, doc->firstBlock().position() + boldIndex + 1));

  // The put line carries the same characters but none of the formatting: the
  // Vi register is a QString. This pins the accepted D5 limitation.
  const auto pastedBlock = doc->findBlockByNumber(1);
  QCOMPARE(pastedBlock.text(), formattedLine);
  QVERIFY(!isBoldAt(doc, pastedBlock.position() + boldIndex + 1));

  // A appends at the end of the line, Esc returns to normal mode.
  auto cursorAtFirst = edit->textCursor();
  cursorAtFirst.setPosition(doc->firstBlock().position());
  edit->setTextCursor(cursorAtFirst);
  QTest::keyClicks(edit, QStringLiteral("A"));
  QCOMPARE(editor.getEditorMode(), EditorMode::ViModeInsert);
  QTest::keyClicks(edit, QStringLiteral("!"));
  QTest::keyClick(edit, Qt::Key_Escape);
  QCOMPARE(editor.getEditorMode(), EditorMode::ViModeNormal);
  QCOMPARE(doc->firstBlock().text(), formattedLine + QLatin1Char('!'));
  QVERIFY(isBoldAt(doc, doc->firstBlock().position() + boldIndex + 1));
}

void TestRichTextEditor::testMimeDataOfOverriddenSelectionHasHtmlAndPlain() {
  ExposedTextEdit edit;
  edit.setHtml(QStringLiteral("<p>plain <b>bold</b></p><p>second</p>"));

  auto doc = edit.document();
  const int end = doc->lastBlock().position() + doc->lastBlock().length() - 1;
  edit.setOverriddenSelection(0, end);

  QScopedPointer<QMimeData> data(edit.createMimeDataFromSelection());
  QVERIFY(data);
  QVERIFY(data->hasHtml());
  QVERIFY(data->html().contains(QStringLiteral("font-weight")));
  QVERIFY(data->hasText());

  // Paragraph separators are normalized in the plain flavour.
  QVERIFY(!data->text().contains(QChar(QChar::ParagraphSeparator)));
  QVERIFY(data->text().contains(QLatin1Char('\n')));
}

void TestRichTextEditor::testMimeDataOfAListSelection() {
  ExposedTextEdit edit;
  edit.setHtml(QStringLiteral("<ul><li>alpha</li><li>beta</li></ul>"
                              "<table border=\"1\"><tr><td>cell one</td>"
                              "<td>cell two</td></tr></table>"));
  auto doc = edit.document();

  // The list part only.
  const auto secondItem = doc->findBlockByNumber(1);
  edit.setOverriddenSelection(doc->firstBlock().position(),
                              secondItem.position() + secondItem.length() - 1);
  {
    QScopedPointer<QMimeData> data(edit.createMimeDataFromSelection());
    QVERIFY(data->hasHtml());
    QVERIFY(data->html().contains(QStringLiteral("<li")));
    QVERIFY(data->text().contains(QStringLiteral("alpha")));
    QVERIFY(data->text().contains(QStringLiteral("beta")));
    QVERIFY(!data->text().contains(QChar(QChar::ParagraphSeparator)));
  }

  // A single table cell.
  QTextCursor cellCursor(doc);
  cellCursor.movePosition(QTextCursor::End);
  auto table = doc->rootFrame()->childFrames().isEmpty()
                   ? nullptr
                   : qobject_cast<QTextTable *>(doc->rootFrame()->childFrames().first());
  QVERIFY(table);
  auto cell = table->cellAt(0, 0);
  const int start = cell.firstCursorPosition().position();
  const int end = cell.lastCursorPosition().position();
  edit.setOverriddenSelection(start, end);
  {
    QScopedPointer<QMimeData> data(edit.createMimeDataFromSelection());
    QVERIFY(data->hasHtml());
    QCOMPARE(data->text(), QStringLiteral("cell one"));
  }
}

void TestRichTextEditor::testJoinLinesIsFormatLossy() {
  VRichTextEditor editor(configWithMode(InputMode::ViMode));
  editor.setHtml(QStringLiteral("<p>first</p><p><b>second</b></p>"));
  auto edit = editor.getTextEdit();
  auto doc = editor.document();

  auto cursor = edit->textCursor();
  cursor.movePosition(QTextCursor::Start);
  edit->setTextCursor(cursor);

  QCOMPARE(doc->blockCount(), 2);

  // Vi J joins through the QString-based interface, so the joined line ends up
  // with a single, uniform character format. This pins the accepted D5
  // limitation.
  QTest::keyClicks(edit, QStringLiteral("J"));

  QCOMPARE(doc->blockCount(), 1);
  const auto block = doc->firstBlock();
  QCOMPARE(block.textFormats().count(), 1);
}

void TestRichTextEditor::testVscodeMoveLineIsFormatLossy() {
  VRichTextEditor editor(configWithMode(InputMode::VscodeMode));
  editor.setHtml(QStringLiteral("<p>first</p><p><b>second</b></p><p>third</p>"));
  auto edit = editor.getTextEdit();
  auto doc = editor.document();

  auto cursor = edit->textCursor();
  cursor.movePosition(QTextCursor::Start);
  edit->setTextCursor(cursor);

  QVERIFY(isBoldAt(doc, doc->findBlockByNumber(1).position() + 1));

  // Alt+Down moves the current line down through the QString-based interface.
  // The moved lines are rebuilt as plain text, which is the accepted D5
  // limitation.
  QTest::keyClick(edit, Qt::Key_Down, Qt::AltModifier);

  QCOMPARE(doc->firstBlock().text(), QStringLiteral("second"));
  QCOMPARE(doc->findBlockByNumber(1).text(), QStringLiteral("first"));
  // Formatting of the touched lines is gone.
  QVERIFY(!isBoldAt(doc, doc->firstBlock().position() + 1));
}

void TestRichTextEditor::testVscodeDuplicateLineIsFormatLossy() {
  VRichTextEditor editor(configWithMode(InputMode::VscodeMode));
  editor.setHtml(QStringLiteral("<p>plain <b>bold</b> tail</p><p>second</p>"));
  auto edit = editor.getTextEdit();
  auto doc = editor.document();

  auto cursor = edit->textCursor();
  cursor.movePosition(QTextCursor::Start);
  edit->setTextCursor(cursor);

  const auto formattedLine = doc->firstBlock().text();
  const int boldIndex = formattedLine.indexOf(QStringLiteral("bold"));
  QVERIFY(boldIndex > 0);
  const int blockCount = doc->blockCount();

  // Shift+Alt+Down duplicates the current line through the QString-based
  // interface, so the copy is plain text (accepted D5 limitation).
  QTest::keyClick(edit, Qt::Key_Down, Qt::ShiftModifier | Qt::AltModifier);

  QCOMPARE(doc->blockCount(), blockCount + 1);
  QCOMPARE(doc->firstBlock().text(), formattedLine);
  QCOMPARE(doc->findBlockByNumber(1).text(), formattedLine);
  QVERIFY(!isBoldAt(doc, doc->findBlockByNumber(1).position() + boldIndex + 1));
}

void TestRichTextEditor::testViMotionsTraverseATable() {
  VRichTextEditor editor(configWithMode(InputMode::ViMode));
  editor.setHtml(QStringLiteral("<p>before</p>"
                                "<table border=\"1\"><tr><td>one</td><td>two</td></tr>"
                                "<tr><td>three</td><td>four</td></tr></table>"
                                "<p>after</p>"));
  auto edit = editor.getTextEdit();
  auto doc = editor.document();

  auto cursor = edit->textCursor();
  cursor.movePosition(QTextCursor::Start);
  edit->setTextCursor(cursor);

  // Accepted D6 limitation: the line model is flat, so j walks into the table
  // cells one by one instead of skipping the table.
  const int blocks = doc->blockCount();
  QVERIFY(blocks > 3);

  QTest::keyClicks(edit, QStringLiteral("j"));
  QCOMPARE(edit->textCursor().blockNumber(), 1);
  QCOMPARE(edit->textCursor().block().text(), QStringLiteral("one"));

  // G still reaches the very last block.
  QTest::keyClicks(edit, QStringLiteral("G"));
  QCOMPARE(edit->textCursor().blockNumber(), blocks - 1);
  QCOMPARE(edit->textCursor().block().text(), QStringLiteral("after"));
}

QTEST_MAIN(tests::TestRichTextEditor)
