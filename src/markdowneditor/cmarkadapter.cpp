#include "cmarkadapter.h"

#include "markdownsyntaxstyles.h"

#include <node.h>

#ifdef VTE_DEBUG_HIGHLIGHT
#include <QDebug>
#endif

// ---------------------------------------------------------------------------
// LineOffsetTable
// ---------------------------------------------------------------------------

// Return the number of bytes in a UTF-8 lead byte's sequence.
static int utf8SeqLen(unsigned char p_byte) {
  if (p_byte < 0x80)
    return 1;
  if ((p_byte & 0xE0) == 0xC0)
    return 2;
  if ((p_byte & 0xF0) == 0xE0)
    return 3;
  if ((p_byte & 0xF8) == 0xF0)
    return 4;
  return 1; // Fallback for continuation/invalid bytes.
}

LineOffsetTable::LineOffsetTable(const QByteArray &p_utf8Text) {
  const unsigned char *data = reinterpret_cast<const unsigned char *>(p_utf8Text.constData());
  const int len = p_utf8Text.size();
  m_data = data;
  m_dataLen = len;

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
    int lineEnd = (lineIdx + 1 < lineCount) ? m_lineByteOffsets[lineIdx + 1] : len;

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

int LineOffsetTable::toDocPosition(int p_line, int p_col) const {
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

  int result = lineStartQChar + byteMap[byteCol];
#ifdef VTE_DEBUG_HIGHLIGHT
  qDebug() << "toDocPosition: line=" << p_line << "col=" << p_col << "result=" << result;
#endif
  return result;
}

int LineOffsetTable::qcharWidthAtEndColumn(int p_line, int p_col) const {
  // cmark end_column is 1-indexed and points at the LAST byte of the last
  // character. m_byteToQChar maps a byte offset to the cumulative QChar count at
  // the START of the character containing that byte, and the entry one past the
  // character's last byte holds the next character's start. The difference is the
  // character's QChar width (2 for a surrogate pair, 1 otherwise).
  int lineIdx = p_line - 1;
  if (lineIdx < 0 || lineIdx >= m_byteToQChar.size()) {
    return 1;
  }

  const QVector<int> &byteMap = m_byteToQChar[lineIdx];
  int byteCol = p_col - 1; // 0-indexed last byte of the character.
  // Need both this byte and the following boundary to be within the line.
  if (byteCol < 0 || byteCol + 1 >= byteMap.size()) {
    return 1; // Past line content (e.g. end_column == lineLen + 1): keep +1.
  }

  int width = byteMap[byteCol + 1] - byteMap[byteCol];
  return width > 0 ? width : 1;
}

int LineOffsetTable::lineStartQCharOffset(int p_lineIdx) const {
  if (p_lineIdx < 0 || p_lineIdx >= m_lineQCharOffsets.size()) {
    return 0;
  }
  return m_lineQCharOffsets[p_lineIdx];
}

int LineOffsetTable::lineCount() const { return m_lineQCharOffsets.size(); }

bool LineOffsetTable::lineByteRange(int p_lineIdx, int &p_start, int &p_length) const {
  if (!m_data || p_lineIdx < 0 || p_lineIdx >= m_lineByteOffsets.size()) {
    return false;
  }

  const int lineStart = m_lineByteOffsets[p_lineIdx];
  int lineEnd =
      (p_lineIdx + 1 < m_lineByteOffsets.size()) ? m_lineByteOffsets[p_lineIdx + 1] : m_dataLen;
  // Drop the line terminator.
  while (lineEnd > lineStart && (m_data[lineEnd - 1] == '\n' || m_data[lineEnd - 1] == '\r')) {
    --lineEnd;
  }

  p_start = lineStart;
  p_length = lineEnd - lineStart;
  return true;
}

int LineOffsetTable::lineEndQCharOffset(int p_lineIdx) const {
  int byteStart = 0;
  int byteLength = 0;
  if (!lineByteRange(p_lineIdx, byteStart, byteLength)) {
    return 0;
  }

  const QVector<int> &byteMap = m_byteToQChar[p_lineIdx];
  if (byteMap.isEmpty()) {
    return m_lineQCharOffsets[p_lineIdx];
  }

  const int idx = qMin(byteLength, byteMap.size() - 1);
  return m_lineQCharOffsets[p_lineIdx] + byteMap[idx];
}

int LineOffsetTable::lineLeadingSpaces(int p_lineIdx) const {
  if (!m_data || p_lineIdx < 0 || p_lineIdx >= m_lineByteOffsets.size()) {
    return 0;
  }
  int lineStart = m_lineByteOffsets[p_lineIdx];
  int lineEnd =
      (p_lineIdx + 1 < m_lineByteOffsets.size()) ? m_lineByteOffsets[p_lineIdx + 1] : m_dataLen;
  int count = 0;
  for (int i = lineStart; i < lineEnd; ++i) {
    if (m_data[i] == 0x20) {
      ++count;
    } else {
      break;
    }
  }
  return count;
}

int LineOffsetTable::lineStrippedPrefixWidth(int p_lineIdx, int p_blockOffset) const {
  if (!m_data || p_lineIdx < 0 || p_lineIdx >= m_lineByteOffsets.size()) {
    return 0;
  }
  int lineStart = m_lineByteOffsets[p_lineIdx];
  int lineEnd =
      (p_lineIdx + 1 < m_lineByteOffsets.size()) ? m_lineByteOffsets[p_lineIdx + 1] : m_dataLen;
  int i = lineStart;
  // Leading indentation: list-continuation padding and/or block-quote indent.
  // These are always stripped or skipped by cmark before the first content column.
  while (i < lineEnd && m_data[i] == 0x20) {
    ++i;
  }
  // Block-quote '>' markers, but only while a marker BEGINS no further than
  // block_offset columns into the line. A '>' that begins beyond block_offset is
  // content (e.g. an over-indented ">" inside a list item, which cmark keeps as
  // literal text), not a stripped marker. A '>' beginning exactly at block_offset
  // is always a real marker (relative indent 0), so the bound is inclusive. Each
  // counted '>' also absorbs the spaces cmark skips after it.
  while (i < lineEnd && m_data[i] == '>' && (i - lineStart) <= p_blockOffset) {
    ++i;
    while (i < lineEnd && m_data[i] == 0x20) {
      ++i;
    }
  }
  return i - lineStart;
}

// ---------------------------------------------------------------------------
// mapCmarkNodeToStyle
// ---------------------------------------------------------------------------

int mapCmarkNodeToStyle(cmark_node_type p_type, cmark_node *p_node) {
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
// Source-position mapping
// ---------------------------------------------------------------------------

static cmark_node *findAncestorParagraph(cmark_node *p_node) {
  cmark_node *cur = cmark_node_parent(p_node);
  while (cur) {
    cmark_node_type t = cmark_node_get_type(cur);
    if (t == CMARK_NODE_PARAGRAPH || t == CMARK_NODE_HEADING) {
      return cur;
    }
    cur = cmark_node_parent(cur);
  }
  return nullptr;
}

// Shared body of cmarkNodeSpan()/cmarkNodeUrlSpan(): both take cmark's 1-indexed
// line / byte-column pairs (the end column being INCLUSIVE of the last byte) and
// need the same continuation-line correction, which depends only on the node's
// ancestor paragraph.
static bool cmarkSpanFromCoords(cmark_node *p_node, const LineOffsetTable &p_offsets, int p_sl,
                                int p_sc, int p_el, int p_ec, int &p_startQChar, int &p_endQChar) {
  if (p_sl == 0 && p_sc == 0 && p_el == 0 && p_ec == 0) {
    return false;
  }

  int sl = p_sl;
  int sc = p_sc;
  int el = p_el;
  int ec = p_ec;

  // Correct for cmark's block_offset accounting on continuation lines.
  // cmark adds the paragraph's first-line prefix width (block_offset =
  // start_column - 1) to ALL inline columns, but on any later line only the
  // container prefix actually present on THAT line was stripped/skipped from
  // the content. The true column is therefore
  //   cmark_col - blockOffset + strippedPrefixWidth(line).
  // strippedPrefixWidth counts the leading run of spaces and block-quote '>'
  // markers (markers only while they begin within blockOffset columns). For a
  // pure-space line it equals the leading-space count, so list lazy / aligned /
  // over-indented continuations behave exactly as before; block-quote '>'
  // markers (invisible to a space-only count) are what was missing, which
  // shifted inline highlights inside block quotes.
  cmark_node *para = findAncestorParagraph(p_node);
  if (para) {
    int blockOffset = cmark_node_get_start_column(para) - 1;
    if (blockOffset > 0) {
      int paraStartLine = cmark_node_get_start_line(para);
      int stripSc = -1;
      if (sl > paraStartLine) {
        stripSc = p_offsets.lineStrippedPrefixWidth(sl - 1, blockOffset);
        sc += stripSc - blockOffset;
#ifdef VTE_DEBUG_HIGHLIGHT
        qDebug() << "  CONT FIX sc: line=" << sl << "blockOffset=" << blockOffset
                 << "strip=" << stripSc << "corrected sc=" << sc;
#endif
      }
      if (el > paraStartLine) {
        // Most inline spans are single-line (el == sl); reuse the start-line
        // prefix width instead of rescanning the identical line for the end.
        int strip = (el == sl && stripSc >= 0)
                        ? stripSc
                        : p_offsets.lineStrippedPrefixWidth(el - 1, blockOffset);
        ec += strip - blockOffset;
#ifdef VTE_DEBUG_HIGHLIGHT
        qDebug() << "  CONT FIX ec: line=" << el << "blockOffset=" << blockOffset
                 << "strip=" << strip << "corrected ec=" << ec;
#endif
      }
      // Guard against negative/invalid columns
      sc = qMax(1, sc);
      ec = qMax(sc, ec);
    }
  }

  p_startQChar = p_offsets.toDocPosition(sl, sc);
  // cmark's end_column is the (1-indexed, byte-based) column of the LAST BYTE of
  // the last character (inclusive). Convert to an exclusive QChar end by adding
  // that character's QChar width. A blind "+ 1" under-counts by 1 for 4-byte
  // UTF-8 chars (e.g. emoji), landing in the MIDDLE of a surrogate pair and
  // splitting it into two lone surrogates that render as tofu. For BMP chars
  // (incl. CJK) and past-content end columns the width is 1, so behavior there
  // is unchanged.
  p_endQChar = p_offsets.toDocPosition(el, ec) + p_offsets.qcharWidthAtEndColumn(el, ec);
  return true;
}

bool cmarkNodeSpan(cmark_node *p_node, const LineOffsetTable &p_offsets, int &p_startQChar,
                   int &p_endQChar) {
  return cmarkSpanFromCoords(p_node, p_offsets, cmark_node_get_start_line(p_node),
                             cmark_node_get_start_column(p_node), cmark_node_get_end_line(p_node),
                             cmark_node_get_end_column(p_node), p_startQChar, p_endQChar);
}

bool cmarkNodeUrlSpan(cmark_node *p_node, const LineOffsetTable &p_offsets, int &p_startQChar,
                      int &p_endQChar) {
  int sl = cmark_node_get_url_start_line(p_node);
  if (sl == 0) {
    // Reference-style, or an empty destination: no source bytes to point at.
    return false;
  }

  return cmarkSpanFromCoords(p_node, p_offsets, sl, cmark_node_get_url_start_column(p_node),
                             cmark_node_get_url_end_line(p_node),
                             cmark_node_get_url_end_column(p_node), p_startQChar, p_endQChar);
}