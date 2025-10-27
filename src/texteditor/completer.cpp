#include "completer.h"

#include <QAbstractItemView>
#include <QDebug>
#include <QKeyEvent>
#include <QScrollBar>
#include <QStringList>
#include <QStringListModel>
#include <QStyledItemDelegate>
#include <QTextBlock>
#include <QTextDocument>
#include <QTimer>

using namespace vte;

const char *Completer::c_popupProperty = "Popup.Completer.vte";

Completer::Completer(QObject *p_parent) : QCompleter(p_parent) {
  m_model = new QStringListModel(this);
  setModel(m_model);

  popup()->installEventFilter(this);
  popup()->setProperty(c_popupProperty, true);
  popup()->setItemDelegate(new QStyledItemDelegate(this));
}

QPair<int, int> Completer::findCompletionPrefix(CompleterInterface *p_interface) {
  // Find beginning of the word.
  auto cursor = p_interface->textCursor();
  auto block = cursor.block();
  auto text = block.text();

  int pib = cursor.positionInBlock();
  int wordStart = pib;
  if (pib >= text.size() || (!text[pib].isLetterOrNumber() && text[pib] != QLatin1Char('_'))) {
    --pib;
  }
  while (pib >= 0 && (text[pib].isLetterOrNumber() || text[pib] == QLatin1Char('_'))) {
    wordStart = pib;
    --pib;
  }

  // Find end of current word.
  pib = cursor.positionInBlock();
  int wordEnd = pib - 1;
  while (pib < text.size() && (text[pib].isLetterOrNumber() || text[pib] == QLatin1Char('_'))) {
    wordEnd = pib;
    ++pib;
  }

  // Now [wordStart, wordEnd] is the prefix word.
  const int position = block.position();
  return qMakePair(position + wordStart, position + wordEnd + 1);
}

QStringList Completer::generateCompletionCandidates(CompleterInterface *p_interface,
                                                    int p_wordStart, int p_wordEnd,
                                                    bool p_reversed) {
  const QString contents = p_interface->contents();
  QRegularExpression reg("\\W+");
  QStringList above = contents.left(p_wordStart).split(reg, Qt::SkipEmptyParts);
  QStringList below = contents.mid(p_wordEnd).split(reg, Qt::SkipEmptyParts);

  // It differs in order regarding duplicates.
  if (p_reversed) {
    QStringList rev;
    rev.reserve(above.size() + below.size());
    for (auto it = above.rbegin(); it != above.rend(); ++it) {
      rev.append(*it);
    }

    for (auto it = below.rbegin(); it != below.rend(); ++it) {
      rev.append(*it);
    }

    rev.removeDuplicates();

    QStringList res;
    res.reserve(rev.size());
    for (auto it = rev.rbegin(); it != rev.rend(); ++it) {
      res.append(*it);
    }

    return res;
  } else {
    // below + above.
    below.append(above);
    below.removeDuplicates();
    return below;
  }
}

static Qt::CaseSensitivity completionCaseSensitivity(const QString &p_prefix) {
  bool upperCase = false;
  for (int i = 0; i < p_prefix.size(); ++i) {
    if (p_prefix[i].isUpper()) {
      upperCase = true;
      break;
    }
  }
  return upperCase ? Qt::CaseSensitive : Qt::CaseInsensitive;
}

void Completer::triggerCompletion(CompleterInterface *p_interface, const QStringList &p_candidates,
                                  const QPair<int, int> &p_prefixRange, bool p_reversed,
                                  const QRect &p_popupRect) {
  Q_ASSERT(m_interface == nullptr && p_interface);
  m_interface = p_interface;
  connect(m_interface->document(), &QTextDocument::contentsChange, this,
          &Completer::handleContentsChange);
  m_prefixRange = p_prefixRange;

  setWidget(m_interface->widget());

  m_model->setStringList(p_candidates);

  const auto prefix = m_interface->getText(p_prefixRange.first, p_prefixRange.second);

  // MUST set before setCompletionPrefix().
  setCaseSensitivity(completionCaseSensitivity(prefix));

  setCompletionPrefix(prefix);

  int cnt = completionCount();
  if (cnt == 0) {
    finishCompletion();
    return;
  }

  selectRow(p_reversed ? cnt - 1 : 0);

  if (cnt == 1 && currentCompletion() == prefix) {
    finishCompletion();
    return;
  }

  auto pu = popup();
  auto rt(p_popupRect);
  const int extraWidth =
      (int)(pu->verticalScrollBar()->sizeHint().width() * (m_interface->scaleFactor() + 1));
  rt.setWidth(pu->sizeHintForColumn(0) + extraWidth);
  complete(rt);
}

