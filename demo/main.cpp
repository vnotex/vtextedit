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
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
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
  auto *central = new QWidget(p_window);
  auto *layout = new QVBoxLayout(central);
  layout->setContentsMargins(0, 0, 0, 0);
  auto *editor = new VTypstEditor(config, QSharedPointer<TextEditorParameters>::create(), central);
  const auto button = [](const QString &p_text, QWidget *p_parent) {
    auto *result = new QToolButton(p_parent);
    result->setText(p_text);
    result->setToolTip(p_text);
    return result;
  };
  // Retain the translation context used by these controls before they moved to the demo.
  auto *modeBar = new QHBoxLayout;
  auto *modeCombo = new QComboBox(central);
  modeCombo->setObjectName(QStringLiteral("typstMode"));
  modeCombo->addItem(vte::VTypstEditor::tr("Edit"), static_cast<int>(VTypstEditor::Mode::Edit));
  modeCombo->addItem(vte::VTypstEditor::tr("Read"), static_cast<int>(VTypstEditor::Mode::Read));
  modeBar->addWidget(modeCombo);
  auto *inplaceButton = button(vte::VTypstEditor::tr("In-place preview"), central);
  inplaceButton->setCheckable(true);
  auto *liveButton = button(vte::VTypstEditor::tr("Live preview"), central);
  liveButton->setCheckable(true);
  modeBar->addWidget(inplaceButton);
  modeBar->addWidget(liveButton);
  auto *statusLabel = new QLabel(central);
  statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
  statusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  modeBar->addWidget(statusLabel, 1);
  auto *retryButton = button(vte::VTypstEditor::tr("Retry"), central);
  modeBar->addWidget(retryButton);
  layout->addLayout(modeBar);

  auto *pdfBar = new QWidget(central);
  auto *pdfLayout = new QHBoxLayout(pdfBar);
  pdfLayout->setContentsMargins(0, 0, 0, 0);
  auto *search = new QLineEdit(pdfBar);
  search->setPlaceholderText(vte::VTypstEditor::tr("Find in PDF"));
  search->setClearButtonEnabled(true);
  pdfLayout->addWidget(search, 1);
  auto *previous = button(vte::VTypstEditor::tr("Previous match"), pdfBar);
  auto *next = button(vte::VTypstEditor::tr("Next match"), pdfBar);
  auto *zoomOut = button(vte::VTypstEditor::tr("Zoom out"), pdfBar);
  auto *zoomIn = button(vte::VTypstEditor::tr("Zoom in"), pdfBar);
  auto *width = button(vte::VTypstEditor::tr("Fit width"), pdfBar);
  auto *page = button(vte::VTypstEditor::tr("Fit page"), pdfBar);
  auto *cursor = button(vte::VTypstEditor::tr("Show cursor in output"), pdfBar);
  auto *updateButton = button(vte::VTypstEditor::tr("Update available"), pdfBar);
  for (auto *item : {previous, next, zoomOut, zoomIn, width, page, cursor, updateButton}) {
    pdfLayout->addWidget(item);
  }
  auto *pageNumber = new QSpinBox(pdfBar);
  pageNumber->setPrefix(vte::VTypstEditor::tr("Page "));
  pageNumber->setMinimum(1);
  pageNumber->setMaximum(1);
  pdfLayout->addWidget(pageNumber);
  layout->addWidget(pdfBar);

  layout->addWidget(editor, 1);

  const auto updateMode = [editor, modeCombo, inplaceButton, liveButton, pdfBar]() {
    const QSignalBlocker modeBlocker(modeCombo);
    const QSignalBlocker inplaceBlocker(inplaceButton);
    const QSignalBlocker liveBlocker(liveButton);
    modeCombo->setCurrentIndex(modeCombo->findData(static_cast<int>(editor->mode())));
    inplaceButton->setChecked(editor->isInplacePreviewEnabled());
    liveButton->setChecked(editor->isLivePreviewEnabled());
    const bool editing = editor->mode() == VTypstEditor::Mode::Edit;
    inplaceButton->setVisible(editing);
    liveButton->setVisible(editing);
    pdfBar->setVisible(!editing || editor->isLivePreviewEnabled());
  };
  QObject::connect(modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), editor,
                   [editor, modeCombo](int p_index) {
                     editor->setMode(
                         static_cast<VTypstEditor::Mode>(modeCombo->itemData(p_index).toInt()));
                   });
  QObject::connect(inplaceButton, &QToolButton::toggled, editor,
                   &VTypstEditor::setInplacePreviewEnabled);
  QObject::connect(liveButton, &QToolButton::toggled, editor, &VTypstEditor::setLivePreviewEnabled);
  QObject::connect(editor, &VTypstEditor::modeChanged, central, updateMode);
  QObject::connect(editor, &VTypstEditor::inplacePreviewEnabledChanged, central, updateMode);
  QObject::connect(editor, &VTypstEditor::livePreviewEnabledChanged, central, updateMode);
  updateMode();
  const auto updateStatus = [statusLabel](VTypstEditor::CompileStatus, const QString &p_message) {
    statusLabel->setText(p_message);
    statusLabel->setToolTip(p_message);
  };
  QObject::connect(editor, &VTypstEditor::compileStatusChanged, statusLabel, updateStatus);
  updateStatus(editor->compileStatus(), editor->compileMessage());
  QObject::connect(retryButton, &QToolButton::clicked, editor, &VTypstEditor::retryCompilation);
  QObject::connect(search, &QLineEdit::textChanged, editor, &VTypstEditor::setPdfSearchText);
  QObject::connect(search, &QLineEdit::returnPressed, editor,
                   [editor]() { editor->findNextPdfMatch(); });
  QObject::connect(previous, &QToolButton::clicked, editor,
                   [editor]() { editor->findNextPdfMatch(true); });
  QObject::connect(next, &QToolButton::clicked, editor, [editor]() { editor->findNextPdfMatch(); });
  QObject::connect(zoomOut, &QToolButton::clicked, editor,
                   [editor]() { editor->setPdfZoom(editor->pdfZoom() / 1.2); });
  QObject::connect(zoomIn, &QToolButton::clicked, editor,
                   [editor]() { editor->setPdfZoom(editor->pdfZoom() * 1.2); });
  QObject::connect(width, &QToolButton::clicked, editor, &VTypstEditor::fitPdfWidth);
  QObject::connect(page, &QToolButton::clicked, editor, &VTypstEditor::fitPdfPage);
  QObject::connect(cursor, &QToolButton::clicked, editor, &VTypstEditor::showOutputAtCursor);
  QObject::connect(updateButton, &QToolButton::clicked, editor,
                   &VTypstEditor::publishPendingPdfUpdate);
  QObject::connect(editor, &VTypstEditor::pdfUpdateAvailableChanged, updateButton,
                   &QWidget::setVisible);
  updateButton->setVisible(editor->hasPendingPdfUpdate());
  QObject::connect(pageNumber, QOverload<int>::of(&QSpinBox::valueChanged), editor,
                   [editor](int p_page) { editor->goToPdfPage(p_page - 1); });
  const auto updatePageNumber = [editor, pageNumber]() {
    const QSignalBlocker blocker(pageNumber);
    pageNumber->setMaximum(qMax(1, editor->pdfPageCount()));
    pageNumber->setValue(qMax(1, editor->currentPdfPage() + 1));
  };
  QObject::connect(editor, &VTypstEditor::pdfNavigationChanged, pageNumber, updatePageNumber);
  updatePageNumber();
  if (fileIndex < 0) {
    editor->setFileOverlay(QStringLiteral("lib.typ"), QStringLiteral("#let mass = 7\n"));
  }
  editor->getTextEditor()->setText(contents);
  editor->setInplacePreviewEnabled(false);
  editor->setLivePreviewEnabled(true);
  editor->setMode(VTypstEditor::Mode::Edit);
  p_window->setCentralWidget(central);
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
      if (!p_editor->hasPendingPdfUpdate()) {
        return;
      }
      if (!pane->property("selectedText").toString().contains(QStringLiteral("BEFORE smoke"))) {
        abort(QStringLiteral("Compilation cleared the displayed PDF selection"));
        return;
      }
      p_editor->setMode(VTypstEditor::Mode::Read);
      state->settling.start();
      state->phase = 4;
    } else if (state->phase >= 4 && state->settling.elapsed() >= 250) {
      if (!p_editor->hasPendingPdfUpdate() ||
          !pane->property("selectedText").toString().contains(QStringLiteral("BEFORE smoke"))) {
        abort(QStringLiteral("Preview transition published or discarded the selected pending PDF"));
        return;
      }
      if (state->phase == 4) {
        p_editor->setMode(VTypstEditor::Mode::Edit);
        p_editor->setLivePreviewEnabled(false);
        state->phase = 5;
      } else if (state->phase == 5) {
        p_editor->setInplacePreviewEnabled(true);
        p_editor->setLivePreviewEnabled(true);
        state->phase = 6;
      } else {
        p_editor->publishPendingPdfUpdate();
        state->phase = 2;
      }
      state->settling.restart();
    } else if (state->phase == 2) {
      if (p_editor->hasPendingPdfUpdate()) {
        return;
      }
      key(Qt::Key_A);
      key(Qt::Key_C);
      if (!QApplication::clipboard()->text().contains(QStringLiteral("AFTER smoke"))) {
        return;
      }
      if (!state->imagePrefix.isEmpty()) {
        p_editor->window()->grab().save(state->imagePrefix + QStringLiteral("-side.png"));
      }
      p_editor->setMode(VTypstEditor::Mode::Read);
      if (!pane->property("selectedText").toString().contains(QStringLiteral("AFTER smoke"))) {
        abort(QStringLiteral("Read mode discarded PDF selection"));
        return;
      }
      p_editor->setLivePreviewEnabled(false);
      p_editor->setInplacePreviewEnabled(true);
      p_editor->setMode(VTypstEditor::Mode::Edit);
      state->settling.start();
      state->phase = 3;
    } else if (state->phase == 3 && state->settling.elapsed() >= 1500) {
      if (!pane->property("selectedText").toString().contains(QStringLiteral("AFTER smoke"))) {
        abort(QStringLiteral("Hiding live preview discarded PDF selection"));
        return;
      }
      const auto cursor = p_editor->getTextEditor()->getTextEdit()->textCursor();
      if (p_editor->document()->toPlainText() != state->after ||
          p_editor->document()->isModified() != state->modified ||
          cursor.anchor() != state->anchor || cursor.position() != state->position) {
        abort(QStringLiteral("Preview or mode transition mutated source state"));
        return;
      }
      if (!state->imagePrefix.isEmpty()) {
        p_editor->window()->grab().save(state->imagePrefix + QStringLiteral("-inplace.png"));
      }
      p_editor->document()->undo();
      if (!p_editor->document()->toPlainText().contains(QStringLiteral("BEFORE smoke"))) {
        abort(QStringLiteral("Mode transitions damaged source undo"));
        return;
      }
      timer->stop();
      std::fprintf(stdout, "TYPST_WIDGET_SMOKE_OK selection copy deferred-update read live-toggle "
                           "in-place combined undo\n");
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
