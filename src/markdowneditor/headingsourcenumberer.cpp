#include "headingsourcenumberer.h"

#include <vtextedit/markdownhighlighter.h>
#include <vtextedit/vtextedit.h>

#include "markdownhighlighterresult.h"
#include "textdocumentlayout.h"

#include <QApplication>
#include <QEvent>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>

#include <limits>
#include <utility>

using namespace vte;

namespace {
const int c_inactivityInterval = 500;

// Match only a block-local first title line. Unicode separators are accepted,
// but a separator must never consume the next physical title line.
const QRegularExpression
    c_sourcePrefix(QStringLiteral(R"(^[0-9]+(?:\\?\.[0-9]+)*(?:\\?[.)])?(?:\s+|$))"),
                   QRegularExpression::UseUnicodePropertiesOption);
const QRegularExpression c_providerPrefix(QStringLiteral(R"(\A[0-9]+(?:\.[0-9]+)*[.)]?\z)"));
} // namespace

struct HeadingSourceNumberer::SourceHeading {
  QTextBlock m_block;
  QString m_text;
  int m_content = 0;
  int m_lineEnd = 0;
  bool m_setext = false;
  bool m_continuation = false;
  bool m_needsSeparator = false;
};

HeadingSourceNumberer::HeadingSourceNumberer(VMarkdownEditor *p_editor)
    : QObject(p_editor), m_editor(p_editor), m_doc(p_editor->document()) {
  setObjectName(QStringLiteral("vte_heading_source_numberer"));
  m_timer.setSingleShot(true);
  m_timer.setTimerType(Qt::PreciseTimer);
  connect(&m_timer, &QTimer::timeout, this, &HeadingSourceNumberer::attempt);
  connect(m_doc, &QTextDocument::contentsChange, this, &HeadingSourceNumberer::contentsChange);
  connect(m_doc, &QTextDocument::contentsChanged, this, &HeadingSourceNumberer::contentsChanged);
  connect(m_doc, &QTextDocument::undoCommandAdded, this, [this]() {
    if (!m_applying) {
      m_newUndoCommand = true;
    }
  });
  connect(p_editor->getHighlighter(), &MarkdownHighlighter::headingsUpdated, this,
          [this](const QVector<md::HeadingInfo> &p_headings) {
            const auto result = m_editor->getHighlighter()->m_result;
            if (!isCurrent(result) || &p_headings != &result->m_headingElements) {
              return;
            }
            m_republish = true;
            // Heading publication precedes folding/preview publication in the
            // highlighter. Preparing is read-only; applying is always queued.
            publishCurrent();
            queueAttempt();
          });
  connect(p_editor->getHighlighter(), &MarkdownHighlighter::highlightCompleted, this,
          &HeadingSourceNumberer::queueAttempt);
  connect(p_editor->documentLayout(), &TextDocumentLayout::becameIdle, this,
          &HeadingSourceNumberer::queueAttempt);
  auto edit = p_editor->getTextEdit();
  const auto selectionChanged = [this]() {
    if (!m_applying) {
      m_restoreViewport = false;
    }
    queueAttempt();
  };
  connect(edit, &QTextEdit::cursorPositionChanged, this, selectionChanged);
  connect(edit, &QTextEdit::selectionChanged, this, selectionChanged);
  edit->installEventFilter(this);
  edit->viewport()->installEventFilter(this);
  edit->horizontalScrollBar()->installEventFilter(this);
  edit->verticalScrollBar()->installEventFilter(this);
  // Releasing a preview to another widget need not focus the source editor.
  connect(qApp, &QApplication::focusChanged, this, [this, edit](QWidget *p_old, QWidget *p_now) {
    if ((p_old && edit->viewport()->isAncestorOf(p_old)) ||
        (p_now && edit->viewport()->isAncestorOf(p_now))) {
      queueAttempt();
    }
  });
  observeDocument();
}

void HeadingSourceNumberer::setProvider(VMarkdownEditor::HeadingSectionNumberProvider p_provider) {
  ++m_providerGeneration;
  m_provider = std::move(p_provider);
  cancel();
  m_replaySuppressed = false;
  if (m_provider) {
    requestNormalization(false);
  }
  m_republish = true;
  publishCurrent();
  queueAttempt();
}

void HeadingSourceNumberer::setActive(bool p_active) {
  if (m_active == p_active) {
    return;
  }
  m_active = p_active;
  ++m_generation;
  m_timer.stop();
  // Suspension retains owed work and replay suppression. Activation only
  // schedules; it is also used while the host unwinds loading/config guards.
  if (m_active) {
    queueAttempt();
  }
}