void Completer::finishCompletion() {
  if (popup()->isVisible()) {
    popup()->hide();
  } else {
    cleanUp();
  }
}

bool Completer::selectRow(int p_row) {
  // setCurrentRow() won't select the model index but currentIndex() is updated.
  if (setCurrentRow(p_row)) {
    selectIndex(currentIndex());
    return true;
  }

  return false;
}

void Completer::selectIndex(QModelIndex p_index) {
  auto pu = popup();
  if (!pu) {
    return;
  }

  if (!p_index.isValid()) {
    pu->selectionModel()->clear();
  } else {
    pu->selectionModel()->setCurrentIndex(p_index, QItemSelectionModel::ClearAndSelect);
  }

  p_index = pu->selectionModel()->currentIndex();
  if (!p_index.isValid()) {
    pu->scrollToTop();
  } else {
    pu->scrollTo(p_index);
  }
}

bool Completer::eventFilter(QObject *p_obj, QEvent *p_eve) {
  if (!m_interface || p_obj != popup()) {
    return QCompleter::eventFilter(p_obj, p_eve);
  }

  switch (p_eve->type()) {
  case QEvent::Hide: {
    // Completion exited.
    // Notice that activated() signal is behind the HideEvent.
    cleanUp();
    break;
  }

  case QEvent::KeyPress: {
    auto keyEvent = static_cast<QKeyEvent *>(p_eve);
    if (keyEvent->modifiers() == Qt::NoModifier) {
      switch (keyEvent->key()) {
      case Qt::Key_Down:
        next(false);
        keyEvent->accept();
        return true;

      case Qt::Key_Up:
        next(true);
        keyEvent->accept();
        return true;

      default:
        break;
      }
    }
    break;
  }

  default:
    break;
  }

  return QCompleter::eventFilter(p_obj, p_eve);
}

bool Completer::isActive(CompleterInterface *p_interface) const {
  return m_interface == p_interface;
}

void Completer::next(bool p_reversed) {
  QModelIndex curIndex = popup()->currentIndex();
  if (p_reversed) {
    if (curIndex.isValid()) {
      int row = curIndex.row();
      if (row == 0) {
        selectIndex(QModelIndex());
      } else {
        selectRow(row - 1);
      }
    } else {
      selectRow(completionCount() - 1);
    }
  } else {
    if (curIndex.isValid()) {
      int row = curIndex.row();
      if (!selectRow(row + 1)) {
        selectIndex(QModelIndex());
      }
    } else {
      selectRow(0);
    }
  }
}

void Completer::cleanUp() {
  auto doc = m_interface ? m_interface->document() : nullptr;
  QTimer::singleShot(0, [this, doc]() {
    setWidget(nullptr);
    if (doc) {
      disconnect(doc, 0, this, 0);
    }
  });
  m_interface = nullptr;
}

void Completer::updateCompletionPrefix() {
  auto prefix = m_interface->getText(m_prefixRange.first, m_prefixRange.second);
  setCompletionPrefix(prefix);

  int cnt = completionCount();
  if (cnt == 0) {
    finishCompletion();
  } else if (cnt == 1) {
    selectRow(0);
    if (currentCompletion() == prefix) {
      finishCompletion();
    }
  }
}

void Completer::handleContentsChange(int p_position, int p_charsRemoved, int p_charsAdded) {
  if (!m_interface) {
    return;
  }

  bool needExit = true;
  if (p_charsRemoved == 0 && p_charsAdded == 1 && p_position == m_prefixRange.second) {
    m_prefixRange.second += 1;
    updateCompletionPrefix();
    needExit = false;
  } else if (p_charsRemoved == 1 && p_charsAdded == 0 && p_position == m_prefixRange.second - 1 &&
             p_position > m_prefixRange.first) {
    m_prefixRange.second -= 1;
    updateCompletionPrefix();
    needExit = false;
  }

  if (needExit) {
    finishCompletion();
  }
}

void Completer::execute() {
  Q_ASSERT(m_interface);
  auto interface = m_interface;
  finishCompletion();

  // Make m_interface nullptr before changing content.
  interface->insertCompletion(m_prefixRange.first, m_prefixRange.second, currentCompletion());
}
