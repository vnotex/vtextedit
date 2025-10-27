#ifndef EDITORINPUTMODE_H
#define EDITORINPUTMODE_H

#include <QObject>
#include <inputmode/inputmodeeditorinterface.h>

class QTextDocument;

namespace vte {
class VTextEditor;
class VTextEdit;

class EditorInputMode : public QObject, public InputModeEditorInterface {
  Q_OBJECT
public:
  explicit EditorInputMode(VTextEditor *p_editor);

public:
  // InputModeEditorInterface.
  void setCaretStyle(CaretStyle p_style) Q_DECL_OVERRIDE;

  void clearSelection() Q_DECL_OVERRIDE;

  void updateCursor(int p_line, int p_column) Q_DECL_OVERRIDE;

  int linesDisplayed() Q_DECL_OVERRIDE;

  void notifyEditorModeChanged(EditorMode p_mode) Q_DECL_OVERRIDE;

  void scrollUp() Q_DECL_OVERRIDE;

  void scrollDown() Q_DECL_OVERRIDE;

  void setSelection(int p_startLine, int p_startColumn, int p_endLine,
                    int p_endColumn) Q_DECL_OVERRIDE;

public:
  // KateViEditorInterface.
  QTextCursor textCursor() const Q_DECL_OVERRIDE;

  void copyToClipboard(const QString &p_text) Q_DECL_OVERRIDE;

  void editStart() Q_DECL_OVERRIDE;

  void editEnd() Q_DECL_OVERRIDE;

  bool removeText(const KateViI::Range &p_range, bool p_blockWise) Q_DECL_OVERRIDE;

  bool removeLine(int p_line) Q_DECL_OVERRIDE;

  void setSelection(const KateViI::Range &p_range) Q_DECL_OVERRIDE;

  void setBlockSelection(bool p_enabled) Q_DECL_OVERRIDE;

  int lineLength(int p_line) const Q_DECL_OVERRIDE;

  QString currentTextLine() const Q_DECL_OVERRIDE;

  QString line(int p_line) const Q_DECL_OVERRIDE;

  QStringList textLines(const KateViI::Range &p_range, bool p_blockWise) const Q_DECL_OVERRIDE;

  QString getText(const KateViI::Range &p_range, bool p_blockWise) const Q_DECL_OVERRIDE;

  KateViI::Cursor cursorPosition() const Q_DECL_OVERRIDE;

  int lines() const Q_DECL_OVERRIDE;

  void removeSelection() Q_DECL_OVERRIDE;

  KateViI::Range selectionRange() const Q_DECL_OVERRIDE;

  QVector<KateViI::Range> searchText(const KateViI::Range &p_range, const QString &p_pattern,
                                     const KateViI::SearchOptions p_options) const Q_DECL_OVERRIDE;

  KateViI::Cursor documentEnd() const Q_DECL_OVERRIDE;

  KateViI::Range documentRange() const Q_DECL_OVERRIDE;

  void setUndoMergeAllEdits(bool p_merge) Q_DECL_OVERRIDE;

  bool isUndoMergeAllEditsEnabled() const Q_DECL_OVERRIDE;

  void notifyViewModeChanged(KateViI::ViewMode p_mode) Q_DECL_OVERRIDE;

  KateViI::ViewMode viewMode() const Q_DECL_OVERRIDE;

  void cursorPrevChar(bool p_selection) Q_DECL_OVERRIDE;

  void connectSelectionChanged(std::function<void()> p_slot) Q_DECL_OVERRIDE;

  void connectTextInserted(std::function<void(const KateViI::Range &)> p_slot) Q_DECL_OVERRIDE;

  void update() Q_DECL_OVERRIDE;

  int toVirtualColumn(int p_line, int p_column, int p_tabWidth) const Q_DECL_OVERRIDE;

  int fromVirtualColumn(int p_line, int p_virtualColumn, int p_tabWidth) const Q_DECL_OVERRIDE;

  int endLine() const Q_DECL_OVERRIDE;

  int lastLine() const Q_DECL_OVERRIDE;

  KateViI::Cursor goVisualLineUpDownDry(int p_lines, bool &p_succeed) Q_DECL_OVERRIDE;

  QChar characterAt(const KateViI::Cursor &p_cursor) const Q_DECL_OVERRIDE;

  int lineToVisibleLine(int p_line) const Q_DECL_OVERRIDE;

  int visibleLineToLine(int p_line) const Q_DECL_OVERRIDE;

  int firstChar(int p_line) const Q_DECL_OVERRIDE;

