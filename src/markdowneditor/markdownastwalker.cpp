#include "markdownastwalker.h"
#include "cmarkadapter.h"
#include "markdownsyntaxstyles.h"

#include <algorithm>

#include <vtextedit/htmlimgscanner.h>
#include <vtextedit/htmltablescanner.h>

#ifdef VTE_DEBUG_HIGHLIGHT
#include <QDebug>
#endif

#include <cmark.h>
#include <node.h>

namespace vte {
namespace md {

static int numberWidth(int p_num) {
  if (p_num <= 0)
    return 1;
  int w = 0;
  int n = p_num;
  while (n > 0) {
    n /= 10;
    ++w;
  }
  return w;
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
        // Skip leading indentation for styles whose monospace font should not bleed into list-item
        // indentation whitespace.
        int ls = (p_style == STYLE_TABLE || p_style == STYLE_TABLEHEADER ||
                  p_style == STYLE_DISPLAYFORMULA)
                     ? p_offsets.lineLeadingSpaces(lineIdx)
                     : 0;
        unit.start = ls;
        unit.length = p_docEnd - lineStartQChar - ls;
      } else {
        // Skip leading indentation for styles whose monospace font should not bleed into list-item
        // indentation whitespace.
        int ls = (p_style == STYLE_TABLE || p_style == STYLE_TABLEHEADER ||
                  p_style == STYLE_DISPLAYFORMULA)
                     ? p_offsets.lineLeadingSpaces(lineIdx)
                     : 0;
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

static void handleListDirect(cmark_node *p_listNode, const LineOffsetTable &p_offsets,
                             ASTWalkResult &p_result, int p_startBlock, int p_numBlocks) {
  cmark_list_type listType = cmark_node_get_list_type(p_listNode);
  int startNum = cmark_node_get_list_start(p_listNode);

  int itemIdx = 0;
  for (cmark_node *item = cmark_node_first_child(p_listNode); item != nullptr;
       item = cmark_node_next(item)) {
    if (cmark_node_get_type(item) != CMARK_NODE_ITEM) {
      continue;
    }

    int sl = cmark_node_get_start_line(item);
    int sc = cmark_node_get_start_column(item);
    int docPos = p_offsets.toDocPosition(sl, sc);

    int lineIdx = sl - 1;
    int blockNum = p_startBlock + lineIdx;
    if (blockNum < 0 || blockNum >= p_numBlocks) {
      ++itemIdx;
      continue;
    }

    int lineStartQChar = p_offsets.lineStartQCharOffset(lineIdx);
    int style;
    int span;
    if (listType == CMARK_BULLET_LIST) {
      style = STYLE_LIST_BULLET;
      span = 1;
    } else {
      style = STYLE_LIST_ENUMERATOR;
      int num = startNum + itemIdx;
      span = numberWidth(num) + 1;
    }

    HLUnit unit;
    unit.start = docPos - lineStartQChar;
    unit.length = span;
    unit.styleIndex = style;
    p_result.blocksHighlights[blockNum].append(unit);
    ++itemIdx;
  }
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
static bool splitTableRow(const QString &p_line, QString &p_prefix, QVector<QString> &p_cells,
                          QVector<int> *p_cellOffsets = nullptr) {
  const int firstPipe = p_line.indexOf(QLatin1Char('|'));
  if (firstPipe < 0) {
    return false;
  }

  p_prefix = p_line.left(firstPipe);

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

// Capture every HTML `<img …>` of one already-resolved slice as an
// ImageElement, so the live editor previews and menus treat it exactly like a
// Markdown image link.
static void extractHtmlImages(const QString &p_slice, int p_sliceStart, const QString &p_text,
                              const QByteArray &p_utf8Text, const LineOffsetTable &p_offsets,
                              ASTWalkResult &p_result, int p_offset, RawTextState &p_rawText) {
  Q_UNUSED(p_text);
  const auto tags = scanHtmlImgTags(p_slice, p_sliceStart, &p_rawText);
  for (const auto &tag : tags) {
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

    // Per-cell highlighting parses each payload as its own cmark document, so
    // it is bounded by the very limit that decides whether an interactive sheet
    // is possible at all (TablePreviewWidget::c_maxCells, which
    // sliceTableCellHighlights() mirrors for the same reason). A table past it
    // never gets a widget, so the runs would be paid for on every parse and
    // then thrown away.
    const int c_maxPreviewCells = 300;
    const bool highlightCells =
        table.m_markdownBacked &&
        static_cast<qint64>(html.m_rowCount) * static_cast<qint64>(html.m_columnCount) <=
            static_cast<qint64>(c_maxPreviewCells);

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
        // Markdown-only: there is no one-source-line-per-row correspondence to
        // slice highlights out of, so no cell offset is meaningful here.
        row.m_cellOffsets.append(-1);
        row.m_cellHighlights.append(highlightCells ? highlightInlineSnippet(text)
                                                   : QVector<HLUnit>());
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
                            int p_startBlock, RawTextState &p_rawText) {
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
    math.m_display = (p_type == CMARK_NODE_FORMULA_BLOCK);
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

// Slice the per-block highlight units of a table's source lines into per-cell,
// cell-local units. Must run after blocksHighlights has been sorted, so the
// relative order the merge algorithm depends on is already final.
static void sliceTableCellHighlights(ASTWalkResult &p_result) {
  // Mirrors TablePreviewWidget::c_maxCells: bigger tables never get a widget,
  // so there is no point in paying for the slicing.
  const int c_maxPreviewCells = 300;

  for (auto &table : p_result.tableElements) {
    // An HTML table has no one-source-line-per-row correspondence: the loop
    // below indexes m_startBlock + r and assumes one source block per row,
    // which no HTML table satisfies. Its cells' runs come from
    // highlightInlineSnippet() at extraction time instead.
    if (table.m_syntax != TableElement::Syntax::Markdown || table.m_startBlock < 0) {
      continue;
    }

    int cellCount = 0;
    for (const auto &row : table.m_rows) {
      cellCount += row.m_cells.size();
    }
    if (cellCount > c_maxPreviewCells) {
      continue;
    }

    for (int r = 0; r < table.m_rows.size(); ++r) {
      auto &row = table.m_rows[r];
      const int blockNum = table.m_startBlock + r;
      if (blockNum < 0 || blockNum >= p_result.blocksHighlights.size()) {
        continue;
      }

      const auto &units = p_result.blocksHighlights.at(blockNum);
      // Kept parallel to m_cells even when there is nothing to slice.
      row.m_cellHighlights.resize(row.m_cells.size());
      if (units.isEmpty()) {
        continue;
      }

      for (int c = 0; c < row.m_cells.size(); ++c) {
        const int off = row.m_cellOffsets.value(c, -1);
        const int len = row.m_cells.at(c).size();
        if (off < 0 || len <= 0) {
          continue;
        }

        const long long cellStart = off;
        const long long cellEnd = static_cast<long long>(off) + len;
        auto &cellUnits = row.m_cellHighlights[c];
        for (const auto &unit : units) {
          const int styleIdx = static_cast<int>(unit.styleIndex);
          if (styleIdx == STYLE_TABLE || styleIdx == STYLE_TABLEHEADER ||
              styleIdx == STYLE_BLOCKQUOTE) {
            continue;
          }

          const long long s = qMax<long long>(static_cast<long long>(unit.start), cellStart);
          const long long e = qMin<long long>(
              static_cast<long long>(unit.start) + static_cast<long long>(unit.length), cellEnd);
          if (e <= s) {
            continue;
          }

          HLUnit sliced;
          sliced.start = static_cast<unsigned long>(s - cellStart);
          sliced.length = static_cast<unsigned long>(e - s);
          sliced.styleIndex = unit.styleIndex;
          cellUnits.append(sliced);
        }
      }
    }
  }
}

ASTWalkResult walkAndConvert(const QByteArray &p_utf8Text, int p_numBlocks, int p_offset,
                             int p_startBlock, bool p_fast) {
  ASTWalkResult result;
  result.blocksHighlights.resize(p_numBlocks);

  if (p_utf8Text.isEmpty()) {
    return result;
  }

  cmark_node *doc =
      cmark_parse_document(p_utf8Text.constData(), p_utf8Text.size(), CMARK_OPT_DEFAULT);
  if (!doc) {
    return result;
  }

  LineOffsetTable offsets(p_utf8Text);

  // Decoded once: the HTML span resolver and the `<img>` scanner both work on
  // QChar offsets, which is also what every downstream region uses.
  const QString text = p_fast ? QString() : QString::fromUtf8(p_utf8Text);

  // Per-WALK raw-text context; see extractHtmlImages().
  RawTextState rawText;

  cmark_iter *iter = cmark_iter_new(doc);
  cmark_event_type ev;

  while ((ev = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
    cmark_node *node = cmark_iter_get_node(iter);
    cmark_node_type type = cmark_node_get_type(node);

    if (type == CMARK_NODE_DOCUMENT) {
      continue;
    }

    if (type == CMARK_NODE_SOFTBREAK) {
      continue;
    }

    if (type == CMARK_NODE_LIST && ev == CMARK_EVENT_ENTER) {
      handleListDirect(node, offsets, result, p_startBlock, p_numBlocks);
      continue;
    }

    bool isLeaf = cmark_node_is_leaf(node);
    if (!isLeaf && ev != CMARK_EVENT_ENTER) {
      continue;
    }

    // Runs BEFORE the span/style guards below: the raw-text state must advance
    // for every HTML node, including one this walk cannot place.
    if (!p_fast && (type == CMARK_NODE_HTML_INLINE || type == CMARK_NODE_HTML_BLOCK)) {
      extractHtmlNode(node, text, p_utf8Text, offsets, result, p_offset, p_startBlock, rawText);
    }

    int style = mapCmarkNodeToStyle(type, node);
    if (style < 0) {
      continue;
    }

    int docStart = 0;
    int docEnd = 0;
    if (!cmarkNodeSpan(node, offsets, docStart, docEnd)) {
      continue;
    }

    // Line numbers are still needed below for folding regions and for the
    // typed-element extractors, which work line-wise rather than by offset.
    int sl = cmark_node_get_start_line(node);
    int el = cmark_node_get_end_line(node);

    // The formula delimiters are not part of cmark's reported span; widening by
    // one on each side is node-type policy, so it stays at the call site.
    if (type == CMARK_NODE_FORMULA_INLINE) {
      docStart -= 1;
      docEnd += 1;
    }

#ifdef VTE_DEBUG_HIGHLIGHT
    qDebug() << "WALKER inline: type=" << cmark_node_get_type_string(node) << "sl=" << sl
             << "el=" << el << "docStart=" << docStart << "docEnd=" << docEnd
             << "startBlock=" << p_startBlock;
#endif

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

  // Sort each block's HLUnits.
  for (auto &blockUnits : result.blocksHighlights) {
    if (blockUnits.size() > 1) {
      std::sort(blockUnits.begin(), blockUnits.end(), HLUnitLess());
    }
  }

  // Sort region vectors that need sorting.
  if (!p_fast) {
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

    // Runs after the blocksHighlights sort so the sliced units keep the final
    // relative order the format-merge algorithm depends on.
    sliceTableCellHighlights(result);
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

QVector<HLUnit> highlightInlineSnippet(const QString &p_snippet) {
  if (p_snippet.isEmpty()) {
    return QVector<HLUnit>();
  }
  // One block, no regions, no typed elements: the fast path is exactly what
  // per-cell highlighting needs, and it also keeps this off the recursive
  // element-extraction path entirely.
  return walkAndConvert(p_snippet.toUtf8(), 1, 0, 0, true).blocksHighlights.value(0);
}

} // namespace md
} // namespace vte
