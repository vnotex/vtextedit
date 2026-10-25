# vtextedit Agent Development Guide

## Build/Lint/Test Commands

* **Build**: `mkdir build && cd build && cmake .. && cmake --build . --config Release`
* **Build requirements**: Qt 6.9+, CMake 3.21+ and Python 3; Qt 5 is not supported. KSyntaxHighlighting 6.30.0 builds without KDE Frameworks/ECM.
* **Format**: Use `.clang-format` (2-space indent, 100-column limit, `PointerAlignment: Right`, `BreakBeforeBraces: Attach`). A pre-commit hook at `scripts/pre-commit` auto-formats staged `.cpp/.h` files (excludes `libs/`)
* **Run All Tests**: `cd build && ctest` (after building)
* **Run Single Test**: `cd build && ctest -R test_textfolding` (pattern matching)
* **Tests are enabled by default** — `add_subdirectory(tests)` is present in root `CMakeLists.txt`
* **Test targets**: `test_textfolding`, `test_utils`

## Checked-In Perl-Generated Syntax Definitions

Normal builds and CI must not require Perl. `libs/syntax-highlighting/data/syntax/`
contains 14 checked-in generated XMLs: six PHP variants (HTML, CSS, JavaScript, JSX,
TypeScript, Mustache), five Twig variants (HTML, CSS, JavaScript, JSX, TypeScript),
DoxygenLua, XSLT and JSONC. Python/Jinja generation remains part of the normal build.

**Tradeoff: generated files no longer refresh automatically.** Whenever their base
grammars, PHP/Twig dependencies, generator scripts or `data/generators/xslt.base.xml`
change in the syntax-highlighting submodule, including during an upstream update, run
this maintainer-only command from the vtextedit root with Perl installed:

```sh
cmake -P libs/syntax-highlighting/data/generators/regenerate-perl-syntax.cmake
```

Commit the refreshed XMLs with their input changes in the submodule, then update the
parent gitlink. Never hand-edit generated XMLs; preserve their generated-file notices
and LF line endings. Rebuild the catalog and exercise the affected languages before
committing. Upstream CMake updates must not restore normal-build Perl discovery or
Perl generation rules: the source XML glob already includes these definitions, so doing
so would both restore the dependency and duplicate catalog entries.

## C++ Code Style Guidelines

### Standards & Tooling

* C++17 library/consumer interfaces; the upstream syntax indexer requires a C++20-capable build compiler. `QT_DEFAULT_MAJOR_VERSION` must be 6.
* Qt modules required by vtextedit: Core, Gui, Network, Svg, Widgets, LinguistTools, Core5Compat, and matching Gui private development headers. Standalone VSyntaxHighlighting needs only Core/Gui/Network.
* `CMAKE_AUTOMOC`/`AUTOUIC`/`AUTORCC` enabled (Qt 6 uses `qt_standard_project_setup()`)

### Naming Conventions

* **Namespace**: `vte` for all library classes. Tests use `tests` namespace
* **Classes**: CamelCase — `VTextEdit`, `TextFolding`, `TextBlockRange`. Nested classes allowed (`VTextEdit::Selection`)
* **Methods**: camelCase — `getContentsSeq()`, `isValid()`, `hasFoldedFolding()`
* **Getters**: Prefer `get` prefix for accessors returning owned data (`getTextEdit()`, `getSelection()`, `getConfig()`). Short-form accessors without prefix are used for Qt-conventional names (`document()`, `theme()`, `statusWidget()`) and boolean queries (`isReadOnly()`, `isModified()`, `isCompletionActive()`)
* **Members**: Prefix `m_` for private/protected members — `m_doc`, `m_start`, `m_config`
* **Parameters**: Prefix `p_` for all function parameters — `p_parent`, `p_config`, `p_event`
* **Pointers/References**: Right-aligned — `int *ptr`, `const QString &p_text` (enforced by `.clang-format`)
* **Static members**: Prefix `s_` — `s_instanceCount`

### Include Order

Includes are grouped by scope (separated by blank lines where applicable):

1. Own public header (`<vtextedit/vtextedit.h>`)
2. Internal non-public headers (`<inputmode/...>`, `<vtextedit/...>` other public headers)
3. Third-party library headers (`<katevi/...>`)
4. Local private headers (`"autoindenthelper.h"`, `"scrollbar.h"`)
5. Qt system headers (`<QTextBlock>`, `<QTimer>`, etc.)

Note: actual ordering varies slightly between files. Follow the pattern in the file you are editing.

### Header Guards & Declarations

* Public headers: `#ifndef VTEXTEDIT_CLASSNAME_H` — e.g., `VTEXTEDIT_VTEXTEDIT_H`, `VTEXTEDIT_GLOBAL_H`
* Internal headers: shorter guards — `TEXTFOLDING_H`, `VTE_NONCOPYABLE_H`
* Test headers: `TESTS_TEST_CLASSNAME_H`
* Forward-declare classes to minimize includes — `class QTextDocument;`, `class QMenu;`

### Qt Patterns

* `Q_OBJECT` macro for all `QObject`-derived classes
* `Q_DECL_OVERRIDE` instead of C++11 `override` keyword
* `Q_DECLARE_FLAGS` / `Q_DECLARE_OPERATORS_FOR_FLAGS` for flag enums
* `Q_ASSERT` for debug assertions
* Signals/slots with `connect()`; no `SIGNAL()`/`SLOT()` macro style
* Qt containers: `QVector`, `QHash`, `QList`, `QString`, `QPair`

