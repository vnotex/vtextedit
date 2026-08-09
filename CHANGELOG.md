# Changelog

## 2.0.0 (unreleased)

### Breaking changes

- **`VMarkdownEditor::tablePreviewVisibleRows()` and `setTablePreviewVisibleRows()`
  have been removed.** The built-in table preview no longer scrolls internally, so
  there is no visible-row budget to configure: the sheet renders at its full
  natural height and the editor scrolls past it. Remove the calls; there is no
  replacement.

  The library now carries a `SOVERSION`, so a consumer built against 1.x fails at
  link time rather than resolving against an incompatible build.

### Behavior changes

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
  back on a 400 ms idle debounce (flushed immediately on cell-leave, focus-out
  and Escape). Selections are confined to a single cell.
