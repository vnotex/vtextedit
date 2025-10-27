#include <vtextedit/texteditutils.h>

#include <QAbstractTextDocumentLayout>
#include <QHash>
#include <QScrollBar>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>

#include <vtextedit/textutils.h>

using namespace vte;

QTextBlock TextEditUtils::firstVisibleBlock(QTextEdit *p_edit) {
  const int topY = contentOffsetAtTop(p_edit);
  auto tb = findBlockByYPosition(p_edit->document(), topY);
  return tb;
}

QTextBlock TextEditUtils::lastVisibleBlock(QTextEdit *p_edit) {
  const int topY = contentOffsetAtTop(p_edit);
  // Minus 1 to not bring extra block in.
  const int bottomY = topY + p_edit->viewport()->height() - 1;
  auto tb = findBlockByYPosition(p_edit->document(), bottomY);
  return tb;
}

QTextBlock TextEditUtils::findBlockByYPosition(QTextDocument *p_doc, int p_y) {
  auto layout = p_doc->documentLayout();
  // Binary search to find the first block that contains @p_y.
  int first = 0, last = p_doc->blockCount() - 1;
  while (first <= last) {
    int mid = first + (last - first) / 2;
    auto tb = p_doc->findBlockByNumber(mid);
    if (!tb.isVisible()) {
      // Need to find another visible block as mid block.
      // Search to the right.
      bool found = false;
      tb = tb.next();
      while (tb.isValid() && tb.blockNumber() <= last) {
        if (tb.isVisible()) {
          found = true;
          break;
        }

        tb = tb.next();
      }

      if (!found) {
        // Search to the left.
        tb = p_doc->findBlockByNumber(mid);
        tb = tb.previous();
        while (tb.isValid() && tb.blockNumber() >= first) {
          if (tb.isVisible()) {
            found = true;
            break;
          }

          tb = tb.previous();
        }
      }

      if (!found) {
        // Invalid block.
        // It may happens when the invisible block is at the end of the
        // document.
        return QTextBlock();
      }
      mid = tb.blockNumber();
    }

    auto rect = layout->blockBoundingRect(tb);
    Q_ASSERT(rect.x() >= 0);
    if (rect.y() <= p_y && rect.y() + rect.height() > p_y) {
      // Found it.
      return tb;
    } else if (rect.y() > p_y) {
      last = mid - 1;
    } else {
      first = mid + 1;
    }
  }

  // First or last visible block.
  auto tb = p_doc->firstBlock();
  while (tb.isValid() && !tb.isVisible()) {
    tb = tb.next();
  }
  Q_ASSERT(tb.isValid());
  auto rect = layout->blockBoundingRect(tb);
  if (rect.y() > p_y) {
    return tb;
  }

  tb = p_doc->lastBlock();
  while (tb.isValid() && !tb.isVisible()) {
    tb = tb.previous();
  }
  Q_ASSERT(tb.isValid());
  return tb;
}

QPair<int, int> TextEditUtils::visibleBlockRange(QTextEdit *p_edit) {
  const int topY = contentOffsetAtTop(p_edit);
  // Minus 1 to not bring extra block in.
  const int bottomY = topY + p_edit->viewport()->height() - 1;

  auto firstBlock = findBlockByYPosition(p_edit->document(), topY);
  auto lastBlock = findBlockByYPosition(p_edit->document(), bottomY);
  return qMakePair(firstBlock.blockNumber(), lastBlock.blockNumber());
}

int TextEditUtils::contentOffsetAtTop(QTextEdit *p_edit) {
  auto sb = p_edit->verticalScrollBar();
  Q_ASSERT(sb);
  return sb->value();
}

void TextEditUtils::removeBlock(QTextBlock &p_block) {
  Q_ASSERT(p_block.isValid());
  QTextCursor cursor(p_block);
  const auto doc = cursor.document();
  const int blockCount = doc->blockCount();
  const int blockNum = p_block.blockNumber();

  cursor.select(QTextCursor::BlockUnderCursor);
  if (blockNum == blockCount - 1) {
    // The last block.
    cursor.deletePreviousChar();
  } else {
    cursor.deleteChar();
    if (blockNum == 0 && blockCount == doc->blockCount()) {
      // Deleting the first block will leave an empty block **sometimes**.
      cursor.deleteChar();
    }
  }
}

