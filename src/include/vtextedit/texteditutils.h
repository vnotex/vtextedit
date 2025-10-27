#ifndef TEXTEDITUTILS_H
#define TEXTEDITUTILS_H

#include "vtextedit_export.h"

#include <QTextBlock>

class QTextEdit;
class QTextDocument;

namespace vte {
class VTEXTEDIT_EXPORT TextEditUtils {
public:
  enum class PagePosition { Top, Center, Bottom };

  TextEditUtils() = delete;

  static QTextBlock firstVisibleBlock(QTextEdit *p_edit);

  static QTextBlock lastVisibleBlock(QTextEdit *p_edit);

  static QTextBlock findBlockByYPosition(QTextDocument *p_doc, int p_y);

  static QPair<int, int> visibleBlockRange(QTextEdit *p_edit);

  static int contentOffsetAtTop(QTextEdit *p_edit);

  // Usually the cursor will be placed at the previous block which may be not
  // expected.
  static void removeBlock(QTextBlock &p_block);

  // Will not change current cursor.
  static void scrollBlockInPage(QTextEdit *p_edit, int p_blockNumber,
                                TextEditUtils::PagePosition p_dest, int p_margin = 0);

  static bool isEmptyBlock(const QTextBlock &p_block);

  static int fetchIndentation(const QTextBlock &p_block);

  static QString fetchIndentationSpaces(const QTextBlock &p_block);

  // Calculate the block margin (prefix spaces) in pixels.
  static int calculateBlockMargin(const QTextBlock &p_block, int p_tabStopDistance);

  // If has selection, indent selected blocks; otherwise, indent the cursor
  // block only.
  static void indentBlocks(QTextEdit *p_edit, bool p_useTab, int p_spaces, bool p_indent);

  // Indent @p_count blocks from @p_start block.
  static void indentBlocks(bool p_useTab, int p_spaces, QTextBlock p_start, int p_blockCount,
                           bool p_indent, int p_indentCount);

  static void align(QTextBlock p_start, int p_cnt);

  // Return number of selected blocks.
  static int getSelectedBlockRange(QTextEdit *p_edit, QTextBlock &p_start);

  static void indentBlock(QTextCursor &p_cursor, bool p_useTab, int p_spaces,
                          bool p_skipEmptyBlock);

  static void unindentBlock(QTextCursor &p_cursor, int p_spaces);

  static bool crossBlocks(QTextEdit *p_edit, int p_start, int p_end);

  static void selectBlockUnderCursor(QTextCursor &p_cursor);

  static void insertBlock(QTextCursor &p_cursor, bool p_above);

  static QString getSelectedText(const QTextCursor &p_cursor);

  static void ensureBlockVisible(QTextEdit *p_edit, int p_blockNumber);
};
} // namespace vte

#endif // TEXTEDITUTILS_H
