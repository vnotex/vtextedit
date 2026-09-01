#include <vtextedit/vmarkdowneditor.h>

#include <inputmode/abstractinputmode.h>
#include <inputmode/inputmodemgr.h>
#include <vtextedit/markdowneditorconfig.h>
#include <vtextedit/markdownhighlighter.h>
#include <vtextedit/markdownutils.h>
#include <vtextedit/textblockdata.h>
#include <vtextedit/texteditutils.h>
#include <vtextedit/textutils.h>
#include <vtextedit/theme.h>
#include <vtextedit/vtextedit.h>

#include <texteditor/textfolding.h>

#include "documentresourcemgr.h"
#include "editormarkdownhighlighter.h"
#include "editorpreviewmgr.h"
#include "interactivepreviewhost.h"
#include "ksyntaxcodeblockhighlighter.h"
#include "markdownfoldingprovider.h"
#include "mathblockhighlighter.h"
#include "textdocumentlayout.h"
#include "webcodeblockhighlighter.h"

#include <QDebug>
#include <QFontMetricsF>
#include <QScrollBar>

using namespace vte;

VMarkdownEditor::VMarkdownEditor(const QSharedPointer<MarkdownEditorConfig> &p_config,
                                 const QSharedPointer<TextEditorParameters> &p_paras,
                                 QWidget *p_parent)
    : VTextEditor(p_config->m_textEditorConfig, p_paras, p_parent), m_config(p_config) {
  setupDocumentLayout();

  setupSyntaxHighlighter();

  setupPreviewMgr();

  // Setup folding provider.
  m_foldingProvider.reset(new MarkdownFoldingProvider(getTextFolding(), document()));
  connect(getHighlighter(), &MarkdownHighlighter::foldingRegionsUpdated, this,
          [this](const QVector<md::FoldingRegion> &p_regions) {
            m_foldingProvider->updateFoldingRegions(p_regions);
            // Never evaluate the fold state synchronously here:
            // MarkdownHighlighter::completeHighlight() emits
            // foldingRegionsUpdated *before* previewElementsUpdated, so at this
            // instant the host still describes the previous generation.
            if (auto host = interactivePreviewHost()) {
              host->scheduleFoldRefresh();
            }
          });

  // Reset provider state when TextFolding is externally cleared
  // (e.g., during document replacement detected by hardClear).
  connect(getTextFolding(), &TextFolding::foldingRangesChanged, this, [this]() {
    if (getTextFolding()->isEmpty()) {
      m_foldingProvider->resetState();
    }

    // A manual fold or unfold from the gutter has to be written back
    // onto the preview item which owns that region.
    if (auto host = interactivePreviewHost()) {
      host->scheduleFoldRefresh();
    }
  });

  // Unnecessary for now.
  // m_textEdit->installEventFilter(this);

  // Hook keys.
  connect(m_textEdit, &VTextEdit::preKeyReturn, this, &VMarkdownEditor::preKeyReturn);
  connect(m_textEdit, &VTextEdit::postKeyReturn, this, &VMarkdownEditor::postKeyReturn);
  connect(m_textEdit, &VTextEdit::preKeyTab, this, &VMarkdownEditor::preKeyTab);
  connect(m_textEdit, &VTextEdit::preKeyBacktab, this, &VMarkdownEditor::preKeyBacktab);

  updateFromConfig();

  // Trigger update of stuffs after init.
  m_textEdit->setText("");
}

VMarkdownEditor::~VMarkdownEditor() {
  // The host is an ordinary QObject child, and QObject destroys its children
  // in creation order - which puts m_textEdit, its viewport and every preview
  // widget parented to it *before* the host. Its destructor asks a dirty sheet
  // to write itself back before the identity is dropped, and that needs the
  // document, the anchors and the widgets to still exist. So destroy it here,
  // while they do.
  delete interactivePreviewHost();
}

void VMarkdownEditor::setSyntax(const QString &p_syntax) {
  // Just ignore it.
  Q_UNUSED(p_syntax);
}

QString VMarkdownEditor::getSyntax() const { return QStringLiteral("richmarkdown"); }

