# Gap Analysis: Replacing peg-markdown-highlight with vnotex/cmark

## Executive Summary

**Verdict: Feasible with significant integration work.**

vnotex/cmark can replace peg-markdown-highlight (pmh) as vtextedit's Markdown parser for syntax highlighting. The vnotex fork already has the critical extensions (math, mark, strikethrough, tables) baked into its core enum. However, the replacement requires addressing 5 architectural gaps and 3 missing features.

**Estimated effort**: Medium-high. The parser swap itself is straightforward, but the position model change (byte offsets → line:column) ripples through the entire highlight pipeline.

---

## 1. Architecture Comparison

| Aspect | peg-markdown-highlight | vnotex/cmark |
|--------|----------------------|--------------|
| **Parser type** | PEG (backtracking) | Two-pass (block structure → inline) |
| **Complexity** | O(n²) worst case | O(n) guaranteed |
| **Output format** | Flat array of linked lists indexed by type | AST tree (parent/child/sibling) |
| **Position model** | Byte offset (code-point based `pos`/`end`) | Line:column (1-indexed) |
| **API style** | Single function call → result array | Parse → iterate tree → free |
| **Thread safety** | Re-entrant (separate parser per call) | Safe with separate parser instances (avoid arena allocator) |
| **Extensions** | Compile-time flags (`pmh_EXT_*`) | Baked into core enum (no plugin system) |
| **Language** | C (greg-generated PEG) | C99 |
| **Build** | CMake | CMake |

### Performance

cmark is significantly faster than PEG-based parsers:
- O(n) vs O(n²) worst case
- Fuzz-tested against pathological inputs that cause PEG parsers to hang
- cmark renders War and Peace (~3.2M chars) in ~127ms

For vtextedit's timer-based re-parse-on-keystroke model, cmark's predictable linear performance is a meaningful upgrade.

---

## 2. Current pmh API Surface in vtextedit

Only 3 functions and 3 struct fields are consumed:

### Functions Called
| Function | Location | Purpose |
|----------|----------|---------|
| `pmh_markdown_to_elements(data, exts, &result)` | `pegparser.cpp:461` | Parse markdown text |
| `pmh_free_elements(result)` | `pegparser.cpp:317` | Free parse result |
| `pmh_NUM_LANG_TYPES` | `pegparser.cpp:465`, `pegmarkdownhighlighter.cpp:571-572` | Array sizing constant |

`pmh_sort_elements_by_pos()` is **never called**. `pmh_has_metadata()` is **never called**.

### Struct Fields Accessed
| Field | Usage |
|-------|-------|
| `elem->pos` | Start position (every traversal) |
| `elem->end` | End position (every traversal) |
| `elem->next` | Linked list traversal |

Fields `label`, `address`, `lang_attribute`, `type` are **never read** (type is known from array index).

### Extension Flags Used
Set in `pegmarkdownhighlighter.cpp:30-35`:
- `pmh_EXT_NOTES` — footnotes
- `pmh_EXT_STRIKE` — strikethrough (`~~`)
- `pmh_EXT_FRONTMATTER` — YAML front matter
- `pmh_EXT_MARK` — `<mark>` / `==highlight==`
- `pmh_EXT_TABLE` — pipe tables
- `pmh_EXT_MATH` — math (conditional on `isMathEnabled()`)
- `pmh_EXT_MATH_RAW` — raw math (conditional, paired with MATH)

---

## 3. Element Type Mapping

### Explicitly Indexed Types (special handling in vtextedit)

