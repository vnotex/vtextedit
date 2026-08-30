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

bool vte::previewRenderEquals(const Preview *p_a, const Preview *p_b) {
  // Null first, so a missing snapshot is never "equal" to anything - including
  // another missing one. The caller's question is "may I reuse the previous
  // measurement", and there is nothing to reuse without a previous snapshot.
  if (!p_a || !p_b) {
    return false;
  }

  if (p_a == p_b) {
    return true;
  }

  if (p_a->type() != p_b->type() || p_a->placement() != p_b->placement()) {
    return false;
  }

  if (p_a->sourceMarkdown() != p_b->sourceMarkdown()) {
    return false;
  }

  // Only the table snapshot has its rendering inputs fully enumerated below.
  // Every other type falls through to "not provably equal", which costs a
  // re-measurement and is the behaviour this cache replaced. See the header:
  // an image's destination is resolved from reference definitions elsewhere in
  // the document, so equal source does NOT imply equal rendering for it.
  if (p_a->type() != PreviewElementType::Table) {
    return false;
  }

  const auto *a = static_cast<const TablePreview *>(p_a);
  const auto *b = static_cast<const TablePreview *>(p_b);

  if (a->rowCount() != b->rowCount() || a->columnCount() != b->columnCount() ||
      a->gridRowCount() != b->gridRowCount() || a->gridColumnCount() != b->gridColumnCount()) {
    return false;
  }

  if (a->syntax() != b->syntax() || a->isMarkdownBacked() != b->isMarkdownBacked() ||
      a->hasHeaderRow() != b->hasHeaderRow()) {
    return false;
  }

  if (a->alignments() != b->alignments() || a->cells() != b->cells()) {
    return false;
  }

  // The one the document-wide highlighting budget can change on its own. A run
  // carries a QTextCharFormat, so a difference here really can rewrap a cell.
  if (a->cellFormats() != b->cellFormats()) {
    return false;
  }

  // Merged cells: two grids with the same text can still lay out differently.
  for (int row = 0; row < a->gridRowCount(); ++row) {
    for (int column = 0; column < a->gridColumnCount(); ++column) {
      if (a->isOrigin(row, column) != b->isOrigin(row, column) ||
          a->rowSpan(row, column) != b->rowSpan(row, column) ||
          a->colSpan(row, column) != b->colSpan(row, column)) {
        return false;
      }
    }
  }

  return true;
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

  QVector<QVector<QVector<PreviewFormatRun>>> m_cellFormats;

  QVector<PreviewTableAlignment> m_alignments;

  QVector<QString> m_rowPrefixes;

  QString m_delimiterPrefix;

  PreviewTableSyntax m_syntax = PreviewTableSyntax::Markdown;

  bool m_markdownBacked = true;

  bool m_hasHeaderRow = true;

  int m_gridRowCount = 0;

  int m_gridColumnCount = 0;

  QVector<PreviewTableSlot> m_slots;

  QString m_openTag;

  QVector<QString> m_rowTags;

  QVector<QVector<QString>> m_cellTags;

  const PreviewTableSlot *slot(int p_row, int p_column) const {
    if (p_row < 0 || p_row >= m_gridRowCount || p_column < 0 || p_column >= m_gridColumnCount) {
      return nullptr;
    }
    const int idx = p_row * m_gridColumnCount + p_column;
    if (idx >= m_slots.size()) {
      return nullptr;
    }
    return &m_slots.at(idx);
  }
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

const QVector<QVector<QVector<PreviewFormatRun>>> &TablePreview::cellFormats() const {
  return m_tableData->m_cellFormats;
}

const QVector<PreviewTableAlignment> &TablePreview::alignments() const {
  return m_tableData->m_alignments;
}

const QVector<QString> &TablePreview::rowPrefixes() const { return m_tableData->m_rowPrefixes; }

const QString &TablePreview::delimiterPrefix() const { return m_tableData->m_delimiterPrefix; }

PreviewTableSyntax TablePreview::syntax() const { return m_tableData->m_syntax; }

bool TablePreview::isMarkdownBacked() const { return m_tableData->m_markdownBacked; }

bool TablePreview::hasHeaderRow() const { return m_tableData->m_hasHeaderRow; }

int TablePreview::gridRowCount() const { return m_tableData->m_gridRowCount; }

int TablePreview::gridColumnCount() const { return m_tableData->m_gridColumnCount; }

bool TablePreview::isOrigin(int p_row, int p_column) const {
  const auto *slot = m_tableData->slot(p_row, p_column);
  return slot && slot->m_originRow == p_row && slot->m_originColumn == p_column;
}

int TablePreview::rowSpan(int p_row, int p_column) const {
  const auto *slot = m_tableData->slot(p_row, p_column);
  return slot ? slot->m_rowSpan : 1;
}

int TablePreview::colSpan(int p_row, int p_column) const {
  const auto *slot = m_tableData->slot(p_row, p_column);
  return slot ? slot->m_colSpan : 1;
}

int TablePreview::originRow(int p_row, int p_column) const {
  const auto *slot = m_tableData->slot(p_row, p_column);
  return slot ? slot->m_originRow : p_row;
}

int TablePreview::originColumn(int p_row, int p_column) const {
  const auto *slot = m_tableData->slot(p_row, p_column);
  return slot ? slot->m_originColumn : p_column;
}

const QString &TablePreview::openTag() const { return m_tableData->m_openTag; }

QString TablePreview::rowTag(int p_row) const { return m_tableData->m_rowTags.value(p_row); }

QString TablePreview::cellTag(int p_row, int p_column) const {
  const auto *slot = m_tableData->slot(p_row, p_column);
  if (!slot) {
    return QString();
  }
  return m_tableData->m_cellTags.value(slot->m_originRow).value(slot->m_originColumn);
}

PreviewReplacementResult::PreviewReplacementResult() : m_data(new PreviewReplacementResultData()) {}

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

QSharedPointer<const Preview> PreviewBuilder::createImage(quint64 p_revision, int p_startPos,
                                                          int p_endPos, const QString &p_source,
                                                          PreviewPlacement p_placement,
                                                          const QString &p_destination,
                                                          const QString &p_alternateText,
                                                          const QString &p_title) {
  auto imageData = new ImagePreviewPrivate();
  imageData->m_destination = p_destination;
  imageData->m_alternateText = p_alternateText;
  imageData->m_title = p_title;

  auto d =
      newCommon(PreviewElementType::Image, p_placement, p_revision, p_startPos, p_endPos, p_source);
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

QSharedPointer<const Preview> PreviewBuilder::createTable(
    quint64 p_revision, int p_startPos, int p_endPos, const QString &p_source, int p_columnCount,
    const QVector<QVector<QString>> &p_cells, const QVector<PreviewTableAlignment> &p_alignments,
    const QVector<QString> &p_rowPrefixes, const QString &p_delimiterPrefix,
    const QVector<QVector<QVector<PreviewFormatRun>>> &p_cellFormats) {
  TableSnapshotData data;
  data.m_columnCount = p_columnCount;
  data.m_cells = p_cells;
  data.m_cellFormats = p_cellFormats;
  data.m_alignments = p_alignments;
  data.m_rowPrefixes = p_rowPrefixes;
  data.m_delimiterPrefix = p_delimiterPrefix;

  // A pipe table's grid is the degenerate case: every slot a 1x1 origin, over
  // the NORMALIZED widest row -- which is what TablePreviewDocument::setTable()
  // already builds, and which a ragged body row may push past the declared
  // width.
  data.m_syntax = PreviewTableSyntax::Markdown;
  data.m_markdownBacked = true;
  data.m_hasHeaderRow = true;
  data.m_gridRowCount = p_cells.size();
  for (const auto &row : p_cells) {
    data.m_gridColumnCount = qMax(data.m_gridColumnCount, row.size());
  }
  data.m_slots.resize(data.m_gridRowCount * data.m_gridColumnCount);
  for (int r = 0; r < data.m_gridRowCount; ++r) {
    for (int c = 0; c < data.m_gridColumnCount; ++c) {
      auto &slot = data.m_slots[r * data.m_gridColumnCount + c];
      slot.m_originRow = r;
      slot.m_originColumn = c;
    }
  }

  return createTable(p_revision, p_startPos, p_endPos, p_source, data);
}

QSharedPointer<const Preview> PreviewBuilder::createTable(quint64 p_revision, int p_startPos,
                                                          int p_endPos, const QString &p_source,
                                                          const TableSnapshotData &p_data) {
  auto tableData = new TablePreviewPrivate();
  tableData->m_columnCount = p_data.m_columnCount;
  tableData->m_cells = p_data.m_cells;
  tableData->m_cellFormats = p_data.m_cellFormats;
  tableData->m_alignments = p_data.m_alignments;
  tableData->m_rowPrefixes = p_data.m_rowPrefixes;
  tableData->m_delimiterPrefix = p_data.m_delimiterPrefix;
  tableData->m_syntax = p_data.m_syntax;
  tableData->m_markdownBacked = p_data.m_markdownBacked;
  tableData->m_hasHeaderRow = p_data.m_hasHeaderRow;
  tableData->m_gridRowCount = p_data.m_gridRowCount;
  tableData->m_gridColumnCount = p_data.m_gridColumnCount;
  tableData->m_slots = p_data.m_slots;
  tableData->m_openTag = p_data.m_openTag;
  tableData->m_rowTags = p_data.m_rowTags;
  tableData->m_cellTags = p_data.m_cellTags;

  auto d = newCommon(PreviewElementType::Table, PreviewPlacement::BlockAfterSource, p_revision,
                     p_startPos, p_endPos, p_source);
  return QSharedPointer<const TablePreview>(new TablePreview(d, tableData));
}
