#include "markdownastwalker.h"
#include "../utils/htmltagparse.h"
#include "cmarkadapter.h"
#include "markdownsyntaxstyles.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

#include <QColor>
#include <QStringView>
#include <QThread>

#include <vtextedit/htmlimgscanner.h>
#include <vtextedit/htmltablescanner.h>
#include <vtextedit/markdownutils.h>

#ifdef VTE_DEBUG_HIGHLIGHT
#include <QDebug>
#endif

#include <cmark.h>
#include <node.h>

namespace vte {
namespace md {

bool scanListMarker(const QString &p_line, int p_start, ListItemInfo &p_marker) {
  p_marker = ListItemInfo();
  if (p_start < 0 || p_start >= p_line.size()) {
    return false;
  }

  int end = p_start;
  int number = 0;
  const QChar first = p_line.at(end);
  const bool bullet =
      first == QLatin1Char('-') || first == QLatin1Char('+') || first == QLatin1Char('*');
  if (bullet) {
    ++end;
  } else {
    while (end < p_line.size() && end - p_start < 9 && p_line.at(end) >= QLatin1Char('0') &&
           p_line.at(end) <= QLatin1Char('9')) {
      number = number * 10 + p_line.at(end).unicode() - '0';
      ++end;
    }
    if (end == p_start || end >= p_line.size() ||
        (p_line.at(end) != QLatin1Char('.') && p_line.at(end) != QLatin1Char(')'))) {
      return false;
    }
    ++end;
  }

  // cmark's locale-independent ASCII whitespace class, not Unicode \s.
  if (end < p_line.size()) {
    const ushort ch = p_line.at(end).unicode();
    if (ch != ' ' && (ch < '\t' || ch > '\r')) {
      return false;
    }
  }

  int content = end;
  while (content < p_line.size() && p_line.at(content).isSpace()) {
    ++content;
  }
  bool task = false;
  if (bullet && content < p_line.size() && p_line.at(content) == QLatin1Char('[')) {
    QChar taskMarker;
    bool taskEmpty = false;
    // Keep the existing task spelling policy (unordered [ ] / [x]) in one place.
    task = scanTodoList(p_line.mid(p_start), taskMarker, taskEmpty);
    if (task) {
      content += 3;
      while (content < p_line.size() && p_line.at(content).isSpace()) {
        ++content;
      }
    }
  }

  p_marker.m_markerStart = p_start;
  p_marker.m_markerEnd = end;
  p_marker.m_contentStart = content;
  p_marker.m_sourceNumber = number;
  p_marker.m_marker = p_line.at(end - 1);
  p_marker.m_task = task;
  p_marker.m_empty = content == p_line.size();
  return true;
}

// Reverse only a verified boundary through the existing byte-to-QChar table.
// No re-encoding of the source prefix, including a non-ASCII footnote label.
static int sourceByteColumn(const LineOffsetTable &p_offsets, int p_line, int p_length,
                            int p_position) {
  int lo = 0;
  int hi = p_length;
  while (lo < hi) {
    const int mid = lo + (hi - lo) / 2;
    if (p_offsets.toDocPosition(p_line, mid + 1) < p_position) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return p_offsets.toDocPosition(p_line, lo + 1) == p_position ? lo : -1;
}

// One decoded opening line and one marker are shared by highlighting and the
// collector. No decoded document or tree pointer escapes the lifetime of a walk.
class ListMarkerReader {
public:
  ListMarkerReader(const QByteArray &p_source, const LineOffsetTable &p_offsets)
      : m_source(p_source), m_offsets(p_offsets) {}

  const ListItemInfo &marker(cmark_node *p_node) {
    if (p_node == m_node) {
      return m_marker;
    }
    m_node = p_node;
    m_marker = ListItemInfo();
    const int line = cmark_node_get_start_line(p_node);
    const int column = cmark_node_get_start_column(p_node);
    int start = 0;
    int length = 0;
    if (column < 1 || !m_offsets.lineByteRange(line - 1, start, length) || column > length) {
      return m_marker;
    }
    if (m_line != line) {
      m_line = line;
      m_text = QString::fromUtf8(m_source.constData() + start, length);
    }
    const int lineStart = m_offsets.lineStartQCharOffset(line - 1);
    const int anchor = m_offsets.toDocPosition(line, column) - lineStart;
    ListItemInfo marker;
    if (!scanListMarker(m_text, anchor, marker)) {
      return m_marker;
    }
    const auto &data = p_node->as.list;
    const bool ordered = data.list_type == CMARK_ORDERED_LIST;
    const QChar expected = ordered ? QLatin1Char(data.delimiter == CMARK_PAREN_DELIM ? ')' : '.')
                                   : QLatin1Char(data.bullet_char);
    const int width = marker.m_markerEnd - marker.m_markerStart;
    if (marker.m_marker != expected || (ordered && marker.m_sourceNumber != data.start) ||
        (!ordered && width != 1) || column - 1 + width > length ||
        m_source.at(start + column - 1) != m_text.at(anchor).toLatin1()) {
      return m_marker;
    }
    const int contentByte =
        sourceByteColumn(m_offsets, line, length, lineStart + marker.m_contentStart);
    if (contentByte < column - 1 + width) {
      return m_marker;
    }
    marker.m_markerStart = m_offsets.toDocPosition(line, column);
    marker.m_markerEnd = m_offsets.toDocPosition(line, column + width);
    marker.m_contentStart = m_offsets.toDocPosition(line, contentByte + 1);
    marker.m_sourceValid = marker.m_markerEnd - marker.m_markerStart == width;
    m_marker = std::move(marker);
    return m_marker;
  }

private:
  const QByteArray &m_source;
  const LineOffsetTable &m_offsets;
  cmark_node *m_node = nullptr;
  int m_line = -1;
  QString m_text;
  ListItemInfo m_marker;
};

// Source-coordinate recovery, not list recognition: all membership and padding
// come from the AST. Keep these byte/virtual-column segments for number-width
// adjustments as well as sibling-prefix construction.
struct ListPrefixCursor {
  int m_byte = 0;
  int m_column = 0;
  bool m_partialTab = false;
};

struct ListPrefixSegment {
  enum class Kind { Quote, OpeningItem, OpeningIndent, Indentation, Blank, Lazy };
  int m_container = -1;
  Kind m_kind = Kind::Indentation;
  ListPrefixCursor m_begin;
  ListPrefixCursor m_end;
  ListPrefixCursor m_markerBegin;
  ListPrefixCursor m_markerEnd;
};

static bool listSpaceOrTab(char p_ch) { return p_ch == ' ' || p_ch == '\t'; }

// blocks.c:S_advance_offset, including a tab left partially consumed in place.
static bool advanceListPrefix(const char *p_line, int p_length, ListPrefixCursor &p_cursor,
                              int p_count, bool p_columns) {
  if (p_count < 0) {
    return false;
  }
  while (p_count > 0 && p_cursor.m_byte < p_length) {
    if (p_line[p_cursor.m_byte] == '\t') {
      const int tab = 4 - p_cursor.m_column % 4;
      const int step = p_columns ? qMin(tab, p_count) : tab;
      p_cursor.m_partialTab = p_columns && step < tab;
      p_cursor.m_column += step;
      p_cursor.m_byte += p_cursor.m_partialTab ? 0 : 1;
      p_count -= p_columns ? step : 1;
    } else {
      p_cursor.m_partialTab = false;
      ++p_cursor.m_byte;
      ++p_cursor.m_column;
      --p_count;
    }
  }
  return p_count == 0;
}

// blocks.c:S_find_first_nonspace, without its parser-local cached lookahead.
static ListPrefixCursor firstListNonspace(const char *p_line, int p_length,
                                          ListPrefixCursor p_cursor) {
  while (p_cursor.m_byte < p_length && listSpaceOrTab(p_line[p_cursor.m_byte])) {
    advanceListPrefix(p_line, p_length, p_cursor, 1, false);
  }
  return p_cursor;
}

// blocks.c's opening-marker padding rule, shared by projection and width changes.
// p_cursor starts immediately after the marker, with the current absolute column.
static bool consumeListOpeningPadding(const char *p_line, int p_length, int p_width,
                                      ListPrefixCursor &p_cursor, int &p_padding) {
  const auto afterMarker = p_cursor;
  while (p_cursor.m_column - afterMarker.m_column <= 5 && p_cursor.m_byte < p_length &&
         listSpaceOrTab(p_line[p_cursor.m_byte])) {
    if (!advanceListPrefix(p_line, p_length, p_cursor, 1, true)) {
      return false;
    }
  }
  const int whitespace = p_cursor.m_column - afterMarker.m_column;
  p_padding = p_width + whitespace;
  if (whitespace >= 5 || whitespace < 1 || p_cursor.m_byte == p_length) {
    p_padding = p_width + 1;
    p_cursor = afterMarker;
    if (whitespace > 0 && !advanceListPrefix(p_line, p_length, p_cursor, 1, true)) {
      return false;
    }
  }
  return true;
}

static bool walkListPrefix(const ListStructure &p_structure, const char *p_line, int p_length,
                           const LineOffsetTable &p_offsets, int p_lineNumber, int p_offset,
                           int p_block, int p_container, QVector<int> &p_chain,
                           QVector<ListPrefixSegment> &p_segments, ListPrefixCursor &p_end) {
  p_chain.clear();
  p_segments.clear();
  p_end = ListPrefixCursor();
  for (int index = p_container; index >= 0;) {
    if (index >= p_structure.m_containers.size()) {
      return false;
    }
    const auto &container = p_structure.m_containers.at(index);
    // Containers are appended parent-first; also rules out a corrupt cycle.
    if (container.m_parent >= index || p_block < container.m_startBlock ||
        p_block > container.m_endBlock) {
      return false;
    }
    p_chain.append(index);
    index = container.m_parent;
  }
  std::reverse(p_chain.begin(), p_chain.end());

  bool lazy = false;
  for (int index : p_chain) {
    const auto &container = p_structure.m_containers.at(index);
    ListPrefixSegment segment;
    segment.m_container = index;
    segment.m_begin = p_end;
    const auto first = firstListNonspace(p_line, p_length, p_end);
    const int indent = first.m_column - p_end.m_column;
    const bool blank = first.m_byte == p_length;
    if (lazy) {
      // check_open_blocks stops at the FIRST missing prefix. A later '>' is
      // paragraph content, not permission to restart container recognition.
      segment.m_kind = ListPrefixSegment::Kind::Lazy;
    } else if (container.m_kind == ListContainerInfo::Kind::Quote) {
      if (indent <= 3 && !blank && p_line[first.m_byte] == '>') {
        segment.m_kind = ListPrefixSegment::Kind::Quote;
        if (!advanceListPrefix(p_line, p_length, p_end, indent + 1, true)) {
          return false;
        }
        if (p_end.m_byte < p_length && listSpaceOrTab(p_line[p_end.m_byte])) {
          advanceListPrefix(p_line, p_length, p_end, 1, true);
        }
      } else {
        segment.m_kind = ListPrefixSegment::Kind::Lazy;
        lazy = true;
      }
    } else if (container.m_startBlock == p_block) {
      if (container.m_markerOffset != indent || blank) {
        return false;
      }
      if (!advanceListPrefix(p_line, p_length, p_end, first.m_byte - p_end.m_byte, false)) {
        return false;
      }
      segment.m_markerBegin = p_end;
      if (container.m_kind == ListContainerInfo::Kind::Item) {
        if (container.m_item < 0 || container.m_item >= p_structure.m_items.size()) {
          return false;
        }
        const auto &item = p_structure.m_items.at(container.m_item);
        if (!item.m_sourceValid ||
            p_offsets.toDocPosition(p_lineNumber, first.m_byte + 1) + p_offset !=
                item.m_markerStart) {
          return false;
        }
        const int width = item.m_markerEnd - item.m_markerStart;
        if (!advanceListPrefix(p_line, p_length, p_end, width, false)) {
          return false;
        }
        segment.m_kind = ListPrefixSegment::Kind::OpeningItem;
        segment.m_markerEnd = p_end;
        int padding = 0;
        if (!consumeListOpeningPadding(p_line, p_length, width, p_end, padding)) {
          return false;
        }
        if (padding != container.m_padding) {
          return false;
        }
      } else {
        // Footnote padding stores opener BYTES, including its trailing spaces.
        // Consume those bytes on the opening line; continuation consumes columns.
        if (container.m_padding < 6 || first.m_byte + container.m_padding > p_length ||
            p_line[first.m_byte] != '[' || p_line[first.m_byte + 1] != '^' ||
            !listSpaceOrTab(p_line[first.m_byte + container.m_padding - 1])) {
          return false;
        }
        if (!advanceListPrefix(p_line, p_length, p_end, container.m_padding, false)) {
          return false;
        }
        segment.m_kind = ListPrefixSegment::Kind::OpeningIndent;
        segment.m_markerEnd = p_end;
      }
    } else {
      const int padding = container.m_markerOffset + container.m_padding;
      if (padding < 1) {
        return false;
      }
      // Footnotes test blank before indentation; ITEMs test indentation first.
      // Every non-opening blank here is inside an AST-confirmed extent. No
      // blank-line segment is ever a candidate for an indentation rewrite.
      if (blank && (container.m_kind == ListContainerInfo::Kind::Indent || indent < padding)) {
        segment.m_kind = ListPrefixSegment::Kind::Blank;
        p_end = first;
      } else if (indent >= padding) {
        segment.m_kind = ListPrefixSegment::Kind::Indentation;
        advanceListPrefix(p_line, p_length, p_end, padding, true);
      } else {
        segment.m_kind = ListPrefixSegment::Kind::Lazy;
        lazy = true;
      }
    }
    segment.m_end = p_end;
    p_segments.append(segment);
  }
  return true;
}

// Append an unchanged source interval. A tab survives unless the interval must
// split it or an earlier change moved its tab stop. Do not split at mere AST
// segment boundaries: e.g. the one column consumed by '>\t' stays one raw tab.
static bool appendListPrefix(const char *p_line, int p_length, ListPrefixCursor &p_from,
                             const ListPrefixCursor &p_to, QByteArray &p_output,
                             int &p_outputColumn) {
  if (p_to.m_byte < p_from.m_byte || p_to.m_column < p_from.m_column) {
    return false;
  }
  while (p_from.m_byte < p_to.m_byte || p_from.m_column < p_to.m_column) {
    if (p_from.m_byte >= p_length) {
      return false;
    }
    const char ch = p_line[p_from.m_byte];
    if (ch == '\t') {
      const int available = 4 - p_from.m_column % 4;
      const int columns =
          p_from.m_byte == p_to.m_byte ? p_to.m_column - p_from.m_column : available;
      if (columns < 1 || columns > available) {
        return false;
      }
      if (!p_from.m_partialTab && columns == available && 4 - p_outputColumn % 4 == columns) {
        p_output.append('\t');
      } else {
        p_output.append(columns, ' ');
      }
      p_outputColumn += columns;
      advanceListPrefix(p_line, p_length, p_from, columns, true);
    } else {
      if (static_cast<unsigned char>(ch) >= 0x80) {
        return false;
      }
      p_output.append(ch);
      ++p_outputColumn;
      advanceListPrefix(p_line, p_length, p_from, 1, false);
    }
  }
  return p_from.m_byte == p_to.m_byte && p_from.m_column == p_to.m_column;
}

static bool cacheListPrefix(ListStructure &p_structure, int p_item, const QByteArray &p_source,
                            const LineOffsetTable &p_offsets, int p_offset, int p_startBlock,
                            QVector<int> &p_chain, QVector<ListPrefixSegment> &p_segments) {
  auto &item = p_structure.m_items[p_item];
  if (!item.m_sourceValid) {
    return false;
  }
  const auto &own = p_structure.m_containers.at(item.m_container);
  const int line = item.m_startBlock - p_startBlock;
  int start = 0;
  int length = 0;
  if (!p_offsets.lineByteRange(line, start, length)) {
    return false;
  }
  const char *source = p_source.constData() + start;
  ListPrefixCursor consumed;
  if (!walkListPrefix(p_structure, source, length, p_offsets, line + 1, p_offset, item.m_startBlock,
                      own.m_parent, p_chain, p_segments, consumed)) {
    return false;
  }
  const auto marker = firstListNonspace(source, length, consumed);
  if (p_offsets.toDocPosition(line + 1, marker.m_byte + 1) + p_offset != item.m_markerStart ||
      marker.m_column - consumed.m_column != own.m_markerOffset) {
    return false;
  }

  QByteArray prefix;
  prefix.reserve(marker.m_byte);
  ListPrefixCursor copied;
  int outputColumn = 0;
  for (const auto &segment : p_segments) {
    const auto &container = p_structure.m_containers.at(segment.m_container);
    if (segment.m_kind == ListPrefixSegment::Kind::OpeningItem ||
        segment.m_kind == ListPrefixSegment::Kind::OpeningIndent) {
      if (!appendListPrefix(source, length, copied, segment.m_markerBegin, prefix, outputColumn)) {
        return false;
      }
      const int width = segment.m_kind == ListPrefixSegment::Kind::OpeningItem
                            ? segment.m_markerEnd.m_column - segment.m_markerBegin.m_column
                            : container.m_padding;
      prefix.append(width, ' ');
      outputColumn += width;
      copied = segment.m_markerEnd;
    } else if (segment.m_kind == ListPrefixSegment::Kind::Lazy) {
      if (container.m_kind != ListContainerInfo::Kind::Quote ||
          !appendListPrefix(source, length, copied, segment.m_begin, prefix, outputColumn)) {
        return false;
      }
      prefix.append("> ");
      outputColumn += 2;
    }
  }
  if (!appendListPrefix(source, length, copied, marker, prefix, outputColumn)) {
    return false;
  }
  item.m_siblingPrefix = QString::fromUtf8(prefix);
  return true;
}

bool listContinuationPrefix(const ListStructure &p_structure, int p_item, QString &p_prefix) {
  p_prefix.clear();
  if (!p_structure.m_valid || p_item < 0 || p_item >= p_structure.m_items.size()) {
    return false;
  }
  const auto &item = p_structure.m_items.at(p_item);
  if (!item.m_sourceValid || !item.m_prefixValid) {
    return false;
  }
  p_prefix = item.m_siblingPrefix;
  return true;
}

class ListCollector {
public:
  ListCollector(ListStructure &p_result, const QByteArray &p_source,
                const LineOffsetTable &p_offsets, ListMarkerReader &p_markers, int p_offset,
                int p_startBlock)
      : m_result(p_result), m_source(p_source), m_offsets(p_offsets), m_markers(p_markers),
        m_offset(p_offset), m_startBlock(p_startBlock) {}

  // Numbering also needs item-owned quote/footnote prefixes with no inner LIST.
  void enterNumberContainer(cmark_node *p_node) { ancestorContainers(p_node); }

  void enter(cmark_node *p_node) {
    const auto type = cmark_node_get_type(p_node);
    if (type == CMARK_NODE_LIST) {
      ListInfo list;
      list.m_parentContainer = ancestorContainers(cmark_node_parent(p_node));
      list.m_ordered = cmark_node_get_list_type(p_node) == CMARK_ORDERED_LIST;
      list.m_startNumber = list.m_ordered ? cmark_node_get_list_start(p_node) : 0;
      list.m_marker = list.m_ordered
                          ? QLatin1Char(p_node->as.list.delimiter == CMARK_PAREN_DELIM ? ')' : '.')
                          : QLatin1Char(p_node->as.list.bullet_char);
      m_lists.insert(p_node, m_result.m_lists.size());
      m_result.m_lists.append(std::move(list));
    } else if (type == CMARK_NODE_ITEM) {
      const auto list = m_lists.constFind(cmark_node_parent(p_node));
      if (list == m_lists.cend()) {
        return;
      }
      // ITEM-enter, never a LIST's eager sibling loop: nested same-line markers
      // and all later markers consequently have the same source ordering.
      ListItemInfo item = m_markers.marker(p_node);
      item.m_list = list.value();
      item.m_startBlock = m_startBlock + cmark_node_get_start_line(p_node) - 1;
      item.m_endBlock = m_startBlock + cmark_node_get_end_line(p_node) - 1;
      if (item.m_sourceValid) {
        item.m_markerStart += m_offset;
        item.m_markerEnd += m_offset;
        item.m_contentStart += m_offset;
      }
      cmark_node *child = cmark_node_first_child(p_node);
      // The checkbox is literal paragraph content in this cmark fork. Only
      // that single opening-line paragraph may coexist with an empty task.
      item.m_empty =
          item.m_sourceValid && item.m_empty &&
          (!child || (item.m_task && cmark_node_get_type(child) == CMARK_NODE_PARAGRAPH &&
                      !cmark_node_next(child) &&
                      cmark_node_get_start_line(child) == cmark_node_get_start_line(p_node) &&
                      cmark_node_get_end_line(child) == cmark_node_get_start_line(p_node)));
      const int index = m_result.m_items.size();
      ListContainerInfo container;
      container.m_kind = ListContainerInfo::Kind::Item;
      container.m_parent = m_result.m_lists.at(item.m_list).m_parentContainer;
      container.m_item = index;
      container.m_startBlock = item.m_startBlock;
      container.m_endBlock = item.m_endBlock;
      container.m_markerOffset = p_node->as.list.marker_offset;
      container.m_padding = p_node->as.list.padding;
      item.m_container = m_result.m_containers.size();
      m_containers.insert(p_node, item.m_container);
      m_result.m_containers.append(container);
      m_result.m_lists[item.m_list].m_items.append(index);
      m_result.m_items.append(std::move(item));
      m_result.m_items[index].m_prefixValid = cacheListPrefix(
          m_result, index, m_source, m_offsets, m_offset, m_startBlock, m_chain, m_segments);
    } else if (type == CMARK_NODE_PARAGRAPH &&
               cmark_node_get_type(cmark_node_parent(p_node)) == CMARK_NODE_ITEM) {
      const auto parent = m_containers.constFind(cmark_node_parent(p_node));
      if (parent == m_containers.cend()) {
        return;
      }
      const int start = cmark_node_get_start_line(p_node);
      const int end = cmark_node_get_end_line(p_node);
      if (start > 0 && end >= start && end <= m_offsets.lineCount()) {
        ListParagraphInfo paragraph;
        paragraph.m_startBlock = m_startBlock + start - 1;
        paragraph.m_endBlock = m_startBlock + end - 1;
        paragraph.m_item = m_result.m_containers.at(parent.value()).m_item;
        m_result.m_paragraphs.append(paragraph);
      }
    }
  }

  void finish() {
    if (!m_result.m_lists.isEmpty()) {
      m_result.m_source = m_source;
      std::sort(m_result.m_paragraphs.begin(), m_result.m_paragraphs.end(),
                [](const ListParagraphInfo &a, const ListParagraphInfo &b) {
                  return a.m_startBlock < b.m_startBlock;
                });
    }
    m_result.m_valid = true;
  }

private:
  // Allocate only ancestors actually needed by a LIST. Quotes/footnotes in a
  // list-free document never leave records or an allocated ancestor stack.
  int ancestorContainers(cmark_node *p_node) {
    m_pendingAncestors.clear();
    int parent = -1;
    for (auto *node = p_node; node; node = cmark_node_parent(node)) {
      const auto known = m_containers.constFind(node);
      if (known != m_containers.cend()) {
        parent = known.value();
        break;
      }
      const auto type = cmark_node_get_type(node);
      if (type == CMARK_NODE_BLOCK_QUOTE || type == CMARK_NODE_FOOTNOTE_DEFINITION) {
        m_pendingAncestors.append(node);
      }
    }
    for (int i = m_pendingAncestors.size() - 1; i >= 0; --i) {
      auto *node = m_pendingAncestors.at(i);
      ListContainerInfo container;
      const bool quote = cmark_node_get_type(node) == CMARK_NODE_BLOCK_QUOTE;
      container.m_kind = quote ? ListContainerInfo::Kind::Quote : ListContainerInfo::Kind::Indent;
      container.m_parent = parent;
      container.m_startBlock = m_startBlock + cmark_node_get_start_line(node) - 1;
      container.m_endBlock = m_startBlock + cmark_node_get_end_line(node) - 1;
      if (!quote) {
        container.m_markerOffset = node->as.footnote_def.marker_offset;
        container.m_padding = node->as.footnote_def.padding;
      }
      parent = m_result.m_containers.size();
      m_containers.insert(node, parent);
      m_result.m_containers.append(container);
    }
    return parent;
  }

  ListStructure &m_result;
  const QByteArray &m_source;
  const LineOffsetTable &m_offsets;
  ListMarkerReader &m_markers;
  int m_offset;
  int m_startBlock;
  QHash<cmark_node *, int> m_lists;
  QHash<cmark_node *, int> m_containers;
  QVector<cmark_node *> m_pendingAncestors;
  QVector<int> m_chain;
  QVector<ListPrefixSegment> m_segments;
};

ListStructure parseListStructure(const QByteArray &p_utf8Text, int p_offset, int p_startBlock) {
  ListStructure result;
  if (p_utf8Text.isEmpty()) {
    result.m_valid = true;
    return result;
  }
  cmark_node *doc =
      cmark_parse_document(p_utf8Text.constData(), p_utf8Text.size(), CMARK_OPT_DEFAULT);
  if (!doc) {
    return result;
  }
  cmark_iter *iter = cmark_iter_new(doc);
  if (!iter) {
    cmark_node_free(doc);
    return result;
  }
  LineOffsetTable offsets(p_utf8Text);
  ListMarkerReader markers(p_utf8Text, offsets);
  ListCollector collector(result, p_utf8Text, offsets, markers, p_offset, p_startBlock);
  cmark_event_type event;
  while ((event = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
    if (event == CMARK_EVENT_ENTER) {
      collector.enter(cmark_iter_get_node(iter));
    }
  }
  collector.finish();
  cmark_iter_free(iter);
  cmark_node_free(doc);
  return result;
}

// Number construction is background-only. All positions below refer to the
// immutable original; even nested width changes are composed before publication.
static bool listNumberInterrupted() { return QThread::currentThread()->isInterruptionRequested(); }

using ListNumberTree = std::unique_ptr<cmark_node, decltype(&cmark_node_free)>;
using ListNumberIterator = std::unique_ptr<cmark_iter, decltype(&cmark_iter_free)>;

static int listNumberWidth(int p_number) {
  int width = 1;
  while (p_number >= 10) {
    p_number /= 10;
    ++width;
  }
  return width;
}

// A connected unit is a LIST and all LISTs below its ITEMs, including intervening
// quote/footnote containers. Sibling lists outside any common ITEM are independent.
static bool listNumberRoots(const ListStructure &p_structure, QVector<int> &p_roots,
                            QVector<int> &p_containerLists) {
  p_containerLists.fill(-1, p_structure.m_containers.size());
  for (int index = 0; index < p_structure.m_containers.size(); ++index) {
    const auto &container = p_structure.m_containers.at(index);
    if (container.m_parent < -1 || container.m_parent >= index || container.m_startBlock < 0 ||
        container.m_endBlock < container.m_startBlock) {
      return false;
    }
    int list = container.m_parent < 0 ? -1 : p_containerLists.at(container.m_parent);
    if (container.m_kind == ListContainerInfo::Kind::Item) {
      if (container.m_item < 0 || container.m_item >= p_structure.m_items.size()) {
        return false;
      }
      const auto &item = p_structure.m_items.at(container.m_item);
      if (item.m_container != index || item.m_list < 0 ||
          item.m_list >= p_structure.m_lists.size()) {
        return false;
      }
      list = item.m_list;
    }
    p_containerLists[index] = list;
  }
  p_roots.resize(p_structure.m_lists.size());
  QVector<bool> seen(p_structure.m_items.size(), false);
  for (int index = 0; index < p_structure.m_lists.size(); ++index) {
    const auto &list = p_structure.m_lists.at(index);
    if (list.m_parentContainer < -1 || list.m_parentContainer >= p_containerLists.size()) {
      return false;
    }
    const int parent =
        list.m_parentContainer < 0 ? -1 : p_containerLists.at(list.m_parentContainer);
    if (parent >= index) {
      return false;
    }
    p_roots[index] = parent < 0 ? index : p_roots.at(parent);
    for (int item : list.m_items) {
      if (item < 0 || item >= seen.size() || seen.at(item) ||
          p_structure.m_items.at(item).m_list != index) {
        return false;
      }
      seen[item] = true;
    }
  }
  return std::all_of(seen.cbegin(), seen.cend(), [](bool p_seen) { return p_seen; });
}

static bool sameListNumberAncestors(const ListStructure &p_left, int p_leftContainer,
                                    const ListStructure &p_right, int p_rightContainer) {
  while (p_leftContainer >= 0 && p_rightContainer >= 0) {
    const auto &left = p_left.m_containers.at(p_leftContainer);
    const auto &right = p_right.m_containers.at(p_rightContainer);
    if (left.m_kind != right.m_kind || left.m_item != right.m_item ||
        left.m_startBlock != right.m_startBlock || left.m_endBlock != right.m_endBlock ||
        left.m_markerOffset != right.m_markerOffset || left.m_padding != right.m_padding) {
      return false;
    }
    p_leftContainer = left.m_parent;
    p_rightContainer = right.m_parent;
  }
  return p_leftContainer == p_rightContainer;
}

struct ListNumberPendingEdit {
  ListSourceEdit m_edit;
  int m_unit = -1;
};

// Token boundaries keep delimiters, quote markers, task boxes, and footnote labels
// outside replacements. A split tab may unite two AST segments into one whitespace
// replacement; that is why edits are recovered from the composed prefix, not from
// each ancestor independently.
static bool appendListNumberLineEdits(const QString &p_source, const char *p_line,
                                      int p_prefixLength, const QByteArray &p_prefix,
                                      const LineOffsetTable &p_offsets, int p_block,
                                      const QVector<QPair<int, int>> &p_digits, int p_unit,
                                      QVector<ListNumberPendingEdit> &p_edits) {
  int before = 0;
  int after = 0;
  int digit = 0;
  while (before < p_prefixLength || after < p_prefix.size()) {
    const int begin = before;
    const int replacement = after;
    while (digit < p_digits.size() && p_digits.at(digit).second <= before) {
      ++digit;
    }
    if (digit < p_digits.size() && before == p_digits.at(digit).first) {
      before = p_digits.at(digit).second;
      while (after < p_prefix.size() && p_prefix.at(after) >= '0' && p_prefix.at(after) <= '9') {
        ++after;
      }
      if (after == replacement || after - replacement > 9) {
        return false;
      }
    } else if ((before < p_prefixLength && listSpaceOrTab(p_line[before])) ||
               (after < p_prefix.size() && listSpaceOrTab(p_prefix.at(after)))) {
      while (before < p_prefixLength && listSpaceOrTab(p_line[before])) {
        ++before;
      }
      while (after < p_prefix.size() && listSpaceOrTab(p_prefix.at(after))) {
        ++after;
      }
    } else {
      if (before >= p_prefixLength || after >= p_prefix.size() ||
          p_line[before] != p_prefix.at(after)) {
        return false;
      }
      ++before;
      ++after;
      continue;
    }
    int common = 0;
    while (begin + common < before && replacement + common < after &&
           p_line[begin + common] == p_prefix.at(replacement + common)) {
      ++common;
    }
    int oldEnd = before;
    int newEnd = after;
    while (oldEnd > begin + common && newEnd > replacement + common &&
           p_line[oldEnd - 1] == p_prefix.at(newEnd - 1)) {
      --oldEnd;
      --newEnd;
    }
    if (oldEnd == begin + common && newEnd == replacement + common) {
      continue;
    }
    ListNumberPendingEdit edit;
    edit.m_unit = p_unit;
    edit.m_edit.m_start = p_offsets.toDocPosition(p_block + 1, begin + common + 1);
    edit.m_edit.m_end = p_offsets.toDocPosition(p_block + 1, oldEnd + 1);
    if (edit.m_edit.m_start < 0 || edit.m_edit.m_end < edit.m_edit.m_start ||
        edit.m_edit.m_end > p_source.size()) {
      return false;
    }
    edit.m_edit.m_before =
        p_source.mid(edit.m_edit.m_start, edit.m_edit.m_end - edit.m_edit.m_start);
    edit.m_edit.m_after = QString::fromUtf8(p_prefix.constData() + replacement + common,
                                            newEnd - replacement - common);
    p_edits.append(std::move(edit));
  }
  return true;
}

static bool buildListNumberLine(const QString &p_source, const ListStructure &p_structure,
                                const LineOffsetTable &p_offsets, int p_block, int p_container,
                                const QVector<int> &p_numbers, QVector<int> &p_paddingChanges,
                                QVector<int> &p_chain, QVector<ListPrefixSegment> &p_segments,
                                QByteArray &p_prefix, QVector<QPair<int, int>> &p_digits,
                                int p_unit, QVector<ListNumberPendingEdit> &p_edits) {
  int start = 0;
  int length = 0;
  if (!p_offsets.lineByteRange(p_block, start, length)) {
    return false;
  }
  const char *line = p_structure.m_source.constData() + start;
  ListPrefixCursor consumed;
  if (!walkListPrefix(p_structure, line, length, p_offsets, p_block + 1, 0, p_block, p_container,
                      p_chain, p_segments, consumed)) {
    return false;
  }
  const bool opening =
      std::any_of(p_segments.cbegin(), p_segments.cend(), [](const ListPrefixSegment &p_segment) {
        return p_segment.m_kind == ListPrefixSegment::Kind::OpeningItem ||
               p_segment.m_kind == ListPrefixSegment::Kind::OpeningIndent;
      });
  if (!opening && firstListNonspace(line, length, consumed).m_byte == length) {
    return true; // Container-relative blank lines remain byte-for-byte unchanged.
  }

  p_prefix.clear();
  p_digits.clear();
  ListPrefixCursor copied;
  int column = 0;
  int newConsumedColumn = 0;
  for (const auto &segment : p_segments) {
    const auto &container = p_structure.m_containers.at(segment.m_container);
    if (segment.m_kind == ListPrefixSegment::Kind::Quote) {
      const auto marker = firstListNonspace(line, length, segment.m_begin);
      if (!appendListPrefix(line, length, copied, marker, p_prefix, column) ||
          column < newConsumedColumn || column - newConsumedColumn > 3) {
        return false;
      }
      newConsumedColumn = column + 1;
      if (marker.m_byte + 1 < length && listSpaceOrTab(line[marker.m_byte + 1])) {
        ++newConsumedColumn;
      }
    } else if (segment.m_kind == ListPrefixSegment::Kind::Indentation) {
      const int width = segment.m_end.m_column - segment.m_begin.m_column;
      const int change = p_paddingChanges.at(segment.m_container);
      if (width + change < 1) {
        return false;
      }
      if (change != 0) {
        if (!appendListPrefix(line, length, copied, segment.m_begin, p_prefix, column) ||
            column != newConsumedColumn) {
          return false;
        }
        // Keep whole tabs whose stops still agree. Only the added/removed tail,
        // a split tab, or a tab shifted by an outer change needs respelling.
        auto retained = segment.m_begin;
        if (!advanceListPrefix(line, length, retained, width + qMin(change, 0), true) ||
            !appendListPrefix(line, length, copied, retained, p_prefix, column)) {
          return false;
        }
        if (change > 0) {
          p_prefix.append(change, ' ');
          column += change;
        }
        copied = segment.m_end;
      }
      newConsumedColumn += width + change;
    } else if (segment.m_kind == ListPrefixSegment::Kind::OpeningItem ||
               segment.m_kind == ListPrefixSegment::Kind::OpeningIndent) {
      if (!appendListPrefix(line, length, copied, segment.m_markerBegin, p_prefix, column)) {
        return false;
      }
      const int markerOffset = column - newConsumedColumn;
      if (markerOffset < 0 || markerOffset > 3) {
        return false;
      }
      if (segment.m_kind == ListPrefixSegment::Kind::OpeningIndent) {
        // cmark's footnote padding counts opener bytes, not rendered columns.
        auto end = segment.m_markerBegin;
        end.m_column = column;
        if (!advanceListPrefix(line, length, end,
                               segment.m_markerEnd.m_byte - segment.m_markerBegin.m_byte, false)) {
          return false;
        }
        p_prefix.append(line + segment.m_markerBegin.m_byte,
                        segment.m_markerEnd.m_byte - segment.m_markerBegin.m_byte);
        copied = segment.m_markerEnd;
        column = newConsumedColumn = end.m_column;
        p_paddingChanges[segment.m_container] = markerOffset - container.m_markerOffset;
        continue;
      }
      const auto &item = p_structure.m_items.at(container.m_item);
      const int number = p_numbers.at(container.m_item);
      const int oldWidth = item.m_markerEnd - item.m_markerStart;
      int width = oldWidth;
      if (p_structure.m_lists.at(item.m_list).m_ordered) {
        p_digits.append(qMakePair(segment.m_markerBegin.m_byte, segment.m_markerEnd.m_byte - 1));
      }
      if (number >= 0) {
        const auto digits = QByteArray::number(number);
        p_prefix.append(digits);
        p_prefix.append(item.m_marker.toLatin1());
        width = digits.size() + 1;
      } else {
        p_prefix.append(line + segment.m_markerBegin.m_byte, oldWidth);
      }
      column += width;
      copied = segment.m_markerEnd;
      auto afterMarker = copied;
      afterMarker.m_column = column;
      auto end = afterMarker;
      int padding = 0;
      if (!consumeListOpeningPadding(line, length, width, end, padding)) {
        return false;
      }
      p_paddingChanges[segment.m_container] =
          markerOffset + padding - container.m_markerOffset - container.m_padding;
      newConsumedColumn = end.m_column;
      // Preserve authored marker-to-content whitespace, including tabs. Its new
      // tab stop determines the new padding; it is not a continuation indent.
      const auto rawEnd = firstListNonspace(line, length, copied);
      p_prefix.append(line + copied.m_byte, rawEnd.m_byte - copied.m_byte);
      if (!advanceListPrefix(line, length, afterMarker, rawEnd.m_byte - copied.m_byte, false)) {
        return false;
      }
      column = afterMarker.m_column;
      copied = rawEnd;
    }
  }
  auto end = firstListNonspace(line, length, consumed);
  if (copied.m_byte > end.m_byte) {
    end = copied;
  }
  if (!appendListPrefix(line, length, copied, end, p_prefix, column)) {
    return false;
  }
  return appendListNumberLineEdits(p_source, line, end.m_byte, p_prefix, p_offsets, p_block,
                                   p_digits, p_unit, p_edits);
}

class ListNumberPositionMap {
public:
  explicit ListNumberPositionMap(const QVector<ListSourceEdit> &p_edits) : m_edits(p_edits) {
    m_deltas.reserve(p_edits.size() + 1);
    m_deltas.append(0);
    for (const auto &edit : p_edits) {
      m_deltas.append(m_deltas.constLast() + edit.m_after.size() - (edit.m_end - edit.m_start));
    }
  }

  qint64 map(int p_position) const {
    const auto after = std::upper_bound(
        m_edits.cbegin(), m_edits.cend(), p_position,
        [](int p_pos, const ListSourceEdit &p_edit) { return p_pos < p_edit.m_start; });
    const int count = after - m_edits.cbegin();
    if (count > 0) {
      const auto &edit = m_edits.at(count - 1);
      if (p_position < edit.m_end) {
        return edit.m_start + m_deltas.at(count - 1) +
               qMin<qint64>(p_position - edit.m_start, edit.m_after.size());
      }
    }
    return p_position + m_deltas.at(count);
  }

private:
  const QVector<ListSourceEdit> &m_edits;
  QVector<qint64> m_deltas;
};

static cmark_event_type nextListNumberBlock(cmark_iter *p_iter) {
  cmark_event_type event;
  do {
    event = cmark_iter_next(p_iter);
  } while (event != CMARK_EVENT_DONE && !cmark_node_is_block(cmark_iter_get_node(p_iter)));
  return event;
}

// Raw block starts can point at a partially consumed tab (notably indented code).
// Anchor such a start to its first non-whitespace byte; its relative indentation
// is independently protected by both the prefix construction and HTML comparison.
static bool listNumberBlockPositions(cmark_node *p_node, const QByteArray &p_source,
                                     const LineOffsetTable &p_offsets, int &p_begin, int &p_end) {
  int start = 0;
  int length = 0;
  const int line = cmark_node_get_start_line(p_node);
  int column = cmark_node_get_start_column(p_node) - 1;
  if (column < 0 || !p_offsets.lineByteRange(line - 1, start, length) || column > length) {
    return false;
  }
  while (column < length && listSpaceOrTab(p_source.at(start + column))) {
    ++column;
  }
  p_begin = p_offsets.toDocPosition(line, column + 1);
  const int endLine = cmark_node_get_end_line(p_node);
  const int endColumn = cmark_node_get_end_column(p_node);
  if (endColumn < 0 || !p_offsets.lineByteRange(endLine - 1, start, length) || endColumn > length) {
    return false;
  }
  p_end = p_offsets.toDocPosition(endLine, endColumn + 1);
  return p_end >= p_begin;
}

static bool sameListNumberBlocks(cmark_node *p_original, cmark_node *p_candidate,
                                 const QByteArray &p_before, const QByteArray &p_after,
                                 const LineOffsetTable &p_beforeOffsets,
                                 const LineOffsetTable &p_afterOffsets,
                                 const QVector<ListSourceEdit> &p_edits,
                                 ListCollector &p_collector) {
  ListNumberIterator original(cmark_iter_new(p_original), &cmark_iter_free);
  ListNumberIterator candidate(cmark_iter_new(p_candidate), &cmark_iter_free);
  if (!original || !candidate) {
    return false;
  }
  const ListNumberPositionMap map(p_edits);
  int visited = 0;
  for (;;) {
    if ((++visited & 1023) == 0 && listNumberInterrupted()) {
      return false;
    }
    const auto leftEvent = nextListNumberBlock(original.get());
    const auto rightEvent = nextListNumberBlock(candidate.get());
    if (leftEvent != rightEvent) {
      return false;
    }
    if (leftEvent == CMARK_EVENT_DONE) {
      p_collector.finish();
      return true;
    }
    auto *left = cmark_iter_get_node(original.get());
    auto *right = cmark_iter_get_node(candidate.get());
    const auto type = cmark_node_get_type(left);
    // Matching ENTER/EXIT streams preserve every block's parent, direct sibling
    // count, and list/item ownership, even for equally rendered nested lists.
    if (type != cmark_node_get_type(right)) {
      return false;
    }
    if (leftEvent != CMARK_EVENT_ENTER) {
      continue;
    }
    p_collector.enter(right);
    if (cmark_node_get_start_line(left) != cmark_node_get_start_line(right) ||
        cmark_node_get_end_line(left) != cmark_node_get_end_line(right)) {
      return false;
    }
    if (type == CMARK_NODE_LIST || type == CMARK_NODE_ITEM) {
      if (left->as.list.list_type != right->as.list.list_type ||
          left->as.list.delimiter != right->as.list.delimiter ||
          left->as.list.bullet_char != right->as.list.bullet_char ||
          (type == CMARK_NODE_LIST && (left->as.list.tight != right->as.list.tight ||
                                       left->as.list.start != right->as.list.start))) {
        return false;
      }
    } else if (type != CMARK_NODE_DOCUMENT) {
      int leftStart = 0;
      int leftEnd = 0;
      int rightStart = 0;
      int rightEnd = 0;
      if (!listNumberBlockPositions(left, p_before, p_beforeOffsets, leftStart, leftEnd) ||
          !listNumberBlockPositions(right, p_after, p_afterOffsets, rightStart, rightEnd) ||
          map.map(leftStart) != rightStart || map.map(leftEnd) != rightEnd) {
        return false;
      }
    }
  }
}

bool buildListNumberEdits(const QString &p_source, const ListStructure &p_structure,
                          const QHash<int, int> &p_listStarts, QVector<ListSourceEdit> &p_edits,
                          ListStructure &p_after) {
  p_edits.clear();
  p_after = ListStructure();
  if (!p_structure.m_valid || listNumberInterrupted() ||
      p_source.size() > std::numeric_limits<int>::max() ||
      p_structure.m_source.size() > std::numeric_limits<int>::max()) {
    return false;
  }
  if (p_listStarts.isEmpty()) {
    p_after = p_structure;
    return true;
  }
  QVector<int> roots;
  QVector<int> containerLists;
  if (!listNumberRoots(p_structure, roots, containerLists)) {
    return false;
  }
  QVector<int> starts(p_structure.m_lists.size(), -1);
  QVector<bool> requested(p_structure.m_lists.size(), false);
  QVector<bool> rejected(p_structure.m_lists.size(), false);
  for (auto target = p_listStarts.cbegin(); target != p_listStarts.cend(); ++target) {
    if (target.key() < 0 || target.key() >= starts.size()) {
      return false;
    }
    const int root = roots.at(target.key());
    requested[root] = true;
    const auto &list = p_structure.m_lists.at(target.key());
    if (!list.m_ordered || list.m_items.isEmpty() || target.value() < 0 ||
        target.value() > 999999999 || list.m_items.size() - 1 > 999999999 - target.value()) {
      rejected[root] = true;
    } else {
      starts[target.key()] = target.value();
    }
  }
  for (int listIndex = 0; listIndex < p_structure.m_lists.size(); ++listIndex) {
    const int root = roots.at(listIndex);
    if (!requested.at(root) || rejected.at(root)) {
      continue;
    }
    const auto &list = p_structure.m_lists.at(listIndex);
    int previous = -1;
    for (int index : list.m_items) {
      const auto &item = p_structure.m_items.at(index);
      const int width = item.m_markerEnd - item.m_markerStart;
      bool valid = item.m_sourceValid && item.m_prefixValid && item.m_markerStart > previous &&
                   item.m_markerStart >= 0 && item.m_markerEnd <= p_source.size() && width >= 1 &&
                   item.m_marker == list.m_marker && item.m_startBlock >= 0 &&
                   item.m_endBlock >= item.m_startBlock && item.m_container >= 0 &&
                   item.m_container < p_structure.m_containers.size();
      if (valid) {
        valid = p_source.at(item.m_markerEnd - 1) == list.m_marker;
        if (list.m_ordered) {
          int value = 0;
          valid = valid && width >= 2 && width <= 10;
          for (int position = item.m_markerStart; valid && position < item.m_markerEnd - 1;
               ++position) {
            const ushort ch = p_source.at(position).unicode();
            valid = ch >= '0' && ch <= '9';
            if (valid) {
              value = value * 10 + ch - '0';
            }
          }
          valid = valid && value == item.m_sourceNumber;
        } else {
          valid = valid && width == 1;
        }
      }
      if (!valid) {
        rejected[root] = true;
        break;
      }
      previous = item.m_markerEnd - 1;
    }
  }
  QVector<int> numbers(p_structure.m_items.size(), -1);
  bool accepted = false;
  bool changed = false;
  for (int index = 0; index < starts.size(); ++index) {
    if (starts.at(index) < 0 || rejected.at(roots.at(index))) {
      continue;
    }
    accepted = true;
    const auto &list = p_structure.m_lists.at(index);
    for (int ordinal = 0; ordinal < list.m_items.size(); ++ordinal) {
      const int itemIndex = list.m_items.at(ordinal);
      const auto &item = p_structure.m_items.at(itemIndex);
      const int number = starts.at(index) + ordinal; // Overflow was preflighted above.
      numbers[itemIndex] = number;
      changed = changed || number != item.m_sourceNumber ||
                listNumberWidth(number) != item.m_markerEnd - item.m_markerStart - 1;
    }
  }
  if (!accepted) {
    return false;
  }
  if (!changed) {
    p_after = p_structure;
    return true;
  }
  // Refuse partial/offset projections and mismatched callers. This encoding is
  // temporary; the full QString was decoded only once by the caller's worker.
  if (p_source.toUtf8() != p_structure.m_source || listNumberInterrupted()) {
    return false;
  }
  ListNumberTree original(cmark_parse_document(p_structure.m_source.constData(),
                                               p_structure.m_source.size(), CMARK_OPT_DEFAULT),
                          &cmark_node_free);
  if (!original || listNumberInterrupted()) {
    return false;
  }
  LineOffsetTable offsets(p_structure.m_source);
  ListStructure working;
  ListMarkerReader markers(p_structure.m_source, offsets);
  ListCollector collector(working, p_structure.m_source, offsets, markers, 0, 0);
  QVector<cmark_node *> lists;
  ListNumberIterator iter(cmark_iter_new(original.get()), &cmark_iter_free);
  if (!iter) {
    return false;
  }
  int visited = 0;
  cmark_event_type event;
  while ((event = cmark_iter_next(iter.get())) != CMARK_EVENT_DONE) {
    if ((++visited & 1023) == 0 && listNumberInterrupted()) {
      return false;
    }
    if (event != CMARK_EVENT_ENTER) {
      continue;
    }
    auto *node = cmark_iter_get_node(iter.get());
    const auto type = cmark_node_get_type(node);
    if (type == CMARK_NODE_BLOCK_QUOTE || type == CMARK_NODE_FOOTNOTE_DEFINITION) {
      // These otherwise-unneeded containers matter when a width change shifts a
      // tab inside an item-owned quote containing no descendant LIST.
      collector.enterNumberContainer(node);
    }
    collector.enter(node);
    if (type == CMARK_NODE_LIST) {
      lists.append(node);
    }
  }
  collector.finish();
  iter.reset();
  if (working.m_lists.size() != p_structure.m_lists.size() ||
      working.m_items.size() != p_structure.m_items.size()) {
    return false;
  }
  for (int index = 0; index < working.m_lists.size(); ++index) {
    const int root = roots.at(index);
    if (!requested.at(root) || rejected.at(root)) {
      continue;
    }
    const auto &before = p_structure.m_lists.at(index);
    const auto &actual = working.m_lists.at(index);
    bool valid = before.m_items == actual.m_items && before.m_ordered == actual.m_ordered &&
                 before.m_marker == actual.m_marker &&
                 before.m_startNumber == actual.m_startNumber &&
                 sameListNumberAncestors(p_structure, before.m_parentContainer, working,
                                         actual.m_parentContainer);
    for (int itemIndex : before.m_items) {
      const auto &left = p_structure.m_items.at(itemIndex);
      const auto &right = working.m_items.at(itemIndex);
      valid = valid && right.m_sourceValid && right.m_prefixValid &&
              left.m_startBlock == right.m_startBlock && left.m_endBlock == right.m_endBlock &&
              left.m_markerStart == right.m_markerStart && left.m_markerEnd == right.m_markerEnd &&
              left.m_contentStart == right.m_contentStart &&
              left.m_sourceNumber == right.m_sourceNumber && left.m_marker == right.m_marker &&
              left.m_task == right.m_task &&
              sameListNumberAncestors(p_structure, left.m_container, working, right.m_container);
    }
    if (!valid) {
      rejected[root] = true;
    }
  }
  QVector<int> actualRoots;
  if (!listNumberRoots(working, actualRoots, containerLists) || actualRoots != roots ||
      listNumberInterrupted()) {
    return false;
  }
  QVector<int> paddingChanges(working.m_containers.size(), 0);
  QVector<int> active;
  QVector<int> chain;
  QVector<ListPrefixSegment> segments;
  QByteArray prefix;
  QVector<QPair<int, int>> digits;
  QVector<ListNumberPendingEdit> pending;
  pending.reserve(working.m_items.size());
  int nextContainer = 0;
  for (int block = 0; block < offsets.lineCount(); ++block) {
    if ((block & 1023) == 0 && listNumberInterrupted()) {
      return false;
    }
    while (!active.isEmpty() && working.m_containers.at(active.constLast()).m_endBlock < block) {
      active.removeLast();
    }
    while (nextContainer < working.m_containers.size() &&
           working.m_containers.at(nextContainer).m_startBlock == block) {
      const int parent = working.m_containers.at(nextContainer).m_parent;
      while (!active.isEmpty() && active.constLast() != parent) {
        active.removeLast();
      }
      if ((active.isEmpty() ? -1 : active.constLast()) != parent) {
        return false;
      }
      active.append(nextContainer++);
    }
    if (active.isEmpty()) {
      continue;
    }
    const int container = active.constLast();
    const int list = containerLists.at(container);
    if (list < 0) {
      continue;
    }
    const int root = roots.at(list);
    if (requested.at(root) && !rejected.at(root) &&
        !buildListNumberLine(p_source, working, offsets, block, container, numbers, paddingChanges,
                             chain, segments, prefix, digits, root, pending)) {
      rejected[root] = true;
    }
  }
  if (nextContainer != working.m_containers.size()) {
    return false;
  }
  QVector<ListSourceEdit> edits;
  edits.reserve(pending.size());
  qint64 size = p_source.size();
  int previousEnd = 0;
  for (auto &entry : pending) {
    if (rejected.at(entry.m_unit)) {
      continue;
    }
    auto &edit = entry.m_edit;
    if (edit.m_start < previousEnd || edit.m_start > edit.m_end) {
      return false;
    }
    previousEnd = edit.m_end;
    size += edit.m_after.size() - edit.m_before.size();
    edits.append(std::move(edit));
  }
  accepted = false;
  for (int index = 0; index < starts.size(); ++index) {
    if (starts.at(index) >= 0 && !rejected.at(roots.at(index))) {
      accepted = true;
      if (!cmark_node_set_list_start(lists.at(index), starts.at(index))) {
        return false;
      }
    }
  }
  if (!accepted || size < 0 || size > std::numeric_limits<int>::max() || listNumberInterrupted()) {
    return false;
  }
  if (edits.isEmpty()) {
    p_after = p_structure;
    return true;
  }
  QString candidate;
  candidate.reserve(static_cast<int>(size));
  int copied = 0;
  for (const auto &edit : edits) {
    candidate.append(p_source.constData() + copied, edit.m_start - copied);
    candidate.append(edit.m_after);
    copied = edit.m_end;
  }
  candidate.append(p_source.constData() + copied, p_source.size() - copied);
  const auto candidateUtf8 = candidate.toUtf8();
  if (listNumberInterrupted()) {
    return false;
  }
  ListNumberTree proposed(
      cmark_parse_document(candidateUtf8.constData(), candidateUtf8.size(), CMARK_OPT_DEFAULT),
      &cmark_node_free);
  if (!proposed || listNumberInterrupted()) {
    return false;
  }
  LineOffsetTable candidateOffsets(candidateUtf8);
  ListStructure after;
  ListMarkerReader candidateMarkers(candidateUtf8, candidateOffsets);
  ListCollector candidateCollector(after, candidateUtf8, candidateOffsets, candidateMarkers, 0, 0);
  if (!sameListNumberBlocks(original.get(), proposed.get(), p_structure.m_source, candidateUtf8,
                            offsets, candidateOffsets, edits, candidateCollector) ||
      listNumberInterrupted()) {
    return false;
  }
  for (int index = 0; index < after.m_items.size(); ++index) {
    const auto &item = after.m_items.at(index);
    const int root = roots.at(item.m_list);
    if (requested.at(root) && !rejected.at(root) &&
        (!item.m_sourceValid || !item.m_prefixValid ||
         (numbers.at(index) >= 0 &&
          (item.m_sourceNumber != numbers.at(index) ||
           item.m_markerEnd - item.m_markerStart - 1 != listNumberWidth(numbers.at(index)))))) {
      return false;
    }
  }
  // Render inert strings only. The original tree differs solely in accepted LIST
  // starts; SOURCEPOS is deliberately absent. Each tree owns its allocator.
  char *beforeHtml = cmark_render_html(original.get(), CMARK_OPT_UNSAFE);
  if (!beforeHtml) {
    return false;
  }
  if (listNumberInterrupted()) {
    original->mem->free(beforeHtml);
    return false;
  }
  char *afterHtml = cmark_render_html(proposed.get(), CMARK_OPT_UNSAFE);
  const bool equivalent = afterHtml && std::strcmp(beforeHtml, afterHtml) == 0;
  original->mem->free(beforeHtml);
  proposed->mem->free(afterHtml);
  if (!equivalent || listNumberInterrupted()) {
    return false;
  }
  p_edits = std::move(edits);
  p_after = std::move(after);
  return true;
}

// Whether a multi-line unit of @p_style must leave the leading indentation of
// its 2nd..Nth lines unstyled.
//
// These are the styles the theme gives a MONOSPACE font family to and which can
// span several lines. Indentation is whitespace, and whitespace rendered at a
// monospace advance width is visibly wider than the same whitespace in the body
// font - so a table or an HTML block sitting inside a list item would have its
// continuation lines step sideways relative to the item's first line. Colour
// alone was invisible on whitespace, which is why this only became a problem
// once these styles carried a font.
//
// STYLE_HTML is deliberately absent: an inline HTML tag opens and closes within
// one source line by construction (decision D8), so it never reaches the
// multi-line arm at all.
static bool skipsLeadingIndentation(int p_style) {
  return p_style == STYLE_TABLE || p_style == STYLE_TABLEHEADER ||
         p_style == STYLE_DISPLAYFORMULA || p_style == STYLE_HTMLBLOCK;
}

static void addHLUnit(ASTWalkResult &p_result, const LineOffsetTable &p_offsets, int p_docStart,
                      int p_docEnd, int p_style, int p_startBlock, int p_numBlocks) {
  // Compute 0-indexed line indices from document-local positions.
  // We need to figure out which lines this element spans.
  // docStart and docEnd are relative to the input text (not document-absolute).

  // Find the line for docStart: scan lineStartQCharOffset.
  int startLineIdx = -1;
  int endLineIdx = -1;
  int lc = p_offsets.lineCount();

  // Binary search for startLineIdx: largest lineIdx where lineStartQCharOffset <= docStart.
  {
    int lo = 0, hi = lc - 1;
    while (lo <= hi) {
      int mid = (lo + hi) / 2;
      if (p_offsets.lineStartQCharOffset(mid) <= p_docStart) {
        startLineIdx = mid;
        lo = mid + 1;
      } else {
        hi = mid - 1;
      }
    }
  }

  // Binary search for endLineIdx: largest lineIdx where lineStartQCharOffset < docEnd.
  {
    int lo = 0, hi = lc - 1;
    while (lo <= hi) {
      int mid = (lo + hi) / 2;
      if (p_offsets.lineStartQCharOffset(mid) < p_docEnd) {
        endLineIdx = mid;
        lo = mid + 1;
      } else {
        hi = mid - 1;
      }
    }
  }

  if (startLineIdx < 0 || endLineIdx < 0) {
    return;
  }

  if (startLineIdx == endLineIdx) {
    int blockNum = p_startBlock + startLineIdx;
    if (blockNum >= 0 && blockNum < p_numBlocks) {
      int lineStartQChar = p_offsets.lineStartQCharOffset(startLineIdx);
      HLUnit unit;
      unit.start = p_docStart - lineStartQChar;
      unit.length = p_docEnd - p_docStart;
      unit.styleIndex = p_style;
      p_result.blocksHighlights[blockNum].append(unit);
#ifdef VTE_DEBUG_HIGHLIGHT
      qDebug() << "addHLUnit: blockNum=" << blockNum << "start=" << unit.start
               << "length=" << unit.length << "style=" << unit.styleIndex;
#endif
    }
  } else {
    for (int lineIdx = startLineIdx; lineIdx <= endLineIdx; ++lineIdx) {
      int blockNum = p_startBlock + lineIdx;
      if (blockNum < 0 || blockNum >= p_numBlocks) {
        continue;
      }
      int lineStartQChar = p_offsets.lineStartQCharOffset(lineIdx);
      int nextLineStartQChar =
          (lineIdx + 1 < lc) ? p_offsets.lineStartQCharOffset(lineIdx + 1) : p_docEnd;
      HLUnit unit;
      if (lineIdx == startLineIdx) {
        unit.start = p_docStart - lineStartQChar;
        unit.length = nextLineStartQChar - p_docStart;
      } else if (lineIdx == endLineIdx) {
        int ls = skipsLeadingIndentation(p_style) ? p_offsets.lineLeadingSpaces(lineIdx) : 0;
        unit.start = ls;
        unit.length = p_docEnd - lineStartQChar - ls;
      } else {
        int ls = skipsLeadingIndentation(p_style) ? p_offsets.lineLeadingSpaces(lineIdx) : 0;
        unit.start = ls;
        unit.length = nextLineStartQChar - lineStartQChar - ls;
      }
      unit.styleIndex = p_style;
      if (unit.length > 0) {
        p_result.blocksHighlights[blockNum].append(unit);
      }
    }
  }
}

static void addRegion(ASTWalkResult &p_result, int p_style, int p_absStart, int p_absEnd) {
  ElementRegion region(p_absStart, p_absEnd);
  switch (p_style) {
  case STYLE_IMAGE:
    p_result.imageRegions.append(region);
    break;
  case STYLE_H1:
  case STYLE_H1 + 1:
  case STYLE_H1 + 2:
  case STYLE_H1 + 3:
  case STYLE_H1 + 4:
  case STYLE_H1 + 5:
    p_result.headerRegions.append(region);
    break;
  case STYLE_FENCEDCODEBLOCK:
    if (!p_result.codeBlockRegions.contains(p_absStart)) {
      p_result.codeBlockRegions.insert(p_absStart, region);
    }
    break;
  case STYLE_INLINEEQUATION:
    p_result.inlineEquationRegions.append(region);
    break;
  case STYLE_DISPLAYFORMULA:
    p_result.displayFormulaRegions.append(region);
    break;
  case STYLE_HRULE:
    p_result.hruleRegions.append(region);
    break;
  case STYLE_TABLE:
    p_result.tableRegions.append(region);
    break;
  case STYLE_TABLEHEADER:
    p_result.tableHeaderRegions.append(region);
    break;
  default:
    break;
  }
}

static void addFoldingRegion(ASTWalkResult &p_result, int p_style, int p_startBlock,
                             int p_endBlock) {
  FoldingRegion region;
  region.m_startBlock = p_startBlock;
  region.m_endBlock = p_endBlock;
  if (p_style >= STYLE_H1 && p_style <= STYLE_H1 + 5) {
    region.m_type = FoldingRegionType::Heading;
    region.m_level = p_style - STYLE_H1 + 1;
  } else if (p_style == STYLE_FENCEDCODEBLOCK) {
    region.m_type = FoldingRegionType::FencedCode;
  } else if (p_style == STYLE_BLOCKQUOTE) {
    region.m_type = FoldingRegionType::Blockquote;
  } else if (p_style == STYLE_TABLE) {
    region.m_type = FoldingRegionType::Table;
  } else if (p_style == STYLE_DISPLAYFORMULA) {
    region.m_type = FoldingRegionType::Math;
  } else if (p_style == STYLE_FRONTMATTER) {
    region.m_type = FoldingRegionType::FrontMatter;
  } else {
    return;
  }
  p_result.foldingRegions.append(region);
}

static void handleListDirect(cmark_node *p_itemNode, ListMarkerReader &p_markers,
                             const LineOffsetTable &p_offsets, ASTWalkResult &p_result,
                             int p_startBlock, int p_numBlocks) {
  const int line = cmark_node_get_start_line(p_itemNode) - 1;
  const int block = p_startBlock + line;
  if (block < 0 || block >= p_numBlocks) {
    return;
  }
  const auto &marker = p_markers.marker(p_itemNode);
  if (!marker.m_sourceValid) {
    return;
  }
  HLUnit unit;
  unit.start = marker.m_markerStart - p_offsets.lineStartQCharOffset(line);
  unit.length = marker.m_markerEnd - marker.m_markerStart;
  unit.styleIndex = p_itemNode->as.list.list_type == CMARK_ORDERED_LIST ? STYLE_LIST_ENUMERATOR
                                                                        : STYLE_LIST_BULLET;
  p_result.blocksHighlights[block].append(unit);
}

// Return the raw QChar text of the given 0-indexed source line, excluding the
// line terminator.
static QString lineText(const QByteArray &p_utf8Text, const LineOffsetTable &p_offsets,
                        int p_lineIdx) {
  int start = 0;
  int len = 0;
  if (!p_offsets.lineByteRange(p_lineIdx, start, len)) {
    return QString();
  }

  return QString::fromUtf8(p_utf8Text.constData() + start, len);
}

// Split a table row source line into its block container prefix and its raw
// cells, mirroring cmark's scanner (blocks.c scan_table_row_helper): the row
// starts with '|' right after the prefix, a pipe preceded by an odd number of
// backslashes is escaped, and only whitespace may follow the trailing pipe.
bool splitTableRow(const QString &p_line, QString &p_prefix, QVector<QString> &p_cells,
                   QVector<int> *p_cellOffsets, QVector<int> *p_cellBorders) {
  const int firstPipe = p_line.indexOf(QLatin1Char('|'));
  if (firstPipe < 0) {
    return false;
  }

  p_prefix = p_line.left(firstPipe);
  if (p_cellBorders) {
    p_cellBorders->append(firstPipe);
  }

  // Offset of the first character of the trimmed slice [p_start, p_end).
  // Must use the very same whitespace semantics as QString::trimmed().
  auto trimmedOffset = [&p_line](int p_start, int p_end) {
    int i = p_start;
    while (i < p_end && p_line.at(i).isSpace()) {
      ++i;
    }
    return i;
  };

  int cellStart = firstPipe + 1;
  bool escaped = false;
  bool closed = false;
  for (int i = cellStart; i < p_line.size(); ++i) {
    const QChar ch = p_line.at(i);
    if (escaped) {
      escaped = false;
      continue;
    }

    if (ch == QLatin1Char('\\')) {
      escaped = true;
      continue;
    }

    if (ch == QLatin1Char('|')) {
      p_cells.append(p_line.mid(cellStart, i - cellStart).trimmed());
      if (p_cellBorders) {
        p_cellBorders->append(i);
      }
      if (p_cellOffsets) {
        p_cellOffsets->append(trimmedOffset(cellStart, i));
      }
      cellStart = i + 1;
      closed = true;
    }
  }

  if (!closed) {
    return false;
  }

  return p_line.mid(cellStart).trimmed().isEmpty();
}

// Collect the concatenated literal text of all descendants.
static QString collectLiteralText(cmark_node *p_node) {
  QString text;
  for (cmark_node *child = cmark_node_first_child(p_node); child; child = cmark_node_next(child)) {
    const char *literal = cmark_node_get_literal(child);
    if (literal) {
      text += QString::fromUtf8(literal);
    } else {
      text += collectLiteralText(child);
    }
  }
  return text;
}

// Whether [p_docStart, p_docEnd) is the only non-whitespace content of its line.
//
// True when nothing but whitespace precedes the span on its first line and
// nothing but whitespace follows it on its last line -- the same rule
// PreviewMgr::buildImageLinksForLayout() applies when deciding to paint a
// block-wise preview. The two must not drift apart, or an image would render
// as a block preview in one path and an inline one in the other. Note that
// nothing currently gates that agreement end to end:
// testImageStandaloneMatchesPaintedPath() pins THIS rule against a reference
// implementation, but never runs PreviewMgr's.
//
// This used to be restricted to single-line spans, because cmark collapsed a
// multiline link or image onto the line where parsing finished, making its
// reported start and end mutually inconsistent. cmark now reports the true
// span, so the whole-span rule applies to multiline constructs too.
static bool isStandaloneSpan(const QByteArray &p_utf8Text, const LineOffsetTable &p_offsets,
                             int p_startLine, int p_endLine, int p_docStart, int p_docEnd) {
  const int startLineIdx = p_startLine - 1;
  const int endLineIdx = p_endLine - 1;
  if (startLineIdx < 0 || endLineIdx < startLineIdx) {
    return false;
  }

  const QString startText = lineText(p_utf8Text, p_offsets, startLineIdx);
  const int localStart = p_docStart - p_offsets.lineStartQCharOffset(startLineIdx);
  if (localStart < 0 || localStart > startText.size()) {
    return false;
  }

  const QString endText = lineText(p_utf8Text, p_offsets, endLineIdx);
  const int localEnd = p_docEnd - p_offsets.lineStartQCharOffset(endLineIdx);
  if (localEnd < 0 || localEnd > endText.size()) {
    return false;
  }

  return startText.left(localStart).trimmed().isEmpty() &&
         endText.mid(localEnd).trimmed().isEmpty();
}

// Capture the full structure of a table while the AST and the original input
// are alive. Bails out (producing nothing) whenever the AST rows do not map
// one-to-one onto consecutive source lines of the table's own range, which is
// the only situation where a rewrite could corrupt the document.
static void extractTable(cmark_node *p_tableNode, const LineOffsetTable &p_offsets,
                         const QByteArray &p_utf8Text, ASTWalkResult &p_result, int p_offset,
                         int p_startBlock) {
  const int startLine = cmark_node_get_start_line(p_tableNode);
  const int endLine = cmark_node_get_end_line(p_tableNode);
  if (startLine <= 0 || endLine < startLine) {
    return;
  }

  TableElement table;
  table.m_columns = p_tableNode->as.table.columns_cnt;
  if (table.m_columns <= 0) {
    return;
  }

  table.m_alignments.reserve(table.m_columns);
  for (int i = 0; i < table.m_columns; ++i) {
    table.m_alignments.append(p_tableNode->as.table.alignments
                                  ? static_cast<int>(p_tableNode->as.table.alignments[i])
                                  : 0);
  }

  int expectedLine = startLine;
  for (cmark_node *row = cmark_node_first_child(p_tableNode); row; row = cmark_node_next(row)) {
    if (cmark_node_get_type(row) != CMARK_NODE_TABLE_ROW) {
      return;
    }

    if (cmark_node_get_start_line(row) != expectedLine || expectedLine > endLine) {
      return;
    }

    TableRowElement rowElement;
    switch (row->as.table_row.type) {
    case CMARK_TABLE_ROW_TYPE_HEADER:
      rowElement.m_type = TableRowType::Header;
      break;
    case CMARK_TABLE_ROW_TYPE_DELIMITER:
      rowElement.m_type = TableRowType::Delimiter;
      break;
    default:
      rowElement.m_type = TableRowType::Data;
      break;
    }

    if (!splitTableRow(lineText(p_utf8Text, p_offsets, expectedLine - 1), rowElement.m_prefix,
                       rowElement.m_cells, &rowElement.m_cellOffsets)) {
      return;
    }
    table.m_rows.append(rowElement);
    ++expectedLine;
  }

  if (expectedLine - 1 != endLine || table.m_rows.size() < 2) {
    return;
  }

  if (table.m_rows[0].m_type != TableRowType::Header ||
      table.m_rows[1].m_type != TableRowType::Delimiter) {
    return;
  }

  table.m_startPos = p_offset + p_offsets.lineStartQCharOffset(startLine - 1);
  table.m_endPos = p_offset + p_offsets.lineEndQCharOffset(endLine - 1);
  if (table.m_endPos <= table.m_startPos) {
    return;
  }

  table.m_startBlock = p_startBlock + startLine - 1;

  // The logical grid of a pipe table is the degenerate case: every slot is a
  // 1x1 origin. Its width is the NORMALIZED widest value row -- what
  // TablePreviewDocument::setTable() already builds -- which a ragged body row
  // may push past the declared m_columns. The two stay separate on purpose:
  // m_columns keeps meaning "declared by the header/delimiter rows", so
  // isRoundTrippable()'s declared-vs-actual width check stays meaningful.
  table.m_syntax = TableElement::Syntax::Markdown;
  table.m_markdownBacked = true;
  table.m_hasHeaderRow = true;
  for (const auto &row : table.m_rows) {
    if (row.m_type == TableRowType::Delimiter) {
      continue;
    }
    ++table.m_rowCount;
    table.m_columnCount = qMax(table.m_columnCount, row.m_cells.size());
  }

  p_result.tableElements.append(table);
}

// The 0-indexed line containing the document position @p_pos, or -1.
static int lineIndexOfDocPos(const LineOffsetTable &p_offsets, int p_pos) {
  int lo = 0;
  int hi = p_offsets.lineCount() - 1;
  if (hi < 0 || p_pos < p_offsets.lineStartQCharOffset(0)) {
    return -1;
  }
  while (lo < hi) {
    const int mid = (lo + hi + 1) / 2;
    if (p_offsets.lineStartQCharOffset(mid) <= p_pos) {
      lo = mid;
    } else {
      hi = mid - 1;
    }
  }
  return lo;
}

// Append only source-local, one-line payloads, adding the document offset once.
static void appendConcealRange(QVector<ConcealRange> &p_ranges, const LineOffsetTable &p_offsets,
                               int p_start, int p_end, MarkdownConcealElement p_element,
                               int p_offset) {
  if (p_start < 0 || p_end <= p_start || p_offset < 0 ||
      p_end > std::numeric_limits<int>::max() - p_offset) {
    return;
  }
  const int line = lineIndexOfDocPos(p_offsets, p_start);
  if (line < 0 || p_end > p_offsets.lineEndQCharOffset(line)) {
    return;
  }
  ConcealRange range;
  range.m_startPos = p_offset + p_start;
  range.m_endPos = p_offset + p_end;
  range.m_element = p_element;
  p_ranges.append(range);
}

static void extractInlineConcealRange(cmark_node *p_node, const QByteArray &p_source,
                                      const LineOffsetTable &p_offsets,
                                      QVector<ConcealRange> &p_ranges,
                                      MarkdownConcealElement p_element, int p_offset) {
  const int line = cmark_node_get_url_start_line(p_node);
  int lineStart = 0;
  int lineLength = 0;
  int start = 0;
  int end = 0;
  if (line <= 0 || line != cmark_node_get_url_end_line(p_node) ||
      !p_offsets.lineByteRange(line - 1, lineStart, lineLength) ||
      lineLength == std::numeric_limits<int>::max() ||
      !cmarkNodeUrlSpan(p_node, p_offsets, start, end) ||
      start < p_offsets.lineStartQCharOffset(line - 1) || end <= start ||
      end > p_offsets.lineEndQCharOffset(line - 1)) {
    return;
  }

  // Inspect only the raw boundary bytes; decoding the whole document is unnecessary.
  const int startByte = sourceByteColumn(p_offsets, line, lineLength, start);
  const int endByte = sourceByteColumn(p_offsets, line, lineLength, end);
  if (startByte < 0 || endByte <= startByte || endByte > lineLength) {
    return;
  }
  if (p_source.at(lineStart + startByte) == '<' && p_source.at(lineStart + endByte - 1) == '>') {
    ++start;
    --end;
  }
  appendConcealRange(p_ranges, p_offsets, start, end, p_element, p_offset);
}

struct ReferenceConcealContext {
  const QByteArray &m_source;
  const LineOffsetTable &m_offsets;
  QVector<ConcealRange> &m_ranges;
  int m_offset;
};

static void collectReferenceDestination(void *p_userData, int p_lineNumber, const char *p_lineText,
                                        int p_lineLength, int p_urlStart, int p_urlEnd) {
  const auto &context = *static_cast<ReferenceConcealContext *>(p_userData);
  int rawStart = 0;
  int rawLength = 0;
  if (!p_lineText || p_lineNumber <= 0 || p_urlStart < 0 || p_urlEnd <= p_urlStart ||
      p_urlEnd > p_lineLength ||
      !context.m_offsets.lineByteRange(p_lineNumber - 1, rawStart, rawLength) ||
      rawLength == std::numeric_limits<int>::max()) {
    return;
  }

  // Container stripping can synthesize partial-tab spaces. Remove only leading
  // indentation, then require the entire remaining callback line to be the raw
  // source line's suffix. Never search for a URL also present in a label/title.
  int indent = 0;
  while (indent < p_lineLength && (p_lineText[indent] == ' ' || p_lineText[indent] == '\t')) {
    ++indent;
  }
  const int suffixLength = p_lineLength - indent;
  if (p_urlStart < indent || suffixLength > rawLength) {
    return;
  }
  const int rawColumn = rawLength - suffixLength;
  if (std::memcmp(context.m_source.constData() + rawStart + rawColumn, p_lineText + indent,
                  suffixLength) != 0) {
    return;
  }
  if (p_lineText[p_urlStart] == '<' && p_lineText[p_urlEnd - 1] == '>') {
    ++p_urlStart;
    --p_urlEnd;
  }
  if (p_urlEnd <= p_urlStart) {
    return;
  }
  const int start =
      context.m_offsets.toDocPosition(p_lineNumber, rawColumn + (p_urlStart - indent) + 1);
  const int end =
      context.m_offsets.toDocPosition(p_lineNumber, rawColumn + (p_urlEnd - indent) + 1);
  appendConcealRange(context.m_ranges, context.m_offsets, start, end,
                     MarkdownConcealElement::ReferenceUrl, context.m_offset);
}

// Capture every HTML `<img …>` of one already-resolved slice as an
// ImageElement, so the live editor previews and menus treat it exactly like a
// Markdown image link.
static void extractHtmlImages(const QString &p_slice, int p_sliceStart, const QString &p_text,
                              const QByteArray &p_utf8Text, const LineOffsetTable &p_offsets,
                              ASTWalkResult &p_result, int p_offset, RawTextState &p_rawText) {
  Q_UNUSED(p_text);
  const auto tags = scanHtmlImgTags(p_slice, p_sliceStart, &p_rawText);
  for (const auto &tag : tags) {
    if (const auto *src = tag.attr("src")) {
      appendConcealRange(p_result.concealRanges, p_offsets, src->m_valueStart, src->m_valueEnd,
                         MarkdownConcealElement::ImageUrl, p_offset);
    }
    ImageElement image;
    image.m_startPos = p_offset + tag.m_tagStart;
    image.m_endPos = p_offset + tag.m_tagEnd;
    image.m_destination = tag.src();
    image.m_alternateText = tag.alt();
    image.m_title = tag.title();
    image.m_width = tag.width();
    image.m_height = tag.height();
    image.m_syntax = ImageLinkInfo::Syntax::Html;

    // A tag is single-line by construction (see scanHtmlImgTags), so one line
    // index answers for both ends.
    const int lineIdx = lineIndexOfDocPos(p_offsets, tag.m_tagStart);
    image.m_standalone =
        lineIdx >= 0 && isStandaloneSpan(p_utf8Text, p_offsets, lineIdx + 1, lineIdx + 1,
                                         tag.m_tagStart, tag.m_tagEnd);
    p_result.imageElements.append(image);
  }
}

// Accumulate the rendered text of a heading's inline subtree.
//
// @p_title is what the reader sees (approximates the browser's textContent of
// the rendered <h1..h6>); @p_anchorText mirrors markdown-it-anchor, which slugs
// the concatenated content of only the `text` and `code_inline` tokens of the
// heading's inlines. The two are therefore built with DIFFERENT whitespace
// rules and only @p_title may be trimmed afterwards: markdown-it-anchor
// concatenates raw token content, so a heading like `# a ![x](y)` keeps the
// trailing space of its text token and slugs to `a-`, and a soft break
// contributes nothing at all (`Foo\nbar` under a setext rule slugs to `foobar`).
//
// All literals come from cmark_node_get_literal(), which is already
// entity-decoded and owned by the node, so there is no escaping, unescaping or
// freeing anywhere in this path.
//
// Known limitation: cmark is always run with CMARK_OPT_DEFAULT, i.e. raw inline
// HTML is always recognized as such. The preview passes markdown-it
// `html: enableHtmlTag`, so with that setting OFF the preview shows the escaped
// tags as visible text while this extractor still drops them. Same class of
// divergence as the preview-only plugins (emoji, texmath, footnotes) documented
// in the plan: the authoritative path for a user-visible link stays
// MarkdownViewerAdapter::fetchHeadingAnchor.
static void appendHeadingText(cmark_node *p_node, QString &p_title, QString &p_anchorText) {
  for (cmark_node *child = cmark_node_first_child(p_node); child; child = cmark_node_next(child)) {
    const cmark_node_type type = cmark_node_get_type(child);
    switch (type) {
    case CMARK_NODE_TEXT:
    case CMARK_NODE_CODE:
    case CMARK_NODE_FORMULA_INLINE: {
      const char *literal = cmark_node_get_literal(child);
      if (literal) {
        const QString text = QString::fromUtf8(literal);
        p_title += text;
        p_anchorText += text;
      }
      break;
    }

    case CMARK_NODE_SOFTBREAK:
    case CMARK_NODE_LINEBREAK:
      // A break is whitespace in the rendered heading, but markdown-it-anchor
      // drops the token entirely, so it must NOT reach the anchor input.
      p_title += QStringLiteral(" ");
      break;

    case CMARK_NODE_IMAGE:
      // The alt text is not part of the rendered textContent.
      break;

    case CMARK_NODE_HTML_INLINE:
      // A leaf whose literal is the tag itself: `## <b>x</b>` renders as `x`.
      break;

    case CMARK_NODE_FOOTNOTE_REFERENCE:
    case CMARK_NODE_INLINE_FOOTNOTE:
      // Rendered as a numbered `[n]` marker, never as the footnote body, and
      // dropped from the slug input by markdown-it-anchor. Recursing into the
      // inline form would splice the note body into the heading title, so both
      // forms are skipped explicitly rather than left to the default branch.
      break;

    default:
      // Emphasis, strong, links, strikethrough and any fork extension with
      // children: recurse. Unknown leaves contribute nothing.
      appendHeadingText(child, p_title, p_anchorText);
      break;
    }
  }
}

static void extractHeading(cmark_node *p_node, ASTWalkResult &p_result, int p_absStart,
                           int p_absEnd) {
  HeadingInfo heading;
  heading.m_startPos = p_absStart;
  heading.m_endPos = p_absEnd;
  heading.m_level = cmark_node_get_heading_level(p_node);
  appendHeadingText(p_node, heading.m_title, heading.m_anchorText);
  // Only the display title is trimmed; see the note above on why the anchor
  // input must keep markdown-it-anchor's raw concatenation.
  heading.m_title = heading.m_title.trimmed();
  p_result.headingElements.append(heading);
}

static int alignmentOrdinal(const QString &p_align) {
  if (p_align == QStringLiteral("left")) {
    return 1;
  }
  if (p_align == QStringLiteral("center")) {
    return 2;
  }
  if (p_align == QStringLiteral("right")) {
    return 3;
  }
  return 0;
}

// Whether @p_node is a TOP-LEVEL block, i.e. a direct child of the document.
//
// Decision D-a made structural rather than textual. The whitespace check below
// cannot answer this on its own: a list item's continuation indent IS
// whitespace, so an HTML block indented under `- item` would pass it while
// living under CMARK_NODE_ITEM. Such a table can only be written back as source
// that either no longer previews or that the host's prefix check reads as a
// changed wrapper chain -- and a multi-line replacement would escape the list
// entirely, since only its first line inherits the retained prefix.
static bool isDocumentChild(cmark_node *p_node) {
  cmark_node *parent = cmark_node_parent(p_node);
  return parent && cmark_node_get_type(parent) == CMARK_NODE_DOCUMENT;
}

// Whether @p_table occupies the WHOLE of the resolved HTML block

// [@p_blockStart, @p_blockEnd) apart from surrounding whitespace, and every one
// of its source lines starts at the block's own column.
//
// This is decision D-a made concrete. It is what refuses
// `<div><table>…</table></div>` and a table under `> ` or a list indent: such a
// table can only be written back as source that either no longer previews, or
// that the host's prefix check reads as a changed wrapper chain. A refused
// table simply renders as plain source, exactly as before this feature existed.
static bool isTopLevelWholeBlock(const QString &p_text, const LineOffsetTable &p_offsets,
                                 const HtmlTable &p_table, int p_blockStart, int p_blockEnd) {
  if (p_table.m_tableStart < p_blockStart || p_table.m_tableEnd > p_blockEnd) {
    return false;
  }
  if (!p_text.mid(p_blockStart, p_table.m_tableStart - p_blockStart).trimmed().isEmpty()) {
    return false;
  }
  if (!p_text.mid(p_table.m_tableEnd, p_blockEnd - p_table.m_tableEnd).trimmed().isEmpty()) {
    return false;
  }

  const int firstLine = lineIndexOfDocPos(p_offsets, p_table.m_tableStart);
  const int lastLine = lineIndexOfDocPos(p_offsets, p_table.m_tableEnd - 1);
  if (firstLine < 0 || lastLine < firstLine) {
    return false;
  }

  const int column = p_table.m_tableStart - p_offsets.lineStartQCharOffset(firstLine);
  if (column < 0) {
    return false;
  }
  for (int line = firstLine; line <= lastLine; ++line) {
    const int lineStart = p_offsets.lineStartQCharOffset(line);
    const int lineEnd = p_offsets.lineEndQCharOffset(line);
    if (lineStart + column > lineEnd) {
      return false;
    }
    // Every line must begin at the SAME column, and everything before it must
    // be whitespace: any container prefix (`> `, a list indent) shows up here.
    if (!p_text.mid(lineStart, column).trimmed().isEmpty()) {
      return false;
    }
  }
  return true;
}

// How many nonempty snippet parse attempts live-cell highlighting has performed.
// See inlineSnippetParseCount(); diagnostics only, measuring live-cell cache misses.
static quint64 s_inlineSnippetParses = 0;

// Capture a canonical top-level `<table>` HTML block as a TableElement, so the
// interactive table preview binds to it exactly as it does to a pipe table.
static void extractHtmlTables(const QString &p_slice, int p_sliceStart, const QString &p_text,
                              const LineOffsetTable &p_offsets, ASTWalkResult &p_result,
                              int p_offset, int p_startBlock, bool p_isBlock, int p_blockStart,
                              int p_blockEnd, RawTextState &p_rawText) {
  const auto tables = scanHtmlTables(p_slice, p_sliceStart, &p_rawText);
  if (!p_isBlock) {
    // HTML_INLINE never yields a table (D-a). The scan still ran, purely so its
    // raw-text state advances in lockstep with the image scan's.
    return;
  }

  for (const auto &html : tables) {
    if (!isTopLevelWholeBlock(p_text, p_offsets, html, p_blockStart, p_blockEnd)) {
      continue;
    }

    TableElement table;
    table.m_syntax = TableElement::Syntax::Html;
    table.m_startBlock = -1;
    table.m_startPos = p_offset + html.m_tableStart;
    table.m_endPos = p_offset + html.m_tableEnd;
    table.m_rowCount = html.m_rowCount;
    table.m_columnCount = html.m_columnCount;
    table.m_columns = html.m_columnCount;
    table.m_hasHeaderRow = html.m_hasHeaderRow;
    table.m_openTag = p_text.mid(html.m_openTagStart, html.m_openTagEnd - html.m_openTagStart);

    // Decision D-j / D-n: backing is per TABLE. One malformed payload makes the
    // whole table HTML-only, so a mutation never has to reconcile two kinds of
    // cell and a half-decoded table can never be written back.
    table.m_markdownBacked = html.m_anyPayloadPresent && !html.m_anyPayloadMalformed;

    table.m_alignments.reserve(html.m_columnCount);
    for (const auto &align : html.m_alignments) {
      table.m_alignments.append(alignmentOrdinal(align));
    }

    for (int r = 0; r < html.m_rows.size(); ++r) {
      const auto &htmlRow = html.m_rows.at(r);
      TableRowElement row;
      row.m_type = (r == 0 && html.m_hasHeaderRow) ? TableRowType::Header : TableRowType::Data;
      row.m_rowTag = p_text.mid(htmlRow.m_tagStart, htmlRow.m_tagEnd - htmlRow.m_tagStart);

      for (const auto &cell : htmlRow.m_cells) {
        // A well-formed payload is the cell's Markdown source; a cell without
        // one in a Markdown-backed table is read as Markdown too (D-j), and an
        // HTML-only table shows its raw inner source as spelled (D-d).
        const QString text =
            (table.m_markdownBacked && cell.m_hasPayload) ? cell.m_payload : cell.m_inner;
        row.m_cells.append(text);
        // Markdown-only: there is no one-source-line-per-row correspondence, so
        // no cell offset is meaningful here.
        row.m_cellOffsets.append(-1);
        row.m_colSpans.append(cell.m_colSpan);
        row.m_rowSpans.append(cell.m_rowSpan);
        row.m_slotColumns.append(cell.m_origin.x());
        row.m_cellTags.append(p_text.mid(cell.m_tagStart, cell.m_tagEnd - cell.m_tagStart));
      }

      table.m_rows.append(row);
    }

    p_result.tableElements.append(table);

    // The fold region is emitted HERE, from the scanner's exact span, and never
    // from the enclosing HTML block node.
    //
    // A CommonMark type-6 HTML block -- which `<table>` opens -- is terminated
    // only by a blank line or by EOF, NEVER by `</table>`. Its reported
    // end_line is not even that: resolveHtmlNodeSpan() deliberately extends
    // past it, because cmark reports the last line CONSUMED before the end
    // condition matched. A fold derived from the node would therefore run to
    // the end of the document whenever the table is not followed by a blank
    // line. m_tableStart/m_tableEnd stop precisely at `</table>`.
    //
    // It is also what makes the sheet's source auto-fold at all:
    // MarkdownFoldingProvider::applyPreviewAutoFold() iterates FOLDING REGIONS
    // and matches a preview whose extent equals one exactly, so an element with
    // no region of its own is never even visited.
    const int firstLine = lineIndexOfDocPos(p_offsets, html.m_tableStart);
    const int lastLine = lineIndexOfDocPos(p_offsets, html.m_tableEnd - 1);
    if (firstLine >= 0 && lastLine >= firstLine) {
      FoldingRegion region;
      region.m_type = FoldingRegionType::Table;
      region.m_startBlock = p_startBlock + firstLine;
      region.m_endBlock = p_startBlock + lastLine;
      // A whole table spelled on one line has nothing to fold, and the provider
      // drops a one-block region anyway.
      if (region.m_endBlock > region.m_startBlock) {
        p_result.foldingRegions.append(region);
      }
    }
  }
}

namespace {

struct FontColorSpan {
  int start;
  int end;
  QColor foreground;
};

// Only HTML nodes feed this lexer. Pair first, then publish: an unfinished font
// never lends its color to the rest of the document, even across block nodes.
class FontColorCollector {
public:
  void exclude(int p_start, int p_end) {
    if (!m_open.isEmpty() && p_start < p_end) {
      m_excluded.append(ElementRegion(p_start, p_end));
    }
  }

  void scanHtml(const QString &p_slice, int p_base, int p_documentEnd) {
    const auto excludeSlice = [&](int p_start, int p_end) {
      // An unresolved token must not expose markup inside an enclosing font.
      // Suppress conservatively rather than inventing source coordinates.
      exclude(p_base < 0 ? 0 : p_base + p_start, p_base < 0 ? p_documentEnd : p_base + p_end);
    };
    int i = 0;
    while (i < p_slice.size()) {
      if (!m_rawText.m_element.isEmpty()) {
        const int end = htmltag::findRawTextClose(p_slice, i, m_rawText.m_element);
        if (end < 0) {
          return;
        }
        exclude(m_rawStart, p_base < 0 ? p_documentEnd : p_base + end);
        m_rawText.m_element.clear();
        i = end;
        continue;
      }

      const int lt = p_slice.indexOf(QLatin1Char('<'), i);
      if (lt < 0) {
        break;
      }

      // Comments, CDATA, processing instructions and declarations are opaque;
      // a font spelling inside any of them is never another tag.
      const auto suffix = QStringView(p_slice).mid(lt);
      int opaqueEnd = -1;
      if (suffix.startsWith(QLatin1String("<!--"))) {
        const int end = p_slice.indexOf(QStringLiteral("-->"), lt + 4);
        opaqueEnd = end < 0 ? p_slice.size() : end + 3;
      } else if (suffix.startsWith(QLatin1String("<![CDATA["))) {
        const int end = p_slice.indexOf(QStringLiteral("]]>"), lt + 9);
        opaqueEnd = end < 0 ? p_slice.size() : end + 3;
      } else if (suffix.startsWith(QLatin1String("<?"))) {
        const int end = p_slice.indexOf(QStringLiteral("?>"), lt + 2);
        opaqueEnd = end < 0 ? p_slice.size() : end + 2;
      } else if (suffix.startsWith(QLatin1String("<!"))) {
        const int end = htmltag::skipTag(p_slice, lt);
        opaqueEnd = end < 0 ? p_slice.size() : end;
      }
      if (opaqueEnd >= 0) {
        excludeSlice(lt, opaqueEnd);
        i = opaqueEnd;
        continue;
      }

      int nameStart = lt + 1;
      const bool closing = nameStart < p_slice.size() && p_slice.at(nameStart) == QLatin1Char('/');
      if (closing) {
        ++nameStart;
      }
      if (nameStart >= p_slice.size() || !htmltag::isNameStart(p_slice.at(nameStart))) {
        i = lt + 1;
        continue;
      }
      int nameEnd = nameStart;
      while (nameEnd < p_slice.size() && htmltag::isNameChar(p_slice.at(nameEnd))) {
        ++nameEnd;
      }
      const QString name = p_slice.mid(nameStart, nameEnd - nameStart).toLower();
      const int tagEnd = htmltag::skipTag(p_slice, lt);
      excludeSlice(lt, tagEnd < 0 ? p_slice.size() : tagEnd);

      if (!closing && htmltag::isRawTextElement(name)) {
        m_rawText.m_element = name;
        m_rawStart = p_base < 0 ? 0 : p_base + lt;
      } else if (tagEnd >= 0 && name == QStringLiteral("font")) {
        int limit = p_slice.indexOf(QLatin1Char('\n'), nameEnd);
        if (limit < 0) {
          limit = p_slice.size();
        }
        QVector<HtmlAttr> attrs;
        int parsedEnd = -1;
        if (htmltag::parseAttrs(p_slice, nameEnd, limit, p_base, attrs, parsedEnd)) {
          if (closing) {
            // Closing tags may contain whitespace, but not attributes or '/'.
            int end = nameEnd;
            while (end < limit && p_slice.at(end).isSpace()) {
              ++end;
            }
            if (end < limit && p_slice.at(end) == QLatin1Char('>') && !m_open.isEmpty()) {
              const FontColorSpan opened = m_open.takeLast();
              const int closeStart = p_base < 0 ? -1 : p_base + lt;
              if (opened.start >= 0 && opened.start < closeStart && opened.foreground.isValid()) {
                m_matched.append({opened.start, closeStart, opened.foreground});
              }
            }
          } else if (p_slice.at(parsedEnd - 2) != QLatin1Char('/')) {
            const HtmlAttr *color = htmltag::findAttr(attrs, "color");
            // parseAttrs has already decoded HTML entities; QColor accepts
            // named and hex colors, without introducing a second CSS parser.
            const QColor foreground = color ? QColor(color->m_value.trimmed()) : QColor();
            m_open.append({p_base < 0 ? -1 : p_base + parsedEnd, -1, foreground});
          }
        }
      }
      if (tagEnd < 0) {
        return;
      }
      i = tagEnd;
    }
  }

  void appendHighlights(ASTWalkResult &p_result, const LineOffsetTable &p_offsets, int p_startBlock,
                        int p_documentEnd) {
    if (m_matched.isEmpty()) {
      return;
    }
    if (!m_rawText.m_element.isEmpty()) {
      exclude(m_rawStart, p_documentEnd);
    }
    std::sort(m_matched.begin(), m_matched.end(),
              [](const FontColorSpan &p_a, const FontColorSpan &p_b) {
                return p_a.start < p_b.start || (p_a.start == p_b.start && p_a.end > p_b.end);
              });
    std::sort(m_excluded.begin(), m_excluded.end());

    int excludedIndex = 0;
    const auto appendVisible = [&](int p_start, int p_end, const QColor &p_foreground) {
      while (p_start < p_end) {
        if (excludedIndex == m_excluded.size() ||
            m_excluded.at(excludedIndex).m_startPos >= p_end) {
          appendSpan(p_result, p_offsets, p_startBlock, p_start, p_end, p_foreground);
          break;
        }
        const auto &excluded = m_excluded.at(excludedIndex);
        if (excluded.m_endPos <= p_start) {
          ++excludedIndex;
          continue;
        }
        if (excluded.m_startPos > p_start) {
          appendSpan(p_result, p_offsets, p_startBlock, p_start, excluded.m_startPos, p_foreground);
        }
        p_start = qMax(p_start, excluded.m_endPos);
        if (excluded.m_endPos <= p_end) {
          ++excludedIndex;
        }
      }
    };

    // Resolve nesting before cutting out excluded spans to emit source-ordered,
    // disjoint foreground overlays separately from ordinary highlights.
    // Invalid colors have no matched span and inherit naturally.
    QVector<int> active;
    int pos = m_matched.first().start;
    for (int idx = 0; idx < m_matched.size(); ++idx) {
      const auto &span = m_matched.at(idx);
      while (!active.isEmpty() && m_matched.at(active.last()).end <= span.start) {
        const auto &finished = m_matched.at(active.takeLast());
        appendVisible(pos, finished.end, finished.foreground);
        pos = finished.end;
      }
      if (!active.isEmpty()) {
        appendVisible(pos, span.start, m_matched.at(active.last()).foreground);
      }
      pos = span.start;
      active.append(idx);
    }
    while (!active.isEmpty()) {
      const auto &finished = m_matched.at(active.takeLast());
      appendVisible(pos, finished.end, finished.foreground);
      pos = finished.end;
    }
  }

private:
  static void appendSpan(ASTWalkResult &p_result, const LineOffsetTable &p_offsets,
                         int p_startBlock, int p_start, int p_end, const QColor &p_foreground) {
    HLUnitStyle unit;
    unit.format.setForeground(p_foreground);
    for (int line = lineIndexOfDocPos(p_offsets, p_start);
         line >= 0 && line < p_offsets.lineCount(); ++line) {
      const int lineStart = p_offsets.lineStartQCharOffset(line);
      if (lineStart >= p_end) {
        break;
      }
      const int block = p_startBlock + line;
      if (block < 0 || block >= p_result.blocksHighlights.size()) {
        continue;
      }
      const int start = qMax(p_start, lineStart);
      const int end = qMin(p_end, p_offsets.lineEndQCharOffset(line));
      if (start < end) {
        unit.start = start - lineStart;
        unit.length = end - start;
        p_result.blockOverlays[block].append(unit);
      }
    }
  }

  QVector<FontColorSpan> m_open;
  QVector<FontColorSpan> m_matched;
  QVector<ElementRegion> m_excluded;
  RawTextState m_rawText;
  int m_rawStart = 0;
};

} // namespace

// The single per-node entry point for HTML scanning.
//
// @p_rawText is per-WALK state, not per-node: cmark emits `<script>`, its
// contents and `</script>` as separate HTML nodes, so the "inside a raw-text
// element" fact has to be threaded across them. It is advanced for EVERY HTML
// node -- including one whose span cannot be resolved, whose results are
// scanned purely for that side effect and then discarded. Skipping the advance
// would let an unresolvable `<script>` unmask an `<img>` spelled inside it.
//
// Two scanners now share that one state, so they cannot simply be called in
// sequence: the second would start from the state left AFTER the whole slice.
// Each runs on its own COPY of the incoming state, and the two outgoing states
// must agree -- which they do because both scanners' top-level lexers are
// identical, down to the table scanner advancing one tag at a time rather than
// jumping over a table it captured. The assert is the guard on that invariant.
static void extractHtmlNode(cmark_node *p_node, const QString &p_text, const QByteArray &p_utf8Text,
                            const LineOffsetTable &p_offsets, ASTWalkResult &p_result, int p_offset,
                            int p_startBlock, RawTextState &p_rawText,
                            FontColorCollector &p_fontColors, bool p_fast) {
  const bool isBlock = cmark_node_get_type(p_node) == CMARK_NODE_HTML_BLOCK;

  int regionStart = -1;
  int regionEnd = -1;
  QString slice;
  int sliceStart = 0;
  bool resolved = resolveHtmlNodeSpan(p_text, p_node, p_offsets, regionStart, regionEnd);
  if (resolved) {
    slice = p_text.mid(regionStart, regionEnd - regionStart);
    sliceStart = regionStart;
  } else {
    // Unplaceable: scan the literal for the raw-text side effect only, and
    // publish nothing.
    const char *literal = cmark_node_get_literal(p_node);
    if (!literal) {
      return;
    }
    slice = QString::fromUtf8(literal);
    sliceStart = 0;
  }

  p_fontColors.scanHtml(slice, resolved ? sliceStart : -1, p_text.size());
  if (p_fast) {
    return;
  }

  const RawTextState incoming = p_rawText;
  RawTextState imageState = incoming;
  RawTextState tableState = incoming;

  ASTWalkResult discarded;
  extractHtmlImages(slice, sliceStart, p_text, p_utf8Text, p_offsets,
                    resolved ? p_result : discarded, p_offset, imageState);
  extractHtmlTables(slice, sliceStart, p_text, p_offsets, resolved ? p_result : discarded, p_offset,
                    p_startBlock, resolved && isBlock && isDocumentChild(p_node), regionStart,
                    regionEnd, tableState);

  Q_ASSERT(imageState.m_element == tableState.m_element);
  p_rawText = imageState;
}

// Capture the typed data of one non-table element.
static void extractTypedElement(cmark_node *p_node, cmark_node_type p_type, int p_style,
                                const QByteArray &p_utf8Text, const LineOffsetTable &p_offsets,
                                ASTWalkResult &p_result, int p_startLine, int p_endLine,
                                int p_docStart, int p_docEnd, int p_absStart, int p_absEnd) {
  switch (p_type) {
  case CMARK_NODE_IMAGE: {
    ImageElement image;
    image.m_startPos = p_absStart;
    image.m_endPos = p_absEnd;
    const char *url = cmark_node_get_url(p_node);
    image.m_destination = url ? QString::fromUtf8(url) : QString();
    const char *title = cmark_node_get_title(p_node);
    image.m_title = title ? QString::fromUtf8(title) : QString();
    image.m_width = cmark_node_get_image_width(p_node);
    image.m_height = cmark_node_get_image_height(p_node);
    image.m_alternateText = collectLiteralText(p_node);
    image.m_standalone =
        isStandaloneSpan(p_utf8Text, p_offsets, p_startLine, p_endLine, p_docStart, p_docEnd);
    p_result.imageElements.append(image);
    break;
  }

  case CMARK_NODE_CODE_BLOCK: {
    if (p_style != STYLE_FENCEDCODEBLOCK) {
      break;
    }
    CodeElement code;
    code.m_startPos = p_absStart;
    code.m_endPos = p_absEnd;
    const char *info = cmark_node_get_fence_info(p_node);
    code.m_language = info ? QString::fromUtf8(info) : QString();
    const char *literal = cmark_node_get_literal(p_node);
    code.m_code = literal ? QString::fromUtf8(literal) : QString();
    p_result.codeElements.append(code);
    break;
  }

  case CMARK_NODE_FORMULA_BLOCK:
  case CMARK_NODE_FORMULA_INLINE: {
    MathElement math;
    math.m_startPos = p_absStart;
    math.m_endPos = p_absEnd;
    const char *literal = cmark_node_get_literal(p_node);
    math.m_expression = literal ? QString::fromUtf8(literal) : QString();
    math.m_display = cmark_node_get_formula_display(p_node);
    math.m_block = (p_type == CMARK_NODE_FORMULA_BLOCK);
    p_result.mathElements.append(math);
    break;
  }

  case CMARK_NODE_HEADING: {
    // Reached exactly once per heading: headings are non-leaf, so the walk
    // loop's ENTER guard lets only the enter event through.
    extractHeading(p_node, p_result, p_absStart, p_absEnd);
    break;
  }

  default:
    break;
  }
}

ASTWalkResult walkAndConvert(const QByteArray &p_utf8Text, int p_numBlocks, int p_offset,
                             int p_startBlock, bool p_fast, bool p_collectLists) {
  const bool collectLists = p_collectLists && !p_fast;
  ASTWalkResult result;
  result.blocksHighlights.resize(p_numBlocks);

  if (p_utf8Text.isEmpty()) {
    result.listStructure.m_valid = collectLists;
    return result;
  }

  LineOffsetTable offsets(p_utf8Text);
  ReferenceConcealContext referenceContext{p_utf8Text, offsets, result.concealRanges, p_offset};
  cmark_parser *parser = cmark_parser_new(CMARK_OPT_DEFAULT);
  if (!parser) {
    return result;
  }
  if (!p_fast) {
    cmark_parser_set_reference_destination_callback(parser, collectReferenceDestination,
                                                    &referenceContext);
  }
  cmark_parser_feed(parser, p_utf8Text.constData(), p_utf8Text.size());
  cmark_node *doc = cmark_parser_finish(parser);
  cmark_parser_free(parser);
  if (!doc) {
    result.concealRanges.clear();
    return result;
  }

  ListMarkerReader listMarkers(p_utf8Text, offsets);
  ListCollector lists(result.listStructure, p_utf8Text, offsets, listMarkers, p_offset,
                      p_startBlock);

  // Decode only when an HTML node needs verified QChar source positions.
  // Ordinary Markdown, including code that merely spells HTML, needs no copy.
  QString text;
  FontColorCollector fontColors;

  // Per-WALK raw-text context; see extractHtmlImages().
  RawTextState rawText;

  cmark_iter *iter = cmark_iter_new(doc);
  if (!iter) {
    cmark_node_free(doc);
    result.concealRanges.clear();
    return result;
  }
  cmark_event_type ev;

  while ((ev = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
    cmark_node *node = cmark_iter_get_node(iter);
    cmark_node_type type = cmark_node_get_type(node);

    if (collectLists && ev == CMARK_EVENT_ENTER) {
      lists.enter(node);
    }

    if (type == CMARK_NODE_DOCUMENT) {
      continue;
    }

    if (type == CMARK_NODE_SOFTBREAK) {
      continue;
    }

    if (type == CMARK_NODE_LIST) {
      continue;
    }
    if (type == CMARK_NODE_ITEM && ev == CMARK_EVENT_ENTER) {
      handleListDirect(node, listMarkers, offsets, result, p_startBlock, p_numBlocks);
      continue;
    }

    bool isLeaf = cmark_node_is_leaf(node);
    if (!isLeaf && ev != CMARK_EVENT_ENTER) {
      continue;
    }

    // Runs BEFORE the span/style guards below: the raw-text state must advance
    // for every HTML node, including one this walk cannot place.
    if (type == CMARK_NODE_HTML_INLINE || type == CMARK_NODE_HTML_BLOCK) {
      if (text.isNull()) {
        text = QString::fromUtf8(p_utf8Text);
      }
      extractHtmlNode(node, text, p_utf8Text, offsets, result, p_offset, p_startBlock, rawText,
                      fontColors, p_fast);
    }

    if (!p_fast && rawText.m_element.isEmpty() &&
        (type == CMARK_NODE_LINK || type == CMARK_NODE_IMAGE)) {
      extractInlineConcealRange(node, p_utf8Text, offsets, result.concealRanges,
                                type == CMARK_NODE_IMAGE ? MarkdownConcealElement::ImageUrl
                                                         : MarkdownConcealElement::LinkUrl,
                                p_offset);
    }

    int style = mapCmarkNodeToStyle(type, node);
    if (style < 0) {
      continue;
    }

    int docStart = 0;
    int docEnd = 0;

    // Line numbers are still needed below for folding regions and for the
    // typed-element extractors, which work line-wise rather than by offset.
    int sl = cmark_node_get_start_line(node);
    int el = cmark_node_get_end_line(node);

    if (type == CMARK_NODE_HTML_BLOCK) {
      // An HTML block's reported END is wrong: cmark gives the last line it
      // CONSUMED BEFORE the end condition matched, so `<pre>…</pre>` reports
      // the line before `</pre>` and the raw span styles every line of the
      // block except its closing tag. resolveHtmlNodeSpan() is the one
      // correction, already used by extractHtmlNode() and by the snapshot API;
      // the highlight path takes its END, or the font and the element
      // extraction disagree about where the block stops.
      //
      // Only the end. The resolver deliberately ignores columns and slices
      // whole lines, which is right for a scanner reading the source back but
      // wrong for a highlight: the first line's container prefix (`> `, `- `)
      // belongs to the quote or the list item, not to the HTML, and painting it
      // in the monospace face would step the marker sideways. cmark's reported
      // START column is prefix-relative and correct, so it is kept.
      int resolvedStart = 0;
      int resolvedEnd = 0;
      const bool resolved = resolveHtmlNodeSpan(text, node, offsets, resolvedStart, resolvedEnd);
      if (!cmarkNodeSpan(node, offsets, docStart, docEnd)) {
        if (!resolved) {
          continue;
        }
        docStart = resolvedStart;
        docEnd = resolvedEnd;
      } else if (resolved) {
        docEnd = qMax(docEnd, resolvedEnd);
      }

      const int firstLine = lineIndexOfDocPos(offsets, docStart);
      const int lastLine = lineIndexOfDocPos(offsets, docEnd > docStart ? docEnd - 1 : docStart);
      if (firstLine < 0 || lastLine < firstLine) {
        continue;
      }
      sl = firstLine + 1;
      el = lastLine + 1;
    } else if (!cmarkNodeSpan(node, offsets, docStart, docEnd)) {
      continue;
    }

    // cmark reports inline formula contents without the $ or $$ delimiters.
    if (type == CMARK_NODE_FORMULA_INLINE) {
      const int delimiterWidth = cmark_node_get_formula_display(node) ? 2 : 1;
      docStart -= delimiterWidth;
      docEnd += delimiterWidth;
    }

#ifdef VTE_DEBUG_HIGHLIGHT
    qDebug() << "WALKER inline: type=" << cmark_node_get_type_string(node) << "sl=" << sl
             << "el=" << el << "docStart=" << docStart << "docEnd=" << docEnd
             << "startBlock=" << p_startBlock;
#endif

    if (type == CMARK_NODE_CODE || type == CMARK_NODE_CODE_BLOCK ||
        type == CMARK_NODE_FORMULA_INLINE || type == CMARK_NODE_FORMULA_BLOCK) {
      fontColors.exclude(docStart, docEnd);
    }

    // Add per-block HLUnits.
    addHLUnit(result, offsets, docStart, docEnd, style, p_startBlock, p_numBlocks);

    // Collect regions (only when not fast-parsing).
    if (!p_fast) {
      int absStart = p_offset + docStart;
      int absEnd = p_offset + docEnd;
      addRegion(result, style, absStart, absEnd);

      if (type == CMARK_NODE_TABLE) {
        extractTable(node, offsets, p_utf8Text, result, p_offset, p_startBlock);
      } else {
        extractTypedElement(node, type, style, p_utf8Text, offsets, result, sl, el, docStart,
                            docEnd, absStart, absEnd);
      }

      int startBlock = p_startBlock + (sl - 1);
      int endBlock = p_startBlock + (el - 1);
      addFoldingRegion(result, style, startBlock, endBlock);
    }
  }

  cmark_iter_free(iter);
  if (collectLists) {
    lists.finish();
  }

  fontColors.appendHighlights(result, offsets, p_startBlock, text.size());

  // Sort ordinary HLUnits; foreground overlays are already disjoint and source-ordered.
  for (auto &blockUnits : result.blocksHighlights) {
    if (blockUnits.size() > 1) {
      std::sort(blockUnits.begin(), blockUnits.end(), HLUnitLess());
    }
  }

  // Sort region vectors that need sorting.
  if (!p_fast) {
    std::sort(result.concealRanges.begin(), result.concealRanges.end(),
              [](const ConcealRange &a, const ConcealRange &b) {
                if (a.m_startPos != b.m_startPos) {
                  return a.m_startPos < b.m_startPos;
                }
                if (a.m_endPos != b.m_endPos) {
                  return a.m_endPos < b.m_endPos;
                }
                return static_cast<int>(a.m_element) < static_cast<int>(b.m_element);
              });
    result.concealRanges.erase(std::unique(result.concealRanges.begin(), result.concealRanges.end(),
                                           [](const ConcealRange &a, const ConcealRange &b) {
                                             return a.m_startPos == b.m_startPos &&
                                                    a.m_endPos == b.m_endPos &&
                                                    a.m_element == b.m_element;
                                           }),
                               result.concealRanges.end());
    std::sort(result.headerRegions.begin(), result.headerRegions.end());
    std::sort(result.displayFormulaRegions.begin(), result.displayFormulaRegions.end());
    std::sort(result.tableRegions.begin(), result.tableRegions.end());
    std::sort(result.tableHeaderRegions.begin(), result.tableHeaderRegions.end());
    std::sort(result.foldingRegions.begin(), result.foldingRegions.end(),
              [](const FoldingRegion &a, const FoldingRegion &b) {
                return a.m_startBlock < b.m_startBlock;
              });

    auto byStart = [](const TypedPreviewElement &a, const TypedPreviewElement &b) {
      return a.m_startPos < b.m_startPos;
    };
    std::sort(result.imageElements.begin(), result.imageElements.end(), byStart);
    std::sort(result.codeElements.begin(), result.codeElements.end(), byStart);
    std::sort(result.mathElements.begin(), result.mathElements.end(), byStart);
    std::sort(result.tableElements.begin(), result.tableElements.end(), byStart);
    std::sort(
        result.headingElements.begin(), result.headingElements.end(),
        [](const HeadingInfo &a, const HeadingInfo &b) { return a.m_startPos < b.m_startPos; });
  }

  cmark_node_free(doc);
  return result;
}

QVector<ImageLinkInfo> buildImageLinks(const QVector<ImageElement> &p_elements) {
  QVector<ImageLinkInfo> links;
  links.reserve(p_elements.size());
  for (const auto &element : p_elements) {
    ImageLinkInfo info(ElementRegion(element.m_startPos, element.m_endPos), element.m_destination,
                       element.m_width, element.m_height);
    info.m_syntax = element.m_syntax;
    info.m_alt = element.m_alternateText;
    info.m_title = element.m_title;
    links.append(info);
  }
  return links;
}

ASTWalkResult parseInlineSnippet(const QString &p_snippet) {
  if (p_snippet.isEmpty()) {
    return ASTWalkResult();
  }
  ++s_inlineSnippetParses;
  return walkAndConvert(p_snippet.toUtf8(), 1, 0, 0, false);
}

quint64 inlineSnippetParseCount() { return s_inlineSnippetParses; }

void resetInlineSnippetParseCount() { s_inlineSnippetParses = 0; }

} // namespace md
} // namespace vte