  void joinLines(uint p_first, uint p_last, bool p_trimSpace) Q_DECL_OVERRIDE;

  int undoCount() const Q_DECL_OVERRIDE;

  void undo() Q_DECL_OVERRIDE;

  int redoCount() const Q_DECL_OVERRIDE;

  void redo() Q_DECL_OVERRIDE;

  bool replaceText(const KateViI::Range &p_range, const QString &p_text,
                   bool p_blockWise) Q_DECL_OVERRIDE;

  bool selection() const Q_DECL_OVERRIDE;

  bool insertText(const KateViI::Cursor &p_position, const QString &p_text,
                  bool p_blockWise) Q_DECL_OVERRIDE;

  void indent(const KateViI::Range &p_range, int p_changes) Q_DECL_OVERRIDE;

  void pageDown(bool p_half) Q_DECL_OVERRIDE;

  void pageUp(bool p_half) Q_DECL_OVERRIDE;

  void scrollInPage(const KateViI::Cursor &p_pos, KateViI::PagePosition p_dest) Q_DECL_OVERRIDE;

  void align(const KateViI::Range &p_range) Q_DECL_OVERRIDE;

  void align() Q_DECL_OVERRIDE;

  void clearOverriddenSelection() Q_DECL_OVERRIDE;

  void backspace() Q_DECL_OVERRIDE;

  void newLine(KateViI::NewLineIndent p_indent) Q_DECL_OVERRIDE;

  void abortCompletion() Q_DECL_OVERRIDE;

  bool setCursorPosition(KateViI::Cursor p_position) Q_DECL_OVERRIDE;

  bool insertLine(int p_line, const QString &p_str) Q_DECL_OVERRIDE;

  void setOverwriteMode(bool p_enabled) Q_DECL_OVERRIDE;

  QWidget *focusProxy() const Q_DECL_OVERRIDE;

  bool isCompletionActive() const Q_DECL_OVERRIDE;

  void completionNext(bool p_reversed) Q_DECL_OVERRIDE;

  // When generating selection items, whether do it reversedly or not.
  void userInvokedCompletion(bool p_reversed) Q_DECL_OVERRIDE;

  void completionExecute() Q_DECL_OVERRIDE;

  // Connect @p_slot to focusIn signal.
  void connectFocusIn(std::function<void()> p_slot) Q_DECL_OVERRIDE;

  // Connect @p_slot to focusOut signal.
  void connectFocusOut(std::function<void()> p_slot) Q_DECL_OVERRIDE;

  QString wordAt(const KateViI::Cursor &cursor) const Q_DECL_OVERRIDE;

  bool isReadOnly() const Q_DECL_OVERRIDE;

  void connectMouseReleased(std::function<void(QMouseEvent *)> p_slot) Q_DECL_OVERRIDE;

public:
  // MarkInterface.
  void connectMarkChanged(
      std::function<void(KateViI::KateViEditorInterface *p_editorInterface, KateViI::Mark p_mark,
                         KateViI::MarkInterface::MarkChangeAction p_action)>
          p_slot) Q_DECL_OVERRIDE;

  void removeMark(int line, uint markType) Q_DECL_OVERRIDE;

private:
  QTextDocument *document() const;

  KateViI::Cursor toKateViCursor(const QTextCursor &p_cursor) const;

  // QTextCursor.position() to KateViI::Cursor.
  KateViI::Cursor toKateViCursor(int p_position) const;

  int kateViCursorToPosition(const KateViI::Cursor &p_cursor) const;

  QTextCursor kateViCursorToTextCursor(const KateViI::Cursor &p_cursor) const;

  QTextCursor kateViRangeToTextCursor(const KateViI::Range &p_range) const;

  // How many blocks in one page scroll step.
  int blockCountOfOnePageStep() const;

  VTextEditor *m_editor = nullptr;

  VTextEdit *m_textEdit = nullptr;

  // Used to pair editStart and editEnd.
  int m_editSessionCount = 0;

  // Whether need to merge all edit blocks.
  bool m_mergeAllEditsEnabled = false;

  // If m_mergeAllEdits is true, this member indicates whether it is the
  // first edit session in the merge.
  bool m_firstEditSessionInMerge = false;

  // Just use a random and rare init value.
  EditorMode m_mode = EditorMode::ViModeReplace;

  // Work around of the Qt bug.
  // See editStart() and editEnd().
  int m_verticalScrollBarValue = 0;
};
} // namespace vte

#endif // EDITORINPUTMODE_H