| pmh Type | vtextedit Usage | cmark Equivalent | Gap? |
|----------|----------------|------------------|------|
| `pmh_H1`–`pmh_H6` | Header region extraction (`m_headerRegions`) | `CMARK_NODE_HEADING` + `cmark_node_get_heading_level()` | ✅ Direct mapping. One node type with level accessor instead of 6 separate types. |
| `pmh_IMAGE` | Image region extraction (`m_imageRegions`) | `CMARK_NODE_IMAGE` | ✅ Direct. |
| `pmh_FENCEDCODEBLOCK` | Code block extraction (`m_codeBlockRegions`), regex post-processing for lang/markers | `CMARK_NODE_CODE_BLOCK` + `cmark_node_get_fence_info()` | ⚠️ cmark uses one type for both fenced and indented code blocks. Need `get_fence_info()` to distinguish. Lang string available natively (no regex needed). |
| `pmh_INLINEEQUATION` | Inline math regions | `CMARK_NODE_FORMULA_INLINE` | ✅ Direct (vnotex extension). |
| `pmh_DISPLAYFORMULA` | Display math regions, `$$`/`\begin{}` marker detection | `CMARK_NODE_FORMULA_BLOCK` | ✅ Direct (vnotex extension). |
| `pmh_HRULE` | Horizontal rule regions | `CMARK_NODE_THEMATIC_BREAK` | ✅ Direct. |
| `pmh_TABLE` | Table regions | `CMARK_NODE_TABLE` | ✅ Direct (vnotex extension). |
| `pmh_TABLEHEADER` | Table boundary detection (new table starts where header is) | `CMARK_NODE_TABLE_ROW` (first child of TABLE) | ⚠️ No dedicated header node. Must check if TABLE_ROW is first child. |
| `pmh_TABLEBORDER` | Border positions filled into `TableBlock.m_borders` | **None** | ❌ **GAP.** Pipe characters (`\|`) not exposed as AST nodes. See §4.1. |

### Generic Highlight Types (iterated in loop over all `pmh_NUM_LANG_TYPES`)

| pmh Type | cmark Equivalent | Gap? |
|----------|------------------|------|
| `pmh_LINK` | `CMARK_NODE_LINK` | ✅ |
| `pmh_AUTO_LINK_URL` | `CMARK_NODE_LINK` | ⚠️ No separate autolink type. Must inspect URL scheme or use heuristic. |
| `pmh_AUTO_LINK_EMAIL` | `CMARK_NODE_LINK` | ⚠️ Same — check URL for `mailto:`. |
| `pmh_CODE` | `CMARK_NODE_CODE` | ✅ |
| `pmh_EMPH` | `CMARK_NODE_EMPH` | ✅ |
| `pmh_STRONG` | `CMARK_NODE_STRONG` | ✅ |
| `pmh_STRIKE` | `CMARK_NODE_STRIKETHROUGH` | ✅ (vnotex extension). |
| `pmh_MARK` | `CMARK_NODE_MARK` | ✅ (vnotex extension). |
| `pmh_HTML` | `CMARK_NODE_HTML_INLINE` | ✅ |
| `pmh_HTMLBLOCK` | `CMARK_NODE_HTML_BLOCK` | ✅ |
| `pmh_HTML_ENTITY` | **None** | ⚠️ Parsed as part of TEXT or HTML_INLINE. Not separately exposed. Low impact — entities are rare in editing context. |
| `pmh_BLOCKQUOTE` | `CMARK_NODE_BLOCK_QUOTE` | ✅ |
| `pmh_VERBATIM` | `CMARK_NODE_CODE_BLOCK` (no fence info) | ✅ Unified with fenced. Distinguish by empty `fence_info`. |
| `pmh_LIST_BULLET` | `CMARK_NODE_LIST` (bullet type) + `CMARK_NODE_ITEM` | ⚠️ No separate marker-span node. See §4.1. |
| `pmh_LIST_ENUMERATOR` | `CMARK_NODE_LIST` (ordered type) + `CMARK_NODE_ITEM` | ⚠️ Same as bullet — marker not a separate node. |
| `pmh_COMMENT` | Part of `CMARK_NODE_HTML_BLOCK` or `HTML_INLINE` | ⚠️ Not distinguished from other HTML. |
| `pmh_NOTE` | **None in vnotex/cmark** | ❌ **GAP.** See §4.2. |
| `pmh_REFERENCE` | **None** (resolved at parse time) | ⚠️ References consumed during parsing, not exposed. Low impact — pmh exposes them but vtextedit only uses them for generic highlighting. |
| `pmh_FRONTMATTER` | **None** | ❌ **GAP.** See §4.3. |

### Summary Scorecard

| Status | Count | Types |
|--------|-------|-------|
| ✅ Direct mapping | 19 | H1-H6, IMAGE, INLINEEQUATION, DISPLAYFORMULA, HRULE, TABLE, LINK, CODE, EMPH, STRONG, STRIKE, MARK, HTML, HTMLBLOCK, BLOCKQUOTE, VERBATIM, FENCEDCODEBLOCK |
| ⚠️ Requires adaptation | 8 | TABLEHEADER, AUTO_LINK_URL, AUTO_LINK_EMAIL, HTML_ENTITY, LIST_BULLET, LIST_ENUMERATOR, COMMENT, REFERENCE |
| ❌ Missing entirely | 3 | TABLEBORDER, NOTE, FRONTMATTER |