void VMarkdownEditor::setupSyntaxHighlighter() {
  m_highlighterInterface.reset(new EditorMarkdownHighlighter(this));
  CodeBlockHighlighter *codeBlockHighlighter = nullptr;
  if (m_config->m_webCodeBlockHighlighterEnabled) {
    m_webCodeBlockHighlighter = new WebCodeBlockHighlighter(this);
    connect(m_webCodeBlockHighlighter,
            &WebCodeBlockHighlighter::externalCodeBlockHighlightRequested, this,
            &VMarkdownEditor::externalCodeBlockHighlightRequested);

    codeBlockHighlighter = m_webCodeBlockHighlighter;
  } else {
    codeBlockHighlighter =
        new KSyntaxCodeBlockHighlighter(m_config->m_textEditorConfig->m_syntaxTheme, this);
  }
  auto highlighterConfig = QSharedPointer<md::HighlighterConfig>::create();
  highlighterConfig->m_mathExtEnabled = true;

  m_mathBlockHighlighter = new MathBlockHighlighter(this);
  connect(m_mathBlockHighlighter, &MathBlockHighlighter::externalMathHighlightRequested, this,
          &VMarkdownEditor::externalMathHighlightRequested);

  m_highlighter =
      new MarkdownHighlighter(m_highlighterInterface.data(), document(), theme(),
                              codeBlockHighlighter, highlighterConfig, m_mathBlockHighlighter);
  updateSpellCheck();
  connect(getHighlighter(), &MarkdownHighlighter::highlightCompleted, this, [this]() {
    m_textEdit->updateCursorWidth();
    if (m_textEdit->isViewportWidgetFocused()) {
      // An in-place preview widget holds the focus. This re-parse is most
      // likely the one its own write-back caused, and the editor's caret is
      // elsewhere - scrolling to it would yank the viewport away from the
      // widget the user is typing in.
      return;
    }

    m_textEdit->ensureCursorVisible();
    m_textEdit->checkCenterCursor();
  });
}

void VMarkdownEditor::setupDocumentLayout() {
  m_resourceMgr.reset(new DocumentResourceMgr());

  auto docLayout = new TextDocumentLayout(document(), m_resourceMgr.data());
  docLayout->setPreviewEnabled(true);

  document()->setDocumentLayout(docLayout);

  connect(m_textEdit, &VTextEdit::cursorWidthChanged, this,
          [this]() { documentLayout()->setCursorWidth(m_textEdit->cursorWidth()); });
}

TextDocumentLayout *VMarkdownEditor::documentLayout() const {
  return static_cast<TextDocumentLayout *>(document()->documentLayout());
}

void VMarkdownEditor::setupPreviewMgr() {
  m_previewMgrInterface.reset(new EditorPreviewMgr(this));
  m_previewMgr = new PreviewMgr(m_previewMgrInterface.data(), this);
  m_previewMgr->setPreviewEnabled(true);
  connect(getHighlighter(), &MarkdownHighlighter::imageLinksUpdated, m_previewMgr,
          &PreviewMgr::updateImageLinks);
  connect(m_previewMgr, &PreviewMgr::requestUpdateImageLinks, getHighlighter(),
          &MarkdownHighlighter::updateHighlight);

  // Interactive preview widgets. The host is an internal QObject child so no
  // exported class needs a new data member.
  auto host = new InteractivePreviewHost(this);
  connect(getHighlighter(), &MarkdownHighlighter::previewElementsUpdated, host,
          &InteractivePreviewHost::updatePreviews);
}

InteractivePreviewHost *VMarkdownEditor::interactivePreviewHost() const {
  return findChild<InteractivePreviewHost *>(QLatin1String(InteractivePreviewHost::c_objectName),
                                             Qt::FindDirectChildrenOnly);
}

bool VMarkdownEditor::registerPreviewWidgetFactory(PreviewWidgetFactory *p_factory,
                                                   int p_priority) {
  auto host = interactivePreviewHost();
  return host ? host->registerFactory(p_factory, p_priority) : false;
}

bool VMarkdownEditor::unregisterPreviewWidgetFactory(PreviewWidgetFactory *p_factory) {
  auto host = interactivePreviewHost();
  return host ? host->unregisterFactory(p_factory) : false;
}

DocumentResourceMgr *VMarkdownEditor::getDocumentResourceMgr() const {
  return m_resourceMgr.data();
}

const QPixmap *VMarkdownEditor::findImageFromDocumentResourceMgr(const QString &p_name) const {
  return m_resourceMgr->findImage(p_name);
}

