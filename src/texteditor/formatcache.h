#ifndef FORMATCACHE_H
#define FORMATCACHE_H

#include <QTextCharFormat>
#include <QVector>

namespace vte {
// Cache of QTextCharFormat from KSyntaxHighlighting::Format by id().
class FormatCache {
public:
  bool contains(int p_id) const;

  const QTextCharFormat &get(int p_id) const;

  void insert(int p_id, const QTextCharFormat &p_format);

  void clear();

private:
  struct CacheItem {
    bool m_valid = false;

    QTextCharFormat m_textCharFormat;
  };

  QVector<CacheItem> m_cache;
};
} // namespace vte

#endif // FORMATCACHE_H
