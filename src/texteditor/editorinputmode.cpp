#include "editorinputmode.h"

#include <QApplication>
#include <QClipboard>
#include <QDebug>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>

#include <inputmode/abstractinputmode.h>
#include <vtextedit/texteditutils.h>
#include <vtextedit/textutils.h>
#include <vtextedit/vtextedit.h>
#include <vtextedit/vtexteditor.h>

#include "textfolding.h"
#include <textedit/autoindenthelper.h>

#define EDITOR_NIY                                                                                 \
  do {                                                                                             \
    qDebug() << __func__ << ": not implemented yet";                                               \
  } while (0)

using namespace vte;

EditorInputMode::EditorInputMode(VTextEditor *p_editor)
    : m_editor(p_editor), m_textEdit(m_editor->getTextEdit()) {}

QTextCursor EditorInputMode::textCursor() const { return m_textEdit->textCursor(); }

void EditorInputMode::setCaretStyle(CaretStyle p_style) {
  const bool asBlock = p_style == CaretStyle::Block || p_style == CaretStyle::Half;
  m_textEdit->setDrawCursorAsBlock(asBlock, p_style == CaretStyle::Half);
}

void EditorInputMode::clearSelection() {
  auto cursor = textCursor();
  if (cursor.hasSelection()) {
    cursor.clearSelection();
    m_textEdit->setTextCursor(cursor);
  }

  m_textEdit->clearOverriddenSelection();
}

void EditorInputMode::notifyEditorModeChanged(EditorMode p_mode) {
  if (m_mode != p_mode) {
    m_mode = p_mode;

    emit m_editor->modeChanged();
  }
}

void EditorInputMode::copyToClipboard(const QString &p_text) {
  auto clipboard = QApplication::clipboard();
  clipboard->setText(p_text, QClipboard::Clipboard);
}

// FIXME: Qt Bug: if we insert a new block within edit session, Qt may set
// the vertical scrollbar to 0.
// We do a hack here to restore the vertical scrollbar if in need.
void EditorInputMode::editStart() {
  if (m_editSessionCount == 0) {
    auto vbar = m_textEdit->verticalScrollBar();
    m_verticalScrollBarValue = vbar ? vbar->value() : 0;
  }
  ++m_editSessionCount;
  if (m_mergeAllEditsEnabled && !m_firstEditSessionInMerge) {
    textCursor().joinPreviousEditBlock();
  } else {
    m_firstEditSessionInMerge = false;
    textCursor().beginEditBlock();
  }
}

void EditorInputMode::editEnd() {
  --m_editSessionCount;
  Q_ASSERT(m_editSessionCount >= 0);
  textCursor().endEditBlock();

  if (m_editSessionCount == 0) {
    auto vbar = m_textEdit->verticalScrollBar();
    if (vbar && vbar->value() == 0 && m_verticalScrollBarValue != 0) {
      vbar->setValue(m_verticalScrollBarValue);
      m_textEdit->ensureCursorVisible();
    }
  }
}

bool EditorInputMode::removeLine(int p_line) {
  auto block = document()->findBlockByNumber(p_line);
  if (block.isValid()) {
    TextEditUtils::removeBlock(block);

    // Move it to the right block after removal.
    auto cursor = textCursor();
    // Will be one line above.
    if (cursor.blockNumber() < p_line) {
      cursor.movePosition(QTextCursor::NextBlock);
      m_textEdit->setTextCursor(cursor);
    }
    return true;
  }

  return false;
}

void EditorInputMode::connectSelectionChanged(std::function<void()> p_slot) {
  connect(m_textEdit, &QTextEdit::selectionChanged, this, p_slot);
}

void EditorInputMode::connectTextInserted(std::function<void(const KateViI::Range &)> p_slot) {
  connect(document(), &QTextDocument::contentsChange, this,
          [this, p_slot](int p_position, int p_charsRemoved, int p_charsAdded) {
            if (p_charsAdded > 0) {
              KateViI::Range range(toKateViCursor(p_position),
                                   toKateViCursor(p_position + p_charsAdded));
              p_slot(range);
            }
          });
}

