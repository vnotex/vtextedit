#include <vtextedit/vtexteditor.h>

#include <vtextedit/texteditorconfig.h>
#include <vtextedit/vtextedit.h>
#include <vtextedit/theme.h>
#include <vtextedit/textblockdata.h>
#include <inputmode/inputmodemgr.h>
#include <inputmode/abstractinputmode.h>
#include <inputmode/viinputmode.h>

#include "indicatorsborder.h"
#include "editorindicatorsborder.h"
#include "editorextraselection.h"
#include "textfolding.h"
#include "syntaxhighlighter.h"
#include "ksyntaxhighlighterwrapper.h"
#include "editorinputmode.h"
#include "editorcompleter.h"
#include "statusindicator.h"

#include <vtextedit/texteditutils.h>

#include <QHBoxLayout>
#include <QScrollBar>
#include <QHash>
#include <QPair>
#include <QDebug>
#include <QRegularExpression>

using namespace vte;

int VTextEditor::s_instanceCount = 0;

Completer *VTextEditor::s_completer = nullptr;

void VTextEditor::FindResultCache::clear()
{
    m_start = -1;
    m_end = -1;

    m_text.clear();
    m_flags = FindFlag::None;
    m_result.clear();
}

bool VTextEditor::FindResultCache::matched(const QString &p_text, FindFlags p_flags, int p_start, int p_end) const
{
    return (m_flags & ~FindFlag::FindBackward) == (p_flags & ~FindFlag::FindBackward)
           && m_start == p_start
           && m_end == p_end
           && m_text == p_text;
}

void VTextEditor::FindResultCache::update(const QString &p_text,
                                          FindFlags p_flags,
                                          int p_start,
                                          int p_end,
                                          const QList<QTextCursor> &p_result)
{
    m_text = p_text;
    m_flags = p_flags;
    m_start = p_start;
    m_end = p_end;
    m_result = p_result;
}

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

    setupCompleter();

    connect(m_textEdit, &VTextEdit::contentsChanged,
            this, &VTextEditor::clearFindResultCache);

    // Status widget.
    connect(m_textEdit, &QTextEdit::cursorPositionChanged,
            this, &VTextEditor::updateCursorOfStatusWidget);
    connect(this, &VTextEditor::syntaxChanged,
            this, &VTextEditor::updateSyntaxOfStatusWidget);
    connect(this, &VTextEditor::modeChanged,
            this, &VTextEditor::updateModeOfStatusWidget);

    // Input method.
    connect(this, &VTextEditor::modeChanged,
            this, [this]() {
                auto mode = getEditorMode();
                switch (mode) {
                case ViModeNormal:
                    Q_FALLTHROUGH();
                case ViModeVisual:
                    Q_FALLTHROUGH();
                case ViModeVisualLine:
                    Q_FALLTHROUGH();
                case ViModeVisualBlock:
                    m_textEdit->setInputMethodEnabled(false);
                    break;

                default:
                    m_textEdit->setInputMethodEnabled(true);
                    break;
                }
            });

    updateFromConfig();

    ++s_instanceCount;

    // Trigger necessary parts after init.
    m_extraSelectionMgr->updateAllExtraSelections();
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

    connect(m_textEdit, &VTextEdit::contentsChanged,
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

    setFocusProxy(m_textEdit);

    m_textEdit->installEventFilter(this);
}

void VTextEditor::setText(const QString &p_text)
{
    m_textEdit->setPlainText(p_text);
}

QString VTextEditor::getText() const
{
    return m_textEdit->toPlainText();
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

    m_incrementalSearchExtraSelection = m_extraSelectionMgr->registerExtraSelection();
    m_extraSelectionMgr->setExtraSelectionEnabled(m_incrementalSearchExtraSelection, true);

    m_searchExtraSelection = m_extraSelectionMgr->registerExtraSelection();
    m_extraSelectionMgr->setExtraSelectionEnabled(m_searchExtraSelection, true);

    m_searchUnderCursorExtraSelection = m_extraSelectionMgr->registerExtraSelection();
    m_extraSelectionMgr->setExtraSelectionEnabled(m_searchUnderCursorExtraSelection, true);
}

void VTextEditor::setupCompleter()
{
    m_completerInterface.reset(new EditorCompleter(this));
}

