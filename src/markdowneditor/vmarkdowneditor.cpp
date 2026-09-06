#include <vtextedit/vmarkdowneditor.h>

#include <inputmode/abstractinputmode.h>
#include <inputmode/inputmodemgr.h>
#include <vtextedit/markdowneditorconfig.h>
#include <vtextedit/markdownhighlighter.h>
#include <vtextedit/markdownutils.h>
#include <vtextedit/textblockdata.h>
#include <vtextedit/texteditutils.h>
#include <vtextedit/textutils.h>
#include <vtextedit/theme.h>
#include <vtextedit/vtextedit.h>

#include <texteditor/textfolding.h>

#include "documentresourcemgr.h"
#include "editormarkdownhighlighter.h"
#include "editorpreviewmgr.h"
#include "interactivepreviewhost.h"
#include "ksyntaxcodeblockhighlighter.h"
#include "markdownastwalker.h"
#include "markdownfoldingprovider.h"
#include "markdownhighlighterresult.h"
#include "mathblockhighlighter.h"
#include "previewbuilder.h"
#include "previewfromast.h"
#include "tablepreviewwidget.h"
#include "textdocumentlayout.h"
#include "webcodeblockhighlighter.h"

#include <QDebug>
#include <QFontMetricsF>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QStringList>
#include <QTextLayout>
#include <QTimer>

#include <utility>

using namespace vte;

namespace vte {
// Scheduling and source positions belong to the editor, not to a preview sheet.
// This QObject child deliberately adds no state to an exported class.
class TableSourceFormatter final : public QObject {
  Q_OBJECT
public:
  explicit TableSourceFormatter(VMarkdownEditor *p_editor)
      : QObject(p_editor), m_editor(p_editor), m_doc(p_editor->document()) {
    setObjectName(QStringLiteral("vte_table_source_formatter"));
    m_timer.setSingleShot(true);
    m_timer.setTimerType(Qt::PreciseTimer);
    m_timer.setInterval(500);
    connect(&m_timer, &QTimer::timeout, this, &TableSourceFormatter::attempt);
    connect(m_doc, &QTextDocument::contentsChange, this, &TableSourceFormatter::contentsChange);
    connect(m_doc, &QTextDocument::contentsChanged, this, &TableSourceFormatter::contentsChanged);
    connect(m_doc, &QTextDocument::undoCommandAdded, this, [this]() {
      if (!m_applying) {
        m_newUndoCommand = true;
      }
    });
    connect(p_editor->getHighlighter(), &MarkdownHighlighter::highlightCompleted, this,
            &TableSourceFormatter::queueAttempt);
    connect(p_editor->documentLayout(), &TextDocumentLayout::becameIdle, this,
            &TableSourceFormatter::queueAttempt);
    auto edit = p_editor->getTextEdit();
    connect(edit, &QTextEdit::cursorPositionChanged, this, &TableSourceFormatter::queueAttempt);
    connect(edit, &QTextEdit::selectionChanged, this, &TableSourceFormatter::queueAttempt);
    edit->installEventFilter(this);
    edit->viewport()->installEventFilter(this);
    observeDocument();
  }

  void setEnabled(bool p_enabled) {
    if (m_enabled == p_enabled) {
      return;
    }
    m_enabled = p_enabled;
    cancel();
    m_baseline.clear();
    m_nextBaseline.clear();
    if (m_enabled) {
      captureBaseline();
    }
    observeDocument();
  }

protected:
  bool eventFilter(QObject *p_object, QEvent *p_event) Q_DECL_OVERRIDE {
    Q_UNUSED(p_object);
    if (p_event->type() == QEvent::InputMethod || p_event->type() == QEvent::FocusIn ||
        p_event->type() == QEvent::FocusOut) {
      queueAttempt();
    }
    return false;
  }

private:
  struct Baseline {
    QTextBlock m_block;
    QString m_text;
  };

  struct Row {
    int m_position = 0;
    QString m_before;
    QString m_after;
    QVector<QString> m_cellsBefore;
    QVector<QString> m_cellsAfter;
    QVector<int> m_offsetsBefore;
    QVector<int> m_offsetsAfter;
    QVector<int> m_bordersBefore;
    QVector<int> m_bordersAfter;
    bool m_delimiter = false;

    bool map(int p_position, int &p_mapped) const;
  };

  void observeDocument() {
    m_revision = m_doc->revision();
    m_characters = m_doc->characterCount();
    m_undoSteps = m_doc->availableUndoSteps();
    m_redoSteps = m_doc->availableRedoSteps();
    m_newUndoCommand = false;
    m_sourceChanged = false;
  }

  void cancel() {
    ++m_generation;
    m_pending = false;
    m_timer.stop();
  }

  QHash<int, int> baselinePositions() const {
    QHash<int, int> positions;
    positions.reserve(m_baseline.size());
    for (int i = 0; i < m_baseline.size(); ++i) {
      if (m_baseline[i].m_block.isValid()) {
        positions.insert(m_baseline[i].m_block.position(), i);
      }
    }
    return positions;
  }

  void captureBaseline(const QSet<int> &p_deferredBlocks = QSet<int>()) {
    const auto positions = baselinePositions();
    m_nextBaseline.resize(0);
    m_nextBaseline.reserve(m_doc->blockCount());
    for (auto block = m_doc->begin(); block.isValid(); block = block.next()) {
      const int old = positions.value(block.position(), -1);
      const bool retained = old >= 0 && m_baseline[old].m_block == block;
      if (p_deferredBlocks.contains(block.blockNumber())) {
        if (retained) {
          m_nextBaseline.append(m_baseline[old]);
        }
        continue;
      }
      const QString text = block.text();
      if (retained && m_baseline[old].m_text == text) {
        m_nextBaseline.append(m_baseline[old]);
      } else {
        m_nextBaseline.append({block, text});
      }
    }
    m_baseline.swap(m_nextBaseline);
  }

  void contentsChange(int p_position, int p_removed, int p_added) {
    // Qt clear removes the terminal character as well. Cursor select-all does
    // not, and a rehighlight does not leave a now-empty document.
    const bool reset = p_position == 0 && p_added == 0 && p_removed > m_characters - 1 &&
                       m_doc->characterCount() == 1;
    if (reset) {
      cancel();
      m_baseline.clear();
      m_nextBaseline.clear();
      m_reset = !m_doc->isUndoRedoEnabled();
      if (m_enabled) {
        captureBaseline();
      }
      observeDocument();
      return;
    }
    // The paired setPlainText insertion includes Qt's terminal character;
    // an ordinary cursor insertion after bare clear never does. Clear itself
    // need not emit a final contentsChanged, even with undo disabled.
    if (m_reset && (p_position != 0 || p_removed != 0 || p_added != m_doc->characterCount())) {
      m_reset = false;
    }
    if (!m_applying && (p_removed != 0 || p_added != 0) && m_doc->revision() != m_revision) {
      m_sourceChanged = true;
    }
    m_characters = m_doc->characterCount();
  }

