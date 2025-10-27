#ifndef EDITORCOMPLETER_H
#define EDITORCOMPLETER_H

#include "completer.h"

namespace vte {
class VTextEditor;

class EditorCompleter : public CompleterInterface {
public:
  explicit EditorCompleter(VTextEditor *p_editor);

  QTextCursor textCursor() const Q_DECL_OVERRIDE;

  QString contents() const Q_DECL_OVERRIDE;

  QWidget *widget() const Q_DECL_OVERRIDE;

  QString getText(int p_start, int p_end) const Q_DECL_OVERRIDE;

  void insertCompletion(int p_start, int p_end, const QString &p_completion) Q_DECL_OVERRIDE;

  qreal scaleFactor() const Q_DECL_OVERRIDE;

  QTextDocument *document() const Q_DECL_OVERRIDE;

private:
  VTextEditor *m_editor = nullptr;
};
} // namespace vte

#endif // EDITORCOMPLETER_H