void VTextEditor::updateFromConfig()
{
    Q_ASSERT(m_config);
    // QTextEdit::font() will reflect the real font point size (after zoom). So we need
    // to remember the original font point size.
    static const int baseFontPointSize = m_textEdit->font().pointSize();

    auto &theme = m_config->m_theme;
    if (!theme) {
        theme = TextEditorConfig::defaultTheme();
    }

    // Editor font and palette.
    {
        m_themeFont = m_textEdit->font();
        m_themePalette = m_textEdit->palette();

        const auto &fmt = theme->editorStyle(Theme::EditorStyle::Text);

        if (!fmt.m_fontFamily.isEmpty()) {
            m_themeFont.setFamily(fmt.m_fontFamily);
        }

        if (fmt.m_fontPointSize > 0) {
            m_themeFont.setPointSize(fmt.m_fontPointSize);
        } else {
            m_themeFont.setPointSize(baseFontPointSize);
        }

        auto textColor = fmt.textColor();
        if (textColor.isValid()) {
            m_themePalette.setColor(QPalette::Text, textColor);
        }

        auto bgColor = fmt.backgroundColor();
        if (bgColor.isValid()) {
            m_themePalette.setColor(QPalette::Base, bgColor);
        }

        auto selectionColor = fmt.selectedTextColor();
        if (selectionColor.isValid()) {
            m_themePalette.setColor(QPalette::HighlightedText, selectionColor);
        }

        auto selectionBgColor = fmt.selectedBackgroundColor();
        if (selectionBgColor.isValid()) {
            m_themePalette.setColor(QPalette::Highlight, selectionBgColor);
        }

        setFontAndPaletteByStyleSheet(m_themeFont, m_themePalette);
    }

    {
        m_folding->setEnabled(m_config->m_textFoldingEnabled);
        const auto &fmt = theme->editorStyle(Theme::FoldedFoldingRangeLine);
        m_folding->setFoldedFoldingRangeLineBackgroundColor(fmt.backgroundColor());
    }

    updateExtraSelectionMgrFromConfig();

    updateIndicatorsBorderFromConfig();

    setInputMode(m_config->m_inputMode);

    m_textEdit->setCenterCursor(m_config->m_centerCursor);

    {
        // Wrap mode.
        QTextOption::WrapMode wrapMode = QTextOption::WrapAtWordBoundaryOrAnywhere;
        switch (m_config->m_wrapMode) {
        case WrapMode::NoWrap:
            wrapMode = QTextOption::NoWrap;
            break;

        case WrapMode::WordWrap:
            wrapMode = QTextOption::WordWrap;
            break;

        case WrapMode::WrapAnywhere:
            wrapMode = QTextOption::WrapAnywhere;
            break;

        case WrapMode::WordWrapOrAnywhere:
            wrapMode = QTextOption::WrapAtWordBoundaryOrAnywhere;
            break;
        }

        m_textEdit->setWordWrapMode(wrapMode);
    }

    // Tab stop width.
    {
        m_textEdit->setExpandTab(m_config->m_expandTab);

        if (m_config->m_tabStopWidth < 1) {
            m_config->m_tabStopWidth = 4;
        }
        m_textEdit->setTabStopWidthInSpaces(m_config->m_tabStopWidth);
    }
}

void VTextEditor::setInputMode(InputMode p_mode)
{
    auto currentMode = m_textEdit->getInputMode();
    if (currentMode && currentMode->mode() == p_mode) {
        return;
    }

    QScopedPointer<EditorInputMode> newInputMode(new EditorInputMode(this));

    auto modeFactory = InputModeMgr::getInst().getInputModeFactory(p_mode);
    Q_ASSERT(modeFactory);
    auto mode = modeFactory->createInputMode(newInputMode.data());
    m_textEdit->setInputMode(mode);

    m_inputModeInterface.swap(newInputMode);

    updateStatusWidget();
}

QSharedPointer<AbstractInputMode> VTextEditor::getInputMode() const
{
    return m_textEdit->getInputMode();
}

void VTextEditor::addSyntaxCustomSearchPaths(const QStringList &p_paths)
{
    // Custom search path will be added only once.
    KSyntaxHighlighterWrapper::Initialize(p_paths);
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

    if (!m_syntax.isEmpty() && SyntaxHighlighter::isValidSyntax(m_syntax)) {
        m_highlighter = new SyntaxHighlighter(document(), m_config->m_syntaxTheme, m_syntax);
    } else {
        m_syntax = QStringLiteral("plaintext");
    }

    emit syntaxChanged();
}

