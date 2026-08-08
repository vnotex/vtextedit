#include "test_interactivepreview.h"

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QApplication>
#include <QEventLoop>
#include <QPointer>
#include <QScrollBar>
#include <QSignalSpy>
#include <QSizePolicy>
#include <QLineEdit>
#include <QTableView>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>

#include <vtextedit/markdowneditorconfig.h>
#include <vtextedit/markdownhighlighter.h>
#include <vtextedit/texteditorconfig.h>
#include <vtextedit/theme.h>
#include <vtextedit/vmarkdowneditor.h>
#include <vtextedit/vtextedit.h>

using namespace tests;
using namespace vte;

// ---------------------------------------------------------------------------
// Test doubles
// ---------------------------------------------------------------------------

RecordingPreviewWidget::RecordingPreviewWidget(PreviewWidgetContext *p_context, QWidget *p_parent,
                                               const QVector<PreviewElementType> &p_types,
                                               const QSize &p_hint)
    : PreviewWidget(p_context, p_parent), m_types(p_types), m_hint(p_hint) {
  if (p_context) {
    connect(p_context, &PreviewWidgetContext::replacementFinished, this,
            &RecordingPreviewWidget::handleReplacementFinished);
  }
}

QVector<PreviewElementType> RecordingPreviewWidget::supportedTypes() const { return m_types; }

bool RecordingPreviewWidget::setPreview(const QSharedPointer<const vte::Preview> &p_preview) {
  if (!p_preview || !m_types.contains(p_preview->type())) {
    return false;
  }

  m_preview = p_preview;
  ++m_setPreviewCount;

  if (m_selfDestructOnUpdate && m_setPreviewCount > 1) {
    // The host must survive a widget destroying itself from inside its own
    // callback and still return a coherent result.
    delete this;
    return true;
  }

  if (m_spinOnNextSetPreview) {
    // A renderer opening a modal dialog runs a real nested event loop here,
    // which keeps delivering the host's own queued timers while this callback
    // is still on the stack.
    m_spinOnNextSetPreview = false;
    ++m_spinCount;
    if (m_deliveryCounterSource) {
      m_deliveriesBeforeSpin =
          m_deliveryCounterSource->property("vte_preview_reconcile_deliveries").toInt();
    }

    QEventLoop loop;
    QTimer::singleShot(30, this, [this]() {
      if (m_duringSpin) {
        m_duringSpin();
      }
    });
    // Bounded, so a failing assertion cannot hang the suite.
    QTimer::singleShot(60, &loop, [&loop]() { loop.quit(); });
    loop.exec();

    if (m_deliveryCounterSource) {
      m_deliveriesAfterSpin =
          m_deliveryCounterSource->property("vte_preview_reconcile_deliveries").toInt();
    }
  }

  return true;
}

QSize RecordingPreviewWidget::sizeHint() const {
  ++m_sizeHintCount;
  return m_hint;
}

qreal RecordingPreviewWidget::preferredWidthFraction() const { return m_widthFraction; }

bool RecordingPreviewWidget::hasHeightForWidth() const { return m_wrapping; }

int RecordingPreviewWidget::heightForWidth(int p_width) const {
  if (!m_wrapping || p_width <= 0) {
    return m_hint.height();
  }

  // A fixed area: the narrower the widget, the taller it has to be.
  const int area = m_hint.width() * m_hint.height();
  return qMax(1, area / p_width);
}

void RecordingPreviewWidget::handleReplacementFinished(const PreviewReplacementResult &p_result) {
  m_lastResult = p_result;
  ++m_resultCount;
}

RecordingPreviewFactory::RecordingPreviewFactory(const QVector<PreviewElementType> &p_types,
                                                 QObject *p_parent)
    : PreviewWidgetFactory(p_parent), m_types(p_types) {}

QVector<PreviewElementType> RecordingPreviewFactory::supportedTypes() const {
  auto self = const_cast<RecordingPreviewFactory *>(this);
  ++self->m_supportedTypesCount;
  if (m_unregisterSelfIn) {
    auto editor = m_unregisterSelfIn;
    self->m_unregisterSelfIn = nullptr;
    editor->unregisterPreviewWidgetFactory(self);
  }

  if (m_registerReentrantlyInSupportedTypes) {
    auto editor = m_registerReentrantlyInSupportedTypes;
    self->m_registerReentrantlyInSupportedTypes = nullptr;
    auto nested = new RecordingPreviewFactory(m_types);
    self->m_reentrantRegistrationAccepted = editor->registerPreviewWidgetFactory(nested, 20);
    if (!self->m_reentrantRegistrationAccepted) {
      delete nested;
    }
  }

  return m_types;
}

PreviewWidget *RecordingPreviewFactory::createWidget(PreviewWidgetContext *p_context,
                                                     const QSharedPointer<const Preview> &p_preview,
                                                     QWidget *p_parent) {
  Q_UNUSED(p_preview);
  ++m_createCount;
  if (m_createCountSink) {
    ++(*m_createCountSink);
  }

  if (m_registerReentrantlyIn) {
    auto editor = m_registerReentrantlyIn;
    m_registerReentrantlyIn = nullptr;
    auto nested = new RecordingPreviewFactory(m_types);
    m_reentrantRegistrationAccepted = editor->registerPreviewWidgetFactory(nested, 20);
    if (!m_reentrantRegistrationAccepted) {
      delete nested;
    }
  }

  if (m_decline) {
    return nullptr;
  }

  auto types = m_refuseSetPreview ? QVector<PreviewElementType>() : m_types;
  auto widget = new RecordingPreviewWidget(p_context, p_parent, types, m_hint);
  widget->m_wrapping = m_wrapping;
  widget->m_widthFraction = m_widthFraction;
  if (m_wrapping) {
    QSizePolicy policy = widget->sizePolicy();
    policy.setHeightForWidth(true);
    widget->setSizePolicy(policy);
  }
  m_widgets.append(widget);
  return widget;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {
const char *c_table = "| h1 | h2 |\n| --- | --- |\n| a | b |\n";

QSharedPointer<MarkdownEditorConfig> makeConfig() {
  auto textConfig = QSharedPointer<TextEditorConfig>::create();
  auto config = QSharedPointer<MarkdownEditorConfig>::create(textConfig);
  // The table sheet can rewrite the document, so it is opt-in.
  config->m_inplacePreviewSources |= MarkdownEditorConfig::Table;
  return config;
}

// Drive one full parse generation and let the host settle.
void settle(VMarkdownEditor &p_editor) {
  auto highlighter = p_editor.getHighlighter();
  QSignalSpy completed(highlighter, &MarkdownHighlighter::highlightCompleted);
  highlighter->updateHighlight();
  QTRY_VERIFY(completed.count() > 0);
  QTest::qWait(30);
  QCoreApplication::processEvents();
}

void setTextAndSettle(VMarkdownEditor &p_editor, const QString &p_text) {
  p_editor.setText(p_text);
  settle(p_editor);
}

QList<PreviewWidget *> previewWidgets(VMarkdownEditor &p_editor) {
  return p_editor.getTextEdit()->viewport()->findChildren<PreviewWidget *>(
      QString(), Qt::FindDirectChildrenOnly);
}

PreviewWidget *singlePreviewWidget(VMarkdownEditor &p_editor) {
  const auto widgets = previewWidgets(p_editor);
  return widgets.size() == 1 ? widgets.first() : nullptr;
}

// The host is an internal QObject child, reachable only by its object name.
QObject *previewHost(VMarkdownEditor &p_editor) {
  return p_editor.findChild<QObject *>(QStringLiteral("vte_interactive_preview_host"));
}

int reconcileDeliveries(VMarkdownEditor &p_editor) {
  auto host = previewHost(p_editor);
  return host ? host->property("vte_preview_reconcile_deliveries").toInt() : -1;
}

// Collects the warnings Qt emits while it is installed, so a test can assert
// that a particular diagnostic was *not* produced.
class WarningRecorder {
public:
  WarningRecorder() {
    Q_ASSERT(!s_active);
    s_active = this;
    m_previous = qInstallMessageHandler(&WarningRecorder::handle);
  }

  ~WarningRecorder() {
    qInstallMessageHandler(m_previous);
    s_active = nullptr;
  }

  bool contains(const QString &p_needle) const {
    for (const auto &message : m_messages) {
      if (message.contains(p_needle)) {
        return true;
      }
    }

    return false;
  }

  QStringList m_messages;

private:
  static void handle(QtMsgType p_type, const QMessageLogContext &p_context,
                     const QString &p_message) {
    if (s_active) {
      s_active->m_messages.append(p_message);
      if (s_active->m_previous) {
        s_active->m_previous(p_type, p_context, p_message);
      }
    }
  }

  QtMessageHandler m_previous = nullptr;

  static WarningRecorder *s_active;
};

WarningRecorder *WarningRecorder::s_active = nullptr;
} // namespace

// ---------------------------------------------------------------------------
// Built-in renderer
// ---------------------------------------------------------------------------

void TestInteractivePreview::testBuiltinTableWidgetCreated() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  setTextAndSettle(editor, QLatin1String(c_table));

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  QCOMPARE(widget->supportedTypes(), QVector<PreviewElementType>{PreviewElementType::Table});

  auto context = widget->previewContext();
  QVERIFY(context);
  QVERIFY(context->preview());
  QCOMPARE(context->preview()->type(), PreviewElementType::Table);
  QCOMPARE(context->preview()->startPos(), 0);
}

void TestInteractivePreview::testNoWidgetForImageCodeMathByDefault() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  setTextAndSettle(editor, QStringLiteral("![a](b.png)\n\n```cpp\nint a;\n```\n\n$$\nx\n$$\n"));

  // Without a custom factory those elements keep the painted static path.
  QVERIFY(previewWidgets(editor).isEmpty());
}

void TestInteractivePreview::testCustomFactoryOverridesBuiltin() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  setTextAndSettle(editor, QLatin1String(c_table));
  QVERIFY(singlePreviewWidget(editor));

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 1));
  QTest::qWait(50);
  QCoreApplication::processEvents();

  auto widget = qobject_cast<RecordingPreviewWidget *>(singlePreviewWidget(editor));
  QVERIFY(widget);
  QCOMPARE(widget->m_preview->type(), PreviewElementType::Table);
}

void TestInteractivePreview::testMultiTypeFactory() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Image, PreviewElementType::Code,
                                              PreviewElementType::Math,
                                              PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 1));

  setTextAndSettle(editor, QStringLiteral("![a](b.png)\n\n```cpp\nint a;\n```\n\n$$\nx\n$$\n\n") +
                               QLatin1String(c_table));

  QSet<int> types;
  const auto widgets = previewWidgets(editor);
  for (auto widget : widgets) {
    auto recording = qobject_cast<RecordingPreviewWidget *>(widget);
    QVERIFY(recording);
    types.insert(static_cast<int>(recording->m_preview->type()));
  }

  QCOMPARE(widgets.size(), 4);
  QCOMPARE(types.size(), 4);
}

void TestInteractivePreview::testFactoryPriorityAndOrder() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto low = new RecordingPreviewFactory({PreviewElementType::Table});
  auto high = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(low, 1));
  QVERIFY(editor.registerPreviewWidgetFactory(high, 5));

  setTextAndSettle(editor, QLatin1String(c_table));

  QCOMPARE(high->m_widgets.size(), 1);
  QCOMPARE(low->m_createCount, 0);

  // Same priority: registration order decides.
  auto firstOfTwo = new RecordingPreviewFactory({PreviewElementType::Table});
  auto secondOfTwo = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(firstOfTwo, 9));
  QVERIFY(editor.registerPreviewWidgetFactory(secondOfTwo, 9));
  QTest::qWait(50);
  QCoreApplication::processEvents();

  QCOMPARE(firstOfTwo->m_widgets.size(), 1);
  QCOMPARE(secondOfTwo->m_createCount, 0);
}

void TestInteractivePreview::testDecliningFactoryFallsThrough() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto declining = new RecordingPreviewFactory({PreviewElementType::Table});
  declining->m_decline = true;
  QVERIFY(editor.registerPreviewWidgetFactory(declining, 5));

  setTextAndSettle(editor, QLatin1String(c_table));

  QVERIFY(declining->m_createCount > 0);
  QVERIFY(declining->m_widgets.isEmpty());
  // The host asks what a factory claims before it ever builds a widget, so a
  // declining factory is still consulted.
  QVERIFY(declining->m_supportedTypesCount > 0);
  // The built-in renderer took over.
  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  QVERIFY(!qobject_cast<RecordingPreviewWidget *>(widget));
}

void TestInteractivePreview::testRefusingWidgetFallsThrough() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto refusing = new RecordingPreviewFactory({PreviewElementType::Table});
  refusing->m_refuseSetPreview = true;
  QVERIFY(editor.registerPreviewWidgetFactory(refusing, 5));

  setTextAndSettle(editor, QLatin1String(c_table));

  QVERIFY(refusing->m_createCount > 0);
  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  QVERIFY(!qobject_cast<RecordingPreviewWidget *>(widget));
}

