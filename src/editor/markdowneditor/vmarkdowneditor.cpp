#include <vtextedit/vmarkdowneditor.h>

#include <vtextedit/markdowneditorconfig.h>
#include <vtextedit/theme.h>
#include <vtextedit/textblockdata.h>
#include <vtextedit/vtextedit.h>
#include <vtextedit/pegmarkdownhighlighter.h>
#include <inputmode/inputmodemgr.h>
#include <inputmode/abstractinputmode.h>

#include "editorpegmarkdownhighlighter.h"
#include "ksyntaxcodeblockhighlighter.h"
#include "documentresourcemgr.h"
#include "textdocumentlayout.h"
#include "editorpreviewmgr.h"

#include <QDebug>

using namespace vte;

VMarkdownEditor::VMarkdownEditor(const QSharedPointer<MarkdownEditorConfig> &p_config,
                                 const QSharedPointer<TextEditorParameters> &p_paras,
                                 QWidget *p_parent)
    : VTextEditor(p_config->m_textEditorConfig, p_paras, p_parent),
      m_config(p_config)
{
    setupDocumentLayout();

    setupSyntaxHighlighter();

    setupPreviewMgr();

    m_textEdit->installEventFilter(this);

    updateFromConfig();

    // Trigger update of stuffs after init.
    m_textEdit->setText("");
}

VMarkdownEditor::~VMarkdownEditor()
{

}

void VMarkdownEditor::setSyntax(const QString &p_syntax)
{
    // Just ignore it.
    Q_UNUSED(p_syntax);
}

QString VMarkdownEditor::getSyntax() const
{
    return QStringLiteral("richmarkdown");
}

void VMarkdownEditor::setupSyntaxHighlighter()
{
    m_highlighterInterface.reset(new EditorPegMarkdownHighlighter(this));
    auto codeBlockHighlighter = new KSyntaxCodeBlockHighlighter(m_config->m_textEditorConfig->m_syntaxTheme, this);
    auto highlighterConfig = QSharedPointer<peg::HighlighterConfig>::create();
    highlighterConfig->m_mathExtEnabled = true;
    m_highlighter = new PegMarkdownHighlighter(m_highlighterInterface.data(),
                                               document(),
                                               theme(),
                                               codeBlockHighlighter,
                                               highlighterConfig);
    connect(m_highlighter, &PegMarkdownHighlighter::highlightCompleted,
            this, [this]() {
                m_textEdit->updateCursorWidth();
                m_textEdit->ensureCursorVisible();
                m_textEdit->checkCenterCursor();
            });
}

void VMarkdownEditor::setupDocumentLayout()
{
    m_resourceMgr.reset(new DocumentResourceMgr());

    auto docLayout = new TextDocumentLayout(document(), m_resourceMgr.data());
    docLayout->setPreviewEnabled(true);

    document()->setDocumentLayout(docLayout);

    connect(m_textEdit, &VTextEdit::cursorWidthChanged,
            this, [this]() {
                documentLayout()->setCursorWidth(m_textEdit->cursorWidth());
            });
}

TextDocumentLayout *VMarkdownEditor::documentLayout() const
{
    return static_cast<TextDocumentLayout *>(document()->documentLayout());
}

void VMarkdownEditor::setupPreviewMgr()
{
    m_previewMgrInterface.reset(new EditorPreviewMgr(this));
    m_previewMgr = new PreviewMgr(m_previewMgrInterface.data(), this);
    m_previewMgr->setPreviewEnabled(true);
    connect(m_highlighter, &PegMarkdownHighlighter::imageLinksUpdated,
            m_previewMgr, &PreviewMgr::updateImageLinks);
    connect(m_previewMgr, &PreviewMgr::requestUpdateImageLinks,
            m_highlighter, &PegMarkdownHighlighter::updateHighlight);
}

DocumentResourceMgr *VMarkdownEditor::getDocumentResourceMgr() const
{
    return m_resourceMgr.data();
}

PegMarkdownHighlighter *VMarkdownEditor::getHighlighter() const
{
    return m_highlighter;
}

PreviewMgr *VMarkdownEditor::getPreviewMgr() const
{
    return m_previewMgr;
}

void VMarkdownEditor::setConfig(const QSharedPointer<MarkdownEditorConfig> &p_config)
{
    m_config = p_config;
    m_config->fillDefaultTheme();

    VTextEditor::setConfig(p_config->m_textEditorConfig);

    updateFromConfig();
}

void VMarkdownEditor::updateFromConfig()
{
    Q_ASSERT(m_config);

    documentLayout()->setConstrainPreviewWidthEnabled(m_config->m_constrainInPlacePreviewWidthEnabled);
}

bool VMarkdownEditor::eventFilter(QObject *p_obj, QEvent *p_event)
{
    if (p_obj == m_textEdit) {
        switch (p_event->type()) {
        case QEvent::KeyPress:
            if (handleKeyPressEvent(static_cast<QKeyEvent *>(p_event))) {
                return true;
            }
            break;

        default:
            break;
        }
    }
    return VTextEditor::eventFilter(p_obj, p_event);
}

bool VMarkdownEditor::handleKeyPressEvent(QKeyEvent *p_event)
{
    const int key = p_event->key();
    switch (key) {
    case Qt::Key_Return:
        return handleKeyReturn(p_event);

    default:
        break;
    }
    return false;
}

bool VMarkdownEditor::handleKeyReturn(QKeyEvent *p_event)
{
    if (p_event->modifiers() == Qt::ShiftModifier) {
        // Shift+Return: insert two spaces and then insert a new line.
        insertText(QStringLiteral("  \n"));
        p_event->accept();
        return true;
    }

    return false;
}

void VMarkdownEditor::zoom(int p_delta)
{
    int preFontSize = m_textEdit->font().pointSize();
    VTextEditor::zoom(p_delta);
    int postFontSize = m_textEdit->font().pointSize();

    if (preFontSize == postFontSize) {
        return;
    }

    m_highlighter->updateStylesFontSize(postFontSize - preFontSize);
}