void TextEditUtils::scrollBlockInPage(QTextEdit *p_edit, int p_blockNumber,
                                      TextEditUtils::PagePosition p_dest, int p_margin) {
  auto doc = p_edit->document();
  if (p_blockNumber >= doc->blockCount()) {
    p_blockNumber = doc->blockCount() - 1;
  }

  QScrollBar *vsbar = p_edit->verticalScrollBar();
  if (!vsbar || !vsbar->isVisible()) {
    // No vertical scrollbar. No need to scroll.
    return;
  }

  auto block = doc->findBlockByNumber(p_blockNumber);
  const int top = contentOffsetAtTop(p_edit);
  const auto blockRect = doc->documentLayout()->blockBoundingRect(block);

  int height = p_edit->rect().height();
  {
    QScrollBar *sbar = p_edit->horizontalScrollBar();
    if (sbar && sbar->isVisible()) {
      height -= sbar->height();
    }
  }

  int delta = 0;
  switch (p_dest) {
  case PagePosition::Top:
    delta = blockRect.y() - top;
    break;

  case PagePosition::Center:
    delta = blockRect.y() - top - height / 2;
    break;

  case PagePosition::Bottom:
    delta = blockRect.bottom() - top - height;
    break;
  }

  if (qAbs(delta) > p_margin) {
    vsbar->setValue(vsbar->value() + delta);
  }
}

bool TextEditUtils::isEmptyBlock(const QTextBlock &p_block) { return p_block.length() == 1; }

int TextEditUtils::fetchIndentation(const QTextBlock &p_block) {
  return TextUtils::fetchIndentation(p_block.text());
}

QString TextEditUtils::fetchIndentationSpaces(const QTextBlock &p_block) {
  return TextUtils::fetchIndentationSpaces(p_block.text());
}

int TextEditUtils::calculateBlockMargin(const QTextBlock &p_block, int p_tabStopDistance) {
  // Font name -> space width.
  static QHash<QString, int> spaceWidthOfFonts;

  if (!p_block.isValid()) {
    return 0;
  }

  QString text = p_block.text();
  int nrSpaces = 0;
  int tabStopWidth = 0;
  for (int i = 0; i < text.size(); ++i) {
    if (!text[i].isSpace()) {
      break;
    } else if (text[i] == QLatin1Char(' ')) {
      ++nrSpaces;
    } else if (text[i] == QLatin1Char('\t')) {
      tabStopWidth += p_tabStopDistance;
    }
  }

  if (nrSpaces == 0) {
    return tabStopWidth;
  }

  int spaceWidth = 0;
  {
    QFont font;
    QVector<QTextLayout::FormatRange> fmts = p_block.layout()->formats();
    if (fmts.isEmpty()) {
      font = p_block.charFormat().font();
    } else {
      font = fmts.first().format.font();
    }

    QString fontName = font.toString();
    auto it = spaceWidthOfFonts.find(fontName);
    if (it != spaceWidthOfFonts.end()) {
      spaceWidth = it.value();
    } else {
      spaceWidth = QFontMetrics(font).horizontalAdvance(' ');
      spaceWidthOfFonts.insert(fontName, spaceWidth);
    }
  }

  return spaceWidth * nrSpaces + tabStopWidth;
}

void TextEditUtils::indentBlocks(QTextEdit *p_edit, bool p_useTab, int p_spaces, bool p_indent) {
  QTextBlock startBlock;
  int count = getSelectedBlockRange(p_edit, startBlock);
  indentBlocks(p_useTab, p_spaces, startBlock, count == 0 ? 1 : count, p_indent, 1);
}

int TextEditUtils::getSelectedBlockRange(QTextEdit *p_edit, QTextBlock &p_start) {
  auto cursor = p_edit->textCursor();
  if (cursor.hasSelection()) {
    int start = cursor.selectionStart();
    int end = cursor.selectionEnd();
    p_start = p_edit->document()->findBlock(start);
    auto endBlock = p_edit->document()->findBlock(end);
    return endBlock.blockNumber() - p_start.blockNumber() + 1;
  } else {
    p_start = cursor.block();
    return 0;
  }
}

void TextEditUtils::indentBlocks(bool p_useTab, int p_spaces, QTextBlock p_start, int p_blockCount,
                                 bool p_indent, int p_indentCount) {
  QTextCursor cursor(p_start);
  cursor.beginEditBlock();
  for (int i = 0; i < p_blockCount; ++i) {
    for (int j = 0; j < p_indentCount; ++j) {
      if (p_indent) {
        indentBlock(cursor, p_useTab, p_spaces, true);
      } else {
        unindentBlock(cursor, p_spaces);
      }
    }
    cursor.movePosition(QTextCursor::NextBlock);
  }
  cursor.endEditBlock();
}

