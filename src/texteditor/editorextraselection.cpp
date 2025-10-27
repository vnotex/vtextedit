#include "editorextraselection.h"

#include <vtextedit/vtextedit.h>
#include <vtextedit/vtexteditor.h>

using namespace vte;

EditorExtraSelection::EditorExtraSelection(VTextEditor *p_editor)
    : ExtraSelectionInterface(), m_editor(p_editor) {}

QTextCursor EditorExtraSelection::textCursor() const { return m_editor->m_textEdit->textCursor(); }

QString EditorExtraSelection::selectedText() const { return m_editor->m_textEdit->selectedText(); }

void EditorExtraSelection::setExtraSelections(
    const QList<QTextEdit::ExtraSelection> &p_selections) {
  m_editor->m_textEdit->setExtraSelections(p_selections);
}

QList<QTextCursor> EditorExtraSelection::findAllText(const QString &p_text,
                                                     bool p_isRegularExpression,
                                                     bool p_caseSensitive) {
  FindFlags flags = None;
  if (p_isRegularExpression) {
    flags |= FindFlag::RegularExpression;
  }
  if (p_caseSensitive) {
    flags |= FindFlag::CaseSensitive;
  }
  return m_editor->m_textEdit->findAllText(p_text, flags);
}
