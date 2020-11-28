// Start of Document
#include <vtextedit/vtexteditor.h>

#include <vtextedit/texteditorconfig.h>
#include <vtextedit/vtextedit.h>
#include <vtextedit/theme.h>
#include <vtextedit/textblockdata.h>
#include <inputmode/inputmodemgr.h>
#include <inputmode/abstractinputmode.h>

#include "indicatorsborder.h"
#include "editorindicatorsborder.h"
#include "editorextraselection.h"
#include "textfolding.h"
#include "syntaxhighlighter.h"
#include "ksyntaxhighlighterwrapper.h"
#include "editorinputmode.h"
#include "editorcompleter.h"
#include "statusindicator.h"

/*
#include <QHBoxLayout>
#include <QScrollBar>
#include <QHash>
#include <QPair>
#include <QDebug>
*/

using namespace vte;

int VTextEditor::s_instanceCount = 0;

Completer *VTextEditor::s_completer = nullptr;

VTextEditor::VTextEditor(const QSharedPointer<TextEditorConfig> &p_config,
                         QWidget *p_parent)
    : QWidget(p_parent),
      m_config(p_config)
{
    if (!m_config) {
        m_config = QSharedPointer<TextEditorConfig>::create();
    }

    setupUI();

    setupTextFolding();

    setupExtraSelection();

    setupSyntaxHighlighter();

    setupCompleter();

    // Status widget.
    connect(m_textEdit, &QTextEdit::cursorPositionChanged,
            this, &VTextEditor::updateCursorOfStatusWidget);
    connect(this, &VTextEditor::syntaxChanged,
            this, &VTextEditor::updateSyntaxOfStatusWidget);
    connect(this, &VTextEditor::modeChanged,
            this, &VTextEditor::updateModeOfStatusWidget);

    m_inputModeInterface = QSharedPointer<EditorInputMode>::create(this);

    updateFromConfig();

    ++s_instanceCount;
}

VTextEditor::~VTextEditor()
{
    --s_instanceCount;

    if (s_instanceCount == 0) {
        delete s_completer;
        s_completer = nullptr;
    }
}

void VTextEditor::setupUI()
{
    auto mainLayout = new QHBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    setupTextEdit();

    setupIndicatorsBorder();

    mainLayout->addWidget(m_indicatorsBorder);
    mainLayout->addWidget(m_textEdit);
}

static IndicatorsBorder::LineNumberType indicatorsBorderLineNumberType(VTextEditor::LineNumberType p_type)
{
    return static_cast<IndicatorsBorder::LineNumberType>(static_cast<int>(p_type));
}

void VTextEditor::setupIndicatorsBorder()
{
    m_indicatorsBorderInterface.reset(new EditorIndicatorsBorder(this));
    m_indicatorsBorder = new IndicatorsBorder(m_indicatorsBorderInterface.data(),
                                              indicatorsBorderLineNumberType(m_config->m_lineNumberType),
                                              m_config->m_textFoldingEnabled,
                                              this);

    connect(m_textEdit, &VTextEdit::cursorLineChanged,
            m_indicatorsBorder, &IndicatorsBorder::updateBorder);

    auto sb = m_textEdit->verticalScrollBar();
    if (sb) {
        connect(m_textEdit->verticalScrollBar(), &QScrollBar::valueChanged,
                m_indicatorsBorder, &IndicatorsBorder::updateBorder);
    }

    connect(document(), &QTextDocument::contentsChanged,
            m_indicatorsBorder, &IndicatorsBorder::updateBorder);

    connect(m_textEdit, &VTextEdit::resized,
            m_indicatorsBorder, &IndicatorsBorder::updateBorder);
}

void VTextEditor::setupTextEdit()
{
    m_textEdit = new VTextEdit(this);

    m_textEdit->setAcceptRichText(false);
    // We don't need its border.
    m_textEdit->setFrameStyle(QFrame::NoFrame);
}

void VTextEditor::setText(const QString &p_text)
{
    m_textEdit->setText(p_text);
}

QTextDocument *VTextEditor::document() const
{
    return m_textEdit->document();
}

VTextEdit *VTextEditor::getTextEdit() const
{
    return m_textEdit;
}

void VTextEditor::setupTextFolding()
{
    Q_ASSERT(m_textEdit && m_indicatorsBorder);
    m_folding = new TextFolding(m_textEdit->document());
    connect(m_folding, &TextFolding::foldingRangesChanged,
            m_indicatorsBorder, &IndicatorsBorder::updateBorder);
}