  void contentsChanged() {
    if (m_reset) {
      // setPlainText's paired insertion is complete, still with undo disabled.
      // No queued reset flag may consume a cursor edit after the load returns.
      m_reset = false;
      if (m_enabled) {
        captureBaseline();
      }
      observeDocument();
      return;
    }
    if (m_applying || !m_enabled) {
      observeDocument();
      return;
    }
    if (!m_sourceChanged || m_doc->revision() == m_revision) {
      // A nested highlight notification may precede our contentsChange slot.
      // Do not consume its revision or undo-command evidence here.
      return;
    }
    const int undo = m_doc->availableUndoSteps();
    const int redo = m_doc->availableRedoSteps();
    const bool replay =
        m_doc->isUndoRedoEnabled() &&
        (undo < m_undoSteps || (undo > m_undoSteps && redo < m_redoSteps && !m_newUndoCommand));
    if (replay || m_editor->isReadOnly()) {
      cancel();
      captureBaseline();
    } else {
      ++m_generation;
      m_pending = true;
      m_idle.restart();
      m_timer.start();
    }
    observeDocument();
  }

  void queueAttempt() {
    if (!m_pending || m_applying || m_queued) {
      return;
    }
    m_queued = true;
    const auto generation = m_generation;
    QTimer::singleShot(0, this, [this, generation]() {
      m_queued = false;
      if (generation == m_generation) {
        attempt();
      } else if (m_pending) {
        queueAttempt();
      }
    });
  }

  void attempt() {
    if (!m_enabled || !m_pending || m_applying) {
      return;
    }
    if (m_editor->isReadOnly()) {
      cancel();
      captureBaseline();
      return;
    }
    if (m_idle.elapsed() < 500) {
      m_timer.start(500 - int(m_idle.elapsed()));
      return;
    }
    auto layout = m_editor->documentLayout();
    if (layout->isBusy()) {
      layout->requestIdleNotification();
      return;
    }
    auto edit = m_editor->getTextEdit();
    if (edit->isViewportWidgetFocused() ||
        !edit->getSelections().getAdditionalSelections().isEmpty()) {
      return;
    }
    for (auto block = m_doc->begin(); block.isValid(); block = block.next()) {
      if (block.layout() && !block.layout()->preeditAreaText().isEmpty()) {
        return;
      }
    }
    const auto highlighter = m_editor->getHighlighter();
    // Highlight-only edit blocks advance document revision without invalidating
    // the AST. formatTables snapshots and rechecks the live revision at apply.
    if (!highlighter->m_result || !highlighter->m_result->matched(highlighter->m_timeStamp)) {
      return;
    }
    formatTables(highlighter->m_result->m_tableElements);
  }

  bool prepareTable(const md::TableElement &p_table, QVector<Row> &p_rows) const;
  void formatTables(const QVector<md::TableElement> &p_tables);

