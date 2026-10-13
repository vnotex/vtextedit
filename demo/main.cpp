#include <QApplication>
#include <QComboBox>
#include <QLoggingCategory>
#include <QMainWindow>
#include <QSharedPointer>
#include <QSslSocket>
#include <QStatusBar>
#include <QVBoxLayout>

#include <functional>

#include "helper.h"
#include "logger.h"

#include <vtextedit/markdowneditorconfig.h>
#include <vtextedit/viconfig.h>
#include <vtextedit/vmarkdowneditor.h>
#include <vtextedit/vrichtexteditor.h>
#include <vtextedit/vtexteditor.h>

#include <vtextedit/spellchecker.h>

#ifdef VTEXTEDIT_ENABLE_TYPST
#include <QAbstractScrollArea>
#include <QClipboard>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QKeyEvent>
#include <QTextDocument>
#include <QTimer>
#include <cstdio>
#include <vtextedit/typsteditorconfig.h>
#include <vtextedit/vtextedit.h>
#include <vtextedit/vtypsteditor.h>
#endif

using namespace vte;

static void setupSpellChecker() {
  SpellChecker::addDictionaryCustomSearchPaths(QStringList(QStringLiteral("D:/tmp/dicts")));
}

/*
static VTextEditor *setupTextEditor(QWidget *p_parent)
{
    auto editorConfig = QSharedPointer<TextEditorConfig>::create();
    editorConfig->m_lineSpacing = 1.5;
    editorConfig->m_maxContentWidth = 600;
    auto editorParas = QSharedPointer<TextEditorParameters>::create();
    editorParas->m_spellCheckEnabled = true;

    auto editor = new VTextEditor(editorConfig, editorParas, p_parent);
    editor->enableInternalContextMenu();
    editor->setBasePath(":/demo/data/example_files");
    editor->setText(Helper::getCppText());
    editor->setSyntax("cpp");
    return editor;
}
*/

static VMarkdownEditor *setupMarkdownEditor(QWidget *p_parent) {
  auto editorConfig = QSharedPointer<TextEditorConfig>::create();
  editorConfig->m_inputMode = InputMode::ViMode;
  editorConfig->m_lineSpacing = 1.25;
  // editorConfig->m_maxContentWidth = 600;
  auto markdownEditorConfig = QSharedPointer<MarkdownEditorConfig>::create(editorConfig);
  // The editable table sheet is opt-in, because it writes back to the document.
  markdownEditorConfig->m_inplacePreviewSources |= MarkdownEditorConfig::Table;
  markdownEditorConfig->m_autoFormatTableSourceEnabled = true;
  markdownEditorConfig->m_autoNumberOrderedListsEnabled = true;
  markdownEditorConfig->m_concealElements = MarkdownConcealElement::ImageUrl |
                                            MarkdownConcealElement::LinkUrl |
                                            MarkdownConcealElement::ReferenceUrl;
  markdownEditorConfig->m_concealLengthThreshold = 20;
  auto editorParas = QSharedPointer<TextEditorParameters>::create();
  editorParas->m_spellCheckEnabled = false;

  auto editor = new VMarkdownEditor(markdownEditorConfig, editorParas, p_parent);
  editor->enableInternalContextMenu();
  editor->setBasePath(":/demo/data/example_files");
  editor->setText(Helper::getMarkdownText());
  return editor;
}

static const char *c_richTextHtml = "<h2>Rich text demo</h2>"
                                    "<p>This paragraph has <b>bold</b> and <i>italic</i> text, "
                                    "plus a <a href=\"https://example.com\">hyperlink</a>.</p>"
                                    "<ul><li>first item</li><li>second item</li>"
                                    "<li>third item</li></ul>"
                                    "<table border=\"1\" cellpadding=\"4\">"
                                    "<tr><td>one</td><td>two</td></tr>"
                                    "<tr><td>three</td><td>four</td></tr></table>"
                                    "<p>Plain trailing paragraph.</p>";

