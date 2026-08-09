#ifndef BLOCKSPELLCHECKDATA_H
#define BLOCKSPELLCHECKDATA_H

#include <QVector>

#include <vtextedit/blocksegment.h>

namespace vte {
struct BlockSpellCheckData {
  bool isValid(int p_revision) const { return m_revision > -1 && m_revision == p_revision; }

  bool isEmpty() const { return m_misspellings.isEmpty(); }

  void clear() {
    m_revision = -1;
    m_misspellings.clear();
  }

  void addMisspell(int p_offset, int p_length) {
    m_misspellings.push_back(BlockSegment(p_offset, p_length));
  }

  int m_revision = -1;

  QVector<BlockSegment> m_misspellings;
};
} // namespace vte

#endif // BLOCKSPELLCHECKDATA_H
