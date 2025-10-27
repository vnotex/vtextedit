#include "extraselectionmgr.h"

#include <QTextBlock>
#include <QTimer>

using namespace vte;

ExtraSelectionMgr::ExtraSelectionMgr(ExtraSelectionInterface *p_interface, QObject *p_parent)
    : QObject(p_parent), m_interface(p_interface) {
  const int extraSelectionTimerInterval = 200;
  const int whitespaceHighlightTimerInterval = 300;
  const int selectedTextHighlightTimerInterval = 300;

  m_extraSelectionTimer = new QTimer(this);
  m_extraSelectionTimer->setSingleShot(true);
  m_extraSelectionTimer->setInterval(extraSelectionTimerInterval);
  connect(m_extraSelectionTimer, &QTimer::timeout, this, &ExtraSelectionMgr::applyExtraSelections);

  m_whitespaceHighlightTimer = new QTimer(this);
  m_whitespaceHighlightTimer->setSingleShot(true);
  m_whitespaceHighlightTimer->setInterval(whitespaceHighlightTimerInterval);
  connect(m_whitespaceHighlightTimer, &QTimer::timeout, this, [=]() { highlightWhitespace(); });

  m_selectedTextHighlightTimer = new QTimer(this);
  m_selectedTextHighlightTimer->setSingleShot(true);
  m_selectedTextHighlightTimer->setInterval(selectedTextHighlightTimerInterval);
  connect(m_selectedTextHighlightTimer, &QTimer::timeout, this, [=]() { highlightSelectedText(); });

  initBuiltInExtraSelections();
}

void ExtraSelectionMgr::initBuiltInExtraSelections() {
  m_extraSelections.resize(SelectionType::MaxBuiltInSelection);

  m_extraSelections[SelectionType::CursorLine].m_enabled = true;
  m_extraSelections[SelectionType::CursorLine].m_isFullWidth = true;
  m_extraSelections[SelectionType::CursorLine].m_background = QColor("#c5cae9");

  m_extraSelections[SelectionType::TrailingSpace].m_enabled = true;
  m_extraSelections[SelectionType::TrailingSpace].m_background = QColor("#a8a8a8");

  m_extraSelections[SelectionType::Tab].m_enabled = true;
  m_extraSelections[SelectionType::Tab].m_background = QColor("#cfcfcf");

  m_extraSelections[SelectionType::SelectedText].m_enabled = true;
  m_extraSelections[SelectionType::SelectedText].m_foreground = QColor("#222222");
  m_extraSelections[SelectionType::SelectedText].m_background = QColor("#dfdf00");
}

void ExtraSelectionMgr::setExtraSelectionEnabled(int p_type, bool p_enabled) {
  Q_ASSERT(p_type < m_extraSelections.size());
  if (m_extraSelections[p_type].m_enabled != p_enabled) {
    m_extraSelections[p_type].m_enabled = p_enabled;
    updateOnExtraSelectionChange(p_type);
  }
}

void ExtraSelectionMgr::setExtraSelectionFormat(int p_type, const QColor &p_foreground,
                                                const QColor &p_background, bool p_isFullWidth) {
  Q_ASSERT(p_type < m_extraSelections.size());
  bool changed = false;
  if (m_extraSelections[p_type].m_foreground != p_foreground) {
    m_extraSelections[p_type].m_foreground = p_foreground;
    changed = true;
  }

  if (m_extraSelections[p_type].m_background != p_background) {
    m_extraSelections[p_type].m_background = p_background;
    changed = true;
  }

  if (m_extraSelections[p_type].m_isFullWidth != p_isFullWidth) {
    m_extraSelections[p_type].m_isFullWidth = p_isFullWidth;
    changed = true;
  }

  if (changed) {
    updateOnExtraSelectionChange(p_type);
  }
}

void ExtraSelectionMgr::updateOnExtraSelectionChange(int p_type) {
  switch (p_type) {
  case SelectionType::CursorLine:
    highlightCursorLine();
    break;

  case SelectionType::TrailingSpace:
  case SelectionType::Tab:
    highlightWhitespace();
    break;

  case SelectionType::SelectedText:
    highlightSelectedText();
    break;

  default:
    // No need to update any built-in selections. Just re-apply them.
    kickOffExtraSelections();
    break;
  }
}

void ExtraSelectionMgr::updateAllExtraSelections() {
  highlightCursorLine(false);

  highlightWhitespace(false);

  highlightSelectedText(false);

  applyExtraSelections();
}