void VTextEditor::setupExtraSelection()
{
    m_extraSelectionInterface.reset(new EditorExtraSelection(this));
    m_extraSelectionMgr = new ExtraSelectionMgr(m_extraSelectionInterface.data(), this);
    connect(m_textEdit, &QTextEdit::cursorPositionChanged,
            m_extraSelectionMgr, &ExtraSelectionMgr::handleCursorPositionChange);
    connect(m_textEdit, &VTextEdit::contentsChanged,
            m_extraSelectionMgr, &ExtraSelectionMgr::handleContentsChange);
    connect(m_textEdit, &VTextEdit::selectionChanged,
            m_extraSelectionMgr, &ExtraSelectionMgr::handleSelectionChange);

    Q_ASSERT(m_folding);
    m_folding->setExtraSelectionMgr(m_extraSelectionMgr);
}

void VTextEditor::setupCompleter()
{
    m_completerInterface.reset(new EditorCompleter(this));
}

void VTextEditor::updateFromConfig()
{
    Q_ASSERT(m_config);

    auto &theme = m_config->m_theme;
    if (!theme) {
        theme = defaultTheme();
    }

    // Editor font.
    {
        auto font = m_textEdit->font();
        bool needUpdate = false;
        const auto &fmt = theme->editorStyle(Theme::Editor);
        if (!fmt.m_fontFamily.isEmpty()) {
            font.setFamily(fmt.m_fontFamily);
            needUpdate = true;
        }
        if (fmt.m_fontPointSize > 0) {
            font.setPointSize(fmt.m_fontPointSize);
            needUpdate = true;
        }
        if (needUpdate) {
            m_textEdit->setFont(font);
        }
    }

    updateExtraSelectionMgrFromConfig();

    updateIndicatorsBorderFromConfig();

    setInputMode(m_config->m_inputMode);
}

void VTextEditor::setInputMode(InputMode p_mode)
{
    auto currentMode = m_textEdit->getInputMode();
    if (currentMode && currentMode->mode() == p_mode) {
        return;
    }

    auto modeFactory = InputModeMgr::getInst().getInputModeFactory(p_mode);
    Q_ASSERT(modeFactory);
    auto mode = modeFactory->createInputMode(m_inputModeInterface.data());
    m_textEdit->setInputMode(mode);
    updateInputModeStatusWidget();
}

QSharedPointer<AbstractInputMode> VTextEditor::getInputMode() const
{
    return m_textEdit->getInputMode();
}

void VTextEditor::setupSyntaxHighlighter()
{
    KSyntaxHighlighterWrapper::Initialize(m_config->m_customDefinitionFilesSearchPaths);
}

void VTextEditor::setSyntax(const QString &p_syntax)
{
    if (m_syntax == p_syntax) {
        return;
    }

    m_syntax = p_syntax;
    if (m_highlighter) {
        delete m_highlighter;
        m_highlighter = nullptr;
    }

    if (!m_syntax.isEmpty()) {
        m_highlighter = new SyntaxHighlighter(document(), "", m_syntax);
    }

    emit syntaxChanged();
}

const QString &VTextEditor::getSyntax() const
{
    return m_syntax;
}

QSharedPointer<Theme> VTextEditor::defaultTheme()
{
    // TODO: read it from a JSON file.
    auto th = QSharedPointer<Theme>::create(QStringLiteral("default"));

    {
        auto &fmt = th->editorStyle(Theme::Editor);
        fmt.m_fontFamily = QStringLiteral("Courier New");
        fmt.m_fontPointSize = 11;
    }

    {
        auto &fmt = th->editorStyle(Theme::CursorLine);
        fmt.m_backgroundColor = 0xffc5cae9;
    }

    {
        auto &fmt = th->editorStyle(Theme::TrailingSpace);
        fmt.m_backgroundColor = 0xffa8a8a8;
    }

    {
        auto &fmt = th->editorStyle(Theme::Tab);
        fmt.m_backgroundColor = 0xffcfcfcf;
    }

    {
        auto &fmt = th->editorStyle(Theme::SelectedText);
        fmt.m_textColor = 0xff222222;
        fmt.m_backgroundColor = 0xffdfdf00;
    }

    {
        auto &fmt = th->editorStyle(Theme::IndicatorsBorder);
        fmt.m_textColor = 0xffaaaaaa;
        fmt.m_backgroundColor = 0xffeeeeee;
    }

    {
        auto &fmt = th->editorStyle(Theme::CurrentLineNumber);
        fmt.m_textColor = 0xff222222;
    }

    {
        auto &fmt = th->editorStyle(Theme::Folding);
        fmt.m_textColor = 0xff6495ed;
    }

    {
        auto &fmt = th->editorStyle(Theme::FoldedFolding);
        fmt.m_textColor = 0xff4169e1;
    }

    {
        auto &fmt = th->editorStyle(Theme::FoldingHighlight);
        fmt.m_textColor = 0xffa9c4f5;
    }

    return th;
}

