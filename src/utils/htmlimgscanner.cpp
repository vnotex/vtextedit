#include <vtextedit/htmlimgscanner.h>

#include <QHash>

using namespace vte;

namespace {

bool isNameStart(QChar p_ch) { return p_ch.isLetter(); }

bool isNameChar(QChar p_ch) {
  // Deliberately permissive: anything that is not whitespace and not one of the
  // attribute-syntax delimiters is part of the name. `data-*` and `xml:lang`
  // must survive so hasUnknownAttrs() can see them.
  return !p_ch.isSpace() && p_ch != QLatin1Char('=') && p_ch != QLatin1Char('>') &&
         p_ch != QLatin1Char('/') && p_ch != QLatin1Char('<') && p_ch != QLatin1Char('"') &&
         p_ch != QLatin1Char('\'');
}

bool isRawTextElement(const QString &p_lowerName) {
  return p_lowerName == QStringLiteral("script") || p_lowerName == QStringLiteral("style") ||
         p_lowerName == QStringLiteral("textarea") || p_lowerName == QStringLiteral("title");
}

// The handful of named entities that can legally appear in an attribute value
// spelled by a human or by any generator in this tree. Anything else is left
// alone rather than guessed at: an unrecognized `&foo;` is far more likely to be
// a literal ampersand in a query string than an entity.
const QHash<QString, QChar> &namedEntities() {
  static const QHash<QString, QChar> s_entities{
      {QStringLiteral("amp"), QLatin1Char('&')},   {QStringLiteral("lt"), QLatin1Char('<')},
      {QStringLiteral("gt"), QLatin1Char('>')},    {QStringLiteral("quot"), QLatin1Char('"')},
      {QStringLiteral("apos"), QLatin1Char('\'')}, {QStringLiteral("nbsp"), QChar(0x00A0)}};
  return s_entities;
}

QString decodeEntities(const QString &p_text) {
  if (!p_text.contains(QLatin1Char('&'))) {
    return p_text;
  }

  QString out;
  out.reserve(p_text.size());
  int i = 0;
  while (i < p_text.size()) {
    const QChar ch = p_text.at(i);
    if (ch != QLatin1Char('&')) {
      out.append(ch);
      ++i;
      continue;
    }

    const int semi = p_text.indexOf(QLatin1Char(';'), i + 1);
    // A reference is short; a distant `;` is punctuation, not a terminator.
    if (semi < 0 || semi - i > 12) {
      out.append(ch);
      ++i;
      continue;
    }

    const QString body = p_text.mid(i + 1, semi - i - 1);
    if (body.isEmpty()) {
      out.append(ch);
      ++i;
      continue;
    }

    if (body.at(0) == QLatin1Char('#')) {
      if (body.size() < 2) {
        out.append(ch);
        ++i;
        continue;
      }
      bool ok = false;
      const uint code = body.at(1) == QLatin1Char('x') || body.at(1) == QLatin1Char('X')
                            ? body.mid(2).toUInt(&ok, 16)
                            : body.mid(1).toUInt(&ok, 10);
      if (ok && code > 0 && code <= 0x10FFFF) {
        if (code <= 0xFFFF) {
          out.append(QChar(static_cast<ushort>(code)));
        } else {
          out.append(QChar(QChar::highSurrogate(code)));
          out.append(QChar(QChar::lowSurrogate(code)));
        }
        i = semi + 1;
        continue;
      }
      out.append(ch);
      ++i;
      continue;
    }

    const auto it = namedEntities().constFind(body.toLower());
    if (it != namedEntities().constEnd()) {
      out.append(it.value());
      i = semi + 1;
      continue;
    }

    out.append(ch);
    ++i;
  }

  return out;
}

// Skip past the end of a tag that started at p_idx (which points at '<'),
// honoring quoted attribute values so a '>' inside `alt="a>b"` does not
// terminate it. Returns the index just past the closing '>', or -1 when the tag
// is never closed.
int skipTag(const QString &p_text, int p_idx) {
  int i = p_idx + 1;
  while (i < p_text.size()) {
    const QChar ch = p_text.at(i);
    if (ch == QLatin1Char('"') || ch == QLatin1Char('\'')) {
      const int close = p_text.indexOf(ch, i + 1);
      if (close < 0) {
        return -1;
      }
      i = close + 1;
      continue;
    }
    if (ch == QLatin1Char('>')) {
      return i + 1;
    }
    ++i;
  }
  return -1;
}

// Find the closing tag of the raw-text element p_element, starting at p_idx.
// Returns the index just past its '>', or -1 when it is not closed in p_text.
int findRawTextClose(const QString &p_text, int p_idx, const QString &p_element) {
  int i = p_idx;
  while (i < p_text.size()) {
    const int lt = p_text.indexOf(QLatin1Char('<'), i);
    if (lt < 0 || lt + 1 >= p_text.size()) {
      return -1;
    }
    if (p_text.at(lt + 1) != QLatin1Char('/')) {
      i = lt + 1;
      continue;
    }

    int j = lt + 2;
    QString name;
    while (j < p_text.size() && isNameChar(p_text.at(j))) {
      name.append(p_text.at(j));
      ++j;
    }
    if (name.toLower() != p_element) {
      i = lt + 1;
      continue;
    }

    while (j < p_text.size() && p_text.at(j).isSpace()) {
      ++j;
    }
    if (j < p_text.size() && p_text.at(j) == QLatin1Char('>')) {
      return j + 1;
    }
    i = lt + 1;
  }
  return -1;
}

// Parse the attributes of a tag whose name ends at p_idx, refusing to read past
// p_limit (the start of the next newline, per the one-line rule). On success
// returns true, fills p_attrs and sets p_tagEnd to the index just past the
// closing '>'.
bool parseAttrs(const QString &p_text, int p_idx, int p_limit, int p_baseOffset,
                QVector<HtmlImgAttr> &p_attrs, int &p_tagEnd) {
  int i = p_idx;
  while (i < p_limit) {
    while (i < p_limit && p_text.at(i).isSpace()) {
      ++i;
    }
    if (i >= p_limit) {
      return false;
    }

    const QChar ch = p_text.at(i);
    if (ch == QLatin1Char('>')) {
      p_tagEnd = i + 1;
      return true;
    }
    if (ch == QLatin1Char('/')) {
      // `/` is only meaningful immediately before `>`; anywhere else it is a
      // stray character in a malformed tag and is simply skipped.
      ++i;
      continue;
    }
    if (!isNameChar(ch)) {
      // '<', a quote outside a value: malformed. Refuse the whole tag rather
      // than guess at where the attribute list resumes.
      return false;
    }

    HtmlImgAttr attr;
    const int nameStart = i;
    while (i < p_limit && isNameChar(p_text.at(i))) {
      ++i;
    }
    attr.m_name = p_text.mid(nameStart, i - nameStart).toLower();
    attr.m_attrStart = p_baseOffset + nameStart;

    int afterName = i;
    while (i < p_limit && p_text.at(i).isSpace()) {
      ++i;
    }
    if (i >= p_limit || p_text.at(i) != QLatin1Char('=')) {
      // Bare attribute: no value, so the value span collapses onto the end.
      attr.m_attrEnd = p_baseOffset + afterName;
      attr.m_valueStart = attr.m_attrEnd;
      attr.m_valueEnd = attr.m_attrEnd;
      p_attrs.push_back(attr);
      i = afterName;
      continue;
    }

    ++i; // past '='
    while (i < p_limit && p_text.at(i).isSpace()) {
      ++i;
    }
    if (i >= p_limit) {
      return false;
    }

    const QChar quote = p_text.at(i);
    if (quote == QLatin1Char('"') || quote == QLatin1Char('\'')) {
      const int valueStart = i + 1;
      const int close = p_text.indexOf(quote, valueStart);
      if (close < 0 || close >= p_limit) {
        // An unterminated quote inside the line: not a tag we can trust.
        return false;
      }
      attr.m_quote = quote;
      attr.m_valueStart = p_baseOffset + valueStart;
      attr.m_valueEnd = p_baseOffset + close;
      attr.m_value = decodeEntities(p_text.mid(valueStart, close - valueStart));
      attr.m_attrEnd = p_baseOffset + close + 1;
      i = close + 1;
    } else {
      const int valueStart = i;
      while (i < p_limit && !p_text.at(i).isSpace() && p_text.at(i) != QLatin1Char('>')) {
        ++i;
      }
      if (i == valueStart) {
        return false;
      }
      attr.m_valueStart = p_baseOffset + valueStart;
      attr.m_valueEnd = p_baseOffset + i;
      attr.m_value = decodeEntities(p_text.mid(valueStart, i - valueStart));
      attr.m_attrEnd = p_baseOffset + i;
    }

    p_attrs.push_back(attr);
  }

  // Hit the newline (or the end of the slice) without a '>': a multiline tag,
  // which is out of scope by design.
  return false;
}

int positiveIntAttr(const HtmlImgAttr *p_attr) {
  if (!p_attr) {
    return 0;
  }
  bool ok = false;
  const int value = p_attr->m_value.toInt(&ok);
  if (!ok || value <= 0) {
    return 0;
  }
  return value;
}

} // namespace

