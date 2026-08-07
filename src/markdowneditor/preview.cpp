#include <vtextedit/preview.h>

#include <QCoreApplication>
#include <QTextCursor>
#include <QTextDocument>

#include "previewbuilder.h"

using namespace vte;

QString vte::previewSourceText(const QTextDocument *p_doc, int p_start, int p_end) {
  if (!p_doc || p_start < 0 || p_end < p_start) {
    return QString();
  }

  const int maxPos = p_doc->characterCount() - 1;
  if (p_end > maxPos) {
    return QString();
  }

  QTextCursor cursor(const_cast<QTextDocument *>(p_doc));
  cursor.setPosition(p_start);
  cursor.setPosition(p_end, QTextCursor::KeepAnchor);
  QString text = cursor.selectedText();
  // QTextCursor::selectedText() uses U+2029 for paragraph boundaries.
  text.replace(QChar(0x2029), QLatin1Char('\n'));
  return text;
}

namespace vte {
class PreviewPrivate {
public:
  PreviewElementType m_type = PreviewElementType::Image;

  PreviewPlacement m_placement = PreviewPlacement::BlockAfterSource;

  quint64 m_revision = 0;

  int m_startPos = 0;

  int m_endPos = 0;

  QString m_sourceMarkdown;
};

class ImagePreviewPrivate {
public:
  QString m_destination;

  QString m_alternateText;

  QString m_title;
};

class CodePreviewPrivate {
public:
  QString m_language;

  QString m_code;
};

class MathPreviewPrivate {
public:
  QString m_expression;

  bool m_displayMath = true;
};

class TablePreviewPrivate {
public:
  int m_columnCount = 0;

  QVector<QVector<QString>> m_cells;

  QVector<PreviewTableAlignment> m_alignments;

  QVector<QString> m_rowPrefixes;

  QString m_delimiterPrefix;
};

class PreviewReplacementResultData : public QSharedData {
public:
  quint64 m_identity = 0;

  quint64 m_requestedRevision = 0;

  quint64 m_currentRevision = 0;

  PreviewReplacementResult::Status m_status = PreviewReplacementResult::UnknownIdentity;

  QString m_diagnostic;
};
} // namespace vte

Preview::Preview(PreviewPrivate *p_d) : m_d(p_d) { Q_ASSERT(p_d); }

Preview::~Preview() {}

PreviewElementType Preview::type() const { return m_d->m_type; }

PreviewPlacement Preview::placement() const { return m_d->m_placement; }

quint64 Preview::revision() const { return m_d->m_revision; }

int Preview::startPos() const { return m_d->m_startPos; }

int Preview::endPos() const { return m_d->m_endPos; }

const QString &Preview::sourceMarkdown() const { return m_d->m_sourceMarkdown; }

ImagePreview::ImagePreview(PreviewPrivate *p_d, ImagePreviewPrivate *p_imageData)
    : Preview(p_d), m_imageData(p_imageData) {}

ImagePreview::~ImagePreview() {}

const QString &ImagePreview::destination() const { return m_imageData->m_destination; }

const QString &ImagePreview::alternateText() const { return m_imageData->m_alternateText; }

const QString &ImagePreview::title() const { return m_imageData->m_title; }

CodePreview::CodePreview(PreviewPrivate *p_d, CodePreviewPrivate *p_codeData)
    : Preview(p_d), m_codeData(p_codeData) {}

CodePreview::~CodePreview() {}

const QString &CodePreview::language() const { return m_codeData->m_language; }

const QString &CodePreview::code() const { return m_codeData->m_code; }

MathPreview::MathPreview(PreviewPrivate *p_d, MathPreviewPrivate *p_mathData)
    : Preview(p_d), m_mathData(p_mathData) {}

MathPreview::~MathPreview() {}

const QString &MathPreview::expression() const { return m_mathData->m_expression; }

bool MathPreview::isDisplayMath() const { return m_mathData->m_displayMath; }

TablePreview::TablePreview(PreviewPrivate *p_d, TablePreviewPrivate *p_tableData)
    : Preview(p_d), m_tableData(p_tableData) {}

TablePreview::~TablePreview() {}

int TablePreview::rowCount() const { return m_tableData->m_cells.size(); }

int TablePreview::columnCount() const { return m_tableData->m_columnCount; }

const QVector<QVector<QString>> &TablePreview::cells() const { return m_tableData->m_cells; }

const QVector<PreviewTableAlignment> &TablePreview::alignments() const {
  return m_tableData->m_alignments;
}

const QVector<QString> &TablePreview::rowPrefixes() const { return m_tableData->m_rowPrefixes; }

const QString &TablePreview::delimiterPrefix() const { return m_tableData->m_delimiterPrefix; }

PreviewReplacementResult::PreviewReplacementResult()
    : m_data(new PreviewReplacementResultData()) {}

PreviewReplacementResult::PreviewReplacementResult(const PreviewReplacementResult &p_other)
    : m_data(p_other.m_data) {}

PreviewReplacementResult &
PreviewReplacementResult::operator=(const PreviewReplacementResult &p_other) {
  if (this != &p_other) {
    m_data = p_other.m_data;
  }
  return *this;
}

PreviewReplacementResult::~PreviewReplacementResult() {}

quint64 PreviewReplacementResult::identity() const { return m_data->m_identity; }

void PreviewReplacementResult::setIdentity(quint64 p_identity) { m_data->m_identity = p_identity; }

