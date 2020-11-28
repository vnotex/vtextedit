#include <QApplication>
#include <QMainWindow>
#include <QSharedPointer>
#include <QStatusBar>
#include <QSslSocket>

#include "helper.h"
#include "logger.h"

#include <vtextedit/vtexteditor.h>
#include <vtextedit/vmarkdowneditor.h>
#include <vtextedit/markdowneditorconfig.h>

using namespace vte;

int main(int p_argc, char *p_argv[])
{
    QApplication app(p_argc, p_argv);

    qInfo() << "OpenSSL build version:" << QSslSocket::sslLibraryBuildVersionString()
            << "link version:" << QSslSocket::sslLibraryVersionNumber();

    qInstallMessageHandler(&Logger::log);

    QMainWindow win;

    VTextEditor::addSyntaxCustomSearchPaths(QStringList(QStringLiteral(":/demo/data")));

    auto editorConfig = QSharedPointer<TextEditorConfig>::create();
    auto markdownEditorConfig = QSharedPointer<MarkdownEditorConfig>::create(editorConfig);
    editorConfig->m_lineNumberType = VTextEditor::LineNumberType::Relative;
    editorConfig->m_inputMode = InputMode::ViMode;

    auto editor = new VMarkdownEditor(markdownEditorConfig, &win);
    editor->setBasePath(":/demo/data/example_files");
    editor->setText(Helper::getMarkdownText());

    win.setCentralWidget(editor);

    auto statusWidget = editor->statusWidget();
    win.statusBar()->addWidget(statusWidget.data());

    win.resize(800, 600);
    win.show();

    int ret = app.exec();
    win.statusBar()->removeWidget(statusWidget.data());

    return ret;
}
