#include "tablepreviewinputmode.h"

#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextTable>
#include <QTextTableCell>

#include <vtextedit/vtextedit.h>

#include "previewlogging.h"
#include "tablepreviewwidget.h"

using namespace vte;

TablePreviewInputMode::TablePreviewInputMode(TablePreviewSheet *p_sheet)
    : TextEditInputMode(p_sheet), m_sheet(p_sheet) {
  Q_ASSERT(m_sheet);
}

// ---------------------------------------------------------------------------
// The projection
// ---------------------------------------------------------------------------

bool TablePreviewInputMode::cellRange(int &p_first, int &p_last) const {
  return m_sheet && m_sheet->currentCellRange(p_first, p_last);
}

QString TablePreviewInputMode::cellText() const {
  int first = 0;
  int last = 0;
  if (!cellRange(first, last)) {
    return QString();
  }

  QTextCursor cursor = m_sheet->textCursor();
  cursor.setPosition(first);
  cursor.setPosition(last, QTextCursor::KeepAnchor);
  return cursor.selectedText();
}

int TablePreviewInputMode::positionOf(const KateViI::Cursor &p_cursor) const {
  int first = 0;
  int last = 0;
  if (!cellRange(first, last)) {
    return -1;
  }

  if (!p_cursor.isValid() || p_cursor.line() < 0) {
    return first;
  }

  if (p_cursor.line() > 0) {
    return last;
  }

  return qBound(first, first + p_cursor.column(), last);
}

void TablePreviewInputMode::applyBaselineFormat(QTextCursor &p_cursor) const {
  auto document = m_sheet ? m_sheet->tableDocument() : nullptr;
  QTextTable *table = document ? document->table() : nullptr;
  if (!table) {
    return;
  }

  const QTextTableCell cell = table->cellAt(p_cursor.position());
  if (!cell.isValid()) {
    return;
  }

  p_cursor.setCharFormat(document->baselineCellFormat(cell.row()));
}

void TablePreviewInputMode::notifyKeyPressBegin() {
  // Only a TOP-LEVEL key press starts a new logical command.
  //
  // A `.` repeat, a macro replay and a mapping expansion all open an outer
  // edit session and then feed synthetic QKeyEvents back through this very
  // widget with QApplication::sendEvent() (KateVi::InputModeManager::
  // feedKeyPresses, MacroRecorder, KeyMapper), which re-enters the sheet's
  // keyPressEvent(). Those are not new commands: resetting here would erase
  // the checkpoint the OUTER command already took, and a replayed `C` - which
  // deletes before it enters insert mode - would then checkpoint the emptied
  // cell and leave `u` on it.
  //
  // An open edit session is exactly what distinguishes them: every one of
  // those replay paths opens one before it feeds anything.
  if (m_editSessionCount > 0) {
    return;
  }

  m_checkpointedThisKeyPress = false;
}

void TablePreviewInputMode::setUndoMergeAllEdits(bool p_merge) {
  // The OTHER undo step boundary. Vi turns this on for the whole of an insert
  // or replace session and off again on the way back to normal mode
  // (libs/katevi/src/modes/modebase.cpp). The false -> true edge is therefore
  // where an insert session begins - but whether it is also where the COMMAND
  // began depends on the command, and katevi is not consistent about it:
  //
  //  - `cc` and `o`/`O` open the window FIRST and delete afterwards
  //    (NormalViMode::commandChangeLine, commandOpenNewLineUnder/Over).
  //  - `C` deletes FIRST and only then enters insert mode, so editStart() has
  //    already checkpointed by the time this runs.
  //
  // The discriminator is the KEY PRESS, not the call order: a checkpoint is
  // taken here only if none has been taken since the current key press began.
  // That makes both shapes one step - `u` after `C` restores the text rather
  // than the empty cell it was changed from - while `x` followed by a separate
  // `i...<Esc>` stays the two steps vim gives it, because the second key press
  // resets the flag.
  //
  // editStart() suppresses itself for the duration of the window, because an
  // insert session is not an open edit SESSION: every key in it gets its own
  // editStart()/editEnd() pair (src/inputmode/viinputmode.cpp), so the session
  // count is back to 0 between characters and checkpointing there would make
  // `u` undo one character.
  if (p_merge && !isUndoMergeAllEditsEnabled() && !m_checkpointedThisKeyPress && m_sheet &&
      !m_sheet->isReadOnly()) {
    m_sheet->collapseComplexSelectionForMutation();
    m_sheet->commitUndoCheckpoint();
    m_checkpointedThisKeyPress = true;
  }

  TextEditInputMode::setUndoMergeAllEdits(p_merge);
}