void EditorInputMode::setSelection(int p_startLine, int p_startColumn, int p_endLine,
                                   int p_endColumn) {
  clearSelection();
  auto cursor = textCursor();
  auto startBlock = document()->findBlockByNumber(p_startLine);
  if (startBlock.isValid()) {
    cursor.setPosition(startBlock.position() + p_startColumn);
    auto endBlock = document()->findBlockByNumber(p_endLine);
    if (endBlock.isValid()) {
      cursor.setPosition(endBlock.position() + p_endColumn, QTextCursor::KeepAnchor);
    } else {
      cursor.setPosition(startBlock.position() + startBlock.length(), QTextCursor::KeepAnchor);
    }
  }
  m_textEdit->setTextCursor(cursor);
}

void EditorInputMode::setSelection(const KateViI::Range &p_range) {
  if (!p_range.isValid()) {
    clearSelection();
    return;
  }

  // For KateVi, we need to make the selection without changing the cursor
  // position.
  auto cursor = textCursor();
  const int pos = cursor.position();
  int startPos = kateViCursorToPosition(p_range.start());
  int endPos = kateViCursorToPosition(p_range.end());

  qDebug() << "setSelection" << pos << startPos << endPos << p_range;
  if (pos == endPos - 1) {
    if (pos != startPos) {
      cursor.setPosition(startPos);
      cursor.setPosition(endPos - 1, QTextCursor::KeepAnchor);
      m_textEdit->setTextCursor(cursor);
    }
  } else if (startPos == pos) {
    cursor.setPosition(endPos);
    cursor.setPosition(startPos, QTextCursor::KeepAnchor);
    m_textEdit->setTextCursor(cursor);
  } else {
    // Need to check which end does the cursor locate at.
    int cline = cursor.block().blockNumber();
    if (p_range.onSingleLine() || cline == p_range.end().line()) {
      cursor.setPosition(startPos);
      cursor.setPosition(endPos, QTextCursor::KeepAnchor);
    } else {
      cursor.setPosition(endPos);
      cursor.setPosition(startPos, QTextCursor::KeepAnchor);
    }
    m_textEdit->setTextCursor(cursor);
  }

  // Set the overridden selection.
  m_textEdit->setOverriddenSelection(startPos, endPos);
}

void EditorInputMode::setBlockSelection(bool p_enabled) {
  EDITOR_NIY;
  Q_ASSERT(!p_enabled);
}

int EditorInputMode::lineLength(int p_line) const {
  auto block = document()->findBlockByNumber(p_line);
  return block.length() - 1;
}

KateViI::Cursor EditorInputMode::cursorPosition() const {
  auto cursor = textCursor();
  return KateViI::Cursor(cursor.block().blockNumber(), cursor.positionInBlock());
}

int EditorInputMode::lines() const { return document()->blockCount(); }

void EditorInputMode::removeSelection() { clearSelection(); }

KateViI::Range EditorInputMode::selectionRange() const {
  if (m_textEdit->hasSelection()) {
    const auto &selection = m_textEdit->getSelection();
    return KateViI::Range(toKateViCursor(selection.start()), toKateViCursor(selection.end()));
  }

  return KateViI::Range::invalid();
}

QVector<KateViI::Range> EditorInputMode::searchText(const KateViI::Range &p_range,
                                                    const QString &p_pattern,
                                                    const KateViI::SearchOptions p_options) const {
  EDITOR_NIY;
  return QVector<KateViI::Range>();
}

KateViI::Cursor EditorInputMode::documentEnd() const {
  auto block = document()->lastBlock();
  return KateViI::Cursor(block.blockNumber(), block.length() - 1);
}

KateViI::Range EditorInputMode::documentRange() const {
  return KateViI::Range(KateViI::Cursor::start(), documentEnd());
}

void EditorInputMode::setUndoMergeAllEdits(bool p_merge) {
  if (m_mergeAllEditsEnabled != p_merge) {
    m_mergeAllEditsEnabled = p_merge;
    if (m_mergeAllEditsEnabled) {
      m_firstEditSessionInMerge = true;
    }
  }
}

bool EditorInputMode::isUndoMergeAllEditsEnabled() const { return m_mergeAllEditsEnabled; }

void EditorInputMode::notifyViewModeChanged(KateViI::ViewMode p_mode) {
  notifyEditorModeChanged(static_cast<EditorMode>(p_mode));
}

KateViI::ViewMode EditorInputMode::viewMode() const {
  return static_cast<KateViI::ViewMode>(m_editor->getInputMode()->editorMode());
}

void EditorInputMode::cursorPrevChar(bool p_selection) {
  auto cursor = textCursor();
  cursor.movePosition(QTextCursor::PreviousCharacter,
                      p_selection ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor);
  m_textEdit->setTextCursor(cursor);
}

void EditorInputMode::update() { m_textEdit->update(); }