MarkdownHighlighter *VMarkdownEditor::getHighlighter() const {
  return static_cast<MarkdownHighlighter *>(m_highlighter);
}

PreviewMgr *VMarkdownEditor::getPreviewMgr() const { return m_previewMgr; }

void VMarkdownEditor::setConfig(const QSharedPointer<MarkdownEditorConfig> &p_config) {
  m_config = p_config;
  m_config->fillDefaultTheme();

  VTextEditor::setConfig(p_config->m_textEditorConfig);

  // The base VTextEditor::updateFromConfig only re-applies the syntax theme to a
  // SyntaxHighlighter; the MarkdownHighlighter is not one, so its styles stay
  // locked to the theme set at construction. Re-apply the new theme explicitly
  // and rehighlight so a theme switch refreshes editor colors.
  if (auto *hl = getHighlighter()) {
    hl->setTheme(theme());
    hl->rehighlight();
  }

  updateFromConfig();
}

void VMarkdownEditor::updateFromConfig() {
  Q_ASSERT(m_config);

  documentLayout()->setConstrainPreviewWidthEnabled(
      m_config->m_constrainInplacePreviewWidthEnabled);

  updateInplacePreviewSources();

  // Not ANDed with the text folding switch: the provider gates on
  // TextFolding::isEnabled() itself, which also covers the restore path. And
  // deliberately not retroactive - a region which has already been settled
  // keeps the state it was settled into.
  m_foldingProvider->setAutoFoldPreviewsEnabled(m_config->m_autoFoldPreviewedBlocksEnabled);

  // Also deliberately not retroactive: a table which is already in the
  // document keeps the shape it has until the user edits it.
  if (auto host = interactivePreviewHost()) {
    host->setTableSourceAlignEnabled(m_config->m_alignTableSourceEnabled);
  }

  applyLineSpacing();

  updateSpaceWidth();
}

void VMarkdownEditor::applyPreviewFolding() {
  if (!m_foldingProvider) {
    return;
  }

  auto vbar = m_textEdit->verticalScrollBar();
  auto layout = documentLayout();

  // Auto-folding a previewed block HIDES its source, so the document gets
  // SHORTER - the mirror image of the growth EditorPreviewMgr::relayout()
  // compensates for, and a second, later geometry change that its anchor has
  // already been retired by the time this runs. QScrollBar keeps its value()
  // across the range update either way, so without an anchor here every fold
  // above the viewport slides the visible text UP.
  //
  // Anchor on the first visible block, exactly as the relayout path does.
  // blockBoundingRect() lazily repairs a stale block layout, so it must not be
  // called from inside a layout pass.
  int originValue = 0;
  int anchorBlockNumber = -1;
  int originRevision = 0;
  qreal anchorViewportY = 0;
  bool anchored = false;

  if (!m_inFoldScrollAnchor && vbar && layout && !layout->isBusy()) {
    originValue = vbar->value();
    // At the very top nothing above the viewport can displace the content.
    if (originValue != vbar->minimum()) {
      const auto anchorBlock = TextEditUtils::firstVisibleBlock(m_textEdit);
      if (anchorBlock.isValid()) {
        anchored = true;
        anchorBlockNumber = anchorBlock.blockNumber();
        anchorViewportY = layout->blockBoundingRect(anchorBlock).y() - originValue;
        originRevision = document()->revision();
      }
    }
  }

  auto host = interactivePreviewHost();
  const auto ranges = host ? host->previewedRanges() : QVector<PreviewedRange>();
  const auto states =
      m_foldingProvider->applyPreviewAutoFold(ranges, m_textEdit->textCursor().blockNumber());
  if (host) {
    host->setPreviewFoldStates(states);
  }

  if (!anchored) {
    return;
  }

  // applyPreviewAutoFold() reaches application code synchronously - foldRange()
  // emits foldingRangesChanged(), and the geometry that follows hands a context
  // to every preview widget - so NOTHING captured above may be trusted without
  // being re-established first. The provider re-resolves its own table across
  // the same boundary for exactly this reason.
  //
  // A replaced scrollbar or layout leaves the captured pointers dangling, and a
  // document edit makes anchorBlockNumber name a different line. Any of those
  // means the correction describes a document nobody is looking at any more;
  // drop it rather than guess.
  if (m_textEdit->verticalScrollBar() != vbar || documentLayout() != layout ||
      document()->revision() != originRevision || layout->isBusy()) {
    return;
  }

  // The same "user scrolled meanwhile" rule as
  // InteractivePreviewHost::applyRealizationScrollCompensation(): a newer
  // scroll always wins over an older correction.
  if (vbar->value() != originValue) {
    return;
  }

  auto anchorBlock = document()->findBlockByNumber(anchorBlockNumber);
  if (!anchorBlock.isValid()) {
    return;
  }

  // The anchor may have been folded away. TextFolding::setRangeFolded() keeps a
  // region's FIRST and last block visible, so the hidden content collapsed
  // upwards into a block above; walking backwards lands on it and keeps the
  // anchor at or above where the user was looking. Walking forward is only the
  // fallback for a document whose head is entirely hidden, which
  // blockBoundingRect() could not measure at all.
  if (!anchorBlock.isVisible()) {
    auto probe = anchorBlock.previous();
    while (probe.isValid() && !probe.isVisible()) {
      probe = probe.previous();
    }

    if (!probe.isValid()) {
      probe = anchorBlock.next();
      while (probe.isValid() && !probe.isVisible()) {
        probe = probe.next();
      }
    }

    if (!probe.isValid()) {
      return;
    }

    anchorBlock = probe;
  }

  const qreal newDocY = layout->blockBoundingRect(anchorBlock).y();
  const int target = qBound(vbar->minimum(), qRound(newDocY - anchorViewportY), vbar->maximum());
  if (target == originValue) {
    return;
  }

  m_inFoldScrollAnchor = true;
  vbar->setValue(target);
  m_inFoldScrollAnchor = false;
}