void TablePreviewInputMode::editStart() {
  // THE undo step boundary for everything outside an insert session. Every
  // mutating override below wraps its edit in editStart()/editEnd(), and
  // katevi wraps a compound command in an outer session of its own - a visual
  // put is editStart(), removeText(), insertText(), editEnd(). Checkpointing
  // per mutating CALL would record the intermediate "selection deleted,
  // replacement not yet inserted" state as a step of its own, and one `u`
  // would leave the cell mangled. Checkpointing on the OUTERMOST session start
  // is D2's "one operator is one step".
  //
  // Suppressed while merge mode is on: that window is one step of its own.
  // See setUndoMergeAllEdits().
  if (m_editSessionCount == 0 && !isUndoMergeAllEditsEnabled() && m_sheet &&
      !m_sheet->isReadOnly()) {
    m_sheet->collapseComplexSelectionForMutation();
    m_sheet->commitUndoCheckpoint();
    m_checkpointedThisKeyPress = true;
  }

  TextEditInputMode::editStart();
}

bool TablePreviewInputMode::prepareMutation() {
  if (!m_sheet || m_sheet->isReadOnly()) {
    return false;
  }

  // Frame safety only. The undo checkpoint belongs to editStart(), which every
  // mutating override goes through and which katevi also opens around a
  // compound command.
  m_sheet->collapseComplexSelectionForMutation();

  int first = 0;
  int last = 0;
  return cellRange(first, last);
}

// ---------------------------------------------------------------------------
// The one-line buffer
// ---------------------------------------------------------------------------

int TablePreviewInputMode::lines() const { return 1; }

int TablePreviewInputMode::endLine() const { return 0; }

int TablePreviewInputMode::linesDisplayed() {
  // The whole buffer is on screen, always: the sheet renders at its full
  // natural height and never scrolls. The base would answer with the number of
  // visible blocks of the WHOLE SHEET, which is what H, M, L, Ctrl+F and
  // Ctrl+D measure themselves against - and every one of those would then
  // address a line outside the cell.
  return 1;
}

QString TablePreviewInputMode::line(int p_line) const {
  return p_line == 0 ? cellText() : QString();
}

QString TablePreviewInputMode::currentTextLine() const { return cellText(); }

int TablePreviewInputMode::lineLength(int p_line) const {
  return p_line == 0 ? cellText().length() : 0;
}

KateViI::Cursor TablePreviewInputMode::documentEnd() const {
  return KateViI::Cursor(0, cellText().length());
}

KateViI::Cursor TablePreviewInputMode::cursorPosition() const {
  int first = 0;
  int last = 0;
  if (!cellRange(first, last)) {
    return KateViI::Cursor(0, 0);
  }

  return KateViI::Cursor(0, qBound(first, m_sheet->textCursor().position(), last) - first);
}

QChar TablePreviewInputMode::characterAt(const KateViI::Cursor &p_cursor) const {
  if (p_cursor.line() != 0) {
    return QChar();
  }

  const QString text = cellText();
  if (p_cursor.column() < 0 || p_cursor.column() >= text.length()) {
    return QChar();
  }

  return text.at(p_cursor.column());
}

