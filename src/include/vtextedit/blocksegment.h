#ifndef BLOCKSEGMENT_H
#define BLOCKSEGMENT_H

namespace vte {
struct BlockSegment {
  BlockSegment() = default;

  BlockSegment(int p_offset, int p_length) : m_offset(p_offset), m_length(p_length) {}

  int m_offset = 0;

  int m_length = 0;
};
} // namespace vte

#endif // BLOCKSEGMENT_H