### Memory & Ownership

* `QSharedPointer` for config objects and shared ownership — `QSharedPointer<TextEditorConfig>`
* `QScopedPointer` for exclusive ownership
* Raw pointers for Qt parent-child managed objects (parent deletes child)
* `Noncopyable` base class available at `src/utils/noncopyable.h` for move-only types

### Exports

* `VTEXTEDIT_EXPORT` macro (from `vtextedit_export.h`) on all public API classes
* Defined as `Q_DECL_EXPORT` when building (`VTEXTEDIT_LIB` defined), `Q_DECL_IMPORT` when consuming
* Tests compile with `VTEXTEDIT_STATIC_DEFINE` (empty macro) to link sources directly

## Immutable Markdown documents

`MarkdownDocumentPtr` shares a read-only cmark tree, source bytes, effective parse options and
heading records. Direct `MarkdownDocument::parse()` does not run editor projections. Rendering
keeps all output state local and exposes no mutable AST pointer. Standalone grammar defaults
remain unchanged; hosts opt into `viewerParseOptions()` separately from HTML output options.

`MarkdownHighlighter` retains full trees only on demand. `currentDocument()` requires an exact
current full parse/profile; partial/fast parses never qualify. `requestDocument()` joins a
scheduled/in-flight full parse, and `documentReady` carries the highlighter's generation, not a
host buffer revision. Replacing source/profile or disabling retention releases accepted trees;
workers retain their own immutable inputs until completion. Existing metadata can remain valid
without retaining a tree, allowing hosts to perform a direct render-only parse.

Heading records include zero-based source lines and rendered order. Unreferenced footnote
headings have no rendered rank and must not reserve reader slugs. Shared title extraction is
iterative so deeply nested inline formatting does not overflow the native stack.

## Empty Heading Metadata

`md::HeadingInfo::m_isEmpty` records that the cmark heading has no inline AST children.
Bare ATX markers, whitespace-only contents and closing-marker-only contents are empty.
Do not derive this flag from `m_title`: images and inline HTML can have an empty display
title despite containing source content. Parsing, highlighting and heading publication
retain empty headings; the host decides whether to number them.

## Session-Owned Heading Source Numbers

`setHeadingSectionNumberProvider({})` and `setHeadingSectionNumberingActive(false)` only
suspend numbering; neither removes source prefixes. Hosts restoring numbers must clear the
provider first, then call synchronous `removeHeadingSectionNumbers()`, even for a hidden editor.

Removal restores only unchanged prefixes actually written in this editing session. Overwritten
authored prefixes are restored byte-for-byte; already-matching/manual prefixes stay untouched.
Renumbering retains the original prefix. Ownership follows title and surrounding edits, but
replacement, cut/paste or reordering of a tracked span loses its lineage; known undo checkpoints
can restore it. Loading/reopening starts without ownership. No provenance is persisted.

`Removed` is one standalone undo step preserving mapped selection and viewport. Immediate undo
restores source and ownership. `Unchanged` makes no source/history/dirty change. `Unavailable`
must be reported by the host, never retried asynchronously against hidden content. Cleanup rejects
read-only, busy/reentrant, IME composition, unsupported additional selections, and an owned edit
without undo support. Bare ATX markers retain a generated separator if a title was later authored.

The prefix-only ledger processes undo/redo even without a provider; it never snapshots documents.
Qt reset notifications can arrive while its cursor registry is detached: defer history-clear
callbacks and ignore synthetic terminal insertion before the safe reset notification releases
owned cursors. Coverage: `testHeadingSourceRemoval*` and `testHeadingSourceGuaranteeReset`.

## Single-Line Display Math

Single-line `$$...$$` is inline source syntax throughout the document, including pipe-table
cells, paragraphs and lists. It uses display-style math typesetting but inline preview
placement. Multiline `$$` blocks retain block placement; formulas never span table cells or
source lines through the inline parser.

cmark represents both `$...$` and single-line `$$...$$` as `CMARK_NODE_FORMULA_INLINE`;
`cmark_node_get_formula_display()` distinguishes the typesetting mode. Its source positions
exclude one or two delimiter characters respectively, which the AST walker restores.
`MathElement::m_block` records source structure independently of `m_display`, and
`PreviewBuilder::createMath()` takes explicit placement. Whole-document and cell-local
parsing must agree on source ranges so preview decorations never leak into saved Markdown.

## Inline Markers Over a Multi-Line Selection

`MarkdownUtils::typeMarker` (bold, italic, strikethrough, mark, inline code, inline math) applies
**one marker pair per line** when the selection crosses blocks — never a single outer pair, which
would put the marker before a `1.` / `-` / `>` / `#` and destroy the block structure.

Contract (`typeMarkerOnLines` / `markerRangeOfBlock` in `src/utils/markdownutils.cpp`):

