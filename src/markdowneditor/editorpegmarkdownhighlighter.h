#ifndef EDITORPEGMARKDOWNHIGHLIGHTER_H
#define EDITORPEGMARKDOWNHIGHLIGHTER_H

#include <vtextedit/pegmarkdownhighlighter.h>

namespace vte {
class VTextEditor;

class EditorPegMarkdownHighlighter : public PegMarkdownHighlighterInterface {
public:
  explicit EditorPegMarkdownHighlighter(VTextEditor *p_editor);

  QTextCursor textCursor() const Q_DECL_OVERRIDE;

  QPair<int, int> visibleBlockRange() const Q_DECL_OVERRIDE;

  void ensureCursorVisible() Q_DECL_OVERRIDE;

  QScrollBar *verticalScrollBar() const Q_DECL_OVERRIDE;

private:
  VTextEditor *m_editor = nullptr;
};
} // namespace vte

#endif // EDITORPEGMARKDOWNHIGHLIGHTER_H
