#ifndef TABLEPREVIEWINPUTMODE_H
#define TABLEPREVIEWINPUTMODE_H

#include <inputmode/texteditinputmode.h>

class QTextTableCell;

namespace vte {
class TablePreviewSheet;

// The editor interface a TablePreviewSheet hands to one of the three input
// modes.
//
// THE BUFFER IS THE CARET'S CELL, projected as a ONE-LINE document and
// recomputed on every call (decision D1). The sheet already guarantees one
// cell is one QTextBlock, so nothing here has to reason about wrapping: the
// projection is
//
//     virtual line 0, column c   <->   physical position cellFirst() + c
//
// and every line other than 0 is off the end of the buffer. Confinement is
// therefore STRUCTURAL rather than a set of range checks: a katevi call can
// only address what this projection exposes, and the projection cannot
// describe a position outside the cell. That is only true while the
// projection is COMPLETE, which is why every one of the 69 interface methods
// is classified below. An unclassified method is a frame-corruption path:
// removing a selection which crosses a QTextTable frame boundary removes the
// frame, and TablePreviewDocument would be left holding a dangling
// QTextTable.
//
// THE CLASSIFICATION. Three outcomes, and nothing else.
//
// (A) OVERRIDDEN - reaches the document and had to be re-expressed against the
//     cell.
//
//       removeText, removeLine, setSelection (both overloads), setBlockSelection,
//       lineLength, currentTextLine, line, textLines, getText, cursorPosition,
//       lines, selectionRange, documentEnd, cursorPrevChar, connectTextInserted,
//       toVirtualColumn, fromVirtualColumn, endLine, goVisualLineUpDownDry,
//       characterAt, joinLines, undoCount, undo, redoCount, redo, replaceText,
//       insertText, indent, pageDown, pageUp, scrollInPage, align(Range),
//       align(), backspace, newLine, setCursorPosition, insertLine,
//       updateCursor, linesDisplayed, notifyEditorModeChanged, scrollUp,
//       scrollDown, editStart, setUndoMergeAllEdits.
//
//     editStart and setUndoMergeAllEdits are the odd ones: both bases are
//     already confined (neither has a range of its own), and both are
//     overridden for the UNDO ring rather than for safety. Between them they
//     are the only places which see a compound katevi command - or a whole
//     insert session - as ONE thing. See the comments on them.
//
// (B) DELIBERATELY INHERITED - the base implementation is already confined,
//     because it reaches the document only THROUGH a method in group (A), or
//     because it does not reach the document at all.
//
//       documentRange   - Cursor::start() .. documentEnd(), both projected.
//       lastLine        - lines() - 1, so 0.
//       firstChar       - scans line(p_line), which is projected.
//       lineToVisibleLine / visibleLineToLine - identity in the base already,
//                         and identity is right for a buffer with no folding.
//       removeSelection - clearSelection().
//       clearSelection  - collapses the live cursor, which the sheet's own
//                         clamps keep inside a cell, and drops the overridden
//                         selection. It cannot widen anything.
//       clearOverriddenSelection - drops a published range; never mutates.
//       textCursor      - hands out the live cursor. Safe for the same reason
//                         clearSelection() is: the sheet clamps it on every
//                         cursorPositionChanged and selectionChanged, so it is
//                         always inside a cell, and every katevi consumer of a
//                         POSITION goes through cursorPosition() instead.
//       selection       - a bool.
//       searchText      - the base is unimplemented and returns nothing, so
//                         '/' and '?' find nothing rather than escaping the
//                         cell. Not a regression: it never worked.
//       wordAt          - likewise unimplemented.
//       editStart / editEnd - begin/endEditBlock pairing plus the scroll-bar
//                         workaround. No range of its own; editStart is
//                         nonetheless overridden, for the undo ring.
//       copyToClipboard, update, setOverwriteMode, focusProxy, isReadOnly
//                       - no document range. isReadOnly() is the SHEET's,
//                         because m_textEdit is the sheet.
//       isUndoMergeAllEditsEnabled, viewMode, notifyViewModeChanged - mode
//                       bookkeeping.
//       setCaretStyle, foldAtCursor, unfoldAtCursor - the sheet has no folding.
//       abortCompletion, isCompletionActive, completionNext,
//       userInvokedCompletion, completionExecute - the sheet has no completer.
//       connectSelectionChanged, connectFocusIn, connectFocusOut,
//       connectMouseReleased - signal plumbing on the sheet itself.
//       connectMarkChanged, removeMark - MarkInterface, unimplemented in the
//                         base. Marks are per InputModeManager anyway, so they
//                         are per sheet and never cross into the editor
//                         (decision D5).
//
// (C) There is no group (C). Every method is in (A) or (B).
//
// WHAT IS DELIBERATELY NOT SUPPORTED. Motions and operators addressing the
// TABLE structurally (rows as lines), block-wise anything, and structural undo
// - u never reverts a merge or a row insert (decisions D1 and D2). A ':'
// command assuming a whole document sees a one-line one.
//
// WHAT IS SHARED WITH THE EDITOR. The factory hands out one KateVi::GlobalState
// for the whole process, so registers, macros, mappings and the command and
// search histories cross freely between the editor and any cell. Marks and
// jumps are per KateVi::InputModeManager and therefore do not (decision D5).
class TablePreviewInputMode : public TextEditInputMode {
  Q_OBJECT
public:
  explicit TablePreviewInputMode(TablePreviewSheet *p_sheet);