void TestInteractivePreview::testRegistrationValidation() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  QVERIFY(!editor.registerPreviewWidgetFactory(nullptr, 0));

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 1));
  // Duplicate registration is rejected and does not take ownership again.
  QVERIFY(!editor.registerPreviewWidgetFactory(factory, 2));

  QVERIFY(!editor.unregisterPreviewWidgetFactory(nullptr));
}

void TestInteractivePreview::testUnregisterRestoresFallback() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QLatin1String(c_table));
  QVERIFY(qobject_cast<RecordingPreviewWidget *>(singlePreviewWidget(editor)));

  QVERIFY(editor.unregisterPreviewWidgetFactory(factory));
  QTest::qWait(50);
  QCoreApplication::processEvents();

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  QVERIFY(!qobject_cast<RecordingPreviewWidget *>(widget));
}

void TestInteractivePreview::testUnregisterDestroysFactory() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QPointer<RecordingPreviewFactory> guard(factory);
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  QCOMPARE(factory->parent(), editor.findChild<QObject *>(
                                  QStringLiteral("vte_interactive_preview_host")));

  QVERIFY(editor.unregisterPreviewWidgetFactory(factory));
  // Unregistering twice is refused.
  QTest::qWait(50);
  QCoreApplication::processEvents();
  QVERIFY(guard.isNull());
}

void TestInteractivePreview::testEditorDestructionDestroysFactory() {
  QPointer<RecordingPreviewFactory> guard;
  {
    VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
    auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
    guard = factory;
    QVERIFY(editor.registerPreviewWidgetFactory(factory, 1));
    setTextAndSettle(editor, QLatin1String(c_table));
    QVERIFY(!guard.isNull());
  }

  QCoreApplication::processEvents();
  QVERIFY(guard.isNull());
}

void TestInteractivePreview::testIdentityReuseOnUnrelatedEdit() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));

  setTextAndSettle(editor, QLatin1String(c_table) + QStringLiteral("\ntail\n"));
  QCOMPARE(factory->m_widgets.size(), 1);
  auto widget = factory->m_widgets.first();
  const int firstSetCount = widget->m_setPreviewCount;

  // Append unrelated text: the same widget must be updated in place.
  QTextCursor cursor(editor.document());
  cursor.movePosition(QTextCursor::End);
  cursor.insertText(QStringLiteral("more text\n"));
  settle(editor);

  QCOMPARE(factory->m_widgets.size(), 1);
  QVERIFY(widget->m_setPreviewCount > firstSetCount);
  QCOMPARE(singlePreviewWidget(editor), static_cast<PreviewWidget *>(widget));
}

// ---------------------------------------------------------------------------
// Replacement
// ---------------------------------------------------------------------------

void TestInteractivePreview::testReplacementAccepted() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QLatin1String(c_table));

  QCOMPARE(factory->m_widgets.size(), 1);
  auto widget = factory->m_widgets.first();
  widget->previewContext()->requestSourceReplacement(
      QStringLiteral("| h1 | h2 |\n| --- | --- |\n| z | b |"));

  QCOMPARE(widget->m_resultCount, 1);
  QCOMPARE(widget->m_lastResult.status(), PreviewReplacementResult::Accepted);
  QVERIFY(widget->m_lastResult.isAccepted());
  QVERIFY(editor.document()->toPlainText().contains(QStringLiteral("| z | b |")));
}

void TestInteractivePreview::testReplacementIsOneUndoStep() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QLatin1String(c_table));

  const QString before = editor.document()->toPlainText();
  auto widget = factory->m_widgets.first();
  widget->previewContext()->requestSourceReplacement(
      QStringLiteral("| h1 | h2 |\n| --- | --- |\n| z | b |"));
  QVERIFY(widget->m_lastResult.isAccepted());
  QVERIFY(editor.document()->toPlainText() != before);

  editor.document()->undo();
  QCOMPARE(editor.document()->toPlainText(), before);

  editor.document()->redo();
  QVERIFY(editor.document()->toPlainText().contains(QStringLiteral("| z | b |")));
}

void TestInteractivePreview::testReplacementRejectedWhenReadOnly() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QLatin1String(c_table));

  const QString before = editor.document()->toPlainText();
  editor.getTextEdit()->setReadOnly(true);

  auto widget = factory->m_widgets.first();
  widget->previewContext()->requestSourceReplacement(
      QStringLiteral("| h1 | h2 |\n| --- | --- |\n| z | b |"));

  QCOMPARE(widget->m_lastResult.status(), PreviewReplacementResult::ReadOnly);
  QCOMPARE(editor.document()->toPlainText(), before);
}

void TestInteractivePreview::testReplacementRejectedOnStaleSnapshot() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QLatin1String(c_table));

  auto widget = factory->m_widgets.first();
  auto context = widget->previewContext();
  auto stale = context->preview();

  // Produce a newer accepted generation.
  QTextCursor cursor(editor.document());
  cursor.movePosition(QTextCursor::End);
  cursor.insertText(QStringLiteral("\ntail\n"));
  settle(editor);

  QVERIFY(context->preview()->revision() != stale->revision());

  // Ask the host directly with the superseded revision.
  QSignalSpy spy(context, &PreviewWidgetContext::replacementFinished);
  emit context->sourceReplacementRequested(context->identity(), stale->revision(),
                                           stale->sourceMarkdown(),
                                           QStringLiteral("| h1 | h2 |\n| --- | --- |\n| z | b |"));
  QCOMPARE(spy.count(), 1);
  QCOMPARE(widget->m_lastResult.status(), PreviewReplacementResult::StaleSnapshot);
}

void TestInteractivePreview::testReplacementRejectedOnTypeMismatch() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QLatin1String(c_table));

  const QString before = editor.document()->toPlainText();
  auto widget = factory->m_widgets.first();
  widget->previewContext()->requestSourceReplacement(QStringLiteral("```cpp\nint a;\n```"));

  QCOMPARE(widget->m_lastResult.status(), PreviewReplacementResult::TypeMismatch);
  QCOMPARE(editor.document()->toPlainText(), before);
}

void TestInteractivePreview::testReplacementRejectedOnElementCountMismatch() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QLatin1String(c_table));

  const QString before = editor.document()->toPlainText();
  auto widget = factory->m_widgets.first();

  // Two tables.
  widget->previewContext()->requestSourceReplacement(QLatin1String(c_table) +
                                                     QStringLiteral("\n") +
                                                     QLatin1String(c_table));
  QCOMPARE(widget->m_lastResult.status(), PreviewReplacementResult::ElementCountMismatch);

  // Unrelated content outside the table.
  widget->previewContext()->requestSourceReplacement(
      QStringLiteral("| h1 | h2 |\n| --- | --- |\n| a | b |\n\nunrelated"));
  QCOMPARE(widget->m_lastResult.status(), PreviewReplacementResult::ElementCountMismatch);

  // Not parseable as any element.
  widget->previewContext()->requestSourceReplacement(QStringLiteral("just text"));
  QCOMPARE(widget->m_lastResult.status(), PreviewReplacementResult::ElementCountMismatch);

  QCOMPARE(editor.document()->toPlainText(), before);
}

void TestInteractivePreview::testReplacementAcceptedAfterUnrelatedEdit() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QLatin1String(c_table) + QStringLiteral("\ntail\n"));

  auto widget = factory->m_widgets.first();

  // An edit elsewhere bumps the document revision but the anchored source is
  // still identical, so the request must be accepted.
  QTextCursor cursor(editor.document());
  cursor.movePosition(QTextCursor::End);
  cursor.insertText(QStringLiteral("appended\n"));
  settle(editor);

  widget->previewContext()->requestSourceReplacement(
      QStringLiteral("| h1 | h2 |\n| --- | --- |\n| z | b |"));
  QCOMPARE(widget->m_lastResult.status(), PreviewReplacementResult::Accepted);
  QVERIFY(editor.document()->toPlainText().contains(QStringLiteral("| z | b |")));
  QVERIFY(editor.document()->toPlainText().contains(QStringLiteral("appended")));
}

void TestInteractivePreview::testReplacementPreservesBlockquotePrefix() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QStringLiteral("> | a | b |\n> | --- | --- |\n> | c | d |\n"));

  QCOMPARE(factory->m_widgets.size(), 1);
  auto widget = factory->m_widgets.first();
  QCOMPARE(widget->m_preview->startPos(), 0);
  QVERIFY(widget->m_preview->sourceMarkdown().startsWith(QStringLiteral("> |")));

  // A replacement dropping the prefix no longer covers the whole text as a
  // single table descendant of the original wrapper chain, but a prefixed one
  // is accepted.
  widget->previewContext()->requestSourceReplacement(
      QStringLiteral("> | a | b |\n> | --- | --- |\n> | z | d |"));
  QCOMPARE(widget->m_lastResult.status(), PreviewReplacementResult::Accepted);
  QVERIFY(editor.document()->toPlainText().contains(QStringLiteral("> | z | d |")));
}

void TestInteractivePreview::testTableEditCommitsCanonicalMarkdown() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  setTextAndSettle(editor, QLatin1String(c_table));

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto model = widget->findChild<QAbstractItemModel *>();
  QVERIFY(model);
  QCOMPARE(model->rowCount(), 2);
  QCOMPARE(model->columnCount(), 2);
  QCOMPARE(model->data(model->index(0, 0)).toString(), QStringLiteral("h1"));
  QCOMPARE(model->data(model->index(1, 1)).toString(), QStringLiteral("b"));

  // A no-op commit changes nothing.
  const QString before = editor.document()->toPlainText();
  QVERIFY(!model->setData(model->index(1, 1), QStringLiteral("b")));
  QCOMPARE(editor.document()->toPlainText(), before);

  // A real edit is written back in canonical form.
  QVERIFY(model->setData(model->index(1, 1), QStringLiteral("changed")));
  const QString after = editor.document()->toPlainText();
  QVERIFY2(after.contains(QStringLiteral("| a   | changed |")), qPrintable(after));
  QVERIFY2(after.contains(QStringLiteral("| --- | ------- |")), qPrintable(after));
  QVERIFY2(after.contains(QStringLiteral("| h1  | h2      |")), qPrintable(after));
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

void TestInteractivePreview::testSourceBitDisablesTablePreview() {
  auto config = makeConfig();
  config->m_inplacePreviewSources =
      MarkdownEditorConfig::ImageLink | MarkdownEditorConfig::CodeBlock |
      MarkdownEditorConfig::Math;

  VMarkdownEditor editor(config, QSharedPointer<TextEditorParameters>::create());
  setTextAndSettle(editor, QLatin1String(c_table));
  QVERIFY(previewWidgets(editor).isEmpty());

  // Turning the bit on at runtime brings the sheet back.
  auto enabled = makeConfig();
  enabled->m_inplacePreviewSources = MarkdownEditorConfig::Table;
  editor.setConfig(enabled);
  settle(editor);
  QVERIFY(singlePreviewWidget(editor));
}

void TestInteractivePreview::testGlobalDisableRemovesWidgets() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  setTextAndSettle(editor, QLatin1String(c_table));
  QVERIFY(singlePreviewWidget(editor));

  editor.setInplacePreviewEnabled(false);
  QTest::qWait(50);
  QCoreApplication::processEvents();
  QVERIFY(previewWidgets(editor).isEmpty());

  editor.setInplacePreviewEnabled(true);
  QTest::qWait(50);
  QCoreApplication::processEvents();
  QVERIFY(singlePreviewWidget(editor));
}

void TestInteractivePreview::testTablePreviewVisibleRows() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  QCOMPARE(editor.tablePreviewVisibleRows(), 10);

  editor.setTablePreviewVisibleRows(3);
  QCOMPARE(editor.tablePreviewVisibleRows(), 3);

  // Consumed as at least one row.
  editor.setTablePreviewVisibleRows(0);
  QCOMPARE(editor.tablePreviewVisibleRows(), 1);
}

void TestInteractivePreview::testDuplicateTablesGetDistinctIdentities() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QLatin1String(c_table) + QStringLiteral("\n") +
                               QLatin1String(c_table));

  QCOMPARE(factory->m_widgets.size(), 2);
  const auto first = factory->m_widgets[0]->previewContext();
  const auto second = factory->m_widgets[1]->previewContext();
  QVERIFY(first->identity() != second->identity());
  QVERIFY(first->preview()->startPos() != second->preview()->startPos());
}

void TestInteractivePreview::testWidgetGeometryFollowsScrolling() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));

  QString text;
  for (int i = 0; i < 80; ++i) {
    text += QStringLiteral("filler line %1\n").arg(i);
  }
  text += QStringLiteral("\n");
  text += QLatin1String(c_table);

  editor.resize(600, 300);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  setTextAndSettle(editor, text);

  QVERIFY2(factory->m_widgets.size() == 1,
           qPrintable(QStringLiteral("created=%1 live=%2")
                          .arg(factory->m_widgets.size())
                          .arg(previewWidgets(editor).size())));
  auto widget = factory->m_widgets.first();
  auto vbar = editor.getTextEdit()->verticalScrollBar();
  QVERIFY(vbar->maximum() > 0);

  vbar->setValue(vbar->minimum());
  QCoreApplication::processEvents();
  const int topY = widget->y();

  vbar->setValue(vbar->maximum());
  QCoreApplication::processEvents();
  QVERIFY(widget->y() < topY);

  // The document rect stays stable while only the viewport mapping moves.
  QVERIFY(!widget->previewContext()->assignedPreviewRect().isNull());
  QVERIFY(widget->isVisible());
}