const HtmlImgAttr *HtmlImgTag::attr(const char *p_name) const {
  const QLatin1String name(p_name);
  for (const auto &attr : m_attrs) {
    if (attr.m_name == name) {
      return &attr;
    }
  }
  return nullptr;
}

bool HtmlImgTag::hasUnknownAttrs() const {
  for (const auto &attr : m_attrs) {
    if (attr.m_name != QStringLiteral("src") && attr.m_name != QStringLiteral("alt") &&
        attr.m_name != QStringLiteral("title") && attr.m_name != QStringLiteral("width") &&
        attr.m_name != QStringLiteral("height")) {
      return true;
    }
  }
  return false;
}

bool HtmlImgTag::hasDuplicateAttrs() const {
  for (int i = 0; i < m_attrs.size(); ++i) {
    for (int j = i + 1; j < m_attrs.size(); ++j) {
      if (m_attrs.at(i).m_name == m_attrs.at(j).m_name) {
        return true;
      }
    }
  }
  return false;
}

QString HtmlImgTag::src() const {
  const auto *a = attr("src");
  return a ? a->m_value : QString();
}

QString HtmlImgTag::alt() const {
  const auto *a = attr("alt");
  return a ? a->m_value : QString();
}

QString HtmlImgTag::title() const {
  const auto *a = attr("title");
  return a ? a->m_value : QString();
}

