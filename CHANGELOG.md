# Changelog

## 9.0.0 (unreleased)

### Breaking changes

- Require Qt 6.9+, CMake 3.21+, Perl and Python 3. Building the upstream syntax indexer
  requires C++20; public library headers require C++17. Qt 5 support is removed.
- Replace the 2019 KSyntaxHighlighting engine with upstream 6.30.0. Rebuild downstream
  consumers against the new engine and export definitions; VTextEdit SOVERSION is 9.
- Markdown code blocks now use native KSyntaxHighlighting by default. Hosts providing
  external highlighted HTML must set `m_webCodeBlockHighlighterEnabled = true` before
  constructing the editor. The embedded repository initializes automatically on first use;
  register custom syntax/theme search paths before constructing editors.

### Features and compatibility

- Keep the standalone `VSyntaxHighlighting` target without KDE Frameworks or ECM, with
  generated public headers and correct static/shared exports.
- Stage the configured Qt Core runtime beside the native Windows syntax indexer, fixing
  Qt Creator/MSVC Debug catalog generation when Qt's `bin` directory is absent from `PATH`.
- Refresh the project DLL beside both Windows demo targets on every demo build, including
  builds that do not relink the executable, so stale deployed libraries cannot mask native
  code-block highlighting and other library updates.
- Embed the current syntax catalog, including generated grammars, and remove the demo's
  obsolete XML copy. Custom syntax search paths and both public header include styles remain.
- Add native PlantUML, Flowchart.js (`flowchart` / `flow`) and WaveDrom / WaveJSON grammars.
  Reuse the bundled Mermaid, Graphviz, LaTeX and Typst definitions; native language tags
  now fall back to case-insensitive catalog names and aliases after extension lookup,
  preserving host extension overrides. Mermaid flowcharts still use `mermaid` / `mmd`.
  Recognize Graphviz's `.gv` extension alongside `.dot`; diagram rendering is unchanged.
- Preserve `Repository::themeFromFile()` as an eager snapshot, including rejection of
  malformed files, and retain the named `Default` theme. Upstream default theme selection
  uses Breeze Light/Breeze Dark.
- Enrich the named `Default` syntax theme with current Breeze Light base styling and
  CSV/TSV column colors, plus INI/Python/Rust/XML/Odin semantic overrides adapted to its
  classic light palette. Modern editor-color roles retain the white background and
  selection color, with amber warning markers and softer separators.
- Grow the editor's format cache for upstream format IDs beyond the former 256-entry limit.
- Replace Qt 5 CI rows with Qt 6.9.3 and retain the Qt 6.10.3/Typst matrix.

## 8.0.0 (unreleased)

### Breaking changes

- `MarkdownEditorConfig` and `Theme` grow for inline concealment. Rebuild all consumers;
  the library SOVERSION is now 8. Matching Qt Gui private development headers are required,
  and the runtime must use the Qt build against which the library was compiled.

### Features

- Highlight tagged Typst raw/code blocks with the selected syntax-highlighting theme,
  retaining the editor's raw font, size and background. Preserve grammar/CST distinctions
  for control flow, imports, types, built-ins, constants, literals and documentation;
  syntax-theme decorations include explicit false resets. Reuse the preview engine's
  built-in grammars for multiline/inline snippets, including Typst markup/code/math,
  without evaluating document customizations. Tokens retain authored source positions
  and remain available when evaluation fails; theme changes and zoom preserve source,
  selection and undo history. Ordinary Typst styling still uses the editor theme.
- Add optional `TypstCompiler`: compile in-memory Typst text asynchronously to per-page
  vector SVG or native RGBA `QImage` bitmaps without an editor widget. Supports bitmap
  scaling, project imports and fonts, diagnostics, bounded all-or-nothing results and
  safe sequential reuse. Requires the matching helper with `svg`/`raster` capabilities.