  VMarkdownEditor *m_editor;
  QTextDocument *m_doc;
  QTimer m_timer;
  QElapsedTimer m_idle;
  QVector<Baseline> m_baseline;
  QVector<Baseline> m_nextBaseline;
  quint64 m_generation = 0;
  int m_revision = 0;
  int m_characters = 1;
  int m_undoSteps = 0;
  int m_redoSteps = 0;
  bool m_enabled = false;
  bool m_pending = false;
  bool m_queued = false;
  bool m_applying = false;
  bool m_reset = false;
  bool m_sourceChanged = false;
  bool m_newUndoCommand = false;
};

bool TableSourceFormatter::prepareTable(const md::TableElement &p_table,
                                        QVector<Row> &p_rows) const {
  if (p_table.m_syntax != md::TableElement::Syntax::Markdown || p_table.m_columns <= 0 ||
      p_table.m_rows.size() < 2 || p_table.m_alignments.size() != p_table.m_columns) {
    return false;
  }
  const auto first = m_doc->findBlockByNumber(p_table.m_startBlock);
  if (!first.isValid() || first.position() != p_table.m_startPos) {
    return false;
  }
  const auto lines =
      previewSourceText(m_doc, p_table.m_startPos, p_table.m_endPos).split(QLatin1Char('\n'));
  if (lines.size() != p_table.m_rows.size()) {
    return false;
  }
  QVector<QVector<QString>> cells;
  QVector<QString> prefixes;
  QString delimiterPrefix;
  auto block = first;
  for (int r = 0; r < lines.size(); ++r, block = block.next()) {
    const auto &parsed = p_table.m_rows[r];
    Row row;
    row.m_position = block.position();
    row.m_before = lines[r];
    row.m_delimiter = r == 1;
    QString prefix;
    if (!block.isValid() || block.text() != row.m_before ||
        !md::splitTableRow(row.m_before, prefix, row.m_cellsBefore, &row.m_offsetsBefore,
                           &row.m_bordersBefore) ||
        prefix != parsed.m_prefix || row.m_cellsBefore != parsed.m_cells ||
        row.m_offsetsBefore != parsed.m_cellOffsets ||
        row.m_cellsBefore.size() > p_table.m_columns ||
        (r < 2 && row.m_cellsBefore.size() != p_table.m_columns)) {
      return false;
    }
    if (row.m_delimiter) {
      if (parsed.m_type != md::TableRowType::Delimiter) {
        return false;
      }
      delimiterPrefix = prefix;
    } else {
      if (parsed.m_type != (r == 0 ? md::TableRowType::Header : md::TableRowType::Data)) {
        return false;
      }
      for (const auto &cell : row.m_cellsBefore) {
        if (TablePreviewSerializer::escapeCell(cell) != cell) {
          return false;
        }
      }
      cells.append(row.m_cellsBefore);
      prefixes.append(prefix);
    }
    p_rows.append(std::move(row));
  }
  QVector<PreviewTableAlignment> alignments;
  alignments.reserve(p_table.m_columns);
  for (int alignment : p_table.m_alignments) {
    alignments.append(toPreviewAlignment(alignment));
  }
  // This also rejects unsafe prefixes and line separators. The raw-source
  // caller above rejects escape changes rather than authoring new Markdown.
  const QString output =
      TablePreviewSerializer::serialize(cells, alignments, prefixes, delimiterPrefix, true);
  const auto formatted = output.split(QLatin1Char('\n'));
  if (output.isEmpty() || formatted.size() != p_rows.size()) {
    return false;
  }
  for (int r = 0; r < p_rows.size(); ++r) {
    auto &row = p_rows[r];
    row.m_after = formatted[r];
    QString prefix;
    if (!md::splitTableRow(row.m_after, prefix, row.m_cellsAfter, &row.m_offsetsAfter,
                           &row.m_bordersAfter) ||
        prefix != p_table.m_rows[r].m_prefix || row.m_cellsAfter.size() != p_table.m_columns) {
      return false;
    }
    if (!row.m_delimiter) {
      auto normalized = row.m_cellsBefore;
      normalized.resize(p_table.m_columns);
      if (normalized != row.m_cellsAfter) {
        return false;
      }
    }
  }
  return true;
}

// Positions are UTF-16 boundaries, not display columns. A boundary belonging
// to retained cell text wins over the pipe immediately following that text.
bool TableSourceFormatter::Row::map(int p_position, int &p_mapped) const {
  if (m_before == m_after || p_position < m_bordersBefore[0]) {
    p_mapped = p_position;
    return true;
  }
  for (int c = 0; c < m_cellsBefore.size(); ++c) {
    const auto &before = m_cellsBefore[c];
    if (before.isEmpty()) {
      continue;
    }
    const int offset = p_position - m_offsetsBefore[c];
    if (offset < 0 || offset > before.size()) {
      continue;
    }
    int mapped = offset;
    if (m_delimiter) {
      const auto &after = m_cellsAfter[c];
      const bool left = before.startsWith(QLatin1Char(':'));
      const bool right = before.endsWith(QLatin1Char(':'));
      if (left != after.startsWith(QLatin1Char(':')) || right != after.endsWith(QLatin1Char(':'))) {
        return false;
      }
      if (left && offset == 0) {
        mapped = 0;
      } else if (right && offset >= before.size() - 1) {
        mapped = after.size() - (before.size() - offset);
      } else {
        const int dashOffset = offset - int(left);
        const int dashCount = after.size() - int(left) - int(right);
        if (dashOffset < 0 || dashOffset > dashCount) {
          return false;
        }
        mapped = int(left) + dashOffset;
      }
    }
    p_mapped = m_offsetsAfter[c] + mapped;
    return true;
  }
  for (int pipe = 0; pipe < m_bordersBefore.size(); ++pipe) {
    if (p_position == m_bordersBefore[pipe]) {
      // The old closing pipe still closes the same field, even when missing
      // body fields are appended after it.
      p_mapped = m_bordersAfter[pipe];
      return true;
    }
  }
  if (p_position > m_bordersBefore.last()) {
    p_mapped = m_bordersAfter.last() + p_position - m_bordersBefore.last();
    return p_mapped <= m_after.size();
  }
  for (int c = 0; c < m_cellsBefore.size(); ++c) {
    if (p_position <= m_bordersBefore[c] || p_position >= m_bordersBefore[c + 1]) {
      continue;
    }
    if (m_cellsBefore[c].isEmpty()) {
      p_mapped = m_bordersAfter[c] + p_position - m_bordersBefore[c];
      return p_mapped < m_bordersAfter[c + 1];
    }
    if (p_position < m_offsetsBefore[c]) {
      p_mapped = m_offsetsAfter[c] - (m_offsetsBefore[c] - p_position);
      return p_mapped > m_bordersAfter[c];
    }
    const int endBefore = m_offsetsBefore[c] + m_cellsBefore[c].size();
    const int endAfter = m_offsetsAfter[c] + m_cellsAfter[c].size();
    p_mapped = endAfter + p_position - endBefore;
    return p_mapped < m_bordersAfter[c + 1];
  }
  return false;
}

void TableSourceFormatter::formatTables(const QVector<md::TableElement> &p_tables) {
  const auto generation = m_generation;
  const int revision = m_doc->revision();
  const int undoSteps = m_undoSteps;
  auto edit = m_editor->getTextEdit();
  const QTextCursor original = edit->textCursor();
  const auto selection = edit->getSelection();
  const bool overridden =
      selection.isValid() &&
      !(selection == VTextEdit::Selection(original.anchor(), original.position()));
  QVector<int> endpoints{original.anchor(), original.position()};
  if (overridden) {
    endpoints.append(selection.start());
    endpoints.append(selection.end());
  }
  const auto positions = baselinePositions();
  QSet<int> deferred;
  QVector<Row> rows;
  for (const auto &table : p_tables) {
    if (table.m_syntax != md::TableElement::Syntax::Markdown) {
      continue;
    }
    bool changed = false;
    int baselineStart = -1;
    auto block = m_doc->findBlockByNumber(table.m_startBlock);
    for (int r = 0; r < table.m_rows.size() && block.isValid(); ++r, block = block.next()) {
      const int old = positions.value(block.position(), -1);
      if (old < 0 || m_baseline[old].m_block != block || m_baseline[old].m_text != block.text()) {
        changed = true;
      } else if (r > 0 && baselineStart < 0) {
        baselineStart = old - r;
      }
    }
    if (changed && baselineStart >= 0 && table.m_rows.size() <= m_baseline.size() - baselineStart) {
      // Qt can leave a split header's old handle on preceding prose. A broad
      // edit-block notification cannot locate that split. Instead, a surviving
      // non-header row anchors the original baseline; every row must still
      // match exactly. A newly inserted identical table has no such anchor.
      bool sameRun = true;
      block = m_doc->findBlockByNumber(table.m_startBlock);
      for (int r = 0; r < table.m_rows.size(); ++r, block = block.next()) {
        const int old = positions.value(block.position(), -1);
        if (!block.isValid() || m_baseline[baselineStart + r].m_text != block.text() ||
            (old >= 0 && old != baselineStart + r)) {
          sameRun = false;
          break;
        }
      }
      changed = !sameRun;
    }
    if (!changed) {
      continue;
    }
    QVector<Row> tableRows;
    if (!prepareTable(table, tableRows)) {
      continue;
    }
    bool representable = true;
    for (const auto &row : tableRows) {
      for (int endpoint : endpoints) {
        if (endpoint >= row.m_position && endpoint <= row.m_position + row.m_before.size()) {
          int mapped = 0;
          if (!row.map(endpoint - row.m_position, mapped)) {
            representable = false;
          }
        }
      }
    }
    if (!representable) {
      for (int r = 0; r < table.m_rows.size(); ++r) {
        deferred.insert(table.m_startBlock + r);
      }
      continue;
    }
    for (auto &row : tableRows) {
      if (row.m_before != row.m_after) {
        rows.append(std::move(row));
      }
    }
  }
  // Mapping all endpoints against one immutable set of row rewrites preserves
  // direction and positions outside tables, including the next block's zero.
  for (auto &endpoint : endpoints) {
    int delta = 0;
    for (const auto &row : rows) {
      if (endpoint < row.m_position) {
        break;
      }
      if (endpoint <= row.m_position + row.m_before.size()) {
        int mapped = 0;
        if (!row.map(endpoint - row.m_position, mapped)) {
          return;
        }
        endpoint = row.m_position + mapped;
        break;
      }
      delta += row.m_after.size() - row.m_before.size();
    }
    endpoint += delta;
  }
  if (!m_enabled || generation != m_generation || revision != m_doc->revision() ||
      m_editor->isReadOnly() || m_applying || m_idle.elapsed() < 500) {
    return;
  }
  if (m_editor->documentLayout()->isBusy()) {
    m_editor->documentLayout()->requestIdleNotification();
    return;
  }
  if (!rows.isEmpty()) {
    QScopedValueRollback<bool> applying(m_applying, true);
    const int horizontal = edit->horizontalScrollBar()->value();
    const int vertical = edit->verticalScrollBar()->value();
    QTextCursor cursor(m_doc);
    // Qt can reopen an edit block but cannot promote a bare insert into one.
    // Preserve that grouping rather than undoing and replaying user input.
    if (m_doc->isUndoRedoEnabled() && undoSteps > 0 && undoSteps == m_doc->availableUndoSteps() &&
        m_doc->availableRedoSteps() == 0) {
      cursor.joinPreviousEditBlock();
    } else {
      cursor.beginEditBlock();
    }
    for (int r = rows.size() - 1; r >= 0; --r) {
      const auto &row = rows[r];
      int prefix = 0;
      const int beforeSize = row.m_before.size();
      const int afterSize = row.m_after.size();
      while (prefix < beforeSize && prefix < afterSize &&
             row.m_before[prefix] == row.m_after[prefix]) {
        ++prefix;
      }
      int suffix = 0;
      while (suffix < beforeSize - prefix && suffix < afterSize - prefix &&
             row.m_before[beforeSize - suffix - 1] == row.m_after[afterSize - suffix - 1]) {
        ++suffix;
      }
      cursor.setPosition(row.m_position + prefix);
      cursor.setPosition(row.m_position + beforeSize - suffix, QTextCursor::KeepAnchor);
      cursor.insertText(row.m_after.mid(prefix, afterSize - prefix - suffix));
    }
    cursor.endEditBlock();
    QTextCursor restored(m_doc);
    restored.setPosition(endpoints[0]);
    restored.setPosition(endpoints[1], QTextCursor::KeepAnchor);
    edit->setTextCursor(restored);
    if (overridden) {
      edit->setOverriddenSelection(endpoints[2], endpoints[3]);
    }
    edit->horizontalScrollBar()->setValue(horizontal);
    edit->verticalScrollBar()->setValue(vertical);
  }
  m_pending = !deferred.isEmpty();
  captureBaseline(deferred);
  observeDocument();
}
} // namespace vte

