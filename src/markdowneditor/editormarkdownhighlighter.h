#ifndef EDITORMARKDOWNHIGHLIGHTER_H
#define EDITORMARKDOWNHIGHLIGHTER_H

#include <vtextedit/markdownhighlighter.h>

namespace vte {
class VTextEditor;

class EditorMarkdownHighlighter : public MarkdownHighlighterInterface {
public:
  explicit EditorMarkdownHighlighter(VTextEditor *p_editor);

  QTextCursor textCursor() const Q_DECL_OVERRIDE;

  QPair<int, int> visibleBlockRange() const Q_DECL_OVERRIDE;

  void ensureCursorVisible() Q_DECL_OVERRIDE;

  QScrollBar *verticalScrollBar() const Q_DECL_OVERRIDE;

private:
  VTextEditor *m_editor = nullptr;
};
} // namespace vte

#endif // EDITORMARKDOWNHIGHLIGHTER_H
