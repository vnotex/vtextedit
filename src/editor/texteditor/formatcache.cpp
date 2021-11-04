#include "formatcache.h"

#include <QDebug>

using namespace vte;

FormatCache::FormatCache()
{
    m_cache.resize(m_capacity);
}

bool FormatCache::contains(int p_id) const
{
    if (p_id >= m_capacity) {
        return false;
    }

    return m_cache[p_id].m_valid;
}

const QTextCharFormat &FormatCache::get(int p_id) const
{
    Q_ASSERT(p_id < m_capacity && m_cache[p_id].m_valid);
    return m_cache[p_id].m_textCharFormat;
}

void FormatCache::insert(int p_id, const QTextCharFormat &p_format)
{
    if (p_id >= m_capacity) {
        qWarning() << "id exceeds the capacity of FormatCache (maybe need to increase the default capacity)" << p_id << m_capacity;
        return;
    }

    Q_ASSERT(p_id >= 0);
    m_cache[p_id].m_valid = true;
    m_cache[p_id].m_textCharFormat = p_format;
}