bool VMarkdownEditor::restoreFoldAfterPreviewRewrite(PreviewElementType p_type, int p_startBlock,
                                                     int p_endBlock) {
  if (m_foldingProvider &&
      m_foldingProvider->restoreFoldedRange(p_type, p_startBlock, p_endBlock)) {
    // The fold was destroyed and restored synchronously inside the rewrite's
    // turn. The folded-line background is an extra selection whose cursor was
    // dragged past the replacement by Qt, and the corrected list is only
    // scheduled behind a 200ms coalescing timer. Push it now so the stale
    // position never reaches the repaint which follows this call. Only owed
    // when a range was really created: the restore is a no-op for a replacement
    // which collapsed the element onto a single block, and then nothing changed.
    applyPendingExtraSelections();
    return true;
  }

  return false;
}

bool VMarkdownEditor::tryPreviewSourceFolded(PreviewElementType p_type, int p_startBlock,
                                             int p_endBlock, bool *p_folded) const {
  return m_foldingProvider
             ? m_foldingProvider->tryRegionFolded(p_type, p_startBlock, p_endBlock, p_folded)
             : false;
}

void VMarkdownEditor::setInplacePreviewEnabled(bool p_enabled) {
  if (m_inplacePreviewEnabled == p_enabled) {
    return;
  }

  m_inplacePreviewEnabled = p_enabled;
  updateInplacePreviewSources();
}

void VMarkdownEditor::updateInplacePreviewSources() {
  auto host = interactivePreviewHost();

  if (!m_inplacePreviewEnabled) {
    m_previewMgr->setPreviewEnabled(false);
    if (host) {
      host->setEnabled(false);
    }
    return;
  }

  if (host) {
    host->setEnabled(true);
    host->setTypeEnabled(PreviewElementType::Image,
                         m_config->m_inplacePreviewSources & MarkdownEditorConfig::ImageLink);
    host->setTypeEnabled(PreviewElementType::Code,
                         m_config->m_inplacePreviewSources & MarkdownEditorConfig::CodeBlock);
    host->setTypeEnabled(PreviewElementType::Math,
                         m_config->m_inplacePreviewSources & MarkdownEditorConfig::Math);
    host->setTypeEnabled(PreviewElementType::Table,
                         m_config->m_inplacePreviewSources & MarkdownEditorConfig::Table);
  }

  // The painted path only knows about image, code and math.
  const auto paintedSources = m_config->m_inplacePreviewSources &
                              (MarkdownEditorConfig::ImageLink | MarkdownEditorConfig::CodeBlock |
                               MarkdownEditorConfig::Math);
  if (paintedSources == (MarkdownEditorConfig::ImageLink | MarkdownEditorConfig::CodeBlock |
                         MarkdownEditorConfig::Math)) {
    m_previewMgr->setPreviewEnabled(true);
  } else {
    m_previewMgr->setPreviewEnabled(false);
    if (paintedSources & MarkdownEditorConfig::ImageLink) {
      m_previewMgr->setPreviewEnabled(PreviewData::Source::ImageLink, true);
    }
    if (paintedSources & MarkdownEditorConfig::CodeBlock) {
      m_previewMgr->setPreviewEnabled(PreviewData::Source::CodeBlock, true);
    }
    if (paintedSources & MarkdownEditorConfig::Math) {
      m_previewMgr->setPreviewEnabled(PreviewData::Source::MathBlock, true);
    }
  }
}