// Rich text editor demo with an input mode switcher and a status bar slot for
// the input mode status widget (the Vi command bar).
// Returns a cleanup callable which must run before the window is destroyed:
// QStatusBar::addWidget() reparents the widget, while the input mode owns it
// through a QSharedPointer, so it has to be handed back before the status bar
// dies.
static std::function<void()> setupRichTextEditor(QMainWindow *p_win) {
  auto config = QSharedPointer<RichTextEditorConfig>::create();
  config->m_viConfig = QSharedPointer<ViConfig>::create();

  auto central = new QWidget(p_win);
  auto layout = new QVBoxLayout(central);

  auto modeCombo = new QComboBox(central);
  modeCombo->addItem(QStringLiteral("Normal"), static_cast<int>(InputMode::NormalMode));
  modeCombo->addItem(QStringLiteral("Vi"), static_cast<int>(InputMode::ViMode));
  modeCombo->addItem(QStringLiteral("VSCode"), static_cast<int>(InputMode::VscodeMode));

  auto editor = new VRichTextEditor(config, central);
  editor->setHtml(QString::fromUtf8(c_richTextHtml));

  layout->addWidget(modeCombo);
  layout->addWidget(editor);

  p_win->setCentralWidget(central);

  auto statusWidget = QSharedPointer<QSharedPointer<QWidget>>::create();
  auto mountStatusWidget = [p_win, statusWidget](QSharedPointer<QWidget> p_widget) {
    if (*statusWidget) {
      p_win->statusBar()->removeWidget(statusWidget->data());
      // removeWidget() only hides it; the status bar is still its parent and
      // would delete it, so give the ownership back to the input mode.
      (*statusWidget)->setParent(nullptr);
      statusWidget->clear();
    }

    if (p_widget) {
      *statusWidget = p_widget;
      p_win->statusBar()->addWidget(p_widget.data());
      p_widget->show();
    }
  };

  QObject::connect(
      editor, &VRichTextEditor::inputModeStatusWidgetChanged, p_win,
      [mountStatusWidget](QSharedPointer<QWidget> p_widget) { mountStatusWidget(p_widget); });
  // Initial sync: the mode was installed during construction.
  mountStatusWidget(editor->inputModeStatusWidget());

  QObject::connect(modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), editor,
                   [editor, modeCombo](int p_index) {
                     editor->setInputMode(
                         static_cast<InputMode>(modeCombo->itemData(p_index).toInt()));
                   });

  return [mountStatusWidget]() { mountStatusWidget(QSharedPointer<QWidget>()); };
}

#ifdef VTEXTEDIT_ENABLE_TYPST
static VTypstEditor *setupTypstEditor(QMainWindow *p_window, const QStringList &p_arguments) {
  auto config = QSharedPointer<TypstEditorConfig>::create();
  config->m_helperExecutable = qEnvironmentVariable("VTEXTEDIT_TYPST_HELPER");
  config->m_projectRoot = QDir::currentPath();
  QString contents;
  const int fileIndex = p_arguments.indexOf(QStringLiteral("--typst-file"));
  if (fileIndex >= 0) {
    if (fileIndex + 1 >= p_arguments.size()) {
      qCritical("--typst-file requires a file path");
      return nullptr;
    }
    const QFileInfo fileInfo(p_arguments[fileIndex + 1]);
    QFile file(fileInfo.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
      qCritical("Could not open Typst source");
      return nullptr;
    }
    config->m_projectRoot = fileInfo.absolutePath();
    config->m_mainFile = fileInfo.fileName();
    contents = QString::fromUtf8(file.readAll());
  } else {
    QFile file(QStringLiteral(":/demo/data/example_files/example.typ"));
    if (!file.open(QIODevice::ReadOnly)) {
      qCritical("Could not open the bundled Typst example");
      return nullptr;
    }
    contents = QString::fromUtf8(file.readAll());
  }
  auto editor = new VTypstEditor(config, QSharedPointer<TextEditorParameters>::create(), p_window);
  if (fileIndex < 0) {
    editor->setFileOverlay(QStringLiteral("lib.typ"), QStringLiteral("#let mass = 7\n"));
  }
  editor->getTextEditor()->setText(contents);
  editor->setMode(VTypstEditor::Mode::SideBySidePreview);
  p_window->setCentralWidget(editor);
  return editor;
}

