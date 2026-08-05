#include "editorextraselection.h"

#include <vtextedit/vtextedit.h>
#include <vtextedit/vtexteditor.h>

#include <QHash>
#include <QRect>
#include <QRegion>
#include <QVector>

using namespace vte;

namespace {
// Qt invalidates the changed extra selections by their tight bounding rectangle.
// Antialiased painting of a full-width selection background may bleed one pixel
// outside of that rectangle, leaving a pale color fringe behind after the cursor
// line moves. Supplement Qt's invalidation with a vertical expansion of the
// changed full-width selections.
//
// The margin is two pixels instead of one: VTextEdit::cursorRect() rounds the
// line geometry with qRound(), while the selection is painted over the aligned
// (floor/ceil) extent of the same geometry. With a fractional line geometry
// (line spacing greater than 1.0, in-place preview images) the two may differ by
// one pixel, so a one pixel margin could land on a row that Qt has invalidated
// already and still miss the fringe row. Two pixels provably cover the fringe
// row in both directions for any rounding of the line geometry.
const int c_selectionInvalidationMargin = 2;

bool isFullWidthSelection(const QTextEdit::ExtraSelection &p_selection) {
  return p_selection.format.boolProperty(QTextFormat::FullWidthSelection);
}

bool isSameSelection(const QTextEdit::ExtraSelection &p_a, const QTextEdit::ExtraSelection &p_b) {
  return p_a.cursor.position() == p_b.cursor.position() && p_a.format == p_b.format;
}

// Return the full-width selections of the document of @p_textEdit.
QList<QTextEdit::ExtraSelection>
fullWidthSelections(const VTextEdit *p_textEdit,
                    const QList<QTextEdit::ExtraSelection> &p_selections) {
  auto doc = p_textEdit->document();
  QList<QTextEdit::ExtraSelection> selections;
  for (const auto &selection : p_selections) {
    if (!selection.cursor.isNull() && selection.cursor.document() == doc &&
        isFullWidthSelection(selection)) {
      selections.append(selection);
    }
  }
  return selections;
}

// Return the region of the selections which are only in one of the two lists,
// expanded vertically by the invalidation margin.
// Both lists must contain full-width selections of the document only.
QRegion changedFullWidthRegion(VTextEdit *p_textEdit,
                               const QList<QTextEdit::ExtraSelection> &p_oldSelections,
                               const QList<QTextEdit::ExtraSelection> &p_newSelections) {
  // Index the old selections by cursor anchor, so that a new selection could be
  // matched without scanning the whole old list.
  QHash<int, QVector<int>> oldCandidates;
  for (int i = 0; i < p_oldSelections.size(); ++i) {
    oldCandidates[p_oldSelections[i].cursor.anchor()].append(i);
  }

  const QRect viewportRect = p_textEdit->viewport()->rect();
  QRegion region;
  auto addSelection = [&](const QTextEdit::ExtraSelection &p_selection) {
    QRect rect = p_textEdit->cursorRect(p_selection.cursor);
    rect.setLeft(viewportRect.left());
    rect.setRight(viewportRect.right());
    rect.adjust(0, -c_selectionInvalidationMargin, 0, c_selectionInvalidationMargin);
    rect &= viewportRect;
    if (!rect.isEmpty()) {
      region += rect;
    }
  };

  for (const auto &selection : p_newSelections) {
    // Consume one matched old selection to handle duplicates correctly.
    bool matched = false;
    auto it = oldCandidates.find(selection.cursor.anchor());
    if (it != oldCandidates.end()) {
      auto &candidates = it.value();
      for (int i = 0; i < candidates.size(); ++i) {
        if (isSameSelection(p_oldSelections[candidates[i]], selection)) {
          candidates.remove(i);
          matched = true;
          break;
        }
      }
    }

    if (!matched) {
      addSelection(selection);
    }
  }

  // The unconsumed old selections are removed or changed.
  for (auto it = oldCandidates.constBegin(); it != oldCandidates.constEnd(); ++it) {
    for (int idx : it.value()) {
      addSelection(p_oldSelections[idx]);
    }
  }

  return region;
}
} // namespace

EditorExtraSelection::EditorExtraSelection(VTextEditor *p_editor)
    : ExtraSelectionInterface(), m_editor(p_editor) {}

QTextCursor EditorExtraSelection::textCursor() const { return m_editor->m_textEdit->textCursor(); }

QString EditorExtraSelection::selectedText() const { return m_editor->m_textEdit->selectedText(); }

void EditorExtraSelection::setExtraSelections(
    const QList<QTextEdit::ExtraSelection> &p_selections) {
  auto textEdit = m_editor->m_textEdit;

  // Only the full-width selections need the supplemental invalidation and they
  // are normally just a few, while the whole list may hold thousands of search
  // and whitespace selections. Keep the previous full-width selections instead
  // of reading the whole applied list back from the widget on every update,
  // since VTextEdit::extraSelections() builds a new list on each call.
  // All the updates funnel through this function, so the cache stays in sync.
  auto newFullWidthSelections = fullWidthSelections(textEdit, p_selections);

  // Let Qt do its normal invalidation of the old and new selections first.
  textEdit->setExtraSelections(p_selections);

  // Then supplement it, so that Qt could coalesce the two update requests.
  // Re-filter the cached selections to drop the ones of a replaced document.
  const auto region = changedFullWidthRegion(
      textEdit, fullWidthSelections(textEdit, m_lastFullWidthSelections), newFullWidthSelections);
  m_lastFullWidthSelections = newFullWidthSelections;
  if (!region.isEmpty()) {
    textEdit->viewport()->update(region);
  }
}

QList<QTextCursor> EditorExtraSelection::findAllText(const QString &p_text,
                                                     bool p_isRegularExpression,
                                                     bool p_caseSensitive) {
  FindFlags flags = None;
  if (p_isRegularExpression) {
    flags |= FindFlag::RegularExpression;
  }
  if (p_caseSensitive) {
    flags |= FindFlag::CaseSensitive;
  }
  return m_editor->m_textEdit->findAllText(p_text, flags);
}