int TablePreviewInputMode::toVirtualColumn(int p_line, int p_column, int p_tabWidth) const {
  if (p_line != 0 || p_column <= 0) {
    return 0;
  }

  const QString text = cellText();
  int x = 0;
  const int zmax = qMin(p_column, text.length());
  for (int z = 0; z < zmax; ++z) {
    if (text.at(z) == QLatin1Char('\t')) {
      x += p_tabWidth - (x % p_tabWidth);
    } else {
      ++x;
    }
  }

  return x + p_column - zmax;
}

int TablePreviewInputMode::fromVirtualColumn(int p_line, int p_virtualColumn,
                                             int p_tabWidth) const {
  if (p_line != 0 || p_virtualColumn <= 0) {
    return 0;
  }

  const QString text = cellText();
  const int zmax = qMin(text.length(), p_virtualColumn);
  int x = 0;
  int z = 0;
  for (; z < zmax; ++z) {
    int diff = 1;
    if (text.at(z) == QLatin1Char('\t')) {
      diff = p_tabWidth - (x % p_tabWidth);
    }

    if (x + diff > p_virtualColumn) {
      break;
    }
    x += diff;
  }

  return z + qMax(p_virtualColumn - x, 0);
}

KateViI::Cursor TablePreviewInputMode::goVisualLineUpDownDry(int p_lines, bool &p_succeed) {
  if (p_lines == 0) {
    p_succeed = true;
    return cursorPosition();
  }

  // There is exactly one visual line, so j and k have nowhere to go. Reporting
  // failure is what makes them no-ops instead of walking the sheet's blocks -
  // the base answers with QTextCursor::Up/Down, which crosses cells.
  p_succeed = false;
  return cursorPosition();
}

// ---------------------------------------------------------------------------
// The caret
// ---------------------------------------------------------------------------

void TablePreviewInputMode::updateCursor(int p_line, int p_column) {
  Q_UNUSED(p_line);

  int first = 0;
  int last = 0;
  if (!cellRange(first, last)) {
    return;
  }

  // The base resolves p_line through findBlockByNumber(), which for the only
  // line this buffer has would be the FIRST BLOCK OF THE SHEET - the top-left
  // cell, not the caret's. This is the single most dangerous inherited method:
  // every katevi motion ends here.
  QTextCursor cursor = m_sheet->textCursor();
  cursor.setPosition(qBound(first, first + qMax(0, p_column), last));
  m_sheet->setTextCursor(cursor);
}

bool TablePreviewInputMode::setCursorPosition(KateViI::Cursor p_position) {
  int first = 0;
  int last = 0;
  if (!cellRange(first, last)) {
    return false;
  }

  updateCursor(0, p_position.line() > 0 ? last - first : p_position.column());
  return true;
}

void TablePreviewInputMode::cursorPrevChar(bool p_selection) {
  int first = 0;
  int last = 0;
  if (!cellRange(first, last)) {
    return;
  }

  QTextCursor cursor = m_sheet->textCursor();
  if (cursor.position() <= first) {
    // At the cell's start there is no previous character. The base moves one
    // PreviousCharacter unconditionally, which at a cell boundary steps into
    // the previous cell - and this runs while an insertion is being ended
    // (KateVi::InputModeManager), i.e. on every Escape out of insert mode.
    return;
  }

  cursor.setPosition(cursor.position() - 1,
                     p_selection ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor);
  m_sheet->setTextCursor(cursor);
}

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------