- Add optional `VTypstEditor` and `TypstEditorConfig`: one source/undo history across
  in-place compiled cards, side-by-side selectable PDF and read modes, with unsaved
  project overlays, diagnostics and compiler-backed navigation. QWidget-only Qt Pdf
  previews use immutable in-memory buffers and page-local selection/copy; math copy
  returns PDF text, not source. Preview geometry excludes printer bleed.
- Add `VTEXTEDIT_ENABLE_TYPST` (default OFF), requiring Qt 6 with Qt Pdf and a helper
  built with pinned Rust 1.99.0, Typst 0.15.1 and Cargo.lock. Qt 5 remains supported
  with Typst disabled; enabling Typst on Qt 5 is a configuration error. Package downloads
  are opt-in; enabled distributions must deploy the helper, Qt Pdf and dependency notices.
- Add `typst-syntax-styles` to vtextedit themes: Typst parser kinds accept literal formats
  or `@markdown-syntax-styles:NAME` references to the same theme. Typst source highlighting
  uses `m_theme` for ordinary document tokens; custom editor themes must add the new
  section to style those tokens. Explicit font sizes follow zoom, and explicit false
  decoration flags are honored by theme format conversion. Compiled output and Markdown
  KSyntax code highlighting are unchanged.
- Add the dedicated `VTypstEditorDemo` target, starting Typst without flags. The existing
  `VTextEditDemo --typst` entry remains available; both accept optional `--typst-file`.
- Add `VTypstEditor::getHeadings()`, `headingsUpdated` and `TypstHeadingInfo`, following
  the Markdown editor's flat heading-list convention. Compiled outlines include imported
  and generated headings with evaluated titles, resolved levels and LF/UTF-16 source ranges;
  `outlined: false` entries are excluded. Input/compiler invalidation clears stale outlines.
  Requires the matching compiler helper with the new `headings` capability.

### Behavior changes

- Clicking a Typst live PDF preview element in Edit focuses its compiler-mapped source position
  without requiring Ctrl. Read-mode clicks and Ctrl-clicks stay in Read without jumping to source;
  pending source-mapping replies cannot switch back to Edit. Inactive in-place cards no longer
  restore an old scroll position over navigation. Drag selection, link activation and stale-preview
  navigation protection are unchanged.
- Move Typst mode/status/retry and PDF toolbar controls from `VTypstEditor` into both demos.
  Hosts can build their own controls through public PDF search, zoom, page navigation and
  pending-update APIs/signals. The widget retains its source/preview area and diagnostics;
  mode changes preserve PDF selection when hiding the pane transfers focus to source.
- Typst read and side-by-side PDF previews prioritize visible pages at physical screen
  resolution before off-screen prefetch. Reduced first-paint images automatically upgrade
  when old snapshots or render reservations release memory, without raising resource limits.
- Typst edits retain the last syntax highlighting and rendered in-place previews while
  parsing/compilation is pending instead of clearing the document on every keystroke.
  Cached source ranges follow edits; deleted/replaced spans and file/project identity
  changes clear the affected state. Failed compilation keeps last-good preview cards.
- Typst in-place previews now keep the last-good snapshot until all visible replacement
  page fragments are rendered. Rapid edits cancel superseded raster requests without
  showing “Preview unavailable”; snapshot geometry and cached images switch together
  within the existing memory budget, including across compiler restarts.
- Typst inline previews align to their source span's start, including in exclusive bands
  and wrapped continuations. Rows fit the remaining wrapped-line width; in no-wrap mode,
  their full source-aligned extent is reachable through the editor's horizontal scrollbar.
- Repeated Typst inline preview occurrences now appear horizontally in evaluation order,
  scaling together to fit the editor width. Block previews and page fragments within
  each occurrence remain vertically stacked.
- Shift+wheel scrolls horizontally in `VTextEditor` and `VMarkdownEditor`, including over
  the line-number gutter. Ordinary wheel scrolling and Ctrl+wheel handling are unchanged.
- Long Markdown image/link/reference destinations and HTML image `src` values display
  three graphemes at each end around three middle dots, without changing source. The default
  threshold is 20 graphemes; `m_concealElements` selects eligible URL kinds and an empty mask
  disables the feature. `Theme::ConcealedText` controls compact foreground/background.