QString EditorInputMode::currentTextLine() const {
  auto cursor = textCursor();
  return cursor.block().text();
}

QString EditorInputMode::line(int p_line) const {
  auto block = document()->findBlockByNumber(p_line);
  return block.text();
}

QTextDocument *EditorInputMode::document() const { return m_textEdit->document(); }

void EditorInputMode::updateCursor(int p_line, int p_column) {
  auto block = document()->findBlockByNumber(p_line);
  Q_ASSERT(block.isValid() && block.isVisible());
  if (block.isValid()) {
    if (p_column >= block.length()) {
      p_column = block.length() - 1;
    }
    auto cursor = m_textEdit->textCursor();
    cursor.setPosition(block.position() + p_column);
    m_textEdit->setTextCursor(cursor);
  }
}

int EditorInputMode::toVirtualColumn(int p_line, int p_column, int p_tabWidth) const {
  if (p_column < 0) {
    return 0;
  }

  auto block = document()->findBlockByNumber(p_line);
  if (!block.isValid()) {
    return 0;
  }
  const auto text = block.text();

  int x = 0;
  const int zmax = qMin(p_column, text.length());
  const QChar *unicode = text.unicode();
  for (int z = 0; z < zmax; ++z) {
    if (unicode[z] == QLatin1Char('\t')) {
      x += p_tabWidth - (x % p_tabWidth);
    } else {
      x++;
    }
  }

  return x + p_column - zmax;
}

int EditorInputMode::fromVirtualColumn(int p_line, int p_virtualColumn, int p_tabWidth) const {
  if (p_virtualColumn < 0) {
    return 0;
  }

  auto block = document()->findBlockByNumber(p_line);
  if (!block.isValid()) {
    return 0;
  }
  const auto text = block.text();

  const int zmax = qMin(text.length(), p_virtualColumn);
  const QChar *unicode = text.unicode();
  int x = 0;
  int z = 0;
  for (; z < zmax; ++z) {
    int diff = 1;
    if (unicode[z] == QLatin1Char('\t')) {
      diff = p_tabWidth - (x % p_tabWidth);
    }

    if (x + diff > p_virtualColumn) {
      break;
    }
    x += diff;
  }

  return z + qMax(p_virtualColumn - x, 0);
}

int EditorInputMode::endLine() const {
  return TextEditUtils::lastVisibleBlock(m_textEdit).blockNumber();
}

int EditorInputMode::linesDisplayed() {
  auto fb = TextEditUtils::firstVisibleBlock(m_textEdit);
  auto lb = TextEditUtils::lastVisibleBlock(m_textEdit);
  return lb.blockNumber() - fb.blockNumber() + 1;
}

KateViI::Cursor EditorInputMode::goVisualLineUpDownDry(int p_lines, bool &p_succeed) {
  p_succeed = true;
  if (p_lines == 0) {
    return cursorPosition();
  }

  auto cursor = textCursor();
  p_succeed = cursor.movePosition(p_lines > 0 ? QTextCursor::Down : QTextCursor::Up,
                                  QTextCursor::MoveAnchor, qAbs(p_lines));
  return toKateViCursor(cursor);
}

KateViI::Cursor EditorInputMode::toKateViCursor(const QTextCursor &p_cursor) const {
  return KateViI::Cursor(p_cursor.block().blockNumber(), p_cursor.positionInBlock());
}

KateViI::Cursor EditorInputMode::toKateViCursor(int p_position) const {
  auto block = document()->findBlock(p_position);
  if (!block.isValid()) {
    return KateViI::Cursor::invalid();
  }

  return KateViI::Cursor(block.blockNumber(), p_position - block.position());
}

QChar EditorInputMode::characterAt(const KateViI::Cursor &p_cursor) const {
  auto block = document()->findBlockByNumber(p_cursor.line());
  if (block.isValid()) {
    if (p_cursor.column() < block.length() - 1) {
      return block.text().at(p_cursor.column());
    }
  }

  return QChar();
}

int EditorInputMode::lineToVisibleLine(int p_line) const {
  return m_editor->getTextFolding()->lineToVisibleLine(p_line);
}

int EditorInputMode::visibleLineToLine(int p_line) const {
  return m_editor->getTextFolding()->visibleLineToLine(p_line);
}