QString VTextEditor::getSyntax() const
{
    return m_syntax;
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
    {
        const auto &fmt = theme->editorStyle(Theme::IncrementalSearch);
        m_extraSelectionMgr->setExtraSelectionFormat(m_incrementalSearchExtraSelection,
                                                     fmt.textColor(),
                                                     fmt.backgroundColor(),
                                                     false);
    }
    {
        const auto &fmt = theme->editorStyle(Theme::Search);
        m_extraSelectionMgr->setExtraSelectionFormat(m_searchExtraSelection,
                                                     fmt.textColor(),
                                                     fmt.backgroundColor(),
                                                     false);
    }
    {
        const auto &fmt = theme->editorStyle(Theme::SearchUnderCursor);
        m_extraSelectionMgr->setExtraSelectionFormat(m_searchUnderCursorExtraSelection,
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
    {
        m_indicatorsBorder->setLineNumberType(indicatorsBorderLineNumberType(m_config->m_lineNumberType));
    }
    {
        m_indicatorsBorder->setTextFoldingEnabled(m_config->m_textFoldingEnabled);
    }

    m_indicatorsBorder->updateBorder();
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
    return m_textEdit->isReadOnly();
}

void VTextEditor::setReadOnly(bool p_enabled)
{
    m_textEdit->setReadOnly(p_enabled);
}

void VTextEditor::focusInEvent(QFocusEvent *p_event)
{
    QWidget::focusInEvent(p_event);

    auto inputMode = getInputMode();
    if (inputMode) {
        inputMode->focusIn();
    }

    emit focusIn();
}

void VTextEditor::focusOutEvent(QFocusEvent *p_event)
{
    QWidget::focusOutEvent(p_event);

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
    if (!m_statusIndicator) {
        m_statusIndicator.reset(createStatusWidget());
        updateStatusWidget();
    }

    return m_statusIndicator;
}

void VTextEditor::updateStatusWidget()
{
    updateCursorOfStatusWidget();
    updateSyntaxOfStatusWidget();
    updateModeOfStatusWidget();
    updateInputModeStatusWidget();
}

StatusIndicator *VTextEditor::createStatusWidget() const
{
    auto widget = new StatusIndicator();
    connect(widget, &StatusIndicator::focusOut,
            this, [this]() {
                m_textEdit->setFocus();
            });
    return widget;
}

void VTextEditor::updateCursorOfStatusWidget()
{
    if (m_statusIndicator) {
        auto pos = getCursorPosition();
        m_statusIndicator->updateCursor(m_textEdit->document()->blockCount(),
                                        pos.first + 1,
                                        pos.second);
    }
}

void VTextEditor::updateSyntaxOfStatusWidget()
{
    if (m_statusIndicator) {
        m_statusIndicator->updateSyntax(getSyntax());
    }
}

void VTextEditor::updateModeOfStatusWidget()
{
    if (m_statusIndicator) {
        m_statusIndicator->updateMode(getEditorMode());
    }
}

void VTextEditor::updateInputModeStatusWidget()
{
    if (m_statusIndicator) {
        m_statusIndicator->updateInputModeStatusWidget(getInputMode()->statusWidget());
    }
}

EditorMode VTextEditor::getEditorMode() const
{
    auto inputMode = getInputMode();
    Q_ASSERT(inputMode);
    return inputMode->editorMode();
}

QSharedPointer<Theme> VTextEditor::theme() const
{
    return m_config->m_theme;
}

const QString &VTextEditor::getBasePath() const
{
    return m_basePath;
}

void VTextEditor::setBasePath(const QString &p_basePath)
{
    m_basePath = p_basePath;
}

const TextEditorConfig &VTextEditor::getConfig() const
{
    return *m_config;
}

void VTextEditor::setConfig(const QSharedPointer<TextEditorConfig> &p_config)
{
    m_config = p_config;
    updateFromConfig();
}

bool VTextEditor::eventFilter(QObject *p_obj, QEvent *p_event)
{
    if (p_obj == m_textEdit) {
        if (p_event->type() == QEvent::FocusIn) {
            emit focusIn();
        }
    }

    return QWidget::eventFilter(p_obj, p_event);
}

bool VTextEditor::isModified() const
{
    return m_textEdit->document()->isModified();
}

void VTextEditor::setModified(bool p_modified)
{
    m_textEdit->document()->setModified(p_modified);
}

QPair<int, int> VTextEditor::getCursorPosition() const
{
    auto cursor = m_textEdit->textCursor();
    return qMakePair(cursor.block().blockNumber(),
                     cursor.positionInBlock());
}

int VTextEditor::getTopLine() const
{
    return TextEditUtils::firstVisibleBlock(m_textEdit).blockNumber();
}

void VTextEditor::scrollToLine(int p_lineNumber, bool p_forceCursor)
{
    if (p_lineNumber < 0) {
        return;
    }

    if (p_lineNumber >= document()->blockCount()) {
        p_lineNumber = document()->blockCount() - 1;
    }

    // First, scroll @p_lineNumber at the top. If cursor is not visible now,
    // set the cursor to @p_lineNumber. Otherwise, leave the cursor untouched.
    TextEditUtils::scrollBlockInPage(m_textEdit,
                                     p_lineNumber,
                                     TextEditUtils::PagePosition::Top,
                                     0);

    const auto rect = m_textEdit->cursorRect();
    if (p_forceCursor || rect.y() < 0 || rect.y() > m_textEdit->rect().height()) {
        // Move the cursor.
        auto cursor = m_textEdit->textCursor();
        auto block = document()->findBlockByNumber(p_lineNumber);
        cursor.setPosition(block.position() + block.length() - 1);
        m_textEdit->setTextCursor(cursor);
    }
}

void VTextEditor::enterInsertModeIfApplicable()
{
    auto inputMode = getInputMode();
    Q_ASSERT(inputMode);
    if (inputMode->mode() == InputMode::ViMode) {
        auto viInputMode = dynamic_cast<ViInputMode *>(inputMode.data());
        viInputMode->changeViInputMode(KateVi::ViMode::InsertMode);
    }
}

void VTextEditor::insertText(const QString &p_text)
{
    m_textEdit->insertPlainText(p_text);
}

int VTextEditor::zoomDelta() const
{
    return m_zoomDelta;
}

// zoomIn() and zoomOut() does not work if we set stylesheet of editor.
void VTextEditor::zoom(int p_delta)
{
    if (p_delta == m_zoomDelta) {
        return;
    }

    const int minSize = 2;
    int step = p_delta - m_zoomDelta;
    auto editFontPtSz = m_textEdit->font().pointSize();
    if (editFontPtSz <= minSize && step < 0) {
        return;
    }

    int ptSz = editFontPtSz + step;
    ptSz = qMax(ptSz, minSize);
    setFontPointSizeByStyleSheet(ptSz);

    // Indicator border.
    {
        auto font = m_indicatorsBorder->getFont();
        int ptSz = font.pointSize() + step;
        ptSz = qMax(ptSz, minSize);
        font.setPointSize(ptSz);
        m_indicatorsBorder->setFont(font);
    }

    m_zoomDelta = p_delta;
}

void VTextEditor::setFontPointSizeByStyleSheet(int p_ptSize)
{
    QFont ft = m_themeFont;
    ft.setPointSize(p_ptSize);
    setFontAndPaletteByStyleSheet(ft, m_themePalette);

    // To make sure the font().pointSize() is updated in case of continuous zoom.
    ensurePolished();
}

void VTextEditor::setFontAndPaletteByStyleSheet(const QFont &p_font, const QPalette &p_palette)
{
    QString styles(QString("vte--VTextEdit {"
                           "font-family: \"%1\";"
                           "font-size: %2pt;"
                           "color: %3;"
                           "background-color: %4;"
                           "selection-color: %5;"
                           "selection-background-color: %6; }")
                          .arg(p_font.family())
                          .arg(p_font.pointSize())
                          .arg(p_palette.color(QPalette::Text).name())
                          .arg(p_palette.color(QPalette::Base).name())
                          .arg(p_palette.color(QPalette::HighlightedText).name())
                          .arg(p_palette.color(QPalette::Highlight).name()));
    setStyleSheet(styles);
}

void VTextEditor::peekText(const QString &p_text, FindFlags p_flags)
{
    if (p_text.isEmpty()) {
        clearIncrementalSearchHighlight();
        return;
    }
    int start = m_textEdit->textCursor().position();
    int skipPosition = start;

    while (true) {
        auto cursor = m_textEdit->findText(p_text, p_flags, start);
        if (cursor.isNull()) {
            break;
        }

        if (!(p_flags & FindFlag::FindBackward) && cursor.selectionStart() == skipPosition) {
            // Skip the first match.
            skipPosition = -1;
            start = cursor.selectionEnd();
            continue;
        }

        TextEditUtils::ensureBlockVisible(m_textEdit, document()->findBlock(cursor.selectionStart()).blockNumber());
        QList<QTextCursor> selections;
        selections.append(cursor);
        m_extraSelectionMgr->setSelections(m_incrementalSearchExtraSelection, selections);
        return;
    }

    clearIncrementalSearchHighlight();
}

VTextEditor::FindResult VTextEditor::findText(const QString &p_text,
                                              FindFlags p_flags,
                                              int p_start,
                                              int p_end)
{
    QTextCursor cursor;
    auto result = findTextHelper(p_text, p_flags, p_start, p_end, true, cursor);
    if (!cursor.isNull()) {
        cursor.setPosition(cursor.selectionStart());
        m_textEdit->setTextCursor(cursor);
    }
    return result;
}

VTextEditor::FindResult VTextEditor::findTextHelper(const QString &p_text,
                                                  FindFlags p_flags,
                                                  int p_start,
                                                  int p_end,
                                                  bool p_skipCurrent,
                                                  QTextCursor &p_currentMatch)
{
    clearIncrementalSearchHighlight();

    FindResult result;
    if (p_text.isEmpty() || (p_start >= p_end && p_end >= 0)) {
        clearSearchHighlight();
        p_currentMatch = QTextCursor();
        return result;
    }

    const auto &allResults = findAllText(p_text, p_flags, p_start, p_end);
    if (!allResults.isEmpty()) {
        // Locate to the right match and update current cursor.
        auto cursor = m_textEdit->textCursor();
        bool wrapped = false;
        int idx = selectCursor(allResults,
                               cursor.position(),
                               p_skipCurrent,
                               !(p_flags & FindFlag::FindBackward),
                               wrapped);
        Q_ASSERT(idx != -1);
        p_currentMatch = allResults[idx];

        // Highlight.
        highlightSearch(allResults, idx);

        result.m_totalMatches = allResults.size();
        result.m_currentMatchIndex = idx;
        result.m_wrapped = wrapped;
    } else {
        clearSearchHighlight();
        p_currentMatch = QTextCursor();
    }

    return result;
}

VTextEditor::FindResult VTextEditor::replaceText(const QString &p_text,
                                                 FindFlags p_flags,
                                                 const QString &p_replaceText,
                                                 int p_start,
                                                 int p_end)
{
    QTextCursor cursor;
    auto result = findTextHelper(p_text, p_flags, p_start, p_end, false, cursor);
    if (result.m_totalMatches > 0) {
        Q_ASSERT(!cursor.isNull());
        result.m_totalMatches = 1;

        QString newText(p_replaceText);
        if (p_flags & FindFlag::RegularExpression) {
            resolveBackReferenceInReplaceText(newText, TextEditUtils::getSelectedText(cursor), QRegularExpression(p_text));
        }
        cursor.insertText(newText);
        m_textEdit->setTextCursor(cursor);
    }
    return result;
}

VTextEditor::FindResult VTextEditor::replaceAll(const QString &p_text,
                                                FindFlags p_flags,
                                                const QString &p_replaceText,
                                                int p_start,
                                                int p_end)
{
    clearIncrementalSearchHighlight();

    FindResult result;
    if (p_text.isEmpty() || (p_start >= p_end && p_end >= 0)) {
        clearSearchHighlight();
        return result;
    }

    const auto &allResults = findAllText(p_text, p_flags, p_start, p_end);
    if (!allResults.isEmpty()) {
        result.m_totalMatches = allResults.size();

        // Replace all matches one by one.
        auto cursor = m_textEdit->textCursor();
        cursor.beginEditBlock();
        QRegularExpression regExp(p_text);
        for (const auto &result : allResults) {
            cursor.setPosition(result.selectionStart());
            cursor.setPosition(result.selectionEnd(), QTextCursor::KeepAnchor);

            QString newText(p_replaceText);
            if (p_flags & FindFlag::RegularExpression) {
                resolveBackReferenceInReplaceText(newText, TextEditUtils::getSelectedText(cursor), regExp);
            }
            cursor.insertText(newText);
        }
        cursor.endEditBlock();
        m_textEdit->setTextCursor(cursor);
    }

    clearSearchHighlight();
    return result;
}

void VTextEditor::clearIncrementalSearchHighlight()
{
    m_extraSelectionMgr->setSelections(m_incrementalSearchExtraSelection, QList<QTextCursor>());
}

void VTextEditor::clearFindResultCache()
{
    m_findResultCache.clear();
}

void VTextEditor::clearSearchHighlight()
{
    m_extraSelectionMgr->setSelections(m_searchExtraSelection, QList<QTextCursor>());
    m_extraSelectionMgr->setSelections(m_searchUnderCursorExtraSelection, QList<QTextCursor>());
}

const QList<QTextCursor> &VTextEditor::findAllText(const QString &p_text, FindFlags p_flags, int p_start, int p_end)
{
    if (p_text.isEmpty()) {
        m_findResultCache.clear();
        return m_findResultCache.m_result;
    }

    if (m_findResultCache.matched(p_text, p_flags, p_start, p_end)) {
        return m_findResultCache.m_result;
    }

    m_findResultCache.update(p_text,
                             p_flags,
                             p_start,
                             p_end,
                             m_textEdit->findAllText(p_text, p_flags, p_start, p_end));

    return m_findResultCache.m_result;
}

int VTextEditor::selectCursor(const QList<QTextCursor> &p_cursors,
                              int p_pos,
                              bool p_skipCurrent,
                              bool p_forward,
                              bool &p_isWrapped)
{
    Q_ASSERT(!p_cursors.isEmpty());

    p_isWrapped = false;
    int first = 0, last = p_cursors.size() - 1;
    int lastMatch = -1;
    while (first <= last) {
        int mid = (first + last) / 2;
        const QTextCursor &cur = p_cursors.at(mid);
        if (p_forward) {
            if (cur.selectionStart() < p_pos) {
                first = mid + 1;
            } else if (cur.selectionStart() == p_pos) {
                if (!p_skipCurrent) {
                    // Found it.
                    lastMatch = mid;
                } else if (mid < p_cursors.size() - 1) {
                    // Next one is the right one.
                    lastMatch = mid + 1;
                } else {
                    lastMatch = 0;
                    p_isWrapped = true;
                }
                break;
            } else {
                // It is a match.
                if (lastMatch == -1 || mid < lastMatch) {
                    lastMatch = mid;
                }

                last = mid - 1;
            }
        } else {
            if (cur.selectionStart() > p_pos) {
                last = mid - 1;
            } else if (cur.selectionStart() == p_pos) {
                if (!p_skipCurrent) {
                    // Found it.
                    lastMatch = mid;
                } else if (mid > 0) {
                    // Previous one is the right one.
                    lastMatch = mid - 1;
                } else {
                    lastMatch = p_cursors.size() - 1;
                    p_isWrapped = true;
                }
                break;
            } else {
                // It is a match.
                if (lastMatch == -1 || mid > lastMatch) {
                    lastMatch = mid;
                }

                first = mid + 1;
            }
        }
    }

    if (lastMatch == -1) {
        p_isWrapped = true;
        lastMatch = p_forward ? 0 : (p_cursors.size() - 1);
    }

    return lastMatch;
}

void VTextEditor::highlightSearch(const QList<QTextCursor> &p_results, int p_currentIdx)
{
    Q_ASSERT(!p_results.isEmpty() && p_currentIdx >= 0);
    m_extraSelectionMgr->setSelections(m_searchExtraSelection, p_results);
    QList<QTextCursor> searchUnderCursor;
    searchUnderCursor << p_results[p_currentIdx];
    m_extraSelectionMgr->setSelections(m_searchUnderCursorExtraSelection, searchUnderCursor);
}

void VTextEditor::resolveBackReferenceInReplaceText(QString &p_replaceText,
                                                    QString p_text,
                                                    const QRegularExpression &p_regExp)
{
    p_replaceText = p_text.replace(p_regExp, p_replaceText);
}