---

## 4. Critical Gaps

### 4.1 Missing Syntax Marker Nodes

**Affected types**: `pmh_TABLEBORDER`, `pmh_LIST_BULLET`, `pmh_LIST_ENUMERATOR`

pmh explicitly creates elements for syntax markers (pipe `|` characters, bullet `- * +` characters, enumerator `1.` strings). cmark does **not** — these are structural syntax consumed by the parser and not represented as AST nodes.

**Impact**: vtextedit uses `TABLEBORDER` positions to populate `TableBlock.m_borders` for border highlighting. List bullet/enumerator markers get their own highlight style.

**Mitigation options**:
1. **Post-process**: After parsing, compute marker positions from the block's start position and the known Markdown syntax rules. For tables, scan the raw text within the TABLE node's line range for `|` characters. For lists, compute the marker from ITEM start position + list type.
2. **Patch cmark**: Add marker-span tracking to vnotex/cmark's parser. The parser already knows where these tokens are during lexing — expose them as child nodes or node metadata.
3. **Regex fallback**: Use simple regex on the raw line text within known block boundaries (similar to current `FENCEDCODEBLOCK` post-processing for lang detection).

**Recommendation**: Option 1 (post-process) is cleanest. The positions are deterministic given the block boundaries.

### 4.2 Footnotes (`pmh_NOTE` / `pmh_EXT_NOTES`)

vnotex/cmark does not support footnotes. cmark-gfm supports them via `CMARK_OPT_FOOTNOTES` flag with `CMARK_NODE_FOOTNOTE_DEFINITION` and `CMARK_NODE_FOOTNOTE_REFERENCE` nodes.

**Impact**: vtextedit always enables `pmh_EXT_NOTES`. Footnote syntax (`[^1]`, `[^1]: ...`) would not be highlighted.

**Mitigation options**:
1. **Port from cmark-gfm**: Backport footnote support from cmark-gfm into vnotex/cmark. cmark-gfm's footnote implementation is well-isolated.
2. **Regex overlay**: Parse footnote syntax with a simple regex pass over the raw text, outside the main parser. This is brittle but avoids modifying cmark.
3. **Accept the gap**: If footnote usage is rare in the target user base, defer.

**Recommendation**: Option 1 (port from cmark-gfm). The vnotex fork author has already demonstrated willingness to add node types (formula, mark, table).

### 4.3 YAML Front Matter (`pmh_FRONTMATTER` / `pmh_EXT_FRONTMATTER`)

No cmark variant supports YAML front matter natively.

**Impact**: vtextedit always enables `pmh_EXT_FRONTMATTER`. Front matter blocks (`---\ntitle: ...\n---`) would not be highlighted.

**Mitigation options**:
1. **Pre-parse strip**: Detect front matter before feeding to cmark (check if document starts with `---\n`), record its span, strip it from parser input, and add it as a synthetic element to the result. This is the most common approach in cmark-based tools.
2. **Patch cmark**: Add a `CMARK_NODE_FRONTMATTER` block type. Simple: if document starts with `---\n`, consume lines until closing `---\n`, emit as a single block node.
3. **Regex overlay**: Similar to pre-parse strip but without actually stripping — just record the region.

**Recommendation**: Option 1 (pre-parse strip). It's simple, reliable, and doesn't require modifying the parser.

---

## 5. Position Model Gap (CRITICAL)

This is the most architecturally significant difference.

### Current Model (pmh)

```
pmh_element.pos  → Unicode code-point offset from document start
pmh_element.end  → Unicode code-point offset from document start
```

