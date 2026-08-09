#ifndef VTEXTEDIT_TEXTRANGE_H
#define VTEXTEDIT_TEXTRANGE_H

#include "vtextedit_export.h"

#include <QTextBlock>

namespace vte {
// When a TextBlockRange is invalid, its first and last block number is not
// reliable.
class VTEXTEDIT_EXPORT TextBlockRange {
public:
  TextBlockRange(const QTextBlock &p_first, const QTextBlock &p_last)
      : m_first(p_first), m_last(p_last) {
    if (m_first.isValid() && m_last.isValid()) {
      int f = m_first.blockNumber();
      int l = m_last.blockNumber();
      if (f <= l) {
        m_validFirstBlockNumber = f;
        m_validLastBlockNumber = l;
      }
    }
  }

  bool isValid() const {
    if (m_first.isValid() && m_last.isValid()) {
      int f = m_first.blockNumber();
      int l = m_last.blockNumber();
      int d = l - f;
      if (d >= 0 &&
          !(d < m_validLastBlockNumber - m_validFirstBlockNumber && f < m_validFirstBlockNumber)) {
        return true;
      }
    }

    return false;
  }

  int size() const {
    if (!isValid()) {
      return 0;
    }

    return m_last.blockNumber() - m_first.blockNumber() + 1;
  }

  bool contains(int p_blockNumber) const {
    if (!isValid()) {
      return false;
    }

    return m_first.blockNumber() <= p_blockNumber && m_last.blockNumber() >= p_blockNumber;
  }

  QString toString() const {
    return QStringLiteral("TextBlockRange %3 [%1, %2]")
        .arg(m_first.blockNumber())
        .arg(m_last.blockNumber())
        .arg(isValid());
  }

  void update() {
    Q_ASSERT(m_first.isValid() && m_last.isValid());
    m_validFirstBlockNumber = m_first.blockNumber();
    m_validLastBlockNumber = m_last.blockNumber();
    Q_ASSERT(m_validLastBlockNumber >= m_validFirstBlockNumber);
  }

  const QTextBlock &first() const { return m_first; }

  const QTextBlock &last() const { return m_last; }

private:
  QTextBlock m_first;
  QTextBlock m_last;

  // Used to judge whether current range is valid.
  int m_validFirstBlockNumber = 0;
  int m_validLastBlockNumber = 0;
};
} // namespace vte

#endif