- Hovering a concealed destination shows its full source text in a tooltip without moving
  the caret or expanding the line.
- Entering any part of a concealed range reveals it. Source cursor positions, counted Vi
  motions, selection, copy/cut, search and undo remain unchanged. IME preedit reveals its block.
- Reapplying unchanged line spacing no longer adds redundant formatting undo commands.
- The demo explicitly enables destination concealment and includes threshold, link, autolink,
  reference and offline image examples, including a base64-encoded PNG image reference.

## 2.0.0 (unreleased)

### Breaking changes

- **`TablePreview::cellFormats()` has been removed.** Table snapshots carry text
  and structure, not resolved syntax formats. `MarkdownHighlighter::getSyntaxStyles()`
  exposes the current, zoom-adjusted formats; `syntaxStylesChanged()` reports updates.

- **`VMarkdownEditor::tablePreviewVisibleRows()` and `setTablePreviewVisibleRows()`
  have been removed.** The built-in table preview no longer scrolls internally, so
  there is no visible-row budget to configure: the sheet renders at its full
  natural height and the editor scrolls past it. Remove the calls; there is no
  replacement.

  The library now carries a `SOVERSION`, so a consumer built against 1.x fails at
  link time rather than resolving against an incompatible build.

### Behavior changes

- Markdown source highlighting recognizes matched `<font color=...>` tags, including
  named/hex colors, nested tags, and multiline contents. Foreground changes preserve
  existing Markdown formatting and update on edits and undo. Tag tokens, code, comments,
  and raw-text HTML are excluded; unmatched tags do not color the rest of the document.

- Folding tracks numeric source anchors instead of retaining `QTextBlock` handles across
  edits, fixing crashes and unrelated folds after bulk deletion. Parser reconciliation
  replaces the whole folding tree atomically, preserving state only for surviving regions
  with matching extents, types and heading levels. Deleted or replaced endpoints never
  transfer their fold state to unrelated text.

- Table cells highlight their current Markdown synchronously while editing, before
  source write-back. Each realized table caches parsed units by live cell text;
  unchanged source parses and theme/zoom changes do not reparse those cells.
  Markdown-backed HTML cells use decoded comment payloads, while HTML-only cells
  remain literal. Theme changes preserve uncommitted text, selection and merged geometry.

- **The interactive table sheet is now offered only for tables of at most 300
  cells** (previously 200 000). Larger tables fall back to the static source
  rendering they already used when the limit was exceeded — nothing is lost, but
  a table which used to be editable in place may no longer be.

  The bound is a measured latency limit, not a policy choice. The sheet is now a
  `QTextEdit` hosting a `QTextTable`, which is not virtualized: every cell is a
  `QTextBlock`, the sheet renders at its full natural height, and Qt relays the
  whole table out on any change inside it. On a release build the cost is linear
  in the cell count and independent of the shape, at roughly 0.055 ms per cell —
  paid on construction, on every width reflow, and on **every keystroke**:

  | cells   | first layout | reflow   | one keystroke |
  |---------|--------------|----------|---------------|
  | 200     | 10 ms        | 10 ms    | 10.6 ms       |
  | 300     | 16 ms        | 16 ms    | 16.5 ms       |
  | 500     | 28 ms        | 29 ms    | 29.1 ms       |
  | 2 000   | 136 ms       | 140 ms   | 138 ms        |
  | 200 000 | 36 702 ms    | 36 367 ms| 36 386 ms     |

  300 cells is the last shape whose keystroke still costs about one 16 ms frame
  rather than a multiple of it. The previous `QTableView` sheet could afford
  200 000 because it was virtualized and fitted only the rows it showed.

- The table sheet behaves like a Word/OneNote table rather than a spreadsheet:
  one caret roams every cell with no edit mode, a click puts the caret at the
  exact character under the pointer, cells wrap natively, and edits are written
  back on a 1-second idle debounce (flushed immediately on cell-leave, focus-out
  and Escape). Selections are confined to a single cell.