vtextedit converts these to Qt positions via `tryFixUnicodeData()` which adjusts for UTF-16 surrogate pairs (Qt's `QChar` count ≠ Unicode code-point count for characters outside BMP).

The entire highlight pipeline (`PegParseResult`, `PegHighlighterResult`, `HLUnit`) works with **document-global offsets**.

### New Model (cmark)

```
cmark_node_get_start_line(node)    → 1-indexed line number
cmark_node_get_start_column(node)  → 1-indexed column (byte offset within line)
cmark_node_get_end_line(node)      → 1-indexed line number
cmark_node_get_end_column(node)    → 1-indexed column (byte offset within line)
```

### Conversion Required

To maintain the existing pipeline, each (line, column) pair must be converted to a document-global position:

```cpp
// Pseudocode for position conversion
int lineToPos(QTextDocument *doc, int line, int col) {
    QTextBlock block = doc->findBlockByNumber(line - 1);  // 0-indexed
    return block.position() + col - 1;                     // col is 1-indexed
}
```

**Considerations**:
- This requires access to `QTextDocument` during result processing, which the current `PegParserWorker` thread does NOT have (it receives raw `QString` text, not the document).
- **Option A**: Build a line-offset table (`QVector<int>` mapping line number → cumulative offset) from the raw text before parsing. Pass it alongside parse results. Conversion is O(1) per position.
- **Option B**: Change the pipeline to work with (line, column) pairs natively instead of global offsets. This is a larger refactor but cleaner long-term.

**Recommendation**: Option A (line-offset table). Minimal pipeline disruption. The table is cheap to build (one pass over the input text counting `\n` characters).

### Column Semantics Warning

cmark columns are **byte offsets** within the line (in the input encoding, typically UTF-8). vtextedit works with UTF-16 (`QChar` indices). For ASCII-only content this is identical. For non-ASCII content, byte offset ≠ character offset. You'll need a byte-to-char conversion per line, similar to the existing `tryFixUnicodeData()` but operating on individual lines.

---

## 6. AST Shape Gap

### Current Pipeline (pmh)

```
pmh_markdown_to_elements() → pmh_element **result
                              result[pmh_H1] → linked list of all H1 elements
                              result[pmh_IMAGE] → linked list of all IMAGE elements
                              ...
                              result[pmh_NUM_LANG_TYPES-1] → ...
```

vtextedit iterates this flat structure in two ways:
1. **By type** (pegparser.cpp): `result[pmh_H1]`, `result[pmh_IMAGE]`, etc. — extract specific element types
2. **All types** (peghighlighterresult.cpp): `for i in 0..pmh_NUM_LANG_TYPES` — build per-block highlight units

### New Pipeline (cmark)

```
cmark_parse_document() → cmark_node *doc (tree root)
cmark_iter_new(doc) → tree iterator
  → ENTER HEADING (level=2, line 5, col 1)
    → ENTER TEXT ("Hello", line 5, col 4)
    → EXIT TEXT
  → EXIT HEADING
  → ENTER PARAGRAPH (line 7, col 1)
    → ENTER EMPH (line 7, col 5)
      → ENTER TEXT ("world", line 7, col 6)
      ...
```

### Adaptation Strategy

Build an adapter that walks the cmark AST once and produces the same flat array structure:

```cpp
// Adapter: cmark AST → pmh-compatible flat array
struct HighlightElement {
    int pos;    // document-global offset (converted from line:col)
    int end;    // document-global offset
    HighlightElement *next;
};

// Indexed by style type (same as pmh_element_type mapping)
QVector<HighlightElement *> walkCmarkTree(cmark_node *doc, const QVector<int> &lineOffsets) {
    QVector<HighlightElement *> result(NUM_STYLE_TYPES, nullptr);
    cmark_iter *iter = cmark_iter_new(doc);
    cmark_event_type ev;

    while ((ev = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
        if (ev != CMARK_EVENT_ENTER) continue;

        cmark_node *node = cmark_iter_get_node(iter);
        int styleType = mapNodeTypeToStyle(cmark_node_get_type(node), node);
        if (styleType < 0) continue;

        int startPos = toGlobalPos(lineOffsets,
                                    cmark_node_get_start_line(node),
                                    cmark_node_get_start_column(node));
        int endPos = toGlobalPos(lineOffsets,
                                  cmark_node_get_end_line(node),
                                  cmark_node_get_end_column(node));

        auto *elem = new HighlightElement{startPos, endPos, result[styleType]};
        result[styleType] = elem;  // prepend to linked list
    }
    cmark_iter_free(iter);
    return result;
}
```

This adapter isolates the cmark-specific logic and lets the rest of the pipeline (`PegHighlighterResult`, `PegMarkdownHighlighter`) remain largely unchanged.

---

## 7. Inline Delimiter Positions

**Critical for highlighting**: cmark AST positions for inline nodes (EMPH, STRONG, CODE, STRIKETHROUGH, MARK) point to the **content**, not the delimiter characters.

For `**bold**`:
- cmark reports: start=col 3 (the `b`), end=col 6 (the `d`)
- pmh reports: pos=offset of first `*`, end=offset after last `*`

vtextedit needs the **full span including delimiters** for proper syntax highlighting.

**Mitigation**:
- For known delimiter lengths: `actual_start = start - delimiter_length`
  - EMPH: -1 (`*` or `_`)
  - STRONG: -2 (`**` or `__`)
  - STRIKETHROUGH: -2 (`~~`)
  - MARK: -2 (`==`)
  - CODE: variable (`` ` `` or ` `` `` `)
  - FORMULA_INLINE: -1 (`$`)
- For CODE spans: check `cmark_node_get_literal()` length vs span width to infer backtick count, or inspect the raw text.
- **Alternative**: Patch vnotex/cmark to store delimiter offsets in the node (e.g., `node->start_delimiter_len`).

**Recommendation**: Use the delimiter-length adjustment for most types. Patch cmark for CODE spans where backtick count varies.

---

## 8. Threading Model

### Current (pmh)
- `PegParser` manages 2 `PegParserWorker` QThreads
- Each worker calls `pmh_markdown_to_elements()` independently (re-entrant)
- Parse input is `QByteArray` (UTF-8 from `QString::toUtf8()`)
- Results returned via signal to main thread

### cmark Threading
- `cmark_parser` instances are independent — safe to use from multiple threads
- **Avoid**: `cmark_get_arena_mem_allocator()` (global state)
- **Avoid**: concurrent calls to `cmark_gfm_core_extensions_ensure_registered()` (not applicable to vnotex/cmark which has no extension system)

### Adaptation
Minimal. Replace `pmh_markdown_to_elements()` with `cmark_parse_document()` + tree walk in each worker. Use default allocator (not arena). No mutex needed if each worker creates its own parser instance.

---

## 9. Migration Strategy

### Phase 1: Adapter Layer (non-breaking)
1. Create `CmarkParser` class parallel to `PegParser`
2. Implement line-offset table builder
3. Implement cmark AST → flat element array adapter (§6)
4. Implement delimiter position adjustment (§7)
5. Add post-processing for table borders, list markers (§4.1)
6. Add pre-parse front matter detection (§4.3)

### Phase 2: Integration
7. Add vnotex/cmark as a `libs/` submodule (or bundled source)
8. Wire `CmarkParser` into `PegMarkdownHighlighter` as an alternative backend
9. Run both parsers side-by-side, compare highlight output for correctness
10. Add `CMARK_NODE_FOOTNOTE_*` to vnotex/cmark fork (§4.2) if footnotes needed

### Phase 3: Cutover
11. Remove pmh backend once cmark backend is verified
12. Remove `libs/peg-markdown-highlight/`
13. Rename/refactor "Peg" prefix classes if desired (optional, cosmetic)

---

## 10. Risk Assessment

| Risk | Severity | Likelihood | Mitigation |
|------|----------|------------|------------|
| Inline delimiter positions inaccurate | High | Medium | Delimiter-length adjustment + cmark patch for variable-length delimiters |
| Column byte-offset ≠ UTF-16 char index | High | High (any CJK/emoji content) | Per-line byte→char conversion in adapter |
| Table border positions unavailable | Medium | Certain | Post-process: scan raw text within TABLE node line range |
| Front matter not parsed | Medium | Certain | Pre-parse detection (trivial) |
| Footnotes not supported | Medium | Certain | Port from cmark-gfm or defer |
| cmark inline positions buggy | Medium | Low (recently fixed Aug 2024) | Test thoroughly with edge cases |
| Performance regression from tree walk + position conversion | Low | Low | O(n) walk + O(1) position lookup. Net faster than pmh's O(n²) worst case |
| Breaking change in vnotex/cmark fork | Low | Low | Pin to specific commit; vtextedit author controls both repos |

---

## 11. Conclusion

The replacement is **feasible and beneficial** (better performance, predictable complexity, maintained by the same author). The primary engineering cost is the **position model adapter** (line:column → global offset with UTF-8→UTF-16 conversion) and **syntax marker post-processing** (table borders, list markers). These are well-understood problems with straightforward solutions.

The 3 missing features (front matter, footnotes, table borders) each have clear mitigation paths. No feature gap is a fundamental blocker.

**Recommended approach**: Build an adapter layer that converts cmark output to the existing pipeline's format, minimizing changes to the downstream highlight code. This allows incremental migration and side-by-side validation.