bool VMarkdownEditor::eventFilter(QObject *p_obj, QEvent *p_event) {
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

bool VMarkdownEditor::handleKeyPressEvent(QKeyEvent *p_event) {
  Q_UNUSED(p_event);
  return false;
}

void VMarkdownEditor::zoom(int p_delta) {
  const int preFontSize = editorFontPointSize();
  VTextEditor::zoom(p_delta);
  const int postFontSize = editorFontPointSize();

  if (preFontSize == postFontSize) {
    return;
  }

  getHighlighter()->updateStylesFontSize(postFontSize - preFontSize);

  updateSpaceWidth();
  applyLineSpacing();
}

void VMarkdownEditor::applyLineSpacing() {
  qreal multiplier = qMax(m_config->m_textEditorConfig->m_lineSpacing, 1.0);
  QFontMetricsF fmf(m_textEdit->font(), m_textEdit);
  const qreal leadingSpace = fmf.lineSpacing() * (multiplier - 1.0);

  if (qFuzzyCompare(documentLayout()->getLeadingSpaceOfLine() + 1.0, leadingSpace + 1.0)) {
    return;
  }

  documentLayout()->setLeadingSpaceOfLine(leadingSpace);
  documentLayout()->relayout();
}

void VMarkdownEditor::updateSpaceWidth() {
  const auto &codeBlockFormat = getHighlighter()->codeBlockStyle();
  auto font = codeBlockFormat.font();
  if (codeBlockFormat.fontPointSize() < 0.001) {
    font.setPointSize(editorFontPointSize());
  }

  QFontMetricsF fmf(font, m_textEdit);
  m_textEdit->setSpaceWidth(fmf.horizontalAdvance(QLatin1Char(' ')));
}

void VMarkdownEditor::preKeyReturn(int p_modifiers, bool *p_changed, bool *p_handled) {
  Q_ASSERT(!m_textEdit->isReadOnly());

  // Probe the AST before any block is inserted, so postKeyReturn can consume it.
  m_returnBlockContext = getHighlighter()->getBlockContext(m_textEdit->textCursor().blockNumber());

  if (p_modifiers == Qt::ShiftModifier) {
    *p_changed = true;
    auto cursor = m_textEdit->textCursor();
    cursor.beginEditBlock();
    cursor.insertText(QStringLiteral("  "));
    cursor.endEditBlock();
    m_textEdit->setTextCursor(cursor);
  } else if (p_modifiers == Qt::NoModifier) {
    auto cursor = m_textEdit->textCursor();
    if (cursor.hasSelection()) {
      // Let handleKeyReturn perform its normal selection-replacing block split.
      // The probe was taken at the active end of the selection, which is not
      // necessarily the line surviving the split, so it must not be able to
      // drive an insertion. Suppression-only data is kept: vetoing is always
      // the safe direction.
      m_returnBlockContext.m_fresh = false;
      m_returnBlockContext.m_quoteDepth = 0;
      return;
    }

    if (m_returnBlockContext.m_valid && m_returnBlockContext.m_inFencedCode) {
      // Never strip markers inside a fence.
      return;
    }

    const auto block = cursor.block();
    const auto text = block.text().left(cursor.positionInBlock());

    QString indent, quotePrefix, rest;
    int depth = 0;
    const bool quoted = MarkdownUtils::isQuote(text, indent, quotePrefix, rest, depth);
    const QString &listSource = quoted ? rest : text;

    QChar listMark;
    QString listNumber;
    bool isEmpty = false;
    const bool isList = MarkdownUtils::isTodoList(listSource, listMark, isEmpty) ||
                        MarkdownUtils::isUnorderedList(listSource, listMark, isEmpty) ||
                        MarkdownUtils::isOrderedList(listSource, listNumber, isEmpty);

    QString replacement;
    bool handled = false;
    if (isList && isEmpty) {
      // Drop only the list marker.
      replacement = quoted ? (indent + quotePrefix + TextUtils::fetchIndentationSpaces(rest))
                           : TextUtils::fetchIndentationSpaces(text);
      handled = true;
    }
    // A bare quote line ("> " or ">") is a blank line *inside* the quote, not a
    // request to leave it, so no quote level is ever stripped here. Enter just
    // starts another quote line via postKeyReturn.

    if (handled) {
      cursor.beginEditBlock();
      cursor.setPosition(block.position(), QTextCursor::KeepAnchor);
      cursor.removeSelectedText();
      cursor.insertText(replacement);
      cursor.endEditBlock();
      m_textEdit->setTextCursor(cursor);

      *p_changed = true;
      *p_handled = true;
    }
  }
}

void VMarkdownEditor::postKeyReturn(int p_modifiers) {
  Q_ASSERT(!m_textEdit->isReadOnly());
  const auto blockContext = m_returnBlockContext;
  m_returnBlockContext = md::BlockContext();

  if (p_modifiers != Qt::NoModifier) {
    return;
  }

  auto cursor = m_textEdit->textCursor();

  const auto block = cursor.block();
  auto preBlock = block.previous();
  Q_ASSERT(preBlock.isValid());
  const auto preText = preBlock.text();

  if (blockContext.m_valid && blockContext.m_inFencedCode) {
    // Never continue markers inside a fence.
    return;
  }

  if (preText.isEmpty()) {
    return;
  }

  // Already indented by VTextEdit.

  QString indent, quotePrefix, rest;
  int textDepth = 0;
  bool quoted = MarkdownUtils::isQuote(preText, indent, quotePrefix, rest, textDepth);

  if (!quoted && blockContext.m_fresh && blockContext.m_quoteDepth > 0) {
    // Lazy continuation: the AST knows this line belongs to a quote although
    // the text carries no marker. Insert-causing, so it requires a fresh AST.
    quotePrefix = QStringLiteral("> ").repeated(blockContext.m_quoteDepth);
    // The leading indentation has already been copied into the new block by
    // AutoIndentHelper::autoIndent.
    rest = preText.mid(TextUtils::fetchIndentation(preText));
    quoted = true;
  }

  const QString &listSource = quoted ? rest : preText;

  QString marker;
  QChar listMark;
  QString listNumber;
  bool isEmpty = false;
  if (MarkdownUtils::isTodoList(listSource, listMark, isEmpty)) {
    marker = QStringLiteral("%1 [ ] ").arg(listMark);
  } else if (MarkdownUtils::isUnorderedList(listSource, listMark, isEmpty)) {
    marker = QStringLiteral("%1 ").arg(listMark);
  } else if (MarkdownUtils::isOrderedList(listSource, listNumber, isEmpty)) {
    marker = QStringLiteral("%1. ").arg(listNumber.toInt() + 1);
  }

  const QString innerIndent = quoted ? TextUtils::fetchIndentationSpaces(rest) : QString();
  const QString textToInsert = quotePrefix + innerIndent + marker;
  if (textToInsert.isEmpty()) {
    return;
  }

  cursor.joinPreviousEditBlock();
  cursor.insertText(textToInsert);
  cursor.endEditBlock();
  m_textEdit->setTextCursor(cursor);
}

void VMarkdownEditor::preKeyTab(int p_modifiers, bool *p_handled) {
  Q_ASSERT(!m_textEdit->isReadOnly());
  if (p_modifiers == Qt::NoModifier) {
    auto cursor = m_textEdit->textCursor();
    if (cursor.hasSelection()) {
      return;
    }

    const auto block = cursor.block();
    const auto text = block.text().left(cursor.positionInBlock());
    if (text.isEmpty()) {
      return;
    }

    QChar listMark;
    bool isEmpty = false;
    if (MarkdownUtils::isTodoList(text, listMark, isEmpty) ||
        MarkdownUtils::isUnorderedList(text, listMark, isEmpty)) {
      // Indent the empty todo/unordered list.
      if (isEmpty) {
        *p_handled = true;
        TextEditUtils::indentBlock(cursor, !m_textEdit->isTabExpanded(),
                                   m_textEdit->getTabStopWidthInSpaces(), false);
        m_textEdit->setTextCursor(cursor);
      }
      return;
    }

    QString listNumber;
    if (MarkdownUtils::isOrderedList(text, listNumber, isEmpty) && isEmpty) {
      *p_handled = true;
      // Reset the list number and indent the empty ordered list.
      auto afterText = MarkdownUtils::setOrderedListNumber(text, 1);
      cursor.beginEditBlock();
      if (afterText != text) {
        cursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::KeepAnchor);
        cursor.insertText(afterText);
      }
      TextEditUtils::indentBlock(cursor, !m_textEdit->isTabExpanded(),
                                 m_textEdit->getTabStopWidthInSpaces(), false);
      cursor.endEditBlock();
      m_textEdit->setTextCursor(cursor);
      return;
    }
  }
}

