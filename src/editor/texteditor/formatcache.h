#ifndef FORMATCACHE_H
#define FORMATCACHE_H

#include <QTextCharFormat>
#include <QVector>

namespace vte
{
    // Cache of QTextCharFormat from KSyntaxHighlighting::Format by id().
    class FormatCache
    {
    public:
        FormatCache();

        bool contains(int p_id) const;

        const QTextCharFormat &get(int p_id) const;

        void insert(int p_id, const QTextCharFormat &p_format);

    private:
        struct CacheItem
        {
            bool m_valid = false;

            QTextCharFormat m_textCharFormat;
        };

        int m_capacity = 256;

        QVector<CacheItem> m_cache;
    };
}

#endif // FORMATCACHE_H
