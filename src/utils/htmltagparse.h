#ifndef VTE_HTMLTAGPARSE_H
#define VTE_HTMLTAGPARSE_H

// INTERNAL header: the low-level HTML tag lexer shared by the `<img>` scanner
// and the `<table>` scanner. It lives next to the .cpp (not under
// include/vtextedit) so it is neither installed nor consumable by users of the
// library.
//
// It exists so the two scanners cannot drift on how an attribute list is read.
// AGENTS.md requires one scanner per construct; this is the layer BELOW that
// rule -- the single spelling of "what an attribute is" that both constructs'
// scanners are built from. Duplicating parseAttrs() would reintroduce exactly
// the divergence the single-scanner rule exists to prevent.

#include <vtextedit/htmlimgscanner.h>

#include <QChar>
#include <QString>
#include <QVector>

namespace vte {
namespace htmltag {

// The first character of a tag or attribute name.
bool isNameStart(QChar p_ch);

// Deliberately permissive: anything that is not whitespace and not one of the
// attribute-syntax delimiters is part of the name. `data-*` and `xml:lang` must
// survive so a caller can see them.
bool isNameChar(QChar p_ch);

// script / style / textarea / title: elements whose contents are text, not
// markup.
bool isRawTextElement(const QString &p_lowerName);

// Decode HTML character references with cmark's decoder and its complete HTML5
// named-reference table, so a value agrees with what the renderer resolves.
QString decodeEntities(const QString &p_text);

// Skip past the end of the tag starting at @p_idx (which points at '<'),
// honoring quoted attribute values so a '>' inside `alt="a>b"` does not
// terminate it. Returns the index just past the closing '>', or -1 when the tag
// is never closed.
int skipTag(const QString &p_text, int p_idx);

// Index just past the '>' of @p_element's closing tag, searching from @p_idx,
// or -1 when it is not closed in @p_text.
int findRawTextClose(const QString &p_text, int p_idx, const QString &p_element);

// Parse the attributes of a tag whose name ends at @p_idx, refusing to read
// past @p_limit (the start of the next newline, per the one-line rule). On
// success returns true, appends to @p_attrs and sets @p_tagEnd to the index
// just past the closing '>'. Spans in @p_attrs are offset by @p_baseOffset.
//
// Returns false for a multiline or malformed tag; the caller must then ignore
// the tag entirely rather than guess.
bool parseAttrs(const QString &p_text, int p_idx, int p_limit, int p_baseOffset,
                QVector<HtmlAttr> &p_attrs, int &p_tagEnd);

// The FIRST attribute with this (lower-cased) name, or nullptr. First-wins is
// the HTML5 rule: a parser discards later duplicates.
const HtmlAttr *findAttr(const QVector<HtmlAttr> &p_attrs, const char *p_name);

// Where a top-level lexer must resume after meeting an `<img …>` opener.
//
// This exists so the `<img>` and the `<table>` scanner cannot disagree. Both
// walk the SAME slice carrying independent copies of RawTextState, and the
// walker asserts the two outgoing states are equal; a divergence of even one
// character can leave one of them inside a `<script>` and the other outside it.
//
// The rule is the `<img>` scanner's historical one and is deliberately NOT
// skipTag(): a malformed or multiline `<img` resumes just after its NAME, so a
// raw-text opener spelled on the following line is still seen. It is spelled
// here rather than duplicated at the two call sites.
//
// @p_nameEnd is the index just past `img`. On success @p_attrs is filled and
// the returned index is just past the closing '>'.
int resumeAfterImgTag(const QString &p_text, int p_nameEnd, int p_baseOffset,
                      QVector<HtmlAttr> &p_attrs, bool &p_parsed);

} // namespace htmltag
} // namespace vte

#endif // VTE_HTMLTAGPARSE_H