void VMarkdownEditor::preKeyBacktab(int p_modifiers, bool *p_handled) {
  Q_ASSERT(!m_textEdit->isReadOnly());
  if (p_modifiers == Qt::ShiftModifier) {
    auto cursor = m_textEdit->textCursor();
    if (cursor.hasSelection()) {
      return;
    }

    const auto block = cursor.block();
    const auto text = block.text().left(cursor.positionInBlock());
    if (text.isEmpty()) {
      return;
    }

    QChar listMark;
    bool isEmpty = false;
    if (MarkdownUtils::isTodoList(text, listMark, isEmpty) ||
        MarkdownUtils::isUnorderedList(text, listMark, isEmpty)) {
      // Unindent the empty todo/unordered list.
      if (isEmpty) {
        *p_handled = true;
        TextEditUtils::unindentBlock(cursor, m_textEdit->getTabStopWidthInSpaces());
        m_textEdit->setTextCursor(cursor);
      }
      return;
    }

    QString listNumber;
    if (MarkdownUtils::isOrderedList(text, listNumber, isEmpty) && isEmpty) {
      *p_handled = true;

      cursor.beginEditBlock();

      // Unindent the empty ordered list.
      TextEditUtils::unindentBlock(cursor, m_textEdit->getTabStopWidthInSpaces());

      const auto newText = block.text().left(cursor.positionInBlock());
      Q_ASSERT(MarkdownUtils::isOrderedList(newText, listNumber, isEmpty));

      // Try to correct the list number.
      int newNumber = 1;
      {
        const auto preBlock = block.previous();
        if (preBlock.isValid()) {
          const auto preText = preBlock.text();
          if (TextUtils::fetchIndentation(preText) == TextUtils::fetchIndentation(newText)) {
            QString preListNumber;
            bool preIsEmpty = false;
            if (MarkdownUtils::isOrderedList(preText, preListNumber, preIsEmpty)) {
              newNumber = preListNumber.toInt() + 1;
            }
          }
        }
      }

      auto afterText = MarkdownUtils::setOrderedListNumber(newText, newNumber);
      if (afterText != newText) {
        cursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::KeepAnchor);
        cursor.insertText(afterText);
      }
      cursor.endEditBlock();
      m_textEdit->setTextCursor(cursor);
      return;
    }
  }
}

void VMarkdownEditor::handleExternalCodeBlockHighlightData(int p_idx, TimeStamp p_timeStamp,
                                                           const QString &p_html) {
  Q_ASSERT(m_webCodeBlockHighlighter);
  m_webCodeBlockHighlighter->handleExternalCodeBlockHighlightData(p_idx, p_timeStamp, p_html);
}

void VMarkdownEditor::setExternalCodeBlockHighlihgtStyles(
    const ExternalCodeBlockHighlightStyles &p_styles) {
  WebCodeBlockHighlighter::setExternalCodeBlockHighlihgtStyles(p_styles);
}

void VMarkdownEditor::handleExternalMathHighlightData(int p_idx, TimeStamp p_timeStamp,
                                                      const QString &p_html) {
  Q_ASSERT(m_mathBlockHighlighter);
  m_mathBlockHighlighter->handleExternalMathHighlightData(p_idx, p_timeStamp, p_html);
}
