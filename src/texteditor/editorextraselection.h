#ifndef EDITOREXTRASELECTION_H
#define EDITOREXTRASELECTION_H

#include "extraselectionmgr.h"

namespace vte {
class VTextEditor;

class EditorExtraSelection : public ExtraSelectionInterface {
public:
  explicit EditorExtraSelection(VTextEditor *p_editor);

  QTextCursor textCursor() const Q_DECL_OVERRIDE;

  QString selectedText() const Q_DECL_OVERRIDE;

  void setExtraSelections(const QList<QTextEdit::ExtraSelection> &p_selections) Q_DECL_OVERRIDE;

  QList<QTextCursor> findAllText(const QString &p_text, bool p_isRegularExpression,
                                 bool p_caseSensitive) Q_DECL_OVERRIDE;

private:
  VTextEditor *m_editor = nullptr;
};
} // namespace vte

#endif
