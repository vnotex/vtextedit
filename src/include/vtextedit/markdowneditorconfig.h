#ifndef VTEXTEDIT_MARKDOWNEDITORCONFIG_H
#define VTEXTEDIT_MARKDOWNEDITORCONFIG_H

#include "global.h"
#include "texteditorconfig.h"

namespace vte {
class VTEXTEDIT_EXPORT MarkdownEditorConfig {
public:
  enum InplacePreviewSource {
    NoInplacePreview = 0,
    ImageLink = 0x1,
    CodeBlock = 0x2,
    Math = 0x4,

    // Renders tables as an editable sheet which writes back to the document.
    // Opt-in: an application has to ask for a preview which can rewrite the
    // user's Markdown.
    Table = 0x8
  };
  Q_DECLARE_FLAGS(InplacePreviewSources, InplacePreviewSource);

  MarkdownEditorConfig(const QSharedPointer<TextEditorConfig> &p_textEditorConfig);

  void fillDefaultTheme();

  // Override the font family of Text style.
  void overrideTextFontFamily(const QString &p_fontFamily);

  QSharedPointer<TextEditorConfig> m_textEditorConfig;

  // Whether constrain the width of in-place preview.
  bool m_constrainInplacePreviewWidthEnabled = false;

  InplacePreviewSources m_inplacePreviewSources;

  // Whether use WebCodeBlockHighlighter or KSyntaxCodeBlockHighlighter for code
  // block syntax highlight.
  bool m_webCodeBlockHighlighterEnabled = true;

  // Fold a foldable region as soon as it first gets a valid in-place preview,
  // painted or interactive. Only the initial state is decided, once per
  // preview-bearing region for as long as its folding range lives: unfolding it
  // by hand is never undone, a region whose interior holds the caret when the
  // preview appears is left open for good, and changing this at runtime only
  // affects regions which have not been settled yet.
  bool m_autoFoldPreviewedBlocksEnabled = true;

  // Write a table sheet back as a padded, column-aligned pipe table instead of
  // the compact one. Off by default, which is the historical output byte for
  // byte. Changing it at runtime affects subsequent commits only: no existing
  // table source is ever reformatted on its own.
  bool m_alignTableSourceEnabled = false;

private:
  void overrideTextStyle();
};
} // namespace vte

Q_DECLARE_OPERATORS_FOR_FLAGS(vte::MarkdownEditorConfig::InplacePreviewSources)

#endif // MARKDOWNEDITORCONFIG_H