void VTextEditor::updateExtraSelectionMgrFromConfig()
{
    auto &theme = m_config->m_theme;
    {
        const auto &fmt = theme->editorStyle(Theme::CursorLine);
        m_extraSelectionMgr->setExtraSelectionFormat(ExtraSelectionMgr::CursorLine,
                                                     fmt.textColor(),
                                                     fmt.backgroundColor(),
                                                     true);
    }
    {
        const auto &fmt = theme->editorStyle(Theme::TrailingSpace);
        m_extraSelectionMgr->setExtraSelectionFormat(ExtraSelectionMgr::TrailingSpace,
                                                     fmt.textColor(),
                                                     fmt.backgroundColor(),
                                                     false);
    }
    {
        const auto &fmt = theme->editorStyle(Theme::Tab);
        m_extraSelectionMgr->setExtraSelectionFormat(ExtraSelectionMgr::Tab,
                                                     fmt.textColor(),
                                                     fmt.backgroundColor(),
                                                     false);
    }
    {
        const auto &fmt = theme->editorStyle(Theme::SelectedText);
        m_extraSelectionMgr->setExtraSelectionFormat(ExtraSelectionMgr::SelectedText,
                                                     fmt.textColor(),
                                                     fmt.backgroundColor(),
                                                     false);
    }
}

void VTextEditor::updateIndicatorsBorderFromConfig()
{
    auto &theme = m_config->m_theme;
    {
        const auto &fmt = theme->editorStyle(Theme::IndicatorsBorder);
        m_indicatorsBorder->setForegroundColor(fmt.textColor());
        m_indicatorsBorder->setBackgroundColor(fmt.backgroundColor());

        auto font = m_textEdit->font();
        bool needUpdate = false;
        if (!fmt.m_fontFamily.isEmpty()) {
            font.setFamily(fmt.m_fontFamily);
            needUpdate = true;
        }
        if (fmt.m_fontPointSize > 0) {
            font.setPointSize(fmt.m_fontPointSize);
            needUpdate = true;
        }
        if (needUpdate) {
            m_indicatorsBorder->setFont(font);
        }
    }
    {
        const auto &fmt = theme->editorStyle(Theme::CurrentLineNumber);
        m_indicatorsBorder->setCurrentLineNumberForegroundColor(fmt.textColor());
    }
    {
        const auto &fmt = theme->editorStyle(Theme::Folding);
        m_indicatorsBorder->setFoldingColor(fmt.textColor());
    }
    {
        const auto &fmt = theme->editorStyle(Theme::FoldedFolding);
        m_indicatorsBorder->setFoldedFoldingColor(fmt.textColor());
    }
    {
        const auto &fmt = theme->editorStyle(Theme::FoldingHighlight);
        m_indicatorsBorder->setFoldingHighlightColor(fmt.textColor());
    }
}

QSharedPointer<TextBlockRange> VTextEditor::fetchSyntaxFoldingRangeStartingOnBlock(int p_blockNumber)
{
    if (p_blockNumber < 0 || p_blockNumber >= document()->blockCount()) {
        return nullptr;
    }

    if (!m_highlighter) {
        return nullptr;
    }

    auto block = document()->findBlockByNumber(p_blockNumber);
    if (!block.isValid()) {
        return nullptr;
    }

    auto data = TextBlockData::get(block);
    if (!data->isMarkedAsFoldingStart()) {
        return nullptr;
    }

    // Search the type of the first folding region which remains pending.
    int openRegionType = 0;
    int openRegionOffset = -1;
    {
        // <folding id, <offset, count>>.
        QHash<int, QPair<int, int>> openFoldingMap;
        for (const auto &folding : data->getFoldings()) {
            if (folding.isOpen()) {
                // A folding open, like `{`.
                auto it = openFoldingMap.find(folding.m_value);
                if (it != openFoldingMap.end()) {
                    ++(it.value().second);
                } else {
                    openFoldingMap.insert(folding.m_value,
                                          qMakePair(folding.m_offset, 1));
                }
            } else {
                // A folding close, like `}`.
                auto it = openFoldingMap.find(-folding.m_value);
                if (it != openFoldingMap.end()) {
                    if (it.value().second > 1) {
                        --(it.value().second);
                    } else {
                        openFoldingMap.erase(it);
                    }
                }
            }
        }

        // Get the first pending open folding with offset.
        for (auto it = openFoldingMap.begin(); it != openFoldingMap.end(); ++it) {
            if (openRegionOffset == -1 || it->first < openRegionOffset) {
                openRegionType = it.key();
                openRegionOffset = it->first;
            }
        }
    }

    if (openRegionType == 0) {
        return nullptr;
    }

    // Search the following lines for the matched close folding.
    {
        int numOfOpenRegions = 1;
        for (auto nextBlock = block.next(); nextBlock.isValid(); nextBlock = nextBlock.next()) {
            auto nextData = TextBlockData::get(nextBlock);
            for (const auto &folding : nextData->getFoldings()) {
                if (folding.m_value == -openRegionType) {
                    // Found one match.
                    --numOfOpenRegions;
                    if (numOfOpenRegions == 0) {
                        // Don't return a valid folding range without contents (only two lines).
                        if (nextBlock.blockNumber() - p_blockNumber == 1) {
                            return nullptr;
                        }

                        // Got the range.
                        return QSharedPointer<TextBlockRange>::create(block, nextBlock);
                    }
                } else if (folding.m_value == openRegionType) {
                    // Another open folding in the same type.
                    ++numOfOpenRegions;
                }
            }
        }
    }

    // Since we could not find a matched close folding, the open folding range spans
    // to the end of the document.
    return QSharedPointer<TextBlockRange>::create(block, document()->end());
}