// Installed-layout smoke drives the actual widget, process and native PDF surface.
// It is opt-in; ordinary demo use never writes files or edits its initial source.
static void runTypstSmoke(VTypstEditor *p_editor, const QStringList &p_arguments) {
  struct SmokeState {
    int phase = 0;
    QElapsedTimer deadline;
    QElapsedTimer settling;
    QString after;
    QString imagePrefix;
    int anchor = 0;
    int position = 0;
    bool modified = false;
  };
  auto state = QSharedPointer<SmokeState>::create();
  state->deadline.start();
  const int imageIndex = p_arguments.indexOf(QStringLiteral("--typst-smoke-image"));
  if (imageIndex >= 0 && imageIndex + 1 < p_arguments.size()) {
    state->imagePrefix = p_arguments[imageIndex + 1];
  }
  auto *timer = new QTimer(p_editor);
  timer->setInterval(50);
  QObject::connect(timer, &QTimer::timeout, p_editor, [p_editor, state, timer]() {
    const auto abort = [timer](const QString &p_reason) {
      std::fprintf(stderr, "TYPST_WIDGET_SMOKE_FAILED: %s\n", qPrintable(p_reason));
      timer->stop();
      QCoreApplication::exit(2);
    };
    if (state->deadline.elapsed() > 60000) {
      abort(p_editor->compileMessage());
      return;
    }
    auto *pane = p_editor->findChild<QAbstractScrollArea *>(QStringLiteral("typstPdf"));
    if (!pane) {
      abort(QStringLiteral("Missing native PDF pane"));
      return;
    }
    const auto key = [pane](int p_key) {
      QKeyEvent press(QEvent::KeyPress, p_key, Qt::ControlModifier);
      QKeyEvent release(QEvent::KeyRelease, p_key, Qt::ControlModifier);
      QCoreApplication::sendEvent(pane, &press);
      QCoreApplication::sendEvent(pane, &release);
    };
    if (p_editor->compileStatus() == VTypstEditor::CompileStatus::Unavailable ||
        p_editor->compileStatus() == VTypstEditor::CompileStatus::Error) {
      abort(p_editor->compileMessage());
      return;
    }
    if (state->phase == 0) {
      if (p_editor->compileStatus() != VTypstEditor::CompileStatus::Ready) {
        return;
      }
      pane->setFocus();
      key(Qt::Key_A);
      if (!pane->property("selectedText").toString().contains(QStringLiteral("BEFORE smoke"))) {
        return;
      }
      key(Qt::Key_C);
      if (!QApplication::clipboard()->text().contains(QStringLiteral("BEFORE smoke"))) {
        abort(QStringLiteral("Native PDF copy did not reach the clipboard"));
        return;
      }
      auto cursor = p_editor->document()->find(QStringLiteral("BEFORE"));
      if (cursor.isNull()) {
        abort(QStringLiteral("Smoke source marker missing"));
        return;
      }
      cursor.beginEditBlock();
      cursor.insertText(QStringLiteral("AFTER"));
      cursor.endEditBlock();
      state->after = p_editor->document()->toPlainText();
      const auto sourceCursor = p_editor->getTextEditor()->getTextEdit()->textCursor();
      state->anchor = sourceCursor.anchor();
      state->position = sourceCursor.position();
      state->modified = p_editor->document()->isModified();
      state->phase = 1;
    } else if (state->phase == 1) {
      if (!pane->property("hasPendingUpdate").toBool()) {
        return;
      }
      if (!pane->property("selectedText").toString().contains(QStringLiteral("BEFORE smoke"))) {
        abort(QStringLiteral("Compilation cleared the displayed PDF selection"));
        return;
      }
      if (!QMetaObject::invokeMethod(pane, "publishPending", Qt::DirectConnection)) {
        abort(QStringLiteral("Explicit PDF update action is unavailable"));
        return;
      }
      state->phase = 2;
    } else if (state->phase == 2) {
      if (pane->property("hasPendingUpdate").toBool()) {
        return;
      }
      key(Qt::Key_A);
      key(Qt::Key_C);
      if (!QApplication::clipboard()->text().contains(QStringLiteral("AFTER smoke"))) {
        return;
      }
      if (!state->imagePrefix.isEmpty()) {
        p_editor->grab().save(state->imagePrefix + QStringLiteral("-side.png"));
      }
      p_editor->setMode(VTypstEditor::Mode::Read);
      if (!pane->property("selectedText").toString().contains(QStringLiteral("AFTER smoke"))) {
        abort(QStringLiteral("Read mode discarded PDF selection"));
        return;
      }
      p_editor->setMode(VTypstEditor::Mode::InPlacePreview);
      state->settling.start();
      state->phase = 3;
    } else if (state->phase == 3 && state->settling.elapsed() >= 1500) {
      const auto cursor = p_editor->getTextEditor()->getTextEdit()->textCursor();
      if (p_editor->document()->toPlainText() != state->after ||
          p_editor->document()->isModified() != state->modified ||
          cursor.anchor() != state->anchor || cursor.position() != state->position) {
        abort(QStringLiteral("Preview or mode transition mutated source state"));
        return;
      }
      if (!state->imagePrefix.isEmpty()) {
        p_editor->grab().save(state->imagePrefix + QStringLiteral("-inplace.png"));
      }
      p_editor->document()->undo();
      if (!p_editor->document()->toPlainText().contains(QStringLiteral("BEFORE smoke"))) {
        abort(QStringLiteral("Mode transitions damaged source undo"));
        return;
      }
      timer->stop();
      std::fprintf(
          stdout,
          "TYPST_WIDGET_SMOKE_OK selection copy deferred-update read side-by-side in-place undo\n");
      std::fflush(stdout);
      QCoreApplication::exit(0);
    }
  });
  timer->start();
}
#endif