VMarkdownEditor::VMarkdownEditor(const QSharedPointer<MarkdownEditorConfig> &p_config,
                                 const QSharedPointer<TextEditorParameters> &p_paras,
                                 QWidget *p_parent)
    : VTextEditor(p_config->m_textEditorConfig, p_paras, p_parent), m_config(p_config) {
  setupDocumentLayout();

  setupSyntaxHighlighter();

  setupPreviewMgr();

  // Setup folding provider.
  m_foldingProvider.reset(new MarkdownFoldingProvider(getTextFolding(), document()));
  connect(getHighlighter(), &MarkdownHighlighter::foldingRegionsUpdated, this,
          [this](const QVector<md::FoldingRegion> &p_regions) {
            m_foldingProvider->updateFoldingRegions(p_regions);
            // Never evaluate the fold state synchronously here:
            // MarkdownHighlighter::completeHighlight() emits
            // foldingRegionsUpdated *before* previewElementsUpdated, so at this
            // instant the host still describes the previous generation.
            if (auto host = interactivePreviewHost()) {
              host->scheduleFoldRefresh();
            }
          });

  // Reset provider state when TextFolding is externally cleared
  // (e.g., during document replacement detected by hardClear).
  connect(getTextFolding(), &TextFolding::foldingRangesChanged, this, [this]() {
    if (getTextFolding()->isEmpty()) {
      m_foldingProvider->resetState();
    }

    // A manual fold or unfold from the gutter has to be written back
    // onto the preview item which owns that region.
    if (auto host = interactivePreviewHost()) {
      host->scheduleFoldRefresh();
    }
  });

  // Unnecessary for now.
  // m_textEdit->installEventFilter(this);

  // Hook keys.
  connect(m_textEdit, &VTextEdit::preKeyReturn, this, &VMarkdownEditor::preKeyReturn);
  connect(m_textEdit, &VTextEdit::postKeyReturn, this, &VMarkdownEditor::postKeyReturn);
  connect(m_textEdit, &VTextEdit::preKeyTab, this, &VMarkdownEditor::preKeyTab);
  connect(m_textEdit, &VTextEdit::preKeyBacktab, this, &VMarkdownEditor::preKeyBacktab);

  new TableSourceFormatter(this);
  updateFromConfig();

  // Trigger update of stuffs after init.
  m_textEdit->setText("");
}

VMarkdownEditor::~VMarkdownEditor() {
  delete findChild<TableSourceFormatter *>(QStringLiteral("vte_table_source_formatter"),
                                           Qt::FindDirectChildrenOnly);
  // The host is an ordinary QObject child, and QObject destroys its children
  // in creation order - which puts m_textEdit, its viewport and every preview
  // widget parented to it *before* the host. Its destructor asks a dirty sheet
  // to write itself back before the identity is dropped, and that needs the
  // document, the anchors and the widgets to still exist. So destroy it here,
  // while they do.
  delete interactivePreviewHost();
}

void VMarkdownEditor::setSyntax(const QString &p_syntax) {
  // Just ignore it.
  Q_UNUSED(p_syntax);
}

QString VMarkdownEditor::getSyntax() const { return QStringLiteral("richmarkdown"); }

void VMarkdownEditor::setupSyntaxHighlighter() {
  m_highlighterInterface.reset(new EditorMarkdownHighlighter(this));
  CodeBlockHighlighter *codeBlockHighlighter = nullptr;
  if (m_config->m_webCodeBlockHighlighterEnabled) {
    m_webCodeBlockHighlighter = new WebCodeBlockHighlighter(this);
    connect(m_webCodeBlockHighlighter,
            &WebCodeBlockHighlighter::externalCodeBlockHighlightRequested, this,
            &VMarkdownEditor::externalCodeBlockHighlightRequested);

    codeBlockHighlighter = m_webCodeBlockHighlighter;
  } else {
    codeBlockHighlighter =
        new KSyntaxCodeBlockHighlighter(m_config->m_textEditorConfig->m_syntaxTheme, this);
  }
  auto highlighterConfig = QSharedPointer<md::HighlighterConfig>::create();
  highlighterConfig->m_mathExtEnabled = true;

  m_mathBlockHighlighter = new MathBlockHighlighter(this);
  connect(m_mathBlockHighlighter, &MathBlockHighlighter::externalMathHighlightRequested, this,
          &VMarkdownEditor::externalMathHighlightRequested);

  m_highlighter =
      new MarkdownHighlighter(m_highlighterInterface.data(), document(), theme(),
                              codeBlockHighlighter, highlighterConfig, m_mathBlockHighlighter);
  updateSpellCheck();
  connect(getHighlighter(), &MarkdownHighlighter::highlightCompleted, this, [this]() {
    m_textEdit->updateCursorWidth();
    if (m_textEdit->isViewportWidgetFocused()) {
      // An in-place preview widget holds the focus. This re-parse is most
      // likely the one its own write-back caused, and the editor's caret is
      // elsewhere - scrolling to it would yank the viewport away from the
      // widget the user is typing in.
      return;
    }

    m_textEdit->ensureCursorVisible();
    m_textEdit->checkCenterCursor();
  });
}

