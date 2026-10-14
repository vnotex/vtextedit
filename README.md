# vtextedit
This project contains the key editor components for [VNote](https://github.com/vnotex/vnote), including:

* `VTextEdit`: the basic edit widget derived from `QTextEdit`.
* `VTextEditor`: a fully-functional editor for plain text, with Vi and syntax highlight supports.
* `VMarkdownEditor`: a fully-functional editor for Markdown derived from `VTextEditor`, with in-place preview support.
* `VTypstEditor` (optional): a QWidget composition for Typst source and native compiled previews.

Check the `demo` project and VNote for details.

![Main Interface](pics/main.png)

## Font colors in Markdown

The source editor colors the contents of matched `<font color=red>text</font>` tags.
`color` accepts named colors and hex values such as `#008000` or `#0f0`, with or without
quotes. Nested colors restore the enclosing color when closed; contents can span lines,
but each tag must fit on one line. Bold, italic, and other Markdown formatting is retained.
The tags remain visible, and the source is not rewritten.

Invalid or missing colors inherit a matched enclosing font's color. Unclosed tags do not
color the rest of the document. Tags inside Markdown code, comments, or HTML raw-text
elements are not interpreted. This is `color`-attribute support, not general CSS styling.

## Typst editor (optional)

`VTypstEditor` is additive: it contains a `VTextEditor`, not a Markdown editor, and uses
one source document, undo history and compiler session across three modes:

| `VTypstEditor::Mode` | Presentation |
| --- | --- |
| `InPlacePreview` | Editable source with source-attached compiled output cards. |
| `SideBySidePreview` | The same source beside a live, selectable PDF preview. |
| `Read` | Source hidden and input disabled; the same PDF fills the widget. |

Side-by-side mode synchronizes vertical scrolling in both directions, including PDF
keyboard and page navigation. Synchronization uses relative scrollbar progress, not
source-to-output mapping, so corresponding text may not line up in uneven layouts.
It preserves the source cursor, selections, focus and undo history. Horizontal scrolling
is independent, and synchronization pauses when either side has no vertical scroll range.
Read and in-place modes remain independent; mode changes, zoom and PDF publication do not
force alignment. The next scroll uses the current ranges.

Moving the source cursor in side-by-side mode also flashes an accent marker three times
at the corresponding PDF location, using Typst's source mapping rather than scroll ratios.
The first mapped location is revealed if off-screen; an already visible target does not
move the preview. Headings use their semantic document locations, so both cursor flashes
and **Show cursor in output** target the actual heading, not a copy of its text in an
outline. This also applies when the outline and heading share a page.
The marker does not change either selection, source history or focus.
Rapid cursor moves replace earlier requests. While the PDF is stale, highlighting waits
for the current compilation to be published; leaving side-by-side mode cancels it.

In-place previews have no panel background, navigation controls, title/page rows, or
internal scrollbar. Repeated inline occurrences appear left-to-right in evaluation order,
in a content-sized row aligned to the source span's start on its visual line. With wrapping,
the row scales down together to fit the remaining width to the right. In no-wrap mode,
it follows the source during horizontal scrolling and remains fully reachable.
Block occurrences and page fragments within each occurrence remain vertically stacked.
Scrolling over previews scrolls the source editor; existing layout reservations keep
previews separate from their source and neighboring content.
Clicking a preview does not select source or change modes. Authored backgrounds inside
the compiled output remain intact.

Changing modes does not reload source, clear undo or mark the document modified.
`setReadOnly()` records the host's explicit policy; `isReadOnly()` returns that policy.
Effective source read-only is also true in `Read`, and leaving it restores the explicit
policy. Programmatic source updates remain possible while the source is hidden. Use
`getTextEditor()` for source editing, find/replace and input modes, and `document()` for
the shared `QTextDocument`.

`VTypstEditor` has no toolbar: it owns the source/preview area and diagnostic list.
Both demo entry points build the mode/status/retry row and PDF controls in `demo/main.cpp`.
Hosts can supply their own controls using `setMode()`, `compileStatusChanged`,
`retryCompilation()` and `showOutputAtCursor()`, plus the public PDF API:

* `pdfZoom()` / `setPdfZoom()`, `fitPdfWidth()` and `fitPdfPage()` control magnification.
* `setPdfSearchText()` and `findNextPdfMatch(backwards)` search the displayed PDF.
* `currentPdfPage()`, `pdfPageCount()` and `goToPdfPage()` use zero-based page indices;
  before publication the current page is `-1` and the count is `0`. `pdfNavigationChanged`
  notifies hosts after vertical navigation or PDF publication. Counts describe displayed
  output, not an update still held behind a selection.
* `hasPendingPdfUpdate()`, `pdfUpdateAvailableChanged` and `publishPendingPdfUpdate()`
  let hosts offer an explicit update action without discarding a PDF selection on arrival.

Seed controls from the getters before relying on signals; initialization may have already
changed state. The demo keeps its PDF row hidden in in-place mode and blocks control signals
while reflecting mode/page changes. Mode switches preserve PDF selection even when hiding
the preview transfers focus to the source editor.

### Building and running

`VTEXTEDIT_ENABLE_TYPST` defaults to **OFF**. Disabled builds do not build the helper or
`VTypstEditorDemo`, install the Typst headers, or require Rust/Qt Pdf. **Typst requires
Qt 6**; Qt 5 remains supported for the existing editors with this option OFF. Configuring
Qt 5 with Typst enabled fails with an explicit error before looking for Cargo or Qt Pdf.
Enabled builds require Cargo with pinned **Rust 1.99.0**, the locked helper dependencies
(official Typst crates pinned to **0.15.1**), and **Qt 6 Pdf** in addition to the existing
Qt Widgets dependencies. Install matching Qt Gui private development headers,
LinguistTools and Core5Compat as required by the existing build. Use CMake 3.16 or later and
a toolchain supporting the selected Qt version (Qt 6.10 requires C++17); on Windows
use an MSVC Qt kit for Qt Pdf, not MinGW.

The preview is QWidget-only: it uses public Qt Pdf APIs, not PdfWidgets, Qt Quick/QML,
WebEngine, a browser, or a Tinymist/LSP server. The Typst CI baseline is Qt 6.10.3;
the native Windows/MSVC Qt 6.10.3 demo has been exercised. This is not a claim that
Linux or the full regression/packaging matrix has passed.

For a single-configuration Ninja build (replace the Qt prefix with your installed kit):

```sh
rustup toolchain install 1.99.0 --profile minimal
cmake -S . -B build-typst -G Ninja -DCMAKE_BUILD_TYPE=Release -DQT_DEFAULT_MAJOR_VERSION=6 -DCMAKE_PREFIX_PATH="/path/to/Qt/6.10.3/kit" -DVTEXTEDIT_ENABLE_TYPST=ON
cmake --build build-typst --target VTypstEditorDemo VTextEditDemo
./build-typst/bin/VTypstEditorDemo
./build-typst/bin/VTypstEditorDemo --typst-file /path/to/project/main.typ
./build-typst/bin/VTextEditDemo --typst
cmake --build build-typst # Build all installable targets before installing.
cmake --install build-typst --prefix install-typst
```

On Windows use the `.exe` names from a developer shell with the Qt runtime available.
Multi-configuration generators may place demos in a configuration subdirectory; build
and install with `--config Release`. `VTypstEditorDemo` starts Typst directly, without
`--typst`. `VTextEditDemo` retains its Markdown default and supports `--typst` when enabled;
it also accepts `--typst-file` after `--typst`. Without a file, the Typst demo uses an
embedded copy of `demo/data/example_files/example.typ` and an unsaved `lib.typ` import.
With a file, its directory becomes the project root and its basename the main entry;
the demo is not a project-root discovery tool.

The bundled showcase covers text and show rules, headings/outline/lists, math, tables,
figures, SVG/raster images, a computed sine graph, layout/templates, programming/imports,
CSV/JSON data, counters/state, cross-references, footnotes and citations/bibliography.
All example data is embedded; no package downloads are needed. In **In-place preview**,
try **Repeated occurrences** (red and blue formulas shown together) and **Sequential page
fragments** (scroll the editor through one multi-page equation to its final row).

CMake builds the helper with `cargo build --release --locked`, places it in the build's
`bin`, and installs it in `CMAKE_INSTALL_BINDIR`. For cross compilation also set
`VTEXTEDIT_TYPST_RUST_TARGET` to the matching Rust target triple. At runtime,
`TypstEditorConfig::m_helperExecutable` overrides lookup; otherwise the editor searches
beside the application, then `PATH`. The demos accept `VTEXTEDIT_TYPST_HELPER` as that
configuration override (useful for configuration subdirectories or bundles). Deploy the
helper with your application or set an explicit path; do not rely on a developer Cargo
cache. Installing the targets does not by itself bundle all Qt/native dependencies.

### Typst source highlighting themes

Typst elements use `TypstEditorConfig::m_textEditorConfig->m_theme`, the same vtextedit
JSON theme as the Markdown editor. Add `typst-syntax-styles` alongside
`markdown-syntax-styles`. Each entry is either a format object or a reference to an
explicitly defined Markdown style in that same file:

```json
{
  "metadata": {"type": "vtextedit", "name": "Shared"},
  "markdown-syntax-styles": {
    "H1": {"text-color": "#222222", "font-size": 17, "bold": true},
    "STRONG": {"bold": true},
    "EMPH": {"italic": true}
  },
  "typst-syntax-styles": {
    "heading": "@markdown-syntax-styles:H1",
    "strong": "@markdown-syntax-styles:STRONG",
    "emph": "@markdown-syntax-styles:EMPH",
    "keyword": {"text-color": "#0057ae", "bold": true},
    "string": {"text-color": "#bf0303"}
  }
}
```

This is a partial theme example; keep your existing `editor-styles` and other sections.
Typst keys are lowercase parser kinds: `comment`, `punctuation`, `escape`, `strong`,
`emph`, `link`, `raw`, `label`, `ref`, `heading`, `listmarker`, `listterm`,
`mathdelimiter`, `mathoperator`, `mathgroupingparens`, `keyword`, `operator`, `number`,
`string`, `function`, `interpolated`, and `error`. The parser currently has one `heading`
kind for all levels, one `listmarker` kind for list markers, and one `raw` kind for inline
and block raw text.

References use the exact, case-sensitive `@markdown-syntax-styles:NAME` spelling and
resolve when the theme is loaded. The target must be a known Markdown style with a
format object in the same file. References cannot chain or add property overrides;
use a literal object instead. Missing entries, invalid values, and unresolved references
add no syntax formatting. An empty object explicitly uses the base `editor-styles.Text`
format. Custom themes without the section therefore leave Typst syntax unstyled; the
bundled default theme supplies mappings and Typst-specific colors.

Formats support the existing foreground/background colors, font family, point size,
bold, italic, underline, and strike-through properties. Explicit `false` disables a
decoration; unspecified properties inherit the base text format. Explicit syntax font
sizes follow editor zoom (minimum 2 points); unspecified sizes inherit the editor font.
Load the theme with `Theme::createThemeFromFile()`, assign it to `m_textEditorConfig->m_theme`,
and apply it through `VTypstEditor::setConfig()`.

`m_syntaxTheme` remains the KSyntaxHighlighting theme for code highlighting elsewhere;
it no longer controls Typst elements. Source themes do not change the compiled PDF.

### Host API and project identity

Link your host against the enabled `VTextEdit` target; it publishes the
`VTEXTEDIT_ENABLE_TYPST` compile definition. For example, within an initialized QWidget
application, with `parent` supplied by the host:

```cpp
#include <vtextedit/typsteditorconfig.h>
#include <vtextedit/vtexteditor.h>
#include <vtextedit/vtypsteditor.h>

auto config = QSharedPointer<vte::TypstEditorConfig>::create();
config->m_projectRoot = QStringLiteral("/absolute/path/to/project");
config->m_mainFile = QStringLiteral("main.typ");
config->m_allowDownloads = false; // Default: installed packages only.
auto editor = new vte::VTypstEditor(
    config, QSharedPointer<vte::TextEditorParameters>::create(), parent);
editor->setFileOverlay(QStringLiteral("lib.typ"), QStringLiteral("#let mass = 7\n"));
editor->setFileOverlay(QStringLiteral("main.typ"),
    QStringLiteral("#import \"lib.typ\": mass\n$ E = #mass c^2 $\n"));
editor->setMode(vte::VTypstEditor::Mode::SideBySidePreview);
```

`TypstEditorConfig` wraps `m_textEditorConfig` and provides helper/root/main-entry paths,
`m_fontPaths`, `m_packagePaths`, `m_allowDownloads`, `m_debounceMs`, `m_compileTimeoutMs`,
`m_maxPdfBytes`, `m_maxCacheBytes`, `m_maxRasterPixels`, `m_previewMaxHeight` and
`m_initialZoom`. Apply changes through `VTypstEditor::setConfig()`, not the contained
source editor's `setConfig()`, so Typst highlighting and preview settings are refreshed.
`m_previewMaxHeight` bounds the scaled height of each page fragment, including padding,
not the total sequential preview. The host reserves the complete sequence while only
painting and requesting rasters for the visible editor slice.

Overlay paths are project-root-relative. Unsaved overlays take precedence over disk;
the host supplies other open buffers with `setFileOverlay()`/`removeFileOverlay()`.
Updating the active file overlay replaces its source text. `setProject(root, mainFile)`
changes compilation root/entry, while `setActiveFile(file)` changes this editor buffer's
identity, not its contents. For Save As or switching files, the host must explicitly
update project/active identity, supply the appropriate text and remove obsolete overlays.
The active buffer stays an overlay while open; switch identity before removing it.
The helper does not save or rewrite source. Compiler snapshots use LF text and UTF-16
half-open source offsets; source saving still follows the host's line-ending policy.

Local files and installed packages work offline. `m_allowDownloads` defaults to false;
missing packages produce diagnostics rather than implicit network access. Opting in
allows helper-side Typst package downloads. File access is constrained to the selected
project and package roots, but a separate helper process is **not an OS sandbox**.
`compileStatus()`, `compileMessage()`, `diagnostics()` and their signals expose state;
`retryCompilation()` explicitly retries after a failure. Missing helpers or compilation
errors leave source editing available; failed updates retain last-good output marked stale.

### Preview and navigation semantics

In-place cards show crops of the original compiled pages for supported equations, images,
tables and figures, including page backgrounds, clipping and surrounding overlap. They
are not independently compiled formula strings or editable generated pixels. Source stays
visible. Inline cards sit above their source line; when a short source span cannot fit
the rendered content, the layout reserves an exclusive band above that line while the
preview itself stays content-sized and source-aligned, rather than moving to the line margin. Repeated inline evaluations share a horizontal row
without wrapping; block evaluations and split-page fragments within an evaluation flow
vertically. There are no buttons, labels or nested scrolling.
Nested figure/table ownership avoids redundant cards. Unmapped/generated/imported objects
remain in the page preview rather than being attached to guessed active-source offsets.
While edits are being parsed, the last syntax highlighting and rendered in-place cards
stay visible. Their source ranges follow insertions and deletions, including line changes;
only spans deleted or replaced in full lose their cached display. Current parser results
replace the provisional highlighting. A compiled preview is staged until all its visible
page fragments have rendered, then its geometry and images replace the old cards together.
Superseded raster requests are cancellations, not “Preview unavailable” failures; continuous
typing keeps the last-good output through both compilation and rasterization. Compilation
or rendering failures retain that output. Displayed and pending images share the bounded
in-place cache budget; lowering configured limits may evict images to honor the new limits.
Changing the active file or project identity clears source-attached cached state rather
than applying it to another file.

Both PDF modes use Qt Pdf text extraction for **page-local** drag selection, Copy,
Select page text, search, zoom/fit and links. A drag is clamped to its owning page;
Select page text is not whole-document Select All. Math copies as PDF-extracted plain
text, not Typst or LaTeX source; reading order, ligatures and equations follow the PDF
text model. Active selection defers new output with an Update available indication until
the selection is cleared, Update is chosen, or focus returns to source editing. Switching
between read and side-by-side keeps the same pane and selection.

Visible PDF pages request physical screen resolution at the current zoom and display DPI.
Off-screen prefetch uses only spare capacity after visible pages are sharp; it does not
lower their resolution. If retained output and in-flight work leave too little room for
a full first paint, the new page can appear at reduced resolution and automatically
sharpen when that memory is released. Existing pixels remain visible during upgrades.
The byte/pixel limits and maximum raster dimensions still apply, including the temporary
space needed for both the retained image and its replacement.

PDF bytes are immutable, bounded in-memory buffers loaded through owned `QBuffer`s;
previewing does not write temporary PDFs. Each PDF document retains its buffer until
its renderer and document are retired. Compilation/export runs in the Rust helper;
Qt renders the PDF pages. In-place raster crops and PDF pages come from the same finished
document. Preview coordinates are **trim-page** points (1/72 inch), top-left origin,
y down, with the original page order. Printer bleed is excluded from preview PDF/raster
geometry; this viewer is not a print-production export UI. Resizing/zooming does not
change authored page settings or reflow the Typst document.

`showOutputAtCursor()` and Ctrl-click on output use the displayed snapshot's compiler
source mapping, which is not guaranteed to map every generated object to editable source.
Stale-output source navigation is disabled. Imported-file navigation emits
`sourceLocationRequested(file, start, end)` for the host to handle. External URLs emit
`externalLinkRequested(url)`; the host decides whether/how to open them.

## License
This project is licensed under [GNU LGPLv3](https://opensource.org/licenses/LGPL-3.0).

Enabled Typst distributions must also deploy the matching Qt Pdf runtime and its actual
native dependencies, plus applicable Qt/PDFium notices. The helper declares LGPL-3.0-only;
Typst and other Cargo dependencies, embedded/system fonts, and installed/downloaded Typst
packages retain their own licenses. Review the locked dependency graph and the fonts/packages
you distribute, and ship required license texts, attribution and source/relinking materials
as applicable. CMake installation and Cargo.lock are not an automated complete license or
runtime-dependency bundle; deployment and license compliance remain the distributor's responsibility.
