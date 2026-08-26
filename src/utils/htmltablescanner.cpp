#include <vtextedit/htmltablescanner.h>

#include "htmltagparse.h"

#include <QHash>
#include <QSet>

using namespace vte;
using namespace vte::htmltag;

const char *const vte::c_vteMarkdownPayloadPrefix = "<!--vte-md:";

namespace {

const QPoint c_noOrigin(-1, -1);

// Hard ceilings on what a table may declare, applied BEFORE any arithmetic on
// them. Two reasons, and the first is the important one:
//
// - A `colspan="2147483647"` is a valid positive int. Validating it as
//   `col + span > limit` would OVERFLOW, pass, and then drive a loop over
//   billions of slots -- a hang and an unbounded allocation from note source
//   alone. Every bound below is therefore checked by SUBTRACTION against a
//   ceiling that is itself small.
// - A table this large could never receive an interactive sheet anyway:
//   TablePreviewDocument refuses one past c_maxCells (300).
const int c_maxSpan = 4096;
const int c_maxRows = 4096;
const int c_maxColumns = 4096;
const int c_maxSlots = 1 << 20;

// The void elements, which are closed by their own opening tag. Anything else
// inside a cell must be closed explicitly, per the canonical subset.
bool isVoidElement(const QString &p_lowerName) {
  static const QSet<QString> s_void{
      QStringLiteral("area"),  QStringLiteral("base"),  QStringLiteral("br"),
      QStringLiteral("col"),   QStringLiteral("embed"), QStringLiteral("hr"),
      QStringLiteral("img"),   QStringLiteral("input"), QStringLiteral("link"),
      QStringLiteral("meta"),  QStringLiteral("param"), QStringLiteral("source"),
      QStringLiteral("track"), QStringLiteral("wbr")};
  return s_void.contains(p_lowerName);
}

// The tag at @p_idx, which must point at '<'. Returns false when this is not a
// tag opener at all (a bare '<' in prose).
bool tagAt(const QString &p_text, int p_idx, QString &p_name, bool &p_closing, int &p_nameEnd) {
  if (p_idx >= p_text.size() || p_text.at(p_idx) != QLatin1Char('<')) {
    return false;
  }
  int ns = p_idx + 1;
  p_closing = false;
  if (ns < p_text.size() && p_text.at(ns) == QLatin1Char('/')) {
    p_closing = true;
    ++ns;
  }
  if (ns >= p_text.size() || !isNameStart(p_text.at(ns))) {
    return false;
  }
  int ne = ns;
  while (ne < p_text.size() && isNameChar(p_text.at(ne))) {
    ++ne;
  }
  p_name = p_text.mid(ns, ne - ns).toLower();
  p_nameEnd = ne;
  return true;
}

// Index just past the '>' of a closing tag whose name ends at @p_nameEnd, or -1
// when anything but whitespace follows the name -- or when the '>' is not on the
// SAME source line, which D-i forbids for every tag, not just opening ones.
int consumeCloseTag(const QString &p_text, int p_nameEnd) {
  int i = p_nameEnd;
  while (i < p_text.size() && p_text.at(i).isSpace()) {
    if (p_text.at(i) == QLatin1Char('\n')) {
      return -1;
    }
    ++i;
  }
  if (i < p_text.size() && p_text.at(i) == QLatin1Char('>')) {
    return i + 1;
  }
  return -1;
}

// The one-line limit for a tag starting inside the line that contains @p_idx.
int lineLimit(const QString &p_text, int p_idx) {
  const int nl = p_text.indexOf(QLatin1Char('\n'), p_idx);
  return nl < 0 ? p_text.size() : nl;
}

int skipSpaces(const QString &p_text, int p_idx) {
  int i = p_idx;
  while (i < p_text.size() && p_text.at(i).isSpace()) {
    ++i;
  }
  return i;
}

// A `colspan` / `rowspan` value. Returns false when the attribute is present
// but is not a positive integer, or exceeds c_maxSpan: a table we cannot read
// exactly is a table we must not rewrite, and a span large enough to overflow
// the grid arithmetic is refused here so no later check has to survive one.
bool spanAttr(const QVector<HtmlAttr> &p_attrs, const char *p_name, int &p_span) {
  p_span = 1;
  const HtmlAttr *attr = findAttr(p_attrs, p_name);
  if (!attr) {
    return true;
  }
  bool ok = false;
  const int value = attr->m_value.trimmed().toInt(&ok);
  if (!ok || value < 1 || value > c_maxSpan) {
    return false;
  }
  p_span = value;
  return true;
}

bool isKnownAlign(const QString &p_value) {
  return p_value == QStringLiteral("left") || p_value == QStringLiteral("center") ||
         p_value == QStringLiteral("right");
}

// Index just past the `</table>` matching the `<table` at @p_idx, counting
// nesting. Returns p_text.size() when it is never closed, so an unbalanced
// table suppresses every candidate after it rather than letting a nested one
// through -- fail safe, exactly as the `<img>` scanner treats an unclosed
// raw-text element.
int findTableRegionEnd(const QString &p_text, int p_idx) {
  int depth = 0;
  int i = p_idx;
  while (i < p_text.size()) {
    const int lt = p_text.indexOf(QLatin1Char('<'), i);
    if (lt < 0) {
      break;
    }
    if (p_text.mid(lt, 4) == QLatin1String("<!--")) {
      const int end = p_text.indexOf(QStringLiteral("-->"), lt + 4);
      if (end < 0) {
        break;
      }
      i = end + 3;
      continue;
    }

    QString name;
    bool closing = false;
    int nameEnd = 0;
    if (!tagAt(p_text, lt, name, closing, nameEnd)) {
      i = lt + 1;
      continue;
    }

    const int tagEnd = skipTag(p_text, lt);
    if (name == QStringLiteral("table")) {
      if (closing) {
        --depth;
        if (depth <= 0) {
          return tagEnd < 0 ? p_text.size() : tagEnd;
        }
      } else {
        ++depth;
      }
    }
    i = tagEnd < 0 ? lt + 1 : tagEnd;
  }
  return p_text.size();
}

// Parse one cell, starting at its `<td` / `<th` opener. Enforces every D-i
// condition that is local to a cell.
bool parseCell(const QString &p_text, int p_idx, int p_baseOffset, const QString &p_name,
               int p_nameEnd, HtmlTableCell &p_cell, int &p_next) {
  int tagEnd = -1;
  if (!parseAttrs(p_text, p_nameEnd, lineLimit(p_text, p_idx), p_baseOffset, p_cell.m_attrs,
                  tagEnd)) {
    return false;
  }

  p_cell.m_header = (p_name == QStringLiteral("th"));
  p_cell.m_tagStart = p_baseOffset + p_idx;
  p_cell.m_tagEnd = p_baseOffset + tagEnd;

  if (!spanAttr(p_cell.m_attrs, "colspan", p_cell.m_colSpan) ||
      !spanAttr(p_cell.m_attrs, "rowspan", p_cell.m_rowSpan)) {
    return false;
  }

  if (const HtmlAttr *align = findAttr(p_cell.m_attrs, "align")) {
    p_cell.m_align = align->m_value.trimmed().toLower();
    if (!p_cell.m_align.isEmpty() && !isKnownAlign(p_cell.m_align)) {
      return false;
    }
  }

  // Find the matching closing tag. Any `<td>`, `<th>`, `<tr>` or `<table>` met
  // before it means an implicit close or a nested table: both are outside the
  // canonical subset, so the whole table is dropped rather than guessed at.
  //
  // Every OTHER tag is tracked on a stack, because "all tags are explicitly
  // balanced" is a condition of the subset, not a hope. A cell holding
  // `<b>x</td>` would otherwise be accepted and then written back verbatim,
  // and the unclosed `<b>` would swallow the rest of the table when a renderer
  // read it. Raw-text elements are refused outright: their contents are not
  // markup, so a `</td>` spelled inside one is text, and admitting that would
  // make the cell's extent depend on which scanner is asking.
  const int innerStart = tagEnd;
  int innerEnd = -1;
  int closeNameEnd = -1;
  QVector<QString> openTags;
  int j = innerStart;
  while (j < p_text.size()) {
    if (p_text.at(j) != QLatin1Char('<')) {
      ++j;
      continue;
    }
    if (p_text.mid(j, 4) == QLatin1String("<!--")) {
      const int end = p_text.indexOf(QStringLiteral("-->"), j + 4);
      if (end < 0) {
        return false;
      }
      j = end + 3;
      continue;
    }

    QString name;
    bool closing = false;
    int nameEnd = 0;
    if (!tagAt(p_text, j, name, closing, nameEnd)) {
      ++j;
      continue;
    }

    if (name == QStringLiteral("td") || name == QStringLiteral("th") ||
        name == QStringLiteral("tr") || name == QStringLiteral("table")) {
      if (closing && name == p_name && openTags.isEmpty()) {
        innerEnd = j;
        closeNameEnd = nameEnd;
        break;
      }
      return false;
    }

    if (!closing && isRawTextElement(name)) {
      return false;
    }

    const int te = skipTag(p_text, j);
    if (te < 0) {
      return false;
    }

    if (closing) {
      if (openTags.isEmpty() || openTags.last() != name) {
        // A stray or misnested closing tag.
        return false;
      }
      openTags.removeLast();
    } else if (!isVoidElement(name)) {
      // `<br />` and friends close themselves; so does any tag spelled `… />`.
      const bool selfClosing = te >= 2 && p_text.at(te - 2) == QLatin1Char('/');
      if (!selfClosing) {
        openTags.append(name);
      }
    }

    j = te;
  }

  if (innerEnd < 0 || !openTags.isEmpty()) {
    return false;
  }

  const int afterClose = consumeCloseTag(p_text, closeNameEnd);
  if (afterClose < 0) {
    return false;
  }

  p_cell.m_inner = p_text.mid(innerStart, innerEnd - innerStart);
  // D-i: a cell's inner source lies on ONE line. This is what lets both the
  // snapshot and the live path slice raw source without mapping container
  // prefixes, exactly as the single-line rule does for `<img>` (D8).
  if (p_cell.m_inner.contains(QLatin1Char('\n'))) {
    return false;
  }

  p_cell.m_innerStart = p_baseOffset + innerStart;
  p_cell.m_innerEnd = p_baseOffset + innerEnd;

  const QString prefix = QString::fromLatin1(c_vteMarkdownPayloadPrefix);
  if (p_cell.m_inner.startsWith(prefix)) {
    p_cell.m_hasPayload = true;
    const int end = p_cell.m_inner.indexOf(QStringLiteral("-->"), prefix.size());
    if (end < 0) {
      p_cell.m_payloadMalformed = true;
    } else if (!unescapePayload(p_cell.m_inner.mid(prefix.size(), end - prefix.size()),
                                p_cell.m_payload)) {
      p_cell.m_payloadMalformed = true;
    }
  }

  p_next = afterClose;
  return true;
}

// Resolve the logical grid and enforce the tiling, header and alignment
// conditions of D-i. The grid is grown on demand, so a row that OVERRUNS the
// others widens it and every other row is then short -- which the exact-tiling
// check below rejects. No gap, no overlap, no overflow.
bool buildGrid(HtmlTable &p_table) {
  const int rowCount = p_table.m_rows.size();
  if (rowCount <= 0 || rowCount > c_maxRows) {
    return false;
  }

  // A sparse slot map keyed by (row, column). QVector<QPoint> cannot be used as
  // the occupancy grid because its value-initialized element, QPoint(0, 0), is
  // a VALID origin and so is indistinguishable from "free".
  //
  // Every bound below is checked by SUBTRACTION against a small ceiling, never
  // by adding a span to an index: `col + colSpan > limit` overflows for a
  // hostile `colspan`, passes, and then drives a loop over billions of slots.
  QHash<qint64, QPoint> slotMap;
  auto key = [](int r, int c) { return static_cast<qint64>(r) * c_maxColumns + c; };

  int columnCount = 0;
  for (int r = 0; r < rowCount; ++r) {
    int col = 0;
    for (auto &cell : p_table.m_rows[r].m_cells) {
      while (col < c_maxColumns && slotMap.contains(key(r, col))) {
        ++col;
      }
      if (col >= c_maxColumns || cell.m_rowSpan > rowCount - r ||
          cell.m_colSpan > c_maxColumns - col ||
          slotMap.size() > c_maxSlots - cell.m_rowSpan * cell.m_colSpan) {
        return false;
      }
      for (int dr = 0; dr < cell.m_rowSpan; ++dr) {
        for (int dc = 0; dc < cell.m_colSpan; ++dc) {
          const qint64 k = key(r + dr, col + dc);
          if (slotMap.contains(k)) {
            return false;
          }
          slotMap.insert(k, QPoint(col, r));
        }
      }
      cell.m_origin = QPoint(col, r);
      columnCount = qMax(columnCount, col + cell.m_colSpan);
      col += cell.m_colSpan;
    }
  }

  if (columnCount <= 0 || columnCount > c_maxColumns ||
      static_cast<qint64>(slotMap.size()) !=
          static_cast<qint64>(rowCount) * static_cast<qint64>(columnCount)) {
    // Either a row is short or one overruns: the grid does not tile the
    // rectangle exactly. qint64 for the product, which two in-range dimensions
    // can still overflow as int.
    return false;
  }

  p_table.m_rowCount = rowCount;
  p_table.m_columnCount = columnCount;
  p_table.m_originAt.resize(rowCount * columnCount);
  for (int r = 0; r < rowCount; ++r) {
    for (int c = 0; c < columnCount; ++c) {
      const auto it = slotMap.constFind(key(r, c));
      if (it == slotMap.constEnd()) {
        return false;
      }
      p_table.m_originAt[r * columnCount + c] = it.value();
    }
  }

  // Header shape: all-`th` row 0 with all-`td` below, or all-`td` throughout.
  bool headerRow = false;
  for (int r = 0; r < rowCount; ++r) {
    const auto &cells = p_table.m_rows.at(r).m_cells;
    if (cells.isEmpty()) {
      return false;
    }
    const bool first = cells.first().m_header;
    for (const auto &cell : cells) {
      if (cell.m_header != first) {
        return false; // Mixed `th`/`td` in one row.
      }
    }
    if (r == 0) {
      headerRow = first;
    } else if (first) {
      return false; // A `<th>` row below row 0.
    }
  }
  p_table.m_hasHeaderRow = headerRow;

  // At most one distinct `align` per column, among the cells that declare one.
  p_table.m_alignments.fill(QString(), columnCount);
  for (const auto &row : p_table.m_rows) {
    for (const auto &cell : row.m_cells) {
      if (cell.m_align.isEmpty()) {
        continue;
      }
      for (int dc = 0; dc < cell.m_colSpan; ++dc) {
        const int c = cell.m_origin.x() + dc;
        if (p_table.m_alignments.at(c).isEmpty()) {
          p_table.m_alignments[c] = cell.m_align;
        } else if (p_table.m_alignments.at(c) != cell.m_align) {
          return false;
        }
      }
    }
  }

  for (const auto &row : p_table.m_rows) {
    for (const auto &cell : row.m_cells) {
      if (cell.m_payloadMalformed) {
        p_table.m_anyPayloadMalformed = true;
      } else if (cell.m_hasPayload) {
        p_table.m_anyPayloadPresent = true;
      }
    }
  }

  return true;
}

// Parse a whole `<table …>…</table>` starting at @p_idx. On success @p_next is
// the local index just past the closing tag.
bool parseTable(const QString &p_text, int p_idx, int p_nameEnd, int p_baseOffset,
                HtmlTable &p_table, int &p_next) {
  int tagEnd = -1;
  if (!parseAttrs(p_text, p_nameEnd, lineLimit(p_text, p_idx), p_baseOffset, p_table.m_attrs,
                  tagEnd)) {
    return false;
  }
  p_table.m_openTagStart = p_baseOffset + p_idx;
  p_table.m_openTagEnd = p_baseOffset + tagEnd;

  int i = tagEnd;
  while (true) {
    i = skipSpaces(p_text, i);
    QString name;
    bool closing = false;
    int nameEnd = 0;
    if (!tagAt(p_text, i, name, closing, nameEnd)) {
      // Text content directly inside `<table>` / between rows, or a stray '<'.
      return false;
    }

    if (closing && name == QStringLiteral("table")) {
      const int after = consumeCloseTag(p_text, nameEnd);
      if (after < 0) {
        return false;
      }
      p_table.m_tableStart = p_baseOffset + p_idx;
      p_table.m_tableEnd = p_baseOffset + after;
      p_next = after;
      break;
    }

    // `<caption>`, `<colgroup>`, `<thead>`, `<tbody>`, `<tfoot>`, a nested
    // `<table>` and anything else are all refused by falling through here.
    if (closing || name != QStringLiteral("tr")) {
      return false;
    }

    HtmlTableRow row;
    int rowTagEnd = -1;
    if (!parseAttrs(p_text, nameEnd, lineLimit(p_text, i), p_baseOffset, row.m_attrs, rowTagEnd)) {
      return false;
    }
    row.m_tagStart = p_baseOffset + i;
    row.m_tagEnd = p_baseOffset + rowTagEnd;
    i = rowTagEnd;

    while (true) {
      i = skipSpaces(p_text, i);
      QString cellName;
      bool cellClosing = false;
      int cellNameEnd = 0;
      if (!tagAt(p_text, i, cellName, cellClosing, cellNameEnd)) {
        return false;
      }

      if (cellClosing && cellName == QStringLiteral("tr")) {
        const int after = consumeCloseTag(p_text, cellNameEnd);
        if (after < 0) {
          return false;
        }
        i = after;
        break;
      }

      if (cellClosing || (cellName != QStringLiteral("td") && cellName != QStringLiteral("th"))) {
        return false;
      }

      HtmlTableCell cell;
      int next = -1;
      if (!parseCell(p_text, i, p_baseOffset, cellName, cellNameEnd, cell, next)) {
        return false;
      }
      row.m_cells.append(cell);
      i = next;
    }

    if (row.m_cells.isEmpty()) {
      return false;
    }
    p_table.m_rows.append(row);
  }

  return buildGrid(p_table);
}

} // namespace