void VMarkdownEditor::setupDocumentLayout() {
  m_resourceMgr.reset(new DocumentResourceMgr());

  auto docLayout = new TextDocumentLayout(document(), m_resourceMgr.data());
  docLayout->setPreviewEnabled(true);

  document()->setDocumentLayout(docLayout);

  connect(m_textEdit, &VTextEdit::cursorWidthChanged, this,
          [this]() { documentLayout()->setCursorWidth(m_textEdit->cursorWidth()); });
}

TextDocumentLayout *VMarkdownEditor::documentLayout() const {
  return static_cast<TextDocumentLayout *>(document()->documentLayout());
}

void VMarkdownEditor::setupPreviewMgr() {
  m_previewMgrInterface.reset(new EditorPreviewMgr(this));
  m_previewMgr = new PreviewMgr(m_previewMgrInterface.data(), this);
  m_previewMgr->setPreviewEnabled(true);
  connect(getHighlighter(), &MarkdownHighlighter::imageLinksUpdated, m_previewMgr,
          &PreviewMgr::updateImageLinks);
  connect(m_previewMgr, &PreviewMgr::requestUpdateImageLinks, getHighlighter(),
          &MarkdownHighlighter::updateHighlight);

  // Interactive preview widgets. The host is an internal QObject child so no
  // exported class needs a new data member.
  auto host = new InteractivePreviewHost(this);
  connect(getHighlighter(), &MarkdownHighlighter::previewElementsUpdated, host,
          &InteractivePreviewHost::updatePreviews);
}

InteractivePreviewHost *VMarkdownEditor::interactivePreviewHost() const {
  return findChild<InteractivePreviewHost *>(QLatin1String(InteractivePreviewHost::c_objectName),
                                             Qt::FindDirectChildrenOnly);
}

bool VMarkdownEditor::registerPreviewWidgetFactory(PreviewWidgetFactory *p_factory,
                                                   int p_priority) {
  auto host = interactivePreviewHost();
  return host ? host->registerFactory(p_factory, p_priority) : false;
}

bool VMarkdownEditor::unregisterPreviewWidgetFactory(PreviewWidgetFactory *p_factory) {
  auto host = interactivePreviewHost();
  return host ? host->unregisterFactory(p_factory) : false;
}
bool VMarkdownEditor::handleTypeAction(TypeAction p_action, const QVariant &p_data) {
  if (auto host = interactivePreviewHost()) {
    if (host->handleTypeAction(p_action, p_data)) {
      return true;
    }
  }

  if (isReadOnly()) {
    return true;
  }

  switch (p_action) {
  case TypeAction::TypeHeading: {
    if (p_data.userType() != QMetaType::Int) {
      return false;
    }

    const int level = p_data.toInt();
    if (level < 0 || level > 6) {
      return false;
    }

    enterInsertModeIfApplicable();
    MarkdownUtils::typeHeading(getTextEdit(), level);
    return true;
  }

  case TypeAction::TypeTodoList:
    if (p_data.userType() != QMetaType::Bool) {
      return false;
    }

    enterInsertModeIfApplicable();
    MarkdownUtils::typeTodoList(getTextEdit(), p_data.toBool());
    return true;

  case TypeAction::TypeLink: {
    if (p_data.userType() != QMetaType::QStringList) {
      return false;
    }

    const QStringList link = p_data.toStringList();
    if (link.size() != 2) {
      return false;
    }

    enterInsertModeIfApplicable();
    MarkdownUtils::typeLink(getTextEdit(), link[0], link[1]);
    return true;
  }

  case TypeAction::TypeImage:
  case TypeAction::TypeTable:
    return false;

  case TypeAction::TypeBold:
  case TypeAction::TypeItalic:
  case TypeAction::TypeStrikethrough:
  case TypeAction::TypeMark:
  case TypeAction::TypeUnorderedList:
  case TypeAction::TypeOrderedList:
  case TypeAction::TypeCode:
  case TypeAction::TypeCodeBlock:
  case TypeAction::TypeMath:
  case TypeAction::TypeMathBlock:
  case TypeAction::TypeQuote:
    if (p_data.isValid()) {
      return false;
    }
    break;
  default:
    return false;
  }

  enterInsertModeIfApplicable();
  switch (p_action) {
  case TypeAction::TypeBold:
    MarkdownUtils::typeBold(getTextEdit());
    break;
  case TypeAction::TypeItalic:
    MarkdownUtils::typeItalic(getTextEdit());
    break;
  case TypeAction::TypeStrikethrough:
    MarkdownUtils::typeStrikethrough(getTextEdit());
    break;
  case TypeAction::TypeMark:
    MarkdownUtils::typeMark(getTextEdit());
    break;
  case TypeAction::TypeUnorderedList:
    MarkdownUtils::typeUnorderedList(getTextEdit());
    break;
  case TypeAction::TypeOrderedList:
    MarkdownUtils::typeOrderedList(getTextEdit());
    break;
  case TypeAction::TypeCode:
    MarkdownUtils::typeCode(getTextEdit());
    break;
  case TypeAction::TypeCodeBlock:
    MarkdownUtils::typeCodeBlock(getTextEdit());
    break;
  case TypeAction::TypeMath:
    MarkdownUtils::typeMath(getTextEdit());
    break;
  case TypeAction::TypeMathBlock:
    MarkdownUtils::typeMathBlock(getTextEdit());
    break;
  case TypeAction::TypeQuote:
    MarkdownUtils::typeQuote(getTextEdit());
    break;
  case TypeAction::TypeHeading:
  case TypeAction::TypeTodoList:
  case TypeAction::TypeLink:
  case TypeAction::TypeImage:
  case TypeAction::TypeTable:
    Q_UNREACHABLE();
  }

  return true;
}

DocumentResourceMgr *VMarkdownEditor::getDocumentResourceMgr() const {
  return m_resourceMgr.data();
}

const QPixmap *VMarkdownEditor::findImageFromDocumentResourceMgr(const QString &p_name) const {
  return m_resourceMgr->findImage(p_name);
}

MarkdownHighlighter *VMarkdownEditor::getHighlighter() const {
  return static_cast<MarkdownHighlighter *>(m_highlighter);
}

PreviewMgr *VMarkdownEditor::getPreviewMgr() const { return m_previewMgr; }

void VMarkdownEditor::setConfig(const QSharedPointer<MarkdownEditorConfig> &p_config) {
  m_config = p_config;
  m_config->fillDefaultTheme();

  VTextEditor::setConfig(p_config->m_textEditorConfig);

  // The base VTextEditor::updateFromConfig only re-applies the syntax theme to a
  // SyntaxHighlighter; the MarkdownHighlighter is not one, so its styles stay
  // locked to the theme set at construction. Re-apply the new theme explicitly
  // and rehighlight so a theme switch refreshes editor colors.
  if (auto *hl = getHighlighter()) {
    hl->setTheme(theme());
    hl->rehighlight();
  }

  updateFromConfig();
}