  // A key press is about to be dispatched. Resets the per-key-press undo
  // bookkeeping, unless it is a NESTED synthetic key fed by a `.` repeat, a
  // macro or a mapping; see the definition. Called by the sheet, which is the
  // only thing that sees key boundaries - katevi does not expose one.
  void notifyKeyPressBegin();

public:
  // InputModeEditorInterface.
  void updateCursor(int p_line, int p_column) Q_DECL_OVERRIDE;

  int linesDisplayed() Q_DECL_OVERRIDE;

  void setSelection(int p_startLine, int p_startColumn, int p_endLine,
                    int p_endColumn) Q_DECL_OVERRIDE;

  void notifyEditorModeChanged(EditorMode p_mode) Q_DECL_OVERRIDE;

  void scrollUp() Q_DECL_OVERRIDE;

  void scrollDown() Q_DECL_OVERRIDE;

public:
  // KateViEditorInterface.
  void setUndoMergeAllEdits(bool p_merge) Q_DECL_OVERRIDE;

  void editStart() Q_DECL_OVERRIDE;

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

  KateViI::Range selectionRange() const Q_DECL_OVERRIDE;

  KateViI::Cursor documentEnd() const Q_DECL_OVERRIDE;

  void cursorPrevChar(bool p_selection) Q_DECL_OVERRIDE;

  void connectTextInserted(std::function<void(const KateViI::Range &)> p_slot) Q_DECL_OVERRIDE;

  int toVirtualColumn(int p_line, int p_column, int p_tabWidth) const Q_DECL_OVERRIDE;

  int fromVirtualColumn(int p_line, int p_virtualColumn, int p_tabWidth) const Q_DECL_OVERRIDE;

  int endLine() const Q_DECL_OVERRIDE;

  KateViI::Cursor goVisualLineUpDownDry(int p_lines, bool &p_succeed) Q_DECL_OVERRIDE;

  QChar characterAt(const KateViI::Cursor &p_cursor) const Q_DECL_OVERRIDE;

  void joinLines(uint p_first, uint p_last, bool p_trimSpace) Q_DECL_OVERRIDE;

  int undoCount() const Q_DECL_OVERRIDE;

  void undo() Q_DECL_OVERRIDE;

  int redoCount() const Q_DECL_OVERRIDE;

  void redo() Q_DECL_OVERRIDE;

  bool replaceText(const KateViI::Range &p_range, const QString &p_text,
                   bool p_blockWise) Q_DECL_OVERRIDE;

  bool insertText(const KateViI::Cursor &p_position, const QString &p_text,
                  bool p_blockWise) Q_DECL_OVERRIDE;

  void indent(const KateViI::Range &p_range, int p_changes) Q_DECL_OVERRIDE;

  void pageDown(bool p_half) Q_DECL_OVERRIDE;

  void pageUp(bool p_half) Q_DECL_OVERRIDE;

  void scrollInPage(const KateViI::Cursor &p_pos, KateViI::PagePosition p_dest) Q_DECL_OVERRIDE;

  void align(const KateViI::Range &p_range) Q_DECL_OVERRIDE;

  void align() Q_DECL_OVERRIDE;

  void backspace() Q_DECL_OVERRIDE;

  void newLine(KateViI::NewLineIndent p_indent) Q_DECL_OVERRIDE;

  bool setCursorPosition(KateViI::Cursor p_position) Q_DECL_OVERRIDE;

  bool insertLine(int p_line, const QString &p_str) Q_DECL_OVERRIDE;

private:
  // The caret's cell as [first, last] character positions. False when the
  // caret is not in a cell at all, which every method answers by doing
  // nothing.
  bool cellRange(int &p_first, int &p_last) const;

  // The caret cell's text, empty when there is no cell.
  QString cellText() const;

  // Project a KateVi cursor onto a physical position, or -1 when there is no
  // cell.
  //
  // A line other than 0 does not exist in a one-line buffer, and katevi does
  // produce one: a LINEWISE range is spelled [ (line, 0), (line + 1, 0) ), so
  // `dd` on the only line asks for a range ending at line 1. Line > 0 is
  // therefore mapped to the END of the cell, which is what makes such a range
  // mean "the whole cell" instead of reaching into the next one.
  int positionOf(const KateViI::Cursor &p_cursor) const;

  // Give @p_cursor the caret cell's baseline character format, so text this
  // mode inserts does not inherit the highlight run to its left - the same
  // rule TablePreviewSheet::resetInsertionFormat() applies to typing.
  void applyBaselineFormat(QTextCursor &p_cursor) const;

  // Everything a mutation owes before it touches the document: collapse a cell
  // rectangle onto one cell, which is the frame-safety gate the sheet's own
  // keyPressEvent() applies and which katevi bypasses entirely.
  //
  // The UNDO checkpoint is deliberately not taken here - it belongs to
  // editStart(). See the comment there.
  //
  // Returns false when the sheet is read-only or has no cell.
  bool prepareMutation();

  // The sheet, which is also what the base holds as its VTextEdit *.
  TablePreviewSheet *m_sheet = nullptr;

  // Whether an undo checkpoint has already been taken since the current
  // TOP-LEVEL key press began. It is what tells "this command deleted and then
  // entered insert mode" apart from "an earlier command, and now a separate
  // insert session". See notifyKeyPressBegin() and setUndoMergeAllEdits().
  bool m_checkpointedThisKeyPress = false;
};
} // namespace vte

#endif // TABLEPREVIEWINPUTMODE_H
