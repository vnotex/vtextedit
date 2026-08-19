# Markdown editor internals

This guide describes the current cmark-based `VMarkdownEditor` implementation for
contributors. It follows an edit from parsing through source highlighting, folding, and
in-place preview painting, with particular attention to `TextDocumentLayout` and preview
resource lifecycles.

For the public project overview, see [README](../README.md). The
[PEG highlighter guide](peg-markdown-highlight.md) and
[cmark replacement gap analysis](cmark-replacement-gap-analysis.md) describe historical
implementation and migration context; they are not descriptions of the current pipeline.

## Component map

`VMarkdownEditor` specializes `VTextEditor`. Its constructor creates the Markdown-specific
components in this order:

| Component | Construction and ownership | Role |
|-----------|----------------------------|------|
| `DocumentResourceMgr` | `VMarkdownEditor::m_resourceMgr`, `QScopedPointer` | Maps preview resource names to `QPixmap` values. |
| `TextDocumentLayout` | Installed on the `QTextDocument` | Lays out source text, preview space, folded blocks, markers, and cursor. |
| `EditorMarkdownHighlighter` | `QScopedPointer` adapter | Gives `MarkdownHighlighter` access to editor state. |
| `KSyntaxCodeBlockHighlighter` or `WebCodeBlockHighlighter` | QObject-managed | Highlights visible fenced-code source. |
| `MathBlockHighlighter` | QObject-managed | Bridges display-math source to an external HTML tokenizer. |
| `MarkdownHighlighter` | `m_highlighter`, QObject-managed | Schedules cmark parsing, applies source formats, and emits semantic regions. |
| `EditorPreviewMgr` | `QScopedPointer` adapter | Gives `PreviewMgr` access to the document, layout, resources, base path, and editor. |
| `PreviewMgr` | QObject-managed | Converts image regions or host pixmaps into per-block preview metadata. |
| `MarkdownFoldingProvider` | `QScopedPointer` | Converts semantic folding regions into `TextFolding` ranges. |

`setupDocumentLayout()`, `setupSyntaxHighlighter()`, and `setupPreviewMgr()` establish this
graph. `MarkdownHighlighter::foldingRegionsUpdated` is then connected to
`MarkdownFoldingProvider::updateFoldingRegions()`.

### Configuration

The Markdown-specific configuration is in `MarkdownEditorConfig`:

