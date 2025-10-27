#include "editorcompleter.h"

#include <vtextedit/texteditorconfig.h>
#include <vtextedit/vtextedit.h>
#include <vtextedit/vtexteditor.h>

using namespace vte;

EditorCompleter::EditorCompleter(VTextEditor *p_editor)
    : CompleterInterface(), m_editor(p_editor) {}

QTextCursor EditorCompleter::textCursor() const { return m_editor->m_textEdit->textCursor(); }

QString EditorCompleter::contents() const { return m_editor->m_textEdit->toPlainText(); }

QWidget *EditorCompleter::widget() const { return m_editor->m_textEdit; }

QString EditorCompleter::getText(int p_start, int p_end) const {
  return m_editor->m_textEdit->getTextByRange(p_start, p_end);
}

qreal EditorCompleter::scaleFactor() const { return m_editor->getConfig().m_scaleFactor; }

QTextDocument *EditorCompleter::document() const { return m_editor->m_textEdit->document(); }

void EditorCompleter::insertCompletion(int p_start, int p_end, const QString &p_completion) {
  if (p_start >= 0 && p_end >= p_start) {
    Q_ASSERT(!p_completion.isEmpty());
    auto textEdit = m_editor->m_textEdit;
    auto cursor = textEdit->textCursor();
    cursor.setPosition(p_start);
    cursor.setPosition(p_end, QTextCursor::KeepAnchor);
    cursor.insertText(p_completion);
    textEdit->setTextCursor(cursor);
  }
}