// ---------------------------------------------------------------------------
// Regressions
// ---------------------------------------------------------------------------

void TestInteractivePreview::testReplacementRejectedOnChangedContainerChain() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QStringLiteral("> | a | b |\n> | --- | --- |\n> | c | d |\n"));

  QCOMPARE(factory->m_widgets.size(), 1);
  auto widget = factory->m_widgets.first();
  const QString before = editor.document()->toPlainText();

  // Dropping the block quote would lift the table out of its container.
  widget->previewContext()->requestSourceReplacement(
      QStringLiteral("| a | b |\n| --- | --- |\n| z | d |"));
  QVERIFY(!widget->m_lastResult.isAccepted());
  QCOMPARE(editor.document()->toPlainText(), before);

  // Deepening the block quote is a structural change too.
  widget->previewContext()->requestSourceReplacement(
      QStringLiteral(">> | a | b |\n>> | --- | --- |\n>> | z | d |"));
  QVERIFY(!widget->m_lastResult.isAccepted());
  QCOMPARE(editor.document()->toPlainText(), before);

  // Turning it into a list item is rejected as well.
  widget->previewContext()->requestSourceReplacement(
      QStringLiteral("- | a | b |\n  | --- | --- |\n  | z | d |"));
  QVERIFY(!widget->m_lastResult.isAccepted());
  QCOMPARE(editor.document()->toPlainText(), before);
}

void TestInteractivePreview::testReplacementRejectedWhenSplittingTrailingText() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Image});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QStringLiteral("lead ![a](b.png) trail\n"));

  QCOMPARE(factory->m_widgets.size(), 1);
  auto widget = factory->m_widgets.first();
  const QString before = editor.document()->toPlainText();

  // A multi-line replacement would push " trail" into a new block.
  widget->previewContext()->requestSourceReplacement(QStringLiteral("![a](b.png)\n![c](d.png)"));
  QVERIFY(!widget->m_lastResult.isAccepted());
  QCOMPARE(editor.document()->toPlainText(), before);

  // A single-line replacement is fine.
  widget->previewContext()->requestSourceReplacement(QStringLiteral("![c](d.png)"));
  QCOMPARE(widget->m_lastResult.status(), PreviewReplacementResult::Accepted);
  QVERIFY(editor.document()->toPlainText().contains(QStringLiteral("lead ![c](d.png) trail")));
}

void TestInteractivePreview::testSourceMismatchDoesNotRestoreStaleValues() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  setTextAndSettle(editor, QLatin1String(c_table));

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto view = widget->findChild<QAbstractItemView *>();
  QVERIFY(view);
  QVERIFY(view->isEnabled());

  // Change the source without letting a new snapshot be published.
  QTextCursor cursor(editor.document());
  cursor.setPosition(3);
  cursor.insertText(QStringLiteral("X"));

  auto model = widget->findChild<QAbstractItemModel *>();
  QVERIFY(model);
  QVERIFY(model->setData(model->index(1, 1), QStringLiteral("edited")));

  // The live source no longer matches, so the widget must stop presenting the
  // superseded snapshot as the truth instead of restoring its old values.
  QVERIFY(!view->isEnabled());
  QVERIFY(editor.document()->toPlainText().contains(QStringLiteral("| hX1 | h2 |")));

  // The authoritative snapshot re-enables editing.
  settle(editor);
  auto refreshed = singlePreviewWidget(editor);
  QVERIFY(refreshed);
  QVERIFY(refreshed->findChild<QAbstractItemView *>()->isEnabled());
}

void TestInteractivePreview::testFactoryUnregisteringItselfFromCallback() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  int createCount = 0;
  auto suicidal = new RecordingPreviewFactory({PreviewElementType::Table});
  suicidal->m_createCountSink = &createCount;
  QPointer<RecordingPreviewFactory> guard(suicidal);
  QVERIFY(editor.registerPreviewWidgetFactory(suicidal, 5));
  suicidal->m_unregisterSelfIn = &editor;

  setTextAndSettle(editor, QLatin1String(c_table));

  // Unregistering from inside a callback destroys the factory without ever
  // reaching createWidget() on the now inactive instance.
  QVERIFY(guard.isNull());
  QVERIFY2(createCount == 0, qPrintable(QStringLiteral("create=%1").arg(createCount)));

  // The host fell through to the built-in renderer.
  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  QVERIFY(!qobject_cast<RecordingPreviewWidget *>(widget));
}

void TestInteractivePreview::testReentrantRegistrationRejected() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  factory->m_registerReentrantlyIn = &editor;

  setTextAndSettle(editor, QLatin1String(c_table));

  QVERIFY(factory->m_createCount > 0);
  QVERIFY(!factory->m_reentrantRegistrationAccepted);
  QVERIFY(qobject_cast<RecordingPreviewWidget *>(singlePreviewWidget(editor)));
}

void TestInteractivePreview::testEditorDestructionDestroysWidgetsAndContexts() {
  QPointer<PreviewWidget> widgetGuard;
  QPointer<PreviewWidgetContext> contextGuard;
  {
    VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
    setTextAndSettle(editor, QLatin1String(c_table));

    auto widget = singlePreviewWidget(editor);
    QVERIFY(widget);
    widgetGuard = widget;
    contextGuard = widget->previewContext();
    QVERIFY(!contextGuard.isNull());
  }

  // No event loop turn needed: nothing is left unowned behind the editor.
  QVERIFY(widgetGuard.isNull());
  QVERIFY(contextGuard.isNull());
}

void TestInteractivePreview::testWrappingWidgetGeometryIsStable() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Image});
  factory->m_wrapping = true;
  factory->m_hint = QSize(400, 40);
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));

  editor.resize(500, 400);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  // An inline image whose source span is much narrower than its size hint.
  setTextAndSettle(editor, QStringLiteral("lead ![a](b.png) trail\n"));

  QCOMPARE(factory->m_widgets.size(), 1);
  auto widget = factory->m_widgets.first();
  const QRectF assigned = widget->previewContext()->assignedPreviewRect();
  QVERIFY(!assigned.isNull());

  // The reserved height honors the width the widget is actually given.
  QCOMPARE(qRound(assigned.height()), widget->heightForWidth(qRound(assigned.width())));
  QCOMPARE(widget->height(), qRound(assigned.height()));

  // Applying the assigned geometry must not feed back into the measurement.
  QTest::qWait(200);
  QCoreApplication::processEvents();
  QCOMPARE(widget->previewContext()->assignedPreviewRect(), assigned);
}

void TestInteractivePreview::testEditorDestructionDestroysPendingRemovals() {
  QPointer<PreviewWidget> widgetGuard;
  QPointer<PreviewWidgetContext> contextGuard;
  {
    VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
    setTextAndSettle(editor, QLatin1String(c_table));

    auto widget = singlePreviewWidget(editor);
    QVERIFY(widget);
    widgetGuard = widget;
    contextGuard = widget->previewContext();

    // Remove the table so the item is queued for deferred deletion, and do not
    // let the event loop deliver it.
    QTextCursor cursor(editor.document());
    cursor.select(QTextCursor::Document);
    cursor.insertText(QStringLiteral("plain text\n"));
    editor.getHighlighter()->updateHighlight();
    QTRY_VERIFY(widgetGuard.isNull() || previewWidgets(editor).isEmpty());
  }

  // Nothing survives the editor, even when its deferred deletion never ran.
  QVERIFY(widgetGuard.isNull());
  QVERIFY(contextGuard.isNull());
}

void TestInteractivePreview::testWrappedInlineSourceMeasuredAtAssignedWidth() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Image});
  factory->m_wrapping = true;
  factory->m_hint = QSize(600, 60);
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));

  editor.resize(320, 400);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));

  // A long inline image whose own syntax wraps over several visual lines.
  const QString alt = QString(160, QLatin1Char('w'));
  setTextAndSettle(editor, QStringLiteral("lead ![%1](b.png) trail\n").arg(alt));

  QCOMPARE(factory->m_widgets.size(), 1);
  auto widget = factory->m_widgets.first();
  const QRectF assigned = widget->previewContext()->assignedPreviewRect();
  QVERIFY(!assigned.isNull());
  QVERIFY(assigned.width() > 0);

  // The reserved height matches the width the widget is actually given, even
  // though the source spans several visual lines.
  QCOMPARE(qRound(assigned.height()), widget->heightForWidth(qRound(assigned.width())));
  QCOMPARE(widget->width(), qRound(assigned.width()));
  QCOMPARE(widget->height(), qRound(assigned.height()));

  QTest::qWait(200);
  QCoreApplication::processEvents();
  QCOMPARE(widget->previewContext()->assignedPreviewRect(), assigned);
}

void TestInteractivePreview::testReplacementOfLaterInlineElement() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Image});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QStringLiteral("![first](a.png) ![second](b.png)\n"));

  QCOMPARE(factory->m_widgets.size(), 2);

  // Find the widget bound to the second image.
  RecordingPreviewWidget *second = nullptr;
  for (auto widget : factory->m_widgets) {
    if (widget->m_preview && widget->m_preview->startPos() > 0) {
      second = widget;
    }
  }
  QVERIFY(second);

  // Its probe carries the preceding image as line prefix; that must not make
  // the candidate ambiguous.
  second->previewContext()->requestSourceReplacement(QStringLiteral("![changed](c.png)"));
  QCOMPARE(second->m_lastResult.status(), PreviewReplacementResult::Accepted);
  QCOMPARE(editor.document()->toPlainText(),
           QStringLiteral("![first](a.png) ![changed](c.png)\n"));
}

void TestInteractivePreview::testWidgetDestroyingItselfOnUpdateFallsBack() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QLatin1String(c_table) + QStringLiteral("\ntail\n"));

  auto widget = qobject_cast<RecordingPreviewWidget *>(singlePreviewWidget(editor));
  QVERIFY(widget);
  const int createdBefore = factory->m_createCount;

  // Arm the widget so the next update destroys it from inside setPreview().
  widget->m_setPreviewCount = 1;
  widget->m_selfDestructOnUpdate = true;

  QTextCursor cursor(editor.document());
  cursor.movePosition(QTextCursor::End);
  cursor.insertText(QStringLiteral("appended\n"));
  settle(editor);

  // The host noticed and rebuilt through the factory chain instead of leaving
  // a dead entry behind.
  QVERIFY(factory->m_createCount > createdBefore);
  auto rebuilt = singlePreviewWidget(editor);
  QVERIFY(rebuilt);
  QVERIFY(rebuilt->previewContext());
  QCOMPARE(rebuilt->previewContext()->preview()->type(), PreviewElementType::Table);
}

void TestInteractivePreview::testCommitKeepsTheSameWidget() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QLatin1String(c_table) + QStringLiteral("\ntail\n"));

  auto widget = qobject_cast<RecordingPreviewWidget *>(singlePreviewWidget(editor));
  QVERIFY(widget);
  const quint64 identity = widget->previewContext()->identity();
  const int createdBefore = factory->m_createCount;

  widget->previewContext()->requestSourceReplacement(
      QStringLiteral("| h1 | h2 |\n| --- | --- |\n| z | b |"));
  QCOMPARE(widget->m_lastResult.status(), PreviewReplacementResult::Accepted);

  // The anchor was retargeted onto the replacement, so the reservation is not
  // dropped and the widget survives the commit.
  QCoreApplication::processEvents();
  QCOMPARE(singlePreviewWidget(editor), static_cast<PreviewWidget *>(widget));
  QVERIFY(!widget->previewContext()->assignedPreviewRect().isNull());

  settle(editor);
  QCOMPARE(singlePreviewWidget(editor), static_cast<PreviewWidget *>(widget));
  QCOMPARE(widget->previewContext()->identity(), identity);
  QCOMPARE(factory->m_createCount, createdBefore);
}

void TestInteractivePreview::testConfigChangeKeepsLiveAnchors() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QStringLiteral("head\n\n") + QLatin1String(c_table));

  auto widget = qobject_cast<RecordingPreviewWidget *>(singlePreviewWidget(editor));
  QVERIFY(widget);
  const int anchoredStart = widget->m_preview->startPos();

  // Edit before the table without letting a new parse generation land, then
  // force a rebuild through a source-bit change.
  QTextCursor cursor(editor.document());
  cursor.setPosition(0);
  cursor.insertText(QStringLiteral("prefix "));

  auto config = makeConfig();
  config->m_inplacePreviewSources =
      MarkdownEditorConfig::Table | MarkdownEditorConfig::ImageLink;
  editor.setConfig(config);
  QTest::qWait(50);
  QCoreApplication::processEvents();

  // The rebuilt preview must follow the edit, not fall back to the superseded
  // parse position.
  auto rebuilt = singlePreviewWidget(editor);
  QVERIFY(rebuilt);
  const QString anchored =
      editor.document()->toPlainText().mid(anchoredStart + 7, 4);
  QCOMPARE(anchored, QStringLiteral("| h1"));

  // A commit still resolves against the live source.
  auto recording = qobject_cast<RecordingPreviewWidget *>(rebuilt);
  QVERIFY(recording);
  recording->previewContext()->requestSourceReplacement(
      QStringLiteral("| h1 | h2 |\n| --- | --- |\n| z | b |"));
  QCOMPARE(recording->m_lastResult.status(), PreviewReplacementResult::Accepted);
  QVERIFY(editor.document()->toPlainText().startsWith(QStringLiteral("prefix head")));
}

