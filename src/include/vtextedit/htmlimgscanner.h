#ifndef VTEXTEDIT_HTMLIMGSCANNER_H
#define VTEXTEDIT_HTMLIMGSCANNER_H

#include "vtextedit_export.h"

#include <QChar>
#include <QString>
#include <QVector>

namespace vte {

// One attribute of an HTML `<img>` tag, with byte-exact source spans so a
// caller may rewrite it in place.
struct VTEXTEDIT_EXPORT HtmlImgAttr {
  // Lower-cased attribute name.
  QString m_name;

  // Half-open span of the whole `name="value"` (or bare `name`) text.
  int m_attrStart = -1;
  int m_attrEnd = -1;

  // Half-open span of the VALUE, quotes EXCLUDED. Equals m_attrEnd for a bare
  // attribute with no value.
  int m_valueStart = -1;
  int m_valueEnd = -1;

  // '"', '\'' or a null QChar when the value is unquoted / absent.
  QChar m_quote;

  // The value with HTML entities decoded. NEVER use its length to compute a
  // replacement span; use m_valueEnd - m_valueStart (or the whole attribute
  // span), exactly as for MarkdownLink::m_urlInLink.
  //
  // Decoded with cmark's decoder and its complete HTML5 named-reference table,
  // so the value agrees with what the renderer resolves for every reference
  // this tree's generators emit and for every well-formed one an author writes.
  //
  // KNOWN DIVERGENCE: cmark requires the terminating semicolon and applies
  // CommonMark's numeric rules, while an HTML *attribute* parser also accepts
  // semicolon-less legacy names (`&copy.png`) and remaps C1 numeric references
  // (`&#128;` -> U+20AC). Such a value is left with its `&` intact, so it
  // simply fails to resolve rather than resolving to the wrong file. Callers
  // that DELETE must treat a decoded value still containing `&` as ambiguous;
  // MarkdownViewWindow2::clearObsoleteImages() disarms itself on one.
  QString m_value;
};

// One `<img …>` tag found in a slice of source text.
//
// Every span is ABSOLUTE: the base offset passed to scanHtmlImgTags() has
// already been added.
struct VTEXTEDIT_EXPORT HtmlImgTag {
  // Half-open span of the whole `<img …>` / `<img …/>` tag.
  int m_tagStart = -1;
  int m_tagEnd = -1;

  // ALL attributes, in source order, duplicates kept.
  QVector<HtmlImgAttr> m_attrs;

  // The FIRST attribute with this (lower-cased) name, or nullptr. First-wins is
  // the HTML5 rule: a parser discards later duplicates.
  const HtmlImgAttr *attr(const char *p_name) const;

  // Whether any attribute is outside {src, alt, title, width, height}. A caller
  // converting to Markdown must refuse, or it would silently drop them.
  bool hasUnknownAttrs() const;

  // Whether any attribute name occurs more than once. A round trip over the
  // EFFECTIVE (first-wins) values cannot observe a discarded duplicate, so this
  // is a separate precondition for HTML -> Markdown conversion.
  bool hasDuplicateAttrs() const;

  // Decoded convenience accessors.
  QString src() const;
  QString alt() const;
  QString title() const;

  // 0 when absent, non-integer, non-positive, or a percentage.
  int width() const;
  int height() const;
};

// Raw-text context carried ACROSS calls.
//
// cmark emits an opening tag, the element contents and the closing tag as
// SEPARATE HTML nodes, so the "we are inside <script>" fact cannot live inside
// one scan. Holds the currently open raw-text element name (lower-cased), or
// empty.
struct VTEXTEDIT_EXPORT RawTextState {
  QString m_element;
};

// Every `<img …>` in @p_text, with spans offset by @p_baseOffset.
//
// This is the ONLY place in the tree allowed to pattern-match `<img` in NOTE
// SOURCE. Both the snapshot API (MarkdownUtils::fetchImageLinks()) and the live
// editor walker call it, so the two cannot drift.
//
// Rules:
// - Tag and attribute names are matched case-insensitively.
// - Single-quoted, double-quoted and unquoted attribute values are all read.
// - A tag MUST open and close within ONE line; a `<img` with no `>` before the
//   next newline is skipped entirely. Single-line tags are what lets a caller
//   slice raw source without mapping container prefixes (`> `, list indent),
//   since a prefix can then only ever appear BETWEEN tags.
// - Anything inside an HTML comment is skipped.
// - The contents of the raw-text elements `script`, `style`, `textarea` and
//   `title` are skipped: an `<img>` inside a JS string or a CSS rule is text.
//   @p_state carries that context in and out, so a slice may begin or end
//   inside one, and a slice that both opens and closes raw text keeps scanning
//   after the close. An unclosed raw-text element suppresses to the end (fail
//   safe).
// - A tag with no `src`, or an empty `src`, is dropped.
// - Duplicate attributes are preserved and flagged; the first wins for reads.
//
// @p_state may be null, in which case no raw-text context is carried.
VTEXTEDIT_EXPORT QVector<HtmlImgTag> scanHtmlImgTags(const QString &p_text, int p_baseOffset,
                                                     RawTextState *p_state);

// Escape a value for use inside a double-quoted HTML attribute. The single
// place escaping is spelled, so every generator and rewriter agrees.
VTEXTEDIT_EXPORT QString htmlEscapeAttrValue(const QString &p_value);

// Spell a whole `src="…"` attribute, always double-quoted. Rewriters replace
// the WHOLE attribute span with this, never just the value: an unquoted
// `src=old.png` renamed to a name containing a space would otherwise split into
// two attributes.
VTEXTEDIT_EXPORT QString spellHtmlSrcAttr(const QString &p_url);

} // namespace vte

#endif // VTEXTEDIT_HTMLIMGSCANNER_H
