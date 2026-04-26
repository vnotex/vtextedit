#include "cmarkadapter.h"
#include "highlightelement.h"

#include <node.h>

#include <QString>

// MarkdownSyntaxStyle ordinals (must match pmh_element_type / Theme::MarkdownSyntaxStyle).
enum {
  STYLE_LINK = 0,
  STYLE_AUTO_LINK_URL = 1,
  STYLE_AUTO_LINK_EMAIL = 2,
  STYLE_IMAGE = 3,
  STYLE_CODE = 4,
  STYLE_HTML = 5,
  // STYLE_HTML_ENTITY = 6,
  STYLE_EMPH = 7,
  STYLE_STRONG = 8,
  STYLE_LIST_BULLET = 9,
  STYLE_LIST_ENUMERATOR = 10,
  // STYLE_COMMENT = 11,
  STYLE_H1 = 12,
  // H2=13, H3=14, H4=15, H5=16, H6=17
  STYLE_BLOCKQUOTE = 18,
  STYLE_VERBATIM = 19,
  STYLE_HTMLBLOCK = 20,
  STYLE_HRULE = 21,
  // STYLE_REFERENCE = 22,
  STYLE_FENCEDCODEBLOCK = 23,
  STYLE_NOTE = 24,
  STYLE_STRIKE = 25,
  STYLE_FRONTMATTER = 26,
  STYLE_DISPLAYFORMULA = 27,
  STYLE_INLINEEQUATION = 28,
  STYLE_MARK = 29,
  STYLE_TABLE = 30,
  STYLE_TABLEHEADER = 31,
  // STYLE_TABLEBORDER = 32,
};

// ---------------------------------------------------------------------------
// LineOffsetTable
// ---------------------------------------------------------------------------

// Return the number of bytes in a UTF-8 lead byte's sequence.
static int utf8SeqLen(unsigned char p_byte)
{
  if (p_byte < 0x80) return 1;
  if ((p_byte & 0xE0) == 0xC0) return 2;
  if ((p_byte & 0xF0) == 0xE0) return 3;
  if ((p_byte & 0xF8) == 0xF0) return 4;
  return 1; // Fallback for continuation/invalid bytes.
}

LineOffsetTable::LineOffsetTable(const QByteArray &p_utf8Text)
{
  const unsigned char *data =
      reinterpret_cast<const unsigned char *>(p_utf8Text.constData());
  const int len = p_utf8Text.size();

  // First pass: find line starts (byte offsets).
  m_lineByteOffsets.append(0); // Line 0 starts at byte 0.
  for (int i = 0; i < len; ++i) {
    if (data[i] == '\n' && i + 1 <= len) {
      m_lineByteOffsets.append(i + 1);
    }
  }

  const int lineCount = m_lineByteOffsets.size();
  m_lineQCharOffsets.resize(lineCount);
  m_byteToQChar.resize(lineCount);

  int cumulativeQChars = 0;

  for (int lineIdx = 0; lineIdx < lineCount; ++lineIdx) {
    int lineStart = m_lineByteOffsets[lineIdx];
    int lineEnd = (lineIdx + 1 < lineCount) ? m_lineByteOffsets[lineIdx + 1]
                                             : len;

    m_lineQCharOffsets[lineIdx] = cumulativeQChars;

    int lineLen = lineEnd - lineStart;
    // Build byte→QChar mapping for this line.
    // Index = byte offset within line, value = cumulative QChar count at that byte.
    QVector<int> byteMap(lineLen + 1, 0);

    int qcharCount = 0;
    int pos = 0;
    while (pos < lineLen) {
      int seqLen = utf8SeqLen(data[lineStart + pos]);
      // Clamp to line boundary.
      if (pos + seqLen > lineLen) {
        seqLen = lineLen - pos;
      }
      int qchars = (seqLen == 4) ? 2 : 1; // 4-byte UTF-8 = surrogate pair = 2 QChars.
      for (int j = 1; j < seqLen; ++j) {
        if (pos + j <= lineLen) {
          byteMap[pos + j] = qcharCount;
        }
      }
      qcharCount += qchars;
      pos += seqLen;
      if (pos <= lineLen) {
        byteMap[pos] = qcharCount;
      }
    }

    m_byteToQChar[lineIdx] = byteMap;
    cumulativeQChars += qcharCount;
  }
}

int LineOffsetTable::toDocPosition(int p_line, int p_col) const
{
  // cmark uses 1-indexed lines and columns.
  int lineIdx = p_line - 1;
  int byteCol = p_col - 1; // Convert to 0-indexed byte offset within line.

  if (lineIdx < 0 || lineIdx >= m_lineQCharOffsets.size()) {
    return 0;
  }

  int lineStartQChar = m_lineQCharOffsets[lineIdx];

  if (byteCol <= 0) {
    return lineStartQChar;
  }

  const QVector<int> &byteMap = m_byteToQChar[lineIdx];
  if (byteCol >= byteMap.size()) {
    // Past end of line — return end of line.
    return lineStartQChar + (byteMap.isEmpty() ? 0 : byteMap.last());
  }

  return lineStartQChar + byteMap[byteCol];
}

