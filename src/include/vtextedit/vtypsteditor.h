#ifndef VTEXTEDIT_VTYPSTEDITOR_H
#define VTEXTEDIT_VTYPSTEDITOR_H

#include <vtextedit/vtextedit_export.h>

#include <QScopedPointer>
#include <QSharedPointer>
#include <QStringList>
#include <QUrl>
#include <QWidget>

class QTextDocument;

namespace vte {
class TypstEditorConfig;
class VTextEditor;
struct TextEditorParameters;

// One source document and compiler session, independent of the presentation mode.
class VTEXTEDIT_EXPORT VTypstEditor : public QWidget {
  Q_OBJECT
public:
  enum class Mode { InPlacePreview, SideBySidePreview, Read };
  Q_ENUM(Mode)
  enum class CompileStatus { Idle, Compiling, Ready, Error, Unavailable };
  Q_ENUM(CompileStatus)

  VTypstEditor(const QSharedPointer<TypstEditorConfig> &p_config,
               const QSharedPointer<TextEditorParameters> &p_parameters,
               QWidget *p_parent = nullptr);
  ~VTypstEditor() Q_DECL_OVERRIDE;

  VTextEditor *getTextEditor() const;
  QTextDocument *document() const;
  Mode mode() const;
  void setMode(Mode p_mode);
  bool isReadOnly() const;
  void setReadOnly(bool p_readOnly);

  const TypstEditorConfig &getConfig() const;
  // Use this instead of the contained editor's setConfig to refresh Typst formats too.
  void setConfig(const QSharedPointer<TypstEditorConfig> &p_config);
  void setProject(const QString &p_root, const QString &p_mainFile);
  QString activeFile() const;
  // Changes this buffer's identity, not its contents. The host supplies the new text.
  void setActiveFile(const QString &p_file);
  // Paths are project-root-relative. Updating the active overlay replaces its source buffer.
  void setFileOverlay(const QString &p_file, const QString &p_text);
  // The active buffer remains an overlay while open; switch identity before removing it.
  void removeFileOverlay(const QString &p_file);

  CompileStatus compileStatus() const;
  QString compileMessage() const;
  QStringList diagnostics() const;
  void retryCompilation();
  void showOutputAtCursor();

signals:
  void modeChanged(vte::VTypstEditor::Mode p_mode);
  void compileStatusChanged(vte::VTypstEditor::CompileStatus p_status, const QString &p_message);
  void diagnosticsChanged(const QStringList &p_diagnostics);
  // Imported-file navigation is delegated to the host. Offsets are LF/UTF-16, half-open.
  void sourceLocationRequested(const QString &p_file, int p_start, int p_end);
  // The host applies its URL opening/security policy; the editor never launches URLs.
  void externalLinkRequested(const QUrl &p_url);

private:
  class Private;
  QScopedPointer<Private> d;
};

} // namespace vte

#endif // VTEXTEDIT_VTYPSTEDITOR_H
