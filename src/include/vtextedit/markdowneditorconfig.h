#ifndef VTEXTEDIT_MARKDOWNEDITORCONFIG_H
#define VTEXTEDIT_MARKDOWNEDITORCONFIG_H

#include "global.h"
#include "texteditorconfig.h"

#include <array>
#include <initializer_list>
#include <optional>
#include <utility>
#include <variant>

namespace vte {
struct MarkdownConcealShortenDestination {
  int m_threshold = 20;
  int m_keepPrefix = 3;
  int m_keepSuffix = 3;
  QString m_replacement = QString(3, QChar(0x00b7));
};

struct MarkdownConcealHideSyntax {};

using MarkdownConcealRule =
    std::variant<MarkdownConcealShortenDestination, MarkdownConcealHideSyntax>;

// One optional rule per kind. There is no separate enablement mask to get out of sync.
class MarkdownConcealRules {
public:
  MarkdownConcealRules() = default;
  MarkdownConcealRules(
      std::initializer_list<std::pair<MarkdownConcealElement, MarkdownConcealRule>> p_rules) {
    for (const auto &entry : p_rules) {
      set(entry.first, entry.second);
    }
  }

  static bool isDestination(MarkdownConcealElement p_kind) {
    return p_kind == MarkdownConcealElement::ImageUrl ||
           p_kind == MarkdownConcealElement::LinkUrl ||
           p_kind == MarkdownConcealElement::ReferenceUrl;
  }

  // Invalid kind/operation combinations are rejected, never reinterpreted.
  bool set(MarkdownConcealElement p_kind, const MarkdownConcealRule &p_rule) {
    if (p_kind <= MarkdownConcealElement::None || p_kind >= MarkdownConcealElement::Count ||
        isDestination(p_kind) !=
            std::holds_alternative<MarkdownConcealShortenDestination>(p_rule)) {
      return false;
    }
    m_rules[static_cast<size_t>(p_kind)] = p_rule;
    return true;
  }

  void remove(MarkdownConcealElement p_kind) {
    if (p_kind > MarkdownConcealElement::None && p_kind < MarkdownConcealElement::Count) {
      m_rules[static_cast<size_t>(p_kind)].reset();
    }
  }

  const MarkdownConcealRule *get(MarkdownConcealElement p_kind) const {
    if (p_kind <= MarkdownConcealElement::None || p_kind >= MarkdownConcealElement::Count) {
      return nullptr;
    }
    const auto &rule = m_rules[static_cast<size_t>(p_kind)];
    return rule ? &*rule : nullptr;
  }

  void clear() {
    for (auto &rule : m_rules) {
      rule.reset();
    }
  }

  bool isEmpty() const {
    for (const auto &rule : m_rules) {
      if (rule) {
        return false;
      }
    }
    return true;
  }

  bool hasSyntaxRules() const {
    for (const auto &rule : m_rules) {
      if (rule && std::holds_alternative<MarkdownConcealHideSyntax>(*rule)) {
        return true;
      }
    }
    return false;
  }

private:
  std::array<std::optional<MarkdownConcealRule>, static_cast<size_t>(MarkdownConcealElement::Count)>
      m_rules;
};

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

  // Effective cmark grammar; 0 preserves standalone native defaults.
  int m_documentParseOptions = 0;

  // Whether constrain the width of in-place preview.
  bool m_constrainInplacePreviewWidthEnabled = false;

  InplacePreviewSources m_inplacePreviewSources;

  // Syntax rules are opt-in. Empty disables concealment; source text is never changed.
  MarkdownConcealRules m_concealRules = {
      {MarkdownConcealElement::ImageUrl, MarkdownConcealShortenDestination{}},
      {MarkdownConcealElement::LinkUrl, MarkdownConcealShortenDestination{}},
      {MarkdownConcealElement::ReferenceUrl, MarkdownConcealShortenDestination{}}};

  // Native KSyntaxHighlighting is the default for code blocks. Set true before
  // constructing the editor to use the host-provided external/web highlighter.
  bool m_webCodeBlockHighlighterEnabled = false;

  // Fold a foldable region as soon as it first gets a valid in-place preview,
  // painted or interactive. Only the initial state is decided, once per
  // preview-bearing region for as long as its folding range lives: unfolding it
  // by hand is never undone, a region whose interior holds the caret when the
  // preview appears is left open for good, and changing this at runtime only
  // affects regions which have not been settled yet.
  bool m_autoFoldPreviewedBlocksEnabled = true;

  // Format changed pipe table source after 500 ms of source-edit inactivity,
  // including programmatic cursor edits. Preview commits serialize the aligned
  // form in their existing transaction. Opt-in: loading documents and toggling
  // this option never rewrite existing source. Formatting joins the preceding
  // undo edit block where Qt permits; a bare insertion may retain a separate
  // formatting undo step.
  bool m_autoFormatTableSourceEnabled = false;

  // Normalize structurally changed ordered lists after 500 ms of source-edit
  // inactivity, including programmatic cursor edits, keeping each list's start.
  // When editing splits a list, later fragments restart at one unless their
  // first number was explicitly changed or their first marker is newly authored.
  // Blank lines alone do not restart a list that the Markdown AST keeps intact.
  // Opt-in: load, enabling this option and undo/redo never normalize source.
  // Joins the preceding edit block where Qt permits; a bare cursor insertion
  // may retain a separate formatting undo step. Identical config reapplication
  // preserves pending work; disabling cancels numbering, not table formatting.
  bool m_autoNumberOrderedListsEnabled = false;

private:
  void overrideTextStyle();
};
} // namespace vte

Q_DECLARE_OPERATORS_FOR_FLAGS(vte::MarkdownEditorConfig::InplacePreviewSources)

#endif // MARKDOWNEDITORCONFIG_H