int LineOffsetTable::lineStartQCharOffset(int p_lineIdx) const
{
  if (p_lineIdx < 0 || p_lineIdx >= m_lineQCharOffsets.size()) {
    return 0;
  }
  return m_lineQCharOffsets[p_lineIdx];
}

int LineOffsetTable::lineCount() const
{
  return m_lineQCharOffsets.size();
}

// ---------------------------------------------------------------------------
// mapCmarkNodeToStyle
// ---------------------------------------------------------------------------

int mapCmarkNodeToStyle(cmark_node_type p_type, cmark_node *p_node)
{
  switch (p_type) {
  case CMARK_NODE_HEADING: {
    int level = cmark_node_get_heading_level(p_node);
    if (level >= 1 && level <= 6) {
      return STYLE_H1 + (level - 1); // H1=12 .. H6=17
    }
    return -1;
  }
  case CMARK_NODE_EMPH:
    return STYLE_EMPH;
  case CMARK_NODE_STRONG:
    return STYLE_STRONG;
  case CMARK_NODE_CODE:
    return STYLE_CODE;
  case CMARK_NODE_CODE_BLOCK:
    // Use internal fenced flag to distinguish fenced vs indented.
    if (p_node->as.code.fenced) {
      return STYLE_FENCEDCODEBLOCK;
    }
    return STYLE_VERBATIM;
  case CMARK_NODE_LINK: {
    const char *url = cmark_node_get_url(p_node);
    if (url) {
      if (strncmp(url, "mailto:", 7) == 0) {
        return STYLE_AUTO_LINK_EMAIL;
      }
    }
    return STYLE_LINK;
  }
  case CMARK_NODE_IMAGE:
    return STYLE_IMAGE;
  case CMARK_NODE_HTML_INLINE:
    return STYLE_HTML;
  case CMARK_NODE_HTML_BLOCK:
    return STYLE_HTMLBLOCK;
  case CMARK_NODE_BLOCK_QUOTE:
    return STYLE_BLOCKQUOTE;
  case CMARK_NODE_THEMATIC_BREAK:
    return STYLE_HRULE;
  case CMARK_NODE_STRIKETHROUGH:
    return STYLE_STRIKE;
  case CMARK_NODE_MARK:
    return STYLE_MARK;
  case CMARK_NODE_FORMULA_INLINE:
    return STYLE_INLINEEQUATION;
  case CMARK_NODE_FORMULA_BLOCK:
    return STYLE_DISPLAYFORMULA;
  case CMARK_NODE_FRONTMATTER:
    return STYLE_FRONTMATTER;
  case CMARK_NODE_TABLE:
    return STYLE_TABLE;
  case CMARK_NODE_TABLE_ROW:
    // Header row → TABLEHEADER; delimiter/data rows → skip.
    if (p_node->as.table_row.type == CMARK_TABLE_ROW_TYPE_HEADER) {
      return STYLE_TABLEHEADER;
    }
    return -1;
  case CMARK_NODE_TABLE_CELL:
    return -1;
  case CMARK_NODE_FOOTNOTE_DEFINITION:
  case CMARK_NODE_FOOTNOTE_REFERENCE:
  case CMARK_NODE_INLINE_FOOTNOTE:
    return STYLE_NOTE;
  case CMARK_NODE_LIST:
    return -1; // Handled specially in walker (synthetic bullet/enumerator).
  case CMARK_NODE_ITEM:
    return -1;
  default:
    return -1;
  }
}

// ---------------------------------------------------------------------------
// Walker helpers
// ---------------------------------------------------------------------------

static HighlightElement *createElement(int p_type, unsigned long p_pos, unsigned long p_end)
{
  auto *elem = new HighlightElement();
  elem->type = p_type;
  elem->pos = p_pos;
  elem->end = p_end;
  elem->next = nullptr;
  return elem;
}

static void prependElement(HighlightElement **p_result, int p_style, HighlightElement *p_elem)
{
  p_elem->next = p_result[p_style];
  p_result[p_style] = p_elem;
}

// Compute the width in bytes of an ordered list number string: e.g. "1" → 1, "10" → 2.
static int numberWidth(int p_num)
{
  if (p_num <= 0) return 1;
  int w = 0;
  int n = p_num;
  while (n > 0) {
    n /= 10;
    ++w;
  }
  return w;
}

