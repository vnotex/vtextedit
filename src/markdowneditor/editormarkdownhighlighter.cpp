#include "editormarkdownhighlighter.h"

#include <vtextedit/texteditutils.h>
#include <vtextedit/vtextedit.h>
#include <vtextedit/vtexteditor.h>

using namespace vte;

EditorMarkdownHighlighter::EditorMarkdownHighlighter(VTextEditor *p_editor)
    : MarkdownHighlighterInterface(), m_editor(p_editor) {}

QTextCursor EditorMarkdownHighlighter::textCursor() const {
  return m_editor->getTextEdit()->textCursor();
}

QPair<int, int> EditorMarkdownHighlighter::visibleBlockRange() const {
  return TextEditUtils::visibleBlockRange(m_editor->getTextEdit());
}

void EditorMarkdownHighlighter::ensureCursorVisible() {
  m_editor->getTextEdit()->ensureCursorVisible();
}

QScrollBar *EditorMarkdownHighlighter::verticalScrollBar() const {
  return m_editor->getTextEdit()->verticalScrollBar();
}