void VMarkdownEditor::updateFromConfig() {
  Q_ASSERT(m_config);

  documentLayout()->setConstrainPreviewWidthEnabled(
      m_config->m_constrainInplacePreviewWidthEnabled);

  updateInplacePreviewSources();

  // Not ANDed with the text folding switch: the provider gates on
  // TextFolding::isEnabled() itself, which also covers the restore path. And
  // deliberately not retroactive - a region which has already been settled
  // keeps the state it was settled into.
  m_foldingProvider->setAutoFoldPreviewsEnabled(m_config->m_autoFoldPreviewedBlocksEnabled);

  // Also deliberately not retroactive: a table which is already in the
  // document keeps the shape it has until the user edits it.
  if (auto host = interactivePreviewHost()) {
    host->setTableSourceAlignEnabled(m_config->m_autoFormatTableSourceEnabled);
  }

  if (auto formatter = findChild<TableSourceFormatter *>(
          QStringLiteral("vte_table_source_formatter"), Qt::FindDirectChildrenOnly)) {
    formatter->setEnabled(m_config->m_autoFormatTableSourceEnabled);
  }

  applyLineSpacing();

  updateSpaceWidth();
}

void VMarkdownEditor::applyPreviewFolding() {
  if (!m_foldingProvider) {
    return;
  }

  auto vbar = m_textEdit->verticalScrollBar();
  auto layout = documentLayout();

  // Auto-folding a previewed block HIDES its source, so the document gets
  // SHORTER - the mirror image of the growth EditorPreviewMgr::relayout()
  // compensates for, and a second, later geometry change that its anchor has
  // already been retired by the time this runs. QScrollBar keeps its value()
  // across the range update either way, so without an anchor here every fold
  // above the viewport slides the visible text UP.
  //
  // Anchor on the first visible block, exactly as the relayout path does.
  // blockBoundingRect() lazily repairs a stale block layout, so it must not be
  // called from inside a layout pass.
  int originValue = 0;
  int anchorBlockNumber = -1;
  int originRevision = 0;
  qreal anchorViewportY = 0;
  bool anchored = false;

  if (!m_inFoldScrollAnchor && vbar && layout && !layout->isBusy()) {
    originValue = vbar->value();
    // At the very top nothing above the viewport can displace the content.
    if (originValue != vbar->minimum()) {
      const auto anchorBlock = TextEditUtils::firstVisibleBlock(m_textEdit);
      if (anchorBlock.isValid()) {
        anchored = true;
        anchorBlockNumber = anchorBlock.blockNumber();
        anchorViewportY = layout->blockBoundingRect(anchorBlock).y() - originValue;
        originRevision = document()->revision();
      }
    }
  }

  auto host = interactivePreviewHost();
  const auto ranges = host ? host->previewedRanges() : QVector<PreviewedRange>();
  const auto states =
      m_foldingProvider->applyPreviewAutoFold(ranges, m_textEdit->textCursor().blockNumber());
  if (host) {
    host->setPreviewFoldStates(states);
  }

  if (!anchored) {
    return;
  }

  // applyPreviewAutoFold() reaches application code synchronously - foldRange()
  // emits foldingRangesChanged(), and the geometry that follows hands a context
  // to every preview widget - so NOTHING captured above may be trusted without
  // being re-established first. The provider re-resolves its own table across
  // the same boundary for exactly this reason.
  //
  // A replaced scrollbar or layout leaves the captured pointers dangling, and a
  // document edit makes anchorBlockNumber name a different line. Any of those
  // means the correction describes a document nobody is looking at any more;
  // drop it rather than guess.
  if (m_textEdit->verticalScrollBar() != vbar || documentLayout() != layout ||
      document()->revision() != originRevision || layout->isBusy()) {
    return;
  }

  // The same "user scrolled meanwhile" rule as
  // InteractivePreviewHost::applyRealizationScrollCompensation(): a newer
  // scroll always wins over an older correction.
  if (vbar->value() != originValue) {
    return;
  }

  auto anchorBlock = document()->findBlockByNumber(anchorBlockNumber);
  if (!anchorBlock.isValid()) {
    return;
  }

  // The anchor may have been folded away. TextFolding::setRangeFolded() keeps a
  // region's FIRST and last block visible, so the hidden content collapsed
  // upwards into a block above; walking backwards lands on it and keeps the
  // anchor at or above where the user was looking. Walking forward is only the
  // fallback for a document whose head is entirely hidden, which
  // blockBoundingRect() could not measure at all.
  if (!anchorBlock.isVisible()) {
    auto probe = anchorBlock.previous();
    while (probe.isValid() && !probe.isVisible()) {
      probe = probe.previous();
    }

    if (!probe.isValid()) {
      probe = anchorBlock.next();
      while (probe.isValid() && !probe.isVisible()) {
        probe = probe.next();
      }
    }

    if (!probe.isValid()) {
      return;
    }

    anchorBlock = probe;
  }

  const qreal newDocY = layout->blockBoundingRect(anchorBlock).y();
  const int target = qBound(vbar->minimum(), qRound(newDocY - anchorViewportY), vbar->maximum());
  if (target == originValue) {
    return;
  }

  m_inFoldScrollAnchor = true;
  vbar->setValue(target);
  m_inFoldScrollAnchor = false;
}

bool VMarkdownEditor::restoreFoldAfterPreviewRewrite(PreviewElementType p_type, int p_startBlock,
                                                     int p_endBlock) {
  if (m_foldingProvider &&
      m_foldingProvider->restoreFoldedRange(p_type, p_startBlock, p_endBlock)) {
    // The fold was destroyed and restored synchronously inside the rewrite's
    // turn. The folded-line background is an extra selection whose cursor was
    // dragged past the replacement by Qt, and the corrected list is only
    // scheduled behind a 200ms coalescing timer. Push it now so the stale
    // position never reaches the repaint which follows this call. Only owed
    // when a range was really created: the restore is a no-op for a replacement
    // which collapsed the element onto a single block, and then nothing changed.
    applyPendingExtraSelections();
    return true;
  }

  return false;
}

bool VMarkdownEditor::tryPreviewSourceFolded(PreviewElementType p_type, int p_startBlock,
                                             int p_endBlock, bool *p_folded) const {
  return m_foldingProvider
             ? m_foldingProvider->tryRegionFolded(p_type, p_startBlock, p_endBlock, p_folded)
             : false;
}

void VMarkdownEditor::setInplacePreviewEnabled(bool p_enabled) {
  if (m_inplacePreviewEnabled == p_enabled) {
    return;
  }

  m_inplacePreviewEnabled = p_enabled;
  updateInplacePreviewSources();
}