void ExtraSelectionMgr::handleCursorPositionChange() {
  bool needUpdate = false;
  auto cursor = m_interface->textCursor();
  const bool cursorLineChanged = m_lastCursor.m_blockNumber != cursor.blockNumber();
  if (m_cursorBehindTrailingSpace) {
    // Before the cursor position change, the cursor is right behind a trailing
    // space, so we need to re-highlight it after the cursor change.
    if (!cursorLineChanged && cursor.atBlockEnd()) {
      // We are still at the same line and at the end of the block.
      // No need to rehighlight.
      auto block = cursor.block();
      auto text = block.text();
      if (text.isEmpty() || !text.at(text.size() - 1).isSpace()) {
        // Now a non-space char is appended to the end.
        m_cursorBehindTrailingSpace = false;
      }
    } else {
      needUpdate = highlightTrailingSpace() || needUpdate;
    }
  } else {
    // Check if the cursor now right behind a trailing space.
    auto &extraSelection = m_extraSelections[SelectionType::TrailingSpace];
    if (extraSelection.m_enabled && cursor.atBlockEnd()) {
      auto block = cursor.block();
      auto text = block.text();
      if (!text.isEmpty() && text.at(text.size() - 1).isSpace()) {
        m_cursorBehindTrailingSpace = true;
        auto &selections = extraSelection.m_selections;
        for (auto it = selections.begin(); it != selections.end(); ++it) {
          auto blockNumber = it->cursor.blockNumber();
          if (blockNumber == block.blockNumber()) {
            // Remove it.
            selections.erase(it);
            needUpdate = true;
            break;
          } else if (blockNumber > block.blockNumber()) {
            // We assume that it is sorted.
            break;
          }
        }
      }
    }
  }

  if (cursorLineChanged) {
    highlightCursorLine();
    // highlightCursorLine() already call the update.
    needUpdate = false;
  } else {
    // Visual line (word-wrap in one block).
    if (m_highlightCursorVisualLineEnabled &&
        ((m_lastCursor.m_positionInBlock - m_lastCursor.m_columnNumber) !=
         (cursor.positionInBlock() - cursor.columnNumber()))) {
      highlightCursorLine();
      // highlightCursorLine() already call the update.
      needUpdate = false;
    }
  }

  m_lastCursor.update(cursor);

  if (needUpdate) {
    kickOffExtraSelections();
  }
}

void ExtraSelectionMgr::applyExtraSelections() {
  m_extraSelectionTimer->stop();

  QList<QTextEdit::ExtraSelection> selections;
  const int nrExtra = m_extraSelections.size();
  for (int i = 0; i < SelectionType::MaxBuiltInSelection; ++i) {
    if (m_extraSelections[i].m_enabled) {
      selections.append(m_extraSelections[i].m_selections);
    }
  }

  for (int i = SelectionType::MaxBuiltInSelection; i < nrExtra; ++i) {
    if (m_extraSelections[i].m_enabled) {
      selections.append(m_extraSelections[i].m_selections);
    }
  }

  m_interface->setExtraSelections(selections);
}

void ExtraSelectionMgr::kickOffExtraSelections() { m_extraSelectionTimer->start(); }

bool ExtraSelectionMgr::isExtraSelectionEnabled(SelectionType p_type) const {
  Q_ASSERT(p_type < m_extraSelections.size());
  return m_extraSelections[p_type].m_enabled;
}

void ExtraSelectionMgr::highlightCursorLine(bool p_applyNow) {
  auto &extraSelection = m_extraSelections[SelectionType::CursorLine];
  auto &selections = extraSelection.m_selections;
  if (extraSelection.m_enabled) {
    selections.clear();

    QTextEdit::ExtraSelection select;
    select.format = extraSelection.format();

    auto cursor = m_interface->textCursor();
    if (m_highlightCursorVisualLineEnabled) {
      cursor.clearSelection();
      select.cursor = cursor;
      selections.append(select);
    } else {
      // Highlight whole block (multiple visual lines).
      cursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::MoveAnchor, 1);
      auto block = cursor.block();
      int blockEnd = block.position() + block.length();
      // In case of there is only one block and one visual line.
      int lastPos = -1;
      while (cursor.position() < blockEnd && lastPos != cursor.position()) {
        select.cursor = cursor;
        selections.append(select);

        lastPos = cursor.position();
        cursor.movePosition(QTextCursor::Down, QTextCursor::MoveAnchor, 1);
      }
    }
  } else {
    // Clear.
    if (selections.isEmpty()) {
      return;
    }
    selections.clear();
  }

  if (p_applyNow) {
    // For cursor line, we need to apply it right now.
    applyExtraSelections();
  }
}

void ExtraSelectionMgr::setHighlightCursorVisualLineEnabled(bool p_enabled) {
  if (m_highlightCursorVisualLineEnabled != p_enabled) {
    m_highlightCursorVisualLineEnabled = p_enabled;
    highlightCursorLine();
  }
}