void TablePreviewInputMode::setSelection(const KateViI::Range &p_range) {
  if (!p_range.isValid()) {
    clearSelection();
    return;
  }

  int first = 0;
  int last = 0;
  if (!cellRange(first, last)) {
    return;
  }

  const int startPos = positionOf(p_range.start());
  const int endPos = positionOf(p_range.end());
  if (startPos < 0 || endPos < 0) {
    return;
  }

  // Vi's visual selection is INCLUSIVE of the character under the caret, so
  // the published range runs one past it while the QTextCursor selection must
  // not - keeping the caret where katevi believes it is. Same shape as the
  // base, with the multi-line arm dropped: every range here is on one line.
  QTextCursor cursor = m_sheet->textCursor();
  const int pos = cursor.position();
  if (pos == endPos - 1) {
    if (pos != startPos) {
      cursor.setPosition(startPos);
      cursor.setPosition(endPos - 1, QTextCursor::KeepAnchor);
      m_sheet->setTextCursor(cursor);
    }
  } else if (pos == startPos) {
    cursor.setPosition(endPos);
    cursor.setPosition(startPos, QTextCursor::KeepAnchor);
    m_sheet->setTextCursor(cursor);
  } else {
    cursor.setPosition(startPos);
    cursor.setPosition(endPos, QTextCursor::KeepAnchor);
    m_sheet->setTextCursor(cursor);
  }

  // m_textEdit is typed as VTextEdit *, so this reaches the base implementation
  // rather than the sheet's refusing shadow - which is exactly right: the
  // shadow exists to stop an EXTERNAL caller publishing arbitrary physical
  // positions, and these two have just been confined to the cell.
  m_textEdit->setOverriddenSelection(startPos, endPos);
}

void TablePreviewInputMode::setSelection(int p_startLine, int p_startColumn, int p_endLine,
                                         int p_endColumn) {
  Q_UNUSED(p_startLine);
  Q_UNUSED(p_endLine);

  int first = 0;
  int last = 0;
  if (!cellRange(first, last)) {
    return;
  }

  clearSelection();

  QTextCursor cursor = m_sheet->textCursor();
  cursor.setPosition(qBound(first, first + qMax(0, p_startColumn), last));
  cursor.setPosition(qBound(first, first + qMax(0, p_endColumn), last), QTextCursor::KeepAnchor);
  m_sheet->setTextCursor(cursor);
}

KateViI::Range TablePreviewInputMode::selectionRange() const {
  int first = 0;
  int last = 0;
  if (!cellRange(first, last) || !m_sheet->hasSelection()) {
    return KateViI::Range::invalid();
  }

  const auto &selection = m_sheet->getSelection();
  return KateViI::Range(KateViI::Cursor(0, qBound(first, selection.start(), last) - first),
                        KateViI::Cursor(0, qBound(first, selection.end(), last) - first));
}

void TablePreviewInputMode::setBlockSelection(bool p_enabled) {
  // Visual BLOCK mode asks for this, and the base answers with
  // Q_ASSERT(!p_enabled) - a debug-build crash on Ctrl+V inside a cell.
  //
  // Refused gracefully instead of supported: a block selection is a rectangle
  // over several lines, and this buffer has exactly one. Ctrl+V therefore
  // behaves as an ordinary character-wise visual mode, which over a single
  // line is the same selection anyway.
  if (p_enabled) {
    qCDebug(previewTableLog) << "the table sheet refused a block selection - a cell is one line";
  }
}

// ---------------------------------------------------------------------------
// Mutation
// ---------------------------------------------------------------------------

bool TablePreviewInputMode::removeText(const KateViI::Range &p_range, bool p_blockWise) {
  if (p_blockWise) {
    return false;
  }

  if (!prepareMutation()) {
    return false;
  }

  const int startPos = positionOf(p_range.start());
  const int endPos = positionOf(p_range.end());
  if (startPos < 0 || endPos <= startPos) {
    return false;
  }

  editStart();
  QTextCursor cursor = m_sheet->textCursor();
  cursor.setPosition(startPos);
  cursor.setPosition(endPos, QTextCursor::KeepAnchor);
  cursor.removeSelectedText();
  m_sheet->setTextCursor(cursor);
  editEnd();
  return true;
}

