#include "cmarkadapter.h"

#include <node.h>

#ifdef VTE_DEBUG_HIGHLIGHT
#include <QDebug>
#endif

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

  int result = lineStartQChar + byteMap[byteCol];
#ifdef VTE_DEBUG_HIGHLIGHT
  qDebug() << "toDocPosition: line=" << p_line << "col=" << p_col
           << "result=" << result;
#endif
  return result;
}

int LineOffsetTable::qcharWidthAtEndColumn(int p_line, int p_col) const
{
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

bool LineOffsetTable::lineByteRange(int p_lineIdx, int &p_start, int &p_length) const
{
  if (!m_data || p_lineIdx < 0 || p_lineIdx >= m_lineByteOffsets.size()) {
    return false;
  }

  const int lineStart = m_lineByteOffsets[p_lineIdx];
  int lineEnd = (p_lineIdx + 1 < m_lineByteOffsets.size()) ? m_lineByteOffsets[p_lineIdx + 1]
                                                           : m_dataLen;
  // Drop the line terminator.
  while (lineEnd > lineStart && (m_data[lineEnd - 1] == '\n' || m_data[lineEnd - 1] == '\r')) {
    --lineEnd;
  }

  p_start = lineStart;
  p_length = lineEnd - lineStart;
  return true;
}

int LineOffsetTable::lineEndQCharOffset(int p_lineIdx) const
{
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

int LineOffsetTable::lineLeadingSpaces(int p_lineIdx) const
{
  if (!m_data || p_lineIdx < 0 || p_lineIdx >= m_lineByteOffsets.size()) {
    return 0;
  }
  int lineStart = m_lineByteOffsets[p_lineIdx];
  int lineEnd = (p_lineIdx + 1 < m_lineByteOffsets.size())
                    ? m_lineByteOffsets[p_lineIdx + 1]
                    : m_dataLen;
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

int LineOffsetTable::lineStrippedPrefixWidth(int p_lineIdx, int p_blockOffset) const
{
  if (!m_data || p_lineIdx < 0 || p_lineIdx >= m_lineByteOffsets.size()) {
    return 0;
  }
  int lineStart = m_lineByteOffsets[p_lineIdx];
  int lineEnd = (p_lineIdx + 1 < m_lineByteOffsets.size())
                    ? m_lineByteOffsets[p_lineIdx + 1]
                    : m_dataLen;
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


