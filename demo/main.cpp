#include <QApplication>
#include <QMainWindow>
#include <QSharedPointer>
#include <QStatusBar>
#include <QSslSocket>
#include <QLoggingCategory>

#include "helper.h"
#include "logger.h"

#include <vtextedit/vtexteditor.h>
#include <vtextedit/vmarkdowneditor.h>
#include <vtextedit/markdowneditorconfig.h>

#include <vtextedit/spellchecker.h>

using namespace vte;

static void setupSpellChecker()
{
    SpellChecker::addDictionaryCustomSearchPaths(QStringList(QStringLiteral("D:/tmp/dicts")));
}

/*
static VTextEditor *setupTextEditor(QWidget *p_parent)
{
    auto editorConfig = QSharedPointer<TextEditorConfig>::create();
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

static VMarkdownEditor *setupMarkdownEditor(QWidget *p_parent)
{
    auto editorConfig = QSharedPointer<TextEditorConfig>::create();
    editorConfig->m_inputMode = InputMode::VscodeMode;
    auto markdownEditorConfig = QSharedPointer<MarkdownEditorConfig>::create(editorConfig);
    auto editorParas = QSharedPointer<TextEditorParameters>::create();
    editorParas->m_spellCheckEnabled = false;

    auto editor = new VMarkdownEditor(markdownEditorConfig, editorParas, p_parent);
    editor->enableInternalContextMenu();
    editor->setBasePath(":/demo/data/example_files");
    editor->setText(Helper::getMarkdownText());
    return editor;
}

int main(int p_argc, char *p_argv[])
{
    QApplication app(p_argc, p_argv);

    QLoggingCategory::setFilterRules("kf.sonnet.clients.hunspell.debug=true");
    // QLoggingCategory::setFilterRules("kf.sonnet.core.debug=true");

    qInfo() << "OpenSSL build version:" << QSslSocket::sslLibraryBuildVersionString()
            << "link version:" << QSslSocket::sslLibraryVersionNumber();

    qInstallMessageHandler(&Logger::log);

    QMainWindow win;

    setupSpellChecker();

    VTextEditor::addSyntaxCustomSearchPaths(QStringList(QStringLiteral(":/demo/data")));

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