QPoint HtmlTable::originAt(int p_row, int p_column) const {
  // HtmlTable is a public, mutable struct: the counts and the origin vector can
  // be made inconsistent by a caller, so the index is validated against the
  // vector itself rather than only against the counts.
  if (p_row < 0 || p_row >= m_rowCount || p_column < 0 || p_column >= m_columnCount) {
    return c_noOrigin;
  }
  const qint64 index = static_cast<qint64>(p_row) * m_columnCount + p_column;
  if (index < 0 || index >= m_originAt.size()) {
    return c_noOrigin;
  }
  return m_originAt.at(static_cast<int>(index));
}

const HtmlTableCell *HtmlTable::cellAt(int p_row, int p_column) const {
  const QPoint origin = originAt(p_row, p_column);
  if (origin == c_noOrigin || origin.y() < 0 || origin.y() >= m_rows.size()) {
    return nullptr;
  }
  const auto &cells = m_rows.at(origin.y()).m_cells;
  for (const auto &cell : cells) {
    if (cell.m_origin == origin) {
      return &cell;
    }
  }
  return nullptr;
}

QVector<HtmlTable> vte::scanHtmlTables(const QString &p_text, int p_baseOffset,
                                       RawTextState *p_state) {
  QVector<HtmlTable> tables;

  // Local index before which a `<table` candidate is ignored, because it lies
  // inside a table already captured -- or inside one already REFUSED, so a
  // refusal never degrades into capturing a nested table on its own.
  int suppressUntil = 0;

  int i = 0;
  while (i < p_text.size()) {
    if (p_state && !p_state->m_element.isEmpty()) {
      const int close = findRawTextClose(p_text, i, p_state->m_element);
      if (close < 0) {
        return tables;
      }
      p_state->m_element.clear();
      i = close;
      continue;
    }

    const int lt = p_text.indexOf(QLatin1Char('<'), i);
    if (lt < 0) {
      break;
    }

    if (p_text.mid(lt, 4) == QLatin1String("<!--")) {
      const int end = p_text.indexOf(QStringLiteral("-->"), lt + 4);
      if (end < 0) {
        break;
      }
      i = end + 3;
      continue;
    }

    QString name;
    bool closing = false;
    int nameEnd = 0;
    if (!tagAt(p_text, lt, name, closing, nameEnd)) {
      i = lt + 1;
      continue;
    }

    if (!closing && isRawTextElement(name)) {
      const int tagEnd = skipTag(p_text, lt);
      if (tagEnd < 0) {
        if (p_state) {
          p_state->m_element = name;
        }
        return tables;
      }
      if (p_state) {
        p_state->m_element = name;
      }
      i = tagEnd;
      continue;
    }

    if (!closing && name == QStringLiteral("img")) {
      // NOT a table construct, and skipped only so this lexer advances exactly
      // as the `<img>` scanner's does. A malformed or multiline `<img` resumes
      // just after its name there, and the walker asserts the two scanners
      // leave RawTextState in the same place.
      QVector<HtmlAttr> attrs;
      bool parsed = false;
      i = resumeAfterImgTag(p_text, nameEnd, 0, attrs, parsed);
      continue;
    }

    if (!closing && name == QStringLiteral("table") && lt >= suppressUntil) {

      HtmlTable table;
      int next = -1;
      if (parseTable(p_text, lt, nameEnd, p_baseOffset, table, next)) {
        tables.append(table);
        suppressUntil = next;
      } else {
        suppressUntil = findTableRegionEnd(p_text, lt);
      }
    }

    // Advance by ONE tag, never over the whole table: the `<img>` scanner walks
    // this slice the same way, and the walker asserts the two agree on the
    // outgoing raw-text state.
    const int tagEnd = skipTag(p_text, lt);
    i = tagEnd < 0 ? lt + 1 : tagEnd;
  }

  return tables;
}