void TestInteractivePreview::testReplacementRejectsExoticLineSeparators() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QLatin1String(c_table));

  auto widget = factory->m_widgets.first();
  const QString before = editor.document()->toPlainText();

  // QTextCursor::insertText() would start a new block on each of these while
  // cmark's line table only splits on '\n'.
  const QVector<QChar> separators{QChar(QLatin1Char('\r')), QChar(0x2028), QChar(0x2029)};
  for (const auto &separator : separators) {
    widget->previewContext()->requestSourceReplacement(
        QStringLiteral("| h1 | h2 |") + separator + QStringLiteral("| --- | --- |") + separator +
        QStringLiteral("| z | b |"));
    QCOMPARE(widget->m_lastResult.status(), PreviewReplacementResult::ParseFailure);
    QCOMPARE(editor.document()->toPlainText(), before);
  }
}

void TestInteractivePreview::testOversizedTableFallsBackToSource() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  // One very wide row would otherwise inflate every other row of the sheet.
  QString wide = QStringLiteral("|");
  for (int i = 0; i < 900; ++i) {
    wide += QStringLiteral(" w |");
  }

  QString text = QStringLiteral("| a |\n| --- |\n");
  for (int i = 0; i < 900; ++i) {
    text += QStringLiteral("| r |\n");
  }
  text += wide + QStringLiteral("\n");

  setTextAndSettle(editor, text);

  // No sheet is materialized; the raw Markdown stays as the only rendering.
  QVERIFY(previewWidgets(editor).isEmpty());
}

void TestInteractivePreview::testReadOnlyEditorDisablesCellEditing() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.getTextEdit()->setReadOnly(true);
  setTextAndSettle(editor, QLatin1String(c_table));

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto view = widget->findChild<QAbstractItemView *>();
  QVERIFY(view);
  // The sheet is presented as a viewer rather than swallowing edits the host
  // would reject at commit time.
  QCOMPARE(view->editTriggers(), QAbstractItemView::NoEditTriggers);

  editor.getTextEdit()->setReadOnly(false);
  editor.getHighlighter()->updateHighlight();
  QTest::qWait(50);
  QCoreApplication::processEvents();
  QVERIFY(view->editTriggers() != QAbstractItemView::NoEditTriggers);
}

void TestInteractivePreview::testNoSnapshotWorkWithoutAClaimableFactory() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  setTextAndSettle(editor, QStringLiteral("![a](b.png)\n\n```cpp\nint a;\n```\n\n$$\nx\n$$\n"));

  // Only Table has a built-in renderer, so no snapshot is produced for the
  // other types at all and the painted path is untouched.
  QSignalSpy spy(editor.getHighlighter(), &MarkdownHighlighter::previewElementsUpdated);
  editor.getHighlighter()->updateHighlight();
  QTRY_VERIFY(spy.count() > 0);

  const auto previews =
      spy.last().at(1).value<QVector<QSharedPointer<const Preview>>>();
  QVERIFY(previews.isEmpty());
  QVERIFY(previewWidgets(editor).isEmpty());

  // Registering a renderer makes the snapshots appear without a document edit.
  auto factory = new RecordingPreviewFactory({PreviewElementType::Image});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 1));
  QTest::qWait(50);
  QCoreApplication::processEvents();

  QCOMPARE(previewWidgets(editor).size(), 1);
  QCOMPARE(factory->m_widgets.first()->m_preview->type(), PreviewElementType::Image);
}

// ---------------------------------------------------------------------------
// Regressions
// ---------------------------------------------------------------------------

void TestInteractivePreview::testBackToBackReplacementsAccepted() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QLatin1String(c_table));

  auto widget = factory->m_widgets.first();
  auto context = widget->previewContext();

  context->requestSourceReplacement(
      QStringLiteral("| h1 | h2 |\n| --- | --- |\n| z | b |"));
  QCOMPARE(widget->m_lastResult.status(), PreviewReplacementResult::Accepted);

  // A second commit before the next parse generation lands. The bound snapshot
  // must describe what the first commit put in the document, otherwise this is
  // rejected as a SourceMismatch and the edit is silently dropped.
  context->requestSourceReplacement(
      QStringLiteral("| h1 | h2 |\n| --- | --- |\n| z | y |"));
  QCOMPARE(widget->m_lastResult.status(), PreviewReplacementResult::Accepted);
  QVERIFY(editor.document()->toPlainText().contains(QStringLiteral("| z | y |")));

  // And the snapshot the next parse generation delivers still matches.
  settle(editor);
  QCOMPARE(previewWidgets(editor).size(), 1);
}

void TestInteractivePreview::testBackToBackReplacementsAcceptedForNonTable() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  // Rebasing is not a table concern: any application renderer can commit twice
  // inside one parse generation.
  auto factory = new RecordingPreviewFactory({PreviewElementType::Code});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QStringLiteral("```cpp\nint a;\n```\n"));

  QCOMPARE(factory->m_widgets.size(), 1);
  auto context = factory->m_widgets.first()->previewContext();

  context->requestSourceReplacement(QStringLiteral("```cpp\nint b;\n```"));
  QCOMPARE(factory->m_widgets.first()->m_lastResult.status(),
           PreviewReplacementResult::Accepted);

  context->requestSourceReplacement(QStringLiteral("```cpp\nint c;\n```"));
  QCOMPARE(factory->m_widgets.first()->m_lastResult.status(),
           PreviewReplacementResult::Accepted);
  QVERIFY(editor.document()->toPlainText().contains(QStringLiteral("int c;")));
}

void TestInteractivePreview::testRebasedSourceSurvivesRebuild() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QLatin1String(c_table));

  const QString committed = QStringLiteral("| h1 | h2 |\n| --- | --- |\n| z | b |");
  factory->m_widgets.first()->previewContext()->requestSourceReplacement(committed);
  QCOMPARE(factory->m_widgets.first()->m_lastResult.status(),
           PreviewReplacementResult::Accepted);

  // A factory change rebuilds every item by replaying the parse generation,
  // which still holds the pre-commit source, and forces a re-emit of that same
  // generation on top. The binding has to keep describing what the live anchor
  // actually spans. Only the queued rebuild is awaited here: a fresh parse
  // generation would repair the state and mask the defect.
  auto other = new RecordingPreviewFactory({PreviewElementType::Image});
  QVERIFY(editor.registerPreviewWidgetFactory(other, 1));
  QCoreApplication::processEvents();
  QCoreApplication::processEvents();

  QVERIFY(!factory->m_widgets.isEmpty());
  auto rebuilt = factory->m_widgets.last();
  QVERIFY(rebuilt->m_preview);
  QCOMPARE(rebuilt->m_preview->sourceMarkdown(), committed);

  // And a further commit is still accepted against it.
  rebuilt->previewContext()->requestSourceReplacement(
      QStringLiteral("| h1 | h2 |\n| --- | --- |\n| z | y |"));
  QCOMPARE(rebuilt->m_lastResult.status(), PreviewReplacementResult::Accepted);
  QVERIFY(editor.document()->toPlainText().contains(QStringLiteral("| z | y |")));
}

void TestInteractivePreview::testReadOnlyToggleReachesLiveSheets() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  setTextAndSettle(editor, QLatin1String(c_table));

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto view = widget->findChild<QAbstractItemView *>();
  QVERIFY(view);
  QVERIFY(view->editTriggers() != QAbstractItemView::NoEditTriggers);

  // QTextEdit::setReadOnly() emits no signal and changes no content, so nothing
  // would republish. The sheet still has to stop offering edits immediately.
  editor.getTextEdit()->setReadOnly(true);
  QCOMPARE(view->editTriggers(), QAbstractItemView::NoEditTriggers);

  editor.getTextEdit()->setReadOnly(false);
  QVERIFY(view->editTriggers() != QAbstractItemView::NoEditTriggers);
}

void TestInteractivePreview::testRaggedTableIsNotEditable() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  // The body row carries one cell more than the header declares. GFM ignores
  // the excess, so writing the sheet back must not promote it into the header.
  const QString ragged = QStringLiteral("| h1 | h2 |\n| --- | --- |\n| a | b | c |\n");
  setTextAndSettle(editor, ragged);

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto view = qobject_cast<QTableView *>(widget->findChild<QAbstractItemView *>());
  QVERIFY(view);

  // The extra cell stays visible, so nothing is hidden from the user.
  QCOMPARE(view->model()->columnCount(), 3);

  // But the sheet is a viewer: it cannot be written back without changing what
  // the table renders to.
  QCOMPARE(view->editTriggers(), QAbstractItemView::NoEditTriggers);

  // Even a programmatic edit leaves the document untouched.
  const QString before = editor.document()->toPlainText();
  QVERIFY(view->model()->setData(view->model()->index(2 % view->model()->rowCount(), 0),
                                 QStringLiteral("zz"), Qt::EditRole));
  QCoreApplication::processEvents();
  QCOMPARE(editor.document()->toPlainText(), before);
}

void TestInteractivePreview::testCommitKeepsCellEditorAcrossNextParse() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  setTextAndSettle(editor, QLatin1String(c_table));

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto view = qobject_cast<QTableView *>(widget->findChild<QAbstractItemView *>());
  QVERIFY(view);
  auto model = view->model();

  QVERIFY(model->setData(model->index(1, 0), QStringLiteral("zz"), Qt::EditRole));
  QCoreApplication::processEvents();
  QVERIFY(editor.document()->toPlainText().contains(QStringLiteral("zz")));

  // Select a cell, then let the parse generation which merely echoes this
  // sheet's own commit arrive. A model reset here would drop the selection and
  // destroy any editor the user has already opened.
  view->setCurrentIndex(model->index(1, 1));
  QVERIFY(view->currentIndex().isValid());

  settle(editor);

  QCOMPARE(singlePreviewWidget(editor), widget);
  QCOMPARE(view->currentIndex(), model->index(1, 1));
  QCOMPARE(model->data(model->index(1, 0), Qt::DisplayRole).toString(), QStringLiteral("zz"));
}

void TestInteractivePreview::testTableIsOptInByDefault() {
  // A consumer which never asks for it must not get a preview which can
  // rewrite the document.
  auto textConfig = QSharedPointer<TextEditorConfig>::create();
  auto config = QSharedPointer<MarkdownEditorConfig>::create(textConfig);
  QVERIFY(!(config->m_inplacePreviewSources & MarkdownEditorConfig::Table));

  VMarkdownEditor editor(config, QSharedPointer<TextEditorParameters>::create());
  setTextAndSettle(editor, QLatin1String(c_table));
  QVERIFY(previewWidgets(editor).isEmpty());
}

void TestInteractivePreview::testMultiLineImageIsStandalone() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Image});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));

  // The construct spans two source lines but nothing else shares them, which
  // the painted path classifies as a block image. The widget path has to place
  // it the same way, not as an inline element above the line.
  setTextAndSettle(editor, QStringLiteral("![alt\ntext](img.png)\n"));

  QCOMPARE(factory->m_widgets.size(), 1);
  auto preview = factory->m_widgets.first()->m_preview;
  QVERIFY(preview);
  QCOMPARE(preview->placement(), PreviewPlacement::BlockAfterSource);
}

void TestInteractivePreview::testTableSheetRefitsAfterFontChange() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.resize(600, 400);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  setTextAndSettle(editor,
                   QStringLiteral("| header | header |\n| --- | --- |\n| haha | haha |\n"));

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto view = qobject_cast<QTableView *>(widget->findChild<QAbstractItemView *>());
  QVERIFY(view);
  auto model = view->model();
  QCOMPARE(model->columnCount(), 2);

  QTest::qWait(50);
  QCoreApplication::processEvents();

  auto fits = [&](const char *p_stage) {
    // The planned sections are authoritative; re-measuring them here would
    // overwrite the very layout under test.
    int total = 0;
    for (int c = 0; c < model->columnCount(); ++c) {
      total += view->columnWidth(c);
    }

    QVERIFY2(view->viewport()->width() >= total,
             qPrintable(QStringLiteral("%1: viewport width %2 < total column width %3")
                            .arg(QLatin1String(p_stage))
                            .arg(view->viewport()->width())
                            .arg(total)));
    QVERIFY2(view->horizontalScrollBar()->value() == 0,
             qPrintable(QStringLiteral("%1: the view scrolled horizontally, so the first column "
                                       "is clipped")
                            .arg(QLatin1String(p_stage))));
  };

  fits("initial");
  if (QTest::currentTestFailed()) {
    return;
  }

  // Growing a cell in the source, one keystroke at a time, is what an actual
  // edit session looks like. The reserved band has to keep up.
  QTextCursor cursor(editor.document());
  cursor.setPosition(editor.document()->toPlainText().indexOf(QStringLiteral("haha")) + 4);
  for (int i = 0; i < 12; ++i) {
    cursor.insertText(QStringLiteral("x"));
    settle(editor);

    fits("while typing");
    if (QTest::currentTestFailed()) {
      return;
    }
  }

  // The editor's font is what the sheet inherits, and an application may apply
  // its theme after the first measurement.
  QFont bigger = editor.getTextEdit()->font();
  bigger.setPointSize(bigger.pointSize() + 8);
  editor.getTextEdit()->setFont(bigger);

  QTest::qWait(100);
  QCoreApplication::processEvents();

  fits("after the font grew");
}

