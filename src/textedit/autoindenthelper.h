#ifndef AUTOINDENTHELPER_H
#define AUTOINDENTHELPER_H

class QTextCursor;

namespace vte {
class AutoIndentHelper {
public:
  AutoIndentHelper() = delete;

  // @p_cursor is already within an edit block.
  // @p_cursor is the cursor right after an Enter.
  static void autoIndent(QTextCursor &p_cursor, bool p_useTab, int p_spaces);
};
} // namespace vte

#endif // AUTOINDENTHELPER_H
