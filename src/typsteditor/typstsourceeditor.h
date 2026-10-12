#ifndef VTE_TYPSTSOURCEEDITOR_H
#define VTE_TYPSTSOURCEEDITOR_H

#include <QTimer>
#include <vtextedit/vtexteditor.h>

namespace vte {
class TextDocumentLayout;
class TypstHighlighter;

class TypstSourceEditor : public VTextEditor {
  Q_OBJECT
public:
  TypstSourceEditor(const QSharedPointer<TextEditorConfig> &p_config,
                    const QSharedPointer<TextEditorParameters> &p_parameters,
                    QWidget *p_parent = nullptr);
  ~TypstSourceEditor() Q_DECL_OVERRIDE;

  void setSyntax(const QString &p_syntax) Q_DECL_OVERRIDE;
  QString getSyntax() const Q_DECL_OVERRIDE;
  void zoom(int p_delta) Q_DECL_OVERRIDE;
  void applyConfig(const QSharedPointer<TextEditorConfig> &p_config);
  TextDocumentLayout *typstLayout() const;
  TypstHighlighter *typstHighlighter() const;

protected:
  bool eventFilter(QObject *p_object, QEvent *p_event) Q_DECL_OVERRIDE;

private:
  void refreshGeometry();
  QTimer m_geometryTimer;
  bool m_geometryPending = false;
};
} // namespace vte

#endif // VTE_TYPSTSOURCEEDITOR_H