bool EditorInputMode::removeText(const KateViI::Range &p_range, bool p_blockWise) {
  if (m_editor->isReadOnly()) {
    return false;
  }

  auto doc = document();
  const int lastLine = doc->blockCount() - 1;
  if (p_range.start().line() > lastLine) {
    return false;
  }

  editStart();

  if (!p_blockWise) {
    auto cursor = kateViRangeToTextCursor(p_range);
    if (cursor.hasSelection()) {
      cursor.removeSelectedText();
      m_textEdit->setTextCursor(cursor);
    }
  } else {
    EDITOR_NIY;
  }

  editEnd();
  return true;
}

int EditorInputMode::kateViCursorToPosition(const KateViI::Cursor &p_cursor) const {
  if (!p_cursor.isValid()) {
    return -1;
  }

  auto doc = document();
  auto block = doc->findBlockByNumber(p_cursor.line());
  if (block.isValid()) {
    // TODO: Do we need to minus 1 here? If minus 1, setSelection() will break
    // since we may select the end of the block.
    int col = qMin(block.length(), p_cursor.column());
    return block.position() + col;
  }

  return -1;
}

QTextCursor EditorInputMode::kateViCursorToTextCursor(const KateViI::Cursor &p_cursor) const {
  int pos = kateViCursorToPosition(p_cursor);
  if (pos == -1) {
    return QTextCursor();
  } else {
    auto cursor = textCursor();
    cursor.setPosition(pos);
    return cursor;
  }
}

QTextCursor EditorInputMode::kateViRangeToTextCursor(const KateViI::Range &p_range) const {
  auto cursor = textCursor();
  cursor.clearSelection();
  if (p_range.isValid()) {
    auto startPos = kateViCursorToPosition(p_range.start());
    auto endPos = kateViCursorToPosition(p_range.end());
    if (startPos > -1 && endPos > -1) {
      cursor.setPosition(startPos);
      cursor.setPosition(endPos, QTextCursor::KeepAnchor);
    }
  }
  return cursor;
}

QStringList EditorInputMode::textLines(const KateViI::Range &p_range, bool p_blockWise) const {
  QStringList ret;

  if (!p_range.isValid()) {
    return ret;
  }

  if (p_blockWise && (p_range.start().column() > p_range.end().column())) {
    return ret;
  }

  if (p_range.onSingleLine()) {
    auto text = line(p_range.start().line());
    ret << text.mid(p_range.start().column(), p_range.columnWidth());
  } else {
    const int endLine = qMin(p_range.end().line(), lines() - 1);
    for (int i = p_range.start().line(); i <= endLine; ++i) {
      auto text = line(i);
      if (!p_blockWise) {
        if (i == p_range.start().line()) {
          text = text.mid(p_range.start().column());
        } else if (i == p_range.end().line()) {
          text = text.mid(0, p_range.end().column());
        }
        ret << text;
      } else {
        EDITOR_NIY;
      }
    }
  }

  return ret;
}

QString EditorInputMode::getText(const KateViI::Range &p_range, bool p_blockWise) const {
  if (!p_range.isValid()) {
    return QString();
  }

  if (p_blockWise) {
    EDITOR_NIY;
    return QString();
  } else {
    auto cursor = kateViRangeToTextCursor(p_range);
    return cursor.selectedText();
  }
}

int EditorInputMode::firstChar(int p_line) const {
  auto text = line(p_line);
  return TextUtils::firstNonSpace(text);
}

void EditorInputMode::joinLines(uint p_first, uint p_last, bool p_trimSpace) {
  Q_ASSERT(p_first < p_last);

  auto doc = document();
  if (p_last >= (uint)doc->blockCount()) {
    p_last = doc->blockCount() - 1;
    if (p_first >= p_last) {
      return;
    }
  }

  editStart();

  auto block = doc->findBlockByNumber(p_first);
  Q_ASSERT(block.isValid());
  auto text = block.text();

  block = block.next();
  while ((uint)block.blockNumber() <= p_last) {
    Q_ASSERT(block.isValid());
    auto blockText = block.text();
    if (p_trimSpace) {
      // Skip empty or all-space block.
      if (!blockText.trimmed().isEmpty()) {
        // Check if there exists space at the end of last block.
        if (!text.isEmpty() && !text.back().isSpace()) {
          text += QLatin1Char(' ');
        }

        // Remove the indentation space.
        int idx = TextUtils::firstNonSpace(blockText);
        text += blockText.mid(idx);
      } else if (block.blockNumber() == static_cast<int>(p_last)) {
        // If the last block is empty, we need to add additional space at the
        // end.
        if (!text.isEmpty() && !text.back().isSpace()) {
          text += QLatin1Char(' ');
        }
        break;
      }
    } else {
      text += blockText;
    }

    block = block.next();
  }

  auto cursor = textCursor();
  cursor.setPosition(doc->findBlockByNumber(p_first).position());

  auto lastBlock = doc->findBlockByNumber(p_last);
  cursor.setPosition(lastBlock.position() + lastBlock.length() - 1, QTextCursor::KeepAnchor);

  cursor.insertText(text);
  m_textEdit->setTextCursor(cursor);

  editEnd();
}