bool TablePreviewInputMode::removeLine(int p_line) {
  if (p_line != 0) {
    return false;
  }

  if (!prepareMutation()) {
    return false;
  }

  int first = 0;
  int last = 0;
  if (!cellRange(first, last) || first == last) {
    // Already empty. Reported as done so `dd` on an empty cell is a no-op
    // rather than a failure katevi answers by trying something else.
    return true;
  }

  // The cell's TEXT is cleared; the block itself never goes. Removing it would
  // take the QTextTableCell's only block with it, which QTextTable does not
  // survive - a cell is one block by construction here.
  editStart();
  QTextCursor cursor = m_sheet->textCursor();
  cursor.setPosition(first);
  cursor.setPosition(last, QTextCursor::KeepAnchor);
  cursor.removeSelectedText();
  m_sheet->setTextCursor(cursor);
  editEnd();
  return true;
}

bool TablePreviewInputMode::replaceText(const KateViI::Range &p_range, const QString &p_text,
                                        bool p_blockWise) {
  if (p_blockWise) {
    return false;
  }

  if (!prepareMutation()) {
    return false;
  }

  const int startPos = positionOf(p_range.start());
  const int endPos = positionOf(p_range.end());
  if (startPos < 0 || endPos <= startPos) {
    return false;
  }

  const QString text = TablePreviewSheet::sanitizeCellPayload(p_text);

  editStart();
  QTextCursor cursor = m_sheet->textCursor();
  cursor.setPosition(startPos);
  cursor.setPosition(endPos, QTextCursor::KeepAnchor);
  applyBaselineFormat(cursor);
  cursor.insertText(text);
  m_sheet->setTextCursor(cursor);
  editEnd();
  return true;
}

bool TablePreviewInputMode::insertText(const KateViI::Cursor &p_position, const QString &p_text,
                                       bool p_blockWise) {
  if (p_blockWise) {
    return false;
  }

  if (!prepareMutation()) {
    return false;
  }

  // A register can hold anything a yank in the EDITOR put there, including a
  // multi-line one - and the shared GlobalState makes that the normal case, not
  // an exotic one. The same policy a paste and an input method commit go
  // through: separators collapse, a payload which is nothing but separators is
  // refused outright.
  const QString text = TablePreviewSheet::sanitizeCellPayload(p_text);
  if (text.isEmpty()) {
    return !p_text.isEmpty();
  }

  const int position = positionOf(p_position);
  if (position < 0) {
    return false;
  }

  editStart();
  QTextCursor cursor = m_sheet->textCursor();
  cursor.setPosition(position);
  applyBaselineFormat(cursor);
  cursor.insertText(text);
  m_sheet->setTextCursor(cursor);
  editEnd();
  return true;
}

void TablePreviewInputMode::backspace() {
  if (!prepareMutation()) {
    return;
  }

  int first = 0;
  int last = 0;
  if (!cellRange(first, last)) {
    return;
  }

  QTextCursor cursor = m_sheet->textCursor();
  if (cursor.position() <= first) {
    // Never across the boundary: deletePreviousChar() at a cell's start
    // deletes the block separator, which is the cell itself.
    return;
  }

  editStart();
  cursor.deletePreviousChar();
  m_sheet->setTextCursor(cursor);
  editEnd();
}

bool TablePreviewInputMode::insertLine(int p_line, const QString &p_str) {
  Q_UNUSED(p_line);
  Q_UNUSED(p_str);
  // o, O and a linewise put have nowhere to put a line: a row is one source
  // line and the serializer rejects every separator that could end one. The
  // affordance for "one more row" is Enter in the last cell.
  return false;
}

void TablePreviewInputMode::newLine(KateViI::NewLineIndent p_indent) { Q_UNUSED(p_indent); }

void TablePreviewInputMode::joinLines(uint p_first, uint p_last, bool p_trimSpace) {
  Q_UNUSED(p_first);
  Q_UNUSED(p_last);
  Q_UNUSED(p_trimSpace);
  // J has nothing to join: there is one line.
}

void TablePreviewInputMode::indent(const KateViI::Range &p_range, int p_changes) {
  Q_UNUSED(p_range);
  Q_UNUSED(p_changes);
  // >> and << would prepend indentation to a cell, which is not indentation at
  // all once the row is written back - it is leading whitespace inside a table
  // cell, which the serializer trims.
}

void TablePreviewInputMode::align(const KateViI::Range &p_range) { Q_UNUSED(p_range); }

