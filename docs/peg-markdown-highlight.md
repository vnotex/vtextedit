# peg-markdown-highlight

A PEG-grammar-based Markdown parser for syntax highlighting, used by vtextedit to power Markdown highlighting in VNote.

## Background

**peg-markdown-highlight** (pmh) was created by Ali Rantakari (2011--2016) as part of his Master's thesis at the University of Helsinki. The core insight: regex-based Markdown highlighters fail on context-sensitive constructs (reference links, nested emphasis, multi-line blocks) because they operate line-by-line without whole-document context. A real parser does not have this limitation.

The parser is adapted from John MacFarlane's [peg-markdown](https://github.com/jgm/peg-markdown) compiler. Instead of compiling Markdown to HTML, it records the character ranges of each language element encountered during parsing -- producing data suitable for editor syntax highlighting.

### Lineage

```
peg-markdown (John MacFarlane)           -- Markdown compiler using PEG grammar
  -> peg-markdown-highlight (Ali Rantakari)  -- Adapted grammar for highlighting
    -> vnotex/peg-markdown-highlight (tamlok)    -- Fork adding extensions
```

The vtextedit copy at `libs/peg-markdown-highlight/` contains only the **pre-generated** C output files from the [vnotex fork](https://github.com/vnotex/peg-markdown-highlight) -- not the `.leg` grammar source.

### Why PEG over Regex

| Aspect | Regex-based | PEG-based (pmh) |
|--------|-------------|-----------------|
| Context sensitivity | No cross-line context | Whole-document awareness |
| Reference link resolution | Cannot check if ref exists | Two-pass: collect refs, then resolve |
| Nested constructs | Fragile/impossible | Recursive descent handles nesting |
| Correctness | Approximate | Matches compiler behavior |

**Concrete example**: If `[foo]` appears in the text but no `[foo]: url` definition exists anywhere in the document, pmh will _not_ highlight it as a link. Regex highlighters cannot do this.

### The `greg` Parser Generator

The C source `pmh_parser.c` is machine-generated from a PEG grammar (`.leg` file) by [greg](https://github.com/ooc-lang/greg), a fork of Ian Piumarta's `peg/leg` parser generator. `greg` adds two features critical for editor integration:

1. **Re-entrancy** -- the parser uses a context pointer (`G->data`) instead of global state, making it safe for multi-threaded use.
2. **Start/end positions** -- `greg` exposes `thunk->begin` and `thunk->end` for matched spans, enabling precise character range recording.

Build pipeline (in the upstream repository):

```
pmh_grammar.leg  -->  [greg]  -->  pmh_parser.c (generated)
                                       +
pmh_parser_head.c  ----------------->  (prepended)
pmh_parser_foot.c  ----------------->  (appended)
```

## Files

| File | Description |
|------|-------------|
| `pmh_definitions.h` | Element type enum, element struct, extension flags |
| `pmh_parser.h` | Public API (4 functions) |
| `pmh_parser.c` | Generated recursive-descent parser (~7500 lines, 269 grammar rules) |
| `CMakeLists.txt` | Builds a static library `peg-markdown-highlight` |

## API Reference

### Core Functions

```c
// Parse Markdown text, return element array indexed by type.
// Caller must pass out_result to pmh_free_elements() when done.
void pmh_markdown_to_elements(char *text,
                              int extensions,
                              pmh_element **out_result[]);

// Sort element linked lists by start offset (ascending).
void pmh_sort_elements_by_pos(pmh_element *element_lists[]);

// Free the element array returned by pmh_markdown_to_elements().
void pmh_free_elements(pmh_element **elems);

// Get human-readable name for an element type (e.g. "LINK", "H1").
char *pmh_element_name_from_type(pmh_element_type type);

// Get element type from name string. Returns pmh_NO_TYPE if not found.
pmh_element_type pmh_element_type_from_name(char *name);
```

### Element Struct

```c
struct pmh_Element {
    pmh_element_type type;    // Type of element
    unsigned long pos;        // Start offset (Unicode code point)
    unsigned long end;        // End offset (Unicode code point)
    struct pmh_Element *next; // Next element in linked list
    char *label;              // Label (links and references)
    char *address;            // URL address (links and references)
};
typedef struct pmh_Element pmh_element;
```

### Result Structure

`pmh_markdown_to_elements()` returns a `pmh_element**` -- an array of linked lists, one per element type. Index into it with any `pmh_element_type` value:

```c
pmh_element **result;
pmh_markdown_to_elements(text, pmh_EXT_NONE, &result);

// Walk all H1 elements
pmh_element *h1 = result[pmh_H1];
while (h1 != NULL) {
    printf("H1 at [%lu, %lu)\n", h1->pos, h1->end);
    h1 = h1->next;
}

pmh_free_elements(result);
```

### Minimal Usage Example

```c
#include "pmh_parser.h"

void highlight(const char *markdown_text) {
    pmh_element **result;
    int extensions = pmh_EXT_NOTES | pmh_EXT_STRIKE;

    pmh_markdown_to_elements((char *)markdown_text, extensions, &result);

    // Apply highlighting for each element type
    for (int i = 0; i < pmh_NUM_LANG_TYPES; i++) {
        pmh_element *cursor = result[i];
        while (cursor != NULL) {
            if (cursor->end > cursor->pos) {
                apply_style(cursor->pos, cursor->end, i);
            }
            cursor = cursor->next;
        }
    }

    pmh_free_elements(result);
}
```

## Element Types

### Language Elements (33 types)

| Enum | Value | Description |
|------|-------|-------------|
| `pmh_LINK` | 0 | Explicit link `[text](url)` |
| `pmh_AUTO_LINK_URL` | 1 | Implicit URL link `<http://...>` |
| `pmh_AUTO_LINK_EMAIL` | 2 | Implicit email link `<user@host>` |
| `pmh_IMAGE` | 3 | Image `![alt](url)` |
| `pmh_CODE` | 4 | Inline code `` `code` `` |
| `pmh_HTML` | 5 | Inline HTML |
| `pmh_HTML_ENTITY` | 6 | HTML entity `&amp;` |
| `pmh_EMPH` | 7 | Emphasis `*text*` or `_text_` |
| `pmh_STRONG` | 8 | Strong `**text**` or `__text__` |
| `pmh_LIST_BULLET` | 9 | Bullet marker `- `, `* `, `+ ` |
| `pmh_LIST_ENUMERATOR` | 10 | Ordered list marker `1. ` |
| `pmh_COMMENT` | 11 | HTML comment `<!-- -->` |
| `pmh_H1` -- `pmh_H6` | 12--17 | Headers, levels 1--6 (contiguous, in order) |
| `pmh_BLOCKQUOTE` | 18 | Blockquote `> ` |
| `pmh_VERBATIM` | 19 | Indented code block |
| `pmh_HTMLBLOCK` | 20 | Block-level HTML |
| `pmh_HRULE` | 21 | Horizontal rule `---` |
| `pmh_REFERENCE` | 22 | Reference definition `[label]: url` |
| `pmh_FENCEDCODEBLOCK` | 23 | Fenced code block `` ``` `` or `~~~` |
| `pmh_NOTE` | 24 | Footnote |
| `pmh_STRIKE` | 25 | Strikethrough `~~text~~` |
| `pmh_FRONTMATTER` | 26 | YAML front matter `---` block |
| `pmh_DISPLAYFORMULA` | 27 | Display math `$$ ... $$` or `\begin{} ... \end{}` |
| `pmh_INLINEEQUATION` | 28 | Inline math `$ ... $` |
| `pmh_MARK` | 29 | HTML `<mark>` tag content |
| `pmh_TABLE` | 30 | GFM table body |
| `pmh_TABLEHEADER` | 31 | GFM table header row |
| `pmh_TABLEBORDER` | 32 | GFM table border `\|` |

**Constants**:
- `pmh_NUM_TYPES` = 39 (all types including internal parser types)
- `pmh_NUM_LANG_TYPES` = 33 (only language element types, i.e. `pmh_NUM_TYPES - 6`)

### Internal Types (not for external use)

| Enum | Description |
|------|-------------|
| `pmh_RAW_LIST` | List of raw spans for second-pass parsing |
| `pmh_RAW` | Span marker for second-pass parsing |
| `pmh_EXTRA_TEXT` | Additional text injected during parsing |
| `pmh_SEPARATOR` | Separator between raw span groups |
| `pmh_NO_TYPE` | Placeholder during parsing |
| `pmh_ALL` | Master list of all allocated elements (for cleanup) |

## Extensions

Extensions are enabled via the `extensions` parameter as a bitfield:

```c
enum pmh_extensions {
    pmh_EXT_NONE         = 0,
    pmh_EXT_NOTES        = (1 << 0),  // Pandoc footnote syntax
    pmh_EXT_STRIKE       = (1 << 1),  // ~~strikethrough~~
    pmh_EXT_FRONTMATTER  = (1 << 2),  // YAML front matter
    pmh_EXT_MATH         = (1 << 3),  // $ and $$ math
    pmh_EXT_MARK         = (1 << 4),  // <mark> tag content
    pmh_EXT_MATH_RAW     = (1 << 5),  // \begin{} ... \end{} math
    pmh_EXT_TABLE        = (1 << 6),  // GFM tables
};
```

Combine with bitwise OR:

```c
int exts = pmh_EXT_NOTES | pmh_EXT_STRIKE | pmh_EXT_FRONTMATTER
         | pmh_EXT_MATH | pmh_EXT_MATH_RAW | pmh_EXT_MARK | pmh_EXT_TABLE;
pmh_markdown_to_elements(text, exts, &result);
```

## How the Parser Works

### Two-Pass Parsing

1. **Reference collection pass** (`parse_references()`): scans the entire document for reference definitions (`[label]: url`). Stores them in a lookup table.
2. **Full parse** (`parse_markdown()`): parses the document, resolving reference links against the collected definitions. Only links with existing references produce `pmh_LINK` elements.
3. **Raw block processing** (`process_raw_blocks()`): iteratively re-parses inline content within block-level elements (list items, blockquotes) until no raw spans remain.

### Input Preprocessing

Before parsing, `strcpy_preformat()`:
- Strips UTF-8 continuation bytes (the parser operates on code points, not bytes)
- Removes any UTF-8 BOM
- Appends two trailing newlines (matching peg-markdown convention)
- Records positions of stripped bytes for offset correction in the output

### Offset Model

Element positions (`pos`, `end`) are **Unicode code point offsets** into the original input string. The parser internally tracks byte-to-codepoint mappings to account for multi-byte UTF-8 sequences stripped during preprocessing.

## Integration in vtextedit

### Architecture Overview

```
QTextDocument::contentsChange
  |
  v
PegMarkdownHighlighter          (timer-based triggering)
  |
  +-- startFastParse()           (sync, main thread, partial document)
  +-- startParse()               (async, background thread, full document)
        |
        v
PegParser                       (thread pool, 2 workers)
  |
  v
PegParserWorker::run()           (QThread)
  |
  v
pmh_markdown_to_elements()       (C library call)
  |
  v
PegParseResult                   (owns pmh_element**, extracts typed regions)
  |
  v  (signal: parseResultReady)
PegHighlighterResult             (converts to per-block HLUnit vectors)
  |
  v
highlightBlock()                 (Qt's QSyntaxHighlighter callback)
  |
  v
setFormat(start, length, style)  (applies QTextCharFormat from theme)
```

### Key Classes

| Class | File | Role |
|-------|------|------|
| `PegParser` | `src/markdowneditor/pegparser.h` | Thread pool manager; wraps C API call |
| `PegParserWorker` | `src/markdowneditor/pegparser.h` | `QThread` that calls `pmh_markdown_to_elements()` |
| `PegParseConfig` | `src/markdowneditor/pegparser.h` | Input: UTF-8 text + extensions + timestamp |
| `PegParseResult` | `src/markdowneditor/pegparser.h` | Owns `pmh_element**`; extracts region vectors by type |
| `PegHighlighterResult` | `src/markdowneditor/peghighlighterresult.h` | Converts pmh elements to per-block `HLUnit` arrays |
| `PegMarkdownHighlighter` | `src/include/vtextedit/pegmarkdownhighlighter.h` | `QSyntaxHighlighter` subclass; timer management, style application |
| `Theme` | `src/include/vtextedit/theme.h` | Maps `pmh_element_type` indices to `QTextCharFormat` styles |

### Parse Triggering

`PegMarkdownHighlighter` connects to `QTextDocument::contentsChange` and uses two timers:

| Timer | Default Interval | Purpose |
|-------|-----------------|---------|
| `m_fastParseTimer` | 50 ms (adaptive 0--100 ms) | Parse a small block range around the edit, synchronously on the main thread |
| `m_parseTimer` | 150 ms (0 ms for first parse) | Full document parse, asynchronously on a background thread |

### Threading

- `PegParser` maintains **2 background `QThread` workers**.
- `parseAsync()` queues work to an idle worker. If both are busy, the worker with the older timestamp is cancelled via `QAtomicInt`.
- Fast parse runs synchronously on the main thread for responsiveness (small block ranges only, max 15 blocks).

### Style Mapping

The style array `m_styles` is a `QVector<QTextCharFormat>` of size `pmh_NUM_LANG_TYPES`, indexed directly by `pmh_element_type`. The `Theme::MarkdownSyntaxStyle` enum in `theme.h` is aligned 1:1 with `pmh_element_type`:

```cpp
// theme.h -- must stay in sync with pmh_definitions.h
enum MarkdownSyntaxStyle {
    LINK = 0,        // pmh_LINK
    AUTO_LINK_URL,   // pmh_AUTO_LINK_URL
    AUTO_LINK_EMAIL, // pmh_AUTO_LINK_EMAIL
    IMAGE,           // pmh_IMAGE
    // ... (same order as pmh_element_type)
    TABLEBORDER,     // pmh_TABLEBORDER
    MaxMarkdownSyntaxStyle
};
```

Theme styles are loaded from JSON files and converted to `QTextCharFormat` via `Format::toTextCharFormat()`. During highlighting, `HLUnit::styleIndex` (which equals the `pmh_element_type` integer) indexes directly into `m_styles[]`.

### Memory Management

- `PegParseResult` owns the `pmh_element**`. Its destructor calls `pmh_free_elements()`.
- `PegParseResult` is wrapped in `QSharedPointer` -- reference counted, freed when the last reference drops.
- The pmh elements are consumed during `PegHighlighterResult` construction (positions extracted into `HLUnit` vectors), after which the `PegParseResult` can be freed independently.

### Unicode Handling

Qt's `QString` uses UTF-16 internally. Characters with code points above U+FFFF are stored as surrogate pairs (2 `QChar`s, `QString::size()` returns 2). But `pmh_markdown_to_elements()` counts them as 1 code point. To reconcile:

`PegParser::parseMarkdownToElements()` calls `tryFixUnicodeData()` which replaces any 4-byte UTF-8 sequence (code point > 65535) with two 1-byte placeholder characters (`'V'`). This makes the byte count match Qt's `QChar` count so that element offsets map correctly to `QTextDocument` positions.

## Build

The library builds as a **static library** via CMake:

```cmake
cmake_minimum_required(VERSION 3.16)
project(peg-markdown-highlight VERSION 1.0 LANGUAGES C CXX)

add_library(peg-markdown-highlight STATIC
    pmh_definitions.h
    pmh_parser.c pmh_parser.h
)
```

Supports Qt 5 and Qt 6 (controlled by `QT_DEFAULT_MAJOR_VERSION`). The only Qt dependency is `Qt::Core` (for `qt_standard_project_setup()` on Qt 6).

Since `pmh_parser.c` is plain C, it compiles with any C99-compatible compiler. The `extern "C"` linkage is handled by the consumer:

```cpp
extern "C" {
#include <pmh_parser.h>
}
```

## References

| Resource | URL |
|----------|-----|
| Original project | https://hasseg.org/peg-markdown-highlight/ |
| Original GitHub | https://github.com/ali-rantakari/peg-markdown-highlight |
| Master's thesis (PDF) | https://hasseg.org/peg-markdown-highlight/peg-markdown-highlight.pdf |
| Doxygen API docs | https://hasseg.org/peg-markdown-highlight/docs/ |
| vnotex fork (source of this copy) | https://github.com/vnotex/peg-markdown-highlight |
| greg parser generator | https://github.com/ooc-lang/greg |
| peg-markdown (upstream grammar) | https://github.com/jgm/peg-markdown |
| QarkDown (demo Qt editor using pmh) | https://github.com/ali-rantakari/QarkDown |

## License

Dual-licensed under **GPL2+** and **MIT**. See the upstream repository for the full license text.