bool HeadingSourceNumberer::restoreViewportAfterHighlight() {
  if (!m_restoreViewport) {
    return false;
  }
  auto edit = m_editor->getTextEdit();
  edit->horizontalScrollBar()->setValue(m_horizontalScroll);
  edit->verticalScrollBar()->setValue(m_verticalScroll);
  // A source change can produce several full highlights. Keep the viewport
  // until cursor/selection, scroll input, or a new source edit takes ownership.
  return true;
}

bool HeadingSourceNumberer::eventFilter(QObject *p_object, QEvent *p_event) {
  Q_UNUSED(p_object);
  if (p_event->type() == QEvent::Wheel || p_event->type() == QEvent::MouseButtonPress ||
      p_event->type() == QEvent::KeyPress) {
    m_restoreViewport = false;
  }
  if (p_event->type() == QEvent::InputMethod || p_event->type() == QEvent::FocusIn ||
      p_event->type() == QEvent::FocusOut || p_event->type() == QEvent::ReadOnlyChange) {
    queueAttempt();
  }
  return false;
}

void HeadingSourceNumberer::observeDocument() {
  m_revision = m_doc->revision();
  m_characters = m_doc->characterCount();
  m_undoSteps = m_doc->availableUndoSteps();
  m_redoSteps = m_doc->availableRedoSteps();
  m_newUndoCommand = false;
  m_sourceChanged = false;
}

void HeadingSourceNumberer::cancel() {
  ++m_generation;
  m_pending = false;
  m_joinSourceEdit = false;
  m_timer.stop();
  m_plan = Plan();
  m_restoreViewport = false;
}

void HeadingSourceNumberer::requestNormalization(bool p_sourceEdit) {
  if (p_sourceEdit) {
    m_restoreViewport = false;
  }
  ++m_generation;
  m_pending = true;
  m_joinSourceEdit = p_sourceEdit;
  m_triggerUndoSteps = m_doc->availableUndoSteps();
  m_idle.restart();
  if (m_active) {
    m_timer.start(c_inactivityInterval);
  }
}

void HeadingSourceNumberer::contentsChange(int p_position, int p_removed, int p_added) {
  // As in TableSourceFormatter, clear removes the terminal character too;
  // selecting all with a cursor does not. setPlainText then inserts it again.
  const bool reset = p_position == 0 && p_added == 0 && p_removed > m_characters - 1 &&
                     m_doc->characterCount() == 1;
  if (reset) {
    cancel();
    m_replaySuppressed = false;
    m_reset = !m_doc->isUndoRedoEnabled();
    observeDocument();
    if (m_provider) {
      requestNormalization(false);
    }
    return;
  }
  if (m_reset && (p_position != 0 || p_removed != 0 || p_added != m_doc->characterCount())) {
    m_reset = false;
  }
  if (!m_applying && (p_removed != 0 || p_added != 0) && m_doc->revision() != m_revision) {
    m_sourceChanged = true;
  }
  m_characters = m_doc->characterCount();
}

void HeadingSourceNumberer::contentsChanged() {
  if (m_reset) {
    // Consume the paired load now, not the first real edit after setPlainText.
    m_reset = false;
    if (m_provider) {
      requestNormalization(false);
    }
    observeDocument();
    return;
  }
  if (m_applying || !m_provider) {
    observeDocument();
    return;
  }
  if (!m_sourceChanged || m_doc->revision() == m_revision) {
    // Nested highlight notifications may precede our contentsChange slot.
    // Preserve its revision and undo-command evidence until the source signal.
    return;
  }
  const int undo = m_doc->availableUndoSteps();
  const int redo = m_doc->availableRedoSteps();
  const bool replay =
      m_doc->isUndoRedoEnabled() &&
      (undo < m_undoSteps || (undo > m_undoSteps && redo < m_redoSteps && !m_newUndoCommand));
  if (replay) {
    cancel();
    m_replaySuppressed = true;
  } else {
    m_replaySuppressed = false;
    requestNormalization(true);
  }
  observeDocument();
}

void HeadingSourceNumberer::queueAttempt() {
  if (m_applying || m_preparing || m_queued ||
      (!m_republish && (!m_pending || !m_active || m_replaySuppressed || !m_provider))) {
    return;
  }
  m_queued = true;
  const auto generation = m_generation;
  QTimer::singleShot(0, this, [this, generation]() {
    m_queued = false;
    if (generation != m_generation) {
      queueAttempt();
      return;
    }
    if (m_republish) {
      publishCurrent();
    }
    attempt();
  });
}

