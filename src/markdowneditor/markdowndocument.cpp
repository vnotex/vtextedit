#include <vtextedit/markdowndocument.h>

#include "cmarkadapter.h"

#include <QMutexLocker>

#include <memory>
#include <utility>

namespace vte {
class MarkdownDocumentPrivate {
public:
  MarkdownDocumentPrivate(cmark_node *p_root, const QByteArray &p_source, int p_parseOptions,
                          const QVector<md::HeadingInfo> &p_headings)
      : m_root(p_root, cmark_node_free), m_source(p_source), m_parseOptions(p_parseOptions),
        m_headings(p_headings) {}

  const std::unique_ptr<cmark_node, decltype(&cmark_node_free)> m_root;
  const QByteArray m_source;
  const int m_parseOptions;
  const QVector<md::HeadingInfo> m_headings;
};

MarkdownDocument::MarkdownDocument(MarkdownDocumentPrivate *p_data) : d(p_data) {}
MarkdownDocument::~MarkdownDocument() = default;

MarkdownDocumentPtr MarkdownDocument::parse(const QByteArray &p_source, int p_parseOptions) {
  std::unique_ptr<cmark_node, decltype(&cmark_node_free)> root(
      cmark_parse_document(p_source.constData(), p_source.size(), p_parseOptions), cmark_node_free);
  if (!root) {
    return {};
  }
  std::unique_ptr<cmark_iter, decltype(&cmark_iter_free)> iter(cmark_iter_new(root.get()),
                                                               cmark_iter_free);
  if (!iter) {
    return {};
  }

  // Only headings need a source map. Read mode never creates editor projections.
  std::unique_ptr<LineOffsetTable> offsets;
  QVector<md::HeadingInfo> headings;
  md::HeadingRenderOrder headingOrder;
  cmark_event_type event;
  while ((event = cmark_iter_next(iter.get())) != CMARK_EVENT_DONE) {
    cmark_node *node = cmark_iter_get_node(iter.get());
    if (event != CMARK_EVENT_ENTER) {
      continue;
    }
    headingOrder.observe(node);
    if (cmark_node_get_type(node) != CMARK_NODE_HEADING) {
      continue;
    }
    if (!offsets) {
      offsets.reset(new LineOffsetTable(p_source));
    }
    int start = 0;
    int end = 0;
    if (cmarkNodeSpan(node, *offsets, start, end)) {
      headings.append(md::headingInfo(node, start, end, 0));
    }
  }
  headingOrder.finish(headings);
  return MarkdownDocumentPtr(new MarkdownDocument(
      new MarkdownDocumentPrivate(root.release(), p_source, p_parseOptions, headings)));
}

int MarkdownDocument::viewerParseOptions(bool p_htmlEnabled, bool p_linkifyEnabled) {
  return CMARK_OPT_DEFAULT | CMARK_OPT_VIEWER | (p_htmlEnabled ? 0 : CMARK_OPT_HTML_AS_TEXT) |
         (p_linkifyEnabled ? CMARK_OPT_LINKIFY : 0);
}

const QByteArray &MarkdownDocument::source() const { return d->m_source; }
int MarkdownDocument::parseOptions() const { return d->m_parseOptions; }
const QVector<md::HeadingInfo> &MarkdownDocument::headings() const { return d->m_headings; }

QString MarkdownDocument::renderHtml(const QStringList &p_headingIds, bool p_autoBreaks,
                                     bool p_htmlEnabled) const {
  QVector<QByteArray> ids;
  ids.reserve(p_headingIds.size());
  QVector<const char *> pointers;
  pointers.reserve(p_headingIds.size());
  for (const auto &id : p_headingIds) {
    ids.append(id.toUtf8());
    pointers.append(ids.constLast().constData());
  }
  cmark_html_options options = {};
  options.options = p_autoBreaks ? CMARK_OPT_HARDBREAKS : 0;
  options.viewer = 1;
  options.render_raw_html = p_htmlEnabled;
  options.allow_file_links = 1;
  options.allow_svg_data_images = 1;
  options.heading_ids = pointers.constData();
  options.heading_count = static_cast<size_t>(pointers.size());
  char *html = cmark_render_html_with_options(d->m_root.get(), &options);
  if (!html) {
    return QString();
  }
  const QString result = *html ? QString::fromUtf8(html) : QStringLiteral("");
  cmark_get_default_mem_allocator()->free(html);
  return result;
}

bool md::MarkdownDocumentRequest::request() {
  QMutexLocker lock(&m_mutex);
  if (m_finished && !m_document) {
    return false;
  }
  m_discarded = false;
  return true;
}

void md::MarkdownDocumentRequest::clear() {
  MarkdownDocumentPtr released;
  {
    QMutexLocker lock(&m_mutex);
    m_discarded = true;
    released.swap(m_document);
  }
}

MarkdownDocumentPtr md::MarkdownDocumentRequest::takeDocument() {
  QMutexLocker lock(&m_mutex);
  return std::move(m_document);
}

void md::MarkdownDocumentRequest::finish(cmark_node *p_root, const QByteArray &p_source,
                                         int p_parseOptions,
                                         const QVector<HeadingInfo> &p_headings) {
  {
    QMutexLocker lock(&m_mutex);
    m_finished = true;
    // Keep only the bounded worker-to-GUI handoff until demand is known at
    // acceptance. Explicit eviction wins even when it races the parser boundary.
    if (p_root && !m_discarded) {
      m_document.reset(new MarkdownDocument(
          new MarkdownDocumentPrivate(p_root, p_source, p_parseOptions, p_headings)));
      return;
    }
  }
  // A discarded large tree must not hold up GUI demand/eviction on this mutex.
  cmark_node_free(p_root);
}
} // namespace vte