* **Prefix aware** — leading indentation, list / todo / quote / heading prefixes (including a
  heading's auto-generated section number, `c_headerRegExp` groups 4-5) and trailing whitespace
  stay *outside* the markers. Try `c_todoListRegExp` before `c_unorderedListRegExp`; both match
  `- [ ] x`.
* **All-wrapped means unwrap** — the pair is removed only when *every* affected line is already
  wrapped. A mixed selection normalizes (wraps the rest) instead; `Ctrl+Z` is the way back.
* **Blank lines are skipped** and do not count toward "all wrapped". If no range survives, the
  document is not touched at all (no empty `beginEditBlock`, no revision bump).
* **Edges honor the selection**; a selection ending at column 0 of a block excludes that block.
* **Selection restoration is syntax-inclusive** — the markers end up selected, so pressing the
  same action again unwraps and a different one nests outside. The start cursor uses
  `setKeepPositionOnInsert(true)`, the end cursor keeps the Qt default.
* Edits run in **reverse document order**, end marker before start marker, inside a single
  `beginEditBlock`/`endEditBlock`.

Regression coverage: `testMultiLineMarker*` in `tests/test_markdowneditor/`.

## Image References: Markdown AND HTML

An image reference is either a Markdown `![alt](dest "title" =WxH)` link or an HTML
`<img …>` tag. Both are reported by `MarkdownUtils::fetchImageLinks()` (snapshot) and by
`md::walkAndConvert()` → `buildImageLinks()` (live editor), and both carry a `Syntax`
enum — `MarkdownLink::m_syntax` and `md::ImageLinkInfo::m_syntax`.

### One scanner, and only one

`src/include/vtextedit/htmlimgscanner.h` is **the only place in the tree allowed to
pattern-match `<img` in note source.** Both consumers call `scanHtmlImgTags()`, so the
snapshot and live paths cannot drift. `tests/utils/test_image_parser_drift.cpp` in the
VNote repo is the grep gate. Scanners over *rendered* or *clipboard* HTML
(`WebViewExporter`, the paste path) are a different problem and are exempt, with a
line-local `// image-parser-allow:` hatch.

Escaping lives in one place too: `htmlEscapeAttrValue()` and `spellHtmlSrcAttr()`.
`MarkdownUtils::generateImageTag()` is the single generator, and
`generateImageLink(title, url, alt, w, h)` delegates to it whenever a size is given
(Markdown's `=WxH` is understood here but by few other tools, so a sized image is
emitted as HTML for portability).

### Span conventions

| Field | Markdown | HTML |
|---|---|---|
| `m_regionStart/End` | the whole `![…](…)` construct | the whole `<img …>` tag |
| `m_urlStart/End` | the RAW destination as spelled | the `src` attribute **VALUE**, quotes EXCLUDED |

**Never** measure a replacement with `m_urlInLink.size()` — it is the decoded value. A
caller replacing a whole `src` attribute (not just its value) must re-scan the region
with `scanHtmlImgTags()` and take the attribute span: an unquoted `src=old.png` renamed
to a name containing a space would otherwise split into two attributes.

### D8 — single-line tags only

A tag is recognised only when it opens and closes within **one** source line. A multiline
`<img …>` is ignored, exactly as before the feature existed. This is what lets both paths
slice raw source without mapping container prefixes: a `> ` or list indent can then only
ever appear *between* tags, never inside one. A test asserts a multiline tag is ignored,
not mis-parsed.

### D9 — never regenerate a tag you did not author

An existing HTML image is rewritten **attribute-locally** (replace the whole `src="…"`,
or the `width`/`height` attributes). Regenerating the tag would silently destroy
`class`, `style`, `data-*`, `loading` and anything else a user wrote. Whole-region
regeneration is reserved for Markdown links and for an explicit, gated conversion.

### Node span resolution and raw text

`resolveHtmlNodeSpan()` (`src/markdowneditor/cmarkadapter.h`) is the single resolver.
Columns are never trusted — cmark strips each line's container prefix independently but
inline parsing gets one `block_offset` from the paragraph, so a lazy continuation line
shifts every reported column.

* `HTML_BLOCK` — sliced by line only. `end_line` is *also* not trusted: cmark reports the
  last line consumed **before** the end condition matched, so `<script>…</script>` reports
  the line before `</script>`. The line count is taken from the literal instead. The
  literal must never be compared for equality; it is not a contiguous copy of the source.
* `HTML_INLINE` — verify the reported span against `cmark_node_get_literal()`, else search
  the reported start LINE for a unique occurrence, else skip. Fail safe, never a guess.

Raw-text elements (`script`, `style`, `textarea`, `title`) are suppressed: an `<img>` in a
JS string is text. cmark emits an opening tag, the contents and the closing tag as
**separate** nodes, so `RawTextState` is threaded *through* the scanner and must be
advanced for **every** HTML node — including one whose span could not be resolved.
Otherwise an unresolvable `<script>` would unmask an `<img>` inside it.

## Pipe Tables After Paragraph Text

A valid pipe-table header/delimiter can interrupt paragraph text without a blank line,
including inside lists and blockquotes. cmark matches the final pending paragraph line;
earlier lines remain a separate paragraph in the same container. Preserve each row's
source columns and the preceding paragraph's end: previews and source formatting use
these ranges to rewrite only the table. Code blocks and disabled extensions are unchanged.

Regression coverage: cmark's `parser_test/table_tests.c` and
`testTableInterruptsListParagraph()` in `tests/test_markdowneditor/`.

## HTML `<table>`: a second first-class reference

A top-level `<table>` HTML block in a strict canonical subset is a
`PreviewElementType::Table`, so the interactive table sheet binds to it exactly as it
does to a GFM pipe table. A table holding a merged cell is written back **as HTML**,
because pipe syntax cannot express `colspan`/`rowspan`.

### One scanner, and only one — again

`src/include/vtextedit/htmltablescanner.h` is **the only place in the tree allowed to
pattern-match `<table` in note source.** `tests/test_markdownparser`'s
`testSingleTableScannerGate()` is the grep gate; a *generator* (spelling a fresh tag) is
exempt with a line-local `// html-table-allow:` hatch.

The `<img>` and `<table>` scanners share one attribute lexer, `src/utils/htmltagparse.h`
— the layer *below* the single-scanner rule, so "what an attribute is" is also spelled
once, along with `resumeAfterImgTag()`, the one rule for where a top-level lexer resumes
after an `<img` opener. They run over the same slice with **independent copies** of
`RawTextState`, and `extractHtmlNode()` in `markdownastwalker.cpp` asserts the two
outgoing states agree. That is why the table scanner advances its lexer **one tag at a
time** instead of jumping over a table it just captured, and why it reproduces the
`<img>` scanner's resume rule verbatim: a malformed or multiline `<img` resumes just
after its name, never past a newline, or one scanner ends up inside a `<script>` the
other has left.

### The canonical subset

A table is captured only when **all** hold; anything else is dropped whole and renders as
plain source:

* it is a whole `CMARK_NODE_HTML_BLOCK` that is a **direct child of the document**, and
  whose lines carry no container prefix. The ancestry test is structural on purpose: a
  list item's continuation indent *is* whitespace, so a source-prefix check alone would
  admit a table under `- item`;
* no `<caption>`, `<colgroup>`, `<thead>`, `<tbody>`, `<tfoot>`, no nested `<table>`;
* every tag opens and closes within one line — **closing tags included** — and every
  cell's inner source lies on one line;
* all tags are explicitly balanced: a cell's content is walked with a tag stack, void and
  self-closing elements aside, and a raw-text element inside a cell is refused outright
  (a `</td>` spelled inside a `<script>` is text, so the cell's extent would depend on
  which scanner was asking);
* either row 0 is entirely `<th>` and every later row entirely `<td>`, or every row is
  entirely `<td>`;
* each column has at most one distinct `align` among the cells declaring one;
* the `colspan`/`rowspan` grid tiles a rectangle **exactly** — no gap, no overlap, no
  overflow.

Every dimension bound is checked by **subtraction against a small ceiling**, never by
adding a span to an index. `colspan="2147483647"` is a valid positive `int`; a
`col + span > limit` test would overflow, pass, and then drive a loop over billions of
slots — a hang and an unbounded allocation from note source alone.

A refused table also suppresses any table nested inside it, so a refusal never degrades
into capturing the inner one.

### The fold region comes from the scanner, not from the block node

A `<table>` opens a CommonMark **type-6** HTML block, which is terminated only by a blank
line or by EOF — *never* by `</table>` — and whose reported `end_line` is not even that,
since `resolveHtmlNodeSpan()` deliberately extends past it. A fold derived from the node
would therefore run to the end of the document. `extractHtmlTables()` emits a
`FoldingRegionType::Table` region from `m_tableStart`/`m_tableEnd` instead, which stop
precisely at `</table>`.

That region is also what makes the sheet's source auto-fold at all:
`MarkdownFoldingProvider::applyPreviewAutoFold()` iterates **folding regions** and matches
a preview whose live extent equals one exactly, so an element with no region of its own is
never even visited. `addFoldingRegion()` has no `STYLE_HTMLBLOCK` arm and must not grow
one.

### Cell source lives in a comment

A cell this tree authors is `<!--vte-md:PAYLOAD-->` followed by the HTML cmark renders
from that payload. The sheet shows and edits the payload; **the comment always wins**, so
a hand edit to the rendered half is destroyed at the next commit anywhere in the table.
The codec is `\` → `\\` then `-` → `\-`, which makes `--` — and therefore `-->` —
unrepresentable.

Backing is **per table, never per cell**: a table is Markdown-backed if any cell carries a
payload, so a comment-less cell in one is *also* read as Markdown and re-rendered. One
malformed payload makes the **whole** table HTML-only, whose cells are then literal text
both ways and for which no comment is ever synthesized.

### Fail closed, always

A commit whose output the scanner then refuses is **unrecoverable in place**: the source
is replaced, the re-parse finds no table, and the user is left with raw HTML and no sheet.
Every serializer path therefore returns an empty string — which `flushPendingCommit()`
already reads as a rejection — rather than emitting something questionable. Two places
this can happen:

* `TablePreviewHtmlSerializer::renderCellHtml()` must produce **one line**. cmark
  terminates every block with a newline, and one line of Markdown can render to multi-line
  HTML (`- x`, `# x`, four leading spaces). A single wrapping `<p>` is stripped, trailing
  newlines are stripped, and every remaining separator becomes `&#10;` — never a space,
  which would silently rewrite `x\n` to `x ` inside a `<pre>`.
* `TablePreviewDocument::mergeCells()` verifies the resulting geometry and, on a mismatch,
  **rebuilds the document from a pre-merge snapshot**. An edit block is not a transaction
  and this document has no undo stack, so recovery is the only rollback available. The
  containment preflight in `canMergeCells()` is what makes it unreachable:
  `QTextTable::mergeCells()` silently refuses a request whose edge cuts a cell.

The last line of defence is that `TablePreviewHtmlSerializer::serialize()` hands its own
finished output back to `scanHtmlTables()` and refuses anything but an exact match on the
row count, column count, header shape, every origin's box and the backing classification.
Nothing else can cover an HTML-only table, whose cell text is written **verbatim**: a cell
edited to hold `</td>` or an unbalanced raw-text tag would otherwise reshape the table.

### Tags are preserved verbatim

`<table>`, `<tr>`, `<td>`/`<th>` keep their authored attribute text. Only `colspan`,
`rowspan` and `align` are rewritten **attribute-locally**, through
`rewriteHtmlTagAttr()` — never by regenerating the tag, which would destroy `class`,
`style`, `data-*` and anything else a user wrote. Only an inserted row or cell, or a slot
newly exposed by a split, gets a generated tag. This is D9 applied to tables.

### The sheet is a logical grid

`TablePreview` keeps `cells()`, `columnCount()`, `rowPrefixes()` and `delimiterPrefix()`
with their exact historical meaning and raggedness. The **grid** — `gridRowCount()`,
`gridColumnCount()`, `isOrigin()`, `rowSpan()`, `colSpan()` — is a separate, always
rectangular view. A pipe table is the degenerate case where every slot is a 1×1 origin.

`QTextTableCell::row()/column()` report the **origin's** coordinates for every covered
slot. That is what makes `isOrigin()` work, and equally why they can never answer "which
half of a colspan was clicked" — the context menu resolves the logical column
geometrically instead.

The single selection clamp is no longer the frame-safety gate. A cell rectangle survives,
for Merge and for the copy actions, so `collapseComplexSelectionForMutation()` is what
every text-mutating path goes through — including the standard context menu's Cut and
Delete, `cut()`/`paste()`, drop, and internal move-drag, none of which reach
`keyPressEvent()`. **External** move-drag reaches none of them either: `mousePressEvent()`
collapses a rectangle the press lands inside, before Qt can arm a drag at all. The
inherited bulk mutators (`clear()`, `setPlainText()`, `setHtml()`, `append()`, …) are not
selection-mediated and are hidden and refused separately; `isIntact()` is the backstop.

Once written back as HTML a table **stays** HTML, even after every merge is split. There
is no in-editor route back to pipe syntax.


## Typst Native Runtime

`TypstRuntime` is application-owned and explicitly injected through
`TypstEditorConfig::m_runtime`; construct it after the Qt application and before consumers.
The Rust cdylib is directly linked through C ABI 1, with opaque lane-confined sessions
and independently owned replies released by Rust. Raster QImages retain immutable Rust
reply storage without a pixel copy and detach on writes; bitmap results may outlive their
compiler and runtime (`compilerBitmapOutlivesRuntime`). There is no helper executable or PATH
lookup. Two serial compile/render lanes and one scanner lane bound native concurrency.

A soft timeout retires delivery, not a running native call. Keep its session epoch/handle
until actual return; Retry records intent and never overlaps or replaces active work.
Rebinding advances GUI generation while retaining the in-flight identity. Editor/compiler
destruction is nonblocking; runtime shutdown cancels queued work, drains active calls,
and joins workers without GUI acknowledgement. A stuck call may delay shutdown indefinitely.
Do not unload the library while workers/reply owners remain. Catch unwinding panics at
every C boundary and poison affected sessions; native aborts/faults remain process-wide.

The parse-only scanner owns its inputs and uses a separate bounded wait with no event
pumping. Timeout or an occupied abandoned scan fails closed without another worker.
Only complete scans can authorize obsolete-image deletion. Keep Rust allocation ownership
inside the library, noexcept C++ callbacks, and context-bound queued GUI delivery.
Watch existing dependencies directly, not their parents: unrelated notebook directory
notifications must not revoke current resources. Missing paths use the nearest existing
ancestor watch; `diskDependenciesRecoverAfterReplacementAndRecreation` covers file
replacement, deletion, retained last-good output and creation recovery.

Typst font lookup includes configured `fontPaths`, bundled fonts, then installed system
fonts (earlier entries win equal matches). This applies to preview and export without
rewriting authored text styles. The process shares one system catalog and lazily loads
font data; installing or replacing system fonts requires restarting the host. Loaded faces
remain owned by retained snapshots. Configured font paths retain their bounded reads and
dependency watches; system fonts are shared process assets, not watched project inputs.
CJK output requires an installed font covering the characters. Coverage: the conditional
`system_fonts_shape_chinese_when_available` glyph test and the deterministic
`explicit_font_precedence_and_frozen_fallback_survive_reset` regression.

## Typst Raw Code Highlighting

`tools/typst-backend/src/highlight.rs` extracts embedded tokens from literal Raw CST nodes,
using direct Text children as authored byte ranges (the lexer owns trimming). Foreign
languages reuse `typst::text::RAW_SYNTAXES` and per-block Syntect state; `typ`/`typst`,
`typc` and `typm` use the official markup/code/math parsers with a multiline source map.
Never evaluate document-defined languages/themes/syntaxes, scan fences separately, or
recursively highlight nested raw examples. Unknown/untagged/empty raw stays ordinary Raw;
unfinished fences retain the official parser's Error styling.

`parse_metadata` emits outer Raw before `raw.<kind>` tokens, through the existing UTF-16
wire mapping and metadata budget. Parsed and compiled replies share the ranges even when
evaluation fails; export's `previewMetadata: false` still bypasses them. Parser/scope or
resource errors reject metadata rather than publishing a partial result.

`RawKind` retains native Tag fallbacks plus control flow, import, data type, built-in,
constant, decimal/base/float, character, verbatim/special string, documentation, attribute
and preprocessor categories. Foreign classification uses preinterned scope prefixes,
innermost first; only precise literal boundary scopes consult enclosing literal families.
Do not infer built-ins/constants/types from names: Rust's ambiguous `storage.type.rust`
remains Keyword. Typst snippets refine official CST tags (including associated Hash tokens)
without changing ordinary document tags. Generic/unit-bearing `raw.number` remains distinct
from `raw.decval` on the wire, but both use KSyntax's DecVal palette slot.

`TypstHighlighter::refreshTheme` keeps ordinary Text + authored Typst formats. Embedded
formats instead start with Text + authored raw, then use `TextEditorConfig::m_syntaxTheme`
through the initialized shared KSyntax repository (file path or name; invalid/empty selects
the default). Cache every supported wire kind even without a `typst-syntax-styles` section.
Token foreground falls back to Normal, then raw; absent token background retains raw.
Resolved syntax-theme booleans explicitly replace all four decorations, with semantic
bold/italic/underline for markup. Never apply syntax editor/selection colors to the block.
Raw alone owns embedded font/size, with positive size zoomed once and clamped to 2; otherwise
size inherits the editor font. Ordinary token fonts/sizes never enter embedded formats.
Theme/zoom refresh and asynchronous range mapping must not change source, selection, dirty
state or undo history. Coverage: inline Rust extractor tests,
`raw_highlights_survive_evaluation_errors`, `rawCodeHighlightsUseSyntaxTheme` and
`rawCodeHighlightsFollowEditsAndErrors`.

## Typst Text Theme Defaults

`Theme::typstTextStyle()` exposes the sparse `typst-editor-styles.Text` format.
`TypstSourceEditor` merges it property-wise over `editor-styles.Text` in a private config/theme
copy; never mutate the canonical shared/default theme. Reconfiguration restores inherited
widget styling when an override is removed and preserves source zoom, content and history.
Syntax formats inherit the specialized Text style; Markdown and ordinary editors do not.

The bridge sends only explicitly configured `font-family` and positive integer `font-size`
as `textFontFamilies` and `textFontSize` in both Preview and Export modes. Font families are
trimmed once when parsing the theme; keep the full ordered list, not Qt's selected family.
Missing font properties retain native Typst defaults in compiled output, not the generic
source-editor font. Source zoom and the preview-color toggle never change document fonts.
The Rust library defaults sit below authored set/show/constructor styles. Changes/removal
invalidate the library without reloading the font store; retained snapshots keep old defaults.
`TypstCompiler` exports use the supplied theme's document fonts. Coverage: `textStyleOverridesMergeWithoutSharedMutation`,
`textFontDefaultsMatchAuthoredStyles`, `textFontThemeSwitchAndZoomPreserveSource`, and the
bundled-font frame/export regressions in `world.rs`.

## Typst Preview Theme Defaults

`TypstEditorConfig::m_enableThemeStyleInPreview` defaults to true. When enabled, valid
`m_pdfBackground` and `m_previewForeground` supply default page/text fills for both PDF
and in-place previews. Invalid colors retain native Typst defaults. The canvas always
uses `m_pdfBackground`, independently of the flag.

Only the Preview bridge sends these colors. The backend installs them in `Library.styles`,
below authored set/show/constructor styles, without rewriting source or recoloring images.
Explicit black, white, and transparent page fills retain their meaning. Theme changes and
opt-out rebuild the library only when defaults change; retained snapshots keep their old
library. `TypstCompiler` uses Export mode and never sends preview colors, even when given
the editor's themed config; the Text font defaults above still apply. Regression coverage:
`previewThemeDefaultsRespectAuthoredStyles`, `previewThemeSwitchAndOptOutPreserveSource`,
and `exportsIgnorePreviewTheme` in `tests/test_typsteditor/`.

## Typst Different-Document Rebinding

Hosts retaining a `VTypstEditor` across file identities call `resetForNewDocument()` before
changing project/source, then `finishDocumentReset()` only after all incoming identity and text
are installed. Reset closes compiler admission before clearing source/history/find, diagnostics,
outline, in-place cards and displayed/pending PDF selection/navigation. Project/active-file setters,
source notifications, watchers and Retry cannot reopen admission during this interval. A failed
incoming source load can leave it closed; ordinary same-document edits keep last-good output.

The native/source widgets, compiler session and user preview/zoom preferences survive. In-flight
native requests retain their IDs and session epoch until actual return; no new request overlaps
them. An obsolete timeout retires delivery without force-stopping native work. Current-generation
failure and explicit Retry also wait for return before recreating the session. Retired render
slots own outstanding renderer callbacks until completion but
can never publish or paint the outgoing PDF. Bind completion clears load-time formatting history.
Reset also revokes both displayed resource IDs before notifying consumers. Queued image requests
complete as cancelled; in-flight image replies retain native ownership until return and are rejected
once their snapshot is no longer retained. Ordinary source edits still allow in-flight reads of a
retained displayed image. Resource invalidation may synchronously destroy the editor, so reset must
not retain rollback guards referring to its members across that notification.
Coverage: `documentResetClearsPublishedState`, `documentResetRetiresPendingPdf`,
`documentResetDrainsTimedOutRequest`, `resourceRequestsAcrossDocumentReset` and
`documentResetInvalidationCanDestroyEditor` in `tests/test_typsteditor/`.

Viewport anchors do not survive whole-document source loads. `TypstPreviewHost` discards them
on safe clear/load notifications, including first load into an empty editor, but must ignore
Qt's synthetic empty-paragraph insertion while its cursor registry is detached. Navigation
supersedes a pending layout anchor with the actual caret and a visible vertical position;
ordinary caret movement must not start a new deferred scroll. `openingPreservesSourcePosition`
covers first load, immediate line restoration, rebinding and same-document reload.

## Typst PDF Camera State

`VTypstEditor::PdfZoomMode` (`Custom`, `Width`, `Page`) is shared with the internal PDF pane.
`pdfZoom()` and `pdfZoomMode()` expose its actual camera; `pdfZoomChanged(qreal, PdfZoomMode)`
forwards native changes from setters, fit actions, Ctrl+wheel, context menus, relayout and PDF
publication. A same-factor mode change notifies; an unchanged factor/mode pair does not.
Notifications follow complete camera updates. Snapshot-driven notifications wait until the
pane's render-slot service finishes, so synchronous consumers may change zoom or configuration.

Fit choices made before any PDF is displayed remain selected and are applied when the first
snapshot is published. `setConfig()` refreshes resource/theme settings without resetting the
camera: `m_initialZoom` applies only during construction. Edit/Read and live-preview switches
retain the mode; fit scales follow the resulting viewport size. PDF camera changes never zoom
the source font or mutate source text, dirty state or undo history. Hosts should restore the
camera before connecting persistence and store the mode explicitly rather than infer it from
the numeric scale. Regression coverage: `pdfZoom*` and `pdfFitBeforePublication*` in
`tests/test_typsteditor/`.

## Typst PDF Context Menu

Edit/live preview and Read share Copy, Select page text, Clear selection, Fit width and
Fit page as native defaults. Resource actions augment, rather than replace, these actions.
Selection-dependent actions keep the same enablement in both modes.
Clearing the selection publishes pending preview updates. The PDF canvas has no top-left
informational overlay; preview errors and compiler diagnostics remain visible.
Keyboard shortcuts and host PDF APIs remain available independently of the context menu.

## Typst Resource Contexts

`VTypstEditor::resourceContextMenuRequested` augments the existing source menu and the
PDF's native Copy/selection/fit menu; in-place cards expose the same typed context.
`TypstResourceContext` can contain both an image occurrence and a link. Source contexts
use current evaluated UTF-16 ranges, excluding comments/raw examples; preview contexts
use their displayed snapshot's transformed/clipped geometry. Cards map their own displayed
parts back to page points, so a nested image is not confused with a figure/table crop.

The epoch/generation/snapshot plus image ID/link index identify compiler-owned records.
Copied destination/source/editability fields are presentation only: `openContextLink`,
`requestImageData`, and `resizeImage` resolve the real record again. Read-only and stale
but retained displayed images remain readable. Source edits require the current writable
active file and an official AST resize plan; aliases, computed/relative dimensions,
ambiguous evaluations, imports, and stale source cannot be rewritten. Resize changes
only named values/additions, preserves comments/other arguments, converts 96px to 72pt,
uses `auto` when clearing an existing axis, and creates one native Undo step. Unchanged
values and absent empty dimensions do not edit source.

`m_linkNavigable` advertises whether Open can safely fulfill the captured target. URI
dispatch remains available on retained snapshots. Internal navigation requires that
snapshot's displayed PDF or the current latest compilation; a stale in-place target is
never reinterpreted on another PDF. Its link index remains present, preventing a linked
image from turning into plain-image double-click activation. Open rechecks the real record
after presentation changes; callers must not infer navigability from a copied page number.

Only Preview requires the `resources` capability and bounded `images`/`sourceLinks`
sidecars. Export accepts empty/absent resource arrays. `imageDataReady` completes each
admitted request once, asynchronously, with retained original passive image bytes or a
standalone PNG of the full retained image; it never rereads a changed file or crops a
page. Cancellation/error completions are explicit. Consumers must still check their own
request/view lifetime. `resourceContextsInvalidated` closes menus and revokes host actions
on generation/lifecycle changes. Snapshot publication does not broadcast global invalidation:
only a menu on the retiring displayed surface closes (in-place notifications are queued).
Current source menus, including non-resource menus, survive unrelated preview handoffs.
Plain-image double-click emits `imageActivated`, while linked images keep link gestures.

PDF and in-place publication are independent: retention keeps their two displayed IDs
plus the latest completed snapshot (at most three), dropping the superseded newest before
another compile; a failed compile never resurrects the dropped newest snapshot. A hidden
in-place surface retains its helper ID while its cached displayed frame can reappear, until
clear or handoff. This finite third document/world increases resident compiler memory; it
is not covered by the payload budget. Payload caching still shares the existing global
byte budget. Heap popups retain their source/pane/card parent to preserve logical host
focus. PDF/card menus use guarded cleanup; the standard source menu clears its scoped
owner on destruction without deleting twice. No pointers into replaceable geometry survive
menu execution. Regression coverage: `resourceContext*`, `resourceImageData*`,
`resourceIndependentSnapshotRetention`, `resourceInternalLinkNavigation`, `resourceResize*`,
and `resourceMenusSurviveDeletion` in `tests/test_typsteditor/`.

## Translation Catalogs

`src/data/translations/` contains Simplified Chinese (`vtextedit_zh_CN`), Japanese
(`vtextedit_ja`) and Korean (`vtextedit_ko`, locale `ko_KR`) catalogs. The explicit
`update_translations` target refreshes all three TS files; translate unfinished entries
before releasing. VNote packages the checked-in QM files, so regenerate them with Qt 5.15
`lrelease` and commit both TS and QM changes (Qt 6 can load these files). The host installs
the `vtextedit` translator with its chosen `QLocale` before constructing editors.

## Testing

* Framework: QtTest (`#include <QtTest>`, link `Qt::Test`)
* Test classes inherit `QObject`, test methods are `private slots`
* Entry point via `QTEST_MAIN(tests::TestClassName)` at end of `.cpp`
* Assertions: `QVERIFY()`, `QCOMPARE()`, `Q_ASSERT()`
* Setup/teardown: `initTestCase()` / `cleanupTestCase()` (once), `cleanup()` (per test)
* Tests live in `tests/test_<name>/` with their own `CMakeLists.txt`
* Each test compiles needed source files directly (no library dependency) with `VTEXTEDIT_STATIC_DEFINE`
* Shared test utilities in `tests/utils/`

Native editor fixtures must establish Qt's active top-level window after showing it;
`activateWindow()` alone depends on a window manager that bare Xvfb does not provide.
Compare configured and rendered font families through `QFontInfo`, not raw generic-family
labels: Qt's stylesheet font can capitalize an equivalent alias differently from the database.
After replacing source, raster fixtures must place the text cursor at the observed source before
aligning horizontal scroll; deferred Qt caret visibility can otherwise rewind the viewport to
its old cursor and clip an already-rendered preview. When replacing with overwide source,
wait for the horizontal scrollbar's nonzero range before alignment: document layout width
alone does not establish that the scrollbar can accept the requested offset.
Raster-request observers and their callbacks must be installed before admitting the source
whose first request they assert: native show/activation can pump events inside fixture
construction. A current preview does not prove a late subscriber saw its startup request.
Keep epoch/snapshot and actual card geometry/color counts in timeout diagnostics; do not
replace raster assertions with the compiler's up-to-date message.

## Architecture

### Overview

vtextedit is a reusable Qt widget library providing rich text editing components for [VNote](https://github.com/vnotex/vnote). Licensed LGPL-3.0.

### Class Hierarchy

```
QTextEdit
  └── VTextEdit           (src/textedit/) — base edit widget, cursor, selection, input method
QWidget
  └── VTextEditor         (src/texteditor/) — wraps VTextEdit, adds syntax highlight, Vi mode, folding, completion
        └── VMarkdownEditor (src/markdowneditor/) — adds Markdown parsing, in-place preview
```

Note: `VTextEditor` is a `QWidget` that **contains** a `VTextEdit`, not a subclass of it.

### Source Layout

| Directory | Purpose |
|---|---|
| `src/include/vtextedit/` | Public API headers (installed) |
| `src/textedit/` | `VTextEdit` implementation — base editor, scrollbar, auto-indent |
| `src/texteditor/` | `VTextEditor` implementation — folding, syntax, completer, extra selections, indicators border |
| `src/markdowneditor/` | `VMarkdownEditor` — PEG parser, preview, document layout, code block highlight |
| `src/inputmode/` | Input mode abstraction — Normal, Vi (`katevi`), VSCode modes |
| `src/spellcheck/` | Spell check integration (Hunspell via Sonnet) |
| `src/utils/` | Shared utilities — `Noncopyable`, text/markdown/network utils |
| `src/data/` | Translations (`.ts`/`.qm`) and theme resources (`.qrc`) |
| `demo/` | Standalone demo application |
| `tests/` | QtTest-based unit tests |

### Third-Party Libraries (`libs/`)

| Library | Purpose | Source |
|---|---|---|
| `syntax-highlighting` | KDE Syntax Highlighting (KSyntaxHighlighting) | git submodule (vnotex fork) |
| `katevi` | KDE Vi input mode engine | bundled |
| `peg-markdown-highlight` | PEG-based Markdown parser | bundled |
| `sonnet` | Spell checking framework | git submodule (vnotex fork) |
| `hunspell` | Spell check dictionary backend | git submodule (vnotex fork) |

### Config Pattern

Configs are heap-allocated and shared via `QSharedPointer`:

```cpp
auto editorConfig = QSharedPointer<TextEditorConfig>::create();
auto markdownConfig = QSharedPointer<MarkdownEditorConfig>::create(editorConfig);
auto paras = QSharedPointer<TextEditorParameters>::create();
auto editor = new VMarkdownEditor(markdownConfig, paras, parent);
```

`MarkdownEditorConfig` wraps a `TextEditorConfig` (composition, not inheritance).