QString vte::escapePayload(const QString &p_text) {
  QString out;
  out.reserve(p_text.size() + 8);
  for (const QChar ch : p_text) {
    if (ch == QLatin1Char('\\')) {
      out += QLatin1String("\\\\");
    } else if (ch == QLatin1Char('-')) {
      out += QLatin1String("\\-");
    } else {
      out += ch;
    }
  }
  return out;
}

bool vte::unescapePayload(const QString &p_text, QString &p_out) {
  QString out;
  out.reserve(p_text.size());
  int i = 0;
  while (i < p_text.size()) {
    const QChar ch = p_text.at(i);
    if (ch != QLatin1Char('\\')) {
      if (ch == QLatin1Char('-')) {
        // An unescaped `-` cannot come out of escapePayload(); the value was
        // not produced by this codec, so it is malformed.
        return false;
      }
      out += ch;
      ++i;
      continue;
    }
    if (i + 1 >= p_text.size()) {
      return false; // Dangling backslash.
    }
    const QChar next = p_text.at(i + 1);
    if (next != QLatin1Char('\\') && next != QLatin1Char('-')) {
      return false;
    }
    out += next;
    i += 2;
  }

  p_out = out;
  return true;
}

QString vte::rewriteHtmlTagAttr(const QString &p_tag, const QString &p_name,
                                const QString &p_value) {
  QString name;
  bool closing = false;
  int nameEnd = 0;
  if (!tagAt(p_tag, 0, name, closing, nameEnd) || closing) {
    return p_tag;
  }

  QVector<HtmlAttr> attrs;
  int tagEnd = -1;
  if (!parseAttrs(p_tag, nameEnd, p_tag.size(), 0, attrs, tagEnd)) {
    return p_tag;
  }

  const QString lower = p_name.toLower();

  // EVERY occurrence, not just the first. First-wins is what the readers do, so
  // rewriting only the first would leave a later duplicate to become effective
  // -- a table whose serialized geometry silently differs from the live one.
  // Collected back to front so an earlier span stays valid while a later one is
  // being edited.
  QVector<const HtmlAttr *> matches;
  for (const auto &attr : attrs) {
    if (attr.m_name == lower) {
      matches.append(&attr);
    }
  }

  QString out = p_tag;
  for (int i = matches.size() - 1; i >= 0; --i) {
    const HtmlAttr *attr = matches.at(i);
    int start = attr->m_attrStart;
    const int end = attr->m_attrEnd;
    // Only the FIRST occurrence may carry the new value; every later duplicate
    // is removed outright.
    if (p_value.isNull() || i > 0) {
      // Take the whitespace that introduced it too, so removing the last
      // attribute does not leave `<td >`.
      while (start > 0 && out.at(start - 1).isSpace()) {
        --start;
      }
      out.remove(start, end - start);
    } else {
      out.replace(start, end - start,
                  QStringLiteral("%1=\"%2\"").arg(lower, htmlEscapeAttrValue(p_value)));
    }
  }

  if (!matches.isEmpty() || p_value.isNull()) {
    return out;
  }

  // Absent: append just before the closing `>`, keeping a `/>` intact.
  int insertAt = out.lastIndexOf(QLatin1Char('>'));
  if (insertAt < 0) {
    return p_tag;
  }
  if (insertAt > 0 && out.at(insertAt - 1) == QLatin1Char('/')) {
    --insertAt;
  }
  out.insert(insertAt, QStringLiteral(" %1=\"%2\"").arg(lower, htmlEscapeAttrValue(p_value)));
  return out;
}