int HtmlImgTag::width() const { return positiveIntAttr(attr("width")); }

int HtmlImgTag::height() const { return positiveIntAttr(attr("height")); }

QVector<HtmlImgTag> vte::scanHtmlImgTags(const QString &p_text, int p_baseOffset,
                                         RawTextState *p_state) {
  QVector<HtmlImgTag> tags;

  int i = 0;
  while (i < p_text.size()) {
    // Inside a raw-text element carried in from an earlier slice or an earlier
    // node: everything up to its closing tag is text.
    if (p_state && !p_state->m_element.isEmpty()) {
      const int close = findRawTextClose(p_text, i, p_state->m_element);
      if (close < 0) {
        // Unclosed: suppress to the end and keep the state for the next slice.
        return tags;
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

    int nameStart = lt + 1;
    if (nameStart < p_text.size() && p_text.at(nameStart) == QLatin1Char('/')) {
      ++nameStart;
    }
    if (nameStart >= p_text.size() || !isNameStart(p_text.at(nameStart))) {
      // A bare '<' in prose.
      i = lt + 1;
      continue;
    }

    int nameEnd = nameStart;
    while (nameEnd < p_text.size() && isNameChar(p_text.at(nameEnd))) {
      ++nameEnd;
    }
    const QString name = p_text.mid(nameStart, nameEnd - nameStart).toLower();
    const bool closing = nameStart != lt + 1;

    if (!closing && isRawTextElement(name)) {
      const int tagEnd = skipTag(p_text, lt);
      if (tagEnd < 0) {
        // Unterminated opener: treat the rest as raw text, fail safe.
        if (p_state) {
          p_state->m_element = name;
        }
        return tags;
      }
      if (p_state) {
        p_state->m_element = name;
      }
      i = tagEnd;
      continue;
    }

    if (closing || name != QStringLiteral("img")) {
      // Skip the whole tag rather than just the '<', so an `<img …>` spelled
      // inside another tag's quoted attribute value is not mistaken for a tag.
      const int tagEnd = skipTag(p_text, lt);
      i = tagEnd < 0 ? lt + 1 : tagEnd;
      continue;
    }

    // An `<img …>`: it must open and close within this line.
    int limit = p_text.indexOf(QLatin1Char('\n'), nameEnd);
    if (limit < 0) {
      limit = p_text.size();
    }

    HtmlImgTag tag;
    int tagEnd = -1;
    if (!parseAttrs(p_text, nameEnd, limit, p_baseOffset, tag.m_attrs, tagEnd)) {
      // Multiline or malformed: ignored, exactly as before this feature existed.
      i = nameEnd;
      continue;
    }

    tag.m_tagStart = p_baseOffset + lt;
    tag.m_tagEnd = p_baseOffset + tagEnd;
    if (!tag.src().isEmpty()) {
      tags.push_back(tag);
    }
    i = tagEnd;
  }

  return tags;
}

QString vte::htmlEscapeAttrValue(const QString &p_value) {
  QString out;
  out.reserve(p_value.size());
  for (const QChar ch : p_value) {
    if (ch == QLatin1Char('&')) {
      out += QStringLiteral("&amp;");
    } else if (ch == QLatin1Char('<')) {
      out += QStringLiteral("&lt;");
    } else if (ch == QLatin1Char('>')) {
      out += QStringLiteral("&gt;");
    } else if (ch == QLatin1Char('"')) {
      out += QStringLiteral("&quot;");
    } else if (ch == QLatin1Char('\'')) {
      out += QStringLiteral("&#39;");
    } else {
      out += ch;
    }
  }
  return out;
}

QString vte::spellHtmlSrcAttr(const QString &p_url) {
  return QStringLiteral("src=\"%1\"").arg(htmlEscapeAttrValue(p_url));
}