void TestInteractivePreview::testTableSheetFitsWithALargeThemeFont() {
  auto textConfig = QSharedPointer<TextEditorConfig>::create();
  textConfig->m_theme =
      Theme::createThemeFromFile(QStringLiteral(":/vtextedit/editor/data/themes/default.theme"));
  QVERIFY(textConfig->m_theme);

  auto config = QSharedPointer<MarkdownEditorConfig>::create(textConfig);
  config->m_inplacePreviewSources |= MarkdownEditorConfig::Table;
  // After construction: that is where overrideTextStyle() replaces the whole
  // generic text format with the Markdown one.
  config->overrideTextFontFamily(QStringLiteral("Courier New"));
  textConfig->m_theme->editorStyle(Theme::EditorStyle::Text).m_fontPointSize = 20;

  VMarkdownEditor editor(config, QSharedPointer<TextEditorParameters>::create());
  editor.resize(600, 400);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  setTextAndSettle(editor, QStringLiteral("| a | b |\n| --- | --- |\n| hello | hello |\n"));

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto view = qobject_cast<QTableView *>(widget->findChild<QAbstractItemView *>());
  QVERIFY(view);

  QTest::qWait(50);
  QCoreApplication::processEvents();

  // A theme font much larger than the application default is the case which
  // originally under-reserved: the sheet measured itself with the 9pt default
  // while being painted with the theme font, the stretched last section
  // absorbed the shortfall, and clicking it scrolled the first column away.
  QCOMPARE(view->font().pointSize(), 20);

  auto model = view->model();
  int totalColumnWidth = 0;
  for (int c = 0; c < model->columnCount(); ++c) {
    totalColumnWidth += view->columnWidth(c);
  }

  QVERIFY2(view->viewport()->width() >= totalColumnWidth,
           qPrintable(QStringLiteral("viewport width %1 < total column width %2")
                          .arg(view->viewport()->width())
                          .arg(totalColumnWidth)));
  QCOMPARE(view->horizontalScrollBar()->value(), 0);
}

void TestInteractivePreview::testTableSheetUsesTheThemeGenericFont() {
  auto textConfig = QSharedPointer<TextEditorConfig>::create();
  // A private theme instance: the default one is shared process wide.
  textConfig->m_theme =
      Theme::createThemeFromFile(QStringLiteral(":/vtextedit/editor/data/themes/default.theme"));
  QVERIFY(textConfig->m_theme);

  auto config = QSharedPointer<MarkdownEditorConfig>::create(textConfig);
  config->m_inplacePreviewSources |= MarkdownEditorConfig::Table;
  // Applied after construction, which is where overrideTextStyle() replaces the
  // generic text style with the Markdown one. A comma is what discriminates the
  // two candidate rules here: overrideTextFontFamily() writes the application
  // string straight into the single already-resolved family field, so a host
  // which re-splits it on ',' would measure with "Courier New" while the style
  // sheet declares the whole string.
  config->overrideTextFontFamily(QStringLiteral("Courier New, A"));

  VMarkdownEditor editor(config, QSharedPointer<TextEditorParameters>::create());
  editor.resize(600, 400);
  setTextAndSettle(editor, QStringLiteral("| a | b |\n| --- | --- |\n| hello | hello |\n"));

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto view = qobject_cast<QTableView *>(widget->findChild<QAbstractItemView *>());
  QVERIFY(view);

  const auto &generic = editor.theme()->editorStyle(Theme::EditorStyle::Text);
  QCOMPARE(generic.m_fontFamily, QStringLiteral("Courier New, A"));

  // Before the editor is shown its style sheet has not been polished, so
  // VTextEdit::font() is still the application default. The sheet has to take
  // the font from the theme, otherwise the very first measurement - the one
  // the layout reserves space from - is made with the wrong metrics.
  QVERIFY2(view->font().family() == generic.m_fontFamily,
           qPrintable(QStringLiteral("sheet font %1 is not the theme's generic font %2")
                          .arg(view->font().family(), generic.m_fontFamily)));

  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  QTest::qWait(50);
  QCoreApplication::processEvents();

  QCOMPARE(view->font().family(), generic.m_fontFamily);
  QCOMPARE(view->font().pointSize(), editor.editorFontPointSize());

  // Zooming re-sizes the editor, and the sheet has to follow it.
  const int before = view->font().pointSize();
  editor.zoom(4);
  QTest::qWait(50);
  QCoreApplication::processEvents();

  QCOMPARE(view->font().pointSize(), editor.editorFontPointSize());
  QVERIFY2(view->font().pointSize() > before,
           qPrintable(QStringLiteral("point size %1 did not grow from %2")
                          .arg(view->font().pointSize())
                          .arg(before)));
}

void TestInteractivePreview::testTableSheetFitsWithoutScrollBars() {
  // Both a table whose cells are wider than the header's minimum section size
  // and one whose cells are narrower than it: the reserved band has to cover
  // the sizes the headers actually assign, not the bare content hints.
  const QVector<QString> tables{
      QStringLiteral("| header1 | header2 |\n| --- | --- |\n| hahaha | hahahha |\n"),
      QStringLiteral("| a | b |\n| --- | --- |\n| c | d |\n")};

  for (const auto &table : tables) {
    VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
    editor.resize(600, 400);
    editor.show();
    QVERIFY(QTest::qWaitForWindowExposed(&editor));

    // A small table which comfortably fits the default visible row budget.
    setTextAndSettle(editor, table);

    auto widget = singlePreviewWidget(editor);
    QVERIFY(widget);
    auto view = qobject_cast<QTableView *>(widget->findChild<QAbstractItemView *>());
    QVERIFY(view);
    auto model = view->model();
    QVERIFY(model);
    QCOMPARE(model->rowCount(), 2);
    QCOMPARE(model->columnCount(), 2);

    QTest::qWait(50);
    QCoreApplication::processEvents();

    const QSize viewportSize = view->viewport()->size();

    // Even a hidden vertical header floors every row at its minimum section
    // size, which can exceed what the delegate reports.
    int totalRowHeight = 0;
    for (int r = 0; r < model->rowCount(); ++r) {
      totalRowHeight += view->rowHeight(r);
    }
    QVERIFY2(viewportSize.height() >= totalRowHeight,
             qPrintable(QStringLiteral("viewport height %1 < total row height %2")
                            .arg(viewportSize.height())
                            .arg(totalRowHeight)));

    // Otherwise a scroll bar appears and eats the width the last, stretched
    // column needs, clipping its contents.
    QVERIFY(!view->verticalScrollBar()->isVisible());

    // The sheet plans every section itself, so the assigned widths are what
    // has to fit; normalizing them to the content hints first would test a
    // layout the sheet never uses.
    int totalColumnWidth = 0;
    for (int c = 0; c < model->columnCount(); ++c) {
      totalColumnWidth += view->columnWidth(c);
    }
    QVERIFY2(viewportSize.width() >= totalColumnWidth,
             qPrintable(QStringLiteral("viewport width %1 < total column width %2")
                            .arg(viewportSize.width())
                            .arg(totalColumnWidth)));
  }
}

// ---------------------------------------------------------------------------
// Sheet geometry
// ---------------------------------------------------------------------------

namespace {
// TablePreviewWidget::c_widthFraction, which lives in an internal header this
// target deliberately cannot reach.
const qreal c_expectedWidthFraction = 0.9;

// The reserved band is a qreal rectangle turned into widget geometry with
// toAlignedRect(), which can round outwards by a pixel on either edge.
const int c_widthTolerance = 2;

QTableView *sheetView(PreviewWidget *p_widget) {
  return qobject_cast<QTableView *>(p_widget->findChild<QAbstractItemView *>());
}

int totalColumnWidth(QTableView *p_view) {
  int total = 0;
  for (int c = 0; c < p_view->model()->columnCount(); ++c) {
    total += p_view->columnWidth(c);
  }

  return total;
}

int totalRowHeight(QTableView *p_view) {
  int total = 0;
  for (int r = 0; r < p_view->model()->rowCount(); ++r) {
    total += p_view->rowHeight(r);
  }

  return total;
}


// A table with p_columns short columns, so every column ends up at the sheet's
// readable floor and the only thing which decides whether the sheet overflows
// is how many of those floors fit in the text column.
QString makeColumnTable(int p_columns) {
  QString header;
  QString delimiter;
  QString body;
  for (int c = 0; c < p_columns; ++c) {
    header += QStringLiteral("| c%1 ").arg(c);
    delimiter += QStringLiteral("| --- ");
    body += QStringLiteral("| v%1 ").arg(c);
  }

  return header + QStringLiteral("|\n") + delimiter + QStringLiteral("|\n") + body +
         QStringLiteral("|\n");
}

// The number of columns the sheet is built with in the overflow scenarios.
// Wide enough that their floors cannot fit a 420 pixel editor whatever the
// platform font is, since the floor is at least twelve average characters.
const int c_overflowColumns = 12;

// The whole width contract in one expression: a sheet narrower than the fill
// grows to it, a wider one keeps its natural width, and one wider than the
// text column is clamped to it.
int expectedSheetWidth(qreal p_available, int p_natural) {
  return qRound(qBound(p_available * c_expectedWidthFraction, qreal(p_natural), p_available));
}
} // namespace

void TestInteractivePreview::testTableSheetSpansContentWidth() {
  // The two outer branches of the contract: far below the fill, and beyond the
  // text column. The middle one only exists at one particular editor width and
  // has a test of its own.
  const QVector<QString> tables{
      QStringLiteral("| a | b |\n| --- | --- |\n| c | d |\n"),
      QStringLiteral("| h |\n| --- |\n| ") + QString(300, QLatin1Char('x')) +
          QStringLiteral(" |\n")};

  for (int i = 0; i < tables.size(); ++i) {
    VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
    editor.resize(600, 400);
    editor.show();
    QVERIFY(QTest::qWaitForWindowExposed(&editor));
    setTextAndSettle(editor, tables.at(i));

    auto widget = singlePreviewWidget(editor);
    QVERIFY(widget);
    QVERIFY(widget->previewContext());

    const qreal available = widget->previewContext()->availableContentRect().width();
    QVERIFY(available > 0);

    // Unchanged by the fill: the measurement derives from the content hints,
    // never from the assigned geometry.
    const int natural = widget->sizeHint().width();

    // Assert the branch this source is meant to exercise, otherwise a drifting
    // font could silently turn both rows into the same case.
    if (i == 0) {
      QVERIFY2(natural < available * c_expectedWidthFraction,
               qPrintable(QStringLiteral("the narrow table is not narrow enough: %1 vs %2")
                              .arg(natural)
                              .arg(available * c_expectedWidthFraction)));
    } else {
      QVERIFY2(natural > available,
               qPrintable(QStringLiteral("the wide table is not wider than the text column: "
                                         "%1 vs %2")
                              .arg(natural)
                              .arg(available)));
    }

    const int expected = expectedSheetWidth(available, natural);
    QVERIFY2(qAbs(widget->width() - expected) <= c_widthTolerance,
             qPrintable(QStringLiteral("table %1: sheet width %2 is not the expected %3 "
                                       "(natural %4, available %5)")
                            .arg(i)
                            .arg(widget->width())
                            .arg(expected)
                            .arg(natural)
                            .arg(available)));

    if (i == 0) {
      // A no-op implementation would still satisfy the clamp above for the
      // tables which are naturally wide enough.
      QVERIFY2(widget->width() > natural,
               qPrintable(QStringLiteral("the narrow sheet was not widened: %1 <= %2")
                              .arg(widget->width())
                              .arg(natural)));
    }
  }
}