int EditorInputMode::undoCount() const { return document()->isUndoAvailable() ? 1 : 0; }

void EditorInputMode::undo() { m_textEdit->undo(); }

int EditorInputMode::redoCount() const { return document()->isRedoAvailable() ? 1 : 0; }

void EditorInputMode::redo() { m_textEdit->redo(); }

int EditorInputMode::lastLine() const { return lines() - 1; }

bool EditorInputMode::replaceText(const KateViI::Range &p_range, const QString &p_text,
                                  bool p_blockWise) {
  if (p_blockWise) {
    EDITOR_NIY;
  } else {
    auto cursor = kateViRangeToTextCursor(p_range);
    if (cursor.hasSelection()) {
      cursor.insertText(p_text);
      m_textEdit->setTextCursor(cursor);
      return true;
    }
  }
  return false;
}

bool EditorInputMode::selection() const { return m_textEdit->hasSelection(); }

bool EditorInputMode::insertText(const KateViI::Cursor &p_position, const QString &p_text,
                                 bool p_blockWise) {
  if (m_editor->isReadOnly()) {
    return false;
  }

  if (p_text.isEmpty()) {
    return true;
  }

  if (p_blockWise) {
    EDITOR_NIY;
  } else {
    auto cursor = kateViCursorToTextCursor(p_position);
    if (cursor.isNull()) {
      return false;
    }

    cursor.insertText(p_text);
    m_textEdit->setTextCursor(cursor);
  }

  return true;
}

void EditorInputMode::connectMarkChanged(
    std::function<void(KateViI::KateViEditorInterface *p_editorInterface, KateViI::Mark p_mark,
                       KateViI::MarkInterface::MarkChangeAction p_action)>
        p_slot) {
  EDITOR_NIY;
}

void EditorInputMode::removeMark(int line, uint markType) { EDITOR_NIY; }

void EditorInputMode::indent(const KateViI::Range &p_range, int p_changes) {
  bool isIndent = p_changes > 0;
  if (!isIndent) {
    p_changes = -p_changes;
  }

  // Check if we need to indent the end line by column.
  int lineCnt = p_range.end().line() - p_range.start().line();
  if (p_range.end().column() > 0) {
    ++lineCnt;
  }
  auto startBlock = document()->findBlockByNumber(p_range.start().line());
  TextEditUtils::indentBlocks(!m_textEdit->isTabExpanded(), m_textEdit->getTabStopWidthInSpaces(),
                              startBlock, lineCnt, isIndent, p_changes);
}

void EditorInputMode::pageDown(bool p_half) {
  const int blockStep = p_half ? blockCountOfOnePageStep() / 2 : blockCountOfOnePageStep();
  auto cursor = textCursor();
  int block = cursor.block().blockNumber();
  block = qMin(block + blockStep, document()->blockCount() - 1);
  cursor.setPosition(document()->findBlockByNumber(block).position());
  m_textEdit->setTextCursor(cursor);
}

void EditorInputMode::pageUp(bool p_half) {
  const int blockStep = p_half ? blockCountOfOnePageStep() / 2 : blockCountOfOnePageStep();
  auto cursor = textCursor();
  int block = cursor.block().blockNumber();
  block = qMax(block - blockStep, 0);
  cursor.setPosition(document()->findBlockByNumber(block).position());
  m_textEdit->setTextCursor(cursor);
}

int EditorInputMode::blockCountOfOnePageStep() const {
  int lineCount = document()->blockCount();
  auto bar = m_textEdit->verticalScrollBar();
  int steps = (bar->maximum() - bar->minimum() + bar->pageStep());
  int pageLineCount = lineCount * (bar->pageStep() * 1.0 / steps);
  return pageLineCount;
}