| Field or method | Effect |
|-----------------|--------|
| `m_inplacePreviewSources` | Enables the `ImageLink`, `CodeBlock`, `Math` and/or `Table` preview sources. The first three are enabled by default; `Table` is opt-in, because its sheet writes back to the document. |
| `m_constrainInplacePreviewWidthEnabled` | Allows block preview images to shrink to the text layout width. |
| `m_webCodeBlockHighlighterEnabled` | Selects the web or KSyntax fenced-code source highlighter during construction. |
| `m_autoFoldPreviewedBlocksEnabled` | Folds a foldable region as soon as it first gets a valid in-place preview. Default on. See [Preview driven folding](#preview-driven-folding). |
| `VMarkdownEditor::setInplacePreviewEnabled()` | Temporarily enables or disables configured preview sources. |

`VMarkdownEditor::setConfig()` reapplies the base editor configuration, Markdown theme,
preview width setting, enabled preview sources, line spacing, and space width. It does not
replace the code-highlighting backend, so changing `m_webCodeBlockHighlighterEnabled` after
construction does not switch an existing editor between web and KSyntax implementations.

## Edit-to-semantics pipeline

The current parser is the bundled cmark implementation, reached through
`md::walkAndConvert()` and the AST adapter. No current class in this path has a `Peg` prefix.

```
QTextDocument::contentsChange                         GUI thread
  |
  v
MarkdownHighlighter::handleContentsChange()
  |  increment m_timeStamp; restart adaptive timers
  |
  +--> startFastParse() --> md::walkAndConvert()      GUI thread
  |      partial range, at most 15 blocks
  |      per-block highlight units only
  |
  +--> startParse() --> MarkdownParser
         |
         +--> one of two MarkdownParserWorker threads
                |
                +--> cmark parse and AST walk
                +--> MarkdownParseResult
                         |
                         v
                 handleParseResult()                  GUI thread
                         |
                         +--> block formats and states
                         +--> code/math source highlight
                         +--> image/header/table/math signals
                         +--> folding regions
```

`handleContentsChange()` increments `m_timeStamp` for a real character change. Fast parsing
runs synchronously after an adaptive delay: 100 ms during rapid typing and initially 50 ms for
other eligible edits. A successful fast parse sets the next interval to 0 ms for a range of at
most five blocks or 30 ms for a larger eligible range. It expands around the edit without exceeding
15 blocks. Fast mode asks `walkAndConvert()` only for per-block `HLUnit` data, not semantic
region vectors.

Full parsing starts from a 150 ms timer, except for the initial immediate parse. `MarkdownParser`
owns exactly two `MarkdownParserWorker` threads. A new request replaces the pending request. If
both workers are busy, one is asked to stop; cancellation is cooperative after
`walkAndConvert()` returns rather than interrupting cmark itself.

`walkAndConvert()` parses with `CMARK_OPT_DEFAULT`, walks the AST once, and produces both
per-block highlight units and semantic regions. cmark reports one-based lines and byte-based
columns. `LineOffsetTable` converts those positions into Qt UTF-16 positions, including
two-`QChar` surrogate pairs, before document-global offsets are formed.

`MarkdownParseConfig::m_extensions` is populated on both parse paths, but
`walkAndConvert()` does not consume it. The bundled cmark behavior therefore determines what
the AST recognizes. Separately, `MarkdownHighlighter::m_parserExts` and `isMathEnabled()` gate
downstream math processing and signals.

`MarkdownHighlighter::handleParseResult()` rejects a result whose timestamp no longer matches
the document. A current result updates `TextBlockData`-backed highlight state, code-block user
states, source highlighting, and semantic signals. In particular:

- `imageLinksUpdated` carries image regions to the internally connected `PreviewMgr`.
- `codeBlocksUpdated`, `mathBlocksUpdated`, `headersUpdated`, and `tableBlocksUpdated` expose
  semantic data but do not themselves create rendered previews.
- `headersUpdated` carries only the heading *regions* (`ElementRegion`). `headingsUpdated`
  additionally publishes typed `md::HeadingInfo` per heading — level plus the AST-derived
  rendered title and slugify input — so a host can build an outline without re-parsing the
  source lines. Both are emitted from the same accepted result.
- `foldingRegionsUpdated` refreshes `MarkdownFoldingProvider`.

## Preview model

Rendered previews are side metadata and pixmaps. They are not `QTextObject` instances, do not
insert document characters, and do not hide or replace the editable Markdown source.

```
cmark image regions                   host-rendered code/math
        |                                  PreviewItem pixmaps
        v                                          |
MarkdownHighlighter::imageLinksUpdated             |
        |                                          v
        +-----------------------> PreviewMgr <-----+
                                      |
                         resource name + QPixmap
                                      v
                            DocumentResourceMgr
                                      |
                       PreviewData / BlockPreviewData
                                      v
                  TextBlockData (QTextBlockUserData)
                                      |
                 TextDocumentLayout computes geometry
                                      v
           BlockLayoutData: block rect, offset, images, markers
                                      |
                                      v
                              QPainter output
```

`TextBlockData` is owned by `QTextDocument` through `QTextBlockUserData`. It holds shared
`BlockPreviewData` and `BlockLayoutData` objects alongside highlighting, folding, and spell-check
state. `BlockPreviewData` owns its `PreviewData` pointers, and each `PreviewData` owns one
`PreviewImageData`. `BlockLayoutData` is a derived geometry cache; it stores the block rectangle,
document Y offset, image paint rectangles, resource names, backgrounds, and marker lines.

`DocumentResourceMgr` is only a `QHash<QString, QPixmap>`. Preview timestamps live in
`PreviewData` and `PreviewMgr::PreviewSourceData`, where they identify the latest update and
resource use for each source.

### Preview sources

`PreviewData::Source` has three values: `ImageLink`, `CodeBlock`, and `MathBlock`. There is no
diagram-specific preview source or signal. A host may represent a rendered diagram as a generic
code-block `PreviewItem` when that contract fits.

Only image links are connected end to end inside `VMarkdownEditor`:

```
MarkdownHighlighter::imageLinksUpdated
  -> PreviewMgr::updateImageLinks

PreviewMgr::requestUpdateImageLinks
  -> MarkdownHighlighter::updateHighlight
```

`PreviewMgr::updateCodeBlocks()` and `updateMathBlocks()` accept host-produced `PreviewItem`
vectors, but the editor does not connect an internal renderer to those slots. Likewise,
`requestUpdateCodeBlocks()` and `requestUpdateMathBlocks()` require host connections if refreshes
are expected.

## Image preview lifecycle

### Region to resource

`PreviewMgr::buildImageLinksForLayout()` joins the parsed image links with the layout
information only the live document supplies. For a multiline image construct, preview metadata
is attached to the final `QTextBlock`; its start is clamped to that block, while indentation
padding is calculated from the first block. The whole construct is blockwise only when
everything before it on the first block and after it on the last block is whitespace. Otherwise
it is inline.

The destination is not re-extracted from the source text. The highlighter publishes
`md::ImageLinkInfo` — region, cmark-resolved destination, and the declared `=WxH` size — and
`PreviewMgr` consumes that directly, so escaped (`a\_b.png`), angle-bracketed (`<a b.png>`) and
entity-encoded (`a&amp;b.png`) destinations all resolve. Backslash-containing destinations are
still explicitly rejected after cmark's unescaping, which leaves Windows-style
`vx_images\a.png` rejected while `a\_b.png` resolves to `a_b.png`; use `/` for local paths.

A destination whose resolved value is empty is skipped. A reference-style image has no
destination span, but its resolved destination is known, so it previews like any other.

> This used to be a regex re-extraction via `MarkdownUtils::fetchImageLinkUrl()`. Removing the
> `=WxH` group from that expression did not make it ignore the size token — it made it fail to
> match, and every sized image silently lost its preview for three and a half months.
> `tests/utils/test_image_parser_drift.cpp` in vnote now fails the build on any string literal
> that looks like an image-link regular expression.

`MarkdownUtils::linkUrlToPath()` checks encoded and decoded base-path-relative candidates. It
returns a base-path-qualified local path only when one of those candidates already exists. If
neither exists, it converts the original short destination directly through `QUrl`; a nonexistent
relative destination therefore remains relative. Resource lookup then follows this order:

1. Reuse an existing pixmap keyed by the short destination.
2. If the resolved path exists according to `QFileInfo`, load it as local data or a pixmap.
3. Otherwise pass the returned string to `NetworkAccess::requestAsync()`, without resolving a
   relative URL against a network base URL.

There is no dedicated qrc loader branch in `PreviewMgr`; a value not accepted by the file check,
including the commented `qrc://` case, follows the asynchronous request path.

Loaded image-link pixmaps are scaled by `MarkdownUtils::scaleImage()` using
`TextEditorConfig::m_scaleFactor`. `PreviewMgr::imageResourceSize()` divides the stored pixel
size by the same scale factor so layout uses logical dimensions. This is configured editor
scaling, not a separate device-pixel-ratio query in the preview manager.

### Metadata and cleanup

For each accepted image, `PreviewMgr` adds a resource name to the source's timestamp map and
inserts `PreviewData` into the final block's `BlockPreviewData`. It removes source entries whose
timestamp does not match the current update, removes resources not marked during that update,
and relayouts affected blocks. `EditorPreviewMgr` forwards relayout to `TextDocumentLayout`,
updates the indicators border, and then `PreviewMgr` restores cursor visibility.

Network completion adds a successfully decoded pixmap and emits `requestUpdateImageLinks()`,
which triggers a new highlighter update and image-preview pass.

## TextDocumentLayout

`TextDocumentLayout` is a `QAbstractTextDocumentLayout` specialized for plain editable blocks
plus side-metadata previews. It maintains document width and height from cached per-block
rectangles rather than embedding preview objects in the document.

### Block geometry and wrapping

Each visible block gets a `QTextLayout`. The available line width starts from
`QTextDocument::pageSize().width()` minus margins and cursor allowance. If page width is not
positive, the effective width is unbounded. Every block wraps, including the blocks of a parsed
table: suppressing wrapping there glitched while the table preview widget was being edited.

Layout is lazy when a caller asks for an uncached `blockBoundingRect()`: the block is laid out
and preceding offsets are established as needed. `BlockLayoutData::m_rect` is block-local;
`m_offset` is its document Y position. Document height is the last block's bottom, while width is
the maximum cached block width.

Folding marks interior `QTextBlock`s invisible. An invisible block receives an empty text layout,
line count zero, and a non-null rectangle with zero height. The non-null width preserves
`BlockLayoutData` sentinel semantics while letting following blocks share its Y position.

### Inline previews

Inline preview processing is selected when the first preview in a block is inline. Preview ranges
are mapped to `QTextLine` cursor X positions. The image is shrunk, preserving aspect ratio, to fit
both the source span width and the 400-pixel maximum inline height. The layout reserves image
height, marker thickness, and top/bottom padding before the corresponding source text line.

For an inline preview crossing wrapped lines, the starting line paints the image if its portion
contains at least half of the source range. Otherwise the next intersecting line paints it. Other
portions receive marker placeholders. Every image-bearing block also gets a vertical dashed
marker, while inline spans get horizontal dashed markers below their reserved image space.

### Block previews

A block preview is considered only when a block has exactly one preview and that preview is not
inline. It is placed below the source text using the producer-provided indentation padding. The
maximum width passed to preview sizing is the current `QTextLayout::boundingRect().width()`.

When `m_constrainInplacePreviewWidthEnabled` is false, the original logical size and padding are
used. When it is true and the image is wider than the available width after padding, the image is
scaled with `Qt::KeepAspectRatio`. If that available width is below 400 pixels, padding is dropped
and the image is scaled against the full text-layout width instead.

### Painting, selection, and hit testing

For each visible block, `draw()` paints in this order:

1. Block background.
2. Source text and selection formats through `QTextLayout::draw()`.
3. Preview pixmaps, including an optional forced background.
4. Preview marker lines.
5. Text cursor.

Selections remain text selections; preview metadata does not add selectable document content.
`PreviewImageData::contains()` is currently unused, and the layout has no preview-specific mouse
interaction. `hitTest()` ignores image rectangles and the requested `Qt::HitTestAccuracy`; it
maps the point through the block's text lines. A click in preview-only vertical space therefore
usually resolves near a source-line boundary rather than to an image object.

### Targeted relayout

Preview updates call `TextDocumentLayout::relayout(const OrderedIntSet &)`, preserving ascending
block order:

```
PreviewMgr identifies changed block numbers
  |
  v
EditorPreviewMgr::relayout()
  |
  +--> TextDocumentLayout::relayout(sorted blocks)
  |      |
  |      +--> clear and rebuild each touched block layout
  |      +--> update offsets for each discontinuous touched block
  |      +--> recompute document size
  |      +--> repaint from the first touched block's offset to document end
  |
  +--> VTextEditor::updateIndicatorsBorder()
  |
  v
PreviewMgr::ensureCursorVisible()
```

`updateOffsetAfter()` propagates a changed bottom offset through already laid-out following blocks
until it reaches an uncached block or an offset that is already correct. Recomputing each touched
block is necessary because the input set may be discontinuous.

## Source highlighting versus rendered previews

Source highlighting changes `QTextCharFormat` ranges for visible Markdown characters. Rendered
previews reserve layout space and paint pixmaps. The two contracts are independent.

### Fenced code source

The constructor chooses one `CodeBlockHighlighter`:

- `KSyntaxCodeBlockHighlighter` uses KSyntaxHighlighting internally.
- `WebCodeBlockHighlighter` emits `externalCodeBlockHighlightRequested()`, receives
  `handleExternalCodeBlockHighlightData()`, and converts Prism-like nested `<span>` HTML into
  token formats for the original source lines. Hosts configure class formats through the
  intentionally spelled API `setExternalCodeBlockHighlihgtStyles()`.

The returned HTML produces source token formatting only. It does not produce a code preview
pixmap.

### Display-math source

`MathBlockHighlighter` similarly emits `externalMathHighlightRequested()` and consumes
`handleExternalMathHighlightData()`. It caches parsed styles by source text. Current validation
only sends blockwise display formulas whose reconstructed physical document text exactly matches
the parser's math text; indented or list-contained formulas that lose indentation during parsing
are skipped to avoid applying shifted offsets. The HTML response again formats source and does
not create a rendered math image.

### Rendered code and math

A host renderer supplies `QVector<QSharedPointer<PreviewItem>>` to
`PreviewMgr::updateCodeBlocks()` or `updateMathBlocks()`. Each item identifies its document and
block positions, padding, pixmap, resource name, optional background, and inline/blockwise mode.
This is the rendered-preview path, separate from both external HTML source-highlighting paths.

## Contributor invariants

### Ranges and layout assumptions

`BlockPreviewData::insert()` keeps entries sorted by source range and non-overlapping. A new entry
deletes every intersecting entry, regardless of preview source. Exact image-data equality replaces
the complete old `PreviewData`; this normally refreshes its timestamp, but source is not part of
the equality comparison and can also change.

The layout has stricter assumptions than the data type expresses:

- If the first preview is inline, inline layout asserts that every preview it visits is inline.
- Block preview geometry is produced only when the block has exactly one non-inline preview.
- Producers should therefore avoid mixed inline/blockwise entries and multiple blockwise entries
  in one block.

### Resource identity

Image links use only the short destination string as their `DocumentResourceMgr` key. Changing the
editor base path, replacing a local file, or changing remote content while keeping that string can
reuse stale pixels.

Code and math resources use `QString::number(source) + "_" + PreviewItem::m_name`. If that key
already exists, `PreviewMgr::imageResourceNameForSource()` reuses it without replacing the
pixmap. A host must therefore change `PreviewItem::m_name` whenever rendered content changes.

### Timestamps and asynchronous work

Each preview source owns an increasing timestamp. A full update stamps all current block entries
and resource names, then deletes older entries for that source. This assumes producers submit the
complete current set, not a delta.

The network path has observed fragile cases:

- `m_urlMap` has one entry per resolved URL, so duplicate requests overwrite shared tracking.
- Failed downloads establish no explicit retry state; a later semantic refresh is needed to try
  again.
- Requests carry no preview timestamp or cancellation token. A response arriving after its source
  was removed can add a resource that is no longer tracked by the source's cleanup map.
- `PreviewMgr::checkBlocksForObsoletePreview()` has no internal caller.

Treat these as current implementation constraints, not API guarantees.

### Threads and ownership

cmark full-document parsing runs on `MarkdownParserWorker` threads and returns immutable result
data to the GUI thread. Fast parsing, `QSyntaxHighlighter` updates, `QPixmap` management, preview
metadata mutation, layout, and painting occur on the GUI side. Hosts should deliver rendered
`PreviewItem` updates to `PreviewMgr` on its owning thread.

Safe extension points are the public highlighter signals/slots for source HTML, the preview refresh
signals, and the `PreviewMgr::updateCodeBlocks()` / `updateMathBlocks()` pixmap slots. Although
`VMarkdownEditor::getDocumentResourceMgr()` exposes an incomplete pointer, its header is internal;
external consumers should normally use `findImageFromDocumentResourceMgr()` or `PreviewMgr`
instead of depending on that private type.

## Preview driven folding

`MarkdownFoldingProvider` owns both halves of this: it turns the parser's folding regions
into `TextFolding` ranges, and it decides the initial fold state of every region which has
a valid preview.

### Reconciliation

`updateFoldingRegions()` matches each parsed region against the *live* extent of the ranges
it already owns, asked from `TextFolding::foldingRangeBlocks()`, not against the block
numbers of the previous parse. An edit which only shifts block numbers therefore keeps
every range, its id and its fold state. Matching compares the region type as well, so a
fenced code block edited into a table at the same extent is treated as the new element it
is. Removal of unmatched ranges precedes creation of missing ones, because `TextFolding`
refuses a new range starting on the same block as an existing one.

Two regions covering exactly the same blocks - a blockquote wrapping nothing but a table,
for instance - are de-duplicated before matching, in favour of the preview-bearing type
(`FencedCode`, `Math`, `Table`) over the wrapper types (`Heading`, `Blockquote`,
`FrontMatter`). Only one of them can become a range, and which one owns it decides which
element owns the fold state.

### The initial fold state

`applyPreviewAutoFold()` is the pass which takes and enforces that decision - the only one
which ever folds a range that already exists. It is always reached through
`InteractivePreviewHost::scheduleFoldRefresh()`, never synchronously: `completeHighlight()`
emits `foldingRegionsUpdated` *before* `previewElementsUpdated`, so at the moment the
regions land the host still describes the previous generation. The refresh is re-armed
from the host's existing unblock points, so it can never run against a half-reconciled item
set.

A region is settled the first time a pass sees it together with a valid preview - a widget
whose block extent and type match the region exactly, or a painted, non-inline `PreviewData`
of the mapped source on the region's first or last block, which are the two blocks folding
keeps visible. Once settled, the option is never re-read for that range: unfolding by hand
is never undone, and a region whose interior holds the caret when the preview appears is
left open for good. Changing the option at runtime therefore only affects regions which
have not been settled yet.

### Keeping the state across a rewrite

`TextFolding` destroys a range the moment the blocks it spans are replaced, which is exactly
what an accepted in-place rewrite does. The fold state therefore lives on the
`InteractivePreviewHost` item, whose identity and anchor survive that edit, and is carried
across every rebuild of the same logical element. `handleSourceReplacementRequested()`
samples the *live* state before the edit - a manual fold made since the last queued refresh
is not on the item yet - and re-creates the range already folded straight after it, in the
same event-loop turn, so the source never visibly expands. Both the query and the restore
use the element's extent, trimmed of the trailing whitespace a replacement may carry but
the parsed element does not.

Nothing folds while text folding itself is off: `applyPreviewAutoFold()`,
`restoreFoldedRange()` and `tryRegionFolded()` all return early, so what a preview
remembers survives a disable/enable cycle and is restored on the next parse.

Deciding and enforcing are two separate phases inside the pass, because `foldRange()`
emits `foldingRangesChanged()` synchronously and that reaches application code (the
layout's `widgetPreviewGeometryChanged` is delivered straight to the preview host, which
hands a geometry context to every widget). Nothing may therefore hold an iterator into the
entry table across a fold; every settled decision is re-resolved by id afterwards.

Undo and redo are the documented exception. An undo of a rewrite performs the inverse
destructive replacement with no post-edit retarget hook, so the next generation is a fresh
element and the option decides again.

## Current limitations

- Preview image extraction supports one direct `![alt](url)` match, not every image region that
  cmark can identify.
- Backslashes in image destinations are rejected.
- A nonexistent relative image destination is passed to the asynchronous path as a relative URL;
  it is not qualified with the editor base path first.
- Image-link cache identity does not include base path, file modification time, or content hash.
- External code/math pixmap renderers are not wired internally.
- Network request deduplication, cancellation, retry, and late-response cleanup are incomplete.
- The fenced-code source backend is selected only during construction.
- The obsolete-preview checker is present but unconnected.
- Mixed inline/blockwise previews and multiple blockwise previews do not satisfy current layout
  assumptions.
- Hit testing and selection are text-oriented rather than preview-aware.
- A region `TextFolding` refuses - one sharing its start block with a strictly larger
  wrapper region - never gets a range, and therefore never auto-folds.
- An element rendered only by a painted preview has no durable identity, so a destructive
  edit over it, or a text-folding disable/enable cycle, re-decides its initial fold state.

## Test coverage

Current parser coverage is substantial: `test_markdownparser` exercises direct
`walkAndConvert()` behavior and includes one real `VMarkdownEditor` source-format integration
case; `test_astwalker` verifies golden highlight output, regions, folding extraction, and UTF-16
position handling; `test_cmark_probe` and `test_goldenmaster` cover parser behavior and migration
fixtures. `test_markdownfolding` covers folding-provider behavior and one custom-layout geometry
case confirming zero-height folded blocks and restoration after unfolding. Preview driven
folding is covered at three levels: `test_textfolding` for the range accessors,
`test_markdownfolding` for reconciliation, the auto-fold decision and the restore, and
`test_interactivepreview` end to end on a real `VMarkdownEditor`.

There are no dedicated tests for `PreviewMgr`, `BlockPreviewData`, `DocumentResourceMgr`, or
`EditorPreviewMgr`. Image extraction/loading and network races, resource cleanup, code/math preview
host wiring, image geometry and painting, markers, preview-space hit testing, external math source
highlighting, and parser-to-rendered-preview end-to-end behavior are also not covered directly.

## Source index

| Area | Main files |
|------|------------|
| Façade and configuration | `src/include/vtextedit/vmarkdowneditor.h`, `src/markdowneditor/vmarkdowneditor.cpp`, `src/include/vtextedit/markdowneditorconfig.h` |
| Parser and AST conversion | `src/markdowneditor/markdownparser.{h,cpp}`, `src/markdowneditor/markdownastwalker.{h,cpp}`, `src/markdowneditor/cmarkadapter.{h,cpp}` |
| Source highlighter | `src/include/vtextedit/markdownhighlighter.h`, `src/markdowneditor/markdownhighlighter.cpp`, `src/markdowneditor/markdownhighlighterresult.{h,cpp}` |
| Code and math source adapters | `src/markdowneditor/ksyntaxcodeblockhighlighter.cpp`, `src/markdowneditor/webcodeblockhighlighter.cpp`, `src/markdowneditor/mathblockhighlighter.cpp` |
| Preview manager and adapter | `src/include/vtextedit/previewmgr.h`, `src/markdowneditor/previewmgr.cpp`, `src/markdowneditor/editorpreviewmgr.cpp` |
| Preview and block metadata | `src/include/vtextedit/previewdata.h`, `src/markdowneditor/previewdata.cpp`, `src/include/vtextedit/textblockdata.h`, `src/textedit/textblockdata.cpp` |
| Resource storage | `src/markdowneditor/documentresourcemgr.{h,cpp}` |
| Layout and paint cache | `src/markdowneditor/textdocumentlayout.{h,cpp}`, `src/markdowneditor/textdocumentlayoutdata.h` |
| Image utilities and network | `src/utils/markdownutils.cpp`, `src/utils/networkutils.cpp` |
| Folding bridge | `src/markdowneditor/markdownfoldingprovider.{h,cpp}` |
