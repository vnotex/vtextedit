#include <vtextedit/vmarkdowneditor.h>

#include <inputmode/abstractinputmode.h>
#include <inputmode/inputmodemgr.h>
#include <vtextedit/markdowneditorconfig.h>
#include <vtextedit/markdownutils.h>
#include <vtextedit/pegmarkdownhighlighter.h>
#include <vtextedit/textblockdata.h>
#include <vtextedit/texteditutils.h>
#include <vtextedit/textutils.h>
#include <vtextedit/theme.h>
#include <vtextedit/vtextedit.h>

#include "documentresourcemgr.h"
#include "editorpegmarkdownhighlighter.h"
#include "editorpreviewmgr.h"
#include "ksyntaxcodeblockhighlighter.h"
#include "textdocumentlayout.h"
#include "webcodeblockhighlighter.h"

#include <QDebug>

using namespace vte;

VMarkdownEditor::VMarkdownEditor(const QSharedPointer<MarkdownEditorConfig> &p_config,
                                 const QSharedPointer<TextEditorParameters> &p_paras,
                                 QWidget *p_parent)
    : VTextEditor(p_config->m_textEditorConfig, p_paras, p_parent), m_config(p_config) {
  setupDocumentLayout();

  setupSyntaxHighlighter();

  setupPreviewMgr();

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

VMarkdownEditor::~VMarkdownEditor() {}

void VMarkdownEditor::setSyntax(const QString &p_syntax) {
  // Just ignore it.
  Q_UNUSED(p_syntax);
}

QString VMarkdownEditor::getSyntax() const { return QStringLiteral("richmarkdown"); }

void VMarkdownEditor::setupSyntaxHighlighter() {
  m_highlighterInterface.reset(new EditorPegMarkdownHighlighter(this));
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
  auto highlighterConfig = QSharedPointer<peg::HighlighterConfig>::create();
  highlighterConfig->m_mathExtEnabled = true;
  m_highlighter = new PegMarkdownHighlighter(m_highlighterInterface.data(), document(), theme(),
                                             codeBlockHighlighter, highlighterConfig);
  updateSpellCheck();
  connect(getHighlighter(), &PegMarkdownHighlighter::highlightCompleted, this, [this]() {
    m_textEdit->updateCursorWidth();
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
  connect(getHighlighter(), &PegMarkdownHighlighter::imageLinksUpdated, m_previewMgr,
          &PreviewMgr::updateImageLinks);
  connect(m_previewMgr, &PreviewMgr::requestUpdateImageLinks, getHighlighter(),
          &PegMarkdownHighlighter::updateHighlight);
}

DocumentResourceMgr *VMarkdownEditor::getDocumentResourceMgr() const {
  return m_resourceMgr.data();
}

const QPixmap *VMarkdownEditor::findImageFromDocumentResourceMgr(const QString &p_name) const {
  return m_resourceMgr->findImage(p_name);
}

PegMarkdownHighlighter *VMarkdownEditor::getHighlighter() const {
  return static_cast<PegMarkdownHighlighter *>(m_highlighter);
}

PreviewMgr *VMarkdownEditor::getPreviewMgr() const { return m_previewMgr; }

void VMarkdownEditor::setConfig(const QSharedPointer<MarkdownEditorConfig> &p_config) {
  m_config = p_config;
  m_config->fillDefaultTheme();

  VTextEditor::setConfig(p_config->m_textEditorConfig);

  updateFromConfig();
}

void VMarkdownEditor::updateFromConfig() {
  Q_ASSERT(m_config);

  documentLayout()->setConstrainPreviewWidthEnabled(
      m_config->m_constrainInplacePreviewWidthEnabled);

  updateInplacePreviewSources();

  updateSpaceWidth();
}

void VMarkdownEditor::setInplacePreviewEnabled(bool p_enabled) {
  if (m_inplacePreviewEnabled == p_enabled) {
    return;
  }

  m_inplacePreviewEnabled = p_enabled;
  updateInplacePreviewSources();
}

void VMarkdownEditor::updateInplacePreviewSources() {
  if (!m_inplacePreviewEnabled) {
    m_previewMgr->setPreviewEnabled(false);
    return;
  }

  if (m_config->m_inplacePreviewSources ==
      (MarkdownEditorConfig::ImageLink | MarkdownEditorConfig::CodeBlock |
       MarkdownEditorConfig::Math)) {
    m_previewMgr->setPreviewEnabled(true);
  } else {
    m_previewMgr->setPreviewEnabled(false);
    if (m_config->m_inplacePreviewSources & MarkdownEditorConfig::ImageLink) {
      m_previewMgr->setPreviewEnabled(PreviewData::Source::ImageLink, true);
    }
    if (m_config->m_inplacePreviewSources & MarkdownEditorConfig::CodeBlock) {
      m_previewMgr->setPreviewEnabled(PreviewData::Source::CodeBlock, true);
    }
    if (m_config->m_inplacePreviewSources & MarkdownEditorConfig::Math) {
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

  if (p_modifiers == Qt::ShiftModifier) {
    *p_changed = true;
    auto cursor = m_textEdit->textCursor();
    cursor.beginEditBlock();
    cursor.insertText(QStringLiteral("  "));
    cursor.endEditBlock();
    m_textEdit->setTextCursor(cursor);
  } else if (p_modifiers == Qt::NoModifier) {
    auto cursor = m_textEdit->textCursor();
    const auto block = cursor.block();
    const auto text = block.text().left(cursor.positionInBlock());

    QChar listMark;
    QString listNumber;
    bool isEmpty = false;
    if ((MarkdownUtils::isTodoList(text, listMark, isEmpty) ||
         MarkdownUtils::isUnorderedList(text, listMark, isEmpty) ||
         MarkdownUtils::isOrderedList(text, listNumber, isEmpty)) &&
        isEmpty) {
      int indentation = TextUtils::fetchIndentation(text);
      cursor.beginEditBlock();
      cursor.removeSelectedText();
      cursor.setPosition(block.position() + indentation, QTextCursor::KeepAnchor);
      cursor.removeSelectedText();
      cursor.endEditBlock();
      m_textEdit->setTextCursor(cursor);

      *p_changed = true;
      *p_handled = true;
    }
  }
}

void VMarkdownEditor::postKeyReturn(int p_modifiers) {
  Q_ASSERT(!m_textEdit->isReadOnly());
  if (p_modifiers == Qt::NoModifier) {
    auto cursor = m_textEdit->textCursor();

    const auto block = cursor.block();
    auto preBlock = block.previous();
    Q_ASSERT(preBlock.isValid());
    const auto preText = preBlock.text();

    if (preText.isEmpty()) {
      return;
    }

    // Already indented by VTextEdit.

    bool changed = false;
    QChar listMark;
    QString listNumber;
    bool isEmpty = false;
    if (MarkdownUtils::isTodoList(preText, listMark, isEmpty)) {
      // Insert a todo list mark.
      Q_ASSERT(!isEmpty);
      changed = true;
      cursor.joinPreviousEditBlock();
      cursor.insertText(QStringLiteral("%1 [ ] ").arg(listMark));
    } else if (MarkdownUtils::isUnorderedList(preText, listMark, isEmpty)) {
      // Insert an unordered list mark.
      Q_ASSERT(!isEmpty);
      changed = true;
      cursor.joinPreviousEditBlock();
      cursor.insertText(QStringLiteral("%1 ").arg(listMark));
    } else if (MarkdownUtils::isOrderedList(preText, listNumber, isEmpty)) {
      // Insert an ordered list mark.
      Q_ASSERT(!isEmpty);
      changed = true;
      cursor.joinPreviousEditBlock();
      cursor.insertText(QStringLiteral("%1. ").arg(listNumber.toInt() + 1));
    }

    if (changed) {
      cursor.endEditBlock();
      m_textEdit->setTextCursor(cursor);
    }
  }
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
