#include <vtextedit/htmlimgscanner.h>

#include "htmltagparse.h"

using namespace vte;
using namespace vte::htmltag;

namespace {

int positiveIntAttr(const HtmlAttr *p_attr) {
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

const HtmlAttr *HtmlImgTag::attr(const char *p_name) const { return findAttr(m_attrs, p_name); }

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

    // An `<img …>`: it must open and close within this line. The advance rule
    // lives in resumeAfterImgTag(), because the `<table>` scanner has to
    // reproduce it exactly for the shared raw-text state to agree.
    HtmlImgTag tag;
    bool parsed = false;
    const int next = resumeAfterImgTag(p_text, nameEnd, p_baseOffset, tag.m_attrs, parsed);
    if (!parsed) {
      // Multiline or malformed: ignored, exactly as before this feature existed.
      i = next;
      continue;
    }

    tag.m_tagStart = p_baseOffset + lt;
    tag.m_tagEnd = p_baseOffset + next;
    if (!tag.src().isEmpty()) {
      tags.push_back(tag);
    }
    i = next;
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
