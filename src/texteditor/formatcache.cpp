#include "formatcache.h"

using namespace vte;

bool FormatCache::contains(int p_id) const {
  if (p_id >= m_cache.size()) {
    return false;
  }

  return m_cache[p_id].m_valid;
}

const QTextCharFormat &FormatCache::get(int p_id) const {
  Q_ASSERT(p_id < m_cache.size() && m_cache[p_id].m_valid);
  return m_cache[p_id].m_textCharFormat;
}

void FormatCache::insert(int p_id, const QTextCharFormat &p_format) {
  Q_ASSERT(p_id >= 0);
  if (p_id >= m_cache.size()) {
    m_cache.resize(qsizetype(p_id) + 1);
  }

  m_cache[p_id].m_valid = true;
  m_cache[p_id].m_textCharFormat = p_format;
}

void FormatCache::clear() {
  for (auto &item : m_cache) {
    item.m_valid = false;
  }
}
