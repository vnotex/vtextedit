# vtextedit Agent Development Guide

## Build/Lint/Test Commands

* **Build**: `mkdir build && cd build && cmake .. && cmake --build . --config Release`
* **Build (Qt 5)**: Pass `-DQT_DEFAULT_MAJOR_VERSION=5` to cmake
* **Format**: Use `.clang-format` (2-space indent, 100-column limit, `PointerAlignment: Right`, `BreakBeforeBraces: Attach`). A pre-commit hook at `scripts/pre-commit` auto-formats staged `.cpp/.h` files (excludes `libs/`)
* **Run All Tests**: `cd build && ctest` (after building)
* **Run Single Test**: `cd build && ctest -R test_textfolding` (pattern matching)
* **Tests are enabled by default** — `add_subdirectory(tests)` is present in root `CMakeLists.txt`
* **Test targets**: `test_textfolding`, `test_utils`

## C++ Code Style Guidelines

### Standards & Tooling

* C++14, Qt 5/6 dual-support via `QT_DEFAULT_MAJOR_VERSION` cache variable
* Qt modules required: Core, Gui, Network, Svg, Widgets, LinguistTools; optional: Core5Compat (Qt 6)
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


## Testing

* Framework: QtTest (`#include <QtTest>`, link `Qt::Test`)
* Test classes inherit `QObject`, test methods are `private slots`
* Entry point via `QTEST_MAIN(tests::TestClassName)` at end of `.cpp`
* Assertions: `QVERIFY()`, `QCOMPARE()`, `Q_ASSERT()`
* Setup/teardown: `initTestCase()` / `cleanupTestCase()` (once), `cleanup()` (per test)
* Tests live in `tests/test_<name>/` with their own `CMakeLists.txt`
* Each test compiles needed source files directly (no library dependency) with `VTEXTEDIT_STATIC_DEFINE`
* Shared test utilities in `tests/utils/`

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