void TestInteractivePreview::testClickEditsACellInPlace() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.resize(1100, 500);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));

  setTextAndSettle(editor,
                   QStringLiteral("| Left | Center | Right |\n"
                                  "|:-----|:------:|------:|\n"
                                  "| *italic* | **bold** | `code` |\n"));
  QTest::qWait(50);
  QCoreApplication::processEvents();

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto view = sheetView(widget);
  QVERIFY(view);
  QVERIFY(view->viewport()->findChildren<QLineEdit *>().isEmpty());

  // One click inside the embedded sheet, near the start of the cell's text.
  const QModelIndex index = view->model()->index(1, 1);
  const QRect cell = view->visualRect(index);
  const QPoint pos(cell.left() + 6, cell.center().y());
  QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, pos);
  QTest::qWait(50);
  QCoreApplication::processEvents();

  const auto editors = view->viewport()->findChildren<QLineEdit *>();
  QVERIFY2(editors.size() == 1, "one click did not start editing the cell");

  auto cellEditor = editors.first();
  QCOMPARE(cellEditor->text(), QStringLiteral("**bold**"));
  // Nothing selected, so the value is not sitting there waiting to be wiped by
  // the next keystroke - and nothing can be hidden behind an unreadable
  // selection colour either.
  QVERIFY(cellEditor->selectedText().isEmpty());
  QVERIFY2(cellEditor->cursorPosition() < cellEditor->text().size(),
           qPrintable(QStringLiteral("the caret went to the end (%1) rather than the click")
                          .arg(cellEditor->cursorPosition())));

  // Typing inserts at the caret, and the commit reaches the source.
  QTest::keyClicks(cellEditor, QStringLiteral("X"));
  QCOMPARE(cellEditor->text().size(), QStringLiteral("**bold**").size() + 1);
  QTest::keyClick(cellEditor, Qt::Key_Return);
  QTest::qWait(80);
  QCoreApplication::processEvents();

  QVERIFY2(editor.getTextEdit()->toPlainText().contains(QStringLiteral("bold")),
           "the committed cell lost its value");
}
void TestInteractivePreview::testTableSheetHeightMatchesItsRows() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.resize(420, 600);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));

  // Cells far longer than the column they can be given: the sheet compresses
  // the columns into the text column and wraps what no longer fits, so the
  // rows have to be measured against the widths they actually got.
  setTextAndSettle(
      editor,
      QStringLiteral("| Component | Description | Supported |\n"
                     "|-----------|-------------|:---------:|\n"
                     "| `VTextEdit` | Base edit widget with cursor and selection | Yes |\n"
                     "| `VTextEditor` | Adds syntax highlight, Vi mode and folding | Yes |\n"
                     "| `VMarkdownEditor` | Markdown parsing and in-place preview | Yes |\n"));
  QTest::qWait(50);
  QCoreApplication::processEvents();

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto view = sheetView(widget);
  QVERIFY(view);
  QCOMPARE(view->model()->rowCount(), 4);

  int shortest = INT_MAX;
  int tallest = 0;
  for (int r = 0; r < view->model()->rowCount(); ++r) {
    const int height = view->rowHeight(r);
    shortest = qMin(shortest, height);
    tallest = qMax(tallest, height);
  }

  // The whole point of the narrow editor: a description cell no longer fits on
  // one line and is shown in full instead of being elided.
  QVERIFY2(tallest >= 2 * shortest,
           qPrintable(QStringLiteral("nothing wrapped: tallest %1, shortest %2")
                          .arg(tallest)
                          .arg(shortest)));

  // No empty strip under the last row, and no row hidden under the bar: the
  // viewport is exactly the rows, and the band the host reserved is exactly
  // the sheet.
  const int rows = totalRowHeight(view);
  QCOMPARE(view->viewport()->height(), rows);
  QCOMPARE(widget->height(), view->height());
  QVERIFY(!view->verticalScrollBar()->isVisible());

  // Given room, the same table stops wrapping and the band shrinks with it.
  const int tallBand = widget->height();
  editor.resize(1100, 600);
  settle(editor);
  QTest::qWait(50);
  QCoreApplication::processEvents();

  int wideShortest = INT_MAX;
  int wideTallest = 0;
  for (int r = 0; r < view->model()->rowCount(); ++r) {
    const int height = view->rowHeight(r);
    wideShortest = qMin(wideShortest, height);
    wideTallest = qMax(wideTallest, height);
  }

  QVERIFY2(wideTallest < 2 * wideShortest,
           qPrintable(QStringLiteral("a row still wraps at full width: tallest %1, shortest %2")
                          .arg(wideTallest)
                          .arg(wideShortest)));
  QVERIFY2(widget->height() < tallBand,
           qPrintable(QStringLiteral("the band did not shrink: %1 vs %2")
                          .arg(widget->height())
                          .arg(tallBand)));
  QCOMPARE(view->viewport()->height(), totalRowHeight(view));
}

void TestInteractivePreview::testWideTableSheetScrollsHorizontally() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.resize(420, 400);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));

  // More columns than their readable floors can fit: compressing further would
  // turn every column into a ribbon of single letters, so the sheet stops and
  // the overflow is reached by scrolling instead.
  setTextAndSettle(editor, makeColumnTable(c_overflowColumns));
  QTest::qWait(50);
  QCoreApplication::processEvents();

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto view = sheetView(widget);
  QVERIFY(view);
  QCOMPARE(view->model()->columnCount(), c_overflowColumns);

  const qreal available = widget->previewContext()->availableContentRect().width();
  QVERIFY2(qAbs(widget->width() - qRound(available)) <= c_widthTolerance,
           qPrintable(QStringLiteral("the sheet was not clamped to the text column: %1 vs %2")
                          .arg(widget->width())
                          .arg(available)));

  // Every column sits at the same floor, which is what "cannot compress any
  // further" means.
  const int floor = view->columnWidth(0);
  for (int c = 1; c < c_overflowColumns; ++c) {
    QCOMPARE(view->columnWidth(c), floor);
  }
  QVERIFY2(totalColumnWidth(view) > view->viewport()->width(),
           qPrintable(QStringLiteral("the columns do not overflow: %1 vs viewport %2")
                          .arg(totalColumnWidth(view))
                          .arg(view->viewport()->width())));

  // The overflow is reachable rather than silently clipped.
  auto hbar = view->horizontalScrollBar();
  QVERIFY(hbar->isVisible());
  QVERIFY2(hbar->maximum() > 0,
           qPrintable(QStringLiteral("the sheet does not scroll: maximum %1").arg(hbar->maximum())));

  // The band reserves the bar's height, so it covers no row. How much the bar
  // costs is style dependent; that it costs *something* is what
  // testSheetReservesTheScrollBarAcrossAResize() pins down, by comparing the
  // band with and without it.
  const int rows = totalRowHeight(view);
  QCOMPARE(view->viewport()->height(), rows);
  QCOMPARE(widget->height(), view->height());

  // Scrolling to the end brings the last column fully into view.
  const int last = view->model()->columnCount() - 1;
  hbar->setValue(hbar->maximum());
  QCoreApplication::processEvents();
  QVERIFY2(view->columnViewportPosition(last) >= 0 &&
               view->columnViewportPosition(last) + view->columnWidth(last) <=
                   view->viewport()->width(),
           qPrintable(QStringLiteral("the last column is not reachable: x %1, width %2, "
                                     "viewport %3")
                          .arg(view->columnViewportPosition(last))
                          .arg(view->columnWidth(last))
                          .arg(view->viewport()->width())));
}

void TestInteractivePreview::testASingleOverflowingColumnScrollsToItsEnd() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.resize(420, 400);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));

  // One cell far wider than the whole sheet, and with no word boundary to
  // break on. Qt's own item delegates wrap on word boundaries only, so this is
  // exactly the cell which would otherwise be elided and unreachable.
  setTextAndSettle(editor, QStringLiteral("| h |\n| --- |\n| ") +
                               QString(300, QLatin1Char('x')) + QStringLiteral(" |\n"));
  QTest::qWait(50);
  QCoreApplication::processEvents();

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto view = sheetView(widget);
  QVERIFY(view);
  QCOMPARE(view->model()->columnCount(), 1);

  // A single column always fits, so nothing scrolls sideways any more.
  QVERIFY(!view->horizontalScrollBar()->isVisible());
  QCOMPARE(view->columnWidth(0), view->viewport()->width());

  // The whole token is shown, on as many lines as it takes.
  QVERIFY2(view->rowHeight(1) >= 3 * view->rowHeight(0),
           qPrintable(QStringLiteral("the unbreakable cell was not wrapped: %1 vs header %2")
                          .arg(view->rowHeight(1))
                          .arg(view->rowHeight(0))));

  // And the band is exactly those rows, with nothing scrolled out of view.
  QCOMPARE(view->viewport()->height(), totalRowHeight(view));
  QVERIFY(!view->verticalScrollBar()->isVisible());
}

namespace {
// Deliver a wheel movement the way the platform would, and report whether it
// was consumed. An unconsumed event is what Qt then propagates to the editor.
bool sendWheel(QWidget *p_target, const QPoint &p_angleDelta, Qt::KeyboardModifiers p_modifiers) {
  const QPointF pos(p_target->width() / 2.0, p_target->height() / 2.0);
  QWheelEvent event(pos, p_target->mapToGlobal(pos.toPoint()), QPoint(), p_angleDelta,
                    Qt::NoButton, p_modifiers, Qt::NoScrollPhase, false);
  event.setAccepted(false);
  QCoreApplication::sendEvent(p_target, &event);
  return event.isAccepted();
}
} // namespace

void TestInteractivePreview::testWideSheetConsumesHorizontalWheel() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.resize(420, 400);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  // The columns only overflow once their readable floors no longer fit, so the
  // scenario is built from column count rather than from cell length.
  setTextAndSettle(editor, makeColumnTable(c_overflowColumns));
  QTest::qWait(50);
  QCoreApplication::processEvents();

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto view = sheetView(widget);
  QVERIFY(view);

  auto hbar = view->horizontalScrollBar();
  QVERIFY(hbar->isVisible());
  QVERIFY(hbar->maximum() > 0);
  QCOMPARE(hbar->value(), 0);

  // A horizontal movement belongs to the sheet while it still has room.
  QVERIFY(sendWheel(view->viewport(), QPoint(-120, 0), Qt::NoModifier));
  QVERIFY2(hbar->value() > 0,
           qPrintable(QStringLiteral("a horizontal wheel did not scroll the sheet: %1")
                          .arg(hbar->value())));

  // Shift is how a plain wheel asks for horizontal movement. Qt routes an
  // event by its dominant axis, so this would otherwise scroll vertically. A
  // small stray horizontal component must not defeat the recognition either.
  hbar->setValue(0);
  QVERIFY(sendWheel(view->viewport(), QPoint(-8, -120), Qt::ShiftModifier));
  QVERIFY2(hbar->value() > 0,
           qPrintable(QStringLiteral("shift+wheel did not scroll the sheet: %1").arg(hbar->value())));

  // Control is the editor's zoom gesture and is never consumed here, however
  // much room the sheet has.
  hbar->setValue(0);
  QVERIFY2(!sendWheel(view->viewport(), QPoint(0, -120), Qt::ControlModifier | Qt::ShiftModifier),
           "ctrl+shift+wheel was swallowed instead of reaching the editor");
  QCOMPARE(hbar->value(), 0);
  QVERIFY2(!sendWheel(view->viewport(), QPoint(-120, 0), Qt::ControlModifier),
           "ctrl+horizontal wheel was swallowed instead of reaching the editor");
  QCOMPARE(hbar->value(), 0);

  // At the end of the range the movement is handed back rather than swallowed.
  hbar->setValue(hbar->maximum());
  QVERIFY2(!sendWheel(view->viewport(), QPoint(-120, 0), Qt::NoModifier),
           "the exhausted sheet swallowed the movement");
  QCOMPARE(hbar->value(), hbar->maximum());
}

void TestInteractivePreview::testSheetReservesTheScrollBarAcrossAResize() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.resize(420, 400);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  setTextAndSettle(editor, makeColumnTable(c_overflowColumns));
  QTest::qWait(50);
  QCoreApplication::processEvents();

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto view = sheetView(widget);
  QVERIFY(view);

  // The reserve is decided against the width the band ends up with. That width
  // is only published into the widget's geometry context after the host has
  // already measured, so a measurement which read it from there would answer
  // for the previous editor size and stay cached under the new one.
  QVERIFY2(view->horizontalScrollBar()->isVisible(),
           "the summed column floors were expected to overflow a 420 pixel editor");
  QCOMPARE(view->viewport()->height(), totalRowHeight(view));

  const int overflowingHeight = widget->height();
  // Every column sits at its floor here, so the width the sheet needs to stop
  // overflowing follows from the runtime floor rather than from a guess.
  const int floor = view->columnWidth(0);
  const qreal available = widget->previewContext()->availableContentRect().width();
  const int missing = qRound(c_overflowColumns * floor + 40 - available);
  QVERIFY(missing > 0);

  editor.resize(editor.width() + missing, 400);
  settle(editor);
  QTest::qWait(50);
  QCoreApplication::processEvents();

  QVERIFY2(!view->horizontalScrollBar()->isVisible(), "widening did not drop the scroll bar");
  QCOMPARE(view->viewport()->height(), totalRowHeight(view));
  QVERIFY2(widget->height() < overflowingHeight,
           qPrintable(QStringLiteral("the band kept the reserve it no longer needs: %1 vs %2")
                          .arg(widget->height())
                          .arg(overflowingHeight)));
  QCOMPARE(widget->height(), 2 * view->frameWidth() + totalRowHeight(view));

  // And narrowing it back: the reserve has to be taken again.
  editor.resize(420, 400);
  settle(editor);
  QTest::qWait(50);
  QCoreApplication::processEvents();

  QVERIFY2(view->horizontalScrollBar()->isVisible(), "narrowing did not bring up the scroll bar");
  QVERIFY2(view->viewport()->height() >= totalRowHeight(view),
           qPrintable(QStringLiteral("the scroll bar covers a row after narrowing: viewport %1, "
                                     "rows %2")
                          .arg(view->viewport()->height())
                          .arg(totalRowHeight(view))));
  QCOMPARE(widget->height(), overflowingHeight);
}

