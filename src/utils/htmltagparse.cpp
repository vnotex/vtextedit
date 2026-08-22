#include "htmltagparse.h"

#include <QByteArray>

// cmark's HTML entity decoder, and the complete named-reference table it comes
// with. Reaching into cmark's internal headers is already established practice
// in this library (markdowneditor/cmarkadapter.cpp includes <node.h>), and the
// alternative -- a hand-maintained subset -- is what this replaced: a
// destination the renderer decodes but the scanner does not is not merely a
// cosmetic difference. It feeds obsolete-image cleanup, which DELETES assets.
extern "C" {
#include <buffer.h>
#include <cmark.h>
#include <houdini.h>
}

namespace vte {
namespace htmltag {

bool isNameStart(QChar p_ch) { return p_ch.isLetter(); }

bool isNameChar(QChar p_ch) {
  return !p_ch.isSpace() && p_ch != QLatin1Char('=') && p_ch != QLatin1Char('>') &&
         p_ch != QLatin1Char('/') && p_ch != QLatin1Char('<') && p_ch != QLatin1Char('"') &&
         p_ch != QLatin1Char('\'');
}

bool isRawTextElement(const QString &p_lowerName) {
  return p_lowerName == QStringLiteral("script") || p_lowerName == QStringLiteral("style") ||
         p_lowerName == QStringLiteral("textarea") || p_lowerName == QStringLiteral("title");
}

QString decodeEntities(const QString &p_text) {
  if (!p_text.contains(QLatin1Char('&'))) {
    return p_text;
  }

  const QByteArray utf8 = p_text.toUtf8();
  cmark_strbuf buf = CMARK_BUF_INIT(cmark_get_default_mem_allocator());
  houdini_unescape_html_f(&buf, reinterpret_cast<const uint8_t *>(utf8.constData()), utf8.size());
  const QString decoded = QString::fromUtf8(reinterpret_cast<const char *>(buf.ptr), buf.size);
  cmark_strbuf_free(&buf);
  return decoded;
}

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

bool parseAttrs(const QString &p_text, int p_idx, int p_limit, int p_baseOffset,
                QVector<HtmlAttr> &p_attrs, int &p_tagEnd) {
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

    HtmlAttr attr;
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

const HtmlAttr *findAttr(const QVector<HtmlAttr> &p_attrs, const char *p_name) {
  const QLatin1String name(p_name);
  for (const auto &attr : p_attrs) {
    if (attr.m_name == name) {
      return &attr;
    }
  }
  return nullptr;
}

int resumeAfterImgTag(const QString &p_text, int p_nameEnd, int p_baseOffset,
                      QVector<HtmlAttr> &p_attrs, bool &p_parsed) {
  int limit = p_text.indexOf(QLatin1Char('\n'), p_nameEnd);
  if (limit < 0) {
    limit = p_text.size();
  }

  int tagEnd = -1;
  p_parsed = parseAttrs(p_text, p_nameEnd, limit, p_baseOffset, p_attrs, tagEnd);
  // Multiline or malformed: resume just after the NAME, never skipTag() past a
  // newline. See the header for why the two scanners must agree here.
  return p_parsed ? tagEnd : p_nameEnd;
}

} // namespace htmltag
} // namespace vte
