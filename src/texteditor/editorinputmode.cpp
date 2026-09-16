#include "editorinputmode.h"

#include <QTextBlock>
#include <QTextDocument>

#include <vtextedit/vtextedit.h>
#include <vtextedit/vtexteditor.h>

#include "textfolding.h"

using namespace vte;

EditorInputMode::EditorInputMode(VTextEditor *p_editor)
    : TextEditInputMode(p_editor->getTextEdit()), m_editor(p_editor) {}

void EditorInputMode::updateCursor(int p_line, int p_column) {
  auto block = document()->findBlockByNumber(p_line);
  if (!block.isValid() || p_column < 0 || p_column >= block.length()) {
    return;
  }
  auto folding = m_editor->getTextFolding();
  qint64 previousOwner = TextFolding::InvalidRangeId;
  while (!block.isVisible()) {
    const auto owner = folding->outermostFoldedRangeOnBlock(p_line);
    if (owner == TextFolding::InvalidRangeId || owner == previousOwner ||
        folding->toggleRange(owner) == false) {
      return;
    }
    previousOwner = owner;
    block = document()->findBlockByNumber(p_line);
    if (!block.isValid()) {
      return;
    }
  }
  TextEditInputMode::updateCursor(p_line, p_column);
}

int EditorInputMode::lineToVisibleLine(int p_line) const {
  return m_editor->getTextFolding()->lineToVisibleLine(p_line);
}

int EditorInputMode::visibleLineToLine(int p_line) const {
  return m_editor->getTextFolding()->visibleLineToLine(p_line);
}

void EditorInputMode::abortCompletion() { m_editor->abortCompletion(); }

bool EditorInputMode::isCompletionActive() const { return m_editor->isCompletionActive(); }

void EditorInputMode::completionNext(bool p_reversed) { m_editor->completionNext(p_reversed); }

// When generating selection items, whether do it reversedly or not.
void EditorInputMode::userInvokedCompletion(bool p_reversed) {
  m_editor->triggerCompletion(p_reversed);
}

void EditorInputMode::completionExecute() { m_editor->completionExecute(); }

void EditorInputMode::connectFocusIn(std::function<void()> p_slot) {
  connect(m_editor, &VTextEditor::focusIn, this, p_slot);
}

void EditorInputMode::connectFocusOut(std::function<void()> p_slot) {
  connect(m_editor, &VTextEditor::focusOut, this, p_slot);
}

bool EditorInputMode::isReadOnly() const { return m_editor->isReadOnly(); }

bool EditorInputMode::foldAtCursor() { return m_editor->foldAtCursor(); }

bool EditorInputMode::unfoldAtCursor() { return m_editor->unfoldAtCursor(); }
