#include "editorpegmarkdownhighlighter.h"

#include <vtextedit/texteditutils.h>
#include <vtextedit/vtextedit.h>
#include <vtextedit/vtexteditor.h>

using namespace vte;

EditorPegMarkdownHighlighter::EditorPegMarkdownHighlighter(VTextEditor *p_editor)
    : PegMarkdownHighlighterInterface(), m_editor(p_editor) {}

QTextCursor EditorPegMarkdownHighlighter::textCursor() const {
  return m_editor->getTextEdit()->textCursor();
}

QPair<int, int> EditorPegMarkdownHighlighter::visibleBlockRange() const {
  return TextEditUtils::visibleBlockRange(m_editor->getTextEdit());
}

void EditorPegMarkdownHighlighter::ensureCursorVisible() {
  m_editor->getTextEdit()->ensureCursorVisible();
}

QScrollBar *EditorPegMarkdownHighlighter::verticalScrollBar() const {
  return m_editor->getTextEdit()->verticalScrollBar();
}
