#include "autoindenthelper.h"

#include <QTextBlock>
#include <QTextCursor>

#include <vtextedit/texteditutils.h>
#include <vtextedit/textutils.h>

using namespace vte;

void AutoIndentHelper::autoIndent(QTextCursor &p_cursor, bool p_useTab, int p_spaces) {
  auto block = p_cursor.block();
  const auto text = block.text();
  const int pib = p_cursor.positionInBlock();

  auto preBlock = block.previous();
  Q_ASSERT(preBlock.isValid());
  const auto preText = preBlock.text();

  if (preText.trimmed().isEmpty()) {
    return;
  }

  // Insert an extract block if it is empty brackets.
  bool needExtraBlock = false;
  if (pib < text.size() && (text[pib] == QLatin1Char(']') || text[pib] == QLatin1Char('}'))) {
    if (preText.size() > 0 && TextUtils::matchBracket(preText[preText.size() - 1], text[pib])) {
      // Insert a new block and indent.
      needExtraBlock = true;
    }
  }

  // Indent current block as pre block.
  const auto indentation = TextUtils::fetchIndentationSpaces(preText);
  if (!indentation.isEmpty()) {
    p_cursor.insertText(indentation);
  }

  if (needExtraBlock) {
    p_cursor.movePosition(QTextCursor::StartOfBlock);
    p_cursor.insertBlock();
    p_cursor.movePosition(QTextCursor::PreviousBlock);
    p_cursor.insertText(indentation);

    TextEditUtils::indentBlock(p_cursor, p_useTab, p_spaces, false);
  }
}
