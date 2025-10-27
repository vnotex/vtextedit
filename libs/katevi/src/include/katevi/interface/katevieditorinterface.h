#ifndef KATEVIEDITORINTERFACE_H
#define KATEVIEDITORINTERFACE_H

#include <functional>

#include <katevi/katevi_export.h>

#include <katevi/interface/markinterface.h>
#include <katevi/interface/range.h>

#include <QModelIndex>
#include <QTextCursor>

class QWidget;
class QMouseEvent;

namespace KateViI {
enum ViewMode {
  NormalModeInsert = 0,    /**< Insert mode. Characters will be added. */
  NormalModeOverwrite = 1, /**< Overwrite mode. Characters will be replaced. */

  ViModeNormal = 10,
  ViModeInsert = 11,
  ViModeVisual = 12,
  ViModeVisualLine = 13,
  ViModeVisualBlock = 14,
  ViModeReplace = 15
};

enum SearchOption {
  Default = 0, ///< Default settings

  // modes
  Regex = 1 << 1, ///< Treats the pattern as a regular expression

  // options for all modes
  CaseInsensitive = 1 << 4, ///< Ignores cases, e.g. "a" matches "A"
  Backwards = 1 << 5,       ///< Searches in backward direction

  // options for plaintext
  EscapeSequences = 1 << 10, ///< Plaintext mode: Processes escape sequences
  WholeWords = 1 << 11,      ///< Plaintext mode: Whole words only, e.g. @em not
                             ///< &quot;amp&quot; in &quot;example&quot;

  MaxSearchOption = 1 << 31 ///< Placeholder for binary compatibility
};

enum class PagePosition { Top, Center, Bottom };

enum NewLineIndent { Indent, NoIndent };

Q_DECLARE_FLAGS(SearchOptions, SearchOption)
Q_DECLARE_OPERATORS_FOR_FLAGS(SearchOptions)

class KATEVI_EXPORT KateViEditorInterface : public MarkInterface {
public:
  virtual ~KateViEditorInterface() {}

  virtual QTextCursor textCursor() const = 0;

  virtual void copyToClipboard(const QString &p_text) = 0;

  // Start an edit session.
  virtual void editStart() = 0;

  // Alias of editStart();
  void editBegin() { editStart(); }

  // End an edit session.
  virtual void editEnd() = 0;

  // Remove text specified in @p_range. If @p_blockWise is true, will remove a
  // text block on the basis of columns.
  virtual bool removeText(const KateViI::Range &p_range, bool p_blockWise = false) = 0;

  // Remove one line.
  virtual bool removeLine(int p_line) = 0;

  virtual void setSelection(const KateViI::Range &p_range) = 0;

  // Whether enable block-wise selection.
  virtual void setBlockSelection(bool p_enabled) = 0;

  // Length of the line text, not the block length.
  virtual int lineLength(int p_line) const = 0;

  // Return current line.
  virtual QString currentTextLine() const = 0;

  virtual QString line(int p_line) const = 0;

  virtual QStringList textLines(const KateViI::Range &p_range, bool p_blockWise = false) const = 0;

  virtual QString getText(const KateViI::Range &p_range, bool p_blockWise = false) const = 0;

  virtual KateViI::Cursor cursorPosition() const = 0;

  // Return number of lines in document.
  virtual int lines() const = 0;

  // Clear the selection without removing the selected text.
  virtual void removeSelection() = 0;

  virtual KateViI::Range selectionRange() const = 0;

  virtual QVector<KateViI::Range> searchText(const KateViI::Range &p_range,
                                             const QString &p_pattern,
                                             const KateViI::SearchOptions p_options) const = 0;

  virtual KateViI::Cursor documentEnd() const = 0;

  virtual KateViI::Range documentRange() const = 0;

  virtual void setUndoMergeAllEdits(bool p_merge) = 0;

  virtual bool isUndoMergeAllEditsEnabled() const = 0;

  virtual KateViI::ViewMode viewMode() const = 0;

  // Move cursor to previous char.
  virtual void cursorPrevChar(bool p_selection = false) = 0;

  virtual void update() = 0;