static void handleList(cmark_node *p_listNode,
                       const LineOffsetTable &p_offsets,
                       HighlightElement **p_result)
{
  cmark_list_type listType = cmark_node_get_list_type(p_listNode);
  int startNum = cmark_node_get_list_start(p_listNode);

  int itemIdx = 0;
  for (cmark_node *item = cmark_node_first_child(p_listNode);
       item != nullptr;
       item = cmark_node_next(item)) {
    if (cmark_node_get_type(item) != CMARK_NODE_ITEM) {
      continue;
    }

    int sl = cmark_node_get_start_line(item);
    int sc = cmark_node_get_start_column(item);
    int docPos = p_offsets.toDocPosition(sl, sc);

    if (listType == CMARK_BULLET_LIST) {
      // Bullet marker: "-" or "*" → 1 byte (matching pmh which excludes trailing space).
      int span = 1;
      auto *elem = createElement(STYLE_LIST_BULLET,
                                 static_cast<unsigned long>(docPos),
                                 static_cast<unsigned long>(docPos + span));
      prependElement(p_result, STYLE_LIST_BULLET, elem);
    } else if (listType == CMARK_ORDERED_LIST) {
      // Enumerator marker: "1." or "10." etc. (pmh excludes trailing space).
      int num = startNum + itemIdx;
      int span = numberWidth(num) + 1; // digits + "." (no trailing space)
      auto *elem = createElement(STYLE_LIST_ENUMERATOR,
                                 static_cast<unsigned long>(docPos),
                                 static_cast<unsigned long>(docPos + span));
      prependElement(p_result, STYLE_LIST_ENUMERATOR, elem);
    }
    ++itemIdx;
  }
}

// ---------------------------------------------------------------------------
// walkCmarkTree
// ---------------------------------------------------------------------------

void walkCmarkTree(cmark_node *p_doc,
                   const LineOffsetTable &p_offsets,
                   HighlightElement **p_result,
                   int p_numTypes)
{
  Q_UNUSED(p_numTypes);

  cmark_iter *iter = cmark_iter_new(p_doc);
  cmark_event_type ev;

  while ((ev = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
    cmark_node *node = cmark_iter_get_node(iter);
    cmark_node_type type = cmark_node_get_type(node);

    // Skip DOCUMENT node.
    if (type == CMARK_NODE_DOCUMENT) {
      continue;
    }

    // Skip SOFTBREAK (no position [0:0-0:0]).
    if (type == CMARK_NODE_SOFTBREAK) {
      continue;
    }

    // Handle LIST on ENTER: create synthetic bullet/enumerator markers.
    if (type == CMARK_NODE_LIST && ev == CMARK_EVENT_ENTER) {
      handleList(node, p_offsets, p_result);
      continue;
    }

    // Only process on ENTER for non-leaf, or the single event for leaf nodes.
    bool isLeaf = cmark_node_is_leaf(node);
    if (!isLeaf && ev != CMARK_EVENT_ENTER) {
      continue;
    }

    int style = mapCmarkNodeToStyle(type, node);
    if (style < 0 || style >= p_numTypes) {
      continue;
    }

    int sl = cmark_node_get_start_line(node);
    int sc = cmark_node_get_start_column(node);
    int el = cmark_node_get_end_line(node);
    int ec = cmark_node_get_end_column(node);

    // Skip nodes with no position.
    if (sl == 0 && sc == 0 && el == 0 && ec == 0) {
      continue;
    }

    int docStart = p_offsets.toDocPosition(sl, sc);
    // end_column is the last byte (inclusive), so +1 for exclusive end.
    int docEnd = p_offsets.toDocPosition(el, ec) + 1;

    // FORMULA_INLINE: cmark excludes $ delimiters — add them back.
    if (type == CMARK_NODE_FORMULA_INLINE) {
      docStart -= 1;
      docEnd += 1;
    }

    auto *elem = createElement(style,
                               static_cast<unsigned long>(docStart),
                               static_cast<unsigned long>(docEnd));
    prependElement(p_result, style, elem);
  }

  cmark_iter_free(iter);
}

// ---------------------------------------------------------------------------
// freeHighlightElements
// ---------------------------------------------------------------------------

void freeHighlightElements(HighlightElement **p_result, int p_numTypes)
{
  if (!p_result) {
    return;
  }
  for (int i = 0; i < p_numTypes; ++i) {
    HighlightElement *cur = p_result[i];
    while (cur) {
      HighlightElement *next = cur->next;
      delete cur;
      cur = next;
    }
  }
  delete[] p_result;
}

// ---------------------------------------------------------------------------
// parseCmark
// ---------------------------------------------------------------------------

HighlightElement **parseCmark(const QByteArray &p_utf8Text)
{
  if (p_utf8Text.isEmpty()) {
    return nullptr;
  }

  cmark_node *doc = cmark_parse_document(p_utf8Text.constData(),
                                         p_utf8Text.size(),
                                         CMARK_OPT_DEFAULT);
  if (!doc) {
    return nullptr;
  }

  LineOffsetTable offsets(p_utf8Text);

  auto *result = new HighlightElement *[NUM_HIGHLIGHT_STYLES]();
  walkCmarkTree(doc, offsets, result, NUM_HIGHLIGHT_STYLES);

  cmark_node_free(doc);
  return result;
}