void VMarkdownEditor::updateInplacePreviewSources() {
  auto host = interactivePreviewHost();

  if (!m_inplacePreviewEnabled) {
    m_previewMgr->setPreviewEnabled(false);
    if (host) {
      host->setEnabled(false);
    }
    return;
  }

  if (host) {
    host->setEnabled(true);
    host->setTypeEnabled(PreviewElementType::Image,
                         m_config->m_inplacePreviewSources & MarkdownEditorConfig::ImageLink);
    host->setTypeEnabled(PreviewElementType::Code,
                         m_config->m_inplacePreviewSources & MarkdownEditorConfig::CodeBlock);
    host->setTypeEnabled(PreviewElementType::Math,
                         m_config->m_inplacePreviewSources & MarkdownEditorConfig::Math);
    host->setTypeEnabled(PreviewElementType::Table,
                         m_config->m_inplacePreviewSources & MarkdownEditorConfig::Table);
  }

  // The painted path only knows about image, code and math.
  const auto paintedSources = m_config->m_inplacePreviewSources &
                              (MarkdownEditorConfig::ImageLink | MarkdownEditorConfig::CodeBlock |
                               MarkdownEditorConfig::Math);
  if (paintedSources == (MarkdownEditorConfig::ImageLink | MarkdownEditorConfig::CodeBlock |
                         MarkdownEditorConfig::Math)) {
    m_previewMgr->setPreviewEnabled(true);
  } else {
    m_previewMgr->setPreviewEnabled(false);
    if (paintedSources & MarkdownEditorConfig::ImageLink) {
      m_previewMgr->setPreviewEnabled(PreviewData::Source::ImageLink, true);
    }
    if (paintedSources & MarkdownEditorConfig::CodeBlock) {
      m_previewMgr->setPreviewEnabled(PreviewData::Source::CodeBlock, true);
    }
    if (paintedSources & MarkdownEditorConfig::Math) {
      m_previewMgr->setPreviewEnabled(PreviewData::Source::MathBlock, true);
    }
  }
}

bool VMarkdownEditor::eventFilter(QObject *p_obj, QEvent *p_event) {
  if (p_obj == m_textEdit) {
    switch (p_event->type()) {
    case QEvent::KeyPress:
      if (handleKeyPressEvent(static_cast<QKeyEvent *>(p_event))) {
        return true;
      }
      break;

    default:
      break;
    }
  }
  return VTextEditor::eventFilter(p_obj, p_event);
}

bool VMarkdownEditor::handleKeyPressEvent(QKeyEvent *p_event) {
  Q_UNUSED(p_event);
  return false;
}

void VMarkdownEditor::zoom(int p_delta) {
  const int preFontSize = editorFontPointSize();
  VTextEditor::zoom(p_delta);
  const int postFontSize = editorFontPointSize();

  if (preFontSize == postFontSize) {
    return;
  }

  getHighlighter()->updateStylesFontSize(postFontSize - preFontSize);

  updateSpaceWidth();
  applyLineSpacing();
}

void VMarkdownEditor::applyLineSpacing() {
  qreal multiplier = qMax(m_config->m_textEditorConfig->m_lineSpacing, 1.0);
  QFontMetricsF fmf(m_textEdit->font(), m_textEdit);
  const qreal leadingSpace = fmf.lineSpacing() * (multiplier - 1.0);

  if (qFuzzyCompare(documentLayout()->getLeadingSpaceOfLine() + 1.0, leadingSpace + 1.0)) {
    return;
  }

  documentLayout()->setLeadingSpaceOfLine(leadingSpace);
  documentLayout()->relayout();
}

void VMarkdownEditor::updateSpaceWidth() {
  const auto &codeBlockFormat = getHighlighter()->codeBlockStyle();
  auto font = codeBlockFormat.font();
  if (codeBlockFormat.fontPointSize() < 0.001) {
    font.setPointSize(editorFontPointSize());
  }

  QFontMetricsF fmf(font, m_textEdit);
  m_textEdit->setSpaceWidth(fmf.horizontalAdvance(QLatin1Char(' ')));
}

void VMarkdownEditor::preKeyReturn(int p_modifiers, bool *p_changed, bool *p_handled) {
  Q_ASSERT(!m_textEdit->isReadOnly());

  // Probe the AST before any block is inserted, so postKeyReturn can consume it.
  m_returnBlockContext = getHighlighter()->getBlockContext(m_textEdit->textCursor().blockNumber());

  if (p_modifiers == Qt::ShiftModifier) {
    *p_changed = true;
    auto cursor = m_textEdit->textCursor();
    cursor.beginEditBlock();
    cursor.insertText(QStringLiteral("  "));
    cursor.endEditBlock();
    m_textEdit->setTextCursor(cursor);
  } else if (p_modifiers == Qt::NoModifier) {
    auto cursor = m_textEdit->textCursor();
    if (cursor.hasSelection()) {
      // Let handleKeyReturn perform its normal selection-replacing block split.
      // The probe was taken at the active end of the selection, which is not
      // necessarily the line surviving the split, so it must not be able to
      // drive an insertion. Suppression-only data is kept: vetoing is always
      // the safe direction.
      m_returnBlockContext.m_fresh = false;
      m_returnBlockContext.m_quoteDepth = 0;
      return;
    }

    if (m_returnBlockContext.m_valid && m_returnBlockContext.m_inFencedCode) {
      // Never strip markers inside a fence.
      return;
    }

    const auto block = cursor.block();
    const auto text = block.text().left(cursor.positionInBlock());

    QString indent, quotePrefix, rest;
    int depth = 0;
    const bool quoted = MarkdownUtils::isQuote(text, indent, quotePrefix, rest, depth);
    const QString &listSource = quoted ? rest : text;

    QChar listMark;
    QString listNumber;
    bool isEmpty = false;
    const bool isList = MarkdownUtils::isTodoList(listSource, listMark, isEmpty) ||
                        MarkdownUtils::isUnorderedList(listSource, listMark, isEmpty) ||
                        MarkdownUtils::isOrderedList(listSource, listNumber, isEmpty);

    QString replacement;
    bool handled = false;
    if (isList && isEmpty) {
      // Drop only the list marker.
      replacement = quoted ? (indent + quotePrefix + TextUtils::fetchIndentationSpaces(rest))
                           : TextUtils::fetchIndentationSpaces(text);
      handled = true;
    }
    // A bare quote line ("> " or ">") is a blank line *inside* the quote, not a
    // request to leave it, so no quote level is ever stripped here. Enter just
    // starts another quote line via postKeyReturn.

    if (handled) {
      cursor.beginEditBlock();
      cursor.setPosition(block.position(), QTextCursor::KeepAnchor);
      cursor.removeSelectedText();
      cursor.insertText(replacement);
      cursor.endEditBlock();
      m_textEdit->setTextCursor(cursor);

      *p_changed = true;
      *p_handled = true;
    }
  }
}