void EditorInputMode::scrollInPage(const KateViI::Cursor &p_pos, KateViI::PagePosition p_dest) {
  TextEditUtils::PagePosition pp = TextEditUtils::PagePosition::Top;
  switch (p_dest) {
  case KateViI::PagePosition::Top:
    pp = TextEditUtils::PagePosition::Top;
    break;

  case KateViI::PagePosition::Center:
    pp = TextEditUtils::PagePosition::Center;
    break;

  case KateViI::PagePosition::Bottom:
    pp = TextEditUtils::PagePosition::Bottom;
    break;
  }

  TextEditUtils::scrollBlockInPage(m_textEdit, p_pos.line(), pp);
}

void EditorInputMode::align(const KateViI::Range &p_range) {
  auto startBlock = document()->findBlockByNumber(p_range.start().line());
  int lineCnt = p_range.end().line() - p_range.start().line() + 1;
  TextEditUtils::align(startBlock, lineCnt);
}

void EditorInputMode::align() {
  const int line = cursorPosition().line();
  KateViI::Range alignRange(KateViI::Cursor(line, 0), KateViI::Cursor(line, 0));
  if (selection()) {
    alignRange = selectionRange();
  }

  align(alignRange);
}

void EditorInputMode::clearOverriddenSelection() { m_textEdit->clearOverriddenSelection(); }

void EditorInputMode::backspace() {
  // Let Qt handle the Key_Backspace event (for auto bracket).
  // We don't see the need to handle it by ourselves.
  Q_ASSERT(false);

  editStart();

  auto cursor = textCursor();
  cursor.deletePreviousChar();
  m_textEdit->setTextCursor(cursor);

  editEnd();
}

void EditorInputMode::abortCompletion() { m_editor->abortCompletion(); }

void EditorInputMode::newLine(KateViI::NewLineIndent p_indent) {
  Q_UNUSED(p_indent);

  editStart();

  auto cursor = textCursor();
  cursor.movePosition(QTextCursor::EndOfBlock);
  cursor.insertBlock();
  if (p_indent == KateViI::NewLineIndent::Indent) {
    AutoIndentHelper::autoIndent(cursor, !m_textEdit->isTabExpanded(),
                                 m_textEdit->getTabStopWidthInSpaces());
  }
  m_textEdit->setTextCursor(cursor);

  editEnd();
}

bool EditorInputMode::setCursorPosition(KateViI::Cursor p_position) {
  auto block = document()->findBlockByNumber(p_position.line());
  if (block.isValid()) {
    updateCursor(p_position.line(), p_position.column());
    return true;
  }
  return false;
}

bool EditorInputMode::insertLine(int p_line, const QString &p_str) {
  if (m_editor->isReadOnly()) {
    return false;
  }

  const int blockCount = lines();
  if (p_line < 0 || p_line > blockCount) {
    return false;
  }

  editStart();

  auto cursor = textCursor();
  if (blockCount == p_line) {
    // Insert a new line after the last line.
    cursor.movePosition(QTextCursor::End);
  } else {
    auto block = document()->findBlockByNumber(p_line);
    Q_ASSERT(block.isValid());
    cursor.setPosition(block.position());
  }

  int pos = cursor.position();
  // After insertBlock(), the cursor is right behind the new block.
  cursor.insertBlock();
  cursor.setPosition(pos);
  cursor.insertText(p_str);
  m_textEdit->setTextCursor(cursor);

  editEnd();

  return true;
}

void EditorInputMode::setOverwriteMode(bool p_enabled) { m_textEdit->setOverwriteMode(p_enabled); }

QWidget *EditorInputMode::focusProxy() const { return m_textEdit; }

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

QString EditorInputMode::wordAt(const KateViI::Cursor &cursor) const {
  EDITOR_NIY;
  // TODO: Refer completer.cpp.
  return QString();
}

bool EditorInputMode::isReadOnly() const { return m_editor->isReadOnly(); }

void EditorInputMode::connectMouseReleased(std::function<void(QMouseEvent *)> p_slot) {
  connect(m_textEdit, &VTextEdit::mouseReleased, this, p_slot);
}

void EditorInputMode::scrollUp() {
  QScrollBar *vbar = m_textEdit->verticalScrollBar();
  if (vbar && (vbar->minimum() != vbar->maximum())) {
    vbar->triggerAction(QAbstractSlider::SliderSingleStepAdd);
  }
}

void EditorInputMode::scrollDown() {
  QScrollBar *vbar = m_textEdit->verticalScrollBar();
  if (vbar && (vbar->minimum() != vbar->maximum())) {
    vbar->triggerAction(QAbstractSlider::SliderSingleStepSub);
  }
}
