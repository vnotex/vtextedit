#include "ksyntaxhighlighterwrapper.h"

#include <FoldingRegion>
#include <Format>
#include <Repository>

using namespace vte;

KSyntaxHighlighting::Repository *KSyntaxHighlighterWrapper::s_repository = nullptr;

QList<KSyntaxHighlighting::Definition>
KSyntaxHighlighterWrapper::definitionsForFileName(const QString &p_fileName) {
  // TODO: We should be able to override the mappings by config.
  auto definitions = repository()->definitionsForFileName(p_fileName).toList();
  return definitions;
}

KSyntaxHighlighting::Definition
KSyntaxHighlighterWrapper::definitionForSyntax(const QString &p_syntax) {
  if (p_syntax.isEmpty()) {
    return definitionForFileName(QStringLiteral("vnotex"));
  } else {
    return definitionForFileName(QStringLiteral("vnotex.") + p_syntax);
  }
}

KSyntaxHighlighting::Definition
KSyntaxHighlighterWrapper::definitionForFileName(const QString &p_fileName) {
  auto defs = definitionsForFileName(p_fileName);
  if (defs.isEmpty()) {
    return KSyntaxHighlighting::Definition();
  } else {
    return defs.at(0);
  }
}

KSyntaxHighlighterWrapper::KSyntaxHighlighterWrapper(ApplyFormatFunc p_applyFormatFunc,
                                                     ApplyFoldingFunc p_applyFoldingFunc,
                                                     QObject *p_parent)
    : QObject(p_parent), m_applyFormatFunc(p_applyFormatFunc),
      m_applyFoldingFunc(p_applyFoldingFunc) {
  Q_ASSERT(m_applyFormatFunc && m_applyFoldingFunc);
}

void KSyntaxHighlighterWrapper::Initialize(const QStringList &p_customDefinitionPaths) {
  if (!s_repository) {
    s_repository = new KSyntaxHighlighting::Repository();
    for (const auto &defPath : p_customDefinitionPaths) {
      s_repository->addCustomSearchPath(defPath);
    }
  }
}

KSyntaxHighlighting::Repository *KSyntaxHighlighterWrapper::repository() {
  Q_ASSERT(s_repository);
  return s_repository;
}

void KSyntaxHighlighterWrapper::applyFormat(int p_offset, int p_length,
                                            const KSyntaxHighlighting::Format &p_format) {
  m_applyFormatFunc(p_offset, p_length, p_format);
}

void KSyntaxHighlighterWrapper::applyFolding(int p_offset, int p_length,
                                             KSyntaxHighlighting::FoldingRegion p_region) {
  m_applyFoldingFunc(p_offset, p_length, p_region);
}

QTextCharFormat
KSyntaxHighlighterWrapper::toTextCharFormat(const KSyntaxHighlighting::Theme &p_theme,
                                            const KSyntaxHighlighting::Format &p_format) {
  QTextCharFormat tf;

  // WARNING: We need to always set the foreground color to avoid palette
  // issues. Otherwise, we will get some block's layout rect to 0x0.
  tf.setForeground(p_format.textColor(p_theme));

  if (p_format.hasBackgroundColor(p_theme)) {
    tf.setBackground(p_format.backgroundColor(p_theme));
  }
  if (p_format.isBold(p_theme)) {
    tf.setFontWeight(QFont::Bold);
  }
  if (p_format.isItalic(p_theme)) {
    tf.setFontItalic(true);
  }
  if (p_format.isUnderline(p_theme)) {
    tf.setFontUnderline(true);
  }
  if (p_format.isStrikeThrough(p_theme)) {
    tf.setFontStrikeOut(true);
  }

  return tf;
}
