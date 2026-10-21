#ifndef VTEXTEDIT_MARKDOWNDOCUMENT_H
#define VTEXTEDIT_MARKDOWNDOCUMENT_H

#include <QByteArray>
#include <QMetaType>
#include <QScopedPointer>
#include <QSharedPointer>
#include <QStringList>
#include <QVector>

#include <vtextedit/markdownhighlighterdata.h>

namespace vte {
class MarkdownDocument;
class MarkdownDocumentPrivate;
namespace md {
class MarkdownDocumentRequest;
}

using MarkdownDocumentPtr = QSharedPointer<const MarkdownDocument>;

// Immutable source, grammar and full native syntax tree. Concurrent render calls
// own all output state separately; no editor or GUI object is retained.
class VTEXTEDIT_EXPORT MarkdownDocument {
public:
  static MarkdownDocumentPtr parse(const QByteArray &p_source, int p_parseOptions);
  static int viewerParseOptions(bool p_htmlEnabled, bool p_linkifyEnabled);

  ~MarkdownDocument();

  const QByteArray &source() const;
  int parseOptions() const;
  const QVector<md::HeadingInfo> &headings() const;

  // A null QString means rendering failed. Empty HTML is a non-null string.
  QString renderHtml(const QStringList &p_headingIds, bool p_autoBreaks, bool p_htmlEnabled) const;

private:
  friend class md::MarkdownDocumentRequest;
  explicit MarkdownDocument(MarkdownDocumentPrivate *p_data);
  Q_DISABLE_COPY(MarkdownDocument)

  const QScopedPointer<MarkdownDocumentPrivate> d;
};
} // namespace vte

Q_DECLARE_METATYPE(vte::MarkdownDocumentPtr)

#endif // VTEXTEDIT_MARKDOWNDOCUMENT_H