void VMarkdownEditor::postKeyReturn(int p_modifiers) {
  Q_ASSERT(!m_textEdit->isReadOnly());
  const auto blockContext = m_returnBlockContext;
  m_returnBlockContext = md::BlockContext();

  if (p_modifiers != Qt::NoModifier) {
    return;
  }

  auto cursor = m_textEdit->textCursor();

  const auto block = cursor.block();
  auto preBlock = block.previous();
  Q_ASSERT(preBlock.isValid());
  const auto preText = preBlock.text();

  if (blockContext.m_valid && blockContext.m_inFencedCode) {
    // Never continue markers inside a fence.
    return;
  }

  if (preText.isEmpty()) {
    return;
  }

  // Already indented by VTextEdit.

  QString indent, quotePrefix, rest;
  int textDepth = 0;
  bool quoted = MarkdownUtils::isQuote(preText, indent, quotePrefix, rest, textDepth);

  if (!quoted && blockContext.m_fresh && blockContext.m_quoteDepth > 0) {
    // Lazy continuation: the AST knows this line belongs to a quote although
    // the text carries no marker. Insert-causing, so it requires a fresh AST.
    quotePrefix = QStringLiteral("> ").repeated(blockContext.m_quoteDepth);
    // The leading indentation has already been copied into the new block by
    // AutoIndentHelper::autoIndent.
    rest = preText.mid(TextUtils::fetchIndentation(preText));
    quoted = true;
  }

  const QString &listSource = quoted ? rest : preText;

  QString marker;
  QChar listMark;
  QString listNumber;
  bool isEmpty = false;
  if (MarkdownUtils::isTodoList(listSource, listMark, isEmpty)) {
    marker = QStringLiteral("%1 [ ] ").arg(listMark);
  } else if (MarkdownUtils::isUnorderedList(listSource, listMark, isEmpty)) {
    marker = QStringLiteral("%1 ").arg(listMark);
  } else if (MarkdownUtils::isOrderedList(listSource, listNumber, isEmpty)) {
    marker = QStringLiteral("%1. ").arg(listNumber.toInt() + 1);
  }

  const QString innerIndent = quoted ? TextUtils::fetchIndentationSpaces(rest) : QString();
  const QString textToInsert = quotePrefix + innerIndent + marker;
  if (textToInsert.isEmpty()) {
    return;
  }

  cursor.joinPreviousEditBlock();
  cursor.insertText(textToInsert);
  cursor.endEditBlock();
  m_textEdit->setTextCursor(cursor);
}

void VMarkdownEditor::preKeyTab(int p_modifiers, bool *p_handled) {
  Q_ASSERT(!m_textEdit->isReadOnly());
  if (p_modifiers == Qt::NoModifier) {
    auto cursor = m_textEdit->textCursor();
    if (cursor.hasSelection()) {
      return;
    }

    const auto block = cursor.block();
    const auto text = block.text().left(cursor.positionInBlock());
    if (text.isEmpty()) {
      return;
    }

    QChar listMark;
    bool isEmpty = false;
    if (MarkdownUtils::isTodoList(text, listMark, isEmpty) ||
        MarkdownUtils::isUnorderedList(text, listMark, isEmpty)) {
      // Indent the empty todo/unordered list.
      if (isEmpty) {
        *p_handled = true;
        TextEditUtils::indentBlock(cursor, !m_textEdit->isTabExpanded(),
                                   m_textEdit->getTabStopWidthInSpaces(), false);
        m_textEdit->setTextCursor(cursor);
      }
      return;
    }

    QString listNumber;
    if (MarkdownUtils::isOrderedList(text, listNumber, isEmpty) && isEmpty) {
      *p_handled = true;
      // Reset the list number and indent the empty ordered list.
      auto afterText = MarkdownUtils::setOrderedListNumber(text, 1);
      cursor.beginEditBlock();
      if (afterText != text) {
        cursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::KeepAnchor);
        cursor.insertText(afterText);
      }
      TextEditUtils::indentBlock(cursor, !m_textEdit->isTabExpanded(),
                                 m_textEdit->getTabStopWidthInSpaces(), false);
      cursor.endEditBlock();
      m_textEdit->setTextCursor(cursor);
      return;
    }
  }
}

void VMarkdownEditor::preKeyBacktab(int p_modifiers, bool *p_handled) {
  Q_ASSERT(!m_textEdit->isReadOnly());
  if (p_modifiers == Qt::ShiftModifier) {
    auto cursor = m_textEdit->textCursor();
    if (cursor.hasSelection()) {
      return;
    }

    const auto block = cursor.block();
    const auto text = block.text().left(cursor.positionInBlock());
    if (text.isEmpty()) {
      return;
    }

    QChar listMark;
    bool isEmpty = false;
    if (MarkdownUtils::isTodoList(text, listMark, isEmpty) ||
        MarkdownUtils::isUnorderedList(text, listMark, isEmpty)) {
      // Unindent the empty todo/unordered list.
      if (isEmpty) {
        *p_handled = true;
        TextEditUtils::unindentBlock(cursor, m_textEdit->getTabStopWidthInSpaces());
        m_textEdit->setTextCursor(cursor);
      }
      return;
    }

    QString listNumber;
    if (MarkdownUtils::isOrderedList(text, listNumber, isEmpty) && isEmpty) {
      *p_handled = true;

      cursor.beginEditBlock();

      // Unindent the empty ordered list.
      TextEditUtils::unindentBlock(cursor, m_textEdit->getTabStopWidthInSpaces());

      const auto newText = block.text().left(cursor.positionInBlock());
      Q_ASSERT(MarkdownUtils::isOrderedList(newText, listNumber, isEmpty));

      // Try to correct the list number.
      int newNumber = 1;
      {
        const auto preBlock = block.previous();
        if (preBlock.isValid()) {
          const auto preText = preBlock.text();
          if (TextUtils::fetchIndentation(preText) == TextUtils::fetchIndentation(newText)) {
            QString preListNumber;
            bool preIsEmpty = false;
            if (MarkdownUtils::isOrderedList(preText, preListNumber, preIsEmpty)) {
              newNumber = preListNumber.toInt() + 1;
            }
          }
        }
      }

      auto afterText = MarkdownUtils::setOrderedListNumber(newText, newNumber);
      if (afterText != newText) {
        cursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::KeepAnchor);
        cursor.insertText(afterText);
      }
      cursor.endEditBlock();
      m_textEdit->setTextCursor(cursor);
      return;
    }
  }
}

void VMarkdownEditor::handleExternalCodeBlockHighlightData(int p_idx, TimeStamp p_timeStamp,
                                                           const QString &p_html) {
  Q_ASSERT(m_webCodeBlockHighlighter);
  m_webCodeBlockHighlighter->handleExternalCodeBlockHighlightData(p_idx, p_timeStamp, p_html);
}

void VMarkdownEditor::setExternalCodeBlockHighlihgtStyles(
    const ExternalCodeBlockHighlightStyles &p_styles) {
  WebCodeBlockHighlighter::setExternalCodeBlockHighlihgtStyles(p_styles);
}

void VMarkdownEditor::handleExternalMathHighlightData(int p_idx, TimeStamp p_timeStamp,
                                                      const QString &p_html) {
  Q_ASSERT(m_mathBlockHighlighter);
  m_mathBlockHighlighter->handleExternalMathHighlightData(p_idx, p_timeStamp, p_html);
}

#include "vmarkdowneditor.moc"