bool HeadingSourceNumberer::canApply() {
  if (!m_active || !m_provider || !m_pending || m_replaySuppressed || m_applying || m_preparing ||
      m_editor->isReadOnly()) {
    return false;
  }
  const auto elapsed = m_idle.elapsed();
  if (elapsed < c_inactivityInterval) {
    m_timer.start(c_inactivityInterval - int(elapsed));
    return false;
  }
  auto layout = m_editor->documentLayout();
  if (layout->isBusy()) {
    layout->requestIdleNotification();
    return false;
  }
  auto edit = m_editor->getTextEdit();
  if (edit->isViewportWidgetFocused() ||
      !edit->getSelections().getAdditionalSelections().isEmpty()) {
    return false;
  }
  for (auto block = m_doc->begin(); block.isValid(); block = block.next()) {
    if (block.layout() && !block.layout()->preeditAreaText().isEmpty()) {
      return false;
    }
  }
  return true;
}

bool HeadingSourceNumberer::isCurrent(
    const QSharedPointer<MarkdownHighlighterResult> &p_result) const {
  const auto highlighter = m_editor->getHighlighter();
  return p_result && p_result == highlighter->m_result &&
         p_result->matched(highlighter->m_timeStamp) &&
         p_result->m_numOfBlocks == m_doc->blockCount();
}

bool HeadingSourceNumberer::isCurrent(const Plan &p_plan) const {
  return p_plan.m_generation == m_generation &&
         p_plan.m_providerGeneration == m_providerGeneration &&
         p_plan.m_revision == m_doc->revision() && isCurrent(p_plan.m_result);
}

bool HeadingSourceNumberer::resolveHeading(const md::HeadingInfo &p_heading,
                                           SourceHeading &p_source) const {
  if (p_heading.m_level < 1 || p_heading.m_level > 6 || p_heading.m_startPos < 0 ||
      p_heading.m_endPos <= p_heading.m_startPos ||
      p_heading.m_endPos > m_doc->characterCount() - 1 ||
      m_doc->characterAt(p_heading.m_endPos).isLowSurrogate()) {
    return false;
  }
  p_source.m_block = m_doc->findBlock(p_heading.m_startPos);
  const auto last = m_doc->findBlock(p_heading.m_endPos - 1);
  if (!p_source.m_block.isValid() || !last.isValid()) {
    return false;
  }
  p_source.m_text = p_source.m_block.text();
  const int size = int(p_source.m_text.size());
  int content = p_heading.m_startPos - p_source.m_block.position();
  if (content < 0 || content >= size || p_source.m_text[content].isLowSurrogate()) {
    return false;
  }
  // A real ATX node ends on its first line. A real Setext node includes its
  // underline; no independent Markdown-looking-line scan is needed or used.
  p_source.m_setext = last != p_source.m_block;
  p_source.m_continuation = last.blockNumber() > p_source.m_block.blockNumber() + 1;
  p_source.m_lineEnd = size;
  const int lineBreak = p_source.m_text.indexOf(QChar::LineSeparator, content);
  if (lineBreak >= 0) {
    p_source.m_lineEnd = lineBreak;
  }
  while (content < p_source.m_lineEnd && p_source.m_text[content].isSpace()) {
    ++content;
  }
  if (p_source.m_setext) {
    if (p_heading.m_level > 2 || content == p_source.m_lineEnd) {
      return false;
    }
    // cmark can discard reference definitions before promoting a paragraph
    // to Setext without advancing that node's source start. Do not write into
    // an ambiguous leading definition or guess where its visible title begins.
    // A label unclosed on this first line might continue onto another line.
    if (p_source.m_text[content] == QLatin1Char('[')) {
      bool ambiguous = true;
      for (int i = content + 1; i < p_source.m_lineEnd; ++i) {
        const auto ch = p_source.m_text[i];
        if (ch == QLatin1Char('\\')) {
          ++i;
        } else if (ch == QLatin1Char('[')) {
          ambiguous = false;
          break;
        } else if (ch == QLatin1Char(']')) {
          ambiguous = i + 1 < p_source.m_lineEnd && p_source.m_text[i + 1] == QLatin1Char(':');
          break;
        }
      }
      if (ambiguous) {
        return false;
      }
    }
  } else {
    const int marker = content;
    while (content < p_source.m_lineEnd && p_source.m_text[content] == QLatin1Char('#')) {
      ++content;
    }
    if (content - marker != p_heading.m_level ||
        (content < p_source.m_lineEnd && p_source.m_text[content] != QLatin1Char(' ') &&
         p_source.m_text[content] != QLatin1Char('\t'))) {
      return false;
    }
    p_source.m_needsSeparator = content == p_source.m_lineEnd;
    while (content < p_source.m_lineEnd && p_source.m_text[content].isSpace()) {
      ++content;
    }
  }
  p_source.m_content = content;
  return true;
}