void TestInteractivePreview::testTableSheetKeepsANaturalWidthInsideTheBand() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.resize(600, 400);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  setTextAndSettle(editor,
                   QStringLiteral("| header one | header two |\n| --- | --- |\n"
                                  "| a fairly long cell value | another long cell value |\n"));

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  const int natural = widget->sizeHint().width();
  QVERIFY(natural > 0);

  // A table whose natural width already sits between the fill and the full
  // text column keeps that width. Which editor size that is depends on the
  // font, so calibrate the editor to the table: aim for the middle of the
  // band, leaving 5% of slack on either side.
  const qreal before = widget->previewContext()->availableContentRect().width();
  const qreal target = natural / 0.95;
  editor.resize(qRound(editor.width() + target - before), 400);
  settle(editor);
  QTest::qWait(50);
  QCoreApplication::processEvents();

  const qreal available = widget->previewContext()->availableContentRect().width();
  QCOMPARE(widget->sizeHint().width(), natural);
  QVERIFY2(natural >= available * c_expectedWidthFraction && natural <= available,
           qPrintable(QStringLiteral("the calibration missed the band: natural %1, available %2")
                          .arg(natural)
                          .arg(available)));

  // Neither shrunk to the natural width of a narrower sheet nor forced to the
  // fill: a "every fitting table is exactly 90%" implementation fails here.
  QVERIFY2(qAbs(widget->width() - natural) <= c_widthTolerance,
           qPrintable(QStringLiteral("sheet width %1 is not the natural %2 (available %3)")
                          .arg(widget->width())
                          .arg(natural)
                          .arg(available)));
}

void TestInteractivePreview::testInlinePreviewIgnoresTheWidthFraction() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Image});
  factory->m_hint = QSize(40, 20);
  factory->m_widthFraction = c_expectedWidthFraction;
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));

  editor.resize(600, 400);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  setTextAndSettle(editor, QStringLiteral("lead ![a](b.png) trail\n"));

  QCOMPARE(factory->m_widgets.size(), 1);
  auto widget = factory->m_widgets.first();
  QVERIFY(widget->m_preview);
  QCOMPARE(widget->m_preview->placement(), PreviewPlacement::InlineAboveLine);

  // An inline preview is bound to the text span it replaces, whatever fraction
  // it declares. The host restricts the fill by placement rather than by "the
  // span width came back unset", because an inline span whose width could not
  // be resolved reaches the same branch and must not be widened either - that
  // sub-case is prevented by construction, not covered here.
  const qreal available = widget->previewContext()->availableContentRect().width();
  QVERIFY(available > 0);
  QVERIFY2(widget->width() < available * c_expectedWidthFraction,
           qPrintable(QStringLiteral("the inline preview was widened to %1 of an available %2")
                          .arg(widget->width())
                          .arg(available)));
}

void TestInteractivePreview::testTableColumnsShareTheExtraWidth() {
  // Equal contents must stay equal, and unequal contents must keep their
  // order: handing the whole surplus to the stretched last section breaks
  // both.
  const QVector<QString> tables{
      QStringLiteral("| aaaa | aaaa |\n| --- | --- |\n| bbbb | bbbb |\n"),
      QStringLiteral("| aaaaaaaaaaaaaaaa | b |\n| --- | --- |\n| cccccccccccccccc | d |\n")};

  for (int i = 0; i < tables.size(); ++i) {
    VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
    editor.resize(600, 400);
    editor.show();
    QVERIFY(QTest::qWaitForWindowExposed(&editor));
    setTextAndSettle(editor, tables.at(i));

    auto widget = singlePreviewWidget(editor);
    QVERIFY(widget);
    auto view = sheetView(widget);
    QVERIFY(view);
    QCOMPARE(view->model()->columnCount(), 2);

    QTest::qWait(50);
    QCoreApplication::processEvents();

    const int viewportWidth = view->viewport()->width();
    QVERIFY2(qAbs(totalColumnWidth(view) - viewportWidth) <= c_widthTolerance,
             qPrintable(QStringLiteral("table %1: columns total %2 do not cover the viewport %3")
                            .arg(i)
                            .arg(totalColumnWidth(view))
                            .arg(viewportWidth)));

    const int first = view->columnWidth(0);
    const int second = view->columnWidth(1);
    if (i == 0) {
      QVERIFY2(qAbs(first - second) <= c_widthTolerance,
               qPrintable(QStringLiteral("equal columns were distributed unequally: %1 vs %2")
                              .arg(first)
                              .arg(second)));
    } else {
      QVERIFY2(first > second,
               qPrintable(QStringLiteral("the wide column %1 did not stay wider than %2")
                              .arg(first)
                              .arg(second)));
    }
  }
}

void TestInteractivePreview::testSingleColumnTableFillsTheSheet() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.resize(600, 400);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  setTextAndSettle(editor, QStringLiteral("| only |\n| --- |\n| a |\n"));

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto view = sheetView(widget);
  QVERIFY(view);
  QCOMPARE(view->model()->columnCount(), 1);

  QTest::qWait(50);
  QCoreApplication::processEvents();

  // The sheet has to have been widened at all: a single column already filled
  // the viewport of its natural-width sheet through the stretched last
  // section, so the column assertion below cannot detect the fill on its own.
  const int natural = widget->sizeHint().width();
  QVERIFY2(widget->width() > natural,
           qPrintable(QStringLiteral("the sheet was not widened: %1 <= %2")
                          .arg(widget->width())
                          .arg(natural)));

  // The case a "distribute over the first count - 1 columns" loop never
  // touches at all.
  QVERIFY2(qAbs(view->columnWidth(0) - view->viewport()->width()) <= c_widthTolerance,
           qPrintable(QStringLiteral("the only column %1 does not cover the viewport %2")
                          .arg(view->columnWidth(0))
                          .arg(view->viewport()->width())));
}

void TestInteractivePreview::testTableWidthFollowsEditorResize() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  editor.resize(600, 400);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  setTextAndSettle(editor, QStringLiteral("| a | b |\n| --- | --- |\n| c | d |\n"));

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto view = sheetView(widget);
  QVERIFY(view);

  const qreal availableBefore = widget->previewContext()->availableContentRect().width();
  const int widthBefore = widget->width();

  editor.resize(900, 400);
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  settle(editor);
  QTest::qWait(50);
  QCoreApplication::processEvents();

  const qreal available = widget->previewContext()->availableContentRect().width();
  QVERIFY2(available > availableBefore,
           qPrintable(QStringLiteral("the text column did not grow: %1 -> %2")
                          .arg(availableBefore)
                          .arg(available)));

  // Both the re-measurement against the new width basis and the redistribution
  // have to re-run.
  const int expected = expectedSheetWidth(available, widget->sizeHint().width());
  QVERIFY2(qAbs(widget->width() - expected) <= c_widthTolerance,
           qPrintable(QStringLiteral("sheet width %1 is not the expected %2 after the resize "
                                     "(was %3)")
                          .arg(widget->width())
                          .arg(expected)
                          .arg(widthBefore)));
  QVERIFY2(qAbs(totalColumnWidth(view) - view->viewport()->width()) <= c_widthTolerance,
           qPrintable(QStringLiteral("columns total %1 do not cover the viewport %2 after the "
                                     "resize")
                          .arg(totalColumnWidth(view))
                          .arg(view->viewport()->width())));
}

// ---------------------------------------------------------------------------
// Review fixes
// ---------------------------------------------------------------------------

void TestInteractivePreview::testRejectionAfterAcceptKeepsCommittedValues() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  setTextAndSettle(editor, QLatin1String(c_table));

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto context = widget->previewContext();
  QVERIFY(context);
  auto model = widget->findChild<QAbstractItemModel *>();
  QVERIFY(model);

  // An accepted commit rebases the binding onto the text now in the document.
  QVERIFY(model->setData(model->index(1, 1), QStringLiteral("committed"), Qt::EditRole));
  QVERIFY(editor.document()->toPlainText().contains(QStringLiteral("committed")));
  const QString afterCommit = editor.document()->toPlainText();

  // A non-authoritative rejection makes the sheet restore itself from the
  // binding. The widget's own cached snapshot still describes the pre-commit
  // source, so restoring from it would revert the accepted commit.
  context->requestSourceReplacement(QStringLiteral("   "));
  QCOMPARE(model->data(model->index(1, 1), Qt::DisplayRole).toString(),
           QStringLiteral("committed"));
  QCOMPARE(editor.document()->toPlainText(), afterCommit);

  // And the next commit must not write the reverted matrix back: it would pass
  // the source check (both texts equal the rebased source) and be accepted.
  QVERIFY(model->setData(model->index(1, 0), QStringLiteral("second"), Qt::EditRole));
  const QString after = editor.document()->toPlainText();
  QVERIFY2(after.contains(QStringLiteral("committed")), qPrintable(after));
  QVERIFY2(after.contains(QStringLiteral("second")), qPrintable(after));
}

void TestInteractivePreview::testReconcileIsDeferredDuringWidgetCallback() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QLatin1String(c_table));

  // Drain every queued pass of the setup before taking a baseline.
  QTest::qWait(50);
  QCoreApplication::processEvents();
  QCOMPARE(factory->m_widgets.size(), 1);

  auto widget = factory->m_widgets.first();
  const int createdBefore = factory->m_createCount;

  int observedCreateCount = -1;
  PreviewWidget *observedWidget = nullptr;
  widget->m_duringSpin = [&]() {
    observedCreateCount = factory->m_createCount;
    observedWidget = singlePreviewWidget(editor);
  };
  widget->m_spinOnNextSetPreview = true;

  // From outside any callback, so the registration is not rejected as
  // reentrant. A rebuild is now owed and one delivery is queued.
  auto second = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(second, 1));

  // Registration edits no document, so this re-emits synchronously and the
  // host reaches setPreview() before the queued delivery gets an ordinary
  // event loop turn.
  editor.getHighlighter()->updateHighlight();
  QCOMPARE(widget->m_spinCount, 1);

  // The rebuild must not have run while the callback was on the stack.
  QCOMPARE(observedCreateCount, createdBefore);
  QCOMPARE(observedWidget, static_cast<PreviewWidget *>(widget));

  // Once the callback returns, the owed rebuild is delivered exactly once.
  QTest::qWait(50);
  QCoreApplication::processEvents();
  QCOMPARE(factory->m_createCount, createdBefore + 1);
  QCOMPARE(previewWidgets(editor).size(), 1);

  auto rebuilt = qobject_cast<RecordingPreviewWidget *>(singlePreviewWidget(editor));
  QVERIFY(rebuilt);
  QVERIFY(rebuilt->previewContext());
  QCOMPARE(rebuilt->previewContext()->preview()->type(), PreviewElementType::Table);
}

void TestInteractivePreview::testBlockedReconcileIsNotRearmedWhileBlocked() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QLatin1String(c_table));

  QTest::qWait(50);
  QCoreApplication::processEvents();
  QCOMPARE(factory->m_widgets.size(), 1);

  auto widget = factory->m_widgets.first();
  const int createdBefore = factory->m_createCount;
  widget->m_deliveryCounterSource = previewHost(editor);
  QVERIFY(widget->m_deliveryCounterSource);
  widget->m_spinOnNextSetPreview = true;

  auto second = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(second, 1));

  editor.getHighlighter()->updateHighlight();
  QCOMPARE(widget->m_spinCount, 1);

  // A delivery which finds the host blocked is consumed once and left to an
  // unblock hook. Re-arming from the blocked branch instead would spin: the
  // nested loop keeps delivering each newly armed zero timer while the flag it
  // waits on can only be cleared by the frame that loop is holding open.
  QVERIFY(widget->m_deliveriesBeforeSpin >= 0);
  QVERIFY(widget->m_deliveriesAfterSpin >= widget->m_deliveriesBeforeSpin);
  const int delta = widget->m_deliveriesAfterSpin - widget->m_deliveriesBeforeSpin;
  QVERIFY2(delta <= 1, qPrintable(QStringLiteral("%1 deliveries ran while blocked").arg(delta)));

  // The owed rebuild is not lost either.
  QTest::qWait(50);
  QCoreApplication::processEvents();
  QCOMPARE(factory->m_createCount, createdBefore + 1);
  QVERIFY(reconcileDeliveries(editor) > widget->m_deliveriesAfterSpin);
}

void TestInteractivePreview::testMeasurementIsNotRepeatedWhenNothingChanged() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));

  editor.resize(600, 400);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  setTextAndSettle(editor, QLatin1String(c_table));

  QCOMPARE(factory->m_widgets.size(), 1);
  auto widget = factory->m_widgets.first();

  // The widget is placed after its source block, so the width it is measured
  // at is the available content width, while the width it reports is its own.
  // Caching the derived width as the guard key never compares equal.
  QCOMPARE(widget->m_preview->placement(), PreviewPlacement::BlockAfterSource);

  auto host = previewHost(editor);
  QVERIFY(host);

  // Drain the queued layout requests of the setup before taking a baseline.
  QTest::qWait(100);
  QCoreApplication::processEvents();
  const int baseline = widget->m_sizeHintCount;
  QVERIFY(baseline > 0);

  for (int i = 0; i < 5; ++i) {
    QVERIFY(QMetaObject::invokeMethod(host, "schedulePublish", Qt::DirectConnection));
    QTest::qWait(20);
    QCoreApplication::processEvents();
  }

  QVERIFY2(widget->m_sizeHintCount == baseline,
           qPrintable(QStringLiteral("measured %1 more time(s) although nothing changed")
                          .arg(widget->m_sizeHintCount - baseline)));
}

