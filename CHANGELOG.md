# Changelog

## 8.0.0 (unreleased)

### Breaking changes

- `MarkdownEditorConfig` and `Theme` grow for inline concealment. Rebuild all consumers;
  the library SOVERSION is now 8. Matching Qt Gui private development headers are required,
  and the runtime must use the Qt build against which the library was compiled.

### Behavior changes

- Long Markdown image/link/reference destinations and HTML image `src` values display
  three graphemes at each end around three middle dots, without changing source. The default
  threshold is 20 graphemes; `m_concealElements` selects eligible URL kinds and an empty mask
  disables the feature. `Theme::ConcealedText` controls compact foreground/background.
- Entering any part of a concealed range reveals it. Source cursor positions, counted Vi
  motions, selection, copy/cut, search and undo remain unchanged. IME preedit reveals its block.
- Reapplying unchanged line spacing no longer adds redundant formatting undo commands.

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