bool HeadingSourceNumberer::prepare(const QSharedPointer<MarkdownHighlighterResult> &p_result) {
  if (m_preparing || !isCurrent(p_result)) {
    return false;
  }
  QScopedValueRollback<bool> preparing(m_preparing, true);
  Plan plan;
  plan.m_result = p_result;
  plan.m_generation = m_generation;
  plan.m_providerGeneration = m_providerGeneration;
  plan.m_revision = m_doc->revision();
  plan.m_valid = true;
  if (m_provider) {
    const auto &headings = p_result->m_headingElements;
    QVector<SourceHeading> sources;
    sources.reserve(headings.size());
    int previousEnd = 0;
    for (const auto &heading : headings) {
      SourceHeading source;
      if (heading.m_startPos < previousEnd || !resolveHeading(heading, source)) {
        plan.m_valid = false;
        break;
      }
      previousEnd = heading.m_endPos;
      sources.append(std::move(source));
    }
    if (plan.m_valid) {
      // Keep the callable alive if a host replaces it from inside its callback.
      const auto provider = m_provider;
      const auto desired = provider(headings);
      if (!isCurrent(plan)) {
        return false;
      }
      plan.m_valid = desired.size() == headings.size();
      qint64 resultingCharacters = m_doc->characterCount();
      for (int i = 0; plan.m_valid && i < desired.size(); ++i) {
        if (desired[i].isEmpty()) {
          continue;
        }
        if (!c_providerPrefix.match(desired[i]).hasMatch()) {
          plan.m_valid = false;
          break;
        }
        ++plan.m_eligible;
        const auto &source = sources[i];
        // Borrow the stable block text for the regex; only saved prefix bytes
        // need an owning copy after preparation returns.
        const QString firstLine = QString::fromRawData(source.m_text.constData() + source.m_content,
                                                       source.m_lineEnd - source.m_content);
        const auto match = c_sourcePrefix.match(firstLine);
        int prefixLength = match.hasMatch() ? int(match.capturedLength()) : 0;
        // If a number is the entire first line of a multiline Setext title,
        // its trailing two-space hard break is syntax, not disposable spacing.
        if (source.m_continuation && prefixLength == firstLine.size()) {
          int spaces = 0;
          while (spaces < prefixLength &&
                 firstLine[prefixLength - spaces - 1] == QLatin1Char(' ')) {
            ++spaces;
          }
          if (spaces >= 2) {
            prefixLength -= spaces - 1;
          }
        }
        QString after = desired[i];
        if (source.m_setext) {
          int digits = 0;
          while (digits < after.size() && after[digits].isDigit()) {
            ++digits;
          }
          if (digits == after.size() - 1) {
            // A single-component terminator otherwise turns Setext into a list.
            after.insert(digits, QLatin1Char('\\'));
          }
        }
        after += QLatin1Char(' ');
        if (source.m_needsSeparator) {
          after.prepend(QLatin1Char(' '));
        }
        const QString before(firstLine.constData(), prefixLength);
        if (before != after) {
          resultingCharacters += after.size() - before.size();
          if (resultingCharacters > std::numeric_limits<int>::max()) {
            plan.m_valid = false;
            break;
          }
          plan.m_replacements.append(
              {source.m_block.position() + source.m_content, before, std::move(after)});
        }
      }
    }
  }
  if (!isCurrent(plan)) {
    return false;
  }
  if (!plan.m_valid) {
    plan.m_replacements.clear();
    plan.m_eligible = 0;
  }
  m_plan = std::move(plan);
  return true;
}

void HeadingSourceNumberer::publishCurrent() {
  if (m_preparing) {
    return;
  }
  const auto result = m_editor->getHighlighter()->m_result;
  if (!isCurrent(result)) {
    return;
  }
  if (!isCurrent(m_plan) && !prepare(result)) {
    queueAttempt();
    return;
  }
  m_republish = false;
  emit m_editor->headingsUpdated(result->m_headingElements, m_provider && m_plan.m_valid &&
                                                                m_plan.m_eligible > 0 &&
                                                                m_plan.m_replacements.isEmpty());
}