TextFolding *VTextEditor::getTextFolding() const
{
    return m_folding;
}

bool VTextEditor::isReadOnly() const
{
    return false;
}

void VTextEditor::focusInEvent(QFocusEvent *p_event)
{
    auto inputMode = getInputMode();
    if (inputMode) {
        inputMode->focusIn();
    }

    emit focusIn();
}

void VTextEditor::focusOutEvent(QFocusEvent *p_event)
{
    auto inputMode = getInputMode();
    if (inputMode) {
        inputMode->focusOut();
    }

    emit focusOut();
}

Completer *VTextEditor::completer() const
{
    if (!s_completer) {
        const_cast<VTextEditor *>(this)->s_completer = new Completer();
    }

    return s_completer;
}

void VTextEditor::triggerCompletion(bool p_reversed)
{
    if (isReadOnly()) {
        return;
    }

    auto prefixRange = Completer::findCompletionPrefix(m_completerInterface.data());
    auto candidates = Completer::generateCompletionCandidates(m_completerInterface.data(),
                                                              prefixRange.first,
                                                              prefixRange.second,
                                                              p_reversed);

    const QRect popupRect = m_textEdit->cursorRect();
    completer()->triggerCompletion(m_completerInterface.data(),
                                   candidates,
                                   prefixRange,
                                   p_reversed,
                                   popupRect);
}

bool VTextEditor::isCompletionActive() const
{
    return completer()->isActive(m_completerInterface.data());
}

void VTextEditor::completionNext(bool p_reversed)
{
    completer()->next(p_reversed);
}

void VTextEditor::completionExecute()
{
    completer()->execute();
}

void VTextEditor::abortCompletion()
{
    completer()->finishCompletion();
}

QSharedPointer<QWidget> VTextEditor::statusWidget()
{
    if (!m_statusWidget) {
        m_statusWidget.reset(createStatusWidget());
        updateCursorOfStatusWidget();
        updateSyntaxOfStatusWidget();
        updateModeOfStatusWidget();
        updateInputModeStatusWidget();
    }

    return m_statusWidget;
}

StatusIndicator *VTextEditor::createStatusWidget() const
{
    auto widget = new StatusIndicator();
    return widget;
}

void VTextEditor::updateCursorOfStatusWidget()
{
    if (m_statusWidget) {
        auto cursor = m_textEdit->textCursor();
        m_statusWidget->updateCursor(m_textEdit->document()->blockCount(),
                                     cursor.block().blockNumber() + 1,
                                     cursor.positionInBlock());
    }
}

void VTextEditor::updateSyntaxOfStatusWidget()
{
    if (m_statusWidget) {
        m_statusWidget->updateSyntax(getSyntax());
    }
}

void VTextEditor::updateModeOfStatusWidget()
{
    if (m_statusWidget) {
        m_statusWidget->updateMode(getEditorMode());
    }
}

void VTextEditor::updateInputModeStatusWidget()
{
    if (m_statusWidget) {
        m_statusWidget->updateInputModeStatusWidget(getInputMode()->statusWidget());
    }
}

EditorMode VTextEditor::getEditorMode() const
{
    auto inputMode = getInputMode();
    Q_ASSERT(inputMode);
    return inputMode->editorMode();
}
// End of Document
