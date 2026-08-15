#ifndef EDITORINPUTMODE_H
#define EDITORINPUTMODE_H

#include <inputmode/texteditinputmode.h>

namespace vte {
class VTextEditor;

// Editor interface backed by a VTextEditor, which provides text folding and
// completion on top of the plain VTextEdit facilities.
class EditorInputMode : public TextEditInputMode {
  Q_OBJECT
public:
  explicit EditorInputMode(VTextEditor *p_editor);

public:
  // InputModeEditorInterface.
  bool foldAtCursor() Q_DECL_OVERRIDE;

  bool unfoldAtCursor() Q_DECL_OVERRIDE;

public:
  // KateViEditorInterface.
  int lineToVisibleLine(int p_line) const Q_DECL_OVERRIDE;

  int visibleLineToLine(int p_line) const Q_DECL_OVERRIDE;

  void abortCompletion() Q_DECL_OVERRIDE;

  bool isCompletionActive() const Q_DECL_OVERRIDE;

  void completionNext(bool p_reversed) Q_DECL_OVERRIDE;

  // When generating selection items, whether do it reversedly or not.
  void userInvokedCompletion(bool p_reversed) Q_DECL_OVERRIDE;

  void completionExecute() Q_DECL_OVERRIDE;

  // Connect @p_slot to focusIn signal.
  void connectFocusIn(std::function<void()> p_slot) Q_DECL_OVERRIDE;

  // Connect @p_slot to focusOut signal.
  void connectFocusOut(std::function<void()> p_slot) Q_DECL_OVERRIDE;

  bool isReadOnly() const Q_DECL_OVERRIDE;

private:
  VTextEditor *m_editor = nullptr;
};
} // namespace vte

#endif // EDITORINPUTMODE_H
