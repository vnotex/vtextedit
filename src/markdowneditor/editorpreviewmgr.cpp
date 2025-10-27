#include "editorpreviewmgr.h"

#include <QTextDocument>

#include <vtextedit/texteditorconfig.h>
#include <vtextedit/vmarkdowneditor.h>
#include <vtextedit/vtextedit.h>

#include "textdocumentlayout.h"
#include <vtextedit/pegmarkdownhighlighter.h>

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
  m_editor->documentLayout()->relayout(p_blocks);
  m_editor->updateIndicatorsBorder();
}

void EditorPreviewMgr::ensureCursorVisible() { m_editor->getTextEdit()->ensureCursorVisible(); }
