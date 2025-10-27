#ifndef LRUCACHE_H
#define LRUCACHE_H

#include <QHash>
#include <QLinkedList>

namespace vte {
template <typename _Key, typename _Val> class LruCache {
public:
  LruCache(int p_capacity, const _Val &p_dummyValue)
      : m_capacity(p_capacity), m_dummyValue(p_dummyValue) {}

  size_t size() const {
    Q_ASSERT(m_hash.size() == m_list.size());
    return m_hash.size();
  }

  _Val &get(const _Key &p_key) {
    auto iter = m_hash.find(p_key);
    if (iter == m_hash.end()) {
      return m_dummyValue;
    }
    iter.value() = moveBackOfList(iter.value());
    return iter.value()->m_value;
  }

  void set(const _Key &p_key, const _Val &p_val) {
    auto iter = m_hash.find(p_key);
    if (iter != m_hash.end()) {
      // Replace it.
      auto nodeIter = moveBackOfList(iter.value());
      nodeIter->m_value = p_val;
      iter.value() = nodeIter;
      return;
    }

    if (static_cast<size_t>(m_capacity) <= size()) {
      // Use the LRU node for the new value.
      auto nodeIter = m_list.begin();
      auto iter = m_hash.find(nodeIter->m_key);
      nodeIter = moveBackOfList(nodeIter);
      nodeIter->m_key = p_key;
      nodeIter->m_value = p_val;

      m_hash.erase(iter);
      m_hash.insert(p_key, nodeIter);
      return;
    }

    // Simply insert.
    auto nodeIter = m_list.insert(m_list.end(), Node(p_key, p_val));
    m_hash.insert(p_key, nodeIter);
  }

  // Set hints about capacity.
  void setCapacityHint(int p_capacity) {
    if (p_capacity < m_capacity / 2) {
      if (++m_numOfReduce == 5) {
        m_numOfReduce = 0;
        m_capacity = qMax(p_capacity / 2 * 3, 50);
      }
    } else {
      m_numOfReduce = 0;
      if (p_capacity > m_capacity) {
        m_capacity = qMax(p_capacity / 2 * 3, 50);
      }
    }

    shrink();
  }

  void clear() {
    m_hash.clear();
    m_list.clear();
  }

private:
  struct Node {
    Node(const _Key &p_key, const _Val &p_val) : m_key(p_key), m_value(p_val) {}

    _Key m_key;
    _Val m_value;
  };

  typedef typename QLinkedList<Node>::iterator _Iterator;

  _Iterator moveBackOfList(_Iterator p_node) {
    Node node = *p_node;
    m_list.erase(p_node);
    return m_list.insert(m_list.end(), node);
  }

  void shrink() {
    while (m_list.size() > m_capacity) {
      auto nodeIter = m_list.begin();
      auto iter = m_hash.find(nodeIter->m_key);
      m_hash.erase(iter);
      m_list.erase(nodeIter);
    }
  }

  int m_capacity = 50;

  int m_numOfReduce = 0;

  _Val m_dummyValue;

  QHash<_Key, _Iterator> m_hash;

  QLinkedList<Node> m_list;
};
} // namespace vte

#endif // LRUCACHE_H
