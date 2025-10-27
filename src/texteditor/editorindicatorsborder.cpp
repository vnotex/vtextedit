#include "editorindicatorsborder.h"

#include <vtextedit/vtextedit.h>
#include <vtextedit/vtexteditor.h>

#include <vtextedit/texteditutils.h>

#include "textfolding.h"

#include <QAbstractTextDocumentLayout>
#include <QMouseEvent>
#include <QScrollBar>
#include <QTextDocument>

using namespace vte;

EditorIndicatorsBorder::EditorIndicatorsBorder(VTextEditor *p_editor)
    : IndicatorsBorderInterface(), m_editor(p_editor), m_textEdit(m_editor->m_textEdit) {}

int EditorIndicatorsBorder::blockCount() const { return document()->blockCount(); }

QTextDocument *EditorIndicatorsBorder::document() const { return m_textEdit->document(); }

QTextBlock EditorIndicatorsBorder::firstVisibleBlock() const {
  return TextEditUtils::firstVisibleBlock(m_textEdit);
}

QTextBlock EditorIndicatorsBorder::findBlockByYPosition(int p_y) const {
  return TextEditUtils::findBlockByYPosition(document(), p_y);
}

QAbstractTextDocumentLayout *EditorIndicatorsBorder::documentLayout() const {
  return document()->documentLayout();
}

int EditorIndicatorsBorder::contentOffsetAtTop() const {
  return TextEditUtils::contentOffsetAtTop(m_textEdit);
}

QSize EditorIndicatorsBorder::viewportSize() const { return m_textEdit->viewport()->size(); }

QRectF EditorIndicatorsBorder::blockBoundingRect(const QTextBlock &p_block) const {
  return documentLayout()->blockBoundingRect(p_block);
}

int EditorIndicatorsBorder::cursorBlockNumber() const {
  return m_textEdit->textCursor().block().blockNumber();
}

QTextBlock EditorIndicatorsBorder::cursorBlock() const { return m_textEdit->textCursor().block(); }

bool EditorIndicatorsBorder::hasInvisibleBlocks() const {
  return m_editor->m_folding->hasFoldedFolding();
}

TextFolding &EditorIndicatorsBorder::textFolding() const { return *(m_editor->m_folding); }

QWidget *EditorIndicatorsBorder::viewWidget() const { return m_textEdit; }

void EditorIndicatorsBorder::forwardWheelEvent(QWheelEvent *p_event) {
  return m_textEdit->wheelEvent(p_event);
}

void EditorIndicatorsBorder::forwardMouseEvent(QMouseEvent *p_event) {
  switch (p_event->type()) {
  case QEvent::MouseButtonPress:
    m_textEdit->mousePressEvent(p_event);
    break;

  case QEvent::MouseButtonRelease:
    m_textEdit->mouseReleaseEvent(p_event);
    break;

  default:
    break;
  }
}

QSharedPointer<TextBlockRange>
EditorIndicatorsBorder::fetchSyntaxFoldingRangeStartingOnBlock(int p_blockNumber) {
  return m_editor->fetchSyntaxFoldingRangeStartingOnBlock(p_blockNumber);
}

void EditorIndicatorsBorder::setFocus() { m_textEdit->setFocus(); }