void TestInteractivePreview::testWideCellSurvivesRoundTrip() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  setTextAndSettle(editor, QLatin1String(c_table));

  auto widget = singlePreviewWidget(editor);
  QVERIFY(widget);
  auto model = widget->findChild<QAbstractItemModel *>();
  QVERIFY(model);

  // Longer than the padding cap, so the serializer stops widening every other
  // row at it. The cell text itself must still survive verbatim.
  const QString wide = QString(200, QLatin1Char('w'));
  QVERIFY(model->setData(model->index(1, 0), wide, Qt::EditRole));
  QVERIFY(editor.document()->toPlainText().contains(wide));

  const auto before = widget->previewContext()->preview().staticCast<const TablePreview>();
  QVERIFY(before);

  settle(editor);

  auto reparsed = singlePreviewWidget(editor);
  QVERIFY(reparsed);
  auto after = reparsed->previewContext()->preview().staticCast<const TablePreview>();
  QVERIFY(after);

  QCOMPARE(after->cells(), before->cells());
  QCOMPARE(after->alignments(), before->alignments());
  QCOMPARE(after->cells().value(1).value(0), wide);
}

void TestInteractivePreview::testRebasedTableMatchesGenerationSnapshot() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QStringLiteral("> | h1 | h2 |\n> | :-- | --: |\n> | a | b |\n"));

  QCOMPARE(factory->m_widgets.size(), 1);
  auto widget = factory->m_widgets.first();

  widget->previewContext()->requestSourceReplacement(
      QStringLiteral("> | h1 | h2 |\n> | :-- | --: |\n> | z | b |"));
  QCOMPARE(widget->m_lastResult.status(), PreviewReplacementResult::Accepted);

  auto rebased = widget->previewContext()->preview().staticCast<const TablePreview>();
  QVERIFY(rebased);

  // The next parse generation describes the very same text. A second copy of
  // the element-to-snapshot conversion is how the two drift apart.
  settle(editor);

  auto generated = widget->previewContext()->preview().staticCast<const TablePreview>();
  QVERIFY(generated);
  QVERIFY(generated != rebased);

  QCOMPARE(generated->cells(), rebased->cells());
  QCOMPARE(generated->rowPrefixes(), rebased->rowPrefixes());
  QCOMPARE(generated->delimiterPrefix(), rebased->delimiterPrefix());
  QCOMPARE(generated->alignments(), rebased->alignments());
  QCOMPARE(generated->columnCount(), rebased->columnCount());
}

void TestInteractivePreview::testStaleGenerationReplayAfterShrink() {
  QString tail;
  for (int i = 0; i < 6; ++i) {
    tail += QStringLiteral("trailing paragraph %1 which stays behind\n").arg(i);
  }

  // (a) A snapshot which never became an item - its factory declined - is
  // still replayed from the parse generation, and nothing carried an anchor
  // for it, so its coordinates go through makeAnchor() unmediated.
  {
    WarningRecorder warnings;

    VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
    auto declining = new RecordingPreviewFactory({PreviewElementType::Image});
    declining->m_decline = true;
    QVERIFY(editor.registerPreviewWidgetFactory(declining, 5));

    setTextAndSettle(editor, QStringLiteral("![a](b.png)\n\n") + tail);
    QVERIFY(declining->m_createCount > 0);
    QVERIFY(previewWidgets(editor).isEmpty());

    // Truncate below the replayed coordinates, without letting a new parse
    // generation land, then force the replay through the queued rebuild.
    QTextCursor cursor(editor.document());
    cursor.select(QTextCursor::Document);
    cursor.insertText(QStringLiteral("ab\n"));

    auto other = new RecordingPreviewFactory({PreviewElementType::Table});
    QVERIFY(editor.registerPreviewWidgetFactory(other, 1));
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();

    QVERIFY(previewWidgets(editor).isEmpty());
    QVERIFY2(!warnings.contains(QStringLiteral("QTextCursor::setPosition")),
             qPrintable(warnings.m_messages.join(QLatin1Char('\n'))));
  }

  // (b) A live item whose source is deleted from the middle. Its carried
  // anchor collapses, which is evidence the tracked source is gone - not the
  // same as having no carried state. The replayed coordinates are still
  // numerically in bounds here, because the trailing text remains.
  {
    WarningRecorder warnings;

    VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
    setTextAndSettle(editor, QLatin1String(c_table) + QStringLiteral("\n") + tail);

    auto widget = singlePreviewWidget(editor);
    QVERIFY(widget);
    const int start = widget->previewContext()->preview()->startPos();
    const int end = widget->previewContext()->preview()->endPos();

    // Delete exactly the table, so its anchor collapses while the stale
    // [start, end) range still resolves against the trailing text.
    QTextCursor cursor(editor.document());
    cursor.setPosition(start);
    cursor.setPosition(end, QTextCursor::KeepAnchor);
    cursor.removeSelectedText();
    QVERIFY(editor.document()->characterCount() - 1 > end);

    // Force the replay without a new parse generation.
    auto other = new RecordingPreviewFactory({PreviewElementType::Image});
    QVERIFY(editor.registerPreviewWidgetFactory(other, 1));
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();

    // No item may be recreated: neither over a collapsed range, nor over the
    // unrelated text which now occupies the replayed coordinates.
    const auto live = previewWidgets(editor);
    QVERIFY2(live.isEmpty(),
             qPrintable(QStringLiteral("%1 preview(s) survived the deletion of their source")
                            .arg(live.size())));
    QVERIFY2(!warnings.contains(QStringLiteral("QTextCursor::setPosition")),
             qPrintable(warnings.m_messages.join(QLatin1Char('\n'))));
  }
}

// ---------------------------------------------------------------------------
// Local review fixes
// ---------------------------------------------------------------------------

void TestInteractivePreview::testUnregisteringTheBuiltinFactoryIsSafe() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());
  setTextAndSettle(editor, QLatin1String(c_table));
  QVERIFY(singlePreviewWidget(editor));

  auto host = previewHost(editor);
  QVERIFY(host);

  // The built-in table renderer is an ordinary registered factory, so the
  // public API can unregister it like any other. The host keeps a direct
  // reference to it for the per-publish read-only push, which must not outlive
  // the object.
  const auto factories = host->findChildren<PreviewWidgetFactory *>(QString(),
                                                                    Qt::FindDirectChildrenOnly);
  QCOMPARE(factories.size(), 1);
  QPointer<PreviewWidgetFactory> guard(factories.first());

  QVERIFY(editor.unregisterPreviewWidgetFactory(factories.first()));
  QTest::qWait(50);
  QCoreApplication::processEvents();
  QVERIFY(guard.isNull());
  QVERIFY(previewWidgets(editor).isEmpty());

  // Every path which used to touch the built-in factory directly.
  editor.setTablePreviewVisibleRows(4);
  QCOMPARE(editor.tablePreviewVisibleRows(), 4);
  editor.getTextEdit()->setReadOnly(true);
  editor.getTextEdit()->setReadOnly(false);
  QVERIFY(QMetaObject::invokeMethod(host, "schedulePublish", Qt::DirectConnection));
  settle(editor);

  // Nothing renders the table anymore, and the editor is still usable.
  QVERIFY(previewWidgets(editor).isEmpty());
  QVERIFY(editor.document()->toPlainText().contains(QStringLiteral("| h1 | h2 |")));
}

void TestInteractivePreview::testReentrantRegistrationFromSupportedTypesRejected() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Image});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QLatin1String(c_table));

  // supportedTypes() is reached from the registry scan, not only from widget
  // construction. That scan must mark the span as a callback too, otherwise a
  // factory can mutate the registry from inside the very loop walking it.
  factory->m_registerReentrantlyInSupportedTypes = &editor;

  // Re-enabling recomputes the claimable type mask, which scans the registry.
  editor.setInplacePreviewEnabled(false);
  QCoreApplication::processEvents();
  editor.setInplacePreviewEnabled(true);
  QTest::qWait(50);
  QCoreApplication::processEvents();

  QVERIFY(!factory->m_registerReentrantlyInSupportedTypes);
  QVERIFY2(!factory->m_reentrantRegistrationAccepted,
           "a factory registered itself from inside the registry scan");

  // And the host is still coherent.
  QVERIFY(singlePreviewWidget(editor));
}

void TestInteractivePreview::testGenerationDeliveredDuringCallbackIsNotLost() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));
  setTextAndSettle(editor, QLatin1String(c_table));

  QTest::qWait(50);
  QCoreApplication::processEvents();
  QCOMPARE(factory->m_widgets.size(), 1);

  auto widget = factory->m_widgets.first();
  auto host = previewHost(editor);
  QVERIFY(host);

  const quint64 nextRevision = widget->previewContext()->preview()->revision() + 1;
  bool delivered = false;

  // A widget callback can spin a nested event loop, which delivers whatever
  // the highlighter has queued. Standing in for that here: a newer generation
  // arrives while the reconciliation pass that called us is still running.
  widget->m_duringSpin = [&]() {
    delivered = QMetaObject::invokeMethod(
        host, "updatePreviews", Qt::DirectConnection, Q_ARG(quint64, nextRevision),
        Q_ARG(QVector<QSharedPointer<const Preview>>,
              QVector<QSharedPointer<const Preview>>()));
  };
  widget->m_spinOnNextSetPreview = true;

  editor.getHighlighter()->updateHighlight();
  QCOMPARE(widget->m_spinCount, 1);
  QVERIFY(delivered);

  QTest::qWait(50);
  QCoreApplication::processEvents();

  // The newer generation described no elements at all. Dropping it would leave
  // the sheet bound to superseded source with nothing to ever replay it.
  QVERIFY2(previewWidgets(editor).isEmpty(),
           "the generation delivered during the callback was discarded");
}

void TestInteractivePreview::testReplacementCannotSplitATableCell() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Image});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));

  // An inline element inside a table row. A single line is not a table to
  // cmark - that needs the delimiter row - so the validation probe cannot see
  // that '|' is structural here.
  setTextAndSettle(editor,
                   QStringLiteral("| a | ![i](x.png) |\n| --- | --- |\n| b | c |\n"));

  QVERIFY2(factory->m_widgets.size() == 1,
           qPrintable(QStringLiteral("created=%1").arg(factory->m_widgets.size())));
  auto widget = factory->m_widgets.first();
  const QString before = editor.document()->toPlainText();

  // Would split the cell and silently give the table a third column.
  widget->previewContext()->requestSourceReplacement(QStringLiteral("![i|j](y.png)"));
  QVERIFY2(!widget->m_lastResult.isAccepted(),
           qPrintable(editor.document()->toPlainText()));
  QCOMPARE(editor.document()->toPlainText(), before);

  // An escaped pipe is not structural, and a plain replacement is fine.
  widget->previewContext()->requestSourceReplacement(QStringLiteral("![i](z.png)"));
  QCOMPARE(widget->m_lastResult.status(), PreviewReplacementResult::Accepted);
  QVERIFY(editor.document()->toPlainText().contains(QStringLiteral("| a | ![i](z.png) |")));
}

void TestInteractivePreview::testSourceTextRectFollowsTheSource() {
  VMarkdownEditor editor(makeConfig(), QSharedPointer<TextEditorParameters>::create());

  auto factory = new RecordingPreviewFactory({PreviewElementType::Table});
  QVERIFY(editor.registerPreviewWidgetFactory(factory, 5));

  editor.resize(600, 500);
  editor.show();
  QVERIFY(QTest::qWaitForWindowExposed(&editor));
  setTextAndSettle(editor, QLatin1String(c_table) + QStringLiteral("\ntail\n"));

  QCOMPARE(factory->m_widgets.size(), 1);
  auto context = factory->m_widgets.first()->previewContext();
  QVERIFY(context);

  QTest::qWait(50);
  QCoreApplication::processEvents();

  const QRectF before = context->sourceTextRect();
  QVERIFY(!before.isNull());

  // The rect is cached, so an edit which pushes the source down has to
  // invalidate it. A stale rect would keep reporting the old position.
  QTextCursor cursor(editor.document());
  cursor.setPosition(0);
  cursor.insertText(QStringLiteral("head\n\nlines\n\n"));
  settle(editor);

  QTest::qWait(50);
  QCoreApplication::processEvents();

  const QRectF after = context->sourceTextRect();
  QVERIFY(!after.isNull());
  QVERIFY2(after.top() > before.top(),
           qPrintable(QStringLiteral("source rect stayed at %1 after the source moved down "
                                     "(was %2)")
                          .arg(after.top())
                          .arg(before.top())));
}

QTEST_MAIN(tests::TestInteractivePreview)