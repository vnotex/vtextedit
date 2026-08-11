#ifndef VTEXTEDIT_PREVIEW_H
#define VTEXTEDIT_PREVIEW_H

#include "vtextedit_export.h"

#include <QMetaType>
#include <QScopedPointer>
#include <QSharedData>
#include <QSharedDataPointer>
#include <QSharedPointer>
#include <QString>
#include <QVector>

namespace vte {
// Closed set of element types which the library can describe as an in-place
// preview. Applications may register renderers for any of them but may not
// extend the set.
enum class PreviewElementType { Image, Code, Math, Table };

// Number of values in PreviewElementType. Derive every per-type array from
// this so extending the enum cannot silently overflow one.
const int c_previewElementTypeCount = static_cast<int>(PreviewElementType::Table) + 1;

// Where the rendered preview is placed relative to its Markdown source.
enum class PreviewPlacement {
  // A band right after the block containing the last character of the source.
  BlockAfterSource,

  // A band above the visual line containing the source, aligned to the source
  // span. This is the legacy in-place image placement.
  InlineAboveLine
};

// Column alignment of a Markdown table, as declared by the delimiter row.
enum class PreviewTableAlignment { None, Left, Center, Right };

class PreviewPrivate;
class ImagePreviewPrivate;
class CodePreviewPrivate;
class MathPreviewPrivate;
class TablePreviewPrivate;
class PreviewBuilder;

// An immutable snapshot of one parsed Markdown element.
//
// Snapshots are produced by the library's own parser adapter and shared as
// QSharedPointer<const Preview>. Applications consume them through the typed
// accessors of the final subclasses; they cannot create or mutate them.
class VTEXTEDIT_EXPORT Preview {
public:
  virtual ~Preview();

  PreviewElementType type() const;

  PreviewPlacement placement() const;

  // Revision of the parse generation which produced this snapshot.
  quint64 revision() const;

  // Half-open UTF-16 document range [startPos(), endPos()) of the source.
  int startPos() const;
  int endPos() const;

  // Exact source Markdown of [startPos(), endPos()), including any block
  // container prefixes and excluding the terminating paragraph separator.
  const QString &sourceMarkdown() const;

protected:
  explicit Preview(PreviewPrivate *p_d);

  QScopedPointer<PreviewPrivate> m_d;

private:
  Preview(const Preview &) = delete;
  Preview &operator=(const Preview &) = delete;
};

class VTEXTEDIT_EXPORT ImagePreview final : public Preview {
public:
  ~ImagePreview() Q_DECL_OVERRIDE;

  const QString &destination() const;

  const QString &alternateText() const;

  const QString &title() const;

private:
  friend class PreviewBuilder;

  ImagePreview(PreviewPrivate *p_d, ImagePreviewPrivate *p_imageData);

  QScopedPointer<ImagePreviewPrivate> m_imageData;
};

class VTEXTEDIT_EXPORT CodePreview final : public Preview {
public:
  ~CodePreview() Q_DECL_OVERRIDE;

  // Info string of the fence, may be empty.
  const QString &language() const;

  // Raw code without the fences.
  const QString &code() const;

private:
  friend class PreviewBuilder;

  CodePreview(PreviewPrivate *p_d, CodePreviewPrivate *p_codeData);

  QScopedPointer<CodePreviewPrivate> m_codeData;
};

class VTEXTEDIT_EXPORT MathPreview final : public Preview {
public:
  ~MathPreview() Q_DECL_OVERRIDE;

  // Expression without the delimiters.
  const QString &expression() const;

  // Whether it is display math ($$...$$) instead of inline math ($...$).
  bool isDisplayMath() const;

private:
  friend class PreviewBuilder;

  MathPreview(PreviewPrivate *p_d, MathPreviewPrivate *p_mathData);

  QScopedPointer<MathPreviewPrivate> m_mathData;
};

class VTEXTEDIT_EXPORT TablePreview final : public Preview {
public:
  ~TablePreview() Q_DECL_OVERRIDE;

  // Number of value rows. Row 0 is the header row. The delimiter row is
  // metadata and is not part of the matrix.
  int rowCount() const;

  // Number of columns declared by the header/delimiter rows.
  int columnCount() const;

  // Raw Markdown of every cell, row major. Row 0 is the header row. Rows may
  // be ragged: a body row may have fewer or more cells than columnCount().
  const QVector<QVector<QString>> &cells() const;

  // Column alignments. Size equals columnCount().
  const QVector<PreviewTableAlignment> &alignments() const;

  // Block container prefix of each matrix row's source line. Size equals
  // rowCount().
  const QVector<QString> &rowPrefixes() const;

  // Block container prefix of the delimiter row's source line.
  const QString &delimiterPrefix() const;

private:
  friend class PreviewBuilder;

  TablePreview(PreviewPrivate *p_d, TablePreviewPrivate *p_tableData);

  QScopedPointer<TablePreviewPrivate> m_tableData;
};

class PreviewReplacementResultData;

// Outcome of a PreviewWidgetContext::requestSourceReplacement() request.
class VTEXTEDIT_EXPORT PreviewReplacementResult {
public:
  enum Status {
    Accepted,

    // No active preview owns the requesting identity anymore.
    UnknownIdentity,

    // The widget targeted a superseded snapshot.
    StaleSnapshot,

    // The live anchored source no longer matches the expected source.
    SourceMismatch,

    // The live anchored range is not resolvable.
    InvalidRange,

    // The editor is read-only.
    ReadOnly,

    // The proposed Markdown could not be parsed.
    ParseFailure,

    // The proposed Markdown did not resolve to exactly one element.
    ElementCountMismatch,

    // The proposed Markdown resolved to an element of a different type.
    TypeMismatch,

    // The host could not apply the request right now, because it arrived from
    // inside a layout pass or another callback during which mutating the
    // editor's document is not supported.
    //
    // Nothing was touched: no host state, no document state and no snapshot.
    // The request simply did not happen, and the *requester* owns the retry -
    // typically by asking again once its own debounce fires. The host retries
    // its own built-in table sheets on its behalf, but it deliberately does
    // not replay a third-party widget's request verbatim, which would deliver
    // two completions for one logical request.
    Deferred
  };

  PreviewReplacementResult();

  PreviewReplacementResult(const PreviewReplacementResult &p_other);

  PreviewReplacementResult &operator=(const PreviewReplacementResult &p_other);

  ~PreviewReplacementResult();

  quint64 identity() const;
  void setIdentity(quint64 p_identity);

  quint64 requestedRevision() const;
  void setRequestedRevision(quint64 p_revision);

  quint64 currentRevision() const;
  void setCurrentRevision(quint64 p_revision);

  Status status() const;
  void setStatus(Status p_status);

  const QString &diagnostic() const;
  void setDiagnostic(const QString &p_diagnostic);

  bool isAccepted() const;

private:
  QSharedDataPointer<PreviewReplacementResultData> m_data;
};

// Register the metatypes needed by queued signals carrying preview types.
// Idempotent and safe to call from any thread after QCoreApplication exists.
VTEXTEDIT_EXPORT void registerPreviewMetaTypes();
} // namespace vte

Q_DECLARE_METATYPE(vte::PreviewElementType)
Q_DECLARE_METATYPE(vte::PreviewPlacement)
Q_DECLARE_METATYPE(vte::PreviewTableAlignment)
Q_DECLARE_METATYPE(vte::PreviewReplacementResult)
Q_DECLARE_METATYPE(QSharedPointer<const vte::Preview>)
Q_DECLARE_METATYPE(QVector<QSharedPointer<const vte::Preview>>)

#endif // VTEXTEDIT_PREVIEW_H