void HeadingSourceNumberer::attempt() {
  if (!canApply()) {
    return;
  }
  if (!isCurrent(m_plan)) {
    // Highlight-only revision changes do not invalidate the AST. Rebuild the
    // block-local plan against the live revision, still requiring a full parse.
    m_republish = true;
    publishCurrent();
  }
  if (!isCurrent(m_plan) || !canApply()) {
    return;
  }
  if (!m_plan.m_valid || m_plan.m_replacements.isEmpty()) {
    m_pending = false;
    m_timer.stop();
    return;
  }
  apply();
}

int HeadingSourceNumberer::mapPosition(int p_position, bool p_followInsertion) const {
  int delta = 0;
  for (const auto &replacement : m_plan.m_replacements) {
    if (p_position < replacement.m_position) {
      break;
    }
    const int before = int(replacement.m_before.size());
    const int after = int(replacement.m_after.size());
    const int offset = p_position - replacement.m_position;
    if (before == 0 && offset == 0) {
      return p_position + delta + (p_followInsertion ? after : 0);
    }
    if (offset < before) {
      return replacement.m_position + delta + qMin(offset, after);
    }
    delta += after - before;
  }
  return p_position + delta;
}

void HeadingSourceNumberer::apply() {
  // Revalidate every original prefix before opening the single edit block.
  for (const auto &replacement : m_plan.m_replacements) {
    const auto block = m_doc->findBlock(replacement.m_position);
    if (!block.isValid()) {
      return;
    }
    const QString text = block.text();
    const int offset = replacement.m_position - block.position();
    if (offset < 0 || offset > text.size() || replacement.m_before.size() > text.size() - offset ||
        text.mid(offset, replacement.m_before.size()) != replacement.m_before) {
      return;
    }
  }
  if (!isCurrent(m_plan) || !canApply()) {
    return;
  }
  auto edit = m_editor->getTextEdit();
  const QTextCursor original = edit->textCursor();
  const auto selection = edit->getSelection();
  const bool overridden =
      selection.isValid() &&
      !(selection == VTextEdit::Selection(original.anchor(), original.position()));
  const int anchor = mapPosition(original.anchor(), original.anchor() <= original.position());
  const int position = mapPosition(original.position(), original.position() <= original.anchor());
  const int selectionStart = overridden ? mapPosition(selection.start(), true) : 0;
  const int selectionEnd = overridden ? mapPosition(selection.end(), false) : 0;
  const int horizontal = edit->horizontalScrollBar()->value();
  const int vertical = edit->verticalScrollBar()->value();
  m_horizontalScroll = horizontal;
  m_verticalScroll = vertical;
  m_restoreViewport = true;
  const auto generation = m_generation;
  // A callback reached by endEditBlock may change the provider and invalidate
  // the member plan. Keep this implicitly-shared replacement snapshot alive.
  const auto replacements = m_plan.m_replacements;
  QScopedValueRollback<bool> applying(m_applying, true);
  m_pending = false;
  m_timer.stop();
  QTextCursor cursor(m_doc);
  // Qt can reopen a grouped edit, but cannot promote a bare insertion into a
  // block. Never undo/replay input to manufacture the latter's grouping.
  if (m_joinSourceEdit && m_doc->isUndoRedoEnabled() && m_triggerUndoSteps > 0 &&
      m_triggerUndoSteps == m_doc->availableUndoSteps() && m_doc->availableRedoSteps() == 0) {
    cursor.joinPreviousEditBlock();
  } else {
    cursor.beginEditBlock();
  }
  for (int i = replacements.size() - 1; i >= 0; --i) {
    const auto &replacement = replacements[i];
    cursor.setPosition(replacement.m_position);
    cursor.setPosition(replacement.m_position + replacement.m_before.size(),
                       QTextCursor::KeepAnchor);
    cursor.insertText(replacement.m_after);
  }
  cursor.endEditBlock();
  QTextCursor restored(m_doc);
  restored.setPosition(anchor);
  restored.setPosition(position, QTextCursor::KeepAnchor);
  edit->setTextCursor(restored);
  if (overridden) {
    edit->setOverriddenSelection(selectionStart, selectionEnd);
  }
  edit->horizontalScrollBar()->setValue(horizontal);
  edit->verticalScrollBar()->setValue(vertical);
  if (generation == m_generation) {
    m_joinSourceEdit = false;
  }
  observeDocument();
}