void ExtraSelectionMgr::highlightWhitespace(bool p_applyNow) {
  m_whitespaceHighlightTimer->stop();
  bool needUpdate = false;

  // Trailing space.
  {
    bool ret = highlightTrailingSpace();
    if (ret) {
      needUpdate = true;
    }
  }

  // Tab.
  {
    // Treat it as trailing space if a tab is at the end of the line.
    bool ret = highlightWhitespaceInternal(QStringLiteral("\\t(?!$)"), SelectionType::Tab, nullptr);
    if (ret) {
      needUpdate = true;
    }
  }

  if (p_applyNow && needUpdate) {
    kickOffExtraSelections();
  }
}

bool ExtraSelectionMgr::highlightTrailingSpace() {
  m_cursorBehindTrailingSpace = false;
  auto cursorPos = m_interface->textCursor().position();
  bool ret = highlightWhitespaceInternal(QStringLiteral("\\s+$"), SelectionType::TrailingSpace,
                                         [this, cursorPos](const QTextCursor &p_cursor) {
                                           if (p_cursor.selectionEnd() == cursorPos) {
                                             this->m_cursorBehindTrailingSpace = true;
                                             return false;
                                           }
                                           return true;
                                         });
  return ret;
}

bool ExtraSelectionMgr::highlightWhitespaceInternal(
    const QString &p_text, SelectionType p_type,
    const std::function<bool(const QTextCursor &)> &p_filter) {
  bool needUpdate = false;
  auto &extraSelection = m_extraSelections[p_type];
  auto &selections = extraSelection.m_selections;
  if (extraSelection.m_enabled) {
    selections.clear();
    findAllTextAsExtraSelection(p_text, true, false, p_type, extraSelection.format(), p_filter);
    needUpdate = true;
  } else {
    // Clear.
    if (!selections.isEmpty()) {
      selections.clear();
      needUpdate = true;
    }
  }
  return needUpdate;
}

void ExtraSelectionMgr::highlightSelectedText(bool p_applyNow) {
  m_selectedTextHighlightTimer->stop();

  auto &extraSelection = m_extraSelections[SelectionType::SelectedText];
  auto &selections = extraSelection.m_selections;
  if (extraSelection.m_enabled) {
    auto selectedText = m_interface->selectedText().trimmed();
    if (selectedText.isEmpty() || selectedText.contains('\n')) {
      if (selections.isEmpty()) {
        return;
      }
      selections.clear();
    } else {
      findAllTextAsExtraSelection(selectedText, false, true, SelectionType::SelectedText,
                                  extraSelection.format());
    }
  } else {
    // Clear.
    if (selections.isEmpty()) {
      return;
    }
    selections.clear();
  }

  if (p_applyNow) {
    kickOffExtraSelections();
  }
}

void ExtraSelectionMgr::handleContentsChange() { m_whitespaceHighlightTimer->start(); }

void ExtraSelectionMgr::handleSelectionChange() { m_selectedTextHighlightTimer->start(); }

void ExtraSelectionMgr::findAllTextAsExtraSelection(
    const QString &p_text, bool p_isRegularExpression, bool p_caseSensitive, SelectionType p_type,
    const QTextCharFormat &p_format, const std::function<bool(const QTextCursor &)> &p_filter) {
  auto &extraSelection = m_extraSelections[p_type];
  Q_ASSERT(extraSelection.m_enabled);
  auto &selections = extraSelection.m_selections;
  selections.clear();
  auto cursors = m_interface->findAllText(p_text, p_isRegularExpression, p_caseSensitive);
  selections.reserve(cursors.size());
  QTextEdit::ExtraSelection select;
  select.format = p_format;
  for (const auto &cursor : cursors) {
    if (p_filter && !p_filter(cursor)) {
      continue;
    }
    select.cursor = cursor;
    selections.append(select);
  }
}

int ExtraSelectionMgr::registerExtraSelection() {
  int size = m_extraSelections.size();
  m_extraSelections.push_back(ExtraSelection());
  return size;
}

void ExtraSelectionMgr::setSelections(int p_type, const QList<QTextCursor> &p_selections) {
  Q_ASSERT(p_type < m_extraSelections.size());
  auto &extraSelection = m_extraSelections[p_type];
  auto &selections = extraSelection.m_selections;
  if (extraSelection.m_enabled) {
    selections.clear();
    QTextEdit::ExtraSelection select;
    select.format = extraSelection.format();
    for (int i = 0; i < p_selections.size(); ++i) {
      select.cursor = p_selections[i];
      selections.append(select);
    }
  } else {
    if (selections.isEmpty()) {
      return;
    }
    selections.clear();
  }

  kickOffExtraSelections();
}