void TextEditUtils::indentBlock(QTextCursor &p_cursor, bool p_useTab, int p_spaces,
                                bool p_skipEmptyBlock) {
  if (p_spaces <= 0) {
    return;
  }

  auto block = p_cursor.block();
  if (block.length() > 1 || !p_skipEmptyBlock) {
    int indentation = fetchIndentation(block);
    int pib = p_cursor.positionInBlock();
    p_cursor.movePosition(QTextCursor::StartOfBlock);
    p_cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::MoveAnchor, indentation);
    if (p_useTab) {
      // Insert Tab to indent.
      p_cursor.insertText(QStringLiteral("\t"));
      if (pib >= indentation) {
        ++pib;
      }
    } else {
      // Insert spaces to indent.
      int indentationSpaces = 0;
      const QString text = block.text();
      for (int i = indentation - 1; i >= 0; --i) {
        if (text[i] == QLatin1Char(' ')) {
          ++indentationSpaces;
        } else {
          break;
        }
      }

      int spaces = p_spaces - (indentationSpaces % p_spaces);
      p_cursor.insertText(QString(spaces, QLatin1Char(' ')));
      if (pib >= indentation) {
        pib += spaces;
      }
    }

    // Restore cursor position.
    p_cursor.setPosition(block.position() + pib);
  }
}

void TextEditUtils::unindentBlock(QTextCursor &p_cursor, int p_spaces) {
  const QTextBlock block = p_cursor.block();
  const QString text = block.text();
  if (text.isEmpty()) {
    return;
  }

  const int indentation = fetchIndentation(block);
  int pib = p_cursor.positionInBlock();
  p_cursor.movePosition(QTextCursor::StartOfBlock);
  p_cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::MoveAnchor, indentation);
  int deletedChars = 0;
  if (indentation == 0) {
    return;
  } else if (text[indentation - 1] == QLatin1Char('\t')) {
    p_cursor.deletePreviousChar();
    ++deletedChars;
  } else if (text[indentation - 1].isSpace()) {
    int indentationSpaces = 0;
    for (int i = indentation - 1; i >= 0; --i) {
      if (text[i] == QLatin1Char(' ')) {
        ++indentationSpaces;
      } else {
        break;
      }
    }
    int spaces = indentationSpaces % p_spaces;
    if (spaces == 0) {
      spaces = p_spaces;
    }
    for (int i = 0; i < spaces; ++i) {
      if (text[indentation - i - 1] == QLatin1Char(' ')) {
        p_cursor.deletePreviousChar();
        ++deletedChars;
      } else {
        break;
      }
    }
  }

  if (pib > indentation - deletedChars) {
    if (pib > indentation) {
      pib -= deletedChars;
    } else {
      pib = indentation;
    }
  }
  p_cursor.setPosition(block.position() + pib);
}

bool TextEditUtils::crossBlocks(QTextEdit *p_edit, int p_start, int p_end) {
  const auto doc = p_edit->document();
  auto startBlock = doc->findBlock(p_start);
  auto endBlock = doc->findBlock(p_end);
  return startBlock.blockNumber() != endBlock.blockNumber();
}

void TextEditUtils::selectBlockUnderCursor(QTextCursor &p_cursor) {
  // QTextCursor::select(BlockUnderCursor) will select the \n, too.
  p_cursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::MoveAnchor);
  p_cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
}

void TextEditUtils::insertBlock(QTextCursor &p_cursor, bool p_above) {
  p_cursor.movePosition(p_above ? QTextCursor::StartOfBlock : QTextCursor::EndOfBlock,
                        QTextCursor::MoveAnchor, 1);

  p_cursor.insertBlock();

  if (p_above) {
    p_cursor.movePosition(QTextCursor::PreviousBlock, QTextCursor::MoveAnchor, 1);
  }

  p_cursor.movePosition(QTextCursor::EndOfBlock);
}

QString TextEditUtils::getSelectedText(const QTextCursor &p_cursor) {
  auto text = p_cursor.selectedText();
  text.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
  return text;
}

void TextEditUtils::ensureBlockVisible(QTextEdit *p_edit, int p_blockNumber) {
  scrollBlockInPage(p_edit, p_blockNumber, PagePosition::Center);
}

void TextEditUtils::align(QTextBlock p_start, int p_cnt) {
  if (!p_start.isValid()) {
    return;
  }

  bool alignFirstBlock = false;
  QString indentationSpaces;
  auto preBlock = p_start.previous();
  if (preBlock.isValid()) {
    alignFirstBlock = true;
    indentationSpaces = fetchIndentationSpaces(preBlock);
  } else {
    indentationSpaces = fetchIndentationSpaces(p_start);
  }

  QTextCursor cursor(p_start);
  cursor.beginEditBlock();

  if (!alignFirstBlock) {
    cursor.movePosition(QTextCursor::NextBlock);
  }

  for (int i = alignFirstBlock ? 0 : 1; i < p_cnt; ++i) {
    auto curBlock = cursor.block();
    int oldIndentation = fetchIndentation(curBlock);
    if (oldIndentation > 0) {
      cursor.movePosition(QTextCursor::StartOfBlock);
      cursor.setPosition(curBlock.position() + oldIndentation, QTextCursor::KeepAnchor);
    }
    cursor.insertText(indentationSpaces);
    cursor.movePosition(QTextCursor::NextBlock);
  }

  cursor.endEditBlock();
}
