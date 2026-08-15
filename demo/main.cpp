#include <QApplication>
#include <QComboBox>
#include <QLoggingCategory>
#include <QMainWindow>
#include <QSharedPointer>
#include <QSslSocket>
#include <QStatusBar>
#include <QVBoxLayout>

#include "helper.h"
#include "logger.h"

#include <vtextedit/markdowneditorconfig.h>
#include <vtextedit/viconfig.h>
#include <vtextedit/vmarkdowneditor.h>
#include <vtextedit/vrichtexteditor.h>
#include <vtextedit/vtexteditor.h>

#include <vtextedit/spellchecker.h>

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
  editorConfig->m_inputMode = InputMode::VscodeMode;
  editorConfig->m_lineSpacing = 1.25;
  // editorConfig->m_maxContentWidth = 600;
  auto markdownEditorConfig = QSharedPointer<MarkdownEditorConfig>::create(editorConfig);
  // The editable table sheet is opt-in, because it writes back to the document.
  markdownEditorConfig->m_inplacePreviewSources |= MarkdownEditorConfig::Table;
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
static void setupRichTextEditor(QMainWindow *p_win) {
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
      statusWidget->clear();
    }

    if (p_widget) {
      *statusWidget = p_widget;
      p_win->statusBar()->addWidget(p_widget.data());
      p_widget->show();
    }
  };

  QObject::connect(editor, &VRichTextEditor::inputModeStatusWidgetChanged, p_win,
                   [mountStatusWidget](QSharedPointer<QWidget> p_widget) {
                     mountStatusWidget(p_widget);
                   });
  // Initial sync: the mode was installed during construction.
  mountStatusWidget(editor->inputModeStatusWidget());

  QObject::connect(modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), editor,
                   [editor, modeCombo](int p_index) {
                     editor->setInputMode(
                         static_cast<InputMode>(modeCombo->itemData(p_index).toInt()));
                   });
}

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
    setupRichTextEditor(&win);

    win.resize(800, 600);
    win.show();

    return app.exec();
  }

  auto editor = setupMarkdownEditor(&win);

  win.setCentralWidget(editor);

  auto statusWidget = editor->statusWidget();
  win.statusBar()->addWidget(statusWidget.data());

  win.resize(800, 600);
  win.show();

  int ret = app.exec();
  win.statusBar()->removeWidget(statusWidget.data());

  return ret;
}