int main(int p_argc, char *p_argv[]) {
  QApplication app(p_argc, p_argv);

  QLoggingCategory::setFilterRules("kf.sonnet.clients.hunspell.debug=true");
  // QLoggingCategory::setFilterRules("kf.sonnet.core.debug=true");

  qInfo() << "OpenSSL build version:" << QSslSocket::sslLibraryBuildVersionString()
          << "link version:" << QSslSocket::sslLibraryVersionNumber();

  qInstallMessageHandler(&Logger::log);

  QMainWindow win;

  setupSpellChecker();

  VTextEditor::addSyntaxCustomSearchPaths(QStringList(QStringLiteral(":/demo/data")));

  if (app.arguments().contains(QStringLiteral("--richtext"))) {
    auto cleanup = setupRichTextEditor(&win);

    win.resize(800, 600);
    win.show();

    const int ret = app.exec();
    // Hand the Vi command bar back to the input mode before the window (and
    // its status bar) is destroyed.
    cleanup();

    return ret;
  }

#ifdef VTEXTEDIT_ENABLE_TYPST
#ifdef VTEXTEDIT_TYPST_DEMO
  const bool useTypst = true;
#else
  const bool useTypst = app.arguments().contains(QStringLiteral("--typst"));
#endif
  if (useTypst) {
    auto *typstEditor = setupTypstEditor(&win, app.arguments());
    if (!typstEditor) {
      return 2;
    }
    win.resize(1200, 800);
    win.show();
    if (app.arguments().contains(QStringLiteral("--typst-smoke"))) {
      runTypstSmoke(typstEditor, app.arguments());
    }
    return app.exec();
  }
#endif

  auto editor = setupMarkdownEditor(&win);

  win.setCentralWidget(editor);

  auto statusWidget = editor->statusWidget();
  win.statusBar()->addWidget(statusWidget.data());

  win.resize(800, 600);
  win.show();

  int ret = app.exec();
  win.statusBar()->removeWidget(statusWidget.data());
  // removeWidget() keeps the status bar as the parent, which would delete the
  // widget the editor still owns through a QSharedPointer.
  statusWidget->setParent(nullptr);

  return ret;
}