quint64 PreviewReplacementResult::requestedRevision() const { return m_data->m_requestedRevision; }

void PreviewReplacementResult::setRequestedRevision(quint64 p_revision) {
  m_data->m_requestedRevision = p_revision;
}

quint64 PreviewReplacementResult::currentRevision() const { return m_data->m_currentRevision; }

void PreviewReplacementResult::setCurrentRevision(quint64 p_revision) {
  m_data->m_currentRevision = p_revision;
}

PreviewReplacementResult::Status PreviewReplacementResult::status() const {
  return m_data->m_status;
}

void PreviewReplacementResult::setStatus(PreviewReplacementResult::Status p_status) {
  m_data->m_status = p_status;
}

const QString &PreviewReplacementResult::diagnostic() const { return m_data->m_diagnostic; }

void PreviewReplacementResult::setDiagnostic(const QString &p_diagnostic) {
  m_data->m_diagnostic = p_diagnostic;
}

bool PreviewReplacementResult::isAccepted() const {
  return m_data->m_status == PreviewReplacementResult::Accepted;
}
void vte::registerPreviewMetaTypes() {
  // Function-local static initialization is thread safe and runs exactly once.
  static const bool registered = []() {
    qRegisterMetaType<vte::PreviewElementType>("vte::PreviewElementType");
    qRegisterMetaType<vte::PreviewPlacement>("vte::PreviewPlacement");
    qRegisterMetaType<vte::PreviewTableAlignment>("vte::PreviewTableAlignment");
    qRegisterMetaType<vte::PreviewReplacementResult>("vte::PreviewReplacementResult");
    qRegisterMetaType<QSharedPointer<const vte::Preview>>("QSharedPointer<const vte::Preview>");
    qRegisterMetaType<QVector<QSharedPointer<const vte::Preview>>>(
        "QVector<QSharedPointer<const vte::Preview>>");
    return true;
  }();
  Q_UNUSED(registered);
}

static PreviewPrivate *newCommon(PreviewElementType p_type, PreviewPlacement p_placement,
                                 quint64 p_revision, int p_startPos, int p_endPos,
                                 const QString &p_source) {
  auto d = new PreviewPrivate();
  d->m_type = p_type;
  d->m_placement = p_placement;
  d->m_revision = p_revision;
  d->m_startPos = p_startPos;
  d->m_endPos = p_endPos;
  d->m_sourceMarkdown = p_source;
  return d;
}

QSharedPointer<const Preview>
PreviewBuilder::createImage(quint64 p_revision, int p_startPos, int p_endPos,
                            const QString &p_source, PreviewPlacement p_placement,
                            const QString &p_destination, const QString &p_alternateText,
                            const QString &p_title) {
  auto imageData = new ImagePreviewPrivate();
  imageData->m_destination = p_destination;
  imageData->m_alternateText = p_alternateText;
  imageData->m_title = p_title;

  auto d = newCommon(PreviewElementType::Image, p_placement, p_revision, p_startPos, p_endPos,
                     p_source);
  return QSharedPointer<const ImagePreview>(new ImagePreview(d, imageData));
}

QSharedPointer<const Preview> PreviewBuilder::createCode(quint64 p_revision, int p_startPos,
                                                         int p_endPos, const QString &p_source,
                                                         const QString &p_language,
                                                         const QString &p_code) {
  auto codeData = new CodePreviewPrivate();
  codeData->m_language = p_language;
  codeData->m_code = p_code;

  auto d = newCommon(PreviewElementType::Code, PreviewPlacement::BlockAfterSource, p_revision,
                     p_startPos, p_endPos, p_source);
  return QSharedPointer<const CodePreview>(new CodePreview(d, codeData));
}

QSharedPointer<const Preview> PreviewBuilder::createMath(quint64 p_revision, int p_startPos,
                                                         int p_endPos, const QString &p_source,
                                                         const QString &p_expression,
                                                         bool p_displayMath) {
  auto mathData = new MathPreviewPrivate();
  mathData->m_expression = p_expression;
  mathData->m_displayMath = p_displayMath;

  auto d = newCommon(PreviewElementType::Math,
                     p_displayMath ? PreviewPlacement::BlockAfterSource
                                   : PreviewPlacement::InlineAboveLine,
                     p_revision, p_startPos, p_endPos, p_source);
  return QSharedPointer<const MathPreview>(new MathPreview(d, mathData));
}

QSharedPointer<const Preview>
PreviewBuilder::createTable(quint64 p_revision, int p_startPos, int p_endPos,
                            const QString &p_source, int p_columnCount,
                            const QVector<QVector<QString>> &p_cells,
                            const QVector<PreviewTableAlignment> &p_alignments,
                            const QVector<QString> &p_rowPrefixes,
                            const QString &p_delimiterPrefix) {
  auto tableData = new TablePreviewPrivate();
  tableData->m_columnCount = p_columnCount;
  tableData->m_cells = p_cells;
  tableData->m_alignments = p_alignments;
  tableData->m_rowPrefixes = p_rowPrefixes;
  tableData->m_delimiterPrefix = p_delimiterPrefix;

  auto d = newCommon(PreviewElementType::Table, PreviewPlacement::BlockAfterSource, p_revision,
                     p_startPos, p_endPos, p_source);
  return QSharedPointer<const TablePreview>(new TablePreview(d, tableData));
}
