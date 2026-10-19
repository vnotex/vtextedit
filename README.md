# vtextedit
This project contains the key editor components for [VNote](https://github.com/vnotex/vnote), including:

* `VTextEdit`: the basic edit widget derived from `QTextEdit`.
* `VTextEditor`: a fully-functional editor for plain text, with Vi and syntax highlight supports.
* `VMarkdownEditor`: a fully-functional editor for Markdown derived from `VTextEditor`, with in-place preview support.
* `VTypstEditor` (optional): a QWidget composition for Typst source and native compiled previews.

Check the `demo` project and VNote for details.

![Main Interface](pics/main.png)

## Building and syntax highlighting

Requires **Qt 6.9 or newer**, CMake 3.21+, Perl, Python 3 and a C++20-capable build
compiler. The syntax catalog indexer uses C++20; library consumers require C++17.
Qt 5 is no longer supported. Install Qt Core, Gui, Network, Svg, Widgets, LinguistTools,
Core5Compat and the matching Qt Gui private development headers. Neither KDE Frameworks
nor ECM is required.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

On Windows, building either demo refreshes the adjacent `VTextEdit` DLL even when the
executable does not need relinking. This prevents stale deployed DLLs from shadowing the
current library. Qt runtime libraries and plugins still need the usual Qt Creator runtime
environment or deployment.

Set `CMAKE_PREFIX_PATH` to your Qt SDK if it is not discoverable. KSyntaxHighlighting is
based on upstream **6.30.0** and remains the `VSyntaxHighlighting` CMake target. Its complete
syntax catalog, including generated PHP/Twig/Jinja variants, and themes are embedded by
default; the demo no longer ships a separate XML catalog. The shared repository initializes
automatically on first use, so no setup call is needed for the embedded catalog. Register
host-specific paths with `VTextEditor::addSyntaxCustomSearchPaths(paths)` before constructing
editors; path registration retains its existing one-shot behavior.

Theme names and file paths remain supported by `TextEditorConfig::m_syntaxTheme`.
`Repository::themeFromFile()` loads a complete snapshot and rejects unreadable or malformed
files. The existing `Default` theme name remains available; upstream `defaultTheme()` uses
Breeze Light/Breeze Dark. Markdown code blocks use native KSyntaxHighlighting by default
(`MarkdownEditorConfig::m_webCodeBlockHighlighterEnabled = false`). Hosts supplying external
highlighted HTML must explicitly set this flag to `true` before constructing the editor and
provide the existing request/reply handler and token palette. Backend selection is made at
construction; theme and palette updates remain available afterward.

Native language tags first use filename-extension lookup, preserving host extension
overrides, then fall back to case-insensitive KSyntaxHighlighting language names and
aliases. Both `py` / `python` and `rs` / `rust`, for example, select their bundled
grammars. The existing explicit aliases/exclusions are unchanged.

Diagram and typesetting code blocks support these tags:

| Language | Native code-block tags |
| --- | --- |
| PlantUML | `plantuml`, `puml`, `pu` |
| Mermaid | `mermaid`, `mmd` |
| Flowchart.js | `flowchart`, `flow` |
| Graphviz DOT | `graphviz`, `dot`, `gv` |
| LaTeX | `latex`, `tex`, `ltx` |
| Typst | `typst`, `typ` |
| WaveDrom / WaveJSON | `wavedrom`, `wavejson` |

`flowchart` means the Flowchart.js `node=>type` DSL, not Mermaid's flowchart syntax;
use `mermaid` for the latter. These are native source-highlighting definitions, not
new diagram renderers. Graphviz files also recognize the standard `.gv` extension.

Rebuild downstream consumers: the KF6 engine and corrected export boundary are not binary
compatible with the old fork. VTextEdit's SOVERSION is now 9. Standalone dependency build
options are documented in `libs/syntax-highlighting/README.md`.

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
one source document, undo history and compiler session across two modes:

| `VTypstEditor::Mode` | Presentation |
| --- | --- |
| `Edit` | Editable source with independent in-place and live previews. |
| `Read` | Source hidden and input disabled; the PDF fills the widget. |

`setInplacePreviewEnabled(bool)` controls source-attached compiled output cards;
`setLivePreviewEnabled(bool)` controls the side-by-side PDF in Edit. Either, both, or
neither can be enabled. Their getters are `isInplacePreviewEnabled()` and
`isLivePreviewEnabled()`, with corresponding `inplacePreviewEnabledChanged(bool)` and
`livePreviewEnabledChanged(bool)` signals. The library starts in Edit with in-place on
and live off. These runtime preferences survive Read and configuration changes; changing
them in Read does not reveal source or hide the PDF. They are not persisted settings.

Side-by-side mode synchronizes vertical scrolling in both directions, including PDF
keyboard and page navigation. Synchronization uses relative scrollbar progress, not
source-to-output mapping, so corresponding text may not line up in uneven layouts.
It preserves the source cursor, selections, focus and undo history. Horizontal scrolling
is independent, and synchronization pauses when either side has no vertical scroll range.
Synchronization is inactive in Read or with live preview off. Presentation changes,
zoom and PDF publication do not force alignment. The next scroll uses the current ranges.

Moving the source cursor in side-by-side mode also flashes an accent marker three times
at the corresponding PDF location, using Typst's source mapping rather than scroll ratios.
The first mapped location is revealed if off-screen; an already visible target does not
move the preview. Headings use their semantic document locations, so both cursor flashes
and **Show cursor in output** target the actual heading, not a copy of its text in an
outline. This also applies when the outline and heading share a page.
The marker does not change either selection, source history or focus.
Rapid cursor moves replace earlier requests. While the PDF is stale, highlighting waits
for the current compilation to be published; leaving side-by-side editing cancels it.
Read-mode PDF clicks keep the PDF-only presentation and do not navigate to source.
**Show cursor in output** enables Live Preview when needed in Edit, without changing
in-place preview; in Read it keeps the PDF-only presentation.

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

Changing modes or preview toggles does not reload source, clear undo or mark it modified.
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
  In combined editing, cards can advance to the newest compilation while a selected PDF
  retains the prior output until publication is accepted.

Seed controls from the getters before relying on signals; initialization may have already
changed state. The demo exposes Edit/Read plus the two preview toggles, and shows PDF
controls in Read or live Edit. It blocks control signals while reflecting native state.
Modes and toggles preserve PDF selection even when hiding the preview transfers focus
to the source editor.

### Document outline

`VTypstEditor::getHeadings()` returns a `const QVector<TypstHeadingInfo> &`.
`headingsUpdated(const QVector<TypstHeadingInfo> &)` publishes each accepted compiled
outline, following `VMarkdownEditor`'s flat heading-list convention. Copy the vector to
retain it across updates; the getter's reference is borrowed until the next update.

Entries are in evaluated document order across the **whole main document**, including
imports and generated occurrences, regardless of the active source file or preview mode.
Each entry has:

* `m_title`: evaluated body plain text, without automatic heading numbering. Empty
  titles are retained.
* `m_level`: resolved positive nesting level, including Typst offsets and levels above six.
* `m_file`, `m_startPos`, `m_endPos`: source identity and half-open LF/UTF-16 offsets within
  that file. Paths are project-relative with `/` separators, or `@package` paths.
  Unmappable headings remain in the list with an empty file and `-1` offsets. Generated
  occurrences may share a source span; they are not deduplicated.

The list honors Typst's `outlined` property, not the PDF-only `bookmarked` property.
It is not a scan of literal `=` lines and does not contain copies of headings rendered
inside `#outline()`.

The getter is empty before the first successful compilation. Source/overlay/project changes,
active-file changes, dependency invalidation and compiler resets clear stale ranges immediately;
a nonempty list being cleared emits an empty update. Failed compilation leaves it empty.
A successful heading-free document also emits an empty update. Listen to `headingsUpdated`
for publication, rather than assuming `compileStatusChanged(Ready)` runs afterward.
Mode changes alone preserve the outline, and a PDF selection delaying preview publication
does not delay the current outline. Retrieval never edits source or undo history.

Deploy the updated compiler helper with the library: it must advertise the `headings`
capability; an older helper is rejected explicitly.

### Compile text to SVG or bitmap

With Typst enabled, `<vtextedit/typstcompiler.h>` exposes `vte::TypstCompiler`, an
asynchronous compiler independent of `VTypstEditor`. A `QGuiApplication` and a running
Qt event loop suffice; no editor widget is constructed. Create and use the compiler on
the same thread, and keep it alive until `finished` is emitted.

```cpp
vte::TypstEditorConfig config;
// Optional: config.m_projectRoot, m_mainFile, m_helperExecutable, m_fontPaths, ...
vte::TypstCompiler compiler(config);
QObject::connect(&compiler, &vte::TypstCompiler::finished,
                 [](const vte::TypstCompiler::Result &result) {
    if (!result.isSuccess()) {
        qWarning() << result.m_error << result.m_diagnostics;
        return;
    }
    for (int page = 0; page < result.m_bitmapPages.size(); ++page) {
        if (!result.m_bitmapPages[page].save(QStringLiteral("page-%1.png").arg(page + 1)))
            qWarning() << "Could not save page" << page + 1;
    }
});
compiler.compileToBitmap(QStringLiteral(
    "#set page(width: auto, height: auto, margin: 6pt)\n"
    "$ integral_0^1 x^2 dif x = 1/3 $"), 2.0);
// Continue running this thread's Qt event loop with compiler still alive.
```

* `compileToSvg(text)` produces UTF-8 SVG documents in `Result::m_svgPages`.
  `compileToBitmap(text, pixelsPerPoint = 1.0)` produces native Typst RGBA `QImage`s
  in `m_bitmapPages`. One entry per page, in document order; only the requested format
  is populated. SVG is vector output, not an embedded raster. Bitmap scale 1 is 72 dpi;
  scale 2 is 144 dpi. Scale must be finite and positive.
* The source controls page size and background. Pages exclude printer bleed, but are
  not automatically cropped or made transparent. The example uses auto-sized pages
  for a formula; use `fill: none` in the page settings for transparency.
* Each accepted call returns `true` and emits `finished(Result)` asynchronously once.
  A busy instance returns `false` without queuing another job or emitting an extra
  completion. `isCompiling()` is false in `finished`; a slot can submit the next job
  immediately. Destruction cancels pending work without a completion.
* Results own their data and can outlive the compiler or later jobs. An empty `m_error`
  means success; `m_diagnostics` contains compilation warnings/errors with available
  source file and LF/UTF-16 offsets. Any failure clears both page vectors, including
  pages rendered before a later error. There is no partial-success result.
* Constructor settings are copied. Text overlays `m_mainFile` in memory, with CR/CRLF
  normalized to LF; disk source is never overwritten. An empty project root resolves
  to the current directory at construction. Imports/assets, font/package paths,
  download opt-in and helper discovery follow `TypstEditorConfig`. No editor overlays
  are implicitly shared. File changes never trigger unsolicited export jobs; a
  dependency change during compilation fails the job instead of retrying it.
* `m_compileTimeoutMs` bounds each helper operation; `m_maxRasterPixels` bounds each
  bitmap page. `m_maxCacheBytes` also bounds the sum of returned page bytes, and each
  wire payload is at most 128 MiB. Helper cache limits apply separately, not as a
  process-wide memory ceiling. Native SVG generation builds its string before checking
  its size; oversized output is not copied, cached or transmitted. `m_maxPdfBytes`,
  debounce and editor presentation settings do not control these exports.

Deploy the matching `vtextedit-typst` helper with `svg` and `raster` capabilities.
Export compilation skips highlighting, outlines and editor-only preview metadata.

### Building and running

`VTEXTEDIT_ENABLE_TYPST` defaults to **OFF**. Disabled builds do not build the helper or
`VTypstEditorDemo`, install the Typst headers, or require Rust/Qt Pdf. All editors require
Qt 6.9 or newer and the build dependencies listed above. Enabled builds additionally require
Cargo with pinned **Rust 1.99.0**, the locked helper dependencies (official Typst crates
pinned to **0.15.1**), and **Qt 6 Pdf**. On Windows use an MSVC Qt kit for Qt Pdf, not MinGW.

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
editor->setMode(vte::VTypstEditor::Mode::Edit);
editor->setInplacePreviewEnabled(true);
editor->setLivePreviewEnabled(true);
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

In Edit mode, click an element in the live PDF preview to move and focus the source cursor at
its compiler-mapped position; Ctrl-click also navigates to source. In Read mode, neither gesture
jumps to source or switches modes, including a mapping reply received after switching to Read.
Dragging still selects PDF text, and ordinary clicks on links still follow their destinations.
In-place cards remain passive.

`showOutputAtCursor()` and preview clicks use the displayed snapshot's compiler source mapping,
which is not guaranteed to map every generated object to editable source. Stale-output source
navigation is disabled. Imported-file navigation emits `sourceLocationRequested(file, start, end)`
for the host to handle. External URLs emit `externalLinkRequested(url)`; the host decides
whether/how to open them.

## License
This project is licensed under [GNU LGPLv3](https://opensource.org/licenses/LGPL-3.0).

Enabled Typst distributions must also deploy the matching Qt Pdf runtime and its actual
native dependencies, plus applicable Qt/PDFium notices. The helper declares LGPL-3.0-only;
Typst and other Cargo dependencies, embedded/system fonts, and installed/downloaded Typst
packages retain their own licenses. Review the locked dependency graph and the fonts/packages
you distribute, and ship required license texts, attribution and source/relinking materials
as applicable. CMake installation and Cargo.lock are not an automated complete license or
runtime-dependency bundle; deployment and license compliance remain the distributor's responsibility.