  // Transform @p_column with each tab expanded into @p_tabWidth characters.
  virtual int toVirtualColumn(int p_line, int p_column, int p_tabWidth) const = 0;

  // Transform @p_column to the real column, where each tab only counts as one
  // character.
  virtual int fromVirtualColumn(int p_line, int p_virtualColumn, int p_tabWidth) const = 0;

  // The last visible line number.
  virtual int endLine() const = 0;

  // The last line number.
  virtual int lastLine() const = 0;

  // Try to go @p_lines visual lines up (negative) or down (positive).
  // Do not move the cursor, just calculate the position.
  virtual KateViI::Cursor goVisualLineUpDownDry(int p_lines, bool &p_succeed) = 0;

  virtual QChar characterAt(const KateViI::Cursor &p_cursor) const = 0;

  // Turn @p_line to visible line regarding text folding.
  virtual int lineToVisibleLine(int p_line) const = 0;

  // Turn @p_line to line regarding text folding.
  virtual int visibleLineToLine(int p_line) const = 0;

  // Returns the position of the first non-whitespace character of @p_line.
  virtual int firstChar(int p_line) const = 0;

  virtual void joinLines(uint p_first, uint p_last, bool p_trimSpace) = 0;

  virtual int undoCount() const = 0;

  virtual void undo() = 0;

  virtual int redoCount() const = 0;

  virtual void redo() = 0;

  virtual void clearSelection() = 0;

  virtual bool replaceText(const KateViI::Range &p_range, const QString &p_text,
                           bool p_blockWise = false) = 0;

  // Whether a selection exists.
  virtual bool selection() const = 0;

  virtual bool insertText(const KateViI::Cursor &p_position, const QString &p_text,
                          bool p_blockWise = false) = 0;

  virtual void indent(const KateViI::Range &range, int p_changes) = 0;

  virtual void pageDown(bool p_half = false) = 0;

  virtual void pageUp(bool p_half = false) = 0;

  virtual void scrollInPage(const KateViI::Cursor &p_pos, KateViI::PagePosition p_dest) = 0;

  virtual void align(const KateViI::Range &p_range) = 0;

  // Align current line if there is no selection. Otherwise, align selection
  // range.
  virtual void align() = 0;

  virtual void clearOverriddenSelection() = 0;

  virtual void backspace() = 0;

  virtual void newLine(KateViI::NewLineIndent p_indent = KateViI::NewLineIndent::Indent) = 0;

  virtual void abortCompletion() = 0;

  virtual bool setCursorPosition(KateViI::Cursor p_position) = 0;

  virtual bool insertLine(int p_line, const QString &p_str) = 0;

  virtual void setOverwriteMode(bool p_enabled) = 0;

  virtual QWidget *focusProxy() const = 0;

  // Get the word at the text position \p cursor.
  virtual QString wordAt(const KateViI::Cursor &cursor) const = 0;

  virtual bool isReadOnly() const = 0;

  // Completion related interfaces.
public:
  virtual bool isCompletionActive() const = 0;

  virtual void completionNext(bool p_reversed) = 0;

  // When generating selection items, whether do it reversedly or not.
  virtual void userInvokedCompletion(bool p_reversed) = 0;

  virtual void completionExecute() = 0;

public:
  // NOTICE: please check if Vi is active in the slot since the connection
  // is not disconnected when Vi is deactivated.
  // Connect @p_slot to selectionChanged signal.
  virtual void connectSelectionChanged(std::function<void()> p_slot) = 0;

  // Connect @p_slot to textInserted signal.
  virtual void connectTextInserted(std::function<void(const KateViI::Range &)> p_slot) = 0;

  // Connect @p_slot to focusIn signal.
  virtual void connectFocusIn(std::function<void()> p_slot) = 0;

  // Connect @p_slot to focusOut signal.
  virtual void connectFocusOut(std::function<void()> p_slot) = 0;

  // Connect @p_slot to mouseReleased signal.
  virtual void connectMouseReleased(std::function<void(QMouseEvent *)> p_slot) = 0;

  // Notifiers.
  virtual void notifyViewModeChanged(KateViI::ViewMode p_mode) = 0;
};
} // namespace KateViI

#endif // KATEVIEDITORINTERFACE_H