void TablePreviewInputMode::align() {}

// ---------------------------------------------------------------------------
// Scrolling: the sheet has none
// ---------------------------------------------------------------------------

void TablePreviewInputMode::scrollUp() {}

void TablePreviewInputMode::scrollDown() {}

void TablePreviewInputMode::pageUp(bool p_half) { Q_UNUSED(p_half); }

void TablePreviewInputMode::pageDown(bool p_half) { Q_UNUSED(p_half); }

void TablePreviewInputMode::scrollInPage(const KateViI::Cursor &p_pos,
                                         KateViI::PagePosition p_dest) {
  Q_UNUSED(p_pos);
  Q_UNUSED(p_dest);
}

// ---------------------------------------------------------------------------
// Text extraction
// ---------------------------------------------------------------------------

QStringList TablePreviewInputMode::textLines(const KateViI::Range &p_range,
                                             bool p_blockWise) const {
  QStringList result;
  if (p_blockWise || !p_range.isValid()) {
    return result;
  }

  result << getText(p_range, false);
  return result;
}

QString TablePreviewInputMode::getText(const KateViI::Range &p_range, bool p_blockWise) const {
  if (p_blockWise || !p_range.isValid()) {
    return QString();
  }

  int first = 0;
  int last = 0;
  if (!cellRange(first, last)) {
    return QString();
  }

  const int startPos = positionOf(p_range.start());
  const int endPos = positionOf(p_range.end());
  if (startPos < 0 || endPos <= startPos) {
    return QString();
  }

  return cellText().mid(startPos - first, endPos - startPos);
}

// ---------------------------------------------------------------------------
// Undo (decision D2)
// ---------------------------------------------------------------------------

int TablePreviewInputMode::undoCount() const { return m_sheet ? m_sheet->undoRingDepth() : 0; }

int TablePreviewInputMode::redoCount() const { return m_sheet ? m_sheet->redoRingDepth() : 0; }

void TablePreviewInputMode::undo() {
  if (m_sheet) {
    // The sheet's cell-text ring, never QTextDocument::undo(): the inner
    // document's stack is disabled precisely because it also records the
    // syntax-format overlays and the structural mutations whose C++ metadata
    // does not travel with them.
    m_sheet->undoFromRing();
  }
}

void TablePreviewInputMode::redo() {
  if (m_sheet) {
    m_sheet->redoFromRing();
  }
}

// ---------------------------------------------------------------------------
// Notifications
// ---------------------------------------------------------------------------

void TablePreviewInputMode::notifyEditorModeChanged(EditorMode p_mode) {
  TextEditInputMode::notifyEditorModeChanged(p_mode);

  if (m_sheet) {
    // The whole point of the feature: input method enablement inside a
    // previewed table follows the SHEET's mode, exactly as it follows the
    // editor's mode outside one.
    m_sheet->syncInputMethodToMode();
  }
}

void TablePreviewInputMode::connectTextInserted(
    std::function<void(const KateViI::Range &)> p_slot) {
  // The base reports PHYSICAL block numbers, which katevi feeds straight into
  // insert tracking and into the registers a repeat (`.`) replays from. Inside
  // a sheet those are block numbers of the whole table, so a repeat would
  // address a cell other than the one typed in.
  connect(m_sheet->document(), &QTextDocument::contentsChange, this,
          [this, p_slot](int p_position, int p_charsRemoved, int p_charsAdded) {
            Q_UNUSED(p_charsRemoved);
            if (p_charsAdded <= 0) {
              return;
            }

            int first = 0;
            int last = 0;
            if (!cellRange(first, last)) {
              return;
            }

            // Only an insertion inside the caret's own cell is this buffer's.
            const int start = p_position;
            const int end = p_position + p_charsAdded;
            if (start < first || end > last) {
              return;
            }

            p_slot(
                KateViI::Range(KateViI::Cursor(0, start - first), KateViI::Cursor(0, end - first)));
          });
}
