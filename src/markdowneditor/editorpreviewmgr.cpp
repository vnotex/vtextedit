#include "editorpreviewmgr.h"

#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

#include <vtextedit/texteditorconfig.h>
#include <vtextedit/texteditutils.h>
#include <vtextedit/vmarkdowneditor.h>
#include <vtextedit/vtextedit.h>

#include "interactivepreviewhost.h"
#include "textdocumentlayout.h"
#include <vtextedit/markdownhighlighter.h>

using namespace vte;

EditorPreviewMgr::EditorPreviewMgr(VMarkdownEditor *p_editor) : m_editor(p_editor) {}

QTextDocument *EditorPreviewMgr::document() const { return m_editor->document(); }

int EditorPreviewMgr::tabStopDistance() const { return m_editor->getTextEdit()->tabStopDistance(); }

const QString &EditorPreviewMgr::basePath() const { return m_editor->getBasePath(); }

DocumentResourceMgr *EditorPreviewMgr::documentResourceMgr() const {
  return m_editor->getDocumentResourceMgr();
}

qreal EditorPreviewMgr::scaleFactor() const { return m_editor->getConfig().m_scaleFactor; }

void EditorPreviewMgr::addPossiblePreviewBlock(int p_blockNumber) {
  m_editor->getHighlighter()->addPossiblePreviewBlock(p_blockNumber);
}

const QSet<int> &EditorPreviewMgr::getPossiblePreviewBlocks() const {
  return m_editor->getHighlighter()->getPossiblePreviewBlocks();
}

void EditorPreviewMgr::clearPossiblePreviewBlocks(const QVector<int> &p_blocksToClear) {
  m_editor->getHighlighter()->clearPossiblePreviewBlocks(p_blocksToClear);
}

void EditorPreviewMgr::relayout(const OrderedIntSet &p_blocks) {
  auto textEdit = m_editor->getTextEdit();
  auto layout = m_editor->documentLayout();
  auto vbar = textEdit->verticalScrollBar();

  // PreviewMgr::relayout() is the only caller of ensureCursorVisible(), and it
  // always calls it right after this function. Record here whether the caret is
  // on screen at all, so that a preview landing far away from the caret does not
  // yank the viewport back to it. See ensureCursorVisible().
  m_caretWasVisibleBeforeRelayout = true;

  // A painted preview grows its block at the BOTTOM, so every block after it -
  // including everything the user is currently looking at - slides down, while
  // QScrollBar keeps its value() across the documentSizeChanged range update.
  // Pin the first visible block instead: capture its document y, and after the
  // relayout put the scrollbar back where that same block sits at the same
  // viewport position. This is the painted-preview counterpart of
  // InteractivePreviewHost::applyRealizationScrollCompensation().
  //
  // blockBoundingRect() performs lazy layout repair, so it must never be called
  // from inside a layout pass; and the setValue() below fires a scroll that may
  // re-enter the preview pipeline, hence the re-entrancy guard.
  int originValue = 0;
  int anchorBlockNumber = -1;
  qreal anchorViewportY = 0;
  bool compensate = false;

  if (vbar && !layout->isBusy()) {
    // One binary search pair for BOTH answers. findBlockByYPosition() is not
    // free: every probe calls blockBoundingRect(), which lazily repairs a stale
    // block's layout, so the anchor is taken from range.first rather than by
    // asking firstVisibleBlock() for the very same y all over again.
    const auto range = TextEditUtils::visibleBlockRange(textEdit);
    const int caretBlock = textEdit->textCursor().blockNumber();
    // An invalid end means the range could not be resolved at all - a folded
    // region with no visible neighbour, say. Fail OPEN there: suppressing the
    // scroll to the caret on a range nobody could compute would strand the
    // caret off screen.
    m_caretWasVisibleBeforeRelayout = range.first < 0 || range.second < 0 ||
                                      (caretBlock >= range.first && caretBlock <= range.second);

    originValue = vbar->value();
    // At the very top nothing above the viewport can displace the content.
    if (!m_inScrollCompensation && originValue != vbar->minimum() && range.first >= 0) {
      const auto anchorBlock = m_editor->document()->findBlockByNumber(range.first);
      if (anchorBlock.isValid()) {
        compensate = true;
        anchorBlockNumber = range.first;
        anchorViewportY =
            m_editor->document()->documentLayout()->blockBoundingRect(anchorBlock).y() -
            originValue;
      }
    }
  }

  layout->relayout(p_blocks);
  m_editor->updateIndicatorsBorder();

  // The painted previews just changed, and they are one of the two things the
  // preview driven fold decision is made from. PreviewMgr clears the obsolete
  // entries and only then relayouts, so this is the first moment the block data
  // describes the new generation.
  if (auto host = m_editor->interactivePreviewHost()) {
    host->scheduleFoldRefresh();
  }

  if (!compensate || anchorBlockNumber < 0 || layout->isBusy()) {
    return;
  }

  // Mirrors applyRealizationScrollCompensation()'s guard: if the viewport moved
  // while we were laying out - a callback scrolled, or the user did - the
  // correction is about a position nobody is looking at any more.
  if (vbar->value() != originValue) {
    return;
  }

  const auto anchorBlock = m_editor->document()->findBlockByNumber(anchorBlockNumber);
  if (!anchorBlock.isValid()) {
    return;
  }

  const qreal newDocY = m_editor->document()->documentLayout()->blockBoundingRect(anchorBlock).y();
  const int target = qBound(vbar->minimum(), qRound(newDocY - anchorViewportY), vbar->maximum());
  if (target == originValue) {
    return;
  }

  m_inScrollCompensation = true;
  vbar->setValue(target);
  m_inScrollCompensation = false;
}

void EditorPreviewMgr::ensureCursorVisible() {
  auto textEdit = m_editor->getTextEdit();
  // See VTextEdit::isViewportWidgetFocused(): never scroll to the editor's
  // caret while an in-place preview widget owns the focus.
  if (textEdit->isViewportWidgetFocused()) {
    return;
  }

  // PreviewMgr::relayout() - the only caller - runs relayout() immediately
  // before this, which is where the flag is computed. A preview arriving while
  // the caret is off screen must not scroll the viewport to the caret: that is
  // the "yank" half of the flashing this class compensates for. The flag
  // defaults to true, so a caller that skips relayout() gets the old behaviour.
  if (!m_caretWasVisibleBeforeRelayout) {
    return;
  }

  textEdit->ensureCursorVisible();
}
