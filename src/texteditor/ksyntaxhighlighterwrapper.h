#ifndef KSYNTAXHIGHLIGHTERWRAPPER_H
#define KSYNTAXHIGHLIGHTERWRAPPER_H

#include <QObject>
#include <QStringList>
#include <QTextCharFormat>

#include <AbstractHighlighter>
#include <Definition>
#include <functional>

namespace KSyntaxHighlighting {
class Repository;
class Theme;
} // namespace KSyntaxHighlighting

namespace vte {
// Used as an external highlighter. The user needs to maintain the state.
class KSyntaxHighlighterWrapper : public QObject, public KSyntaxHighlighting::AbstractHighlighter {
  Q_OBJECT
  Q_INTERFACES(KSyntaxHighlighting::AbstractHighlighter)
  typedef std::function<void(int, int, const KSyntaxHighlighting::Format &)> ApplyFormatFunc;
  typedef std::function<void(int, int, KSyntaxHighlighting::FoldingRegion)> ApplyFoldingFunc;

public:
  KSyntaxHighlighterWrapper(ApplyFormatFunc p_applyFormatFunc, ApplyFoldingFunc p_applyFoldingFunc,
                            QObject *p_parent = nullptr);

  using KSyntaxHighlighting::AbstractHighlighter::highlightLine;

  static void Initialize(const QStringList &p_customDefinitionPaths);

  static KSyntaxHighlighting::Repository *repository();

  static KSyntaxHighlighting::Definition definitionForSyntax(const QString &p_syntax);

  static KSyntaxHighlighting::Definition definitionForFileName(const QString &p_fileName);

  static QTextCharFormat toTextCharFormat(const KSyntaxHighlighting::Theme &p_theme,
                                          const KSyntaxHighlighting::Format &p_format);

protected:
  void applyFormat(int p_offset, int p_length,
                   const KSyntaxHighlighting::Format &p_format) Q_DECL_OVERRIDE;

  void applyFolding(int p_offset, int p_length,
                    KSyntaxHighlighting::FoldingRegion p_region) Q_DECL_OVERRIDE;

private:
  static QList<KSyntaxHighlighting::Definition> definitionsForFileName(const QString &p_fileName);

  ApplyFormatFunc m_applyFormatFunc;
  ApplyFoldingFunc m_applyFoldingFunc;

  static KSyntaxHighlighting::Repository *s_repository;
};
} // namespace vte

#endif // KSYNTAXHIGHLIGHTERWRAPPER_H
